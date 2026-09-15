module;

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef AWJ_UPDATE_PUBLIC_KEY_HEX
#define AWJ_UPDATE_PUBLIC_KEY_HEX ""
#endif
#ifndef AWJ_UPDATE_ROOT_RECOVERY_A_PUBLIC_KEY_HEX
#define AWJ_UPDATE_ROOT_RECOVERY_A_PUBLIC_KEY_HEX ""
#endif
#ifndef AWJ_UPDATE_ROOT_RECOVERY_B_PUBLIC_KEY_HEX
#define AWJ_UPDATE_ROOT_RECOVERY_B_PUBLIC_KEY_HEX ""
#endif

export module awj.update_keyring;

import awj.update_manifest;
import awj.update_model;

export namespace awj::update {

inline constexpr std::string_view update_keyring_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-keyring.json";
inline constexpr std::string_view update_keyring_signature_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-keyring.json.sig";
inline constexpr std::string_view legacy_update_keyring_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-keyring-v1.json";
inline constexpr std::string_view legacy_update_keyring_signature_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-keyring-v1.json.sig";
inline constexpr std::uint32_t supported_update_keyring_schema = 1;

struct UpdateReleaseKey {
  std::string key_id{};
  std::string public_key_hex{};
  std::string not_before{};
  std::string expires_at{};
  bool revoked{};
};

struct UpdateKeyring {
  std::uint32_t schema{};
  std::uint64_t sequence{};
  std::string issued_at{};
  std::string expires_at{};
  std::vector<UpdateReleaseKey> release_keys{};
};

struct VerifiedUpdateKeyring {
  UpdateKeyring keyring{};
  std::string raw_bytes{};
  std::string signature_envelope{};
};

namespace keyring_detail {

using Json = nlohmann::ordered_json;

struct RootKey {
  std::string_view key_id{};
  std::string_view public_key_hex{};
};

constexpr std::array root_keys{
    RootKey{"root-legacy-2026", AWJ_UPDATE_PUBLIC_KEY_HEX},
    RootKey{"root-recovery-a-2026", AWJ_UPDATE_ROOT_RECOVERY_A_PUBLIC_KEY_HEX},
    RootKey{"root-recovery-b-2026", AWJ_UPDATE_ROOT_RECOVERY_B_PUBLIC_KEY_HEX}};

std::expected<std::string, std::string> required_string(
    const Json& object, std::string_view field, std::size_t maximum) {
  const auto key = std::string{field};
  if (!object.contains(key) || !object.at(key).is_string()) {
    return std::unexpected{std::format("更新密钥环字段 {} 必须是字符串。", field)};
  }
  auto value = object.at(key).get<std::string>();
  if (value.empty() || value.size() > maximum || value.contains('\0')) {
    return std::unexpected{std::format("更新密钥环字段 {} 长度非法。", field)};
  }
  return value;
}

bool valid_public_key_hex(std::string_view value) noexcept {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

std::expected<void, std::string> validate_release_key_window(
    std::string_view not_before, std::string_view expires_at) {
  auto start = parse_rfc3339_utc(not_before, "release_keys.not_before");
  auto end = parse_rfc3339_utc(expires_at, "release_keys.expires_at");
  if (!start) return std::unexpected{start.error()};
  if (!end) return std::unexpected{end.error()};
  if (*end <= *start || *end - *start > std::chrono::days{366}) {
    return std::unexpected{"更新发布密钥的有效期必须为 1 到 366 天。"};
  }
  return {};
}

std::expected<void, std::string> verify_root_signature_envelope(
    std::string_view raw_bytes, std::string_view signature_envelope) {
  if (signature_envelope.empty() || signature_envelope.size() > 16 * 1024 ||
      signature_envelope.contains('\0')) {
    return std::unexpected{"更新密钥环签名封套为空、过大或含 NUL。"};
  }
  try {
    const auto envelope = Json::parse(signature_envelope.begin(),
                                      signature_envelope.end());
    if (!envelope.is_object() || !envelope.contains("schema") ||
        !envelope.at("schema").is_number_unsigned() ||
        envelope.at("schema").get<std::uint32_t>() != 1 ||
        !envelope.contains("signatures") || !envelope.at("signatures").is_array() ||
        envelope.at("signatures").size() < 2 ||
        envelope.at("signatures").size() > root_keys.size()) {
      return std::unexpected{"更新密钥环签名封套格式非法。"};
    }
    std::array<bool, root_keys.size()> used{};
    std::size_t valid_count = 0;
    for (const auto& item : envelope.at("signatures")) {
      if (!item.is_object()) {
        return std::unexpected{"更新密钥环签名项必须是对象。"};
      }
      auto key_id = required_string(item, "key_id", 64);
      auto signature = required_string(item, "signature", 256);
      if (!key_id || !signature || !valid_update_key_id(*key_id)) {
        return std::unexpected{!key_id ? key_id.error()
                            : !signature ? signature.error()
                                         : "更新密钥环签名 key_id 非法。"};
      }
      const auto root = std::ranges::find(root_keys, *key_id, &RootKey::key_id);
      if (root == root_keys.end()) {
        return std::unexpected{"更新密钥环签名引用了未知根密钥。"};
      }
      const auto index = static_cast<std::size_t>(root - root_keys.begin());
      if (used[index]) {
        return std::unexpected{"更新密钥环签名重复使用了同一根密钥。"};
      }
      used[index] = true;
      if (auto verified = verify_detached_ed25519_signature(
              raw_bytes, *signature, root->public_key_hex,
              "update-keyring-v1.json signature");
          !verified) {
        return std::unexpected{verified.error()};
      }
      ++valid_count;
    }
    if (valid_count < 2) {
      return std::unexpected{"更新密钥环至少需要两把不同根密钥的有效签名。"};
    }
    return {};
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected{std::format("解析更新密钥环签名封套失败：{}", error.what())};
  }
}

}  // namespace keyring_detail

bool update_keyring_roots_configured() noexcept {
  return std::ranges::count_if(keyring_detail::root_keys, [](const auto& root) {
           return keyring_detail::valid_public_key_hex(root.public_key_hex);
         }) >= 2;
}

std::expected<UpdateKeyring, std::string> parse_update_keyring_json(
    std::string_view raw_bytes) {
  if (raw_bytes.empty() || raw_bytes.size() > maximum_manifest_bytes ||
      raw_bytes.contains('\0') ||
      (raw_bytes.size() >= 3 &&
       static_cast<unsigned char>(raw_bytes[0]) == 0xef &&
       static_cast<unsigned char>(raw_bytes[1]) == 0xbb &&
       static_cast<unsigned char>(raw_bytes[2]) == 0xbf)) {
    return std::unexpected{"更新密钥环为空、过大、含 NUL 或带 UTF-8 BOM。"};
  }
  try {
    const auto document = keyring_detail::Json::parse(raw_bytes.begin(), raw_bytes.end());
    if (!document.is_object() || !document.contains("schema") ||
        !document.at("schema").is_number_unsigned() ||
        !document.contains("sequence") || !document.at("sequence").is_number_unsigned() ||
        !document.contains("release_keys") || !document.at("release_keys").is_array()) {
      return std::unexpected{"更新密钥环根字段不完整或类型错误。"};
    }
    auto issued_at = keyring_detail::required_string(document, "issued_at", 20);
    auto expires_at = keyring_detail::required_string(document, "expires_at", 20);
    if (!issued_at || !expires_at) {
      return std::unexpected{!issued_at ? issued_at.error() : expires_at.error()};
    }
    auto issued = parse_rfc3339_utc(*issued_at, "keyring issued_at");
    auto expires = parse_rfc3339_utc(*expires_at, "keyring expires_at");
    if (!issued || !expires || *expires <= *issued ||
        *expires - *issued > maximum_signed_update_document_lifetime) {
      return std::unexpected{"更新密钥环的 issued_at/expires_at 有效期非法。"};
    }
    UpdateKeyring keyring{
        .schema = document.at("schema").get<std::uint32_t>(),
        .sequence = document.at("sequence").get<std::uint64_t>(),
        .issued_at = std::move(*issued_at),
        .expires_at = std::move(*expires_at)};
    if (keyring.schema != supported_update_keyring_schema || keyring.sequence == 0 ||
        document.at("release_keys").empty() ||
        document.at("release_keys").size() > 16) {
      return std::unexpected{"更新密钥环 schema、sequence 或 release_keys 数量非法。"};
    }
    for (const auto& item : document.at("release_keys")) {
      if (!item.is_object()) {
        return std::unexpected{"更新密钥环 release_keys 元素必须是对象。"};
      }
      auto key_id = keyring_detail::required_string(item, "key_id", 64);
      auto public_key = keyring_detail::required_string(item, "public_key", 64);
      auto not_before = keyring_detail::required_string(item, "not_before", 20);
      auto key_expires = keyring_detail::required_string(item, "expires_at", 20);
      if (!key_id || !public_key || !not_before || !key_expires ||
          !valid_update_key_id(*key_id) ||
          !keyring_detail::valid_public_key_hex(*public_key)) {
        return std::unexpected{"更新密钥环 release_keys 字段非法。"};
      }
      bool revoked = false;
      if (item.contains("revoked")) {
        if (!item.at("revoked").is_boolean()) {
          return std::unexpected{"更新密钥环 release_keys.revoked 必须是布尔值。"};
        }
        revoked = item.at("revoked").get<bool>();
      }
      if (auto valid = keyring_detail::validate_release_key_window(
              *not_before, *key_expires);
          !valid) {
        return std::unexpected{valid.error()};
      }
      if (std::ranges::any_of(keyring.release_keys, [&](const auto& existing) {
            return existing.key_id == *key_id;
          })) {
        return std::unexpected{"更新密钥环 release_keys 中存在重复 key_id。"};
      }
      keyring.release_keys.push_back(UpdateReleaseKey{
          .key_id = std::move(*key_id),
          .public_key_hex = std::move(*public_key),
          .not_before = std::move(*not_before),
          .expires_at = std::move(*key_expires),
          .revoked = revoked});
    }
    return keyring;
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected{std::format("解析 update-keyring-v1.json 失败：{}", error.what())};
  }
}

std::expected<UpdateKeyring, std::string> verify_and_parse_update_keyring(
    std::string_view raw_bytes, std::string_view signature_envelope) {
  // The envelope is only an untrusted signature carrier.  Authenticate raw
  // keyring bytes with two compiled root keys before parsing their contents.
  if (auto verified = keyring_detail::verify_root_signature_envelope(
          raw_bytes, signature_envelope);
      !verified) {
    return std::unexpected{verified.error()};
  }
  auto keyring = parse_update_keyring_json(raw_bytes);
  if (!keyring) return std::unexpected{keyring.error()};
  if (auto valid = validate_signed_update_document_window(
          keyring->issued_at, keyring->expires_at);
      !valid) {
    return std::unexpected{valid.error()};
  }
  return keyring;
}

std::expected<std::string, std::string> verify_manifest_signature_with_keyring(
    std::string_view raw_bytes, std::string_view signature_base64,
    const UpdateKeyring& keyring) {
  const auto now = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
  std::string matched_key_id;
  for (const auto& key : keyring.release_keys) {
    if (key.revoked) continue;
    auto not_before = parse_rfc3339_utc(key.not_before, "release_keys.not_before");
    auto expires_at = parse_rfc3339_utc(key.expires_at, "release_keys.expires_at");
    if (!not_before || !expires_at || now < *not_before || now >= *expires_at) {
      continue;
    }
    if (verify_detached_ed25519_signature(raw_bytes, signature_base64,
                                          key.public_key_hex,
                                          "update manifest signature")) {
      if (!matched_key_id.empty()) {
        return std::unexpected{"更新 manifest 签名匹配多把发布密钥，已拒绝。"};
      }
      matched_key_id = key.key_id;
    }
  }
  if (matched_key_id.empty()) {
    return std::unexpected{"更新 manifest 未由当前未撤销的发布密钥签名。"};
  }
  return matched_key_id;
}

std::expected<Manifest, std::string> verify_and_parse_manifest_with_keyring(
    std::string_view raw_bytes, std::string_view signature_base64,
    const UpdateKeyring& keyring) {
  auto signing_key = verify_manifest_signature_with_keyring(raw_bytes,
                                                            signature_base64,
                                                            keyring);
  if (!signing_key) return std::unexpected{signing_key.error()};
  auto manifest = parse_manifest_json(raw_bytes);
  if (!manifest) return std::unexpected{manifest.error()};
  if (manifest->key_id != *signing_key) {
    return std::unexpected{"manifest key_id 与实际验签发布密钥不一致。"};
  }
  if (auto valid = validate_signed_update_document_window(
          manifest->issued_at, manifest->expires_at);
      !valid) {
    return std::unexpected{valid.error()};
  }
  return manifest;
}

}  // namespace awj::update
