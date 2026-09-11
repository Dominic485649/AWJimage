module;

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <codecvt>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <locale>

#ifndef AWJ_BUILD_VERSION
#error "AWJ_BUILD_VERSION must be provided by CMake"
#endif

export module awj.core;

import awj.config;
import awj.encoding_defaults;
import awj.visual_quality;

#ifdef _WIN32
using BCRYPT_ALG_HANDLE = void*;
using BCRYPT_HASH_HANDLE = void*;
using NTSTATUS = LONG;

constexpr auto kBcryptSha256Algorithm = L"SHA256";
constexpr auto kBcryptObjectLength = L"ObjectLength";
constexpr auto kBcryptHashLength = L"HashDigestLength";

constexpr bool bcrypt_success(NTSTATUS status) noexcept { return status >= 0; }

extern "C" __declspec(dllimport) NTSTATUS __stdcall BCryptOpenAlgorithmProvider(
    BCRYPT_ALG_HANDLE* phAlgorithm, LPCWSTR pszAlgId, LPCWSTR pszImplementation,
    ULONG dwFlags);
extern "C" __declspec(dllimport) NTSTATUS __stdcall
BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags);
extern "C" __declspec(dllimport) NTSTATUS __stdcall BCryptGetProperty(
    BCRYPT_ALG_HANDLE hObject, LPCWSTR pszProperty, PUCHAR pbOutput,
    ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags);
extern "C" __declspec(dllimport) NTSTATUS __stdcall BCryptCreateHash(
    BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_HASH_HANDLE* phHash,
    PUCHAR pbHashObject, ULONG cbHashObject, PUCHAR pbSecret, ULONG cbSecret,
    ULONG dwFlags);
extern "C" __declspec(dllimport) NTSTATUS __stdcall BCryptHashData(
    BCRYPT_HASH_HANDLE hHash, PUCHAR pbInput, ULONG cbInput, ULONG dwFlags);
extern "C" __declspec(dllimport) NTSTATUS __stdcall BCryptFinishHash(
    BCRYPT_HASH_HANDLE hHash, PUCHAR pbOutput, ULONG cbOutput, ULONG dwFlags);
extern "C" __declspec(dllimport) NTSTATUS __stdcall BCryptDestroyHash(
    BCRYPT_HASH_HANDLE hHash);

#endif

export namespace awj {

namespace fs = std::filesystem;

inline constexpr std::string_view kAwjVersion = AWJ_BUILD_VERSION;

struct ImageFile {
  std::size_t index{};
  fs::path path{};
  fs::path relative_dir{};
  std::wstring source_extension_disambiguator{};
  std::uintmax_t bytes{};
  std::wstring date_token{};
  std::wstring time_token{};
  std::wstring datetime_token{};
  std::wstring unix_token{};
  std::wstring random_token{};
  std::wstring hash_token{};
  std::wstring sha256_token{};
  bool extension_disambiguated{};
  fs::path resolved_output_path{};
  bool output_path_resolved{};
};

struct EncodeResult {
  std::size_t index{};
  fs::path input_path{};
  fs::path output_path{};
  std::string output_format{};
  std::uintmax_t original_bytes{};
  std::uintmax_t output_bytes{};
  int quality{};
  std::optional<int> requested_visual_quality{};
  double visual_score{};
  double raw_gmsd{};
  double raw_ms_ssim{};
  double gmsd_quality_score{};
  double msssim_quality_score{};
  double gmsd_weight{};
  double msssim_weight{};
  int final_encoder_quality{};
  bool visual_quality_target_met{true};
  int search_attempt_count{};
  int speed{};
  std::string decoder_id{};
  std::string encoder_id{};
  std::string requested_encoder_id{};
  std::string user_encoder_id{};
  std::string user_chroma{};
  std::string source_chroma{};
  std::string requested_chroma{};
  std::string applied_chroma{};
  std::string chroma_reason{};
  std::string requested_color_representation{};
  std::string applied_color_representation{};
  std::optional<int> source_bit_depth{};
  std::optional<int> requested_bit_depth{};
  std::optional<int> applied_bit_depth{};
  std::string bit_depth_reason{};
  std::string alpha_policy{};
  bool source_has_alpha_channel{};
  std::string source_alpha_mode{};
  std::optional<bool> has_non_opaque_alpha{};
  bool encoder_supports_alpha{};
  std::string applied_alpha{};
  std::string alpha_reason{};
  std::optional<int> source_color_primaries{};
  std::optional<int> source_transfer_characteristics{};
  std::optional<int> source_matrix_coefficients{};
  std::optional<int> source_color_range{};
  std::optional<int> applied_color_primaries{};
  std::optional<int> applied_transfer_characteristics{};
  std::optional<int> applied_matrix_coefficients{};
  std::optional<int> applied_color_range{};
  bool source_has_icc{};
  std::string applied_icc{};
  bool source_has_hdr_metadata{};
  std::string applied_hdr_metadata{};
  std::string color_metadata_source{};
  std::string color_reason{};
  std::string fallback_reason{};
  bool used_decoder_fallback{};
  std::string encoder_license{};
  std::string integration_mode{};
  std::string speed_parameter_kind{};
  int applied_speed{};
  int encoder_threads{};
  std::uint64_t memory_budget_bytes{};
  double seconds{};
  double decode_seconds{-1.0};
  double prepare_seconds{-1.0};
  double encode_seconds{-1.0};
  double avif_rgb_to_yuv_seconds{-1.0};
  double avif_add_image_seconds{-1.0};
  double avif_finish_seconds{-1.0};
  double avif_output_copy_seconds{-1.0};
  double write_seconds{-1.0};
  double visual_quality_search_seconds{-1.0};
  double visual_quality_candidate_encode_seconds{-1.0};
  double visual_quality_candidate_decode_seconds{-1.0};
  double visual_quality_candidate_io_seconds{-1.0};
  double visual_quality_luma_seconds{-1.0};
  double gmsd_seconds{-1.0};
  double ms_ssim_seconds{-1.0};
  double visual_quality_metric_seconds{-1.0};
  int visual_quality_candidate_count{};
  int visual_quality_decode_memory_fallback_count{};
  int visual_quality_gpu_fallback_count{};
  bool visual_quality_gpu_requested{};
  bool visual_quality_gpu_used{};
  std::string visual_quality_gpu_path{};
  std::string visual_quality_gpu_fallback_reason{};
  std::string visual_quality_search_trace{};
  bool quality_overridden_by_visual_quality{false};
  bool lossless{false};
  bool processed{false};
  bool ok{false};
  bool skipped{false};
  bool large_image_queued{false};
  bool canceled{false};
  std::string message{};
  std::string command{};
};

std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

void set_process_low_priority() noexcept {
#ifdef _WIN32
  // 保留给需要整进程后台运行的调用方。UI 启动阶段不调用它，
  // 避免高负载时窗口初始化被普通优先级任务饿住。
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
  ::setpriority(PRIO_PROCESS, 0, 10);
#endif
}

namespace core_detail {

#ifdef _WIN32
struct LocalFreeDeleter {
  void operator()(void* value) const noexcept {
    if (value != nullptr) {
      LocalFree(value);
    }
  }
};
#endif

std::string narrow_ascii(std::wstring_view text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t ch : text) {
    out.push_back(ch <= 0x7f ? static_cast<char>(ch) : '?');
  }
  return out;
}

bool has_path_separator(std::wstring_view text) {
  return text.find(L'\\') != std::wstring_view::npos ||
         text.find(L'/') != std::wstring_view::npos;
}

std::string trim_copy(std::string text) {
  const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  const auto first = std::ranges::find_if(text, not_space);
  const auto last =
      std::ranges::find_if(text | std::views::reverse, not_space).base();
  if (first >= last) {
    return {};
  }
  return std::string{first, last};
}

bool csv_formula_trigger(char ch) noexcept {
  return ch == '=' || ch == '+' || ch == '-' || ch == '@';
}

bool csv_needs_formula_prefix(std::string_view value) noexcept {
  for (const unsigned char ch : value) {
    if (std::isspace(ch)) {
      continue;
    }
    return csv_formula_trigger(static_cast<char>(ch));
  }
  return false;
}

std::string csv_escape(std::string value) {
  // summary.csv 可能被 Excel 打开；用户文件名不能被解释成公式执行。
  if (csv_needs_formula_prefix(value)) {
    value.insert(value.begin(), '\'');
  }
  if (value.find_first_of(",\"\r\n") == std::string::npos) {
    return value;
  }
  std::string out{"\""};
  for (const char ch : value) {
    if (ch == '"') {
      out += "\"\"";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
  return out;
}

void replace_all(std::wstring& text, std::wstring_view token,
                 std::wstring_view value) {
  std::size_t pos = 0;
  while ((pos = text.find(token, pos)) != std::wstring::npos) {
    text.replace(pos, token.size(), value);
    pos += value.size();
  }
}

void replace_all(std::string& text, std::string_view token,
                 std::string_view value) {
  if (token.empty()) {
    return;
  }
  std::size_t pos = 0;
  while ((pos = text.find(token, pos)) != std::string::npos) {
    text.replace(pos, token.size(), value);
    pos += value.size();
  }
}

bool contains_token(std::wstring_view text, std::wstring_view token) {
  return text.find(token) != std::wstring_view::npos;
}

bool is_windows_reserved_device_digit(wchar_t ch) noexcept {
  return (ch >= L'1' && ch <= L'9') || ch == L'¹' || ch == L'²' || ch == L'³';
}

bool is_windows_reserved_device_name(std::wstring_view value) noexcept {
  return value == L"CON" || value == L"PRN" || value == L"AUX" ||
         value == L"NUL" ||
         (value.size() == 4 &&
          (value.starts_with(L"COM") || value.starts_with(L"LPT")) &&
          is_windows_reserved_device_digit(value[3]));
}

std::wstring sanitize_output_stem(std::wstring value, std::size_t index) {
  // 模板变量来自文件名和用户输入，必须清理 Windows 禁用字符和保留设备名。
  for (auto& ch : value) {
    const bool invalid = ch < L' ' || ch == L'<' || ch == L'>' || ch == L':' ||
                         ch == L'"' || ch == L'/' || ch == L'\\' ||
                         ch == L'|' || ch == L'?' || ch == L'*';
    if (invalid) {
      ch = L'_';
    }
  }

  while (!value.empty() && (value.back() == L'.' || value.back() == L' ')) {
    value.pop_back();
  }
  if (value.empty()) {
    value = std::format(L"image-{:04}", index + 1);
  }

  auto reserved = value;
  const auto dot = reserved.find(L'.');
  if (dot != std::wstring::npos) {
    reserved.resize(dot);
  }
  std::ranges::transform(reserved, reserved.begin(),
                         [](wchar_t ch) { return std::towupper(ch); });
  if (is_windows_reserved_device_name(reserved)) {
    if (dot == std::wstring::npos) {
      value.push_back(L'_');
    } else {
      value.insert(dot, 1, L'_');
    }
  }
  return value;
}

}  // namespace core_detail

std::string utf8_from_wide(std::wstring_view text) {
  const auto ascii_fallback = [](std::wstring_view value) {
    try {
      return core_detail::narrow_ascii(value);
    } catch (const std::bad_alloc&) {
      return std::string{"?"};
    } catch (const std::length_error&) {
      return std::string{"?"};
    }
  };

  if (text.empty()) {
    return {};
  }
#ifdef _WIN32
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return ascii_fallback(text);
  }
  const int input_size = static_cast<int>(text.size());
  const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(), input_size,
                                           nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return ascii_fallback(text);
  }
  try {
    std::string out(static_cast<std::size_t>(required), '\0');
    const int written =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), input_size, out.data(),
                            required, nullptr, nullptr);
    if (written != required) {
      return ascii_fallback(text);
    }
    return out;
  } catch (const std::bad_alloc&) {
    return "?";
  } catch (const std::length_error&) {
    return "?";
  }
