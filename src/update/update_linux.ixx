module;

#ifndef _WIN32

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#ifndef AWJ_BUILD_VERSION
#define AWJ_BUILD_VERSION "0.0.0"
#endif

export module awj.update_linux;

import awj.core;
import awj.update_archive;
import awj.update_keyring;
import awj.update_manifest;
import awj.update_manifest_v2;
import awj.update_model;
import awj.update_runtime;
import awj.update_security_state;

export namespace awj::update {

namespace linux_detail {

namespace fs = std::filesystem;

constexpr std::string_view update_root_name{".awj-update"};
constexpr std::string_view transaction_name{".awj-update-transaction"};

std::expected<void, std::string> write_all(int fd, std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      return std::unexpected{"写入 Linux 更新事务文件失败。"};
    }
    if (written == 0) return std::unexpected{"写入 Linux 更新事务文件失败。"};
    offset += static_cast<std::size_t>(written);
  }
  return {};
}

std::expected<void, std::string> fsync_directory(const fs::path& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return std::unexpected{"无法刷新 Linux 更新目录。"};
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) return std::unexpected{"无法刷新 Linux 更新目录。"};
  return {};
}

std::expected<void, std::string> atomic_write_text(const fs::path& path,
                                                    std::string_view text) {
  const auto temporary = path.string() + std::format(".tmp-{}-{}", ::getpid(),
      std::chrono::steady_clock::now().time_since_epoch().count());
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        0600);
  if (fd < 0) return std::unexpected{"无法创建 Linux 更新事务文件。"};
  const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
  auto written = write_all(fd, bytes);
  const bool flushed = written && ::fsync(fd) == 0;
  ::close(fd);
  if (!flushed) {
    std::error_code ec;
    fs::remove(temporary, ec);
    if (!written) return std::unexpected{written.error()};
    return std::unexpected{"无法刷新 Linux 更新事务文件。"};
  }
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    std::error_code ec;
    fs::remove(temporary, ec);
    return std::unexpected{"无法原子替换 Linux 更新事务文件。"};
  }
  return fsync_directory(path.parent_path());
}

std::expected<std::string, std::string> read_text(const fs::path& path,
                                                   std::uint64_t maximum) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > maximum || size >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected{"Linux 更新事务文件大小非法。"};
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) return std::unexpected{"无法读取 Linux 更新事务文件。"};
  std::string result(static_cast<std::size_t>(size), '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  if (!input && !input.eof()) return std::unexpected{"读取 Linux 更新事务文件失败。"};
  return result;
}

std::expected<std::string, std::string> read_compatible_stage_text(
    const fs::path& stage, std::string_view current_name,
    std::string_view legacy_name, std::uint64_t maximum) {
  const auto current = stage / current_name;
  std::error_code ec;
  const bool current_exists = fs::exists(current, ec);
  if (ec) return std::unexpected{"检查 Linux 更新暂存文件失败。"};
  return read_text(current_exists ? current : stage / legacy_name, maximum);
}

std::expected<void, std::string> copy_with_fsync(const fs::path& source,
                                                  const fs::path& destination,
                                                  mode_t mode) {
  const int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return std::unexpected{"无法读取 Linux 更新文件。"};
  const int out = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                         mode);
  if (out < 0) {
    ::close(in);
    return std::unexpected{"无法创建 Linux 更新文件。"};
  }
  std::array<std::byte, 128 * 1024> buffer{};
  std::expected<void, std::string> result{};
  for (;;) {
    const auto read = ::read(in, buffer.data(), buffer.size());
    if (read < 0) {
      if (errno == EINTR) continue;
      result = std::unexpected{"读取 Linux 更新文件失败。"};
      break;
    }
    if (read == 0) break;
    result = write_all(out, std::span{buffer.data(), static_cast<std::size_t>(read)});
    if (!result) break;
  }
  const bool flushed = result && ::fsync(out) == 0;
  ::close(in);
  ::close(out);
  if (!result || !flushed) {
    std::error_code ec;
    fs::remove(destination, ec);
    if (!result) return std::unexpected{result.error()};
    return std::unexpected{"无法刷新 Linux 更新文件。"};
  }
  return {};
}

std::expected<void, std::string> ensure_install_directory_writable(
    const fs::path& directory) {
  const auto probe = directory / std::format(".awj-update-probe-{}", ::getpid());
  const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        0600);
  if (fd < 0) {
    return std::unexpected{
        "INSTALL_DIR_NOT_WRITABLE:安装目录不可写；请手动下载更新。"};
  }
  const bool flushed = ::fsync(fd) == 0;
  ::close(fd);
  std::error_code ec;
  fs::remove(probe, ec);
  if (!flushed || ec) return std::unexpected{"无法验证 Linux 安装目录写入权限。"};
  return {};
}

