module;

#include <sodium.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

export module awj.update_runtime;

import awj.update_manifest;
import awj.update_keyring;
import awj.update_manifest_v2;
import awj.update_model;
import awj.update_security_state;

export namespace awj::update {

struct VerifiedManifest {
  Manifest manifest{};
  std::string raw_bytes{};
  std::string signature_base64{};
};

struct VerifiedArchiveManifestV2 {
  ArchiveManifestV2 manifest{};
  std::string raw_bytes{};
  std::string signature_base64{};
  std::string keyring_raw_bytes{};
  std::string keyring_signature_envelope{};
};

std::expected<void, std::string> verify_asset_file(
    const std::filesystem::path& path, const AssetInfo& asset) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size != asset.size_bytes) {
    return std::unexpected{"更新资产的实际大小与签名 manifest 不一致。"};
  }
  if (sodium_init() < 0) return std::unexpected{"初始化 SHA-256 失败。"};
  std::ifstream input{path, std::ios::binary};
  if (!input) return std::unexpected{"无法读取待校验更新资产。"};
  crypto_hash_sha256_state state{};
  crypto_hash_sha256_init(&state);
  std::array<unsigned char, 128 * 1024> buffer{};
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      crypto_hash_sha256_update(
          &state, buffer.data(), static_cast<unsigned long long>(count));
    }
  }
  if (!input.eof()) return std::unexpected{"读取待校验更新资产失败。"};
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256_final(&state, digest.data());
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
  if (std::string_view{hex.data(), digest.size() * 2} != asset.sha256) {
    return std::unexpected{"更新资产 SHA-256 校验失败。"};
  }
  return {};
}

#ifdef _WIN32
namespace runtime_detail {

struct WinHttpCloser {
  using pointer = HINTERNET;
  void operator()(HINTERNET handle) const noexcept {
    if (handle != nullptr) WinHttpCloseHandle(handle);
  }
};
using UniqueWinHttp = std::unique_ptr<void, WinHttpCloser>;

struct GlobalMemoryFree {
  void operator()(wchar_t* value) const noexcept {
    if (value != nullptr) GlobalFree(value);
  }
};
using GlobalWideString = std::unique_ptr<wchar_t, GlobalMemoryFree>;

struct ProxyInfoMemory {
  WINHTTP_PROXY_INFO value{};
  ProxyInfoMemory() = default;
  ProxyInfoMemory(const ProxyInfoMemory&) = delete;
  ProxyInfoMemory& operator=(const ProxyInfoMemory&) = delete;
  ProxyInfoMemory(ProxyInfoMemory&& other) noexcept : value(other.value) {
    other.value = {};
  }
  ProxyInfoMemory& operator=(ProxyInfoMemory&& other) noexcept {
    if (this != &other) {
      if (value.lpszProxy != nullptr) GlobalFree(value.lpszProxy);
      if (value.lpszProxyBypass != nullptr) GlobalFree(value.lpszProxyBypass);
      value = other.value;
      other.value = {};
    }
    return *this;
  }
  ~ProxyInfoMemory() {
    if (value.lpszProxy != nullptr) GlobalFree(value.lpszProxy);
    if (value.lpszProxyBypass != nullptr) GlobalFree(value.lpszProxyBypass);
  }
};

struct AppliedProxy {
  GlobalWideString proxy{};
  GlobalWideString bypass{};
  ProxyInfoMemory resolved{};
};

std::expected<std::wstring, std::string> utf8_to_wide(std::string_view text) {
  if (text.empty()) return std::wstring{};
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected{"更新 URL 超出 Windows 字符串限制。"};
  }
  const int needed = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0);
  if (needed <= 0) return std::unexpected{"更新 URL 不是有效 UTF-8。"};
  std::wstring result(static_cast<std::size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), needed) !=
      needed) {
    return std::unexpected{"转换更新 URL 失败。"};
  }
  return result;
}

std::expected<std::string, std::string> wide_to_utf8(std::wstring_view text) {
  if (text.empty()) return std::string{};
  const int needed = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::unexpected{"HTTP 重定向地址不是有效 Unicode。"};
  std::string result(static_cast<std::size_t>(needed), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), needed,
                          nullptr, nullptr) != needed) {
    return std::unexpected{"转换 HTTP 重定向地址失败。"};
  }
  return result;
}

