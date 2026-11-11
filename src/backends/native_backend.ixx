module;
#include "../compat/stdlib_compat.h"
#include "../core/hdr_cost.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

export module awj.native_backend;

import awj.avif_aom_codec;
import awj.animation;
import awj.gain_map;
import awj.animation_decoder;
import awj.avif_registry;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.hdr_tonemap;
import awj.image;
import awj.pixel_conversion;
#if AWJ_HAS_JPEGLI
import awj.jpegli_codec;
import awj.jpeg_hdr_codec;
#endif
import awj.jxl_codec;
import awj.large_image_plan;
import awj.native_visual_search;
import awj.png_codec;
import awj.resource_planner;
import awj.image_memory_plan;
import awj.visual_quality;
import awj.webp_codec;

export namespace awj {

namespace native_backend_detail {

using Clock = std::chrono::steady_clock;

double elapsed_seconds(Clock::time_point started) {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

// 同步且不重叠的阶段区间；错误/取消时也在作用域退出记录已发生的工作。
// 可选嵌套计数器用于扣除动画编码外层墙钟内发生的 reader 工作。
class ScopedStageTimer {
 public:
  explicit ScopedStageTimer(double& seconds, const double* nested_seconds = nullptr) noexcept
      : seconds_{seconds}, nested_seconds_{nested_seconds},
        nested_started_{nested_seconds ? *nested_seconds : 0.0}, started_{Clock::now()} {}
  ScopedStageTimer(const ScopedStageTimer&) = delete;
  ScopedStageTimer& operator=(const ScopedStageTimer&) = delete;
  ~ScopedStageTimer() noexcept {
    const auto nested = nested_seconds_ ? std::max(0.0, *nested_seconds_ - nested_started_) : 0.0;
    seconds_ = std::max(0.0, seconds_) + std::max(0.0, elapsed_seconds(started_) - nested);
  }

 private:
  double& seconds_;
  const double* nested_seconds_;
  double nested_started_;
  Clock::time_point started_;
};

std::string format_timing_seconds(double seconds) {
  return seconds >= 0.0 ? awj::compat::format("{:.3f}", seconds) : std::string{""};
}

std::string format_timing_share(double part, double total) {
  return part >= 0.0 && total > 0.0 ? awj::compat::format("{:.1f}", part * 100.0 / total) : std::string{""};
}

template <class Function>
void log_info_noexcept(FileLogger& logger, Function&& message) noexcept {
  try {
    logger.info(message());
  } catch (...) {
  }
}

void merge_stage_timing(NativeEncodeResult& native, const EncodeResult& result) {
  auto& timing = native.diagnostics.timing;
  if (result.decode_seconds >= 0.0) {
    timing.decode_seconds = result.decode_seconds;
  }
  // 用户几何计时只由 backend 管理；未观察到的尝试必须清除复用/内部编码结果的旧值。
  timing.resize_seconds = result.resize_seconds;
  timing.resize_workers = result.resize_seconds >= 0.0 ? result.resize_workers : 0;
  if (result.prepare_seconds >= 0.0) {
    timing.prepare_seconds = result.prepare_seconds;
  }
  if (timing.encode_seconds < 0.0 && result.encode_seconds >= 0.0) {
    timing.encode_seconds = result.encode_seconds;
  }
  if (result.write_seconds >= 0.0) {
    timing.write_seconds = result.write_seconds;
  }
}

class UnsupportedDecoder final : public ImageDecoder {
 public:
  explicit UnsupportedDecoder(std::string id) : id_{std::move(id)} {}

  [[nodiscard]] std::string_view id() const noexcept override { return id_; }
  [[nodiscard]] bool can_decode(const fs::path&) const override { return false; }
  awj::compat::expected<ImageDecodeResult, std::string> decode(const fs::path&) const override {
    return awj::compat::unexpected{awj::compat::format("native backend 当前不支持该输入解码器: {}", id_)};
  }

 private:
  std::string id_;
};

std::unique_ptr<ImageDecoder> decoder_for_output_format(OutputFormat format, int decode_threads) {
  const auto clamped_decode_threads = std::max(1, decode_threads);
  switch (format) {
    case OutputFormat::png:
      return std::make_unique<PngImageDecoder>();
    case OutputFormat::webp:
      return std::make_unique<WebPImageDecoder>(clamped_decode_threads);
    case OutputFormat::jxl:
      return std::make_unique<JXLImageDecoder>(clamped_decode_threads);
    case OutputFormat::jpgli:
#if AWJ_HAS_JPEGLI
      return std::make_unique<JpegliImageDecoder>();
#else
      return std::make_unique<UnsupportedDecoder>("jpegli");
#endif
    case OutputFormat::avif:
      return make_avif_image_decoder(clamped_decode_threads);
    default:
      return std::make_unique<UnsupportedDecoder>("avif");
  }
}

std::unique_ptr<ImageEncoder> encoder_for_output_format(OutputFormat format,
                                                        AvifEncoderMode avif_encoder) {
  switch (format) {
    case OutputFormat::png:
      return std::make_unique<PngImageEncoder>();
    case OutputFormat::webp:
      return std::make_unique<WebPImageEncoder>();
    case OutputFormat::jxl:
      return std::make_unique<JXLImageEncoder>();
    case OutputFormat::jpgli:
#if AWJ_HAS_JPEGLI
      return std::make_unique<JpegliImageEncoder>();
#else
      return nullptr;
#endif
    case OutputFormat::avif:
      return make_avif_image_encoder(avif_encoder);
    default:
      return nullptr;
  }
}

class UniqueHandle {
 public:
#ifdef _WIN32
  using native_handle = HANDLE;
  static constexpr native_handle invalid() noexcept { return INVALID_HANDLE_VALUE; }
#else
  using native_handle = int;
  static constexpr native_handle invalid() noexcept { return -1; }
#endif

  UniqueHandle() noexcept = default;
  explicit UniqueHandle(native_handle handle) noexcept : handle_{handle} {}
  ~UniqueHandle() { reset(); }
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
  UniqueHandle(UniqueHandle&& other) noexcept : handle_{other.release()} {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
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

void remove_file_noexcept(const fs::path& path) noexcept {
  try {
    std::error_code ec;
    fs::remove(path, ec);
  } catch (...) {
  }
}

class TempOutputFile {
 public:
  explicit TempOutputFile(const fs::path& path) noexcept : path_{&path} {}
  ~TempOutputFile() {
    if (active_) {
      remove_file_noexcept(*path_);
    }
  }
  TempOutputFile(const TempOutputFile&) = delete;
  TempOutputFile& operator=(const TempOutputFile&) = delete;
  [[nodiscard]] const fs::path& path() const noexcept { return *path_; }
  void release() noexcept { active_ = false; }

 private:
  const fs::path* path_{};
  bool active_{true};
};

#ifdef _WIN32
struct SourceFileTimes {
  FILETIME creation{};
  FILETIME modification{};
  FILETIME access{};
};

awj::compat::expected<SourceFileTimes, std::string> capture_source_file_times(
    const fs::path& path) {
  const HANDLE raw_file = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw_file == INVALID_HANDLE_VALUE) {
    return awj::compat::unexpected{awj::compat::format("无法读取源文件时间 {}: {}",
                                       display_path_for_user(path),
                                       win32_error_message(GetLastError()))};
  }
  UniqueHandle file{raw_file};
  SourceFileTimes times{};
  if (!GetFileTime(file.get(), &times.creation, &times.access,
                   &times.modification)) {
    return awj::compat::unexpected{awj::compat::format("无法读取源文件时间 {}: {}",
                                       display_path_for_user(path),
                                       win32_error_message(GetLastError()))};
  }
  return times;
}

awj::compat::expected<void, std::string> apply_source_file_times(
    const fs::path& path, const SourceFileTimes& source,
    bool preserve_creation, bool preserve_modification, bool preserve_access) {
  const HANDLE raw_file = CreateFileW(
      path.c_str(), FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw_file == INVALID_HANDLE_VALUE) {
    return awj::compat::unexpected{awj::compat::format("无法写入输出文件时间 {}: {}",
                                       display_path_for_user(path),
                                       win32_error_message(GetLastError()))};
  }
  UniqueHandle file{raw_file};
  if (!SetFileTime(file.get(), preserve_creation ? &source.creation : nullptr,
                   preserve_access ? &source.access : nullptr,
                   preserve_modification ? &source.modification : nullptr)) {
    return awj::compat::unexpected{awj::compat::format("无法写入输出文件时间 {}: {}",
                                       display_path_for_user(path),
                                       win32_error_message(GetLastError()))};
  }
  return {};
}
#endif

awj::compat::expected<void, std::string> clear_transient_file_attributes(const fs::path& path,
                                                                 std::string_view label) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    return awj::compat::unexpected{awj::compat::format("无法读取{}属性 {}: {}",
                                       label,
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
    return awj::compat::unexpected{awj::compat::format("无法更新{}属性 {}: {}",
                                       label,
                                       display_path_for_user(path),
                                       win32_error_message(error))};
  }
#else
  (void)path;
  (void)label;
#endif
  return {};
}

struct TempOutputWriteFailure {
  std::string message;
  int create_error{};
};

bool output_temp_name_collision(int error) noexcept {
#ifdef _WIN32
  return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
#else
  return error == EEXIST;
#endif
}

awj::compat::expected<void, TempOutputWriteFailure> write_file_bytes_exclusive(const fs::path& path,
                                                                         std::span<const std::byte> bytes) {
  if (bytes.size() > encoding_defaults::effective_max_input_file_bytes()) {
    return awj::compat::unexpected{TempOutputWriteFailure{
        .message = awj::compat::format("临时输出文件内容超过当前运行时上限: {}",
                               display_path_for_user(path))}};
  }
#ifdef _WIN32
  const auto raw_file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                   nullptr);
  if (raw_file == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    return awj::compat::unexpected{TempOutputWriteFailure{
        .message = awj::compat::format("无法创建临时输出文件 {}: {}",
                               display_path_for_user(path),
                               win32_error_message(error)),
        .create_error = static_cast<int>(error)}};
  }
#else
  const int raw_file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (raw_file < 0) {
    const int error = errno;
    return awj::compat::unexpected{TempOutputWriteFailure{
        .message = awj::compat::format("无法创建临时输出文件 {}: {}",
                               display_path_for_user(path),
                               posix_error_message(error)),
        .create_error = error}};
  }
#endif
  TempOutputFile cleanup{path};
  UniqueHandle file{raw_file};

  auto remaining = bytes.size();
  const auto* cursor = reinterpret_cast<const std::uint8_t*>(bytes.data());
  while (remaining > 0) {
#ifdef _WIN32
    const auto chunk = static_cast<DWORD>(
        std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file.get(), cursor, chunk, &written, nullptr)) {
      return awj::compat::unexpected{TempOutputWriteFailure{
          .message = awj::compat::format("写入临时输出文件失败 {}: {}",
                                 display_path_for_user(path),
                                 win32_error_message(GetLastError()))}};
    }
#else
    const auto written = ::write(file.get(), cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return awj::compat::unexpected{TempOutputWriteFailure{
          .message = awj::compat::format("写入临时输出文件失败 {}: {}",
                                 display_path_for_user(path),
                                 posix_error_message(errno))}};
    }
#endif
    if (written == 0) {
      return awj::compat::unexpected{TempOutputWriteFailure{
          .message = awj::compat::format("写入临时输出文件失败 {}", display_path_for_user(path))}};
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
#ifdef _WIN32
  if (!FlushFileBuffers(file.get())) {
    return awj::compat::unexpected{TempOutputWriteFailure{
        .message = awj::compat::format("刷新临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               win32_error_message(GetLastError()))}};
  }
  const HANDLE file_handle = file.get();
  if (!CloseHandle(file_handle)) {
    return awj::compat::unexpected{TempOutputWriteFailure{
        .message = awj::compat::format("关闭临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               win32_error_message(GetLastError()))}};
  }
#else
  const int file_handle = file.get();
  if (::fsync(file_handle) != 0) {
    return awj::compat::unexpected{TempOutputWriteFailure{
        .message = awj::compat::format("刷新临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               posix_error_message(errno))}};
  }
  if (::close(file_handle) != 0) {
    return awj::compat::unexpected{TempOutputWriteFailure{
        .message = awj::compat::format("关闭临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               posix_error_message(errno))}};
  }
#endif
  file.release();
  cleanup.release();
  return {};
}

awj::compat::expected<void, std::string> commit_temp_output(const fs::path& temp_path,
                                                    const fs::path& path,
                                                    bool replace_existing) {
  if (!replace_existing) {
    if (const auto published = core_detail::publish_file_noreplace(temp_path, path); published) {
      if (published == std::errc::file_exists)
        return awj::compat::unexpected{awj::compat::format("输出路径已存在，未覆盖 {}", display_path_for_user(path))};
      return awj::compat::unexpected{awj::compat::format("替换输出文件失败 {}: {}",
          display_path_for_user(path), published.message())};
    }
  } else {
#ifdef _WIN32
    if (!MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return awj::compat::unexpected{awj::compat::format("替换输出文件失败 {}: {}",
          display_path_for_user(path), win32_error_message(GetLastError()))};
    }
#else
    if (::rename(temp_path.c_str(), path.c_str()) != 0) {
      return awj::compat::unexpected{awj::compat::format("替换输出文件失败 {}: {}", display_path_for_user(path),
          posix_error_message(errno))};
    }
#endif
  }
#ifndef _WIN32
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    const int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
      (void)::fsync(dir);
      (void)::close(dir);
    }
  }
#endif
  return {};
}

bool avif_lossless_requested(const AppConfig& cfg) noexcept {
  return cfg.output_format == OutputFormat::avif &&
         (cfg.visual_quality ? *cfg.visual_quality >= 100 : cfg.quality >= 100);
}

bool avif_lossless_passthrough_source(const fs::path& path) {
  static constexpr std::wstring_view avif_extensions[] = {L".avif"};
  return decoder_common::extension_is_one_of(path, avif_extensions);
}

bool jxl_jpeg_bitstream_source(const fs::path& path) {
  static constexpr std::wstring_view jpeg_extensions[] = {L".jpg", L".jpeg", L".jpe", L".jfif"};
  return decoder_common::extension_is_one_of(path, jpeg_extensions);
}

std::uint16_t read_be_u16(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) << 8) |
      static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])));
}

bool is_jpeg_sof_marker(std::uint8_t marker) noexcept {
  switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
      return true;
    default:
      return false;
  }
}

PixelFormat jpeg_pixel_format_from_sampling(std::uint8_t component_count,
                                            std::uint8_t horizontal_factor,
                                            std::uint8_t vertical_factor) noexcept {
  if (component_count == 1) {
    return PixelFormat::gray;
  }
  if (component_count != 3) {
    return PixelFormat::unknown;
  }
  if (horizontal_factor == 1 && vertical_factor == 1) {
    return PixelFormat::yuv444;
  }
  if (horizontal_factor == 2 && vertical_factor == 1) {
    return PixelFormat::yuv422;
  }
  if (horizontal_factor == 2 && vertical_factor == 2) {
    return PixelFormat::yuv420;
  }
  return PixelFormat::unknown;
}

struct JpegBitstreamSourceDiagnostics {
  std::size_t width{};
  std::size_t height{};
  PixelFormat pixel_format{PixelFormat::unknown};
  std::optional<int> bit_depth{};
  bool has_icc{};
  bool has_mpf{};
};

JpegBitstreamSourceDiagnostics inspect_jpeg_bitstream_source(
    std::span<const std::byte> bytes) noexcept {
  JpegBitstreamSourceDiagnostics diagnostics{};
  if (bytes.size() < 4 || std::to_integer<std::uint8_t>(bytes[0]) != 0xFF ||
      std::to_integer<std::uint8_t>(bytes[1]) != 0xD8) {
    return diagnostics;
  }

  std::size_t offset = 2;
  while (offset + 3 < bytes.size()) {
    while (offset < bytes.size() && std::to_integer<std::uint8_t>(bytes[offset]) == 0xFF) {
      ++offset;
    }
    if (offset >= bytes.size()) {
      break;
    }

    const auto marker = std::to_integer<std::uint8_t>(bytes[offset++]);
    if (marker == 0xD9 || marker == 0xDA) {
      break;
    }
    if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
      continue;
    }
    if (offset + 2 > bytes.size()) {
      break;
    }

    const auto segment_size = read_be_u16(bytes, offset);
    if (segment_size < 2) {
      break;
    }
    const auto payload_offset = offset + 2;
    const auto payload_size = static_cast<std::size_t>(segment_size - 2);
    if (payload_offset > bytes.size() || payload_size > bytes.size() - payload_offset) {
      break;
    }

    if (marker == 0xE2 && payload_size >= sizeof("ICC_PROFILE") + 2) {
      static constexpr char signature[] = "ICC_PROFILE";
      bool matches = true;
      for (std::size_t i = 0; i < sizeof(signature); ++i) {
        if (std::to_integer<std::uint8_t>(bytes[payload_offset + i]) !=
            static_cast<std::uint8_t>(signature[i])) {
          matches = false;
          break;
        }
      }
      diagnostics.has_icc = diagnostics.has_icc || matches;
    }
    if (marker == 0xE2 && payload_size >= 4 &&
        bytes[payload_offset] == std::byte{'M'} &&
        bytes[payload_offset + 1] == std::byte{'P'} &&
        bytes[payload_offset + 2] == std::byte{'F'} &&
        bytes[payload_offset + 3] == std::byte{0}) {
      diagnostics.has_mpf = true;
    }

    if (is_jpeg_sof_marker(marker) && payload_size >= 6) {
      const auto precision = std::to_integer<std::uint8_t>(bytes[payload_offset]);
      const auto component_count = std::to_integer<std::uint8_t>(bytes[payload_offset + 5]);
      if (payload_size >= 6 + static_cast<std::size_t>(component_count) * 3 && component_count > 0) {
        const auto sampling = std::to_integer<std::uint8_t>(bytes[payload_offset + 7]);
        diagnostics.bit_depth = static_cast<int>(precision);
        diagnostics.height = read_be_u16(bytes, payload_offset + 1);
        diagnostics.width = read_be_u16(bytes, payload_offset + 3);
        diagnostics.pixel_format = jpeg_pixel_format_from_sampling(
            component_count,
            static_cast<std::uint8_t>(sampling >> 4),
            static_cast<std::uint8_t>(sampling & 0x0F));
      }
    }

    offset = payload_offset + payload_size;
  }
  return diagnostics;
}

std::string alpha_mode_name(AlphaMode mode) {
  switch (mode) {
    case AlphaMode::straight:
      return "straight";
    case AlphaMode::premultiplied:
      return "premultiplied";
    case AlphaMode::none:
    default:
      return "none";
  }
}

bool image_has_metadata(const ImageBuffer& image, MetadataKind kind) noexcept {
  return awj::compat::ranges::find_if(image.metadata, [kind](const MetadataBlock& block) {
           return block.kind == kind && !block.bytes.empty();
         }) != image.metadata.end();
}

bool sdr_only_format_conversion_needed(OutputFormat format, const ImageBuffer& image) noexcept {
  if (format != OutputFormat::webp && format != OutputFormat::jpgli) {
    return false;
  }
  const bool hdr_transfer = image.source_info &&
                            (image.source_info->transfer_characteristics == 16 ||
                             image.source_info->transfer_characteristics == 18);
  return image.bit_depth > 8 || image.sample_representation == SampleRepresentation::ieee_half_float ||
         hdr_transfer || (image.source_info && image.source_info->has_hdr_metadata);
}

awj::compat::expected<void, std::string> select_output_rendition(
    ImageDecodeResult& decoded, const AppConfig& cfg, awj::compat::stop_token stop) {
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  if (!decoded.gain_map_alternative) return {};
  auto& alternative = *decoded.gain_map_alternative;
  const auto main_headroom = alternative.main_hdr_headroom_log2;
  const auto alternate_headroom = alternative.alternate_hdr_headroom_log2;
  if (!std::isfinite(main_headroom) || !std::isfinite(alternate_headroom) ||
      main_headroom < 0 || alternate_headroom < 0) {
    return awj::compat::unexpected{"Gain Map: 两端呈现 headroom 无效。"};
  }
  if (decoded.image.width != alternative.image.width || decoded.image.height != alternative.image.height) {
    return awj::compat::unexpected{"Gain Map: 两端呈现的逻辑尺寸不一致。"};
  }
  for (const auto* image : {&decoded.image, &alternative.image}) {
    if (auto valid = hdr::hdr_detail::validate_rgba(*image, "Gain Map 呈现选择"); !valid)
      return awj::compat::unexpected{valid.error()};
  }
  auto main_color = gain_map_detail::resolve_iso_color(decoded.image, stop);
  if (!main_color) return awj::compat::unexpected{main_color.error()};
  auto alternate_color = gain_map_detail::resolve_iso_color(alternative.image, stop);
  if (!alternate_color) return awj::compat::unexpected{alternate_color.error()};
  gain_map_detail::normalize_iso_base(decoded.image, *main_color);
  gain_map_detail::normalize_iso_base(alternative.image, *alternate_color);
  const auto main_hdr = hdr::has_explicit_hdr_signal(decoded.image);
  if (!main_hdr) return awj::compat::unexpected{main_hdr.error()};
  const auto alternate_hdr = hdr::has_explicit_hdr_signal(alternative.image);
  if (!alternate_hdr) return awj::compat::unexpected{alternate_hdr.error()};
  if (*main_hdr != (main_headroom > 0) || *alternate_hdr != (alternate_headroom > 0)) {
    return awj::compat::unexpected{"Gain Map: 呈现 headroom 与 SDR/HDR 色彩解释冲突。"};
  }
  if (main_headroom < alternate_headroom) {
    std::swap(decoded.image, alternative.image);
    std::swap(alternative.main_hdr_headroom_log2, alternative.alternate_hdr_headroom_log2);
  }
  const bool paired = alternative.main_hdr_headroom_log2 > 0 &&
                      alternative.alternate_hdr_headroom_log2 == 0;
  const bool select_sdr = cfg.output_format == OutputFormat::webp ||
      (cfg.output_format == OutputFormat::jpgli && cfg.jpeg_hdr == JpegHdrMode::sdr);
  if (paired && select_sdr) std::swap(decoded.image, alternative.image);
  // 只有真实 SDR/HDR 配对才输出 Gain Map；其余保留所选单层，JPEG 可另生成 SDR base。
  if (!paired || select_sdr || cfg.output_format == OutputFormat::png)
    decoded.gain_map_alternative.reset();
  return {};
}

bool geometry_keeps_alpha(const AppConfig& cfg) noexcept {
  return cfg.output_format != OutputFormat::jpgli && cfg.alpha_policy != AlphaModePolicy::off;
}

awj::compat::expected<void, std::string> apply_image_size_limit(ImageDecodeResult& decoded,
    const AppConfig& cfg, awj::compat::stop_token stop, const ResourcePlan& resources,
    EncodeResult& timing, const std::function<bool(std::uint64_t)>& reserve_memory = {}) {
  // 所有呈现成功后才提交 worker 最大值；错误或异常保留已耗时长，但不发布成功 worker 数。
  timing.resize_workers = 0;
  int workers{};
  auto held_resources = resources;
  const auto transform = [&](ImageBuffer& image, const ImageBuffer* other) -> awj::compat::expected<void, std::string> {
    auto plan = plan_image_geometry(image.width, image.height, cfg);
    if (!plan) return awj::compat::unexpected{plan.error()};
    if (!plan->changes()) {
      if (timing.resize_seconds < 0.0) timing.resize_seconds = 0.0;
      return {};
    }
    ScopedStageTimer timer{timing.resize_seconds};
    auto layer_resources = held_resources;
    if (other) layer_resources.geometry_retained_bytes = hdr_cost::add(
        layer_resources.geometry_retained_bytes, resident_image_bytes(*other));
    const auto reserve = reserve_memory ? std::function<bool(std::uint64_t)>{[&](std::uint64_t bytes) {
      if (!reserve_memory(bytes)) return false;
      timing.memory_plan.reserved_peak_bytes = std::max(timing.memory_plan.reserved_peak_bytes, bytes);
      return true;
    }} : std::function<bool(std::uint64_t)>{};
    auto admitted = admit_image_geometry(image, *plan, layer_resources, reserve);
    if (!admitted) return awj::compat::unexpected{admitted.error()};
    held_resources.memory_reservation_bytes = admitted->memory_limit_bytes;
    GeometryTransformDiagnostics diagnostics{};
    auto transformed = apply_image_geometry(image, *plan, geometry_keeps_alpha(cfg), stop, *admitted, &diagnostics);
    if (!transformed) return awj::compat::unexpected{transformed.error()};
    image = std::move(*transformed);
    workers = std::max(workers, diagnostics.workers);
    return {};
  };
  if (auto result = transform(decoded.image, decoded.gain_map_alternative ? &decoded.gain_map_alternative->image : nullptr); !result) return result;
  if (decoded.gain_map_alternative) {
    if (auto result = transform(decoded.gain_map_alternative->image, &decoded.image); !result) return result;
  }
  timing.resize_workers = workers;
  return {};
}

class GeometryAnimationReader final : public AnimationReader {
 public:
  // 读取与编码均为同步；调用方在编码器返回前持有计时与 cfg，计数器和借用帧不逃逸。
  GeometryAnimationReader(std::unique_ptr<AnimationReader> reader, const AppConfig& cfg,
                          ResourcePlan resources, EncodeResult& timing,
                          std::function<bool(std::uint64_t)> reserve_memory = {})
      : reader_{std::move(reader)}, cfg_{cfg}, info_{reader_->info()}, resources_{normalize_memory_policy_resources(cfg, resources)}, timing_{timing},
        reserve_memory_{std::move(reserve_memory)} {
    const auto threads = per_file_thread_budget(resources_);
    if (resources_.memory_limit_bytes && !resources_.memory_reservation_bytes && !reserve_memory_) {
      const auto budget = resource_memory_budget(resources_);
      if (!budget) error_ = "动画每文件内存预算不足。";
      else resources_.memory_limit_bytes = budget;
    }
    resources_.encoder_threads_per_file = threads;
    resources_.file_parallelism = 1;
    auto plan = plan_image_geometry(info_.width, info_.height, cfg_);
    if (!plan) error_ = plan.error();
    else {
      plan_ = *plan;
      info_.width = plan->canvas_width; info_.height = plan->canvas_height;
    }
  }
  const AnimationInfo& info() const noexcept override { return info_; }
  const ImageBuffer& frame() const noexcept override { return transformed_ ? *transformed_ : reader_->frame(); }
  std::uint64_t memory_usage_bytes() const noexcept override {
    return hdr_cost::add(underlying_memory_bytes(), transformed_ ? resident_image_bytes(*transformed_) : 0);
  }
  // 编码器在 next() 前已预约 reader、编码工作区及保留流；嵌套读取/缩放期间，
  // 继续保留已接受额度中的非 reader 部分，几何增长必须先取得新增字节。
  void bind_encoding_memory(NativeEncodeSettings& settings) {
    reserve_memory_ = settings.memory_reservation;
    note_encoding_reservation(settings.memory_base_reservation);
    settings.memory_reservation = [this, reserve = reserve_memory_](std::uint64_t bytes) {
      if (reserve && !reserve(bytes)) return false;
      note_encoding_reservation(bytes);
      return true;
    };
  }
  double read_seconds() const noexcept { return read_seconds_; }
  [[nodiscard]] ScopedStageTimer time_encode(double& seconds) const noexcept {
    return ScopedStageTimer{seconds, &read_seconds_};
  }
  awj::compat::expected<bool, std::string> next(awj::compat::stop_token stop = {}) override {
    ScopedStageTimer read_timer{read_seconds_};
    const auto previous_workers = std::exchange(timing_.resize_workers, 0);
    if (!error_.empty()) return awj::compat::unexpected{error_};
    if (transformed_) {
      ScopedStageTimer timer{timing_.resize_seconds};
      transformed_.reset();
    }
    auto next = [&] {
      ScopedStageTimer timer{timing_.decode_seconds};
      return reader_->next(stop);
    }();
    if (!next) return next;
    if (!*next) {
      timing_.resize_workers = previous_workers;
      return false;
    }
    if (plan_->changes()) {
      ScopedStageTimer timer{timing_.resize_seconds};
      auto transform_resources = resources_;
      std::uint64_t frame_bytes{};
      for (const auto& plane : reader_->frame().planes)
        frame_bytes = hdr_cost::add(frame_bytes, hdr_cost::add(plane.bytes.capacity(), plane.borrowed.size()));
      const auto underlying = underlying_memory_bytes();
      const auto other = underlying > frame_bytes ? underlying - frame_bytes : 0;
      transform_resources.geometry_retained_bytes = hdr_cost::add(transform_resources.geometry_retained_bytes,
          hdr_cost::add(other, encoder_retained_bytes_));
      auto admitted = admit_image_geometry(reader_->frame(), *plan_, transform_resources, reserve_memory_);
      if (!admitted) return awj::compat::unexpected{admitted.error()};
      resources_.memory_reservation_bytes = admitted->memory_limit_bytes;
      if (reserve_memory_ && admitted->memory_limit_bytes)
        timing_.memory_plan.reserved_peak_bytes = std::max(timing_.memory_plan.reserved_peak_bytes, admitted->memory_limit_bytes);
      GeometryTransformDiagnostics diagnostics{};
      auto result = apply_image_geometry(reader_->frame(), *plan_, geometry_keeps_alpha(cfg_), stop, *admitted, &diagnostics);
      if (!result) return awj::compat::unexpected{result.error()};
      transformed_ = std::move(*result);
      timing_.resize_workers = std::max(previous_workers, diagnostics.workers);
    } else {
      const auto required = hdr_cost::add(memory_usage_bytes(), hdr_cost::add(encoder_retained_bytes_, resources_.geometry_retained_bytes));
      if (required == UINT64_MAX) return awj::compat::unexpected{"动画阅读器工作集尺寸溢出。"};
      if (!encoding_defaults::current_memory_policy().unrestricted_memory && required > resource_memory_budget(resources_)) {
        if (!reserve_memory_ || required > resources_.memory_limit_bytes || !reserve_memory_(required))
          return awj::compat::unexpected{"动画阅读器无法取得工作集预约。"};
        resources_.memory_reservation_bytes = required;
      }
      if (timing_.resize_seconds < 0.0) timing_.resize_seconds = 0.0;
      timing_.resize_workers = previous_workers;
    }
    return true;
  }
 private:
  std::uint64_t underlying_memory_bytes() const noexcept {
    // info_ 复制 reader 索引；reader 的字节报告若已含平面/容器元数据，
    // 几何估算仍保守地再次计入这些描述数据。
    return hdr_cost::add(std::max(reader_->memory_usage_bytes(), resident_image_bytes(reader_->frame())), hdr_cost::add(
        hdr_cost::multiply(info_.durations.capacity(), sizeof(FrameDuration)), hdr_cost::add(info_.decoder_id.capacity(), 1)));
  }
  void note_encoding_reservation(std::uint64_t bytes) noexcept {
    const auto reader_bytes = memory_usage_bytes();
    encoder_retained_bytes_ = bytes > reader_bytes ? bytes - reader_bytes : 0;
    resources_.memory_reservation_bytes = resources_.memory_limit_bytes ? bytes : 0;
  }
  std::unique_ptr<AnimationReader> reader_;
  const AppConfig& cfg_;
  AnimationInfo info_;
  ResourcePlan resources_;
  EncodeResult& timing_;
  std::optional<ImageGeometryPlan> plan_;
  std::optional<ImageBuffer> transformed_;
  std::string error_;
  double read_seconds_{};
  std::function<bool(std::uint64_t)> reserve_memory_;
  std::uint64_t encoder_retained_bytes_{};
};

std::string chroma_name_from_pixel_format(PixelFormat pixel_format) {
  return pixel_format == PixelFormat::unknown ? "unknown"
                                             : chroma_mode_name(avif_auto_chroma(pixel_format));
}

std::string source_chroma_name(const ImageBuffer& image) {
  if (!image.source_info) {
    return "unknown";
  }
  return chroma_name_from_pixel_format(image.source_info->pixel_format);
}

ChromaMode lossless_source_chroma(const ImageBuffer& image) noexcept {
  return avif_auto_chroma(image.source_info ? image.source_info->pixel_format
                                          : image.pixel_format);
}

AvifColorRepresentation effective_avif_color_representation(
    const AppConfig& cfg, const ImageBuffer& image) noexcept {
  if (cfg.avif_color_representation != AvifColorRepresentation::source) {
    return cfg.avif_color_representation;
  }
  if (cfg.chroma_mode == ChromaMode::yuv400) return AvifColorRepresentation::yuv;
  if (!image.source_info) {
    return image.pixel_format == PixelFormat::rgb ||
                   image.pixel_format == PixelFormat::rgba
               ? AvifColorRepresentation::rgb_identity
               : AvifColorRepresentation::yuv;
  }
  const auto format = image.source_info->pixel_format;
  if (format == PixelFormat::rgb || format == PixelFormat::rgba ||
      image.source_info->matrix_coefficients == 0) {
    return AvifColorRepresentation::rgb_identity;
  }
  return AvifColorRepresentation::yuv;
}

std::optional<int> lossless_source_bit_depth(const ImageBuffer& image) noexcept {
  if (image.source_info && image.source_info->bit_depth > 0) {
    if (image.source_info->bit_depth < 8 && image.bit_depth >= 8) {
      return image.bit_depth;
    }
    return image.source_info->bit_depth;
  }
  return image.bit_depth > 0 ? std::optional<int>{image.bit_depth} : std::nullopt;
}

bool lossless_uses_decoded_bit_depth(const ImageBuffer& image) noexcept {
  return image.source_info && image.source_info->bit_depth > 0 &&
         image.source_info->bit_depth < 8 && image.bit_depth >= 8;
}

std::optional<int> source_bit_depth(const ImageBuffer& image) noexcept {
  if (image.source_info && image.source_info->bit_depth > 0) {
    return image.source_info->bit_depth;
  }
  return image.bit_depth > 0 ? std::optional<int>{image.bit_depth} : std::nullopt;
}

std::optional<int> choose_color_value(std::optional<int> user_value,
                                      std::optional<int> source_value) noexcept {
  return user_value ? user_value : source_value;
}

std::optional<int> non_unspecified_color_value(std::optional<int> value, int unspecified) noexcept {
  if (!value || *value == unspecified) {
    return {};
  }
  return value;
}

bool has_user_cicp_settings(const AppConfig& cfg) noexcept {
  return cfg.color_primaries || cfg.transfer_characteristics || cfg.matrix_coefficients ||
         cfg.color_range;
}



bool has_user_color_settings(const AppConfig& cfg) noexcept {
  return has_user_cicp_settings(cfg);
}

bool requested_color_conversion_needed(const ImageBuffer& image, const AppConfig& cfg) noexcept {
  if (!cfg.color_primaries && !cfg.transfer_characteristics) return false;
  const auto& info = image.source_info;
  return image.sample_representation == SampleRepresentation::ieee_half_float || !info ||
      (cfg.color_primaries && info->color_primaries != cfg.color_primaries) ||
      (cfg.transfer_characteristics && info->transfer_characteristics != cfg.transfer_characteristics);
}

bool avif_lossless_passthrough_allowed(const AppConfig& cfg,
                                       const fs::path& path) {
  return !cfg.avif_layers.enabled() && avif_lossless_requested(cfg) && avif_lossless_passthrough_source(path) &&
         cfg.avif_encoder == AvifEncoderMode::automatic && cfg.chroma_mode == ChromaMode::auto_keep &&
         cfg.avif_color_representation != AvifColorRepresentation::rgb_identity &&
         !cfg.bit_depth && cfg.alpha_policy != AlphaModePolicy::off && !cfg.strip_metadata &&
         !has_user_color_settings(cfg);
}

bool jxl_jpeg_bitstream_transcode_allowed(const AppConfig& cfg,
                                          const fs::path& path) {
  return cfg.jxl_jpeg_lossless && cfg.output_format == OutputFormat::jxl && cfg.bit_depth == 8 &&
         jxl_jpeg_bitstream_source(path) && !cfg.strip_metadata && !has_user_color_settings(cfg);
}

bool encoder_supports_alpha(AvifEncoderMode mode) noexcept {
  return mode == AvifEncoderMode::aom;
}

bool encoder_supports_alpha(OutputFormat format) noexcept {
  return format == OutputFormat::png || format == OutputFormat::jxl ||
         format == OutputFormat::webp;
}

bool alpha_must_be_preserved(AlphaModePolicy policy,
                             bool source_has_alpha_channel,
                             bool has_non_opaque_alpha) noexcept {
  return source_has_alpha_channel &&
         (policy == AlphaModePolicy::force ||
          (policy == AlphaModePolicy::automatic && has_non_opaque_alpha));
}

std::string applied_alpha_name(bool source_has_alpha_channel,
                               bool preserve_alpha,
                               bool supports_alpha) {
  if (!source_has_alpha_channel) {
    return "none";
  }
  if (preserve_alpha && supports_alpha) {
    return "kept";
  }
  return "stripped";
}

awj::compat::expected<void, std::string> populate_regular_alpha_decision(
    NativeEncodeSettings& settings,
    const ImageBuffer& image,
    const AppConfig& cfg) {
  const auto has_non_opaque_alpha = decoder_common::has_non_opaque_alpha(image,
                                                                         "native encoder");
  if (!has_non_opaque_alpha) {
    return awj::compat::unexpected{has_non_opaque_alpha.error()};
  }
  settings.has_non_opaque_alpha = *has_non_opaque_alpha;
  settings.encoder_supports_alpha = encoder_supports_alpha(cfg.output_format);
  const bool preserve_alpha = settings.source_has_alpha_channel && settings.encoder_supports_alpha &&
                              (cfg.alpha_policy == AlphaModePolicy::force ||
                               (cfg.alpha_policy == AlphaModePolicy::automatic &&
                                *has_non_opaque_alpha));
  settings.applied_alpha = applied_alpha_name(settings.source_has_alpha_channel,
                                              preserve_alpha,
                                              settings.encoder_supports_alpha);
  if (!settings.source_has_alpha_channel) {
    settings.alpha_reason = "源图没有 alpha 通道";
  } else if (cfg.alpha_policy == AlphaModePolicy::off) {
    settings.alpha_reason = "用户请求移除 alpha";
  } else if (cfg.alpha_policy == AlphaModePolicy::automatic && !*has_non_opaque_alpha) {
    settings.alpha_reason = "auto 移除全不透明 alpha";
  } else if (cfg.alpha_policy == AlphaModePolicy::automatic && settings.encoder_supports_alpha) {
    settings.alpha_reason = "auto 保留非不透明 alpha，因为当前编码器支持 alpha";
  } else if (cfg.alpha_policy == AlphaModePolicy::automatic) {
    settings.alpha_reason = "auto 移除 alpha，因为当前编码器不支持 alpha";
  } else if (settings.encoder_supports_alpha) {
    settings.alpha_reason = "force 保留源图 alpha 通道";
  } else {
    settings.alpha_reason = "force 请求保留 alpha，但当前编码器不支持 alpha";
  }
  return {};
}

void populate_source_image_diagnostics(NativeEncodeSettings& settings,
                                       const ImageBuffer& image) {
  settings.source_chroma = source_chroma_name(image);
  settings.source_bit_depth = source_bit_depth(image);
  settings.alpha_policy_name = alpha_mode_policy_name(settings.requested_alpha_policy);
  settings.source_has_alpha_channel = image.alpha_mode != AlphaMode::none;
  settings.source_alpha_mode = alpha_mode_name(image.alpha_mode);
  settings.source_has_icc = image_has_metadata(image, MetadataKind::icc);
  settings.source_has_hdr_metadata = image.source_info ? image.source_info->has_hdr_metadata : false;
  if (image.source_info) {
    settings.source_color_primaries = image.source_info->color_primaries;
    settings.source_transfer_characteristics = image.source_info->transfer_characteristics;
    settings.source_matrix_coefficients = image.source_info->matrix_coefficients;
    settings.source_color_range = image.source_info->color_range;
    settings.source_content_light = image.source_info->content_light;
    if (!image.source_info->color_metadata_source.empty()) {
      settings.color_metadata_source = image.source_info->color_metadata_source;
    }
  }
}

void populate_rendition_diagnostics(NativeEncodeSettings& settings, const ImageBuffer& image, bool hdr) {
  settings.rendition.selected = hdr ? "hdr" : "sdr";
  settings.rendition.origin = rendition_origin_name(image.source_info
      ? image.source_info->rendition_origin : RenditionOrigin::input);
  settings.rendition.reference_chroma = source_chroma_name(image);
  settings.rendition.reference_bit_depth = rendition_reference_depth(image);
  settings.rendition.reused_sdr = !hdr && image.source_info && image.source_info->source_has_gain_map;
}

void populate_regular_color_decision(NativeEncodeSettings& settings, const AppConfig& cfg) {
  settings.applied_color_primaries = choose_color_value(cfg.color_primaries, settings.source_color_primaries);
  settings.applied_transfer_characteristics = choose_color_value(cfg.transfer_characteristics, settings.source_transfer_characteristics);
  settings.applied_matrix_coefficients = choose_color_value(cfg.matrix_coefficients, settings.source_matrix_coefficients);
  settings.applied_color_range = choose_color_value(cfg.color_range, settings.source_color_range);
  const bool overridden = cfg.color_primaries || cfg.transfer_characteristics;
  settings.applied_icc = settings.source_has_icc ? (overridden ? "not-written" : "kept") : "none";
  settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? (cfg.strip_metadata ? "kept-essential-color" : "kept") : "none";
  settings.color_metadata_source = overridden ? "user-cicp-settings" : settings.source_has_icc ? "source-icc" : "source-cicp";
  settings.color_reason = cfg.strip_metadata
      ? "移除可选元数据，保留解释所选呈现所必需的 ICC/CICP"
      : "继承所选呈现的色彩解释，不因 SDR/HDR 改变编码精度";
}

void populate_jxl_jpeg_bitstream_source_diagnostics(NativeEncodeSettings& settings,
                                                    std::span<const std::byte> jpeg_bytes,
                                                    const AppConfig& cfg) {
  const auto source = inspect_jpeg_bitstream_source(jpeg_bytes);
  settings.source_chroma = chroma_name_from_pixel_format(source.pixel_format);
  settings.source_bit_depth = source.bit_depth;
  settings.alpha_policy_name = alpha_mode_policy_name(settings.requested_alpha_policy);
  settings.source_has_alpha_channel = false;
  settings.source_alpha_mode = alpha_mode_name(AlphaMode::none);
  settings.has_non_opaque_alpha = false;
  settings.encoder_supports_alpha = encoder_supports_alpha(cfg.output_format);
  settings.applied_alpha = applied_alpha_name(false, false, settings.encoder_supports_alpha);
  settings.alpha_reason = "源图没有 alpha 通道";
  settings.source_has_icc = source.has_icc;
  settings.source_has_hdr_metadata = false;
  populate_regular_color_decision(settings, cfg);
  settings.chroma_reason = "无损转封装保留 JPEG 码流 chroma";
  settings.bit_depth_reason = "无损转封装保留 JPEG 码流 bit-depth";
  settings.color_metadata_source = settings.source_has_icc ? "source-icc" : "jpeg-bitstream";
  settings.color_reason = settings.source_has_icc
                              ? "无损转封装保留 JPEG 码流 ICC profile"
                              : "无损转封装保留 JPEG 码流元数据";
}

void populate_source_diagnostics(NativeEncodeSettings& settings,
                                 const ImageBuffer& image,
                                 AvifEncoderMode user_encoder) {
  settings.user_encoder_id = avif_encoder_mode_name(user_encoder);
  settings.user_chroma = chroma_mode_name(settings.requested_chroma_mode);
  populate_source_image_diagnostics(settings, image);
}

bool source_hdr_content_light_kept(const NativeEncodeSettings& settings,
                                   bool strip_metadata,
                                   bool user_color_settings) noexcept {
  return settings.source_content_light && !strip_metadata && !user_color_settings;
}

bool source_hdr_cicp_kept(const NativeEncodeSettings& settings,
                          bool strip_metadata,
                          bool user_color_settings) noexcept {
  return !strip_metadata && !user_color_settings &&
         (settings.source_color_primaries || settings.source_transfer_characteristics ||
          settings.source_matrix_coefficients || settings.source_color_range);
}

std::string applied_hdr_metadata_name(const NativeEncodeSettings& settings,
                                      bool strip_metadata,
                                      bool user_color_settings) {
  if (!settings.source_has_hdr_metadata) {
    return "none";
  }
  if (strip_metadata) {
    return "kept-essential-color";
  }
  if (source_hdr_content_light_kept(settings, strip_metadata, user_color_settings) ||
      source_hdr_cicp_kept(settings, strip_metadata, user_color_settings)) {
    return "kept";
  }
  return "not-written";
}

std::string applied_hdr_metadata_name(const NativeEncodeSettings& settings,
                                      const AppConfig& cfg) {
  return applied_hdr_metadata_name(settings, cfg.strip_metadata, has_user_color_settings(cfg));
}

void populate_color_decision(NativeEncodeSettings& settings, const AppConfig& cfg) {
  settings.applied_color_primaries = choose_color_value(cfg.color_primaries,
                                                        settings.source_color_primaries);
  settings.applied_transfer_characteristics = choose_color_value(cfg.transfer_characteristics,
                                                                 settings.source_transfer_characteristics);
  settings.applied_matrix_coefficients = choose_color_value(cfg.matrix_coefficients,
                                                            settings.source_matrix_coefficients);
  settings.applied_color_range = choose_color_value(cfg.color_range,
                                                    settings.source_color_range);

  const bool user_color_settings = has_user_color_settings(cfg);
  settings.applied_icc = settings.source_has_icc
                             ? (cfg.color_primaries || cfg.transfer_characteristics ? "not-written" : "kept")
                             : "none";
  settings.applied_hdr_metadata = applied_hdr_metadata_name(settings, cfg);
  if (user_color_settings) {
    settings.color_metadata_source = "user-cicp-settings";
    settings.color_reason = "用户 color/HDR 设置覆盖源图元数据";
  } else if (settings.source_has_icc) {
    settings.color_metadata_source = "source-icc";
    settings.color_reason = "使用源图 ICC profile 写入编码器元数据";
  } else if (settings.applied_color_primaries || settings.applied_transfer_characteristics ||
             settings.applied_matrix_coefficients || settings.applied_color_range) {
    if (settings.color_metadata_source.empty()) {
      settings.color_metadata_source = "source-cicp";
    }
    settings.color_reason = "使用源图 CICP 字段写入编码器设置";
  } else {
    settings.color_metadata_source = "encoder-default";
    settings.color_reason = "源图色彩元数据未知，使用编码器默认值";
  }
}

void populate_applied_avif_color_diagnostics(NativeEncodeSettings& settings,
                                             const AppConfig& cfg,
                                             ChromaMode chroma,
                                             bool lossless) {




  const bool user_cicp_settings = has_user_cicp_settings(cfg);
  if (user_cicp_settings && settings.color_metadata_source == "user-cicp-settings") {
    settings.color_metadata_source = "user-cicp-settings";
    settings.color_reason = cfg.strip_metadata
                                ? "已移除源图元数据，并使用用户 CICP 设置"
                                : "用户 CICP 设置覆盖源图元数据";
  }

  settings.applied_color_primaries = cfg.color_primaries
                                          ? cfg.color_primaries
                                          : non_unspecified_color_value(
                                                settings.applied_color_primaries, 2);
  settings.applied_transfer_characteristics = cfg.transfer_characteristics
                                                 ? cfg.transfer_characteristics
                                                 : non_unspecified_color_value(
                                                       settings.applied_transfer_characteristics, 2);
  settings.applied_matrix_coefficients = cfg.matrix_coefficients
                                            ? cfg.matrix_coefficients
                                            : non_unspecified_color_value(
                                                  settings.applied_matrix_coefficients, 2);
  settings.applied_color_range = settings.applied_color_range.value_or(1);

  if (!settings.applied_color_primaries && !lossless && settings.applied_icc != "kept") {
    settings.applied_color_primaries = 1;
  }
  if (!settings.applied_transfer_characteristics && !lossless && settings.applied_icc != "kept") {
    settings.applied_transfer_characteristics = 13;
  }
  if (settings.avif_color_representation ==
      AvifColorRepresentation::rgb_identity) {
    settings.applied_matrix_coefficients = 0;
    settings.applied_color_range = 1;
    settings.color_metadata_source = "aom-rgb-identity";
    settings.color_reason = "RGB(A)/GBR(A) Identity 强制使用 identity matrix 与 4:4:4";
  } else if (!settings.applied_matrix_coefficients ||
             *settings.applied_matrix_coefficients == 0 ||
             *settings.applied_matrix_coefficients == 2) {
    const bool bt2020 = settings.source_color_primaries == 9 ||
                        settings.source_matrix_coefficients == 9;
    settings.applied_matrix_coefficients = bt2020 ? 9 : 1;
    settings.color_metadata_source = bt2020 ? "source-bt2020-ncl" : "yuv-bt709-fallback";
    settings.color_reason = bt2020
                                ? "YUV 颜色表示将 Identity/未知 matrix 回退为 BT.2020 NCL"
                                : "YUV 颜色表示将 Identity/未知 matrix 回退为 BT.709";
  }

  if (settings.avif_color_representation == AvifColorRepresentation::yuv &&
      lossless && chroma == ChromaMode::yuv444 && !cfg.matrix_coefficients &&
      !settings.source_matrix_coefficients) {
    settings.color_metadata_source = "yuv-bt709-fallback";
    settings.color_reason = "无损 YUV 4:4:4 使用 BT.709 matrix，不隐式切换 Identity";
  } else if (settings.color_metadata_source == "stripped" &&
             (settings.applied_color_primaries || settings.applied_transfer_characteristics ||
              settings.applied_matrix_coefficients || settings.applied_color_range)) {
    settings.color_metadata_source = user_cicp_settings ? "user-cicp-settings" : "aom-encoder-default";
    settings.color_reason = user_cicp_settings
                                ? "已移除源图元数据，并使用用户 CICP 设置"
                                : "已移除源图元数据，并使用 AOM/libavif 默认值";
  } else if (settings.color_metadata_source == "encoder-default") {
    settings.color_metadata_source = "aom-encoder-default";
    settings.color_reason = "源图色彩元数据未知，使用 AOM/libavif 默认值";
  }
}

std::atomic<std::uint64_t> output_temp_counter{};

fs::path make_output_temp_path(const fs::path& target) {
  const auto parent = target.parent_path();
  const auto id = output_temp_counter.fetch_add(1, std::memory_order_relaxed);
  return parent / awj::compat::format(L".awj-output-{}-{}.tmp", current_process_id(), id);
}

awj::compat::expected<void, std::string> write_output_bytes(const fs::path& path,
                                                    std::span<const std::byte> bytes,
                                                    bool replace_existing,
                                                    awj::compat::stop_token stop_token = {}) {
  try {
    if (stop_token.stop_requested()) {
      return awj::compat::unexpected{"任务已取消。"};
    }
    if (bytes.empty()) {
      return awj::compat::unexpected{awj::compat::format("输出内容为空，无法写入输出文件: {}", display_path_for_user(path))};
    }
    if (bytes.size() > encoding_defaults::effective_max_input_file_bytes()) {
      return awj::compat::unexpected{awj::compat::format("输出内容超过当前运行时上限，无法写入输出文件: {}",
                                        display_path_for_user(path))};
    }
    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        return awj::compat::unexpected{awj::compat::format("无法创建输出目录 {}: {}",
                                           display_path_for_user(parent), ec.message())};
      }
    }
    if (stop_token.stop_requested()) {
      return awj::compat::unexpected{"任务已取消。"};
    }
    std::optional<fs::path> temp_path;
    for (int attempt = 0;
         attempt < encoding_defaults::max_output_temp_path_attempts;
         ++attempt) {
      auto candidate = make_output_temp_path(path);
      if (normalized_lower_path_key(candidate) == normalized_lower_path_key(path)) {
        continue;
      }
      if (auto written = write_file_bytes_exclusive(candidate, bytes); written) {
        temp_path = std::move(candidate);
        break;
      } else {
        auto failure = std::move(written.error());
        if (!output_temp_name_collision(failure.create_error)) {
          return awj::compat::unexpected{std::move(failure.message)};
        }
      }
    }
    if (!temp_path) {
      return awj::compat::unexpected{awj::compat::format("无法创建唯一临时输出路径: {}",
                                         display_path_for_user(path))};
    }
    TempOutputFile temp{*temp_path};
    if (stop_token.stop_requested()) {
      return awj::compat::unexpected{"任务已取消。"};
    }
    if (auto attributes = clear_transient_file_attributes(*temp_path, "临时输出文件"); !attributes) {
      return awj::compat::unexpected{attributes.error()};
    }
    if (auto committed = commit_temp_output(*temp_path, path, replace_existing); !committed) {
      return awj::compat::unexpected{committed.error()};
    }
    temp.release();
    return {};
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"写入输出文件时内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"写入输出文件路径超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return awj::compat::unexpected{"写入输出文件时文件系统访问失败。"};
  }
}

awj::compat::expected<std::uintmax_t, std::string> copy_file_to_output(const fs::path& source,
                                                               const fs::path& path,
                                                               bool replace_existing,
                                                               awj::compat::stop_token stop_token = {}) {
  try {
    if (stop_token.stop_requested()) {
      return awj::compat::unexpected{"任务已取消。"};
    }
    std::error_code source_size_error;
    const auto source_size = fs::file_size(source, source_size_error);
    if (source_size_error) {
      return awj::compat::unexpected{awj::compat::format("读取 AVIF 直通输入文件大小失败 {}: {}",
                                         display_path_for_user(source),
                                         source_size_error.message())};
    }
    if (source_size == 0) {
      return awj::compat::unexpected{awj::compat::format("AVIF 直通输入文件为空: {}",
                                         display_path_for_user(source))};
    }
    if (source_size > encoding_defaults::effective_max_input_file_bytes()) {
      return awj::compat::unexpected{awj::compat::format("AVIF 直通输入超过当前输入上限: {}",
                                         display_path_for_user(source))};
    }
    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        return awj::compat::unexpected{awj::compat::format("无法创建输出目录 {}: {}",
                                           display_path_for_user(parent), ec.message())};
      }
    }
    if (stop_token.stop_requested()) {
      return awj::compat::unexpected{"任务已取消。"};
    }
    std::uintmax_t copied_bytes = 0;
    std::optional<fs::path> temp_path;
    std::optional<TempOutputFile> temp;
    {
#ifdef _WIN32
      const HANDLE raw_source = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                           nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                           nullptr);
      if (raw_source == INVALID_HANDLE_VALUE) {
        return awj::compat::unexpected{awj::compat::format("无法读取 AVIF 直通输入文件 {}: {}",
                                           display_path_for_user(source),
                                           win32_error_message(GetLastError()))};
      }
#else
      const int raw_source = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
      if (raw_source < 0) {
        return awj::compat::unexpected{awj::compat::format("无法读取 AVIF 直通输入文件 {}: {}",
                                           display_path_for_user(source),
                                           posix_error_message(errno))};
      }
#endif
      UniqueHandle source_file{raw_source};

      UniqueHandle output_file;
      for (int attempt = 0;
           attempt < encoding_defaults::max_output_temp_path_attempts;
           ++attempt) {
        auto candidate = make_output_temp_path(path);
        if (normalized_lower_path_key(candidate) == normalized_lower_path_key(path)) {
          continue;
        }
#ifdef _WIN32
        const HANDLE raw_output = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                             CREATE_NEW,
                                             FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                             nullptr);
        if (raw_output == INVALID_HANDLE_VALUE) {
          const auto error = GetLastError();
          if (output_temp_name_collision(static_cast<int>(error))) {
            continue;
          }
          return awj::compat::unexpected{awj::compat::format("无法创建临时输出文件 {}: {}",
                                             display_path_for_user(candidate),
                                             win32_error_message(error))};
        }
#else
        const int raw_output = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (raw_output < 0) {
          const int error = errno;
          if (output_temp_name_collision(error)) {
            continue;
          }
          return awj::compat::unexpected{awj::compat::format("无法创建临时输出文件 {}: {}",
                                             display_path_for_user(candidate),
                                             posix_error_message(error))};
        }
#endif
        output_file.reset(raw_output);
        temp_path = std::move(candidate);
        temp.emplace(*temp_path);
        break;
      }
      if (!temp_path || !output_file) {
        return awj::compat::unexpected{awj::compat::format("无法创建唯一临时输出路径: {}",
                                           display_path_for_user(path))};
      }

      std::vector<std::byte> buffer(encoding_defaults::output_copy_buffer_bytes);
      while (true) {
        if (stop_token.stop_requested()) {
          return awj::compat::unexpected{"任务已取消。"};
        }
#ifdef _WIN32
        DWORD read = 0;
        if (!ReadFile(source_file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
          return awj::compat::unexpected{awj::compat::format("读取 AVIF 直通输入文件失败 {}: {}",
                                             display_path_for_user(source),
                                             win32_error_message(GetLastError()))};
        }
#else
        const auto read = ::read(source_file.get(), buffer.data(), buffer.size());
        if (read < 0) {
          if (errno == EINTR) {
            continue;
          }
          return awj::compat::unexpected{awj::compat::format("读取 AVIF 直通输入文件失败 {}: {}",
                                             display_path_for_user(source),
                                             posix_error_message(errno))};
        }
#endif
        if (read == 0) {
          break;
        }
        if (!encoding_defaults::byte_sum_fits(copied_bytes, static_cast<std::uintmax_t>(read),
                                             encoding_defaults::effective_max_input_file_bytes())) {
          return awj::compat::unexpected{awj::compat::format("AVIF 直通输入超过当前输入上限: {}",
                                             display_path_for_user(source))};
        }
        const auto* cursor = reinterpret_cast<const std::uint8_t*>(buffer.data());
#ifdef _WIN32
        DWORD remaining = read;
#else
        auto remaining = static_cast<std::size_t>(read);
#endif
        while (remaining > 0) {
#ifdef _WIN32
          DWORD written = 0;
          if (!WriteFile(output_file.get(), cursor, remaining, &written, nullptr)) {
            return awj::compat::unexpected{awj::compat::format("写入临时输出文件失败 {}: {}",
                                               display_path_for_user(*temp_path),
                                               win32_error_message(GetLastError()))};
          }
#else
          const auto written = ::write(output_file.get(), cursor, remaining);
          if (written < 0) {
            if (errno == EINTR) {
              continue;
            }
            return awj::compat::unexpected{awj::compat::format("写入临时输出文件失败 {}: {}",
                                               display_path_for_user(*temp_path),
                                               posix_error_message(errno))};
          }
#endif
          if (written == 0) {
            return awj::compat::unexpected{awj::compat::format("写入临时输出文件失败 {}", display_path_for_user(*temp_path))};
          }
          cursor += written;
          remaining -= written;
        }
        copied_bytes += static_cast<std::uintmax_t>(read);
      }
      if (copied_bytes == 0) {
        return awj::compat::unexpected{awj::compat::format("AVIF 直通输入文件为空: {}", display_path_for_user(source))};
      }
#ifdef _WIN32
      if (!FlushFileBuffers(output_file.get())) {
        return awj::compat::unexpected{awj::compat::format("刷新临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           win32_error_message(GetLastError()))};
      }
      const HANDLE output_handle = output_file.get();
      if (!CloseHandle(output_handle)) {
        return awj::compat::unexpected{awj::compat::format("关闭临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           win32_error_message(GetLastError()))};
      }
#else
      const int output_handle = output_file.get();
      if (::fsync(output_handle) != 0) {
        return awj::compat::unexpected{awj::compat::format("刷新临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           posix_error_message(errno))};
      }
      if (::close(output_handle) != 0) {
        return awj::compat::unexpected{awj::compat::format("关闭临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           posix_error_message(errno))};
      }
#endif
      output_file.release();
    }

    if (stop_token.stop_requested()) {
      return awj::compat::unexpected{"任务已取消。"};
    }
    if (auto attributes = clear_transient_file_attributes(*temp_path, "临时输出文件"); !attributes) {
      return awj::compat::unexpected{attributes.error()};
    }
    if (auto committed = commit_temp_output(*temp_path, path, replace_existing); !committed) {
      return awj::compat::unexpected{committed.error()};
    }
    temp->release();
    return copied_bytes;
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"复制输出文件时内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"复制输出文件路径超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return awj::compat::unexpected{"复制输出文件时文件系统访问失败。"};
  }
}

NativeEncodeSettings settings_from_config(const AppConfig& cfg, ResourcePlan resources) {
  return NativeEncodeSettings{.output_format = cfg.output_format,
                              .quality = cfg.quality,
                              .visual_quality = cfg.visual_quality,
                              .speed = cfg.speed.value_or(default_speed_for(cfg.output_format)),
                              .speed_explicit = cfg.speed.has_value(),
                              .bit_depth = cfg.bit_depth,
                              .bit_depth_explicit = cfg.bit_depth.has_value(),
                              .chroma_mode = cfg.chroma_mode,
                              .avif_color_representation = cfg.avif_color_representation,
                              .avif_encoder = cfg.avif_encoder,
                              .alpha_policy = cfg.alpha_policy,
                              .requested_chroma_mode = cfg.chroma_mode,
                              .requested_avif_color_representation =
                                  cfg.avif_color_representation,
                              .requested_avif_encoder = cfg.avif_encoder,
                              .requested_alpha_policy = cfg.alpha_policy,
                              .requested_bit_depth = cfg.bit_depth,
                              .strip_metadata = cfg.strip_metadata,
                              .visual_quality_fallback = cfg.visual_quality_fallback,
                              .jxl_jpeg_lossless_candidate = false,
                              .avif_tune_iq = encoding_defaults::default_avif_tune_iq,
                              .avif_animation_tune = cfg.avif_animation_tune,
                              .avif_animation_keyframe = cfg.avif_animation_keyframe,
                              .avif_layers = cfg.avif_layers,
                              .jpegli_progressive_level = cfg.jpegli_progressive_level,
                              .jpegli_optimize_huffman = cfg.jpegli_optimize_huffman,
                              .jpegli_xyb = cfg.jpegli_xyb,
                              .resources = resources};
}

void copy_native_result(const NativeEncodeResult& native, EncodeResult& result) {
  const bool has_speed_mapping = !native.diagnostics.speed_mapping.codec_key.empty();
  result.output_bytes = native.encoded.bytes.size();
  result.final_encoder_quality = native.final_quality;
  result.visual_quality_target_met = native.visual_quality_target_met;
  result.search_attempt_count = native.search_attempt_count;
  result.lossless = native.lossless;
  result.speed = has_speed_mapping ? native.diagnostics.speed_mapping.user_speed : -1;
  result.decoder_id = native.diagnostics.decoder_id;
  result.encoder_id = native.diagnostics.encoder_id;
  result.requested_encoder_id = native.diagnostics.requested_encoder_id;
  result.user_encoder_id = native.diagnostics.user_encoder_id;
  result.user_chroma = native.diagnostics.user_chroma;
  result.source_chroma = native.diagnostics.source_chroma;
  result.requested_chroma = native.diagnostics.requested_chroma;
  result.applied_chroma = native.diagnostics.applied_chroma;
  result.chroma_reason = native.diagnostics.chroma_reason;
  result.requested_color_representation =
      native.diagnostics.requested_color_representation;
  result.applied_color_representation =
      native.diagnostics.applied_color_representation;
  result.source_bit_depth = native.diagnostics.source_bit_depth;
  result.requested_bit_depth = native.diagnostics.requested_bit_depth;
  result.applied_bit_depth = native.diagnostics.applied_bit_depth;
  result.bit_depth_reason = native.diagnostics.bit_depth_reason;
  result.alpha_policy = native.diagnostics.alpha_policy;
  result.source_has_alpha_channel = native.diagnostics.source_has_alpha_channel;
  result.source_alpha_mode = native.diagnostics.source_alpha_mode;
  result.has_non_opaque_alpha = native.diagnostics.has_non_opaque_alpha;
  result.encoder_supports_alpha = native.diagnostics.encoder_supports_alpha;
  result.applied_alpha = native.diagnostics.applied_alpha;
  result.alpha_reason = native.diagnostics.alpha_reason;
  result.source_color_primaries = native.diagnostics.source_color_primaries;
  result.source_transfer_characteristics = native.diagnostics.source_transfer_characteristics;
  result.source_matrix_coefficients = native.diagnostics.source_matrix_coefficients;
  result.source_color_range = native.diagnostics.source_color_range;
  result.applied_color_primaries = native.diagnostics.applied_color_primaries;
  result.applied_transfer_characteristics = native.diagnostics.applied_transfer_characteristics;
  result.applied_matrix_coefficients = native.diagnostics.applied_matrix_coefficients;
  result.applied_color_range = native.diagnostics.applied_color_range;
  result.source_has_icc = native.diagnostics.source_has_icc;
  result.applied_icc = native.diagnostics.applied_icc;
  result.source_has_hdr_metadata = native.diagnostics.source_has_hdr_metadata;
  result.applied_hdr_metadata = native.diagnostics.applied_hdr_metadata;
  result.color_metadata_source = native.diagnostics.color_metadata_source;
  result.color_reason = native.diagnostics.color_reason;
  result.fallback_reason = native.diagnostics.fallback_reason;
  result.used_decoder_fallback = native.diagnostics.used_decoder_fallback;
  result.visual_quality_search_trace = native.diagnostics.visual_quality_search_trace;
  result.encoder_license = native.diagnostics.encoder_license;
  result.integration_mode = native.diagnostics.integration_mode;
  result.speed_parameter_kind = has_speed_mapping ? native.diagnostics.speed_mapping.codec_key : std::string{};
  result.applied_speed = has_speed_mapping ? native.diagnostics.speed_mapping.codec_value : -1;
  result.encoder_threads = native.diagnostics.encoder_threads;
  result.avif_layer_count = native.diagnostics.avif_layer_count;
  result.avif_layer_trace = native.diagnostics.avif_layer_trace;
  result.memory_budget_bytes = native.diagnostics.memory_budget_bytes;
  result.rendition = native.diagnostics.rendition;
  result.gain_map_compute_seconds = native.diagnostics.timing.gain_map_compute_seconds;
  result.gain_map_compute_workers = native.diagnostics.timing.gain_map_compute_workers;
  result.color_convert_seconds = native.diagnostics.timing.color_convert_seconds;
  result.decode_seconds = native.diagnostics.timing.decode_seconds;
  result.resize_seconds = native.diagnostics.timing.resize_seconds;
  result.resize_workers = native.diagnostics.timing.resize_workers;
  result.prepare_seconds = native.diagnostics.timing.prepare_seconds;
  result.encode_seconds = native.diagnostics.timing.encode_seconds;
  result.avif_rgb_to_yuv_seconds =
      native.diagnostics.timing.avif_rgb_to_yuv_seconds;
  result.avif_add_image_seconds =
      native.diagnostics.timing.avif_add_image_seconds;
  result.avif_finish_seconds = native.diagnostics.timing.avif_finish_seconds;
  result.avif_output_copy_seconds =
      native.diagnostics.timing.avif_output_copy_seconds;
  result.write_seconds = native.diagnostics.timing.write_seconds;
  result.visual_quality_search_seconds = native.diagnostics.timing.visual_quality_search_seconds;
  result.visual_quality_candidate_encode_seconds = native.diagnostics.timing.visual_quality_candidate_encode_seconds;
  result.visual_quality_candidate_decode_seconds = native.diagnostics.timing.visual_quality_candidate_decode_seconds;
  result.visual_quality_candidate_io_seconds = native.diagnostics.timing.visual_quality_candidate_io_seconds;
  result.visual_quality_luma_seconds = native.diagnostics.timing.visual_quality_luma_seconds;
  result.gmsd_seconds = native.diagnostics.timing.gmsd_seconds;
  result.ms_ssim_seconds = native.diagnostics.timing.ms_ssim_seconds;
  result.visual_quality_metric_seconds = native.diagnostics.timing.visual_quality_metric_seconds;
  result.visual_quality_candidate_count = native.diagnostics.timing.visual_quality_candidate_count;
  result.visual_quality_decode_memory_fallback_count = native.diagnostics.timing.visual_quality_decode_memory_fallback_count;
  if (native.visual_score) {
    result.visual_score = native.visual_score->visual_score;
    result.gmsd_quality_score = native.visual_score->gmsd_quality_score;
    result.msssim_quality_score = native.visual_score->msssim_quality_score;
  }
  result.raw_gmsd = native.raw_gmsd;
  result.raw_ms_ssim = native.raw_ms_ssim;
  result.gmsd_weight = GMSD_WEIGHT;
  result.msssim_weight = MSSSIM_WEIGHT;
  const auto format_name = result.output_format.empty() ? std::string{"native"}
                                                        : result.output_format;
  result.command = awj::compat::format("native:{}:{}:{} q{} speed={}",
                               format_name,
                               result.decoder_id,
                               native.diagnostics.encoder_id,
                               native.final_quality,
                               has_speed_mapping
                                   ? std::to_string(native.diagnostics.speed_mapping.user_speed)
                                   : std::string{"n/a"});
  if (native.diagnostics.encoder_id == "jpegli") {
    result.command += awj::compat::format(" chroma={} bit-depth={} progressive={} huffman={} xyb={}",
                                  result.applied_chroma.empty() ? "auto" : result.applied_chroma,
                                  result.applied_bit_depth ? awj::compat::format("{}", *result.applied_bit_depth) : std::string{"8"},
                                  native.diagnostics.jpegli_progressive_level,
                                  native.diagnostics.jpegli_optimize_huffman ? "optimized" : "fixed",
                                  native.diagnostics.jpegli_xyb ? "on" : "off");
  }
}

void copy_settings_diagnostics(const NativeEncodeSettings& settings, EncodeResult& result);

void populate_avif_passthrough_diagnostics(const ImageBuffer& image,
                                           AvifEncoderMode requested_encoder,
                                           AvifColorRepresentation requested_representation,
                                           EncodeResult& result) {
  NativeEncodeSettings settings{};
  settings.requested_avif_encoder = requested_encoder;
  settings.requested_avif_color_representation = requested_representation;
  settings.avif_color_representation = AvifColorRepresentation::yuv;
  settings.requested_chroma_mode = ChromaMode::auto_keep;
  settings.chroma_mode = native_backend_detail::lossless_source_chroma(image);
  settings.requested_alpha_policy = AlphaModePolicy::automatic;
  settings.requested_bit_depth = source_bit_depth(image);
  settings.bit_depth = settings.requested_bit_depth;
  populate_source_diagnostics(settings, image, requested_encoder);
  if (image.source_info && image.source_info->color_metadata_source == "source-icc") {
    settings.source_has_icc = true;
  }
  settings.avif_encoder = AvifEncoderMode::aom;
  settings.applied_icc = settings.source_has_icc ? "kept" : "none";
  settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? "kept" : "none";
  settings.applied_color_primaries = settings.source_color_primaries;
  settings.applied_transfer_characteristics = settings.source_transfer_characteristics;
  settings.applied_matrix_coefficients = settings.source_matrix_coefficients;
  settings.applied_color_range = settings.source_color_range;
  settings.chroma_reason = "无损直通复制 AVIF 码流 chroma";
  settings.bit_depth_reason = "无损直通复制 AVIF 码流 bit-depth";
  settings.alpha_policy_name = alpha_mode_policy_name(settings.requested_alpha_policy);
  settings.encoder_supports_alpha = true;
  settings.applied_alpha = settings.source_has_alpha_channel ? "kept" : "none";
  settings.alpha_reason = "无损直通复制 AVIF 码流 alpha";
  settings.color_metadata_source = settings.source_has_icc
                                       ? "source-icc"
                                       : (settings.color_metadata_source.empty()
                                              ? "avif-passthrough"
                                              : settings.color_metadata_source);
  settings.color_reason = "无损直通复制 AVIF 码流元数据";
  copy_settings_diagnostics(settings, result);
  result.encoder_id = "avif-passthrough";
  result.requested_encoder_id = avif_encoder_mode_name(requested_encoder);
  result.requested_chroma = chroma_mode_name(ChromaMode::auto_keep);
  result.applied_chroma = settings.chroma_mode == ChromaMode::auto_keep
                              ? "unknown"
                              : chroma_mode_name(settings.chroma_mode);
}

void copy_settings_diagnostics(const NativeEncodeSettings& settings, EncodeResult& result) {
  const auto diagnostics = diagnostics_from_settings(settings);
  result.encoder_id = avif_encoder_mode_name(settings.avif_encoder);
  result.requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder);
  result.user_encoder_id = diagnostics.user_encoder_id;
  result.user_chroma = diagnostics.user_chroma;
  result.source_chroma = diagnostics.source_chroma;
  result.requested_chroma = chroma_mode_name(settings.requested_chroma_mode);
  result.applied_chroma = chroma_mode_name(settings.chroma_mode);
  result.chroma_reason = diagnostics.chroma_reason;
  result.requested_color_representation =
      diagnostics.requested_color_representation;
  result.applied_color_representation =
      diagnostics.applied_color_representation;
  result.source_bit_depth = diagnostics.source_bit_depth;
  result.requested_bit_depth = settings.requested_bit_depth;
  result.applied_bit_depth = settings.bit_depth;
  result.bit_depth_reason = settings.bit_depth_reason;
  result.alpha_policy = diagnostics.alpha_policy;
  result.source_has_alpha_channel = diagnostics.source_has_alpha_channel;
  result.source_alpha_mode = diagnostics.source_alpha_mode;
  result.has_non_opaque_alpha = diagnostics.has_non_opaque_alpha;
  result.encoder_supports_alpha = diagnostics.encoder_supports_alpha;
  result.applied_alpha = diagnostics.applied_alpha;
  result.alpha_reason = diagnostics.alpha_reason;
  result.source_color_primaries = diagnostics.source_color_primaries;
  result.source_transfer_characteristics = diagnostics.source_transfer_characteristics;
  result.source_matrix_coefficients = diagnostics.source_matrix_coefficients;
  result.source_color_range = diagnostics.source_color_range;
  result.applied_color_primaries = diagnostics.applied_color_primaries;
  result.applied_transfer_characteristics = diagnostics.applied_transfer_characteristics;
  result.applied_matrix_coefficients = diagnostics.applied_matrix_coefficients;
  result.applied_color_range = diagnostics.applied_color_range;
  result.source_has_icc = diagnostics.source_has_icc;
  result.applied_icc = diagnostics.applied_icc;
  result.source_has_hdr_metadata = diagnostics.source_has_hdr_metadata;
  result.applied_hdr_metadata = diagnostics.applied_hdr_metadata;
  result.color_metadata_source = diagnostics.color_metadata_source;
  result.color_reason = diagnostics.color_reason;
  result.fallback_reason = settings.encoder_fallback_reason;
  result.speed = settings.speed;
  // 这里只拷贝准备阶段配置。实际线程数由已启动编码器的结果填写。
  result.encoder_threads = 0;
  result.memory_budget_bytes = settings.resources.memory_limit_bytes;
  result.rendition = settings.rendition;
}

// 原输入的已确认事实与后续工作像素、编码器准备参数分开，失败也不能丢失。
void copy_decode_diagnostics(const DecodeDiagnostics& diagnostics, EncodeResult& result) {
  if (!diagnostics.decoder_id.empty()) result.decoder_id = diagnostics.decoder_id;
  result.decode_stage = diagnostics.stage;
  result.source_metadata_available = diagnostics.source_info.has_value();
  result.base_decode_seconds = diagnostics.timing.base_decode_seconds;
  result.auxiliary_decode_seconds = diagnostics.timing.auxiliary_decode_seconds;
  result.gain_map_render_seconds = diagnostics.timing.gain_map_render_seconds;
  result.gain_map_render_workers = diagnostics.timing.gain_map_render_workers;
  result.heif_tile_parallelism = diagnostics.heif_tile_workers;
  result.heif_codec_thread_budget = diagnostics.heif_codec_threads;
  if (!diagnostics.source_info) return;
  const auto& source = *diagnostics.source_info;
  result.source_chroma = chroma_name_from_pixel_format(source.pixel_format);
  result.source_bit_depth = source.bit_depth > 0 ? std::optional<int>{source.bit_depth} : std::nullopt;
  result.source_color_primaries = source.color_primaries;
  result.source_transfer_characteristics = source.transfer_characteristics;
  result.source_matrix_coefficients = source.matrix_coefficients;
  result.source_color_range = source.color_range;
  result.source_has_icc = diagnostics.source_has_icc;
  result.source_has_hdr_metadata = source.has_hdr_metadata;
  result.source_has_alpha_channel = diagnostics.source_has_alpha_channel;
  result.source_alpha_mode = alpha_mode_name(diagnostics.source_alpha_mode);
  if (!source.color_metadata_source.empty()) result.color_metadata_source = source.color_metadata_source;
  if (result.ok && diagnostics.icc_normalized_to_cicp) {
    if (result.applied_icc == "none" || result.applied_icc == "stripped")
      result.applied_icc = "normalized-to-cicp";
    result.color_reason = "源 ICC 已按数学等价 CICP 规范化，保留 Gain Map 原运算空间；" + result.color_reason;
  }
}

}  // namespace native_backend_detail

class NativeBackend final {
  struct EncodeOverrides {
    std::optional<AvifEncoderMode> avif_encoder{};
    std::optional<GridPlan> avif_grid_plan{};
  };

 public:
  // 同步阶段边界申请额度；0 归还额度，返回值为实际授权而非观测到的忙核数。
  using CpuBudgetCallback = std::function<int(int)>;

  NativeBackend(const AppConfig& cfg, FileLogger& logger, ResourcePlan resources,
                std::function<bool(std::uint64_t)> memory_reservation = {},
                CpuBudgetCallback cpu_budget = {})
      : cfg_{cfg}, logger_{logger}, resources_{normalize_memory_policy_resources(cfg, resources)},
        reserve_memory_{cfg.unlock_memory_limit ? std::function<bool(std::uint64_t)>{} : std::move(memory_reservation)},
        cpu_budget_{std::move(cpu_budget)}, memory_policy_{operation_memory_policy(cfg)} {
    const auto checked = checked_memory_policy_resources(cfg, resources_);
    if (checked) resources_ = *checked;
    else resource_plan_error_ = checked.error();
    resources_.encoder_threads_per_file = per_file_thread_budget(resources_);
  }

  EncodeResult encode(const ImageFile& image,
                      awj::compat::stop_token stop_token = {}) const {
    if (!cpu_budget_) return encode_with_requested_file_times(image, EncodeOverrides{}, stop_token);
    // 阶段快照只属于本次同步操作，不修改可被其他调用方复用的 backend。
    auto operation = *this;
    return operation.encode_with_requested_file_times(image, EncodeOverrides{}, stop_token);
  }

  EncodeResult encode_avif_grid(const ImageFile& image,
                                GridPlan plan,
                                awj::compat::stop_token stop_token = {}) const {
    EncodeOverrides overrides{};
    overrides.avif_encoder = AvifEncoderMode::aom;
    overrides.avif_grid_plan = std::move(plan);
    if (!cpu_budget_) return encode_with_requested_file_times(image, std::move(overrides), stop_token);
    auto operation = *this;
    return operation.encode_with_requested_file_times(image, std::move(overrides), stop_token);
  }

  // 内存入口只复用参数决策和编码；不扫描文件、不初始化文件结果、不写出。
  awj::compat::expected<NativeEncodeResult, std::string> encode_memory(
      ImageDecodeResult decoded, awj::compat::stop_token stop_token = {}) const {
    if (!cpu_budget_) return encode_memory_operation(std::move(decoded), stop_token);
    auto operation = *this;
    return operation.encode_memory_operation(std::move(decoded), stop_token);
  }

 private:
  awj::compat::expected<NativeEncodeResult, std::string> encode_memory_operation(
      ImageDecodeResult decoded, awj::compat::stop_token stop_token) const {
    if (!memory_policy_) return awj::compat::unexpected{memory_policy_.error()};
    if (resource_plan_error_) return awj::compat::unexpected{*resource_plan_error_};
    const encoding_defaults::ScopedMemoryPolicy memory_scope{*memory_policy_};
    if (stop_token.stop_requested() || refresh_cpu_budget() < 1)
      return awj::compat::unexpected{"任务已取消。"};
    // 像素已经解码；decode/write 保持未知，不伪造为 0。
    EncodeResult memory_result{};
    {
      native_backend_detail::ScopedStageTimer timer{memory_result.prepare_seconds};
      if (auto selected = native_backend_detail::select_output_rendition(decoded, cfg_, stop_token); !selected)
        return awj::compat::unexpected{selected.error()};
    }
    if (auto limited = native_backend_detail::apply_image_size_limit(decoded, cfg_, stop_token, resources_, memory_result, reserve_memory_); !limited)
      return awj::compat::unexpected{limited.error()};
    const ImageFile source{};
    auto prepared = [&] {
      native_backend_detail::ScopedStageTimer timer{memory_result.prepare_seconds};
      return prepare_encoding(source, decoded, {});
    }();
    if (!prepared) return awj::compat::unexpected{prepared.error().message};
    {
      native_backend_detail::ScopedStageTimer timer{memory_result.prepare_seconds};
      if (!admit_encoding_memory(memory_result, decoded, prepared->settings, 0))
        return awj::compat::unexpected{memory_result.message};
    }
    // 路径形参在候选搜索中仅用作输出码流的格式提示，不是输入或候选文件。
    const auto format_hint = fs::path{std::wstring{L"memory"} + output_extension_for(cfg_)};
    auto encoded = [&] {
      native_backend_detail::ScopedStageTimer timer{memory_result.encode_seconds};
      return execute_encode(decoded, prepared->encoder.get(), prepared->settings, format_hint, stop_token, memory_result, 0);
    }();
    if (encoded) native_backend_detail::merge_stage_timing(*encoded, memory_result);
    return encoded;
  }

 private:
  using EncodeStartedAt = std::chrono::steady_clock::time_point;

  struct DecodedInput {
    ImageDecodeResult decoded{};
    bool decoder_used_fallback{};
  };

  struct PreparedEncoding {
    NativeEncodeSettings settings{};
    std::unique_ptr<ImageEncoder> encoder{};
    std::string avif_bit_depth_reason{};
  };

  struct PrepareError {
    std::string message{};
    NativeEncodeSettings settings{};
  };

  static awj::compat::unexpected<PrepareError> prepare_failed(std::string message,
                                                       const NativeEncodeSettings& settings) {
    return awj::compat::unexpected{PrepareError{.message = std::move(message), .settings = settings}};
  }

  [[nodiscard]] EncodeResult initialize_result(const ImageFile& image) const {
    const auto planned_output_path = output_path_for(cfg_, image);
    auto output_path = image.output_path_resolved
                           ? awj::compat::expected<fs::path, std::string>{planned_output_path}
                           : resolve_collision_output_path(
                                 planned_output_path, cfg_.collision_mode, nullptr,
                                 output_extension_for(cfg_));
    auto result = EncodeResult{.index = image.index,
                               .input_path = image.path,
                               .output_path = output_path ? *output_path : planned_output_path,
                               .output_format = output_format_name(cfg_.output_format),
                               .original_bytes = image.bytes,
                               .quality = cfg_.quality,
                               .requested_visual_quality = cfg_.visual_quality,
                               .gmsd_weight = GMSD_WEIGHT,
                               .msssim_weight = MSSSIM_WEIGHT,
                               .final_encoder_quality = cfg_.quality,
                               .speed = cfg_.speed.value_or(default_speed_for(cfg_.output_format)),
                               .quality_overridden_by_visual_quality = cfg_.visual_quality.has_value()};
    if (!output_path) {
      mark_failed(result, output_path.error());
    }
    return result;
  }

  static EncodeResult failed_encode_result(const ImageFile& image, std::string_view message) {
    return EncodeResult{.index = image.index,
                        .input_path = image.path,
                        .original_bytes = image.bytes,
                        .processed = true,
                        .ok = false,
                        .message = std::string{message}};
  }

  static void mark_failed(EncodeResult& result, std::string message) {
    result.processed = true;
    result.ok = false;
    result.skipped = false;
    result.resize_workers = 0;
    result.message = std::move(message);
  }

  static bool cancel_if_requested(EncodeResult& result, awj::compat::stop_token stop_token) {
    if (!stop_token.stop_requested()) {
      return false;
    }
    result.canceled = true;
    result.processed = true;
    result.ok = false;
    result.skipped = false;
    result.resize_workers = 0;
    result.message = "任务已取消。";
    return true;
  }

  void append_success_warning(EncodeResult& result, std::string warning) const {
    if (warning.empty()) {
      return;
    }
    result.message = result.message.empty() || result.message == "OK"
                         ? std::move(warning)
                         : awj::compat::format("{}；{}", result.message, warning);
    try {
      logger_.warn(awj::compat::format("native encode warning: item={:04} {}",
                               result.index + 1, result.message));
    } catch (...) {
    }
  }

  EncodeResult encode_with_requested_file_times(
      const ImageFile& image, EncodeOverrides overrides,
      awj::compat::stop_token stop_token) const {
    if (!memory_policy_) return failed_encode_result(image, memory_policy_.error());
    if (resource_plan_error_) return failed_encode_result(image, *resource_plan_error_);
    const encoding_defaults::ScopedMemoryPolicy memory_scope{*memory_policy_};
#ifdef _WIN32
    const bool preserve_any_time = cfg_.preserve_creation_time ||
                                   cfg_.preserve_modification_time ||
                                   cfg_.preserve_access_time;
    std::optional<native_backend_detail::SourceFileTimes> source_times;
    std::string time_warning;
    if (preserve_any_time) {
      auto captured = native_backend_detail::capture_source_file_times(image.path);
      if (captured) {
        source_times = *captured;
      } else {
        time_warning = awj::compat::format("保留文件时间失败：{}", captured.error());
      }
    }
    auto result = encode_with_overrides(image, std::move(overrides), stop_token);
    if (!result.ok || result.skipped || !preserve_any_time) {
      return result;
    }
    if (!time_warning.empty()) {
      append_success_warning(result, std::move(time_warning));
      return result;
    }
    if (auto applied = native_backend_detail::apply_source_file_times(
            result.output_path, *source_times, cfg_.preserve_creation_time,
            cfg_.preserve_modification_time, cfg_.preserve_access_time);
        !applied) {
      append_success_warning(result,
                             awj::compat::format("保留文件时间失败：{}", applied.error()));
    }
    return result;
#else
    return encode_with_overrides(image, std::move(overrides), stop_token);
#endif
  }

  [[nodiscard]] std::optional<EncodeResult> try_avif_lossless_passthrough(
      const ImageFile& image,
      EncodeResult& result,
      EncodeStartedAt started,
      awj::compat::stop_token stop_token) const {
    if (!native_backend_detail::avif_lossless_passthrough_allowed(cfg_, image.path)) {
      return std::nullopt;
    }

    auto container_info = [&] {
      native_backend_detail::ScopedStageTimer timer{result.decode_seconds};
      return parse_avif_container_info(image.path);
    }();
    if (!container_info) {
      return std::nullopt;
    }
    if (limited_dimensions(container_info->width, container_info->height, cfg_)) {
      return std::nullopt;
    }
    if (!container_info->source_info || container_info->source_info->bit_depth < 10 ||
        (container_info->source_info->pixel_format != PixelFormat::yuv420 &&
         container_info->source_info->pixel_format != PixelFormat::yuv422 &&
         container_info->source_info->pixel_format != PixelFormat::yuv444) ||
        container_info->source_info->matrix_coefficients == 0 ||
        container_info->source_info->matrix_coefficients == 2) {
      return std::nullopt;
    }
    if (cancel_if_requested(result, stop_token)) {
      return result;
    }
    result.decode_stage = "avif.parse";
    result.resize_seconds = 0.0;
    result.resize_workers = 0;
    result.encode_seconds = 0.0;  // Confirmed bitstream copy; no encoder was run.
    if (!admit_output_cpu(result)) return result;
    auto copied = [&] {
      native_backend_detail::ScopedStageTimer timer{result.write_seconds};
      return native_backend_detail::copy_file_to_output(
          image.path, result.output_path, cfg_.collision_mode == CollisionMode::overwrite, stop_token);
    }();
    if (!copied) {
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
      } else {
        mark_failed(result, redact_path_for_user(copied.error(), image.path));
      }
      return result;
    }

    result.output_bytes = *copied;
    result.final_encoder_quality = 100;
    result.search_attempt_count = 1;
    result.lossless = true;
    {
      native_backend_detail::ScopedStageTimer timer{result.prepare_seconds};
      native_backend_detail::populate_avif_passthrough_diagnostics(*container_info,
                                                                   cfg_.avif_encoder,
                                                                   cfg_.avif_color_representation,
                                                                   result);
    }
    result.decoder_id = "libavif-parse";
    result.encoder_id = "avif-passthrough";
    result.integration_mode = "avif-lossless-passthrough";
    result.encoder_threads = 1;
    result.memory_budget_bytes = resources_.memory_limit_bytes;
    result.command = "native:AVIF:avif-passthrough lossless";
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    result.processed = true;
    result.ok = true;
    result.message = "OK";
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return awj::compat::format("native avif lossless passthrough ok: item={:04}",
                         result.index + 1);
    });
    return result;
  }

  [[nodiscard]] std::optional<EncodeResult> try_jxl_jpeg_bitstream_transcode(
      const ImageFile& image,
      EncodeResult& result,
      EncodeStartedAt started,
      awj::compat::stop_token stop_token) const {
    if (!native_backend_detail::jxl_jpeg_bitstream_transcode_allowed(cfg_, image.path)) {
      return std::nullopt;
    }

    auto bytes = [&] {
      native_backend_detail::ScopedStageTimer timer{result.decode_seconds};
      return decoder_common::read_file_bytes(image.path, "JPEG");
    }();
    if (!bytes) {
      return std::nullopt;
    }
    const auto source = [&] {
      native_backend_detail::ScopedStageTimer timer{result.decode_seconds};
      return native_backend_detail::inspect_jpeg_bitstream_source(*bytes);
    }();
    if (source.has_mpf || source.width == 0 || source.height == 0 ||
        limited_dimensions(source.width, source.height, cfg_)) {
      return std::nullopt;
    }
    // HDR 和损坏的增益图必须经过常规解码入口，不能用 JPEG 位流转码绕过 ISO 校验。
    const auto gain_map = [&] {
      native_backend_detail::ScopedStageTimer timer{result.decode_seconds};
      return probe_jpeg_gain_map_dimensions(std::span<const std::byte>{*bytes}, stop_token);
    }();
    if (!gain_map || gain_map->has_value()) {
      return std::nullopt;
    }
    if (cancel_if_requested(result, stop_token)) {
      return result;
    }

    result.decoder_id = "jpeg-bitstream";
    result.decode_stage = "jpeg.bitstream";
    result.encoder_id = "libjxl";
    result.integration_mode = "jxl-jpeg-bitstream-transcode";
    result.final_encoder_quality = 100;
    result.lossless = true;
    result.encoder_threads = 0;
    result.memory_budget_bytes = resources_.memory_limit_bytes;
    result.resize_seconds = 0.0;
    result.resize_workers = 0;
    if (refresh_cpu_budget() < 1) {
      cancel_if_requested(result, stop_token);
      return result;
    }
    auto settings = [&] {
      native_backend_detail::ScopedStageTimer timer{result.prepare_seconds};
      auto settings = native_backend_detail::settings_from_config(cfg_, resources_);
      settings.jxl_jpeg_lossless_candidate = true;
      native_backend_detail::populate_jxl_jpeg_bitstream_source_diagnostics(
          settings, std::span<const std::byte>{*bytes}, cfg_);
      return settings;
    }();

    auto encoder = JXLImageEncoder{};
    auto encoded = [&] {
      native_backend_detail::ScopedStageTimer timer{result.encode_seconds};
      return encoder.encode_jpeg_bitstream(std::span<const std::byte>{*bytes}, settings, stop_token);
    }();
    if (!encoded) {
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
        return result;
      }
      native_backend_detail::log_info_noexcept(logger_, [&] {
        return awj::compat::format("native jpeg bitstream transcode unavailable, fallback to pixels: item={:04}",
                           result.index + 1);
      });
      return std::nullopt;
    }

    if (cancel_if_requested(result, stop_token)) {
      return result;
    }

    if (!admit_output_cpu(result)) return result;
    auto written = [&] {
      native_backend_detail::ScopedStageTimer timer{result.write_seconds};
      return native_backend_detail::write_output_bytes(
          result.output_path, std::span<const std::byte>{encoded->encoded.bytes},
          cfg_.collision_mode == CollisionMode::overwrite, stop_token);
    }();
    if (!written) {
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
      } else {
        mark_failed(result, written.error());
      }
      return result;
    }

    encoded->diagnostics.timing.encode_seconds = result.encode_seconds;
    native_backend_detail::merge_stage_timing(*encoded, result);
    native_backend_detail::copy_native_result(*encoded, result);
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    result.processed = true;
    result.ok = true;
    result.message = "OK";
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return awj::compat::format("native jpeg bitstream transcode ok: item={:04}",
                         result.index + 1);
    });
    return result;
  }

