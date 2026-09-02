module;

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <shlobj_core.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef AWJ_BUILD_VERSION
#define AWJ_BUILD_VERSION "0.0.0"
#endif

export module awj.update_windows;

import awj.update_manifest;
import awj.update_keyring;
import awj.update_manifest_v2;
import awj.update_model;
import awj.update_archive;
import awj.update_runtime;
import awj.update_security_state;

export namespace awj::update {

namespace windows_detail {

struct HandleCloser {
  using pointer = HANDLE;
  void operator()(HANDLE handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

UniqueHandle handle(HANDLE value) noexcept {
  return UniqueHandle{value == INVALID_HANDLE_VALUE ? nullptr : value};
}

std::expected<std::filesystem::path, std::string> module_path() {
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return std::unexpected{"无法定位当前 AWJ.exe。"};
  }
  buffer.resize(length);
  return std::filesystem::path{std::move(buffer)};
}

std::expected<std::filesystem::path, std::string> local_update_root() {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                  nullptr, &raw)) ||
      raw == nullptr) {
    return std::unexpected{"无法定位 LocalAppData 更新目录。"};
  }
  std::filesystem::path root{raw};
  CoTaskMemFree(raw);
  root /= L"AWJimage";
  root /= L"updates";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) return std::unexpected{"无法创建 LocalAppData 更新目录。"};
  return root;
}

bool is_direct_stage_directory(const std::filesystem::path& stage) {
  auto root = local_update_root();
  if (!root) return false;
  std::error_code ec;
  const auto canonical_root = std::filesystem::weakly_canonical(*root, ec);
  if (ec) return false;
  const auto canonical_stage = std::filesystem::weakly_canonical(stage, ec);
  return !ec && canonical_stage.parent_path() == canonical_root;
}

std::expected<void, std::string> write_file(
    const std::filesystem::path& path, std::span<const std::byte> bytes,
    bool create_new = true) {
  const HANDLE raw = CreateFileW(
      path.c_str(), GENERIC_WRITE, 0, nullptr,
      create_new ? CREATE_NEW : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  auto file = handle(raw);
  if (!file) return std::unexpected{"无法创建更新事务文件。"};
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, static_cast<std::size_t>(1u << 20)));
    DWORD written = 0;
    if (WriteFile(file.get(), bytes.data() + offset, chunk, &written, nullptr) ==
            FALSE ||
        written != chunk) {
      return std::unexpected{"写入更新事务文件失败。"};
    }
    offset += written;
  }
  if (FlushFileBuffers(file.get()) == FALSE) {
    return std::unexpected{"刷新更新事务文件失败。"};
  }
  return {};
}

std::expected<void, std::string> write_text(
    const std::filesystem::path& path, std::string_view text,
    bool create_new = true) {
  return write_file(path,
                    std::as_bytes(std::span{text.data(), text.size()}),
                    create_new);
}

std::expected<std::string, std::string> read_file(
    const std::filesystem::path& path, std::uint64_t maximum) {
  auto file = handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                 nullptr));
  if (!file) return std::unexpected{"无法读取更新事务文件。"};
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > maximum) {
    return std::unexpected{"更新事务文件大小非法。"};
  }
  std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD read = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, static_cast<std::size_t>(1u << 20)));
    if (ReadFile(file.get(), bytes.data() + offset, chunk, &read, nullptr) ==
            FALSE ||
        read == 0) {
      return std::unexpected{"读取更新事务文件失败。"};
    }
    offset += read;
  }
  return bytes;
}

std::expected<void, std::string> atomic_text_replace(
    const std::filesystem::path& target, std::string_view text) {
  auto temp = target;
  temp += L".tmp";
  std::error_code ec;
  std::filesystem::remove(temp, ec);
  if (auto written = write_text(temp, text); !written) return written;
  if (MoveFileExW(temp.c_str(), target.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    std::filesystem::remove(temp, ec);
    return std::unexpected{"原子替换更新事务日志失败。"};
  }
  return {};
}

std::expected<void, std::string> ensure_install_directory_writable(
    const std::filesystem::path& directory) {
  const auto probe = directory /
      std::format(L".awj-update-write-test-{}", GetCurrentProcessId());
  auto file = handle(CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY |
                                     FILE_FLAG_DELETE_ON_CLOSE,
                                 nullptr));
  if (!file) {
    return std::unexpected{
        "INSTALL_DIR_NOT_WRITABLE:安装目录不可写；请手动下载更新。"};
  }
  return {};
}