std::expected<void, std::string> apply_current_user_proxy(
    HINTERNET session, const std::wstring& url, AppliedProxy& applied) {
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
  if (WinHttpGetIEProxyConfigForCurrentUser(&ie) == FALSE) {
    // Keep WinHTTP's automatic proxy mode when the user has no readable
    // WinINet profile (for example, a service account or a fresh profile).
    return {};
  }
  GlobalWideString auto_config{ie.lpszAutoConfigUrl};
  GlobalWideString proxy{ie.lpszProxy};
  GlobalWideString bypass{ie.lpszProxyBypass};

  if (proxy && *proxy) {
    applied.proxy.reset(proxy.release());
    applied.bypass.reset(bypass.release());
    WINHTTP_PROXY_INFO info{
        .dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        .lpszProxy = applied.proxy.get(),
        .lpszProxyBypass = applied.bypass.get(),
    };
    if (WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &info,
                         sizeof(info)) == FALSE) {
      return std::unexpected{"应用 Windows 用户代理设置失败。"};
    }
    return {};
  }

  const bool has_auto_config = auto_config && *auto_config;
  if (!ie.fAutoDetect && !has_auto_config) return {};

  WINHTTP_AUTOPROXY_OPTIONS options{};
  if (ie.fAutoDetect) {
    options.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
    options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP |
                                WINHTTP_AUTO_DETECT_TYPE_DNS_A;
  }
  if (has_auto_config) {
    options.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
    options.lpszAutoConfigUrl = auto_config.get();
  }
  options.fAutoLogonIfChallenged = TRUE;

  ProxyInfoMemory resolved;
  if (WinHttpGetProxyForUrl(session, url.c_str(), &options,
                            &resolved.value) == FALSE) {
    // WPAD/PAC is optional; when discovery is unavailable, use a direct
    // connection instead of turning a normal direct connection into a hard
    // update failure or inheriting a stale machine-wide proxy.
    WINHTTP_PROXY_INFO direct{.dwAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY};
    if (WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &direct,
                         sizeof(direct)) == FALSE) {
      return std::unexpected{"应用直连更新设置失败。"};
    }
    return {};
  }
  if (WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &resolved.value,
                       sizeof(resolved.value)) == FALSE) {
    return std::unexpected{"应用 Windows 自动代理设置失败。"};
  }
  applied.resolved = std::move(resolved);
  return {};
}

struct OpenResponse {
  UniqueWinHttp session{};
  UniqueWinHttp connection{};
  UniqueWinHttp request{};
  AppliedProxy proxy{};
  std::string final_url{};
};

std::expected<std::string, std::string> query_header_string(
    HINTERNET request, DWORD query) {
  DWORD bytes = 0;
  WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                      &bytes, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) {
    return std::unexpected{"HTTP 响应缺少所需头字段。"};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                          value.data(), &bytes,
                          WINHTTP_NO_HEADER_INDEX) == FALSE) {
    return std::unexpected{"读取 HTTP 响应头失败。"};
  }
  value.resize(bytes / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  return wide_to_utf8(value);
}