std::expected<fs::path, std::string> make_stage_directory(const fs::path& install,
                                                           std::string_view version) {
  const auto root = install / update_root_name;
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec) return std::unexpected{"无法创建 Linux 更新 staging 目录。"};
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    const auto stage = root / std::format("{}-{}-{}-{}", version, ::getpid(),
        std::chrono::steady_clock::now().time_since_epoch().count(), attempt);
    if (fs::create_directory(stage, ec)) return stage;
    if (ec != std::errc::file_exists) {
      return std::unexpected{"无法创建唯一的 Linux 更新 staging 目录。"};
    }
    ec.clear();
  }
  return std::unexpected{"无法分配唯一的 Linux 更新 staging 目录。"};
}

bool is_direct_stage_directory(const fs::path& stage, const fs::path& install) {
  std::error_code ec;
  const auto expected = fs::weakly_canonical(install / update_root_name, ec);
  if (ec) return false;
  const auto actual = fs::weakly_canonical(stage.parent_path(), ec);
  return !ec && actual == expected &&
         stage.parent_path().filename() == fs::path{std::string{update_root_name}};
}

fs::path transaction_pointer(const fs::path& install) {
  return install / transaction_name;
}

std::expected<fs::path, std::string> executable_for_pid(pid_t pid) {
  std::string link = std::format("/proc/{}/exe", pid);
  std::string buffer(4096, '\0');
  const auto length = ::readlink(link.c_str(), buffer.data(), buffer.size());
  if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size()) {
    return std::unexpected{"无法确认 Linux 更新父进程路径。"};
  }
  buffer.resize(static_cast<std::size_t>(length));
  return fs::path{buffer};
}

bool wait_for_exit(pid_t pid, std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  return false;
}