std::expected<std::filesystem::path, std::string> make_stage_directory(
    std::string_view version) {
  auto root = local_update_root();
  if (!root) return std::unexpected{root.error()};
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    auto stage = *root / std::format(L"{}-{}-{}-{}",
                                     std::wstring{version.begin(), version.end()},
                                     GetCurrentProcessId(), stamp, attempt);
    std::error_code ec;
    if (std::filesystem::create_directory(stage, ec)) return stage;
    if (ec && ec != std::errc::file_exists) {
      return std::unexpected{"无法创建更新 staging 目录。"};
    }
  }
  return std::unexpected{"无法分配唯一的更新 staging 目录。"};
}

std::expected<void, std::string> flush_copy(
    const std::filesystem::path& source,
    const std::filesystem::path& destination, bool fail_if_exists) {
  if (CopyFileW(source.c_str(), destination.c_str(), fail_if_exists) == FALSE) {
    return std::unexpected{"复制更新事务文件失败。"};
  }
  auto file = handle(CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                 nullptr));
  if (!file || FlushFileBuffers(file.get()) == FALSE) {
    return std::unexpected{"刷新更新事务副本失败。"};
  }
  return {};
}

// Stage files live under LocalAppData while the installed application may be
// on another volume (for example D:\). MoveFileEx cannot cross volumes, so
// copy into a same-volume temporary beside the target and atomically replace
// from there. The temporary is removed on every failure path.
std::expected<void, std::string> replace_from_stage(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
  auto temporary = target;
  temporary += std::format(L".awj-update-new-{}", GetCurrentProcessId());
  std::error_code ec;
  std::filesystem::remove(temporary, ec);
  if (auto copied = flush_copy(source, temporary, true); !copied) {
    return std::unexpected{copied.error()};
  }
  if (MoveFileExW(temporary.c_str(), target.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    std::filesystem::remove(temporary, ec);
    return std::unexpected{"替换已安装更新文件失败。"};
  }
  return {};
}

std::expected<PROCESS_INFORMATION, std::string> launch_process(
    std::wstring command_line, const std::filesystem::path& working_directory,
    DWORD flags = CREATE_NO_WINDOW) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                     flags, nullptr, working_directory.c_str(), &startup,
                     &process) == FALSE) {
    return std::unexpected{"启动更新 helper 失败。"};
  }
  return process;
}

std::expected<std::filesystem::path, std::string> parent_executable_path(
    DWORD parent_pid, UniqueHandle& parent_process) {
  parent_process = handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                          SYNCHRONIZE,
                                      FALSE, parent_pid));
  if (!parent_process) return std::unexpected{"无法打开更新父进程。"};
  std::wstring buffer(32768, L'\0');
  DWORD length = static_cast<DWORD>(buffer.size());
  if (QueryFullProcessImageNameW(parent_process.get(), 0, buffer.data(),
                                 &length) == FALSE) {
    return std::unexpected{"无法确认更新父进程路径。"};
  }
  buffer.resize(length);
  std::filesystem::path path{std::move(buffer)};
  if (_wcsicmp(path.filename().c_str(), L"AWJ.exe") != 0) {
    return std::unexpected{"更新 helper 只接受当前 AWJ.exe 作为父进程。"};
  }
  DWORD parent_session = 0;
  DWORD helper_session = 0;
  if (ProcessIdToSessionId(parent_pid, &parent_session) == FALSE ||
      ProcessIdToSessionId(GetCurrentProcessId(), &helper_session) == FALSE ||
      parent_session != helper_session) {
    return std::unexpected{"更新 helper 与父进程不在同一登录会话。"};
  }
  return path;
}

std::filesystem::path transaction_pointer(
    const std::filesystem::path& install_directory) {
  return install_directory / L".awj-update-transaction";
}

std::expected<void, std::string> restore_backups(
    const std::filesystem::path& install_directory,
    const std::filesystem::path& stage) {
  const auto old_exe = stage / L"AWJ.exe.old";
  const auto old_com = stage / L"AWJ.com.old";
  std::error_code ec;
  if (std::filesystem::exists(old_exe, ec) && !ec) {
    if (auto copied = flush_copy(old_exe, install_directory / L"AWJ.exe", false);
        !copied) {
      return std::unexpected{copied.error()};
    }
  }
  ec.clear();
  if (std::filesystem::exists(old_com, ec) && !ec) {
    if (auto copied = flush_copy(old_com, install_directory / L"AWJ.com", false);
        !copied) {
      return std::unexpected{copied.error()};
    }
  }
  std::filesystem::remove(transaction_pointer(install_directory), ec);
  return {};
}