#else
  try {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> convert;
    return convert.to_bytes(text.data(), text.data() + text.size());
  } catch (...) {
    return ascii_fallback(text);
  }
#endif
}

std::wstring wide_from_utf8(std::string_view text) {
  const auto byte_fallback = [](std::string_view value) {
    std::wstring fallback;
    fallback.reserve(value.size());
    for (const char ch : value) {
      fallback.push_back(static_cast<unsigned char>(ch));
    }
    return fallback;
  };

  if (text.empty()) {
    return {};
  }
#ifdef _WIN32
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    try {
      return byte_fallback(text);
    } catch (const std::bad_alloc&) {
      return L"?";
    } catch (const std::length_error&) {
      return L"?";
    }
  }
  const int input_size = static_cast<int>(text.size());
  const int required =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), input_size, nullptr, 0);
  if (required <= 0) {
    try {
      return byte_fallback(text);
    } catch (const std::bad_alloc&) {
      return L"?";
    } catch (const std::length_error&) {
      return L"?";
    }
  }
  try {
    std::wstring out(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, text.data(), input_size,
                                            out.data(), required);
    if (written != required) {
      return byte_fallback(text);
    }
    return out;
  } catch (const std::bad_alloc&) {
    return L"?";
  } catch (const std::length_error&) {
    return L"?";
  }
#else
  try {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> convert;
    return convert.from_bytes(text.data(), text.data() + text.size());
  } catch (...) {
    try {
      return byte_fallback(text);
    } catch (const std::bad_alloc&) {
      return L"?";
    } catch (const std::length_error&) {
      return L"?";
    }
  }
#endif
}

std::string path_to_utf8(const fs::path& path) {
#ifdef _WIN32
  return utf8_from_wide(path.native());
#else
  return path.string();
#endif
}

std::string display_path_for_user(const fs::path& path) {
  const auto normalized = path.lexically_normal();
  auto filename = normalized.filename();
  if (filename.empty()) {
    filename = path.filename();
  }
  if (!filename.empty()) {
    return path_to_utf8(filename);
  }
  const auto root = path.root_name();
  if (!root.empty()) {
    return path_to_utf8(root);
  }
  return path_to_utf8(path);
}

fs::path long_existing_path_or_self(const fs::path& path) {
#ifdef _WIN32
  try {
    const auto native = path.wstring();
    if (native.empty()) {
      return path;
    }
    const DWORD needed = GetLongPathNameW(native.c_str(), nullptr, 0);
    if (needed == 0) {
      return path;
    }
    std::wstring buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    const DWORD length = GetLongPathNameW(
        native.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
      return path;
    }
    buffer.resize(length);
    return fs::path{std::move(buffer)};
  } catch (...) {
    return path;
  }
#else
  return path;
#endif
}

std::string redact_path_for_user(std::string message, const fs::path& path) {
  if (path.empty()) {
    return message;
  }
  const auto display = display_path_for_user(path);
  const auto replace_path = [&](const fs::path& candidate) {
    const auto full = path_to_utf8(candidate);
    if (!full.empty() && full != display) {
      core_detail::replace_all(message, full, display);
    }
  };
  replace_path(path);
  std::error_code ec;
  const auto absolute = fs::absolute(path, ec);
  if (!ec) {
    replace_path(absolute);
  }
  return message;
}

std::wstring normalized_lower_path_key(const fs::path& path) {
  auto key = path.lexically_normal().wstring();
#ifdef _WIN32
  std::ranges::transform(key, key.begin(),
                         [](wchar_t ch) { return std::towlower(ch); });
#endif
  return key;
}

#ifdef _WIN32
std::string win32_error_message(DWORD error) {
  wchar_t* raw_buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length =
      FormatMessageW(flags, nullptr, error, 0,
                     reinterpret_cast<wchar_t*>(&raw_buffer), 0, nullptr);
  std::unique_ptr<wchar_t, core_detail::LocalFreeDeleter> buffer{raw_buffer};
  if (length == 0 || buffer == nullptr) {
    return std::format("Win32 error {}", error);
  }
  // FormatMessageW 用 LocalAlloc 返回系统缓冲区，交给 unique_ptr
  // 确保所有早退路径都调用 LocalFree。
  std::wstring message{buffer.get(), buffer.get() + length};
  return core_detail::trim_copy(utf8_from_wide(message));
}
#endif

std::string posix_error_message(int error) {
  return std::generic_category().message(error);
}

std::expected<fs::path, std::string> executable_path() {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return std::unexpected{std::format("获取程序路径失败: {}",
                                         win32_error_message(GetLastError()))};
    }
    if (length < buffer.size()) {
      buffer.resize(length);
      return fs::path{buffer};
    }
    if (buffer.size() >
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max() / 2)) {
      return std::unexpected{"程序路径超过 Win32 API 长度限制。"};
    }
    buffer.assign(buffer.size() * 2, L'\0');
  }
#else
  std::string buffer(4096, '\0');
  while (true) {
    const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0) {
      return std::unexpected{std::format("获取程序路径失败: {}",
                                         posix_error_message(errno))};
    }
    if (static_cast<std::size_t>(length) < buffer.size()) {
      buffer.resize(static_cast<std::size_t>(length));
      return fs::path{buffer};
    }
    if (buffer.size() > 1024u * 1024u) {
      return std::unexpected{"程序路径超过运行时长度限制。"};
    }
    buffer.assign(buffer.size() * 2, '\0');
  }
#endif
}

std::expected<fs::path, std::string> executable_directory() {
  auto path = executable_path();
  if (!path) {
    return std::unexpected{path.error()};
  }
  return path->parent_path();
}

namespace core_detail {

class LogFileLock {
 public:
  LogFileLock() = default;
  LogFileLock(const LogFileLock&) = delete;
  LogFileLock& operator=(const LogFileLock&) = delete;

  ~LogFileLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
#else
    if (fd_ >= 0) {
      ::close(fd_);
    }
#endif
  }

  bool open(const fs::path& path) noexcept {
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE |
                              FILE_SHARE_DELETE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    return handle_ != INVALID_HANDLE_VALUE;
#else
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    return fd_ >= 0;
#endif
  }

  bool lock() noexcept {
#ifdef _WIN32
    OVERLAPPED overlapped{};
    return handle_ != INVALID_HANDLE_VALUE &&
           LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0,
                      &overlapped) != FALSE;
#else
    if (fd_ < 0) {
      return false;
    }
    while (::flock(fd_, LOCK_EX) != 0) {
      if (errno != EINTR) {
        return false;
      }
    }
    return true;
#endif
  }

  void unlock() noexcept {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      UnlockFileEx(handle_, 0, 1, 0, &overlapped);
    }
#else
    if (fd_ >= 0) {
      while (::flock(fd_, LOCK_UN) != 0 && errno == EINTR) {
      }
    }
#endif
  }

 private:
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int fd_{-1};
#endif
};

class ScopedLogFileLock {
 public:
  explicit ScopedLogFileLock(LogFileLock& lock) noexcept
      : lock_{lock}, locked_{lock_.lock()} {}
  ScopedLogFileLock(const ScopedLogFileLock&) = delete;
  ScopedLogFileLock& operator=(const ScopedLogFileLock&) = delete;
  ~ScopedLogFileLock() {
    if (locked_) {
      lock_.unlock();
    }
  }
  explicit operator bool() const noexcept { return locked_; }

 private:
  LogFileLock& lock_;
  bool locked_{};
};

bool valid_utf8(std::string_view text) noexcept {
  if (text.empty()) {
    return true;
  }
#ifdef _WIN32
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), nullptr, 0) > 0;
#else
  try {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> convert;
    (void)convert.from_bytes(text.data(), text.data() + text.size());
    return true;
  } catch (...) {
    return false;
  }
#endif
}

bool invalid_log_control(unsigned char ch) noexcept {
  return (ch < 0x20 && ch != '\t') || ch == 0x7f;
}

bool valid_log_line(std::string_view line) noexcept {
  constexpr std::array<std::size_t, 14> digit_positions{
      1, 2, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19};
  if (line.size() < 29 || !valid_utf8(line) || line[0] != '[' ||
      line[5] != '-' || line[8] != '-' || line[11] != ' ' ||
      line[14] != ':' || line[17] != ':' || line[20] != ']' ||
      std::ranges::any_of(line, invalid_log_control) ||
      !std::ranges::all_of(digit_positions, [&](std::size_t position) {
        return std::isdigit(static_cast<unsigned char>(line[position]));
      })) {
    return false;
  }
  const auto two_digits = [&](std::size_t position) {
    return (line[position] - '0') * 10 + line[position + 1] - '0';
  };
  const int year = (line[1] - '0') * 1000 + (line[2] - '0') * 100 +
                   (line[3] - '0') * 10 + line[4] - '0';
  const int month = two_digits(6);
  const int day = two_digits(9);
  const int hour = two_digits(12);
  const int minute = two_digits(15);
  const int second = two_digits(18);
  const std::chrono::year_month_day date{
      std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
      std::chrono::day{static_cast<unsigned>(day)}};
  if (year < 1 || !date.ok() || hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  const auto suffix = line.substr(21);
  return suffix.starts_with(" [INFO] ") || suffix.starts_with(" [WARN] ") ||
         suffix.starts_with(" [ERROR] ");
}

bool valid_existing_log(const fs::path& path) noexcept try {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return false;
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  input.seekg(-1, std::ios::end);
  char last{};
  if (!input.get(last) || last != '\n') {
    return false;
  }
  input.clear();
  input.seekg(0);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!valid_log_line(line)) {
      return false;
    }
  }
  return input.eof();
} catch (...) {
  return false;
}

}  // namespace core_detail