std::expected<OpenResponse, std::string> open_response(
    std::string initial_url, std::stop_token token) {
  constexpr int maximum_redirects = 4;
  auto session = UniqueWinHttp{WinHttpOpen(
      L"AWJimage updater/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session) return std::unexpected{"初始化 WinHTTP 失败。"};
  if (WinHttpSetTimeouts(session.get(), 5000, 5000, 15000, 15000) == FALSE) {
    return std::unexpected{"设置 WinHTTP 超时失败。"};
  }

  std::string current = std::move(initial_url);
  auto proxy_url = utf8_to_wide(current);
  if (!proxy_url) return std::unexpected{proxy_url.error()};
  AppliedProxy proxy_storage;
  if (auto proxy = apply_current_user_proxy(session.get(), *proxy_url,
                                            proxy_storage);
      !proxy) {
    return std::unexpected{proxy.error()};
  }
  for (int redirect = 0; redirect <= maximum_redirects; ++redirect) {
    if (token.stop_requested()) return std::unexpected{"更新请求已取消。"};
    auto parsed = parse_allowed_https_url(current);
    if (!parsed) return std::unexpected{parsed.error()};
    auto host = utf8_to_wide(parsed->host);
    auto path = utf8_to_wide(parsed->path);
    if (!host || !path) {
      return std::unexpected{!host ? host.error() : path.error()};
    }
    auto connection =
        UniqueWinHttp{WinHttpConnect(session.get(), host->c_str(),
                                     INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection) return std::unexpected{"连接更新服务器失败。"};
    auto request = UniqueWinHttp{WinHttpOpenRequest(
        connection.get(), L"GET", path->c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH)};
    if (!request) return std::unexpected{"创建 WinHTTP 请求失败。"};
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                         &redirect_policy, sizeof(redirect_policy)) == FALSE) {
      return std::unexpected{"禁用 WinHTTP 自动重定向失败。"};
    }
    if (WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) {
      return std::unexpected{std::format(
          "更新服务器请求发送失败（WinHTTP {}，URL {}）。", GetLastError(),
          current)};
    }
    if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
      return std::unexpected{std::format(
          "更新服务器响应失败（WinHTTP {}，URL {}）。", GetLastError(),
          current)};
    }
    DWORD status = 0;
    DWORD status_bytes = sizeof(status);
    if (WinHttpQueryHeaders(
            request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_bytes,
            WINHTTP_NO_HEADER_INDEX) == FALSE) {
      return std::unexpected{"读取更新服务器状态码失败。"};
    }
    if (status == 200) {
      return OpenResponse{.session = std::move(session),
                          .connection = std::move(connection),
                          .request = std::move(request),
                          .proxy = std::move(proxy_storage),
                          .final_url = std::move(current)};
    }
    if (status != 301 && status != 302 && status != 303 && status != 307 &&
        status != 308) {
      return std::unexpected{std::format("更新服务器返回 HTTP {}。", status)};
    }
    if (redirect == maximum_redirects) {
      return std::unexpected{"更新下载重定向次数超过限制。"};
    }
    auto location = query_header_string(request.get(), WINHTTP_QUERY_LOCATION);
    if (!location) return std::unexpected{location.error()};
    current = location->starts_with('/')
                  ? std::format("https://{}{}", parsed->host, *location)
                  : std::move(*location);
    if (auto next = parse_allowed_https_url(current); !next) {
      return std::unexpected{std::format("拒绝恶意重定向：{}", next.error())};
    }
  }
  return std::unexpected{"更新请求未得到有效响应。"};
}

std::expected<std::uint64_t, std::string> content_length(HINTERNET request) {
  auto text = query_header_string(request, WINHTTP_QUERY_CONTENT_LENGTH);
  if (!text) return std::unexpected{text.error()};
  std::uint64_t value = 0;
  for (const char ch : *text) {
    if (ch < '0' || ch > '9' ||
        value > (std::numeric_limits<std::uint64_t>::max() -
                 static_cast<unsigned>(ch - '0')) /
                    10) {
      return std::unexpected{"HTTP Content-Length 非法。"};
    }
    value = value * 10 + static_cast<unsigned>(ch - '0');
  }
  return value;
}

std::expected<std::string, std::string> get_bytes(
    std::string url, std::size_t maximum, std::stop_token token) {
  auto response = open_response(std::move(url), token);
  if (!response) return std::unexpected{response.error()};
  auto length = content_length(response->request.get());
  if (!length || *length > maximum) {
    return std::unexpected{!length ? length.error()
                                   : "HTTP 响应超过允许的大小。"};
  }
  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(*length));
  std::array<char, 64 * 1024> buffer{};
  for (;;) {
    if (token.stop_requested()) return std::unexpected{"更新请求已取消。"};
    DWORD read = 0;
    if (WinHttpReadData(response->request.get(), buffer.data(),
                        static_cast<DWORD>(buffer.size()), &read) == FALSE) {
      return std::unexpected{"读取更新服务器响应失败。"};
    }
    if (read == 0) break;
    if (bytes.size() > maximum - read) {
      return std::unexpected{"HTTP 响应超过允许的大小。"};
    }
    bytes.append(buffer.data(), read);
  }
  if (bytes.size() != *length) {
    return std::unexpected{"HTTP 响应长度与 Content-Length 不一致。"};
  }
  return bytes;
}

}  // namespace runtime_detail
#else
namespace runtime_detail {

struct CurlResponse {
  std::string bytes{};
  std::string location{};
  std::optional<std::uint64_t> content_length{};
  bool overflow{};
  std::size_t maximum{};
  std::stop_token token{};
};

std::size_t curl_write(char* data, std::size_t size, std::size_t count,
                       void* opaque) {
  auto& response = *static_cast<CurlResponse*>(opaque);
  if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
    response.overflow = true;
    return 0;
  }
  const std::size_t bytes = size * count;
  if (response.token.stop_requested() ||
      bytes > response.maximum - std::min(response.bytes.size(), response.maximum)) {
    response.overflow = true;
    return 0;
  }
  response.bytes.append(data, bytes);
  return bytes;
}

