#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>

#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import awj.update_archive;
import awj.update_manifest_v2;
import awj.update_model;

namespace {

namespace fs = std::filesystem;

int fail(std::string_view message) {
  std::cerr << message << '\n';
  return 1;
}

std::string valid_manifest() {
  return R"json({
  "schema": 2,
  "sequence": 11,
  "key_id": "release-test-2026",
  "issued_at": "2026-08-01T00:00:00Z",
  "expires_at": "2026-09-01T00:00:00Z",
  "entries": [{
    "version": "1.0.4",
    "channel": "prerelease",
    "published_at": "2026-08-20T12:00:00Z",
    "release_url": "https://github.com/Dominic485649/AWJimage/releases/tag/1.0.4",
    "minimum_updater_version": "1.0.4",
    "assets": {
      "windows_x64_archive": {
        "archive": {"url": "https://github.com/Dominic485649/AWJimage/releases/download/1.0.4/AWJ_Win.7z", "size": 100, "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
        "members": [
          {"path": "AWJ.exe", "url": "https://github.com/Dominic485649/AWJimage/releases/download/1.0.4/AWJ_Win.7z", "size": 10, "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
          {"path": "AWJ.com", "url": "https://github.com/Dominic485649/AWJimage/releases/download/1.0.4/AWJ_Win.7z", "size": 11, "sha256": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}
        ]
      },
      "linux_x64_archive": {
        "archive": {"url": "https://github.com/Dominic485649/AWJimage/releases/download/1.0.4/AWJ_Linux.7z", "size": 101, "sha256": "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"},
        "members": [
          {"path": "AWJ", "url": "https://github.com/Dominic485649/AWJimage/releases/download/1.0.4/AWJ_Linux.7z", "size": 12, "sha256": "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"}
        ]
      }
    },
    "changelog": {"zh-CN": "归档更新", "en": "Archive update"}
  }]
})json";
}

std::string replace_once(std::string text, std::string_view from,
                         std::string_view to) {
  const auto position = text.find(from);
  if (position == std::string::npos) return {};
  text.replace(position, from.size(), to);
  return text;
}

std::expected<void, std::string> write_7z(
    const fs::path& path,
    const std::vector<std::pair<std::string, std::string>>& entries,
    const std::vector<std::string>& directories = {}) {
  archive* raw = archive_write_new();
  if (raw == nullptr) return std::unexpected{"cannot create libarchive writer"};
  const auto close = [&] {
    archive_write_close(raw);
    archive_write_free(raw);
  };
  if (archive_write_add_filter_none(raw) != ARCHIVE_OK ||
      archive_write_set_format_7zip(raw) != ARCHIVE_OK ||
      archive_write_open_filename(raw, path.string().c_str()) != ARCHIVE_OK) {
    close();
    return std::unexpected{"cannot create 7z fixture"};
  }
  for (const auto& name : directories) {
    archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, AE_IFDIR);
    archive_entry_set_perm(entry, 0755);
    archive_entry_set_size(entry, 0);
    const bool ok = archive_write_header(raw, entry) == ARCHIVE_OK;
    archive_entry_free(entry);
    if (!ok) {
      close();
      return std::unexpected{"cannot write 7z directory fixture"};
    }
  }
  for (const auto& [name, contents] : entries) {
    archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
    const bool ok = archive_write_header(raw, entry) == ARCHIVE_OK &&
                    archive_write_data(raw, contents.data(), contents.size()) ==
                        static_cast<la_ssize_t>(contents.size());
    archive_entry_free(entry);
    if (!ok) {
      close();
      return std::unexpected{"cannot write 7z fixture"};
    }
  }
  const bool ok = archive_write_close(raw) == ARCHIVE_OK;
  archive_write_free(raw);
  if (!ok) return std::unexpected{"cannot finalize 7z fixture"};
  return {};
}

std::expected<std::string, std::string> sha256_file(const fs::path& path) {
  if (sodium_init() < 0) return std::unexpected{"cannot initialize sodium"};
  std::ifstream input{path, std::ios::binary};
  if (!input) return std::unexpected{"cannot read fixture"};
  crypto_hash_sha256_state state{};
  crypto_hash_sha256_init(&state);
  std::array<unsigned char, 4096> buffer{};
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    if (const auto count = input.gcount(); count > 0) {
      crypto_hash_sha256_update(&state, buffer.data(),
                                static_cast<unsigned long long>(count));
    }
  }
  if (!input.eof()) return std::unexpected{"cannot hash fixture"};
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256_final(&state, digest.data());
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> text{};
  sodium_bin2hex(text.data(), text.size(), digest.data(), digest.size());
  return std::string{text.data()};
}

std::expected<void, std::string> set_asset(
    awj::update::AssetInfo& asset, const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec) return std::unexpected{"cannot stat fixture"};
  auto hash = sha256_file(path);
  if (!hash) return std::unexpected{hash.error()};
  asset.size_bytes = size;
  asset.sha256 = std::move(*hash);
  return {};
}

std::string sha256_bytes(std::string_view bytes) {
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256(digest.data(),
                     reinterpret_cast<const unsigned char*>(bytes.data()),
                     bytes.size());
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> text{};
  sodium_bin2hex(text.data(), text.size(), digest.data(), digest.size());
  return text.data();
}

}  // namespace