class FileLogger {
 public:
  explicit FileLogger(fs::path output_dir, bool enabled = true)
      : enabled_{enabled} {
    if (!enabled_) {
      return;
    }
    try {
      log_dir_ = std::move(output_dir) / L"log";
      log_file_ = log_dir_ / L"awj.log";
      lock_file_ = log_dir_ / L".awj.log.lock";
      std::error_code ec;
      fs::create_directories(log_dir_, ec);
      if (ec) {
        disable_with_error("无法创建日志目录。", [&] {
          return std::format("无法创建日志目录 {}: {}",
                             display_path_for_user(log_dir_), ec.message());
        });
        return;
      }
      if (!interprocess_lock_.open(lock_file_)) {
        disable_with_error("无法创建日志锁文件。", [&] {
          return std::format("无法创建日志锁文件 {}",
                             display_path_for_user(lock_file_));
        });
        return;
      }
      core_detail::ScopedLogFileLock file_lock{interprocess_lock_};
      if (!file_lock) {
        disable_with_error("无法锁定日志文件。", [&] {
          return std::format("无法锁定日志文件 {}",
                             display_path_for_user(log_file_));
        });
        return;
      }
      const bool log_exists = fs::exists(log_file_, ec);
      if (ec) {
        disable_with_error("无法检查日志文件。", [&] {
          return std::format("无法检查日志文件 {}: {}",
                             display_path_for_user(log_file_), ec.message());
        });
        return;
      }
      if (log_exists && !core_detail::valid_existing_log(log_file_)) {
        std::ofstream reset{log_file_, std::ios::binary | std::ios::trunc};
        if (!reset) {
          disable_with_error("无法清空无效日志文件。", [&] {
            return std::format("无法清空无效日志文件 {}",
                               display_path_for_user(log_file_));
          });
          return;
        }
      }
      stream_.open(log_file_, std::ios::binary | std::ios::app);
      if (!stream_) {
        disable_with_error("无法写入日志文件。", [&] {
          return std::format("无法写入日志文件 {}",
                             display_path_for_user(log_file_));
        });
        return;
      }
      const auto now = std::chrono::floor<std::chrono::seconds>(
          std::chrono::system_clock::now());
      stream_ << std::format("[{:%F %T}] [INFO] ===== NEW SESSION START =====\n",
                             now);
      stream_.flush();
      if (!stream_) {
        disable_with_error("无法写入日志文件。", [&] {
          return std::format("无法写入日志文件 {}",
                             display_path_for_user(log_file_));
        });
      }
    } catch (...) {
      disable_with_error("初始化日志失败。",
                         [] { return std::string{"初始化日志失败。"}; });
    }
  }

  void info(std::string_view message) noexcept { append("INFO", message); }
  void warn(std::string_view message) noexcept { append("WARN", message); }
  void error(std::string_view message) noexcept { append("ERROR", message); }

  [[nodiscard]] bool enabled() const {
    std::scoped_lock lock{mutex_};
    return enabled_;
  }

  [[nodiscard]] std::string last_error() const {
    std::scoped_lock lock{mutex_};
    return last_error_;
  }

 private:
  template <class Function>
  void disable_with_error(std::string_view fallback,
                          Function message) noexcept {
    enabled_ = false;
    try {
      last_error_ = message();
    } catch (...) {
      last_error_.clear();
      try {
        last_error_ = std::string{fallback};
      } catch (...) {
        last_error_.clear();
      }
    }
  }

  void append(std::string_view level, std::string_view message) noexcept {
    try {
      std::scoped_lock lock{mutex_};
      if (!enabled_) {
        return;
      }
      core_detail::ScopedLogFileLock file_lock{interprocess_lock_};
      if (!file_lock) {
        disable_with_error("无法锁定日志文件。", [&] {
          return std::format("无法锁定日志文件 {}",
                             display_path_for_user(log_file_));
        });
        return;
      }
      std::string normalized_message;
      if (!core_detail::valid_utf8(message)) {
        message = "[invalid UTF-8 log message]";
      } else if (std::ranges::any_of(message,
                                     core_detail::invalid_log_control)) {
        normalized_message.assign(message);
        std::ranges::replace_if(normalized_message,
                                core_detail::invalid_log_control, ' ');
        message = normalized_message;
      }
      const auto now = std::chrono::floor<std::chrono::seconds>(
          std::chrono::system_clock::now());
      stream_ << std::format("[{:%F %T}] [{}] {}\n", now, level, message);
      stream_.flush();
      if (!stream_) {
        disable_with_error("无法写入日志文件。", [&] {
          return std::format("无法写入日志文件 {}",
                             display_path_for_user(log_file_));
        });
      }
    } catch (...) {
      try {
        std::scoped_lock lock{mutex_};
        disable_with_error("日志写入失败。",
                           [] { return std::string{"日志写入失败。"}; });
      } catch (...) {
      }
    }
  }

  bool enabled_{true};
  fs::path log_dir_;
  fs::path log_file_;
  fs::path lock_file_;
  std::string last_error_{};
  std::ofstream stream_{};
  core_detail::LogFileLock interprocess_lock_{};
  mutable std::mutex mutex_;
};

constexpr std::string_view kSupportedImageExtensionsText =
    "jpg/jpeg/jpe/jfif/png/webp/bmp/dib/rle/ico/tif/tiff/gif/jxl/avif/awsraw/dng/"
    "cr2/cr3/nef/arw/rw2/orf/raf/pef/srw/x3f/3fr/erf/kdc/mrw/raw/heic/heif/jxr/"
    "wdp/hdp";

bool is_supported_image_extension(const fs::path& path) {
  auto ext = path.extension().wstring();
  std::ranges::transform(ext, ext.begin(),
                         [](wchar_t ch) { return std::towlower(ch); });
  return ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" ||
         ext == L".jfif" || ext == L".png" || ext == L".webp" ||
         ext == L".bmp" || ext == L".dib" || ext == L".rle" || ext == L".ico" || ext == L".tif" ||
         ext == L".tiff" || ext == L".gif" || ext == L".jxl" ||
         ext == L".avif" || ext == L".awsraw" || ext == L".dng" ||
         ext == L".cr2" || ext == L".cr3" || ext == L".nef" || ext == L".arw" ||
         ext == L".rw2" || ext == L".orf" || ext == L".raf" || ext == L".pef" ||
         ext == L".srw" || ext == L".x3f" || ext == L".3fr" || ext == L".erf" ||
         ext == L".kdc" || ext == L".mrw" || ext == L".raw" ||
         ext == L".heic" || ext == L".heif" || ext == L".jxr" ||
         ext == L".wdp" || ext == L".hdp";
}

fs::path default_output_dir_for(const fs::path& input_path) {
  std::error_code ec;
  if (fs::is_regular_file(input_path, ec) && !ec) {
    const auto parent = input_path.parent_path();
    if (!parent.empty()) {
      return parent;
    }
    std::error_code current_ec;
    auto current = fs::current_path(current_ec);
    return current_ec ? input_path : current;
  }
  return input_path;
}

fs::path output_dir_for(const AppConfig& cfg) {
  if (!cfg.output_dir.empty()) {
    return cfg.output_dir;
  }
  if (cfg.output_policy == OutputPolicy::shell) {
    std::error_code ec;
    if (fs::is_directory(cfg.input_path, ec) && !ec) {
      const auto parent = cfg.input_path.parent_path();
      return (parent.empty() ? cfg.input_path : parent) / L"AWJOutput";
    }
  }
  return default_output_dir_for(cfg.input_path);
}

std::wstring output_extension_for(OutputFormat format) {
  switch (format) {
    case OutputFormat::png:
      return L".png";
    case OutputFormat::avif:
      return L".avif";
    case OutputFormat::webp:
      return L".webp";
    case OutputFormat::jxl:
      return L".jxl";
    case OutputFormat::jpgli:
      return L".jpg";
  }
  return L".avif";
}

std::wstring output_extension_for(const AppConfig& cfg) {
  auto extension = output_extension_for(cfg.output_format);
  if (cfg.output_format == OutputFormat::avif && cfg.append_png_suffix) {
    extension += L".png";
  }
  return extension;
}

std::string output_format_name(OutputFormat format) {
  switch (format) {
    case OutputFormat::png:
      return "PNG";
    case OutputFormat::avif:
      return "AVIF";
    case OutputFormat::webp:
      return "WEBP";
    case OutputFormat::jxl:
      return "JXL";
    case OutputFormat::jpgli:
      return "JPGLI";
  }
  return "AVIF";
}

ImageFile make_image_file(std::size_t index, const fs::path& path,
                          fs::path relative_dir, std::uintmax_t bytes,
                          std::mt19937_64& rng, std::wstring hash_token = {},
                          std::wstring sha256_token = {}) {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
  const auto unix_seconds = seconds.time_since_epoch().count();
  const auto random_value = rng();
  return ImageFile{
      .index = index,
      .path = path,
      .relative_dir = std::move(relative_dir),
      .bytes = bytes,
      .date_token = std::format(L"{:%Y%m%d}", seconds),
      .time_token = std::format(L"{:%H%M%S}", seconds),
      .datetime_token = std::format(L"{:%Y%m%d-%H%M%S}", seconds),
      .unix_token = std::format(L"{}", unix_seconds),
      .random_token = std::format(
          L"{:08x}", static_cast<unsigned int>(random_value & 0xffffffffu)),
      .hash_token = std::move(hash_token),
      .sha256_token = std::move(sha256_token)};
}

std::expected<void, std::string> check_scanned_input_file_size(
    const fs::path& path, std::uintmax_t bytes) {
  if (bytes >
      static_cast<std::uintmax_t>(encoding_defaults::effective_max_input_file_bytes())) {
    return std::unexpected{std::format("输入文件超过当前输入上限: {}。",
                                       display_path_for_user(path))};
  }
  return {};
}

std::expected<void, std::string> file_hash_token(const fs::path& path,
                                                 std::wstring& out) {
  try {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;

    std::error_code ec;
    const auto file_size = fs::file_size(path, ec);
    if (ec) {
      return std::unexpected{std::format(
          "读取用于 {{hash}}/{{hash8}} 的文件大小失败: {}；系统错误：{}。",
          display_path_for_user(path), ec.message())};
    }
    if (file_size > encoding_defaults::effective_max_input_file_bytes()) {
      return std::unexpected{std::format(
          "用于 {{hash}}/{{hash8}} 的输入文件超过当前输入上限: {}。",
          display_path_for_user(path))};
    }
    std::uint64_t hashed_bytes = 0;

    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
      return std::unexpected{std::format(
          "无法读取用于 {{hash}}/{{hash8}} 的文件内容: "
          "{}。请检查文件是否仍存在、是否被占用，或当前用户是否有读取权限。",
          display_path_for_user(path))};
    }

    std::array<char, 64 * 1024> buffer{};
    while (stream) {
      stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto read = stream.gcount();
      const auto chunk_bytes = static_cast<std::uint64_t>(read);
      if (hashed_bytes >
          encoding_defaults::effective_max_input_file_bytes() - chunk_bytes) {
        return std::unexpected{std::format(
            "用于 {{hash}}/{{hash8}} 的输入文件超过当前输入上限: {}。",
            display_path_for_user(path))};
      }
      hashed_bytes += chunk_bytes;
      for (std::streamsize i = 0; i < read; ++i) {
        hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
        hash *= prime;
      }
    }
    if (!stream.eof()) {
      return std::unexpected{std::format(
          "读取文件哈希时发生 I/O 错误: {}。请检查磁盘、权限或杀毒软件拦截。",
          display_path_for_user(path))};
    }
    out = std::format(L"{:016x}", hash);
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"读取文件哈希时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"读取文件哈希时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"读取文件哈希时文件系统访问失败。"};
  }
}


