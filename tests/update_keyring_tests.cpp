#include <sodium.h>

#include <array>
#include <chrono>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

import awj.update_keyring;

namespace {

int fail(std::string_view message) {
  std::cerr << message << '\n';
  return 1;
}

std::string utc(std::chrono::system_clock::time_point point) {
  return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                     std::chrono::floor<std::chrono::seconds>(point));
}

std::string hex(const std::array<unsigned char, crypto_sign_PUBLICKEYBYTES>& value) {
  std::array<char, crypto_sign_PUBLICKEYBYTES * 2 + 1> out{};
  sodium_bin2hex(out.data(), out.size(), value.data(), value.size());
  return out.data();
}

std::string signature(std::string_view raw,
                      const std::array<unsigned char, crypto_sign_SECRETKEYBYTES>& key) {
  std::array<unsigned char, crypto_sign_BYTES> detached{};
  crypto_sign_detached(detached.data(), nullptr,
                       reinterpret_cast<const unsigned char*>(raw.data()),
                       raw.size(), key.data());
  std::array<char, sodium_base64_ENCODED_LEN(
                       crypto_sign_BYTES, sodium_base64_VARIANT_ORIGINAL)> text{};
  sodium_bin2base64(text.data(), text.size(), detached.data(), detached.size(),
                    sodium_base64_VARIANT_ORIGINAL);
  return text.data();
}

}  // namespace

int main() {
  if (awj::update::update_keyring_url !=
          "https://raw.githubusercontent.com/Dominic485649/AWJimage/main/update-keyring.json" ||
      awj::update::update_keyring_signature_url !=
          "https://raw.githubusercontent.com/Dominic485649/AWJimage/main/update-keyring.json.sig") {
    return fail("密钥环入口应使用 main 主线。");
  }
  if (sodium_init() < 0) return fail("cannot initialize sodium");
  const auto now = std::chrono::system_clock::now();
  const auto not_before = utc(now - std::chrono::hours{1});
  const auto expires_at = utc(now + std::chrono::hours{1});

  std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
  for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<unsigned char>(i);
  std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
  std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret_key{};
  crypto_sign_seed_keypair(public_key.data(), secret_key.data(), seed.data());

  awj::update::UpdateKeyring keyring{
      .schema = awj::update::supported_update_keyring_schema,
      .sequence = 1,
      .issued_at = not_before,
      .expires_at = expires_at,
      .release_keys = {{.key_id = "release-test-2026",
                        .public_key_hex = hex(public_key),
                        .not_before = not_before,
                        .expires_at = expires_at,
                        .revoked = false}}};
  constexpr std::string_view raw{"manifest payload"};
  const auto signed_raw = signature(raw, secret_key);
  const auto verified = awj::update::verify_manifest_signature_with_keyring(
      raw, signed_raw, keyring);
  if (!verified || *verified != "release-test-2026") {
    return fail(verified ? "release key id was not retained" : verified.error());
  }
  keyring.release_keys.front().revoked = true;
  if (awj::update::verify_manifest_signature_with_keyring(raw, signed_raw, keyring)) {
    return fail("a revoked release key was accepted");
  }

  const std::string one_signature_envelope =
      R"json({"schema":1,"signatures":[{"key_id":"root-legacy-2026","signature":"AAAA"}]})json";
  const std::string minimal_keyring =
      std::format(R"json({{"schema":1,"sequence":1,"issued_at":"{}","expires_at":"{}","release_keys":[{{"key_id":"release-test-2026","public_key":"{}","not_before":"{}","expires_at":"{}","revoked":false}}]}})json",
                  not_before, expires_at, hex(public_key), not_before, expires_at);
  if (awj::update::verify_and_parse_update_keyring(minimal_keyring,
                                                    one_signature_envelope)) {
    return fail("a keyring with fewer than two root signatures was accepted");
  }

  std::cout << "update keyring tests passed\n";
  return 0;
}