void cleanup_stage_after_success(const std::filesystem::path& stage) noexcept {
  std::error_code ec;
  for (const auto* name : {L"AWJ.exe.old", L"AWJ.com.old", L"manifest-v2.json",
                           L"manifest-v2.sig", L"update-keyring-v1.json",
                           L"update-keyring-v1.sig", L"version.txt", L"state.txt"}) {
    std::filesystem::remove(stage / name, ec);
    ec.clear();
  }
  if (auto self = module_path()) {
    MoveFileExW(self->c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
  }
}

}  // namespace windows_detail

std::expected<void, std::string> stage_and_launch_update(
    std::string requested_version, ChannelPreference preference,
    std::uint64_t last_verified_sequence, std::stop_token token = {}) {
  auto current_path = windows_detail::module_path();
  if (!current_path) return std::unexpected{current_path.error()};
  const auto install_directory = current_path->parent_path();
  if (auto writable =
          windows_detail::ensure_install_directory_writable(install_directory);
      !writable) {
    return writable;
  }
  auto fetched = fetch_verified_archive_manifest_v2(last_verified_sequence, token);
  if (!fetched) return std::unexpected{fetched.error()};
  const auto current = parse_version(AWJ_BUILD_VERSION);
  if (!current) return std::unexpected{"当前构建版本号非法。"};
  const auto candidate = select_archive_candidate_v2(
      fetched->manifest,
      {.current_version = *current,
       .updater_version = *current,
       .preference = preference});
  if (!candidate || to_string(candidate->version) != requested_version) {
    return std::unexpected{
        "签名 manifest 中的最佳候选已变化；请重新检查后再更新。"};
  }
  auto stage = windows_detail::make_stage_directory(requested_version);
  if (!stage) return std::unexpected{stage.error()};
  const auto cleanup_on_failure = [&] {
    std::error_code ec;
    std::filesystem::remove_all(*stage, ec);
  };
  const auto* entry = find_archive_manifest_v2_entry(fetched->manifest,
                                                       candidate->version);
  if (entry == nullptr || entry->windows_x64_archive.members.empty()) {
    cleanup_on_failure();
    return std::unexpected{"签名 v2 manifest 缺少 Windows 归档成员。"};
  }
  const auto* exe_member = find_archive_member(entry->windows_x64_archive, "AWJ.exe");
  const auto* com_member = find_archive_member(entry->windows_x64_archive, "AWJ.com");
  if (exe_member == nullptr || com_member == nullptr) {
    cleanup_on_failure();
    return std::unexpected{"签名 v2 manifest 的 Windows 归档缺少 AWJ.exe 或 AWJ.com。"};
  }
  const auto archive_path = *stage / L"AWJ_Win.7z";
  const auto unpacked = *stage / L"unpacked";
  const auto new_exe = *stage / L"AWJ.exe.new";
  const auto new_com = *stage / L"AWJ.com.new";
  if (auto downloaded = download_https_asset(
          entry->windows_x64_archive.archive.url, archive_path,
          entry->windows_x64_archive.archive.size_bytes, token);
      !downloaded) {
    cleanup_on_failure();
    return downloaded;
  }
  if (auto extracted = extract_verified_7z_archive(
          archive_path, entry->windows_x64_archive, unpacked);
      !extracted) {
    cleanup_on_failure();
    return extracted;
  }
  if (auto copied = windows_detail::flush_copy(unpacked / L"AWJ.exe", new_exe, true);
      !copied) {
    cleanup_on_failure();
    return copied;
  }
  if (auto copied = windows_detail::flush_copy(unpacked / L"AWJ.com", new_com, true);
      !copied) {
    cleanup_on_failure();
    return copied;
  }
  if (auto valid = verify_asset_file(new_exe, exe_member->asset); !valid) {
    cleanup_on_failure();
    return valid;
  }
  if (auto valid = verify_asset_file(new_com, com_member->asset); !valid) {
    cleanup_on_failure();
    return valid;
  }
  std::error_code extraction_cleanup_error;
  std::filesystem::remove_all(unpacked, extraction_cleanup_error);
  std::filesystem::remove(archive_path, extraction_cleanup_error);
  if (extraction_cleanup_error) {
    cleanup_on_failure();
    return std::unexpected{"无法清理已校验的归档 staging 文件。"};
  }
  if (auto saved = windows_detail::write_text(*stage / L"manifest-v2.json",
                                               fetched->raw_bytes);
      !saved) {
    cleanup_on_failure();
    return saved;
  }
  if (auto saved = windows_detail::write_text(*stage / L"manifest-v2.sig",
                                               fetched->signature_base64);
      !saved) {
    cleanup_on_failure();
    return saved;
  }
  if (auto saved = windows_detail::write_text(*stage / L"update-keyring-v1.json",
                                               fetched->keyring_raw_bytes);
      !saved) {
    cleanup_on_failure();
    return saved;
  }
  if (auto saved = windows_detail::write_text(*stage / L"update-keyring-v1.sig",
                                               fetched->keyring_signature_envelope);
      !saved) {
    cleanup_on_failure();
    return saved;
  }
  if (auto saved = windows_detail::write_text(*stage / L"version.txt",
                                               requested_version);
      !saved) {
    cleanup_on_failure();
    return saved;
  }
  if (auto saved = windows_detail::write_text(*stage / L"state.txt", "staged");
      !saved) {
    cleanup_on_failure();
    return saved;
  }
  const auto helper = *stage / L"AWJ-update-helper.exe";
  if (auto copied = windows_detail::flush_copy(*current_path, helper, true);
      !copied) {
    cleanup_on_failure();
    return copied;
  }
  auto launched = windows_detail::launch_process(
      std::format(L"\"{}\" --update-helper {}", helper.wstring(),
                  GetCurrentProcessId()),
      *stage);
  if (!launched) {
    cleanup_on_failure();
    return std::unexpected{launched.error()};
  }
  CloseHandle(launched->hThread);
  CloseHandle(launched->hProcess);
  return {};
}