#ifndef _WIN32
namespace core_detail {

// ponytail: POSIX has no native SHA-256 API; this tiny CPU implementation keeps {sha256} working without a new dependency.
struct Sha256State {
  std::array<std::uint32_t, 8> h{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                 0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                 0x1f83d9abu, 0x5be0cd19u};
  std::array<unsigned char, 64> block{};
  std::uint64_t bit_count{};
  std::size_t used{};
};

constexpr std::array<std::uint32_t, 64> sha256_k{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t value, int bits) noexcept {
  return (value >> bits) | (value << (32 - bits));
}

void sha256_transform(Sha256State& state, const unsigned char* data) noexcept {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i) {
    const auto j = i * 4;
    w[i] = (static_cast<std::uint32_t>(data[j]) << 24) |
           (static_cast<std::uint32_t>(data[j + 1]) << 16) |
           (static_cast<std::uint32_t>(data[j + 2]) << 8) |
           static_cast<std::uint32_t>(data[j + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  auto a = state.h[0];
  auto b = state.h[1];
  auto c = state.h[2];
  auto d = state.h[3];
  auto e = state.h[4];
  auto f = state.h[5];
  auto g = state.h[6];
  auto h = state.h[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const auto ch = (e & f) ^ ((~e) & g);
    const auto temp1 = h + s1 + ch + sha256_k[i] + w[i];
    const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const auto maj = (a & b) ^ (a & c) ^ (b & c);
    const auto temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state.h[0] += a;
  state.h[1] += b;
  state.h[2] += c;
  state.h[3] += d;
  state.h[4] += e;
  state.h[5] += f;
  state.h[6] += g;
  state.h[7] += h;
}

void sha256_update(Sha256State& state, const unsigned char* data, std::size_t size) noexcept {
  state.bit_count += static_cast<std::uint64_t>(size) * 8ull;
  for (std::size_t i = 0; i < size; ++i) {
    state.block[state.used++] = data[i];
    if (state.used == state.block.size()) {
      sha256_transform(state, state.block.data());
      state.used = 0;
    }
  }
}

std::array<unsigned char, 32> sha256_final(Sha256State& state) noexcept {
  const auto bit_count = state.bit_count;
  state.block[state.used++] = 0x80u;
  if (state.used > 56) {
    while (state.used < state.block.size()) {
      state.block[state.used++] = 0;
    }
    sha256_transform(state, state.block.data());
    state.used = 0;
  }
  while (state.used < 56) {
    state.block[state.used++] = 0;
  }
  for (int i = 7; i >= 0; --i) {
    state.block[state.used++] = static_cast<unsigned char>((bit_count >> (i * 8)) & 0xffu);
  }
  sha256_transform(state, state.block.data());

  std::array<unsigned char, 32> digest{};
  for (std::size_t i = 0; i < state.h.size(); ++i) {
    digest[i * 4] = static_cast<unsigned char>((state.h[i] >> 24) & 0xffu);
    digest[i * 4 + 1] = static_cast<unsigned char>((state.h[i] >> 16) & 0xffu);
    digest[i * 4 + 2] = static_cast<unsigned char>((state.h[i] >> 8) & 0xffu);
    digest[i * 4 + 3] = static_cast<unsigned char>(state.h[i] & 0xffu);
  }
  return digest;
}

}  // namespace core_detail
#endif

std::expected<std::string, std::string> file_sha256_hex(const fs::path& path) {
  try {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
      return std::unexpected{
          std::format("无法计算 SHA-256，路径不是文件或不可访问: {}。",
                      display_path_for_user(path))};
    }
    const auto file_size = fs::file_size(path, ec);
    if (ec) {
      return std::unexpected{
          std::format("读取 SHA-256 文件大小失败: {}；系统错误：{}。",
                      display_path_for_user(path), ec.message())};
    }
    if (file_size > encoding_defaults::effective_max_input_file_bytes()) {
      return std::unexpected{
          std::format("用于 SHA-256 的文件超过当前输入上限: {}。",
                      display_path_for_user(path))};
    }

#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm{};
    if (!bcrypt_success(BCryptOpenAlgorithmProvider(
            &algorithm, kBcryptSha256Algorithm, nullptr, 0))) {
      return std::unexpected{"初始化 SHA-256 算法失败。"};
    }
    struct AlgorithmCloser {
      BCRYPT_ALG_HANDLE handle{};
      ~AlgorithmCloser() {
        if (handle != nullptr) {
          BCryptCloseAlgorithmProvider(handle, 0);
        }
      }
    } algorithm_closer{algorithm};

    DWORD object_length{};
    DWORD property_size{};
    if (!bcrypt_success(
            BCryptGetProperty(algorithm, kBcryptObjectLength,
                              reinterpret_cast<PUCHAR>(&object_length),
                              sizeof(object_length), &property_size, 0))) {
      return std::unexpected{"读取 SHA-256 对象长度失败。"};
    }
    DWORD hash_length{};
    if (!bcrypt_success(
            BCryptGetProperty(algorithm, kBcryptHashLength,
                              reinterpret_cast<PUCHAR>(&hash_length),
                              sizeof(hash_length), &property_size, 0))) {
      return std::unexpected{"读取 SHA-256 输出长度失败。"};
    }

    std::vector<unsigned char> object(object_length);
    BCRYPT_HASH_HANDLE hash{};
    if (!bcrypt_success(BCryptCreateHash(algorithm, &hash, object.data(),
                                         object_length, nullptr, 0, 0))) {
      return std::unexpected{"创建 SHA-256 状态失败。"};
    }
    struct HashCloser {
      BCRYPT_HASH_HANDLE handle{};
      ~HashCloser() {
        if (handle != nullptr) {
          BCryptDestroyHash(handle);
        }
      }
    } hash_closer{hash};

    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
      return std::unexpected{
          std::format("无法读取用于 SHA-256 的文件内容: {}。",
                      display_path_for_user(path))};
    }
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t hashed_bytes = 0;
    while (stream) {
      stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto read = stream.gcount();
      const auto chunk_bytes = static_cast<std::uint64_t>(read);
      if (hashed_bytes >
          encoding_defaults::effective_max_input_file_bytes() - chunk_bytes) {
        return std::unexpected{
            std::format("用于 SHA-256 的文件超过当前输入上限: {}。",
                        display_path_for_user(path))};
      }
      hashed_bytes += chunk_bytes;
      if (read > 0 && !bcrypt_success(BCryptHashData(
                          hash, reinterpret_cast<PUCHAR>(buffer.data()),
                          static_cast<ULONG>(read), 0))) {
        return std::unexpected{"更新 SHA-256 状态失败。"};
      }
    }
    if (!stream.eof()) {
      return std::unexpected{
          std::format("读取 SHA-256 文件时发生 I/O 错误: {}。",
                      display_path_for_user(path))};
    }

    std::vector<unsigned char> digest(hash_length);
    if (!bcrypt_success(
            BCryptFinishHash(hash, digest.data(), hash_length, 0))) {
      return std::unexpected{"完成 SHA-256 计算失败。"};
    }
#else
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
      return std::unexpected{
          std::format("无法读取用于 SHA-256 的文件内容: {}。",
                      display_path_for_user(path))};
    }
    core_detail::Sha256State state{};
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t hashed_bytes = 0;
    while (stream) {
      stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto read = stream.gcount();
      const auto chunk_bytes = static_cast<std::uint64_t>(read);
      if (hashed_bytes > encoding_defaults::effective_max_input_file_bytes() - chunk_bytes) {
        return std::unexpected{
            std::format("用于 SHA-256 的文件超过当前输入上限: {}。",
                        display_path_for_user(path))};
      }
      hashed_bytes += chunk_bytes;
      if (read > 0) {
        core_detail::sha256_update(
            state, reinterpret_cast<const unsigned char*>(buffer.data()),
            static_cast<std::size_t>(read));
      }
    }
    if (!stream.eof()) {
      return std::unexpected{
          std::format("读取 SHA-256 文件时发生 I/O 错误: {}。",
                      display_path_for_user(path))};
    }
    const auto digest = core_detail::sha256_final(state);
#endif
    std::string out;
    out.reserve(digest.size() * 2);
    for (const auto byte : digest) {
      out += std::format("{:02x}", byte);
    }
    return out;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"计算 SHA-256 时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"计算 SHA-256 时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"计算 SHA-256 时文件系统访问失败。"};
  }
}

std::expected<void, std::string> file_sha256_token(const fs::path& path,
                                                   std::wstring& out) {
  out.clear();
  auto digest = file_sha256_hex(path);
  if (!digest) {
    return std::unexpected{digest.error()};
  }
  out.assign(digest->begin(), digest->end());
  return {};
}

fs::path relative_output_dir(const fs::path& input_root,
                             const fs::path& image_path) {
  // 文件夹输入时记录图片相对输入根目录的位置，输出时据此重建原目录结构。
  std::error_code ec;
  const auto root = fs::absolute(input_root, ec);
  if (ec) {
    return {};
  }
  const auto parent = fs::absolute(image_path.parent_path(), ec);
  if (ec) {
    return {};
  }
  auto relative = fs::relative(parent, root, ec);
  if (ec || relative.empty() || relative == L".") {
    return {};
  }
  return relative;
}

std::wstring output_name_for(const AppConfig& cfg, const ImageFile& image);
fs::path output_path_for(const AppConfig& cfg, const ImageFile& image);

std::wstring source_extension_disambiguator(const fs::path& path) {
  auto ext = path.extension().wstring();
  if (ext.empty()) {
    return L".source";
  }
  if (ext.front() != L'.') {
    ext.insert(ext.begin(), L'.');
  }
  return ext;
}

fs::path planned_output_path_for(const AppConfig& cfg, const ImageFile& image) {
  auto output_dir = output_dir_for(cfg);
  if (!image.relative_dir.empty()) {
    output_dir /= image.relative_dir;
  }
  return output_dir / output_name_for(cfg, image);
}