std::size_t curl_header(char* data, std::size_t size, std::size_t count,
                        void* opaque) {
  auto& response = *static_cast<CurlResponse*>(opaque);
  if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
    response.overflow = true;
    return 0;
  }
  const std::size_t bytes = size * count;
  std::string_view line{data, bytes};
  const auto trim = [](std::string_view value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                              value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
    }
    return value;
  };
  if (line.size() >= 9 &&
      std::ranges::equal(line.substr(0, 9), std::string_view{"location:"},
                         [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) == b;
                         })) {
    response.location = std::string{trim(line.substr(9))};
  } else if (line.size() >= 15 &&
             std::ranges::equal(
                 line.substr(0, 15), std::string_view{"content-length:"},
                 [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == b;
                 })) {
    auto value = trim(line.substr(15));
    std::uint64_t parsed = 0;
    bool valid = !value.empty();
    for (const char ch : value) {
      if (ch < '0' || ch > '9' ||
          parsed > (std::numeric_limits<std::uint64_t>::max() -
                    static_cast<unsigned>(ch - '0')) /
                       10) {
        valid = false;
        break;
      }
      parsed = parsed * 10 + static_cast<unsigned>(ch - '0');
    }
    if (valid) response.content_length = parsed;
  }
  return bytes;
}

int curl_progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<CurlResponse*>(opaque)->token.stop_requested() ? 1 : 0;
}

std::expected<std::string, std::string> get_bytes(
    std::string initial_url, std::size_t maximum, std::stop_token token) {
  static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  if (!curl_ready) return std::unexpected{"初始化 libcurl 失败。"};
  std::string current = std::move(initial_url);
  for (int redirect = 0; redirect <= 4; ++redirect) {
    if (auto allowed = parse_allowed_https_url(current); !allowed) {
      return std::unexpected{allowed.error()};
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{
        curl_easy_init(), &curl_easy_cleanup};
    if (!curl) return std::unexpected{"创建 libcurl 请求失败。"};
    CurlResponse response{.maximum = maximum, .token = token};
    curl_easy_setopt(curl.get(), CURLOPT_URL, current.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "AWJimage updater/1");
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 20000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &curl_write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &curl_header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &curl_progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &response);
    const CURLcode code = curl_easy_perform(curl.get());
    if (code != CURLE_OK) {
      if (token.stop_requested()) return std::unexpected{"更新请求已取消。"};
      return std::unexpected{response.overflow
                                 ? "HTTP 响应超过允许的大小。"
                                 : std::format("更新服务器请求失败：{}",
                                               curl_easy_strerror(code))};
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status == 200) {
      if (response.content_length &&
          *response.content_length != response.bytes.size()) {
        return std::unexpected{"HTTP 响应长度与 Content-Length 不一致。"};
      }
      return std::move(response.bytes);
    }
    if (status != 301 && status != 302 && status != 303 && status != 307 &&
        status != 308) {
      return std::unexpected{std::format("更新服务器返回 HTTP {}。", status)};
    }
    if (redirect == 4 || response.location.empty()) {
      return std::unexpected{"更新重定向次数超过限制或缺少 Location。"};
    }
    const auto parsed = parse_allowed_https_url(current);
    current = response.location.starts_with('/')
                  ? std::format("https://{}{}", parsed->host, response.location)
                  : std::move(response.location);
  }
  return std::unexpected{"更新请求未得到有效响应。"};
}

}  // namespace runtime_detail
#endif