  [[nodiscard]] awj::compat::expected<DecodedInput, std::string> decode_input(
      const ImageFile& image, awj::compat::stop_token stop_token, DecodeDiagnostics& diagnostics) const {
    auto decoded = decode_image_for_path(
        image.path,
        DecoderRegistryOptions{.allow_wic_fallback = cfg_.allow_wic_fallback,
                               .decode_threads = per_file_thread_budget(resources_),
                               .stop_token = stop_token,
                               .native_pixels = !cfg_.visual_quality,
                               .gain_map_mode = gain_map_mode_for_output(cfg_.output_format, cfg_.jpeg_hdr),
                               .diagnostics = &diagnostics});
    if (!decoded) {
      return awj::compat::unexpected{redact_path_for_user(decoded.error(), image.path)};
    }

    const bool decoder_used_fallback = decoded->used_fallback;
    return DecodedInput{.decoded = std::move(*decoded),
                        .decoder_used_fallback = decoder_used_fallback};
  }

  [[nodiscard]] awj::compat::expected<PreparedEncoding, PrepareError> prepare_encoding(
      const ImageFile& image,
      const ImageDecodeResult& decoded,
      EncodeOverrides overrides) const {
    PreparedEncoding prepared{};
    prepared.settings = native_backend_detail::settings_from_config(cfg_, resources_);
    // decoded_input 在同步编码及所有已 join 的候选搜索结束前拥有两端图像。
    prepared.settings.gain_map_alternative = decoded.gain_map_alternative ? &*decoded.gain_map_alternative : nullptr;
    const auto hdr_signal = hdr::has_explicit_hdr_signal(decoded.image);
    if (!hdr_signal) return prepare_failed(hdr_signal.error(), prepared.settings);
    const auto depth = resolve_rendition_bit_depth(cfg_.output_format, decoded.image, *hdr_signal, cfg_.bit_depth);
    if (!depth) return prepare_failed(depth.error(), prepared.settings);
    prepared.settings.bit_depth = *depth;
    native_backend_detail::populate_rendition_diagnostics(prepared.settings, decoded.image, *hdr_signal);
    const auto requested_avif_encoder = overrides.avif_encoder.value_or(cfg_.avif_encoder);
    prepared.settings.requested_avif_encoder = requested_avif_encoder;
    prepared.settings.requested_chroma_mode = cfg_.chroma_mode;
    prepared.settings.requested_avif_color_representation =
        cfg_.avif_color_representation;
    prepared.settings.requested_alpha_policy = cfg_.alpha_policy;
    prepared.settings.requested_bit_depth = cfg_.bit_depth;
    if (overrides.avif_grid_plan) {
      prepared.settings.avif_grid_plan = std::move(overrides.avif_grid_plan);
    }
    if (cfg_.output_format == OutputFormat::jxl) {
      prepared.settings.jxl_jpeg_lossless_candidate =
          native_backend_detail::jxl_jpeg_bitstream_transcode_allowed(cfg_, image.path);
    }

    if (cfg_.output_format == OutputFormat::avif) {
      native_backend_detail::populate_source_diagnostics(prepared.settings,
                                                         decoded.image,
                                                         requested_avif_encoder);
      prepared.settings.avif_color_representation =
          native_backend_detail::effective_avif_color_representation(
              cfg_, decoded.image);
      native_backend_detail::populate_color_decision(prepared.settings, cfg_);
      if (decoded.image.width == 0 || decoded.image.height == 0 ||
          decoded.image.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
          decoded.image.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return prepare_failed("AVIF encoder 输入尺寸超过 API 限制。", prepared.settings);
      }
      if (decoded.image.width > std::numeric_limits<std::uint64_t>::max() / decoded.image.height) {
        return prepare_failed("AVIF encoder 输入尺寸过大。", prepared.settings);
      }
      const auto pixel_count = static_cast<std::uint64_t>(decoded.image.width) *
                               static_cast<std::uint64_t>(decoded.image.height);
      if (prepared.settings.avif_grid_plan && manual_image_size_mode(cfg_.image_size_limit.mode)) {
        // A manual limit can resize after preflight; use the actual samples.
        if (decoded.image.width <= aom_large_image_limits.max_width &&
            decoded.image.height <= aom_large_image_limits.max_height &&
            pixel_count <= aom_large_image_limits.max_pixels) {
          prepared.settings.avif_grid_plan.reset();
        } else {
          auto grid = plan_grid(GridPlanRequest{
              .width = static_cast<std::uint32_t>(decoded.image.width),
              .height = static_cast<std::uint32_t>(decoded.image.height),
              .mode = GridMode::auto_grid,
              .clamped_padding_enabled = cfg_.experimental_clamped_grid_padding});
          if (!grid) return prepare_failed(grid.error(), prepared.settings);
          prepared.settings.avif_grid_plan = *grid;
        }
      }
      const auto has_non_opaque_alpha = decoder_common::has_non_opaque_alpha(decoded.image,
                                                                             "AVIF encoder");
      if (!has_non_opaque_alpha) {
        return prepare_failed(has_non_opaque_alpha.error(), prepared.settings);
      }
      prepared.settings.has_non_opaque_alpha = *has_non_opaque_alpha;

      const bool must_preserve_alpha = native_backend_detail::alpha_must_be_preserved(
          cfg_.alpha_policy, prepared.settings.source_has_alpha_channel, *has_non_opaque_alpha);
      const bool requested_avif_lossless =
          native_backend_detail::avif_lossless_requested(cfg_);
      const bool avif_lossless = requested_avif_lossless;

      const bool identity_representation =
          prepared.settings.avif_color_representation ==
          AvifColorRepresentation::rgb_identity;
      if (identity_representation &&
          (cfg_.chroma_mode == ChromaMode::yuv420 ||
           cfg_.chroma_mode == ChromaMode::yuv422 ||
           cfg_.chroma_mode == ChromaMode::yuv400)) {
        return prepare_failed(
            "RGB(A)/GBR(A) Identity AVIF 必须使用 4:4:4；请使用 chroma=auto/444。",
            prepared.settings);
      }

      if (identity_representation && cfg_.matrix_coefficients &&
          *cfg_.matrix_coefficients != 0) {
        return prepare_failed(
            "RGB(A)/GBR(A) Identity 与非 Identity matrix-coefficients 冲突。",
            prepared.settings);
      }
      if (!identity_representation && cfg_.matrix_coefficients &&
          *cfg_.matrix_coefficients == 0) {
        return prepare_failed(
            "YUV 颜色表示不能使用 Identity matrix-coefficients；请选择 "
            "RGB(A)/GBR(A) 或移除该参数。",
            prepared.settings);
      }



      const auto selection_requested_encoder =
          (identity_representation || avif_lossless) &&
                  requested_avif_encoder == AvifEncoderMode::automatic
              ? AvifEncoderMode::aom
              : requested_avif_encoder;
      ChromaMode selection_requested_chroma = ChromaMode::auto_keep;
      if (identity_representation) {
        selection_requested_chroma = ChromaMode::yuv444;
        prepared.settings.chroma_reason =
            "RGB(A)/GBR(A) Identity 强制使用 4:4:4 chroma";
      } else if (cfg_.chroma_mode != ChromaMode::auto_keep) {
        selection_requested_chroma = cfg_.chroma_mode;
        prepared.settings.chroma_reason = "用户请求 chroma";
      } else {
        selection_requested_chroma = native_backend_detail::lossless_source_chroma(decoded.image);
        prepared.settings.chroma_reason = awj::compat::format(
            "chroma auto 参考 avifenc 根据源图选择 {} chroma",
            chroma_mode_name(selection_requested_chroma));
      }

      const std::optional<int> selection_requested_bit_depth = prepared.settings.bit_depth;
      prepared.settings.bit_depth_reason = cfg_.bit_depth ? "用户明确请求 bit-depth"
          : "按所选呈现有效精度自动选择 AVIF 10/12-bit，不按容器原底图降精度";
      // The codec's single-image bounds apply to a Grid cell, not the canvas.
      const auto encode_width = prepared.settings.avif_grid_plan
          ? prepared.settings.avif_grid_plan->tile_width : static_cast<std::uint32_t>(decoded.image.width);
      const auto encode_height = prepared.settings.avif_grid_plan
          ? prepared.settings.avif_grid_plan->tile_height : static_cast<std::uint32_t>(decoded.image.height);
      const auto selection = select_avif_encoder_for_current_build(AvifEncoderSelectionRequest{
          .requested_encoder = selection_requested_encoder,
          .requested_chroma = selection_requested_chroma,
          .requested_bit_depth = selection_requested_bit_depth,
          .requested_bit_depth_explicit = cfg_.bit_depth.has_value(),
          .requested_bit_depth_reason = prepared.settings.bit_depth_reason,
          .has_alpha = prepared.settings.source_has_alpha_channel,
          .must_preserve_alpha = must_preserve_alpha,
          .visual_quality_search = cfg_.visual_quality.has_value(),
          .speed_explicit = cfg_.speed.has_value(),
          .pixel_count = static_cast<std::uint64_t>(encode_width) * encode_height,
          .width = encode_width,
          .height = encode_height,
          .speed = prepared.settings.speed});
      if (!selection) {
        prepared.settings.requested_avif_encoder = selection_requested_encoder;
        prepared.settings.requested_chroma_mode = selection_requested_chroma;
        prepared.settings.requested_bit_depth = selection_requested_bit_depth;
        prepared.settings.encoder_supports_alpha = native_backend_detail::encoder_supports_alpha(
            selection_requested_encoder);
        prepared.settings.applied_alpha = native_backend_detail::applied_alpha_name(
            prepared.settings.source_has_alpha_channel, false, prepared.settings.encoder_supports_alpha);
        if (cfg_.alpha_policy == AlphaModePolicy::force &&
            prepared.settings.source_has_alpha_channel && !prepared.settings.encoder_supports_alpha) {
          prepared.settings.alpha_reason = "force 请求保留 alpha，但当前编码器不支持 alpha";
        }
        return prepare_failed(selection.error(), prepared.settings);
      }
      prepared.settings.avif_encoder = selection->applied_encoder;
      prepared.settings.chroma_mode = selection->applied_chroma;
      prepared.settings.bit_depth = selection->applied_bit_depth;
      prepared.settings.speed = selection->speed;
      prepared.settings.requested_avif_encoder = selection->requested_encoder;
      prepared.settings.requested_chroma_mode = selection->requested_chroma;
      prepared.settings.requested_bit_depth = selection->requested_bit_depth;
      const bool selected_bit_depth_was_clamped =
          selection->requested_bit_depth && selection->applied_bit_depth &&
          *selection->requested_bit_depth != *selection->applied_bit_depth;
      if (!selection->bit_depth_reason.empty() &&
          (prepared.settings.bit_depth_reason.empty() || selected_bit_depth_was_clamped)) {
        prepared.settings.bit_depth_reason = selection->bit_depth_reason;
      }
      prepared.settings.encoder_fallback_reason = selection->fallback_reason;
      prepared.settings.encoder_supports_alpha = native_backend_detail::encoder_supports_alpha(
          selection->applied_encoder);
      const bool preserve_alpha = prepared.settings.source_has_alpha_channel &&
                                  ((cfg_.alpha_policy == AlphaModePolicy::force &&
                                    prepared.settings.encoder_supports_alpha) ||
                                   (cfg_.alpha_policy == AlphaModePolicy::automatic &&
                                    *has_non_opaque_alpha &&
                                    prepared.settings.encoder_supports_alpha));
      prepared.settings.applied_alpha = native_backend_detail::applied_alpha_name(
          prepared.settings.source_has_alpha_channel, preserve_alpha,
          prepared.settings.encoder_supports_alpha);
      if (!prepared.settings.source_has_alpha_channel) {
        prepared.settings.alpha_reason = "源图没有 alpha 通道";
      } else if (cfg_.alpha_policy == AlphaModePolicy::off) {
        prepared.settings.alpha_reason = "用户请求移除 alpha";
      } else if (cfg_.alpha_policy == AlphaModePolicy::automatic && !*has_non_opaque_alpha) {
        prepared.settings.alpha_reason = "auto 移除全不透明 alpha";
      } else if (cfg_.alpha_policy == AlphaModePolicy::automatic &&
                 prepared.settings.encoder_supports_alpha) {
        prepared.settings.alpha_reason = "auto 保留非不透明 alpha，因为当前编码器支持 alpha";
      } else if (cfg_.alpha_policy == AlphaModePolicy::automatic) {
        prepared.settings.alpha_reason = "auto 移除 alpha，因为当前编码器不支持 alpha";
      } else if (prepared.settings.encoder_supports_alpha) {
        prepared.settings.alpha_reason = "force 保留源图 alpha 通道";
      } else {
        prepared.settings.alpha_reason = "force 请求保留 alpha，但当前编码器不支持 alpha";
      }
      native_backend_detail::populate_applied_avif_color_diagnostics(prepared.settings,
                                                                     cfg_,
                                                                     selection->applied_chroma,
                                                                     avif_lossless);
      prepared.avif_bit_depth_reason = prepared.settings.bit_depth_reason;
      native_backend_detail::log_info_noexcept(logger_, [&] {
        return awj::compat::format(
            "AVIF decision: item={:04} user_encoder={} user_chroma={} source_chroma={} color_representation_requested={} color_representation_applied={} source_bit_depth={} alpha_policy={} source_alpha={} non_opaque_alpha={} selected_encoder={} requested_chroma={} applied_chroma={} requested_bit_depth={} applied_bit_depth={} applied_alpha={} fallback={} chroma_reason={} alpha_reason={} bit_depth_reason={} color_source={}",
            image.index + 1,
            prepared.settings.user_encoder_id,
            prepared.settings.user_chroma,
            prepared.settings.source_chroma,
            avif_color_representation_name(
                prepared.settings.requested_avif_color_representation),
            avif_color_representation_name(
                prepared.settings.avif_color_representation),
            prepared.settings.source_bit_depth ? awj::compat::format("{}", *prepared.settings.source_bit_depth) : std::string{""},
            prepared.settings.alpha_policy_name,
            prepared.settings.source_alpha_mode,
            *has_non_opaque_alpha ? "true" : "false",
            avif_encoder_mode_name(selection->applied_encoder),
            chroma_mode_name(selection->requested_chroma),
            chroma_mode_name(selection->applied_chroma),
            selection->requested_bit_depth ? awj::compat::format("{}", *selection->requested_bit_depth) : std::string{""},
            selection->applied_bit_depth ? awj::compat::format("{}", *selection->applied_bit_depth) : std::string{""},
            prepared.settings.applied_alpha,
            selection->fallback_reason,
            prepared.settings.chroma_reason,
            prepared.settings.alpha_reason,
            prepared.settings.bit_depth_reason,
            prepared.settings.color_metadata_source);
      });
      prepared.encoder = native_backend_detail::encoder_for_output_format(
          cfg_.output_format, selection->applied_encoder);
    } else {
      if (cfg_.output_format == OutputFormat::png ||
          cfg_.output_format == OutputFormat::jxl ||
          cfg_.output_format == OutputFormat::webp ||
          cfg_.output_format == OutputFormat::jpgli) {
        native_backend_detail::populate_source_image_diagnostics(prepared.settings,
                                                                 decoded.image);
        const auto alpha_decision = native_backend_detail::populate_regular_alpha_decision(
            prepared.settings, decoded.image, cfg_);
        if (!alpha_decision) {
          return prepare_failed(alpha_decision.error(), prepared.settings);
        }
        native_backend_detail::populate_regular_color_decision(prepared.settings, cfg_);
        if (cfg_.output_format == OutputFormat::png) {
          prepared.settings.applied_matrix_coefficients = 0;
          prepared.settings.applied_color_range = 1;
          prepared.settings.bit_depth_reason = cfg_.bit_depth ? "用户明确请求 PNG 8/16-bit"
              : *hdr_signal ? "所选 HDR 呈现默认 PNG 16-bit" : "PNG 兼容性优先：SDR 默认 8-bit";
        }
        if (cfg_.output_format == OutputFormat::jxl)
          prepared.settings.bit_depth_reason = cfg_.bit_depth ? "用户明确请求 JXL 码流位深"
              : "所选呈现默认 JXL 16-bit，与 SDR/HDR 判断独立";
        if (cfg_.output_format == OutputFormat::jpgli) {
          prepared.settings.user_chroma =
              chroma_mode_name(prepared.settings.requested_chroma_mode);
          prepared.settings.bit_depth = 8;
          prepared.settings.bit_depth_reason =
              cfg_.bit_depth ? "用户明确请求 JPG 8-bit 输出"
                             : "JPG 输出 JPEG 兼容 8-bit precision";
          // Fix the sampling before any RGB/tone-map conversion loses the source layout.
          const bool hdr_jpeg = *hdr_signal && cfg_.jpeg_hdr != JpegHdrMode::sdr;
          prepared.settings.chroma_mode =
              cfg_.chroma_mode == ChromaMode::auto_keep && !cfg_.jpegli_xyb && !hdr_jpeg
                  ? jpeg_auto_chroma(decoded.image)
                  : cfg_.chroma_mode;
          if (!cfg_.jpegli_xyb) {
            prepared.settings.applied_matrix_coefficients = 6;
            prepared.settings.applied_color_range = 1;
          }
          prepared.settings.chroma_reason =
              cfg_.chroma_mode == ChromaMode::auto_keep
                  ? (cfg_.jpegli_xyb ? "XYB 模式使用 Jpegli 默认 RGB/XYB 采样"
                                     : awj::compat::format("JPG chroma auto 根据源图选择 {} chroma（灰度回退三分量 444）",
                                                   chroma_mode_name(prepared.settings.chroma_mode)))
                  : "用户请求 JPG 色度采样";
          if (cfg_.jpegli_xyb && !cfg_.strip_metadata) {
            prepared.settings.applied_icc =
                prepared.settings.source_has_icc ? "stripped-xyb" : "none";
            prepared.settings.color_metadata_source =
                prepared.settings.source_has_icc ? "xyb-no-source-icc" : "encoder-default";
            prepared.settings.color_reason =
                prepared.settings.source_has_icc
                    ? "XYB 模式不写入源 ICC，避免源色彩配置与 XYB/RGB 量化模式冲突"
                    : prepared.settings.color_reason;
          }
        }
      }
      prepared.encoder = native_backend_detail::encoder_for_output_format(
          cfg_.output_format, requested_avif_encoder);
    }

    if (!prepared.encoder) {
      return prepare_failed(awj::compat::format("native backend 暂不支持输出格式: {}",
                                        output_format_name(cfg_.output_format)),
                            prepared.settings);
    }
    return std::move(prepared);
  }