int run_update_helper(DWORD parent_pid) noexcept {
  try {
    using namespace windows_detail;
    auto self = module_path();
    if (!self || _wcsicmp(self->filename().c_str(), L"AWJ-update-helper.exe") != 0) {
      return 20;
    }
    const auto stage = self->parent_path();
    if (!is_direct_stage_directory(stage)) return 21;
    UniqueHandle parent{};
    auto parent_path = parent_executable_path(parent_pid, parent);
    if (!parent_path || parent_path->parent_path() == stage) return 22;
    const auto install = parent_path->parent_path();
    const auto target_com = install / L"AWJ.com";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(target_com, ec) || ec) return 23;
    auto version = read_file(stage / L"version.txt", 64);
    auto raw = read_file(stage / L"manifest-v2.json", maximum_manifest_bytes);
    auto signature = read_file(stage / L"manifest-v2.sig", maximum_signature_bytes);
    auto keyring_raw = read_file(stage / L"update-keyring-v1.json",
                                 maximum_manifest_bytes);
    auto keyring_signature = read_file(stage / L"update-keyring-v1.sig", 16 * 1024);
    if (!version || !raw || !signature || !keyring_raw || !keyring_signature) return 24;
    while (!version->empty() &&
           (version->back() == '\r' || version->back() == '\n')) {
      version->pop_back();
    }
    auto keyring = verify_and_parse_update_keyring(*keyring_raw, *keyring_signature);
    if (!keyring) return 25;
    auto manifest = verify_and_parse_archive_manifest_v2(*raw, *signature, *keyring);
    if (!manifest) return 25;
    if (!accept_verified_update_document(UpdateSecurityDocument::keyring,
                                         keyring->sequence, *keyring_raw, 0,
                                         install) ||
        !accept_verified_update_document(UpdateSecurityDocument::archive_manifest_v2,
                                         manifest->sequence, *raw, 0, install)) {
      return 25;
    }
    const auto parsed_version = parse_version(*version);
    if (!parsed_version) return 26;
    const auto* entry = find_archive_manifest_v2_entry(*manifest, *parsed_version);
    if (entry == nullptr || entry->revoked) return 27;
    const auto* exe_member = find_archive_member(entry->windows_x64_archive, "AWJ.exe");
    const auto* com_member = find_archive_member(entry->windows_x64_archive, "AWJ.com");
    if (exe_member == nullptr || com_member == nullptr) return 27;
    const auto new_exe = stage / L"AWJ.exe.new";
    const auto new_com = stage / L"AWJ.com.new";
    if (!verify_asset_file(new_exe, exe_member->asset) ||
        !verify_asset_file(new_com, com_member->asset)) {
      return 28;
    }
    if (WaitForSingleObject(parent.get(), 60000) != WAIT_OBJECT_0) return 29;

    const auto old_exe = stage / L"AWJ.exe.old";
    const auto old_com = stage / L"AWJ.com.old";
    if (!flush_copy(*parent_path, old_exe, true) ||
        !flush_copy(target_com, old_com, true)) {
      return 30;
    }
    const auto pointer = transaction_pointer(install);
    const auto stage_utf8 = stage.u8string();
    const std::string pointer_text{
        reinterpret_cast<const char*>(stage_utf8.data()), stage_utf8.size()};
    if (!atomic_text_replace(pointer, pointer_text) ||
        !atomic_text_replace(stage / L"state.txt", "prepared")) {
      return 31;
    }
    if (!replace_from_stage(new_exe, *parent_path)) {
      (void)restore_backups(install, stage);
      return 32;
    }
    (void)atomic_text_replace(stage / L"state.txt", "exe-replaced");
    if (!replace_from_stage(new_com, target_com)) {
      (void)restore_backups(install, stage);
      return 33;
    }
    (void)atomic_text_replace(stage / L"state.txt", "files-replaced");

    const auto event_name = std::format(
        L"Local\\AWJUpdateHealth-{}-{}", GetCurrentProcessId(),
        static_cast<unsigned long long>(GetTickCount64()));
    auto health = handle(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    if (!health) {
      (void)restore_backups(install, stage);
      return 34;
    }
    auto launched = launch_process(
        std::format(L"\"{}\" --update-health-check \"{}\" \"{}\"",
                    parent_path->wstring(), event_name,
                    std::wstring{version->begin(), version->end()}),
        install, 0);
    if (!launched) {
      (void)restore_backups(install, stage);
      return 35;
    }
    CloseHandle(launched->hThread);
    auto new_process = handle(launched->hProcess);
    const bool signaled = WaitForSingleObject(health.get(), 30000) == WAIT_OBJECT_0;
    const bool process_alive_after_grace =
        signaled && WaitForSingleObject(new_process.get(), 3000) == WAIT_TIMEOUT;
    if (classify_update_health_observation(signaled, process_alive_after_grace) !=
        UpdateHealthObservation::ready) {
      TerminateProcess(new_process.get(), 36);
      WaitForSingleObject(new_process.get(), 10000);
      (void)restore_backups(install, stage);
      return 36;
    }
    (void)atomic_text_replace(stage / L"state.txt", "committed");
    std::filesystem::remove(pointer, ec);
    cleanup_stage_after_success(stage);
    return 0;
  } catch (...) {
    return 39;
  }
}