namespace runtime_detail {

bool endpoint_not_found(std::string_view error) noexcept {
  return error == "更新服务器返回 HTTP 404。";
}

std::expected<std::pair<std::string, std::string>, std::string>
get_document_pair(std::string_view primary_document,
                  std::string_view primary_signature,
                  std::string_view legacy_document,
                  std::string_view legacy_signature,
                  std::size_t document_limit,
                  std::size_t signature_limit,
                  std::stop_token token) {
  auto raw = get_bytes(std::string{primary_document}, document_limit, token);
  if (raw) {
    auto signature = get_bytes(std::string{primary_signature}, signature_limit, token);
    if (signature) return std::pair{std::move(*raw), std::move(*signature)};
    if (!endpoint_not_found(signature.error())) {
      return std::unexpected{signature.error()};
    }
  } else if (!endpoint_not_found(raw.error())) {
    return std::unexpected{raw.error()};
  }

  auto legacy_raw = get_bytes(std::string{legacy_document}, document_limit, token);
  if (!legacy_raw) return std::unexpected{legacy_raw.error()};
  auto legacy_sig = get_bytes(std::string{legacy_signature}, signature_limit, token);
  if (!legacy_sig) return std::unexpected{legacy_sig.error()};
  return std::pair{std::move(*legacy_raw), std::move(*legacy_sig)};
}

}  // namespace runtime_detail

std::expected<VerifiedUpdateKeyring, std::string> fetch_verified_update_keyring(
    std::uint64_t last_verified_sequence = 0, std::stop_token token = {}) {
  if (!update_keyring_roots_configured()) {
    return std::unexpected{
        "此构建未配置至少两把有效更新根公钥，已拒绝联网更新。"};
  }
  auto pair = runtime_detail::get_document_pair(
      update_keyring_url, update_keyring_signature_url,
      legacy_update_keyring_url, legacy_update_keyring_signature_url,
      maximum_manifest_bytes, 16 * 1024, token);
  if (!pair) return std::unexpected{pair.error()};
  auto& [raw, signature] = *pair;
  auto keyring = verify_and_parse_update_keyring(raw, signature);
  if (!keyring) return std::unexpected{keyring.error()};
  if (auto accepted = accept_verified_update_document(
          UpdateSecurityDocument::keyring, keyring->sequence, raw,
          last_verified_sequence);
      !accepted) {
    return std::unexpected{accepted.error()};
  }
  return VerifiedUpdateKeyring{.keyring = std::move(*keyring),
                               .raw_bytes = std::move(raw),
                               .signature_envelope = std::move(signature)};
}

std::expected<VerifiedManifest, std::string> fetch_verified_manifest(
    std::uint64_t last_verified_sequence, std::stop_token token = {}) {
  auto keyring = fetch_verified_update_keyring(0, token);
  if (!keyring) return std::unexpected{keyring.error()};
  auto raw = runtime_detail::get_bytes(std::string{manifest_url},
                                       maximum_manifest_bytes, token);
  if (!raw) return std::unexpected{raw.error()};
  auto signature = runtime_detail::get_bytes(
      std::string{manifest_signature_url}, maximum_signature_bytes, token);
  if (!signature) return std::unexpected{signature.error()};
  auto manifest = verify_and_parse_manifest_with_keyring(*raw, *signature,
                                                          keyring->keyring);
  if (!manifest) return std::unexpected{manifest.error()};
  if (!is_manifest_fresh(*manifest, last_verified_sequence)) {
    return std::unexpected{"收到的签名 manifest sequence 低于本机已验证序号，已拒绝重放。"};
  }
  if (auto accepted = accept_verified_update_document(
          UpdateSecurityDocument::legacy_manifest, manifest->sequence, *raw,
          last_verified_sequence);
      !accepted) {
    return std::unexpected{accepted.error()};
  }
  return VerifiedManifest{.manifest = std::move(*manifest),
                          .raw_bytes = std::move(*raw),
                          .signature_base64 = std::move(*signature)};
}