std::expected<void, std::string> run_health_check(const fs::path& executable) {
  const pid_t child = ::fork();
  if (child < 0) return std::unexpected{"无法启动 Linux 更新健康检查。"};
  if (child == 0) {
    ::execl(executable.c_str(), executable.c_str(), "--linux-update-health-check",
            static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = ::waitpid(child, &status, WNOHANG);
    if (result == child) {
      return WIFEXITED(status) && WEXITSTATUS(status) == 0
                 ? std::expected<void, std::string>{}
                 : std::unexpected{"Linux 更新健康检查失败。"};
    }
    if (result < 0 && errno != EINTR) {
      return std::unexpected{"等待 Linux 更新健康检查失败。"};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  ::kill(child, SIGTERM);
  ::waitpid(child, &status, 0);
  return std::unexpected{"Linux 更新健康检查超时。"};
}

std::expected<void, std::string> launch_studio(const fs::path& executable) {
  const pid_t child = ::fork();
  if (child < 0) return std::unexpected{"无法启动更新后的 AWJ。"};
  if (child == 0) {
    ::setsid();
    ::execl(executable.c_str(), executable.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  return {};
}

std::expected<void, std::string> restore_backup(const fs::path& install,
                                                 const fs::path& stage) {
  const auto target = install / "AWJ";
  const auto backup = stage / "AWJ.old";
  std::error_code ec;
  if (fs::is_regular_file(backup, ec) && !ec) {
    fs::remove(target, ec);
    ec.clear();
    if (::rename(backup.c_str(), target.c_str()) != 0) {
      return std::unexpected{"Linux 更新回滚失败。"};
    }
    if (auto synced = fsync_directory(install); !synced) return synced;
  }
  fs::remove(transaction_pointer(install), ec);
  return {};
}

std::expected<void, std::string> verify_stage(const fs::path& stage,
                                              const fs::path& install,
                                              fs::path& target,
                                              ArchiveMemberInfo const*& member) {
  auto version = read_text(stage / "version.txt", 64);
  auto raw = read_compatible_stage_text(stage, "update-archive.json",
                                        "manifest-v2.json", maximum_manifest_bytes);
  auto signature = read_compatible_stage_text(stage, "update-archive.json.sig",
                                              "manifest-v2.sig", maximum_signature_bytes);
  auto keyring_raw = read_compatible_stage_text(stage, "update-keyring.json",
                                                "update-keyring-v1.json",
                                                maximum_manifest_bytes);
  auto keyring_signature = read_compatible_stage_text(stage, "update-keyring.json.sig",
                                                      "update-keyring-v1.sig", 16 * 1024);
  if (!version || !raw || !signature || !keyring_raw || !keyring_signature) {
    return std::unexpected{"Linux 更新事务缺少签名元数据。"};
  }
  while (!version->empty() &&
         (version->back() == '\r' || version->back() == '\n')) version->pop_back();
  auto keyring = verify_and_parse_update_keyring(*keyring_raw, *keyring_signature);
  if (!keyring) {
    return std::unexpected{"Linux 更新事务中的更新密钥环无效。"};
  }
  auto manifest = verify_and_parse_archive_manifest_v2(*raw, *signature, *keyring);
  const auto parsed = parse_version(*version);
  if (!manifest || !parsed) {
    return std::unexpected{"Linux 更新事务中的签名清单或版本无效。"};
  }
  if (auto accepted = accept_verified_update_document(
          UpdateSecurityDocument::keyring, keyring->sequence, *keyring_raw,
          0, install);
      !accepted) {
    return std::unexpected{accepted.error()};
  }
  if (auto accepted = accept_verified_update_document(
          UpdateSecurityDocument::archive_manifest_v2, manifest->sequence,
          *raw, 0, install);
      !accepted) {
    return std::unexpected{accepted.error()};
  }
  const auto* entry = find_archive_manifest_v2_entry(*manifest, *parsed);
  if (entry == nullptr || entry->revoked) {
    return std::unexpected{"Linux 更新事务目标已被撤销或不存在。"};
  }
  member = find_archive_member(entry->linux_x64_archive, "AWJ");
  if (member == nullptr || !verify_asset_file(stage / "AWJ.new", member->asset)) {
    return std::unexpected{"Linux 更新事务中的 AWJ 文件校验失败。"};
  }
  target = install / "AWJ";
  std::error_code ec;
  if (!fs::is_regular_file(target, ec) || ec) {
    return std::unexpected{"Linux 安装目录缺少原始 AWJ。"};
  }
  return {};
}

}  // namespace linux_detail

std::expected<void, std::string> stage_and_launch_linux_update(
    std::string requested_version, ChannelPreference preference,
    std::uint64_t last_verified_sequence, std::stop_token token = {}) {
  using namespace linux_detail;
  auto current = awj::executable_path();
  if (!current || current->filename() != "AWJ") {
    return std::unexpected{"无法定位当前 Linux AWJ 可执行文件。"};
  }
  const auto install = current->parent_path();
  if (auto writable = ensure_install_directory_writable(install); !writable) {
    return writable;
  }
  auto fetched = fetch_verified_archive_manifest_v2(last_verified_sequence, token);
  if (!fetched) return std::unexpected{fetched.error()};
  const auto current_version = parse_version(AWJ_BUILD_VERSION);
  if (!current_version) return std::unexpected{"当前构建版本号非法。"};
  const auto candidate = select_archive_candidate_v2(
      fetched->manifest, {.current_version = *current_version,
                          .updater_version = *current_version,
                          .preference = preference});
  if (!candidate || to_string(candidate->version) != requested_version) {
    return std::unexpected{"签名 manifest 中的最佳候选已变化；请重新检查后再更新。"};
  }
  const auto* entry = find_archive_manifest_v2_entry(fetched->manifest,
                                                       candidate->version);
  if (entry == nullptr || entry->linux_x64_archive.members.empty()) {
    return std::unexpected{"签名 v2 manifest 缺少 Linux 归档成员。"};
  }
  const auto* member = find_archive_member(entry->linux_x64_archive, "AWJ");
  if (member == nullptr) {
    return std::unexpected{"签名 v2 manifest 的 Linux 归档缺少 AWJ。"};
  }
  auto stage = make_stage_directory(install, requested_version);
  if (!stage) return std::unexpected{stage.error()};
  const auto clean_failure = [&] {
    std::error_code ec;
    fs::remove_all(*stage, ec);
  };
  const auto archive = *stage / "AWJ_Linux.7z";
  const auto unpacked = *stage / "unpacked";
  const auto next = *stage / "AWJ.new";
  if (auto downloaded = download_https_asset(entry->linux_x64_archive.archive.url,
                                              archive,
                                              entry->linux_x64_archive.archive.size_bytes,
                                              token); !downloaded) {
    clean_failure();
    return downloaded;
  }
  if (auto extracted = extract_verified_7z_archive(
          archive, entry->linux_x64_archive, unpacked); !extracted) {
    clean_failure();
    return extracted;
  }
  if (auto copied = copy_with_fsync(unpacked / "AWJ", next, 0755); !copied) {
    clean_failure();
    return copied;
  }
  std::error_code ec;
  fs::permissions(next, fs::perms::owner_exec | fs::perms::group_exec |
                            fs::perms::others_exec,
                  fs::perm_options::add, ec);
  if (ec || !verify_asset_file(next, member->asset)) {
    clean_failure();
    return std::unexpected{"Linux 更新文件不可执行或校验失败。"};
  }
  fs::remove_all(unpacked, ec);
  ec.clear();
  fs::remove(archive, ec);
  if (ec || !atomic_write_text(*stage / "update-archive.json", fetched->raw_bytes) ||
      !atomic_write_text(*stage / "update-archive.json.sig", fetched->signature_base64) ||
      !atomic_write_text(*stage / "update-keyring.json",
                         fetched->keyring_raw_bytes) ||
      !atomic_write_text(*stage / "update-keyring.json.sig",
                         fetched->keyring_signature_envelope) ||
      !atomic_write_text(*stage / "version.txt", requested_version) ||
      !atomic_write_text(*stage / "state.txt", "staged")) {
    clean_failure();
    return std::unexpected{"无法写入 Linux 更新事务。"};
  }
  const auto helper = *stage / "AWJ-update-helper";
  if (auto copied = copy_with_fsync(*current, helper, 0755); !copied) {
    clean_failure();
    return copied;
  }
  const auto parent_pid = ::getpid();
  const pid_t child = ::fork();
  if (child < 0) {
    clean_failure();
    return std::unexpected{"无法启动 Linux 更新 helper。"};
  }
  if (child == 0) {
    ::setsid();
    const auto pid = std::to_string(parent_pid);
    ::execl(helper.c_str(), helper.c_str(), "--linux-update-helper", pid.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }
  return {};
}

int run_linux_update_helper(pid_t parent_pid) noexcept {
  using namespace linux_detail;
  try {
    auto helper = awj::executable_path();
    if (!helper || helper->filename() != "AWJ-update-helper") return 20;
    const auto stage = helper->parent_path();
    const auto install = stage.parent_path().parent_path();
    if (!is_direct_stage_directory(stage, install)) return 21;
    auto parent = executable_for_pid(parent_pid);
    if (!parent || parent->filename() != "AWJ" || parent->parent_path() != install) {
      return 22;
    }
    fs::path target;
    ArchiveMemberInfo const* member = nullptr;
    if (!verify_stage(stage, install, target, member)) return 23;
    if (!wait_for_exit(parent_pid, std::chrono::seconds{60})) return 24;
    const auto backup = stage / "AWJ.old";
    if (::rename(target.c_str(), backup.c_str()) != 0 ||
        !atomic_write_text(transaction_pointer(install), stage.string()) ||
        !atomic_write_text(stage / "state.txt", "prepared")) {
      (void)restore_backup(install, stage);
      return 25;
    }
    if (::rename((stage / "AWJ.new").c_str(), target.c_str()) != 0 ||
        !atomic_write_text(stage / "state.txt", "files-replaced") ||
        !fsync_directory(install)) {
      (void)restore_backup(install, stage);
      return 26;
    }
    if (auto health = run_health_check(target); !health) {
      (void)restore_backup(install, stage);
      return 27;
    }
    if (!atomic_write_text(stage / "state.txt", "committed")) return 28;
    std::error_code ec;
    fs::remove(transaction_pointer(install), ec);
    if (auto launched = launch_studio(target); !launched) return 29;
    fs::remove_all(stage, ec);
    return 0;
  } catch (...) {
    return 39;
  }
}

bool launch_linux_update_recovery_if_needed() noexcept {
  using namespace linux_detail;
  try {
    auto current = awj::executable_path();
    if (!current || current->filename() != "AWJ") return false;
    const auto install = current->parent_path();
    const auto pointer = transaction_pointer(install);
    std::error_code ec;
    if (!fs::is_regular_file(pointer, ec) || ec) return false;
    auto stage_text = read_text(pointer, 32768);
    if (!stage_text) return false;
    const fs::path stage{*stage_text};
    if (!is_direct_stage_directory(stage, install)) return false;
    auto state = read_text(stage / "state.txt", 64);
    if (state && *state == "committed") {
      fs::remove(pointer, ec);
      return false;
    }
    if (!restore_backup(install, stage)) return false;
    (void)launch_studio(install / "AWJ");
    fs::remove_all(stage, ec);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace awj::update

#endif