  [[nodiscard]] awj::compat::expected<NativeEncodeResult, std::string> execute_encode(
      ImageDecodeResult& decoded,
      ImageEncoder* encoder,
      const NativeEncodeSettings& settings,
      const fs::path& output_path,
      awj::compat::stop_token stop_token,
      EncodeResult& memory_result,
      std::uint64_t compressed_bytes,
      std::uint64_t additional_resident_bytes = 0) const {
    const bool source_has_gain_map = decoded.image.source_info && decoded.image.source_info->source_has_gain_map;
    const auto encode_started = native_backend_detail::Clock::now();
    if (stop_token.stop_requested()) {
      return awj::compat::unexpected{"任务已取消。"};
    }
    auto effective_settings = settings;
    double color_convert_seconds = -1.0;
    if (cfg_.color_primaries || cfg_.transfer_characteristics) {
      const auto conversion_started = native_backend_detail::Clock::now();
      if (native_backend_detail::image_has_metadata(decoded.image, MetadataKind::icc)) {
        auto verified = gain_map_detail::resolve_iso_color(decoded.image, stop_token);
        if (!verified) return awj::compat::unexpected{verified.error()};
        gain_map_detail::normalize_iso_base(decoded.image, *verified);
      }
      if (native_backend_detail::requested_color_conversion_needed(decoded.image, cfg_)) {
        auto converted = convert_image_pixels(decoded.image, PixelConversionRequest{
            .format = PixelFormat::rgba, .bit_depth = 16,
            .preserve_alpha = effective_settings.applied_alpha == "kept",
            .color_primaries = cfg_.color_primaries,
            .transfer_characteristics = cfg_.transfer_characteristics}, stop_token, effective_settings.resources);
        if (!converted) return awj::compat::unexpected{converted.error()};
        decoded.image = std::move(*converted);
      }
      if (decoded.image.source_info) {
        effective_settings.applied_color_primaries = decoded.image.source_info->color_primaries;
        effective_settings.applied_transfer_characteristics = decoded.image.source_info->transfer_characteristics;
        effective_settings.applied_icc = "none";
        effective_settings.source_content_light = decoded.image.source_info->content_light;
        effective_settings.color_reason = "按明确源色彩实际转换像素，必要颜色信令不随 strip 删除";
      }
      color_convert_seconds = native_backend_detail::elapsed_seconds(conversion_started);
    }
    if (cfg_.output_format == OutputFormat::jpgli) {
      auto signal = hdr::has_explicit_hdr_signal(decoded.image);
      if (!signal) return awj::compat::unexpected{signal.error()};
      if (cfg_.jpeg_hdr == JpegHdrMode::hdr && !*signal) {
        effective_settings.encoder_fallback_reason = "SDR input -> SDR JPEG";
        effective_settings.color_reason += "；原输入仅有 SDR，保留单层且不伪造 HDR/Gain Map";
      }
#if AWJ_HAS_JPEGLI
      if (*signal && cfg_.jpeg_hdr != JpegHdrMode::sdr) {
        std::optional<ImageBuffer> rgb;
        if (is_yuv_format(decoded.image.pixel_format)) {
          auto converted = convert_image_pixels(decoded.image, PixelConversionRequest{.format = PixelFormat::rgba,
              .bit_depth = decoded.image.bit_depth > 8 ? 16 : 8, .preserve_alpha = false},
              stop_token, effective_settings.resources);
          if (!converted) return awj::compat::unexpected{converted.error()};
          rgb = std::move(*converted);
        } else if (decoded.image.geometry) {
          auto transformed = resize_native_pixels(decoded.image, decoded.image.width, decoded.image.height,
                                                   stop_token, effective_settings.resources);
          if (!transformed) return awj::compat::unexpected{transformed.error()};
          rgb = std::move(*transformed);
        }
        auto result = encode_hdr_jpeg(rgb ? *rgb : decoded.image, effective_settings, stop_token);
        if (result) {
          result->diagnostics.timing.encode_seconds = native_backend_detail::elapsed_seconds(encode_started);
          result->diagnostics.timing.color_convert_seconds = color_convert_seconds;
        }
        return result;
      }
#endif
    }
    const ImageBuffer* effective_image = &decoded.image;
    std::optional<ImageBuffer> search_reference;
    if (native_backend_detail::sdr_only_format_conversion_needed(cfg_.output_format, decoded.image)) {
      auto signal = hdr::has_explicit_hdr_signal(decoded.image);
      if (!signal) return awj::compat::unexpected{signal.error()};
      effective_settings.tone_map_to_sdr_pixels = *signal;
      if (cfg_.output_format != OutputFormat::png) effective_settings.bit_depth = 8;
      else effective_settings.bit_depth = settings.bit_depth.value_or(8);
      if (*signal) {
        effective_settings.applied_color_primaries = 1;
        effective_settings.applied_transfer_characteristics = 13;
        effective_settings.applied_matrix_coefficients = 0;
        effective_settings.applied_color_range = 1;
        effective_settings.applied_icc = "stripped-hdr-sdr";
        effective_settings.applied_hdr_metadata = "sdr-tone-map";
        effective_settings.color_metadata_source = "hdr-sdr-srgb";
        effective_settings.encoder_fallback_reason = "HDR -> SDR tone-map";
        effective_settings.color_reason = "目标格式不支持 HDR，且没有可复用的 SDR 端点，已转换为 SDR sRGB";
        effective_settings.rendition.selected = "sdr";
        effective_settings.rendition.origin = "tone-mapped";
        effective_settings.rendition.tone_mapped = true;
        effective_settings.rendition.reused_sdr = false;
      } else effective_settings.bit_depth_reason = "目标格式只支持 8-bit，已按 SDR 缩减位深";
    } else if (decoded.image.sample_representation == SampleRepresentation::ieee_half_float) {
      effective_settings.applied_color_primaries = 9;
      effective_settings.applied_transfer_characteristics = 16;
      effective_settings.applied_matrix_coefficients = 9;
      effective_settings.applied_color_range = 1;
      effective_settings.applied_icc = "stripped-scrgb-hdr";
      effective_settings.applied_hdr_metadata = "scrgb-to-hdr";
      effective_settings.color_metadata_source = "scrgb-linear-to-bt2020-pq";
      effective_settings.encoder_fallback_reason = source_has_gain_map ? "Gain Map linear HDR -> BT.2020/PQ" : "scRGB -> BT.2020/PQ";
      effective_settings.color_reason = "线性 FP16 直接转换为目标 BT.2020/PQ 像素";
    }
    // 视觉搜索共享稳定 RGB 参考；独立物化时外层原图仍存活到所有候选结束。
    if (cfg_.visual_quality && (effective_settings.tone_map_to_sdr_pixels ||
        decoded.image.sample_representation == SampleRepresentation::ieee_half_float)) {
      auto converted = convert_image_pixels(decoded.image, PixelConversionRequest{.format = PixelFormat::rgba,
          .bit_depth = effective_settings.tone_map_to_sdr_pixels ? 8 : 16,
          .preserve_alpha = effective_settings.applied_alpha == "kept",
          .tone_map_to_sdr = effective_settings.tone_map_to_sdr_pixels},
          stop_token, effective_settings.resources);
      if (!converted) return awj::compat::unexpected{converted.error()};
      search_reference = std::move(*converted); effective_image = &*search_reference;
      // 初次准入已预留参考；现在用真实 capacity 精化，仍只在持有 ticket 内选缓存。
      if (!admit_encoding_memory(memory_result, decoded, effective_settings, compressed_bytes,
              additional_resident_bytes, &*search_reference))
        return awj::compat::unexpected{memory_result.message};
      // codec 几何已把稳定参考作为 source 计费；这里只补它看不到的原图，不能再加参考。
      effective_settings.resources.geometry_retained_bytes = hdr_cost::add(
          effective_settings.resources.geometry_retained_bytes, resident_image_bytes(decoded.image));
      effective_settings.tone_map_to_sdr_pixels = false;
    }
    if (cfg_.visual_quality) {
      auto output_decoder = native_backend_detail::decoder_for_output_format(
          cfg_.output_format, per_file_thread_budget(effective_settings.resources));
      const auto candidate_path = output_path.parent_path() /
                                  (output_path.filename().wstring() + L".candidate");


      auto search = encode_with_native_visual_quality_search(*effective_image, *encoder,
                                                             *output_decoder, effective_settings,
                                                             candidate_path, stop_token);
      if (!search) {
        return awj::compat::unexpected{search.error()};
      }
      search->encode_result.diagnostics.timing.encode_seconds =
          search->encode_result.diagnostics.timing.visual_quality_candidate_encode_seconds;
      search->encode_result.visual_quality_target_met = search->target_met;
      search->encode_result.diagnostics.timing.color_convert_seconds = color_convert_seconds;
      return std::move(search->encode_result);
    }


    auto encoded = encoder->encode(*effective_image, effective_settings, stop_token);
    if (encoded) {
      encoded->diagnostics.timing.encode_seconds =
          native_backend_detail::elapsed_seconds(encode_started);
      encoded->diagnostics.timing.color_convert_seconds = color_convert_seconds;
    }
    return encoded;
  }