std::expected<void, std::string> apply_source_extension_disambiguation(
    const AppConfig& cfg, std::vector<ImageFile>& files) {
  // 例如 1.jpg 和 1.bmp 都套用 {name}<输出扩展名>
  // 时会同名；保留源扩展避免互相覆盖。
  std::unordered_map<std::wstring, std::vector<std::size_t>> by_output;
  try {
    by_output.reserve(files.size());
    for (const auto i : std::views::iota(std::size_t{}, files.size())) {
      by_output[normalized_lower_path_key(
                    planned_output_path_for(cfg, files[i]))]
          .push_back(i);
    }

    for (const auto& [_, indices] : by_output) {
      if (indices.size() < 2) {
        continue;
      }

      std::unordered_map<std::wstring, int> source_extensions;
      source_extensions.reserve(indices.size());
      for (const auto index : indices) {
        auto ext = files[index].path.extension().wstring();
        std::ranges::transform(ext, ext.begin(),
                               [](wchar_t ch) { return std::towlower(ch); });
        source_extensions.try_emplace(std::move(ext), 0);
      }
      if (source_extensions.size() < 2) {
        continue;
      }

      for (const auto index : indices) {
        files[index].source_extension_disambiguator =
            source_extension_disambiguator(files[index].path);
        files[index].extension_disambiguated = true;
      }
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected{"输出路径冲突检测内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"输出路径冲突检测数量超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"输出路径冲突检测文件系统访问失败。"};
  }
  return {};
}

std::optional<std::wstring> scan_output_directory_key(
    const AppConfig& cfg, const fs::path& input_path) noexcept {
  try {
    std::error_code input_ec;
    std::error_code output_ec;
    const auto input = fs::absolute(input_path, input_ec);
    const auto output = fs::absolute(output_dir_for(cfg), output_ec);
    if (input_ec || output_ec) {
      return std::nullopt;
    }
    const auto input_key = normalized_lower_path_key(input);
    const auto output_key = normalized_lower_path_key(output);
    if (output_key.empty() || output_key == input_key) {
      return std::nullopt;
    }
    return output_key;
  } catch (...) {
    return std::nullopt;
  }
}

std::atomic_uint64_t output_collision_counter{};

struct NumberedCollisionStem {
  std::wstring base;
  std::uint64_t next{1};
};

NumberedCollisionStem numbered_collision_stem(std::wstring stem) {
  if (stem.ends_with(L')')) {
    const auto open = stem.find_last_of(L'(');
    if (open != std::wstring::npos && open + 1 < stem.size() - 1) {
      std::uint64_t value = 0;
      bool digits = true;
      for (std::size_t i = open + 1; i + 1 < stem.size(); ++i) {
        const wchar_t ch = stem[i];
        if (ch < L'0' || ch > L'9') {
          digits = false;
          break;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - L'0');
        if (value > 999'999'999ULL) {
          digits = false;
          break;
        }
      }
      if (digits) {
        stem.resize(open);
        return NumberedCollisionStem{.base = std::move(stem), .next = value + 1};
      }
    }
  }
  return NumberedCollisionStem{.base = std::move(stem), .next = 1};
}

std::wstring collision_suffix(CollisionMode mode) {
  const auto now = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
  switch (mode) {
    case CollisionMode::suffix_time:
      return std::format(L"-{:%Y%m%d-%H%M%S}", now);
    case CollisionMode::suffix_random: {
      const auto value =
          output_collision_counter.fetch_add(1, std::memory_order_relaxed);
      const auto tick = static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count());
      return std::format(
          L"-{:08x}",
          static_cast<unsigned int>((value ^ tick) & 0xffffffffu));
    }
    case CollisionMode::suffix_number:
    case CollisionMode::overwrite:
    case CollisionMode::skip:
    default:
      return {};
  }
}

std::expected<fs::path, std::string> resolve_collision_output_path(
    const fs::path& planned, CollisionMode mode,
    std::unordered_map<std::wstring, int>* reserved_outputs = nullptr,
    std::wstring_view complete_extension = {}) {
  try {
    const auto reserve_available =
        [&](const fs::path& path) -> std::expected<bool, std::string> {
      if (reserved_outputs == nullptr) {
        return true;
      }
      auto key = normalized_lower_path_key(path);
      if (key.empty()) {
        return false;
      }
      const auto [_, inserted] =
          reserved_outputs->try_emplace(std::move(key), 0);
      return inserted;
    };

    if (mode != CollisionMode::suffix_time &&
        mode != CollisionMode::suffix_random &&
        mode != CollisionMode::suffix_number) {
      if (auto reserved = reserve_available(planned); !reserved) {
        return std::unexpected{reserved.error()};
      }
      return planned;
    }

    std::error_code ec;
    const auto path_available =
        [&](const fs::path& path) -> std::expected<bool, std::string> {
      const auto exists = fs::exists(path, ec);
      if (ec) {
        return std::unexpected{std::format("无法检查输出路径 {}: {}",
                                           display_path_for_user(path),
                                           ec.message())};
      }
      if (exists) {
        return false;
      }
      if (auto reserved = reserve_available(path); !reserved) {
        return std::unexpected{reserved.error()};
      } else {
        return *reserved;
      }
    };

    if (auto available = path_available(planned); !available) {
      return std::unexpected{available.error()};
    } else if (*available) {
      return planned;
    }

    const auto parent = planned.parent_path();
    auto stem = planned.stem().wstring();
    auto extension = planned.extension().wstring();
    if (!complete_extension.empty()) {
      const auto filename = planned.filename().wstring();
      if (filename.size() > complete_extension.size() &&
          filename.ends_with(complete_extension)) {
        stem = filename.substr(0, filename.size() - complete_extension.size());
        extension = std::wstring{complete_extension};
      }
    }
    const auto planned_key = normalized_lower_path_key(planned);

    if (mode == CollisionMode::suffix_number) {
      auto numbered = numbered_collision_stem(stem);
      for (const auto offset : std::views::iota(0ULL, 10000ULL)) {
        const auto number = numbered.next + offset;
        auto candidate = parent /
                         (numbered.base + std::format(L"({})", number) +
                          extension);
        if (normalized_lower_path_key(candidate) == planned_key) {
          continue;
        }
        if (auto available = path_available(candidate); !available) {
          return std::unexpected{available.error()};
        } else if (*available) {
          return candidate;
        }
      }
      const auto fallback =
          parent / (numbered.base +
                    std::format(L"({}-{})", numbered.next, current_process_id()) +
                    extension);
      if (auto available = path_available(fallback); !available) {
        return std::unexpected{available.error()};
      } else if (*available) {
        return fallback;
      }
      return std::unexpected{std::format("无法找到可用输出路径: {}",
                                         display_path_for_user(planned))};
    }

    const auto suffix = collision_suffix(mode);
    for (const auto attempt : std::views::iota(0, 1000)) {
      auto candidate = parent / (stem + suffix +
                                 (attempt == 0 ? std::wstring{}
                                               : std::format(L"-{}", attempt)) +
                                 extension);
      if (normalized_lower_path_key(candidate) == planned_key) {
        continue;
      }
      if (auto available = path_available(candidate); !available) {
        return std::unexpected{available.error()};
      } else if (*available) {
        return candidate;
      }
    }

    const auto fallback =
        parent / (stem + suffix + std::format(L"-{}", current_process_id()) +
                  extension);
    if (auto available = path_available(fallback); !available) {
      return std::unexpected{available.error()};
    } else if (*available) {
      return fallback;
    }
    return std::unexpected{std::format("无法找到可用输出路径: {}",
                                       display_path_for_user(planned))};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"输出路径冲突检测内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"输出路径冲突检测数量超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"输出路径冲突检测文件系统访问失败。"};
  }
}

