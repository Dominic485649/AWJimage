#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

import awj.update_manifest;
import awj.update_model;
import awj.update_runtime;

namespace {

using namespace awj::update;

int fail(std::string_view message) {
  std::cerr << message << '\n';
  return 1;
}

std::string valid_manifest() {
  return R"json({
  "schema": 1,
  "sequence": 7,
  "key_id": "release-test-2026",
  "issued_at": "2026-08-01T00:00:00Z",
  "expires_at": "2026-09-01T00:00:00Z",
  "entries": [
    {
      "version": "1.0.2",
      "channel": "prerelease",
      "published_at": "2026-08-09T12:34:56Z",
      "release_url": "https://github.com/Dominic485649/AWJimage/releases/tag/1.0.2",
      "minimum_updater_version": "1.0.1",
      "assets": {
        "windows_x64_exe": {
          "url": "https://github.com/Dominic485649/AWJimage/releases/download/1.0.2/AWJ.exe",
          "size": 100,
          "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        },
        "windows_x64_com": {
          "url": "https://github.com/Dominic485649/AWJimage/releases/download/1.0.2/AWJ.com",
          "size": 101,
          "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        },
        "linux_x64": {
          "url": "https://github.com/Dominic485649/AWJimage/releases/download/1.0.2/AWJ",
          "size": 102,
          "sha256": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
        }
      },
      "changelog": {
        "zh-CN": "更新测试版",
        "en": "Update test build"
      },
      "revoked": false
    }
  ]
})json";
}

std::string replaced(std::string text, std::string_view from,
                     std::string_view to) {
  const auto position = text.find(from);
  if (position == std::string::npos) return {};
  text.replace(position, from.size(), to);
  return text;
}

int test_valid_manifest() {
  const auto parsed = parse_manifest_json(valid_manifest());
  if (!parsed || parsed->schema != 1 || parsed->sequence != 7 ||
      parsed->entries.size() != 1) {
    return fail(parsed ? "valid manifest parsed incorrectly" : parsed.error());
  }
  const auto& entry = parsed->entries.front();
  if (entry.version != Version{1, 0, 2} ||
      entry.channel != Channel::prerelease ||
      entry.windows_x64_exe.size_bytes != 100 ||
      entry.windows_x64_com.size_bytes != 101 ||
      entry.linux_x64.size_bytes != 102) {
    return fail("valid manifest fields were not preserved");
  }
  return 0;
}

int test_url_policy() {
  for (const std::string_view bad : {
           "http://github.com/Dominic485649/AWJimage/releases/tag/1.0.2",
           "https://evil.example/AWJ.exe",
           "https://github.com@evil.example/AWJ.exe",
           "https://github.com\\@evil.example/AWJ.exe",
           "https://github.com:444/AWJ.exe",
           "https://github.com/AWJ.exe#fragment",
           "https://github.com\n.evil.example/AWJ.exe"}) {
    if (parse_allowed_https_url(bad)) {
      std::cerr << "unsafe URL accepted: " << bad << '\n';
      return 1;
    }
  }
  for (const std::string_view good : {
           "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-manifest.json",
           "https://github.com/Dominic485649/AWJimage/releases/tag/1.0.2",
           "https://release-assets.githubusercontent.com/file",
           "https://objects.githubusercontent.com/file"}) {
    if (!parse_allowed_https_url(good)) {
      std::cerr << "allowed URL rejected: " << good << '\n';
      return 1;
    }
  }
  return 0;
}

int test_strict_fields() {
  const auto manifest = valid_manifest();
  for (const auto& bad : {
           replaced(manifest, "2026-08-09T12:34:56Z",
                    "2026-13-09T12:34:56Z"),
           replaced(manifest, "2026-08-09T12:34:56Z",
                    "2026-02-30T12:34:56Z"),
           replaced(manifest, "2026-08-09T12:34:56Z",
                    "2026-08-09T24:00:00Z"),
           replaced(manifest, "releases/tag/1.0.2",
                    "releases/tag/9.9.9"),
           replaced(manifest,
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
           replaced(manifest, "\"size\": 100", "\"size\": 0"),
           replaced(manifest, "\"schema\": 1", "\"schema\": 2"),
           replaced(manifest, "\"key_id\": \"release-test-2026\"",
                    "\"key_id\": \"Release_Test\""),
           replaced(manifest, "2026-09-01T00:00:00Z",
                    "2026-08-01T00:00:00Z")}) {
    if (bad.empty() || parse_manifest_json(bad)) {
      return fail("a malformed manifest field was accepted");
    }
  }
  return 0;
}

int test_expiry_window() {
  const auto now = parse_rfc3339_utc("2026-08-20T00:00:00Z", "test now");
  if (!now) return fail(now.error());
  if (!validate_signed_update_document_window(
          "2026-08-01T00:00:00Z", "2026-09-01T00:00:00Z", *now)) {
    return fail("a bounded current signed-document window was rejected");
  }
  if (validate_signed_update_document_window(
          "2026-08-01T00:00:00Z", "2026-08-19T00:00:00Z", *now)) {
    return fail("an expired signed document was accepted");
  }
  if (validate_signed_update_document_window(
          "2026-08-01T00:00:00Z", "2027-08-01T00:00:00Z", *now)) {
    return fail("an overlong signed-document window was accepted");
  }
  return 0;
}

int test_response_limits_and_signature_gate() {
  std::string oversized(maximum_manifest_bytes + 1, ' ');
  if (parse_manifest_json(oversized)) {
    return fail("an oversized manifest was accepted");
  }
  auto bom = std::string{"\xef\xbb\xbf"} + valid_manifest();
  if (parse_manifest_json(bom)) {
    return fail("a UTF-8 BOM was accepted");
  }
  auto nul = valid_manifest();
  nul.push_back('\0');
  if (parse_manifest_json(nul)) {
    return fail("a NUL byte was accepted");
  }
  if (verify_and_parse_manifest(valid_manifest(), "AAAA")) {
    return fail("an unsigned/invalidly signed manifest was accepted");
  }
  if (!update_public_key_configured()) {
    return fail("the release test build must contain the tracked update public key");
  }
  return 0;
}

int test_alias_fallback_gate() {
  if (!runtime_detail::endpoint_not_found("更新服务器返回 HTTP 404。")) {
    return fail("canonical update alias did not permit 404 fallback");
  }
  for (const std::string_view error : {
           "更新服务器返回 HTTP 403。",
           "更新服务器返回 HTTP 500。",
           "更新服务器请求失败：timeout",
           "更新密钥环签名封套格式非法。"}) {
    if (runtime_detail::endpoint_not_found(error)) {
      return fail("non-404 update failure incorrectly permitted legacy fallback");
    }
  }
  return 0;
}

int test_asset_hash_and_size() {
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("awj-update-hash-test-{}",
                                std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count());
  {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << "abc";
  }
  const AssetInfo valid{
      .size_bytes = 3,
      .sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
  const auto verified = verify_asset_file(path, valid);
  if (!verified) {
    std::filesystem::remove(path);
    return fail(verified.error());
  }
  auto wrong_hash = valid;
  wrong_hash.sha256 = std::string(64, '0');
  if (verify_asset_file(path, wrong_hash)) {
    std::filesystem::remove(path);
    return fail("an asset with the wrong SHA-256 was accepted");
  }
  auto wrong_size = valid;
  wrong_size.size_bytes = 4;
  if (verify_asset_file(path, wrong_size)) {
    std::filesystem::remove(path);
    return fail("an asset with the wrong declared size was accepted");
  }
  std::filesystem::remove(path);
  return 0;
}

}  // namespace

int main() {
  if (const auto result = test_valid_manifest()) return result;
  if (const auto result = test_url_policy()) return result;
  if (const auto result = test_strict_fields()) return result;
  if (const auto result = test_expiry_window()) return result;
  if (const auto result = test_response_limits_and_signature_gate()) return result;
  if (const auto result = test_alias_fallback_gate()) return result;
  if (const auto result = test_asset_hash_and_size()) return result;
  std::cout << "update manifest security tests passed\n";
  return 0;
}