  [[nodiscard]] EncodeResult finalize_result(EncodeResult& result,
                                             NativeEncodeResult encoded,
                                             const ImageDecodeResult& decoded,
                                             bool decoder_used_fallback,
                                             std::string avif_bit_depth_reason,
                                             EncodeStartedAt started,
                                             awj::compat::stop_token stop_token) const {
    if (cpu_budget_) cpu_pinned_ = false;
    if (!admit_output_cpu(result)) return result;
    auto written = [&] {
      native_backend_detail::ScopedStageTimer timer{result.write_seconds};
      return native_backend_detail::write_output_bytes(
          result.output_path, std::span<const std::byte>{encoded.encoded.bytes},
          cfg_.collision_mode == CollisionMode::overwrite, stop_token);
    }();
    if (!written) {
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
      } else {
        mark_failed(result, written.error());
      }
      return std::move(result);
    }

    encoded.diagnostics.decoder_id = decoded.decoder_id;
    encoded.diagnostics.used_decoder_fallback = decoder_used_fallback;
    native_backend_detail::merge_stage_timing(encoded, result);
    native_backend_detail::copy_native_result(encoded, result);
    if (!decoded.decoder_id.empty()) {
      result.decoder_id = decoded.decoder_id;
      result.command = awj::compat::format("native:{}:{}:{} q{} speed={}",
                                   result.output_format,
                                   result.decoder_id,
                                   result.encoder_id,
                                   result.final_encoder_quality,
                                   result.speed);
    }
    if (!avif_bit_depth_reason.empty()) {
      result.bit_depth_reason = avif_bit_depth_reason;
    }
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    result.processed = true;
    result.ok = true;
    result.message = "OK";
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return awj::compat::format("native encode ok: item={:04}", result.index + 1);
    });
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return awj::compat::format(
          "native timing: item={:04} total={}s decode={}s resize={}s resize_workers={} prepare={}s encode={}s vq={}s metrics={}s write={}s",
          result.index + 1,
          native_backend_detail::format_timing_seconds(result.seconds),
          native_backend_detail::format_timing_seconds(result.decode_seconds),
          native_backend_detail::format_timing_seconds(result.resize_seconds),
          result.resize_workers,
          native_backend_detail::format_timing_seconds(result.prepare_seconds),
          native_backend_detail::format_timing_seconds(result.encode_seconds),
          native_backend_detail::format_timing_seconds(result.visual_quality_search_seconds),
          native_backend_detail::format_timing_seconds(result.visual_quality_metric_seconds),
          native_backend_detail::format_timing_seconds(result.write_seconds));
    });
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return awj::compat::format(
          "native rendition: item={:04} selected={} origin={} reference_depth={} reference_chroma={} reused_sdr={} tone_mapped={} gain_map={} map={}x{} map_depth={}",
          result.index + 1, result.rendition.selected, result.rendition.origin,
          result.rendition.reference_bit_depth.value_or(0), result.rendition.reference_chroma,
          result.rendition.reused_sdr, result.rendition.tone_mapped, result.rendition.gain_map_generated,
          result.rendition.gain_map_width, result.rendition.gain_map_height, result.rendition.gain_map_bit_depth);
    });
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return awj::compat::format(
          "native HDR breakdown: item={:04} base_decode={}s aux_decode={}s map_render={}s map_compute={}s color_convert={}s render_workers={} compute_workers={} heif_tile_budget={} heif_codec_budget={}",
          result.index + 1, native_backend_detail::format_timing_seconds(result.base_decode_seconds),
          native_backend_detail::format_timing_seconds(result.auxiliary_decode_seconds),
          native_backend_detail::format_timing_seconds(result.gain_map_render_seconds),
          native_backend_detail::format_timing_seconds(result.gain_map_compute_seconds),
          native_backend_detail::format_timing_seconds(result.color_convert_seconds),
          result.gain_map_render_workers, result.gain_map_compute_workers,
          result.heif_tile_parallelism, result.heif_codec_thread_budget);
    });
    if (result.visual_quality_search_seconds >= 0.0) {
      native_backend_detail::log_info_noexcept(logger_, [&] {
        return awj::compat::format(
            "native vq breakdown: item={:04} candidates={} memory_fallback_decodes={} candidate_encode={}s candidate_decode={}s candidate_io={}s luma={}s gmsd={}s ms_ssim={}s metrics={}s metric_share={}%",
            result.index + 1,
            result.visual_quality_candidate_count,
            result.visual_quality_decode_memory_fallback_count,
            native_backend_detail::format_timing_seconds(result.visual_quality_candidate_encode_seconds),
            native_backend_detail::format_timing_seconds(result.visual_quality_candidate_decode_seconds),
            native_backend_detail::format_timing_seconds(result.visual_quality_candidate_io_seconds),
            native_backend_detail::format_timing_seconds(result.visual_quality_luma_seconds),
            native_backend_detail::format_timing_seconds(result.gmsd_seconds),
            native_backend_detail::format_timing_seconds(result.ms_ssim_seconds),
            native_backend_detail::format_timing_seconds(result.visual_quality_metric_seconds),
            native_backend_detail::format_timing_share(result.visual_quality_metric_seconds,
                                                      result.visual_quality_search_seconds));
      });
      if (!result.visual_quality_search_trace.empty()) {
        native_backend_detail::log_info_noexcept(logger_, [&] {
          return awj::compat::format("native vq trace: item={:04} {}",
                             result.index + 1,
                             result.visual_quality_search_trace);
        });
      }
    }
    return std::move(result);
  }

  EncodeResult encode_with_overrides(const ImageFile& image,
                                     EncodeOverrides overrides,
                                     awj::compat::stop_token stop_token = {}) const {
    const auto started = native_backend_detail::Clock::now();
    auto result = encode_with_overrides_impl(image, std::move(overrides), stop_token);
    if (result.processed && !result.ok) result.seconds = native_backend_detail::elapsed_seconds(started);
    return result;
  }

  EncodeResult encode_with_overrides_impl(const ImageFile& image,
                                          EncodeOverrides overrides,
                                          awj::compat::stop_token stop_token) const {
    const auto started = native_backend_detail::Clock::now();
    EncodeResult result{};
    const auto failed = [&](std::string_view message) {
      // 保留所有已观察阶段，包含栈展开时的作用域退出计时；
      // 初始化自身可能在路径就绪前失败。
      result.index = image.index;
      if (result.input_path.empty()) result.input_path = image.path;
      result.original_bytes = image.bytes;
      if (!cancel_if_requested(result, stop_token)) mark_failed(result, std::string{message});
      return std::move(result);
    };
    try {
      result = initialize_result(image);

      if (cancel_if_requested(result, stop_token)) {
        return result;
      }
      if (result.processed && !result.ok) {
        return result;
      }

      if (cfg_.collision_mode == CollisionMode::skip) {
        std::error_code exists_ec;
        const auto output_exists = fs::exists(result.output_path, exists_ec);
        if (exists_ec) {
          result.processed = true;
          result.ok = false;
          result.message = awj::compat::format("无法检查输出路径 {}: {}",
                                       display_path_for_user(result.output_path), exists_ec.message());
          return result;
        }
        if (output_exists) {
          result.processed = true;
          result.ok = true;
          result.skipped = true;
          result.message = "输出已存在，已跳过。";
          return result;
        }
      }

      if (refresh_cpu_budget() < 1) return failed("计算线程预约已取消。");

      if (cfg_.output_format == OutputFormat::avif || cfg_.output_format == OutputFormat::webp) {
        auto animation_failed = [&](std::string message) -> EncodeResult {
          if (!cancel_if_requested(result, stop_token)) mark_failed(result, std::move(message));
          return std::move(result);
        };
        result.decode_stage = "animation.probe";
        auto opened = [&]() -> awj::compat::expected<std::unique_ptr<AnimationReader>, std::string> {
          native_backend_detail::ScopedStageTimer timer{result.decode_seconds};
          // 只有尚未进入逐帧解码的 open 可以重试。失败返回已销毁 reader/线程配置，
          // 扩容始终非阻塞；降 CPU 后重新创建 reader，不令旧配置越过新额度。
          constexpr int max_attempts = 2 * encoding_defaults::max_automatic_thread_budget + 2;
          for (int attempt = 0; attempt < max_attempts; ++attempt) {
            const auto reader_budget = resource_memory_budget(resources_);
            if (!reader_budget) return awj::compat::unexpected{"动画每文件内存预算不足。"};
            std::uint64_t required{};
            auto candidate = open_animation(image.path,
                resources_.memory_limit_bytes ? reader_budget : 0, stop_token,
                cfg_.output_format == OutputFormat::webp, per_file_thread_budget(resources_), &required);
            if (candidate || !required || stop_token.stop_requested()) return candidate;
            if (required != UINT64_MAX && required > reader_budget && reserve_memory_ &&
                required <= resources_.memory_limit_bytes && reserve_memory_(required)) {
              resources_.memory_reservation_bytes = required;
              result.memory_plan.reserved_peak_bytes = std::max(result.memory_plan.reserved_peak_bytes, required);
              result.estimated_working_set_bytes = std::max(result.estimated_working_set_bytes, required);
              continue;
            }
            const int current = per_file_thread_budget(resources_);
            if (!cpu_budget_ || current <= 1) return candidate;
            if (refresh_cpu_budget(current - 1) < 1)
              return awj::compat::unexpected{"计算线程预约已取消，未启动动画解码。"};
          }
          return awj::compat::unexpected{"动画输入的工作集需求持续变化，已停止重试。"};
        }();
        if (!opened) return animation_failed(opened.error());
        if (*opened) {
          // Reader/序列编码器会跨帧保留线程配置，整段同步生命周期固定已授权额度。
          if (cpu_budget_) cpu_pinned_ = true;
          if (cfg_.avif_layers.enabled() && (*opened)->info().durations.size() > 1)
            return animation_failed("AVIF 渐进式仅支持静态图，不能与多帧动画组合。");
          if (cfg_.visual_quality && *cfg_.visual_quality < 100 && (*opened)->info().durations.size() > 1) {
            return animation_failed("动画输出暂不支持视觉质量搜索；请使用编码质量预设。动画不会降为单图。");
          }
          auto reader = [&] {
            native_backend_detail::ScopedStageTimer timer{result.prepare_seconds};
            return native_backend_detail::GeometryAnimationReader{std::move(*opened), cfg_, resources_, result, reserve_memory_};
          }();
          result.decode_stage = "animation.decode";
          auto next = reader.next(stop_token);
          if (!next || !*next) {
            return animation_failed(next ? "动画不包含帧。" : next.error());
          }
          ImageDecodeResult first{};
          auto prepared = [&] {
            native_backend_detail::ScopedStageTimer timer{result.prepare_seconds};
            first = ImageDecodeResult{.image = reader.frame(), .decoder_id = reader.info().decoder_id};
            return prepare_encoding(image, first, {});
          }();
          // 单帧使用普通编码；多帧也先按真实首帧计划取得预约，再释放借用副本。
          if (!prepared) return animation_failed(prepared.error().message);
          const auto admitted = [&] {
            native_backend_detail::ScopedStageTimer timer{result.prepare_seconds};
            return admit_encoding_memory(result, first, prepared->settings, image.bytes, reader.memory_usage_bytes());
          }();
          if (!admitted) return result;
          if (reader.info().durations.size() > 1) first.image.planes.clear();
          reader.bind_encoding_memory(prepared->settings);
          if (prepared->settings.avif_grid_plan || overrides.avif_grid_plan) {
            mark_failed(result, "动画 AVIF 不能使用静态 Grid；请通过尺寸限制缩小画布。"); return result;
          }
          if (cfg_.frame_progress) {
            prepared->settings.frame_progress = [&](std::size_t completed, std::size_t total) {
              cfg_.frame_progress(image.index, completed, total);
            };
          }
          auto encoded = [&] {
            auto timer = reader.time_encode(result.encode_seconds);
            return reader.info().durations.size() == 1
                ? execute_encode(first, prepared->encoder.get(), prepared->settings, result.output_path, stop_token,
                    result, image.bytes, reader.memory_usage_bytes())
                : cfg_.output_format == OutputFormat::webp
                    ? encode_webp_animation(reader, prepared->settings, stop_token)
                    : encode_avif_animation(reader, prepared->settings, stop_token);
          }();
          if (!encoded) return animation_failed(encoded.error());
          if (reader.info().durations.size() > 1) {
            // 编码外层墙钟包含同步 next()；只发布排除嵌套读取后的 encode 区间，避免重复计帧。
            encoded->diagnostics.timing.encode_seconds = result.encode_seconds;
          }
          if (reader.info().durations.size() == 1 && prepared->settings.frame_progress)
            prepared->settings.frame_progress(1, 1);
          auto finished = finalize_result(result, std::move(*encoded), first, false,
              {}, started, stop_token);
          if (finished.ok && reader.info().zero_delay_adjusted) {
            finished.message = "OK；零时长帧已按 GIF 10ms / APNG、WebP 1ms 编码。";
          }
          if (finished.ok && cfg_.output_format == OutputFormat::avif && reader.info().durations.size() > 1 && prepared->settings.speed >= 7)
            finished.message += "；速度 7–10 使用实时编码路径（10 对应 AOM CPU 9）";
          return finished;
        }
      }

      if (auto passthrough = try_avif_lossless_passthrough(image, result, started, stop_token)) {
        return std::move(*passthrough);
      }

      if (auto transcode = try_jxl_jpeg_bitstream_transcode(image, result, started, stop_token)) {
        return std::move(*transcode);
      }

      DecodeDiagnostics decode_diagnostics;
      auto decoded_input = [&] {
        native_backend_detail::ScopedStageTimer timer{result.decode_seconds};
        return decode_input(image, stop_token, decode_diagnostics);
      }();
      native_backend_detail::copy_decode_diagnostics(decode_diagnostics, result);
      if (!decoded_input) {
        if (!cancel_if_requested(result, stop_token)) mark_failed(result, decoded_input.error());
        return result;
      }

      if (cancel_if_requested(result, stop_token)) return result;
      if (refresh_cpu_budget() < 1) return failed("计算线程预约已取消。");

      auto selected = [&] {
        native_backend_detail::ScopedStageTimer timer{result.prepare_seconds};
        return native_backend_detail::select_output_rendition(decoded_input->decoded, cfg_, stop_token);
      }();
      if (!selected) {
        if (!cancel_if_requested(result, stop_token)) mark_failed(result, selected.error());
        return result;
      }
      if (auto limited = native_backend_detail::apply_image_size_limit(
              decoded_input->decoded, cfg_, stop_token, resources_, result, reserve_memory_); !limited) {
        if (!cancel_if_requested(result, stop_token)) mark_failed(result, limited.error());
        return result;
      }

      if (refresh_cpu_budget() < 1) return failed("计算线程预约已取消。");
      auto prepared = [&] {
        native_backend_detail::ScopedStageTimer timer{result.prepare_seconds};
        return prepare_encoding(image, decoded_input->decoded, std::move(overrides));
      }();
      if (!prepared) {
        native_backend_detail::copy_settings_diagnostics(prepared.error().settings, result);
        native_backend_detail::copy_decode_diagnostics(decode_diagnostics, result);
        mark_failed(result, prepared.error().message);
        return result;
      }

      if (cancel_if_requested(result, stop_token)) {
        return result;
      }

      const auto admitted = [&] {
        native_backend_detail::ScopedStageTimer timer{result.prepare_seconds};
        return admit_encoding_memory(result, decoded_input->decoded, prepared->settings, image.bytes);
      }();
      if (!admitted) return result;
      const auto previous_encode_seconds = std::max(0.0, result.encode_seconds);
      auto encoded = [&] {
        native_backend_detail::ScopedStageTimer timer{result.encode_seconds};
        return execute_encode(decoded_input->decoded, prepared->encoder.get(),
                              prepared->settings, result.output_path, stop_token, result, image.bytes);
      }();
      if (encoded && encoded->diagnostics.timing.encode_seconds >= 0.0)
        encoded->diagnostics.timing.encode_seconds += previous_encode_seconds;
      if (!encoded) {
        native_backend_detail::copy_settings_diagnostics(prepared->settings, result);
        native_backend_detail::copy_decode_diagnostics(decode_diagnostics, result);
        if (stop_token.stop_requested()) {
          cancel_if_requested(result, stop_token);
        } else {
          mark_failed(result, encoded.error());
        }
        return result;
      }

      if (cancel_if_requested(result, stop_token)) {
        return result;
      }

      auto finished = finalize_result(result, std::move(*encoded), decoded_input->decoded,
                             decoded_input->decoder_used_fallback,
                             std::move(prepared->avif_bit_depth_reason), started, stop_token);
      native_backend_detail::copy_decode_diagnostics(decode_diagnostics, finished);
      if (finished.ok && cfg_.output_format != OutputFormat::avif) {
        auto sequence = input_uses_animation_reader(image.path);
        if (sequence && *sequence) {
          finished.message += "；当前输出格式按静态图像处理输入，仅输出首帧";
          logger_.info(finished.message);
        }
      }
      return finished;
    } catch (const std::bad_alloc&) {
      return failed("native backend 单项转换内存不足。");
    } catch (const std::length_error&) {
      return failed("native backend 单项转换数据超过运行时限制。");
    } catch (const std::filesystem::filesystem_error&) {
      return failed("native backend 单项转换文件系统访问失败。");
    } catch (...) {
      return failed("native backend 单项转换发生未知异常。");
    }
  }

 private:
  bool admit_output_cpu(EncodeResult& result) const {
    if (refresh_cpu_budget(1) > 0) return true;
    mark_failed(result, "计算线程预约已取消，未写出输出。");
    result.canceled = true;
    return false;
  }

  bool admit_encoding_memory(EncodeResult& result, const ImageDecodeResult& decoded,
                             NativeEncodeSettings& settings, std::uint64_t compressed,
                             std::uint64_t additional_resident_bytes = 0,
                             const ImageBuffer* visual_reference = nullptr) const {
    const bool limited = !cfg_.unlock_memory_limit && resources_.memory_limit_bytes != 0;
    const auto limit = limited ? (reserve_memory_ ? resources_.memory_limit_bytes : resource_memory_budget(resources_)) : 0;
    if (refresh_cpu_budget() < 1) {
      mark_failed(result, "计算线程预约已取消，未启动编码。");
      result.canceled = true;
      return false;
    }
    // 线程相关工作区与 CPU 份额一起精化；扩容失败只尝试降低份额，绝不持图等待内存。
    const auto cache_budget = limited ? resource_memory_budget(settings.resources) : UINT64_MAX;
    ImageMemoryEstimate estimate{};
    bool reserved = false;
    bool exceeds_limit = false;
    bool cpu_canceled = false;
    for (;;) {
      settings.resources.encoder_threads_per_file = per_file_thread_budget(resources_);
      settings.resources.global_thread_budget = resources_.global_thread_budget;
      settings.resources.file_parallelism = 1;
      estimate = refined_regular_memory(decoded, settings, cfg_, compressed, cache_budget,
          additional_resident_bytes, visual_reference);
      exceeds_limit = estimate.bytes == UINT64_MAX || (limited && estimate.bytes > limit);
      reserved = !exceeds_limit && (!reserve_memory_ || reserve_memory_(estimate.bytes));
      const int current = per_file_thread_budget(resources_);
      if (reserved || !cpu_budget_ || cpu_pinned_ || current == 1) break;
      if (refresh_cpu_budget(current - 1) < 1) { cpu_canceled = true; break; }
    }
    const auto previous_peak = result.memory_plan.reserved_peak_bytes;
    store_memory_estimate(result, estimate);
    result.memory_plan.reserved_peak_bytes = previous_peak;
    if (!reserved) {
      mark_failed(result, cpu_canceled ? "计算线程预约已取消，未启动编码。" : exceeds_limit
          ? awj::compat::format("解码后精化预估工作集 {} 超过内存预算 {}，未启动编码。",
              format_size(estimate.bytes), format_size(limit))
          : "解码后工作集预约不足，已释放当前图像；未持有像素阻塞等待其他任务。");
      if (cpu_canceled) result.canceled = true;
      return false;
    }
    // 编码与候选搜索同步完成；result 和其预约 guard 均活到所有 codec 调用结束。
    settings.memory_reservation = [reserve = reserve_memory_, limit, limited, &result](std::uint64_t bytes) {
      if (bytes == UINT64_MAX || (limited && bytes > limit) || (reserve && !reserve(bytes))) return false;
      if (limited) result.memory_plan.reserved_peak_bytes = std::max(result.memory_plan.reserved_peak_bytes, bytes);
      result.estimated_working_set_bytes = std::max(result.estimated_working_set_bytes, bytes);
      return true;
    };
    result.memory_plan.reserved_peak_bytes = limited ? std::max(previous_peak, estimate.bytes) : 0;
    settings.memory_base_reservation = estimate.bytes;
    settings.resources.encoder_threads_per_file = per_file_thread_budget(resources_);
    settings.resources.file_parallelism = 1;
    settings.resources.memory_limit_bytes = limit;
    settings.resources.memory_reservation_bytes = limited ? estimate.bytes : 0;
    settings.resources.geometry_retained_bytes = hdr_cost::add(
        additional_resident_bytes, estimate.visual_encode_retained_bytes);
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return awj::compat::format("内存计划 v2: prepared={} source={} conversion={} codec={} output-capacity={} layers={} depth={} chroma={} alpha={}",
          estimate.bytes, estimate.source_resident_bytes, estimate.conversion_bytes,
          estimate.codec_workspace_bytes, estimate.output_capacity_bytes, estimate.layer_retained_bytes,
          estimate.target_bit_depth, chroma_mode_name(estimate.target_chroma), estimate.target_alpha);
    });
    return true;
  }
  int refresh_cpu_budget(int demand = -1) const {
    if (!cpu_budget_ || cpu_pinned_) return per_file_thread_budget(resources_);
    const int requested = demand < 0 ? resources_.global_thread_budget : demand;
    const int granted = cpu_budget_(requested);
    if (requested > 0 && granted > 0) {
      resources_.encoder_threads_per_file = std::clamp(granted, 1,
          std::max(1, std::min(requested, resources_.global_thread_budget)));
      resources_.file_parallelism = 1;
    }
    return granted;
  }

  const AppConfig& cfg_;
  FileLogger& logger_;
  mutable ResourcePlan resources_;
  std::function<bool(std::uint64_t)> reserve_memory_;
  CpuBudgetCallback cpu_budget_;
  mutable bool cpu_pinned_{};
  const awj::compat::expected<encoding_defaults::MemoryPolicy, std::string> memory_policy_;
  std::optional<std::string> resource_plan_error_;
};

}  // namespace awj
