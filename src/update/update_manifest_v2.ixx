module;

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module awj.update_manifest_v2;

import awj.update_manifest;
import awj.update_keyring;
import awj.update_model;

export namespace awj::update {

inline constexpr std::string_view archive_manifest_v2_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-archive.json";
inline constexpr std::string_view archive_manifest_v2_signature_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-archive.json.sig";
inline constexpr std::string_view legacy_archive_manifest_v2_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-manifest-v2.json";
inline constexpr std::string_view legacy_archive_manifest_v2_signature_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-manifest-v2.json.sig";
inline constexpr std::uint32_t supported_archive_manifest_v2_schema = 2;

struct ArchiveManifestV2 {
  std::uint32_t schema{};
  std::uint64_t sequence{};
  std::string key_id{};
  std::string issued_at{};
  std::string expires_at{};
  std::vector<ManifestEntry> entries{};
};

namespace archive_manifest_detail {

using Json = nlohmann::ordered_json;

std::expected<std::string, std::string> required_string(
    const Json& object, std::string_view name, std::size_t maximum) {
  const auto key = std::string{name};
  if (!object.contains(key) || !object.at(key).is_string()) {
    return std::unexpected{std::format("v2 manifest 字段 {} 必须是字符串。", name)};
  }
  auto value = object.at(key).get<std::string>();
  if (value.empty() || value.size() > maximum || value.contains('\0')) {
    return std::unexpected{std::format("v2 manifest 字段 {} 长度非法。", name)};
  }
  return value;
}

bool valid_rfc3339_utc(std::string_view value) noexcept {
  if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value[19] != 'Z') {
    return false;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == 19) {
      continue;
    }
    if (value[i] < '0' || value[i] > '9') return false;
  }
  const auto two = [&](std::size_t offset) noexcept {
    return static_cast<unsigned>(value[offset] - '0') * 10u +
           static_cast<unsigned>(value[offset + 1] - '0');
  };
  const auto year = two(0) * 100u + two(2);
  const auto month = two(5);
  const auto day = two(8);
  const auto hour = two(11);
  const auto minute = two(14);
  const auto second = two(17);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr std::array days_per_month{31u, 28u, 31u, 30u, 31u, 30u,
                                      31u, 31u, 30u, 31u, 30u, 31u};
  auto maximum_day = days_per_month[month - 1];
  const bool leap = year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
  if (month == 2 && leap) ++maximum_day;
  return day >= 1 && day <= maximum_day;
}

std::expected<AssetInfo, std::string> parse_asset(const Json& value,
                                                   std::string_view name) {
  if (!value.is_object()) {
    return std::unexpected{std::format("v2 manifest 资产 {} 必须是对象。", name)};
  }
  auto url = required_string(value, "url", 4096);
  if (!url) return std::unexpected{url.error()};
  if (auto allowed = parse_allowed_https_url(*url); !allowed) {
    return std::unexpected{std::format("v2 manifest 资产 {}：{}", name,
                                       allowed.error())};
  }
  if (!value.contains("size") || !value.at("size").is_number_unsigned()) {
    return std::unexpected{std::format("v2 manifest 资产 {} 缺少无符号 size。", name)};
  }
  const auto size = value.at("size").get<std::uint64_t>();
  if (size == 0 || size > maximum_asset_bytes) {
    return std::unexpected{std::format("v2 manifest 资产 {} 的 size 超出限制。", name)};
  }
  auto sha256 = required_string(value, "sha256", 64);
  if (!sha256 || sha256->size() != 64 ||
      !std::ranges::all_of(*sha256, [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      })) {
    return std::unexpected{std::format(
        "v2 manifest 资产 {} 的 SHA-256 必须是 64 位小写十六进制。", name)};
  }
  return AssetInfo{.url = std::move(*url),
                   .size_bytes = size,
                   .sha256 = std::move(*sha256)};
}

bool safe_member_path(std::string_view path) noexcept {
  if (path.empty() || path.size() > 512 || path.front() == '/' ||
      path.contains('\\') || path.contains(':') || path.contains('\0')) {
    return false;
  }
  std::size_t start = 0;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end == std::string_view::npos
                                             ? std::string_view::npos
                                             : end - start);
    if (part.empty() || part == "." || part == ".." ||
        std::ranges::any_of(part, [](unsigned char ch) {
          return ch < 0x20 || ch == 0x7f;
        })) {
      return false;
    }
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
  return false;
}