bool launch_update_recovery_if_needed() noexcept {
  try {
    using namespace windows_detail;
    auto current = module_path();
    if (!current || _wcsicmp(current->filename().c_str(), L"AWJ.exe") != 0) {
      return false;
    }
    const auto pointer = transaction_pointer(current->parent_path());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pointer, ec) || ec) return false;
    auto stage_text = read_file(pointer, 32768);
    if (!stage_text) return false;
    const std::u8string stage_utf8{
        reinterpret_cast<const char8_t*>(stage_text->data()), stage_text->size()};
    const std::filesystem::path stage{stage_utf8};
    if (!is_direct_stage_directory(stage)) return false;
    auto state = read_file(stage / L"state.txt", 64);
    if (state && *state == "committed") {
      std::filesystem::remove(pointer, ec);
      return false;
    }
    if (!std::filesystem::exists(stage / L"AWJ.exe.old", ec) || ec) {
      std::filesystem::remove(pointer, ec);
      return false;
    }
    const auto recovery = stage / L"AWJ-recovery-helper.exe";
    std::filesystem::remove(recovery, ec);
    if (!flush_copy(*current, recovery, true)) return false;
    auto launched = launch_process(
        std::format(L"\"{}\" --update-recover {}", recovery.wstring(),
                    GetCurrentProcessId()),
        stage);
    if (!launched) return false;
    CloseHandle(launched->hThread);
    CloseHandle(launched->hProcess);
    return true;
  } catch (...) {
    return false;
  }
}

int run_update_recovery_helper(DWORD parent_pid) noexcept {
  try {
    using namespace windows_detail;
    auto self = module_path();
    if (!self ||
        _wcsicmp(self->filename().c_str(), L"AWJ-recovery-helper.exe") != 0) {
      return 40;
    }
    const auto stage = self->parent_path();
    if (!is_direct_stage_directory(stage)) return 41;
    UniqueHandle parent{};
    auto parent_path = parent_executable_path(parent_pid, parent);
    if (!parent_path || WaitForSingleObject(parent.get(), 60000) != WAIT_OBJECT_0) {
      return 42;
    }
    if (auto restored = restore_backups(parent_path->parent_path(), stage);
        !restored) {
      return 43;
    }
    std::error_code ec;
    for (const auto* name : {L"manifest-v2.json", L"manifest-v2.sig",
                             L"update-keyring-v1.json", L"update-keyring-v1.sig",
                             L"version.txt", L"state.txt", L"AWJ.exe.new", L"AWJ.com.new"}) {
      std::filesystem::remove(stage / name, ec);
      ec.clear();
    }
    MoveFileExW(self->c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
  } catch (...) {
    return 49;
  }
}

}  // namespace awj::update