std::expected<void, std::string> resolve_batch_output_paths(
    const AppConfig& cfg, std::vector<ImageFile>& files) {
  if (cfg.collision_mode != CollisionMode::suffix_time &&
      cfg.collision_mode != CollisionMode::suffix_random &&
      cfg.collision_mode != CollisionMode::suffix_number) {
    return {};
  }

  std::unordered_map<std::wstring, int> reserved_outputs;
  try {
    reserved_outputs.reserve(files.size());
    for (auto& image : files) {
      auto output = resolve_collision_output_path(
          output_path_for(cfg, image), cfg.collision_mode, &reserved_outputs,
          output_extension_for(cfg));
      if (!output) {
        return std::unexpected{output.error()};
      }
      image.resolved_output_path = std::move(*output);
      image.output_path_resolved = true;
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected{"输出路径冲突检测内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"输出路径冲突检测数量超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"输出路径冲突检测文件系统访问失败。"};
  }
  return {};
}

std::expected<void, std::string> scan_images(const AppConfig& cfg,
                                             std::vector<ImageFile>& files) {
  // 输入可以是单张图片或一个目录。这里不再抛异常，而是把失败原因、路径和建议
  // 作为 std::expected 的 error 返回给 CLI/UI，避免用户只看到“未知异常”。
  std::error_code ec;
  const auto input_path = long_existing_path_or_self(cfg.input_path);
  const bool exists = fs::exists(input_path, ec);
  if (ec) {
    return std::unexpected{std::format(
        "检查输入路径失败: "
        "{}；系统错误：{}。请确认路径可访问，或尝试用管理员权限/本地磁盘路径。",
        display_path_for_user(input_path), ec.message())};
  }
  if (!exists) {
    return std::unexpected{std::format(
        "输入路径不存在: "
        "{}。请检查路径是否写错、盘符是否挂载，或文件是否已被移动。",
        display_path_for_user(input_path))};
  }

  std::random_device random_device;
  std::mt19937_64 rng{random_device()};

  files.clear();
  const bool template_needs_hash =
      core_detail::contains_token(cfg.output_template, L"{hash}") ||
      core_detail::contains_token(cfg.output_template, L"{hash8}");
  const bool template_needs_sha256 =
      core_detail::contains_token(cfg.output_template, L"{sha256}") ||
      core_detail::contains_token(cfg.output_template, L"{sha2568}") ||
      core_detail::contains_token(cfg.output_template, L"{sha256_8}");
  const auto build_hash =
      [&](const fs::path& path,
          std::wstring& out) -> std::expected<void, std::string> {
    out.clear();
    if (!template_needs_hash) {
      return {};
    }
    return file_hash_token(path, out);
  };
  const auto build_sha256 =
      [&](const fs::path& path,
          std::wstring& out) -> std::expected<void, std::string> {
    out.clear();
    if (!template_needs_sha256) {
      return {};
    }
    return file_sha256_token(path, out);
  };

  if (fs::is_regular_file(input_path, ec) && !ec) {
    if (!is_supported_image_extension(input_path)) {
      return std::unexpected{std::format("输入文件格式不受支持: {}。支持 {}。",
                                         display_path_for_user(input_path),
                                         kSupportedImageExtensionsText)};
    }
    auto bytes = fs::file_size(input_path, ec);
    if (ec) {
      return std::unexpected{
          std::format("读取输入文件大小失败: {}；系统错误：{}。",
                      display_path_for_user(input_path), ec.message())};
    }
    if (auto within_limit = check_scanned_input_file_size(input_path, bytes);
        !within_limit) {
      return std::unexpected{within_limit.error()};
    }
    std::wstring hash;
    if (auto ok = build_hash(input_path, hash); !ok) {
      return std::unexpected{ok.error()};
    }
    std::wstring sha256;
    if (auto ok = build_sha256(input_path, sha256); !ok) {
      return std::unexpected{ok.error()};
    }
    try {
      files.push_back(make_image_file(0, input_path, {}, bytes, rng,
                                      std::move(hash), std::move(sha256)));
    } catch (const std::bad_alloc&) {
      return std::unexpected{"文件列表内存不足，无法记录输入图片。"};
    } catch (const std::length_error&) {
      return std::unexpected{"文件列表数量超过运行时限制。"};
    }
    if (auto disambiguated = apply_source_extension_disambiguation(cfg, files);
        !disambiguated) {
      return std::unexpected{disambiguated.error()};
    }
    if (auto resolved = resolve_batch_output_paths(cfg, files); !resolved) {
      return std::unexpected{resolved.error()};
    }
    return {};
  }

  if (ec) {
    return std::unexpected{
        std::format("判断输入路径类型失败: {}；系统错误：{}。",
                    display_path_for_user(input_path), ec.message())};
  }
  const auto input_is_directory = fs::is_directory(input_path, ec);
  if (ec) {
    return std::unexpected{
        std::format("判断输入路径类型失败: {}；系统错误：{}。",
                    display_path_for_user(input_path), ec.message())};
  }
  if (!input_is_directory) {
    return std::unexpected{std::format(
        "输入路径不是文件或文件夹: {}。请确认输入模式和实际路径一致。",
        display_path_for_user(input_path))};
  }

  std::size_t skipped_access = 0;
  const auto skipped_output_dir_key =
      scan_output_directory_key(cfg, input_path);
  fs::recursive_directory_iterator it{
      input_path, fs::directory_options::skip_permission_denied, ec};
  if (ec) {
    return std::unexpected{std::format("扫描输入目录失败: {}；系统错误：{}。",
                                       display_path_for_user(input_path),
                                       ec.message())};
  }
  for (fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
    if (ec) {
      ++skipped_access;
      ec.clear();
      continue;
    }
    if (skipped_output_dir_key) {
      const auto entry_is_directory = it->is_directory(ec);
      if (ec) {
        ++skipped_access;
        ec.clear();
        continue;
      }
      if (entry_is_directory) {
        std::error_code path_ec;
        const auto entry = fs::absolute(it->path(), path_ec);
        if (!path_ec &&
            normalized_lower_path_key(entry) == *skipped_output_dir_key) {
          it.disable_recursion_pending();
          continue;
        }
      }
    }
    if (!it->is_regular_file(ec) || ec) {
      if (ec) {
        ++skipped_access;
      }
      ec.clear();
      continue;
    }
    const auto image_path = long_existing_path_or_self(it->path());
    if (!is_supported_image_extension(image_path)) {
      continue;
    }

    auto bytes = fs::file_size(image_path, ec);
    if (ec) {
      ++skipped_access;
      ec.clear();
      continue;
    }
    if (auto within_limit = check_scanned_input_file_size(image_path, bytes);
        !within_limit) {
      return std::unexpected{within_limit.error()};
    }
    std::wstring hash;
    if (auto ok = build_hash(image_path, hash); !ok) {
      return std::unexpected{ok.error()};
    }
    std::wstring sha256;
    if (auto ok = build_sha256(image_path, sha256); !ok) {
      return std::unexpected{ok.error()};
    }
    try {
      files.push_back(make_image_file(
          files.size(), image_path, relative_output_dir(input_path, image_path),
          bytes, rng, std::move(hash), std::move(sha256)));
    } catch (const std::bad_alloc&) {
      return std::unexpected{"文件列表内存不足，无法继续扫描输入目录。"};
    } catch (const std::length_error&) {
      return std::unexpected{"文件列表数量超过运行时限制。"};
    }
    ec.clear();
  }

  if (files.empty() && skipped_access > 0) {
    return std::unexpected{std::format(
        "未找到可转换图片，并且扫描时跳过了 {} "
        "个无权限或不可访问的条目。请检查目录权限或复制到本地目录后重试。",
        skipped_access)};
  }

  std::ranges::sort(files, [](const ImageFile& left, const ImageFile& right) {
    return normalized_lower_path_key(left.path) <
           normalized_lower_path_key(right.path);
  });
  for (const auto i : std::views::iota(std::size_t{}, files.size())) {
    files[i].index = i;
  }
  if (auto disambiguated = apply_source_extension_disambiguation(cfg, files);
      !disambiguated) {
    return std::unexpected{disambiguated.error()};
  }
  if (auto resolved = resolve_batch_output_paths(cfg, files); !resolved) {
    return std::unexpected{resolved.error()};
  }
  return {};
}


std::expected<void, std::string> scan_images(const AppConfig& cfg,
                                             std::span<const fs::path> input_paths,
                                             std::vector<ImageFile>& files) {
  if (input_paths.empty()) {
    return scan_images(cfg, files);
  }
  if (input_paths.size() == 1) {
    auto one = cfg;
    one.input_path = input_paths.front();
    return scan_images(one, files);
  }

  files.clear();
  std::unordered_set<std::wstring> seen;
  try {
    seen.reserve(input_paths.size());
    for (const auto& raw_input : input_paths) {
      auto one_cfg = cfg;
      one_cfg.input_path = raw_input;
      std::vector<ImageFile> batch;
      if (auto scanned = scan_images(one_cfg, batch); !scanned) {
        return std::unexpected{scanned.error()};
      }
      const auto input_path = long_existing_path_or_self(raw_input);
      std::error_code type_ec;
      const bool input_is_file = fs::is_regular_file(input_path, type_ec) && !type_ec;
      for (auto& image : batch) {
        const auto key = normalized_lower_path_key(image.path);
        if (!key.empty() && !seen.insert(key).second) {
          continue;
        }
        image.index = files.size();
        image.source_extension_disambiguator.clear();
        image.extension_disambiguated = false;
        image.resolved_output_path.clear();
        image.output_path_resolved = false;
        if (cfg.output_policy == OutputPolicy::shell && cfg.output_dir.empty() &&
            input_is_file) {
          image.relative_dir = fs::absolute(image.path.parent_path(), type_ec);
          if (type_ec) {
            image.relative_dir = image.path.parent_path();
            type_ec.clear();
          }
        }
        files.push_back(std::move(image));
      }
    }
    std::ranges::sort(files, [](const ImageFile& left, const ImageFile& right) {
      return normalized_lower_path_key(left.path) <
             normalized_lower_path_key(right.path);
    });
    for (const auto i : std::views::iota(std::size_t{}, files.size())) {
      files[i].index = i;
    }
    if (auto disambiguated = apply_source_extension_disambiguation(cfg, files);
        !disambiguated) {
      return std::unexpected{disambiguated.error()};
    }
    if (auto resolved = resolve_batch_output_paths(cfg, files); !resolved) {
      return std::unexpected{resolved.error()};
    }
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"文件列表内存不足，无法记录多选图片。"};
  } catch (const std::length_error&) {
    return std::unexpected{"文件列表数量超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"扫描多选输入时文件系统访问失败。"};
  }
}

std::wstring encode_params_token_for(const AppConfig& cfg) {
  std::wstring token = cfg.visual_quality
                           ? std::format(L"vq{}", *cfg.visual_quality)
                           : std::format(L"q{}", cfg.quality);
  if (cfg.visual_quality && *cfg.visual_quality == 100) {
    token += L"_lossless";
  }
  if (cfg.speed) {
    token += std::format(L"t{}", *cfg.speed);
  }
  if (cfg.chroma_mode != ChromaMode::auto_keep) {
    switch (cfg.chroma_mode) {
      case ChromaMode::yuv444:
        token += L"_444";
        break;
      case ChromaMode::yuv422:
        token += L"_422";
        break;
      case ChromaMode::yuv420:
        token += L"_420";
        break;
      case ChromaMode::auto_keep:
        break;
    }
  }
  if (cfg.bit_depth) {
    token += std::format(L"_{}", *cfg.bit_depth);
  }
  if (cfg.output_format == OutputFormat::jpgli) {
    token += std::format(L"_p{}", cfg.jpegli_progressive_level);
    token += cfg.jpegli_optimize_huffman ? L"_hopt" : L"_hfix";
    if (cfg.jpegli_xyb) {
      token += L"_xyb";
    }
  }
  return token;
}

std::wstring output_name_for(const AppConfig& cfg, const ImageFile& image) {
  auto stem = image.path.stem().wstring();
  auto ext = image.path.extension().wstring();
  if (!ext.empty() && ext.front() == L'.') {
    ext.erase(ext.begin());
  }

  const auto index_token = std::format(L"{:04}", image.index + 1);
  const auto params_token = encode_params_token_for(cfg);
  const auto hash_token = image.hash_token.empty()
                              ? std::wstring{L"hash-unavailable"}
                              : image.hash_token;
  const auto hash8_token = hash_token.substr(0, 8);
  const auto sha256_token = image.sha256_token.empty()
                                ? std::wstring{L"sha256-unavailable"}
                                : image.sha256_token;
  const auto sha2568_token = sha256_token.substr(0, 8);
  const std::wstring_view template_text{cfg.output_template.data(),
                                        cfg.output_template.size()};
  std::wstring name;
  name.reserve(template_text.size());

  for (std::size_t pos = 0; pos < template_text.size();) {
    const auto rest = template_text.substr(pos);
    const auto append = [&](std::wstring_view token,
                            const std::wstring& value) {
      if (!rest.starts_with(token)) {
        return false;
      }
      name += value;
      pos += token.size();
      return true;
    };

    if (append(L"{index}", index_token) || append(L"{name}", stem) ||
        append(L"{ext}", ext) || append(L"{date}", image.date_token) ||
        append(L"{time}", image.time_token) ||
        append(L"{datetime}", image.datetime_token) ||
        append(L"{unix}", image.unix_token) ||
        append(L"{rand}", image.random_token) ||
        append(L"{params}", params_token) || append(L"{hash}", hash_token) ||
        append(L"{hash8}", hash8_token) || append(L"{sha256}", sha256_token) ||
        append(L"{sha2568}", sha2568_token) ||
        append(L"{sha256_8}", sha2568_token)) {
      continue;
    }

    name.push_back(template_text[pos]);
    ++pos;
  }

  name = core_detail::sanitize_output_stem(std::move(name), image.index);
  name += image.source_extension_disambiguator;
  name += output_extension_for(cfg);
  return name;
}

fs::path output_path_for(const AppConfig& cfg, const ImageFile& image) {
  if (image.output_path_resolved) {
    return image.resolved_output_path;
  }

  auto output = planned_output_path_for(cfg, image);
  std::error_code output_ec;
  std::error_code image_ec;
  const auto output_key =
      normalized_lower_path_key(fs::absolute(output, output_ec));
  const auto image_key =
      normalized_lower_path_key(fs::absolute(image.path, image_ec));
  const bool same_path_text =
      !output_ec && !image_ec && output_key == image_key;
  std::error_code equivalent_ec;
  const bool same_existing_file =
      fs::equivalent(output, image.path, equivalent_ec);
  if (!same_path_text && (!same_existing_file || equivalent_ec)) {
    return output;
  }
  if (cfg.collision_mode == CollisionMode::suffix_number) {
    return output;
  }

  auto extension = output.extension().wstring();
  auto stem = output.stem().wstring();
  const auto complete_extension = output_extension_for(cfg);
  const auto filename = output.filename().wstring();
  if (filename.size() > complete_extension.size() &&
      filename.ends_with(complete_extension)) {
    stem = filename.substr(0, filename.size() - complete_extension.size());
    extension = complete_extension;
  }
  output.replace_filename(stem + L"-converted" + extension);
  return output;
}

std::string format_size(std::uintmax_t bytes) {
  constexpr double kib = 1024.0;
  constexpr double mib = kib * 1024.0;
  if (bytes >= static_cast<std::uintmax_t>(mib)) {
    return std::format("{:.2f} MiB", static_cast<double>(bytes) / mib);
  }
  if (bytes >= static_cast<std::uintmax_t>(kib)) {
    return std::format("{:.1f} KiB", static_cast<double>(bytes) / kib);
  }
  return std::format("{} B", bytes);
}

std::string display_path_for_report(const fs::path& path) {
  return display_path_for_user(path);
}

std::atomic_uint64_t summary_csv_temp_counter{};

class SummaryTempFile {
 public:
  explicit SummaryTempFile(const fs::path& path) noexcept : path_{&path} {}
  ~SummaryTempFile() { cleanup(); }
  SummaryTempFile(const SummaryTempFile&) = delete;
  SummaryTempFile& operator=(const SummaryTempFile&) = delete;

  void release() noexcept { active_ = false; }

 private:
  void cleanup() noexcept {
    if (!active_) {
      return;
    }
    try {
      std::error_code ec;
      fs::remove(*path_, ec);
    } catch (...) {
    }
  }

  const fs::path* path_{};
  bool active_{true};
};

class UniqueFileHandle {
 public:
#ifdef _WIN32
  using native_handle = HANDLE;
  static constexpr native_handle invalid() noexcept { return INVALID_HANDLE_VALUE; }
#else
  using native_handle = int;
  static constexpr native_handle invalid() noexcept { return -1; }
#endif

  UniqueFileHandle() noexcept = default;
  explicit UniqueFileHandle(native_handle handle) noexcept : handle_{handle} {}
  ~UniqueFileHandle() { reset(); }
  UniqueFileHandle(const UniqueFileHandle&) = delete;
  UniqueFileHandle& operator=(const UniqueFileHandle&) = delete;
  UniqueFileHandle(UniqueFileHandle&& other) noexcept : handle_{other.release()} {}
  UniqueFileHandle& operator=(UniqueFileHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] native_handle get() const noexcept { return handle_; }
  explicit operator bool() const noexcept {
#ifdef _WIN32
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
#else
    return handle_ >= 0;
#endif
  }
  native_handle release() noexcept {
    const auto old = handle_;
    handle_ = invalid();
    return old;
  }
  void reset(native_handle handle = invalid()) noexcept {
#ifdef _WIN32
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
#else
    if (handle_ >= 0) {
      ::close(handle_);
    }
#endif
    handle_ = handle;
  }

 private:
  native_handle handle_{invalid()};
};

namespace core_detail {

std::expected<void, std::string> clear_transient_file_attributes(
    const fs::path& path, std::string_view label) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    return std::unexpected{std::format("无法读取{}属性 {}: {}", label,
                                       display_path_for_user(path),
                                       win32_error_message(error))};
  }
  constexpr DWORD transient_attributes =
      FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
  const DWORD final_attributes = attributes & ~transient_attributes;
  const DWORD normalized_attributes =
      final_attributes == 0 ? FILE_ATTRIBUTE_NORMAL : final_attributes;
  if (normalized_attributes == attributes) {
    return {};
  }
  if (!SetFileAttributesW(path.c_str(), normalized_attributes)) {
    const auto error = GetLastError();
    return std::unexpected{std::format("无法更新{}属性 {}: {}", label,
                                       display_path_for_user(path),
                                       win32_error_message(error))};
  }
#else
  (void)path;
  (void)label;
#endif
  return {};
}
}  // namespace core_detail