std::string ascii_fold(std::string_view value) {
  std::string out{value};
  std::ranges::transform(out, out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

std::expected<ArchiveMemberInfo, std::string> parse_member(
    const Json& value, std::string_view asset_name) {
  if (!value.is_object()) {
    return std::unexpected{std::format("v2 manifest {} 成员必须是对象。", asset_name)};
  }
  auto path = required_string(value, "path", 512);
  if (!path || !safe_member_path(*path)) {
    return std::unexpected{std::format("v2 manifest {} 成员路径不安全。", asset_name)};
  }
  auto asset = parse_asset(value, std::format("{} 成员 {}", asset_name, *path));
  if (!asset) return std::unexpected{asset.error()};
  return ArchiveMemberInfo{.path = std::move(*path),
                           .asset = std::move(*asset)};
}

std::expected<ArchiveAssetInfo, std::string> parse_archive_asset(
    const Json& value, std::string_view name) {
  if (!value.is_object() || !value.contains("archive") ||
      !value.contains("members") || !value.at("members").is_array()) {
    return std::unexpected{std::format("v2 manifest 归档资产 {} 结构非法。", name)};
  }
  auto archive = parse_asset(value.at("archive"), name);
  if (!archive) return std::unexpected{archive.error()};
  const auto& raw_members = value.at("members");
  if (raw_members.empty() || raw_members.size() > 32) {
    return std::unexpected{std::format("v2 manifest 归档资产 {} 成员数量非法。", name)};
  }
  ArchiveAssetInfo result{.archive = std::move(*archive)};
  result.members.reserve(raw_members.size());
  std::vector<std::string> folded_names;
  folded_names.reserve(raw_members.size());
  for (const auto& raw_member : raw_members) {
    auto member = parse_member(raw_member, name);
    if (!member) return std::unexpected{member.error()};
    // 成员 URL 是签名数据的一部分。成员位于同一个不可分割的 7z 资产内，
    // 因而必须精确指向该归档，不能借此引入第二个下载来源。
    if (member->asset.url != result.archive.url) {
      return std::unexpected{std::format(
          "v2 manifest 归档资产 {} 的成员 URL 必须与归档 URL 一致。", name)};
    }
    const auto folded = ascii_fold(member->path);
    if (std::ranges::find(folded_names, folded) != folded_names.end()) {
      return std::unexpected{std::format("v2 manifest 归档资产 {} 含大小写重复成员。", name)};
    }
    folded_names.push_back(folded);
    result.members.push_back(std::move(*member));
  }
  return result;
}

std::expected<ManifestEntry, std::string> parse_entry(const Json& value) {
  if (!value.is_object()) {
    return std::unexpected{"v2 manifest entries 的元素必须是对象。"};
  }
  auto version_text = required_string(value, "version", 64);
  auto channel_text = required_string(value, "channel", 32);
  auto release_url = required_string(value, "release_url", 4096);
  auto published_at = required_string(value, "published_at", 64);
  auto minimum_text = required_string(value, "minimum_updater_version", 64);
  if (!version_text) return std::unexpected{version_text.error()};
  if (!channel_text) return std::unexpected{channel_text.error()};
  if (!release_url) return std::unexpected{release_url.error()};
  if (!published_at) return std::unexpected{published_at.error()};
  if (!minimum_text) return std::unexpected{minimum_text.error()};
  auto version = parse_version(*version_text);
  auto channel = parse_channel(*channel_text);
  auto minimum = parse_version(*minimum_text);
  if (!version) return std::unexpected{version.error()};
  if (!channel) return std::unexpected{channel.error()};
  if (!minimum) return std::unexpected{minimum.error()};
  if (!valid_rfc3339_utc(*published_at)) {
    return std::unexpected{"v2 manifest published_at 必须是 YYYY-MM-DDTHH:MM:SSZ。"};
  }
  const auto parsed_release = parse_allowed_https_url(*release_url);
  const auto expected_release_path =
      std::format("/Dominic485649/AWJimage/releases/tag/{}", *version_text);
  if (!parsed_release || parsed_release->host != "github.com" ||
      parsed_release->path != expected_release_path) {
    return std::unexpected{"v2 manifest release_url 必须指向 AWJimage GitHub Release。"};
  }
  if (!value.contains("assets") || !value.at("assets").is_object()) {
    return std::unexpected{"v2 manifest entry 缺少 assets 对象。"};
  }
  const auto& assets = value.at("assets");
  auto windows = parse_archive_asset(
      assets.value("windows_x64_archive", Json{}), "windows_x64_archive");
  auto linux = parse_archive_asset(
      assets.value("linux_x64_archive", Json{}), "linux_x64_archive");
  if (!windows) return std::unexpected{windows.error()};
  if (!linux) return std::unexpected{linux.error()};
  if (!value.contains("changelog") || !value.at("changelog").is_object()) {
    return std::unexpected{"v2 manifest entry 缺少 changelog 对象。"};
  }
  const auto& changelog = value.at("changelog");
  auto zh = required_string(changelog, "zh-CN", 256 * 1024);
  auto en = required_string(changelog, "en", 256 * 1024);
  if (!zh) return std::unexpected{zh.error()};
  if (!en) return std::unexpected{en.error()};
  bool revoked = false;
  if (value.contains("revoked")) {
    if (!value.at("revoked").is_boolean()) {
      return std::unexpected{"v2 manifest revoked 必须是布尔值。"};
    }
    revoked = value.at("revoked").get<bool>();
  }
  return ManifestEntry{.version = *version,
                       .channel = *channel,
                       .release_url = std::move(*release_url),
                       .published_at = std::move(*published_at),
                       .minimum_updater_version = *minimum,
                       .windows_x64_archive = std::move(*windows),
                       .linux_x64_archive = std::move(*linux),
                       .changelog = {.zh_cn = std::move(*zh),
                                     .en = std::move(*en)},
                       .revoked = revoked};
}

}  // namespace archive_manifest_detail

std::expected<ArchiveManifestV2, std::string> parse_archive_manifest_v2_json(
    std::string_view raw_bytes) {
  if (raw_bytes.empty() || raw_bytes.size() > maximum_manifest_bytes ||
      raw_bytes.contains('\0') ||
      (raw_bytes.size() >= 3 && static_cast<unsigned char>(raw_bytes[0]) == 0xef &&
       static_cast<unsigned char>(raw_bytes[1]) == 0xbb &&
       static_cast<unsigned char>(raw_bytes[2]) == 0xbf)) {
    return std::unexpected{"v2 manifest 为空、过大、含 NUL 或带 UTF-8 BOM。"};
  }
  try {
    auto json = archive_manifest_detail::Json::parse(raw_bytes.begin(), raw_bytes.end());
    if (!json.is_object() || !json.contains("schema") ||
        !json.at("schema").is_number_unsigned() || !json.contains("sequence") ||
        !json.at("sequence").is_number_unsigned() || !json.contains("key_id") ||
        !json.contains("issued_at") || !json.contains("expires_at") ||
        !json.contains("entries") || !json.at("entries").is_array()) {
      return std::unexpected{"v2 manifest 根对象字段不完整或类型错误。"};
    }
    auto key_id = archive_manifest_detail::required_string(json, "key_id", 64);
    auto issued_at = archive_manifest_detail::required_string(json, "issued_at", 20);
    auto expires_at = archive_manifest_detail::required_string(json, "expires_at", 20);
    if (!key_id || !issued_at || !expires_at || !valid_update_key_id(*key_id)) {
      return std::unexpected{!key_id ? key_id.error()
                          : !issued_at ? issued_at.error()
                          : !expires_at ? expires_at.error()
                                        : "v2 manifest key_id 非法。"};
    }
    auto issued = parse_rfc3339_utc(*issued_at, "v2 manifest issued_at");
    auto expires = parse_rfc3339_utc(*expires_at, "v2 manifest expires_at");
    if (!issued || !expires || *expires <= *issued ||
        *expires - *issued > maximum_signed_update_document_lifetime) {
      return std::unexpected{"v2 manifest 的 issued_at/expires_at 有效期非法。"};
    }
    ArchiveManifestV2 manifest{.schema = json.at("schema").get<std::uint32_t>(),
                               .sequence = json.at("sequence").get<std::uint64_t>(),
                               .key_id = std::move(*key_id),
                               .issued_at = std::move(*issued_at),
                               .expires_at = std::move(*expires_at)};
    if (manifest.schema != supported_archive_manifest_v2_schema ||
        manifest.sequence == 0 || json.at("entries").size() > 256) {
      return std::unexpected{"v2 manifest schema、sequence 或 entries 数量非法。"};
    }
    manifest.entries.reserve(json.at("entries").size());
    for (const auto& value : json.at("entries")) {
      auto entry = archive_manifest_detail::parse_entry(value);
      if (!entry) return std::unexpected{entry.error()};
      manifest.entries.push_back(std::move(*entry));
    }
    Manifest compatibility{.schema = supported_manifest_schema,
                           .sequence = manifest.sequence,
                           .entries = manifest.entries};
    if (auto consistent = validate_manifest_consistency(compatibility); !consistent) {
      return std::unexpected{consistent.error()};
    }
    return manifest;
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected{std::format("解析 update-manifest-v2.json 失败：{}", error.what())};
  } catch (const std::exception& error) {
    return std::unexpected{std::format("读取 update-manifest-v2.json 失败：{}", error.what())};
  }
}

std::expected<ArchiveManifestV2, std::string> verify_and_parse_archive_manifest_v2(
    std::string_view raw_bytes, std::string_view signature_base64) {
  if (auto verified = verify_manifest_signature(raw_bytes, signature_base64);
      !verified) {
    return std::unexpected{verified.error()};
  }
  auto manifest = parse_archive_manifest_v2_json(raw_bytes);
  if (!manifest) return std::unexpected{manifest.error()};
  if (auto valid = validate_signed_update_document_window(
          manifest->issued_at, manifest->expires_at);
      !valid) {
    return std::unexpected{valid.error()};
  }
  return manifest;
}

std::expected<ArchiveManifestV2, std::string> verify_and_parse_archive_manifest_v2(
    std::string_view raw_bytes, std::string_view signature_base64,
    const UpdateKeyring& keyring) {
  auto signing_key = verify_manifest_signature_with_keyring(raw_bytes,
                                                            signature_base64,
                                                            keyring);
  if (!signing_key) return std::unexpected{signing_key.error()};
  auto manifest = parse_archive_manifest_v2_json(raw_bytes);
  if (!manifest) return std::unexpected{manifest.error()};
  if (manifest->key_id != *signing_key) {
    return std::unexpected{"v2 manifest key_id 与实际验签发布密钥不一致。"};
  }
  if (auto valid = validate_signed_update_document_window(
          manifest->issued_at, manifest->expires_at);
      !valid) {
    return std::unexpected{valid.error()};
  }
  return manifest;
}

bool is_archive_manifest_v2_fresh(const ArchiveManifestV2& manifest,
                                  std::uint64_t last_verified_sequence) noexcept {
  return manifest.sequence >= last_verified_sequence;
}

std::optional<UpdateCandidate> select_archive_candidate_v2(
    const ArchiveManifestV2& manifest, const CandidateRequest& request) {
  Manifest compatibility{.schema = supported_manifest_schema,
                         .sequence = manifest.sequence,
                         .key_id = manifest.key_id,
                         .issued_at = manifest.issued_at,
                         .expires_at = manifest.expires_at,
                         .entries = manifest.entries};
  return select_candidate(compatibility, request);
}

const ManifestEntry* find_archive_manifest_v2_entry(
    const ArchiveManifestV2& manifest, const Version& version) noexcept {
  const auto it = std::ranges::find(manifest.entries, version, &ManifestEntry::version);
  return it == manifest.entries.end() ? nullptr : &*it;
}

Manifest archive_manifest_v2_for_history(const ArchiveManifestV2& manifest) {
  return {.schema = supported_manifest_schema,
          .sequence = manifest.sequence,
          .key_id = manifest.key_id,
          .issued_at = manifest.issued_at,
          .expires_at = manifest.expires_at,
          .entries = manifest.entries};
}

}  // namespace awj::update