std::expected<VerifiedArchiveManifestV2, std::string>
fetch_verified_archive_manifest_v2(std::uint64_t last_verified_sequence,
                                   std::stop_token token = {}) {
  auto keyring = fetch_verified_update_keyring(0, token);
  if (!keyring) return std::unexpected{keyring.error()};
  auto pair = runtime_detail::get_document_pair(
      archive_manifest_v2_url, archive_manifest_v2_signature_url,
      legacy_archive_manifest_v2_url, legacy_archive_manifest_v2_signature_url,
      maximum_manifest_bytes, maximum_signature_bytes, token);
  if (!pair) return std::unexpected{pair.error()};
  auto& [raw, signature] = *pair;
  auto manifest = verify_and_parse_archive_manifest_v2(raw, signature,
                                                        keyring->keyring);
  if (!manifest) return std::unexpected{manifest.error()};
  if (!is_archive_manifest_v2_fresh(*manifest, last_verified_sequence)) {
    return std::unexpected{
        "收到的签名 v2 manifest sequence 低于本机已验证序号，已拒绝重放。"};
  }
  if (auto accepted = accept_verified_update_document(
          UpdateSecurityDocument::archive_manifest_v2, manifest->sequence,
          raw, last_verified_sequence);
      !accepted) {
    return std::unexpected{accepted.error()};
  }
  return VerifiedArchiveManifestV2{.manifest = std::move(*manifest),
                                   .raw_bytes = std::move(raw),
                                   .signature_base64 = std::move(signature),
                                   .keyring_raw_bytes = std::move(keyring->raw_bytes),
                                   .keyring_signature_envelope =
                                       std::move(keyring->signature_envelope)};
}

std::expected<void, std::string> download_https_asset(
    std::string url, const std::filesystem::path& destination,
    std::uint64_t expected_size, std::stop_token token = {}) {
#ifdef _WIN32
  if (expected_size == 0 || expected_size > maximum_asset_bytes) {
    return std::unexpected{"下载资产声明大小超出限制。"};
  }
  auto response = runtime_detail::open_response(std::move(url), token);
  if (!response) return std::unexpected{response.error()};
  auto length = runtime_detail::content_length(response->request.get());
  if (!length || *length != expected_size) {
    return std::unexpected{!length ? length.error()
                                   : "下载资产的 Content-Length 与 manifest 不一致。"};
  }
  const HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::unexpected{"无法创建更新 staging 文件。"};
  }
  struct FileCloser {
    HANDLE handle{};
    ~FileCloser() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  } closer{file};
  std::uint64_t total = 0;
  std::array<std::byte, 64 * 1024> buffer{};
  for (;;) {
    if (token.stop_requested()) return std::unexpected{"更新下载已取消。"};
    DWORD read = 0;
    if (WinHttpReadData(response->request.get(), buffer.data(),
                        static_cast<DWORD>(buffer.size()), &read) == FALSE) {
      return std::unexpected{"读取更新资产失败。"};
    }
    if (read == 0) break;
    if (total > expected_size - read) {
      return std::unexpected{"更新资产实际大小超过 manifest 声明。"};
    }
    DWORD written = 0;
    if (WriteFile(file, buffer.data(), read, &written, nullptr) == FALSE ||
        written != read) {
      return std::unexpected{"写入更新 staging 文件失败。"};
    }
    total += read;
  }
  if (total != expected_size || FlushFileBuffers(file) == FALSE) {
    return std::unexpected{"更新资产大小不一致或刷新 staging 文件失败。"};
  }
  return {};
#else
  if (expected_size == 0 || expected_size > maximum_asset_bytes) {
    return std::unexpected{"下载资产声明大小超出限制。"};
  }
  auto bytes = runtime_detail::get_bytes(std::move(url), expected_size, token);
  if (!bytes) return std::unexpected{bytes.error()};
  if (bytes->size() != expected_size) {
    return std::unexpected{"下载资产实际大小与 manifest 不一致。"};
  }
  const int fd = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        0600);
  if (fd < 0) return std::unexpected{"无法创建更新 staging 文件。"};
  std::size_t offset = 0;
  while (offset < bytes->size()) {
    const auto written = ::write(fd, bytes->data() + offset,
                                 bytes->size() - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return std::unexpected{"写入更新 staging 文件失败。"};
    }
    if (written == 0) {
      ::close(fd);
      return std::unexpected{"写入更新 staging 文件失败。"};
    }
    offset += static_cast<std::size_t>(written);
  }
  const bool flushed = ::fsync(fd) == 0;
  ::close(fd);
  if (!flushed) return std::unexpected{"刷新更新 staging 文件失败。"};
  return {};
#endif
}

}  // namespace awj::update