std::expected<UniqueFileHandle, std::string> create_summary_csv_temp_file(
    const fs::path& output_dir, fs::path& path) {
  for (std::uint64_t attempt = 0; attempt < 1000; ++attempt) {
    const auto id = summary_csv_temp_counter.fetch_add(1, std::memory_order_relaxed);
    path = output_dir / std::format(L"summary.csv.tmp-{}-{}", current_process_id(), id);
#ifdef _WIN32
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      return UniqueFileHandle{file};
    }
    const auto error = GetLastError();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
      return std::unexpected{std::format("无法创建报告临时文件 {}: {}",
                                         display_path_for_user(path),
                                         win32_error_message(error))};
    }
#else
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (file >= 0) {
      return UniqueFileHandle{file};
    }
    const int error = errno;
    if (error != EEXIST) {
      return std::unexpected{std::format("无法创建报告临时文件 {}: {}",
                                         display_path_for_user(path),
                                         posix_error_message(error))};
    }
#endif
  }
  return std::unexpected{std::format("无法创建唯一报告临时文件: {}",
                                     display_path_for_user(output_dir))};
}

class SummaryCsvWriter {
 public:
  SummaryCsvWriter(UniqueFileHandle file, fs::path report_path)
      : file_{std::move(file)}, report_path_{std::move(report_path)} {}

  template <std::size_t N>
  SummaryCsvWriter& operator<<(const char (&value)[N]) {
    write(std::string_view{value, N - 1});
    return *this;
  }

  SummaryCsvWriter& operator<<(const char* value) {
    if (value == nullptr) {
      error_ = std::format("写入报告文件失败: {}",
                           display_path_for_user(report_path_));
      return *this;
    }
    write(std::string_view{value});
    return *this;
  }

  SummaryCsvWriter& operator<<(std::string_view value) {
    write(value);
    return *this;
  }

  SummaryCsvWriter& operator<<(const std::string& value) {
    write(value);
    return *this;
  }

  SummaryCsvWriter& operator<<(char value) {
    write(std::string_view{&value, 1});
    return *this;
  }

  template <typename T>
    requires std::is_arithmetic_v<T>
  SummaryCsvWriter& operator<<(T value) {
    return *this << std::format("{}", value);
  }

  explicit operator bool() const noexcept { return error_.empty(); }

  const std::string& error() const noexcept { return error_; }

  std::expected<void, std::string> close() {
    if (!error_.empty()) {
      return std::unexpected{error_};
    }
#ifdef _WIN32
    if (!FlushFileBuffers(file_.get())) {
      return std::unexpected{std::format("刷新报告文件失败: {}；系统错误：{}。",
                                         display_path_for_user(report_path_),
                                         win32_error_message(GetLastError()))};
    }
    const HANDLE file_handle = file_.get();
    if (!CloseHandle(file_handle)) {
      return std::unexpected{std::format("关闭报告文件失败: {}；系统错误：{}。",
                                         display_path_for_user(report_path_),
                                         win32_error_message(GetLastError()))};
    }
#else
    const int file_handle = file_.get();
    if (::fsync(file_handle) != 0) {
      return std::unexpected{std::format("刷新报告文件失败: {}；系统错误：{}。",
                                         display_path_for_user(report_path_),
                                         posix_error_message(errno))};
    }
    if (::close(file_handle) != 0) {
      return std::unexpected{std::format("关闭报告文件失败: {}；系统错误：{}。",
                                         display_path_for_user(report_path_),
                                         posix_error_message(errno))};
    }
#endif
    file_.release();
    return {};
  }

 private:
  void write(std::string_view value) {
    if (!error_.empty() || value.empty()) {
      return;
    }
    auto remaining = value.size();
    const auto* cursor = value.data();
    while (remaining > 0) {
#ifdef _WIN32
      const auto chunk = static_cast<DWORD>(
          std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
      DWORD written = 0;
      if (!WriteFile(file_.get(), cursor, chunk, &written, nullptr)) {
        error_ = std::format("写入报告文件失败: {}；系统错误：{}。",
                             display_path_for_user(report_path_),
                             win32_error_message(GetLastError()));
        return;
      }
      if (written == 0) {
        error_ = std::format("写入报告文件失败: {}",
                             display_path_for_user(report_path_));
        return;
      }
#else
      const auto written = ::write(file_.get(), cursor, remaining);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        error_ = std::format("写入报告文件失败: {}；系统错误：{}。",
                             display_path_for_user(report_path_),
                             posix_error_message(errno));
        return;
      }
      if (written == 0) {
        error_ = std::format("写入报告文件失败: {}",
                             display_path_for_user(report_path_));
        return;
      }
#endif
      cursor += written;
      remaining -= written;
    }
  }

  UniqueFileHandle file_;
  fs::path report_path_;
  std::string error_;
};