int main() {
  if (awj::update::archive_manifest_v2_url !=
          "https://raw.githubusercontent.com/Dominic485649/AWJimage/main/update-archive.json" ||
      awj::update::archive_manifest_v2_signature_url !=
          "https://raw.githubusercontent.com/Dominic485649/AWJimage/main/update-archive.json.sig") {
    return fail("归档更新入口应使用 main 主线。");
  }
  auto parsed = awj::update::parse_archive_manifest_v2_json(valid_manifest());
  if (!parsed || parsed->schema != 2 || parsed->sequence != 11 ||
      parsed->entries.size() != 1 ||
      parsed->entries.front().windows_x64_archive.members.size() != 2 ||
      !awj::update::find_archive_member(
          parsed->entries.front().windows_x64_archive, "AWJ.exe")) {
    return fail(parsed ? "valid v2 archive manifest lost member data" : parsed.error());
  }
  for (const std::string_view unsafe : {"/AWJ.exe", "../AWJ.exe", "a/../AWJ.exe",
                                        "C:AWJ.exe", "a\\AWJ.exe", "a//AWJ.exe"}) {
    if (awj::update::archive_member_path_is_safe(unsafe)) {
      return fail("unsafe archive member path was accepted");
    }
  }
  for (const auto& malformed : {
           replace_once(valid_manifest(), "\"AWJ.exe\"", "\"../AWJ.exe\""),
           replace_once(valid_manifest(), "\"AWJ.com\"", "\"awj.exe\""),
           replace_once(valid_manifest(), "\"schema\": 2", "\"schema\": 1"),
           replace_once(valid_manifest(), "\"size\": 100", "\"size\": 0")}) {
    if (malformed.empty() || awj::update::parse_archive_manifest_v2_json(malformed)) {
      return fail("malformed v2 archive manifest was accepted");
    }
  }

  const auto root = fs::temp_directory_path() /
                    std::format("awj-update-archive-test-{}",
                                std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count());
  std::error_code ec;
  fs::create_directory(root, ec);
  if (ec) return fail("cannot create archive fixture directory");
  const auto clean = [&] { fs::remove_all(root, ec); };
  const auto archive_path = root / "valid.7z";
  if (auto written = write_7z(archive_path, {{"AWJ.exe", "binary"}}); !written) {
    clean();
    return fail(written.error());
  }
  auto assets = parsed->entries.front().windows_x64_archive;
  assets.members.resize(1);
  if (auto result = set_asset(assets.archive, archive_path); !result) {
    clean();
    return fail(result.error());
  }
  assets.members.front().asset.size_bytes = std::string_view{"binary"}.size();
  assets.members.front().asset.sha256 = sha256_bytes("binary");
  if (auto extracted = awj::update::extract_verified_7z_archive(
          archive_path, assets, root / "unpacked"); !extracted ||
      !fs::is_regular_file(root / "unpacked" / "AWJ.exe")) {
    clean();
    return fail(extracted ? "valid 7z was not extracted" : extracted.error());
  }

  const auto directory_path = root / "directory.7z";
  constexpr std::string_view license_bytes = "license";
  if (auto written = write_7z(
          directory_path,
          {{"THIRD_PARTY_LICENSES/license.txt", std::string{license_bytes}}},
          {"THIRD_PARTY_LICENSES/"});
      !written) {
    clean();
    return fail(written.error());
  }
  auto directory_assets = assets;
  directory_assets.members.front().path = "THIRD_PARTY_LICENSES/license.txt";
  directory_assets.members.front().asset.size_bytes = license_bytes.size();
  directory_assets.members.front().asset.sha256 = sha256_bytes(license_bytes);
  if (auto set = set_asset(directory_assets.archive, directory_path); !set) {
    clean();
    return fail(set.error());
  }
  if (auto extracted = awj::update::extract_verified_7z_archive(
          directory_path, directory_assets, root / "directory-unpacked");
      !extracted || !fs::is_regular_file(root / "directory-unpacked" /
                                         "THIRD_PARTY_LICENSES" / "license.txt")) {
    clean();
    return fail(extracted ? "signed structural directory was not accepted"
                          : extracted.error());
  }

  const auto unexpected_directory_path = root / "unexpected-directory.7z";
  if (auto written = write_7z(unexpected_directory_path, {{"AWJ.exe", "binary"}},
                              {"unexpected/"});
      !written) {
    clean();
    return fail(written.error());
  }
  auto unexpected_directory_assets = assets;
  if (auto set = set_asset(unexpected_directory_assets.archive,
                           unexpected_directory_path);
      !set || awj::update::extract_verified_7z_archive(
                  unexpected_directory_path, unexpected_directory_assets,
                  root / "unexpected-directory-unpacked")) {
    clean();
    return fail(set ? "archive with an unsigned directory was accepted" : set.error());
  }

  const auto extra_path = root / "extra.7z";
  if (auto written = write_7z(extra_path,
      {{"AWJ.exe", "binary"}, {"unexpected.txt", "x"}}); !written) {
    clean();
    return fail(written.error());
  }
  auto extra_assets = assets;
  if (auto set = set_asset(extra_assets.archive, extra_path); !set ||
      awj::update::extract_verified_7z_archive(extra_path, extra_assets,
                                                root / "extra-unpacked")) {
    clean();
    return fail(set ? "archive with an extra member was accepted" : set.error());
  }

  const auto corrupt_path = root / "corrupt.7z";
  fs::copy_file(archive_path, corrupt_path, fs::copy_options::none, ec);
  const auto size = fs::file_size(corrupt_path, ec);
  if (ec || size < 2) {
    clean();
    return fail("cannot prepare corrupt archive fixture");
  }
  fs::resize_file(corrupt_path, size - 1, ec);
  if (ec || awj::update::extract_verified_7z_archive(
                corrupt_path, assets, root / "corrupt-unpacked")) {
    clean();
    return fail("corrupt archive was accepted");
  }
  clean();
  std::cout << "update archive manifest tests passed\n";
  return 0;
}