std::expected<void, std::string> write_csv(
    const fs::path& output_dir, std::span<const EncodeResult> results) try {
  std::error_code ec;
  fs::create_directories(output_dir, ec);
  if (ec) {
    return std::unexpected{std::format(
        "无法创建报告目录: "
        "{}；系统错误：{}"
        "。请确认输出目录可写、磁盘未满，且路径没有被其他程序锁定。",
        display_path_for_user(output_dir), ec.message())};
  }

  const auto report_path = output_dir / L"summary.csv";
  fs::path temp_path;
  auto created = create_summary_csv_temp_file(output_dir, temp_path);
  if (!created) {
    return std::unexpected{created.error()};
  }
  SummaryTempFile temp_file{temp_path};
  SummaryCsvWriter csv{std::move(*created), report_path};

  csv << "\xEF\xBB\xBF";
  csv << "index,input,output,format,encoder_id,input_sha256,output_sha256,original_bytes,output_"
         "bytes,ratio,quality,speed,"
         "requested_visual_quality,visual_score,raw_gmsd,raw_ms_ssim,"
         "gmsd_quality_score,msssim_quality_score,gmsd_weight,msssim_weight,"
         "final_encoder_quality,visual_quality_target_met,search_attempt_count,"
         "decoder_id,decoder_fallback,encoder_selected,encoder_requested,"
         "encoder_license,"
         "integration_mode,"
         "requested_chroma,applied_chroma,requested_color_representation,applied_color_representation,requested_bit_depth,applied_bit_"
         "depth,bit_depth_reason,"
         "fallback_reason,speed_parameter_kind,applied_speed,encoder_threads,"
         "memory_budget_bytes,"
         "quality_overridden_by_visual_quality,lossless,"
         "seconds,decode_seconds,prepare_seconds,encode_seconds,avif_rgb_to_yuv_"
         "seconds,avif_add_image_seconds,avif_finish_seconds,avif_output_copy_"
         "seconds,write_seconds,"
         "visual_quality_search_seconds,visual_quality_candidate_encode_"
         "seconds,visual_quality_candidate_decode_seconds,"
         "visual_quality_candidate_io_seconds,visual_quality_luma_seconds,gmsd_"
         "seconds,ms_ssim_seconds,"
         "visual_quality_metric_seconds,visual_quality_candidate_count,visual_"
         "quality_decode_memory_fallback_count,visual_quality_gpu_fallback_"
         "count,visual_quality_gpu_requested,visual_quality_gpu_used,"
         "visual_quality_gpu_path,visual_quality_gpu_fallback_reason,"
         "status,message,command,"
         "user_encoder,user_chroma,source_chroma,chroma_reason,source_bit_"
         "depth,"
         "alpha_policy,source_has_alpha_channel,source_alpha_mode,has_non_"
         "opaque_alpha,encoder_supports_alpha,applied_alpha,alpha_reason,"
         "source_color_primaries,source_transfer_characteristics,source_matrix_"
         "coefficients,source_color_range,"
         "applied_color_primaries,applied_transfer_characteristics,applied_"
         "matrix_coefficients,applied_color_range,"
         "source_has_icc,applied_icc,source_has_hdr_metadata,applied_hdr_"
         "metadata,color_metadata_source,color_reason,visual_quality_search_trace,awj_version\n";

  for (const auto& result : results) {
    const double ratio = result.original_bytes == 0
                             ? 0.0
                             : static_cast<double>(result.output_bytes) /
                                   static_cast<double>(result.original_bytes);
    const char* status =
        result.large_image_queued
            ? "large_mode_queued"
            : (result.ok ? (result.skipped ? "skipped" : "ok")
                         : (result.canceled
                                ? "canceled"
                                : (result.processed ? "failed" : "pending")));
    const std::string speed =
        result.speed < 0 ? "default" : std::to_string(result.speed);
    const std::string requested_visual_quality =
        result.requested_visual_quality
            ? std::to_string(*result.requested_visual_quality)
            : std::string{};
    const auto optional_int = [](std::optional<int> value) {
      return value ? std::to_string(*value) : std::string{};
    };
    const auto optional_bool = [](std::optional<bool> value) {
      return value ? (*value ? std::string{"true"} : std::string{"false"})
                   : std::string{};
    };
    const auto metric = [](double value) {
      return value > 0.0 ? std::format("{:.6f}", value) : std::string{};
    };
    const std::string visual_quality_target_met =
        result.ok && !result.skipped &&
                result.requested_visual_quality.has_value()
            ? (result.visual_quality_target_met ? std::string{"true"}
                                                : std::string{"false"})
            : std::string{};
    const bool has_visual_metrics =
        result.ok && !result.skipped && !result.lossless &&
        result.requested_visual_quality.has_value() &&
        result.search_attempt_count > 0;
    const auto visual_metric = [&](double value) {
      return has_visual_metrics ? std::format("{:.6f}", value) : std::string{};
    };
    const auto duration = [](double value) {
      return value >= 0.0 ? std::format("{:.6f}", value) : std::string{};
    };
    const auto sha256_or_empty = [](const fs::path& path) {
      if (path.empty()) {
        return std::string{};
      }
      std::error_code ec;
      if (!fs::is_regular_file(path, ec) || ec) {
        return std::string{};
      }
      auto digest = file_sha256_hex(path);
      return digest ? *digest : std::string{};
    };
    const auto input_sha256 = sha256_or_empty(result.input_path);
    const auto output_sha256 = result.ok && !result.skipped
                                   ? sha256_or_empty(result.output_path)
                                   : std::string{};
    csv << (result.index + 1) << ','
        << core_detail::csv_escape(display_path_for_report(result.input_path))
        << ','
        << core_detail::csv_escape(display_path_for_report(result.output_path))
        << ',' << core_detail::csv_escape(result.output_format)
        << ',' << core_detail::csv_escape(result.encoder_id)
        << ',' << input_sha256 << ',' << output_sha256 << ','
        << result.original_bytes << ',' << result.output_bytes << ','
        << std::format("{:.4f}", ratio) << ',' << result.quality << ',' << speed
        << ',' << requested_visual_quality << ','
        << visual_metric(result.visual_score) << ','
        << visual_metric(result.raw_gmsd) << ','
        << visual_metric(result.raw_ms_ssim) << ','
        << visual_metric(result.gmsd_quality_score) << ','
        << visual_metric(result.msssim_quality_score) << ','
        << metric(result.gmsd_weight) << ',' << metric(result.msssim_weight)
        << ',' << result.final_encoder_quality << ','
        << visual_quality_target_met << ',' << result.search_attempt_count
        << ',' << core_detail::csv_escape(result.decoder_id) << ','
        << (result.used_decoder_fallback ? "yes" : "no") << ','
        << core_detail::csv_escape(result.encoder_id) << ','
        << core_detail::csv_escape(result.requested_encoder_id) << ','
        << core_detail::csv_escape(result.encoder_license) << ','
        << core_detail::csv_escape(result.integration_mode) << ','
        << core_detail::csv_escape(result.requested_chroma) << ','
        << core_detail::csv_escape(result.applied_chroma) << ','
        << core_detail::csv_escape(result.requested_color_representation) << ','
        << core_detail::csv_escape(result.applied_color_representation) << ','
        << optional_int(result.requested_bit_depth) << ','
        << optional_int(result.applied_bit_depth) << ','
        << core_detail::csv_escape(result.bit_depth_reason) << ','
        << core_detail::csv_escape(result.fallback_reason) << ','
        << core_detail::csv_escape(result.speed_parameter_kind) << ','
        << result.applied_speed << ',' << result.encoder_threads << ','
        << result.memory_budget_bytes << ','
        << (result.quality_overridden_by_visual_quality ? "true" : "false")
        << ',' << (result.lossless ? "true" : "false") << ','
        << std::format("{:.3f}", result.seconds) << ','
        << duration(result.decode_seconds) << ','
        << duration(result.prepare_seconds) << ','
        << duration(result.encode_seconds) << ','
        << duration(result.avif_rgb_to_yuv_seconds) << ','
        << duration(result.avif_add_image_seconds) << ','
        << duration(result.avif_finish_seconds) << ','
        << duration(result.avif_output_copy_seconds) << ','
        << duration(result.write_seconds) << ','
        << duration(result.visual_quality_search_seconds) << ','
        << duration(result.visual_quality_candidate_encode_seconds) << ','
        << duration(result.visual_quality_candidate_decode_seconds) << ','
        << duration(result.visual_quality_candidate_io_seconds) << ','
        << duration(result.visual_quality_luma_seconds) << ','
        << duration(result.gmsd_seconds) << ','
        << duration(result.ms_ssim_seconds) << ','
        << duration(result.visual_quality_metric_seconds) << ','
        << result.visual_quality_candidate_count << ','
        << result.visual_quality_decode_memory_fallback_count << ','
        << result.visual_quality_gpu_fallback_count << ','
        << (result.visual_quality_gpu_requested ? "true" : "false") << ','
        << (result.visual_quality_gpu_used ? "true" : "false") << ','
        << core_detail::csv_escape(result.visual_quality_gpu_path) << ','
        << core_detail::csv_escape(result.visual_quality_gpu_fallback_reason) << ','
        << status << ','
        << core_detail::csv_escape(result.message) << ','
        << core_detail::csv_escape(result.command) << ','
        << core_detail::csv_escape(result.user_encoder_id) << ','
        << core_detail::csv_escape(result.user_chroma) << ','
        << core_detail::csv_escape(result.source_chroma) << ','
        << core_detail::csv_escape(result.chroma_reason) << ','
        << optional_int(result.source_bit_depth) << ','
        << core_detail::csv_escape(result.alpha_policy) << ','
        << (result.source_has_alpha_channel ? "true" : "false") << ','
        << core_detail::csv_escape(result.source_alpha_mode) << ','
        << optional_bool(result.has_non_opaque_alpha) << ','
        << (result.encoder_supports_alpha ? "true" : "false") << ','
        << core_detail::csv_escape(result.applied_alpha) << ','
        << core_detail::csv_escape(result.alpha_reason) << ','
        << optional_int(result.source_color_primaries) << ','
        << optional_int(result.source_transfer_characteristics) << ','
        << optional_int(result.source_matrix_coefficients) << ','
        << optional_int(result.source_color_range) << ','
        << optional_int(result.applied_color_primaries) << ','
        << optional_int(result.applied_transfer_characteristics) << ','
        << optional_int(result.applied_matrix_coefficients) << ','
        << optional_int(result.applied_color_range) << ','
        << (result.source_has_icc ? "true" : "false") << ','
        << core_detail::csv_escape(result.applied_icc) << ','
        << (result.source_has_hdr_metadata ? "true" : "false") << ','
        << core_detail::csv_escape(result.applied_hdr_metadata) << ','
        << core_detail::csv_escape(result.color_metadata_source) << ','
        << core_detail::csv_escape(result.color_reason) << ','
        << core_detail::csv_escape(result.visual_quality_search_trace) << ','
        << kAwjVersion << '\n';
  }

  if (auto closed = csv.close(); !closed) {
    return std::unexpected{closed.error()};
  }
  if (auto attributes = core_detail::clear_transient_file_attributes(
          temp_path, "报告临时文件");
      !attributes) {
    return std::unexpected{attributes.error()};
  }
#ifdef _WIN32
  if (!MoveFileExW(temp_path.c_str(), report_path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return std::unexpected{std::format("替换报告文件失败: {}；系统错误：{}。",
                                       display_path_for_user(report_path),
                                       win32_error_message(GetLastError()))};
  }
#else
  if (::rename(temp_path.c_str(), report_path.c_str()) != 0) {
    return std::unexpected{std::format("替换报告文件失败: {}；系统错误：{}。",
                                       display_path_for_user(report_path),
                                       posix_error_message(errno))};
  }
  const auto parent = report_path.parent_path();
  if (!parent.empty()) {
    const int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
      (void)::fsync(dir);
      (void)::close(dir);
    }
  }
#endif
  temp_file.release();
  return {};
} catch (const std::bad_alloc&) {
  return std::unexpected{"summary.csv 内容内存不足，报告未写入。"};
} catch (const std::length_error&) {
  return std::unexpected{"summary.csv 内容超过运行时限制，报告未写入。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"summary.csv 文件系统访问失败，报告未写入。"};
}

}  // namespace awj
