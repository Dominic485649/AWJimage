module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <expected>
#include <filesystem>
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
import awj.avif_registry;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.hdr_tonemap;
import awj.image;
#if AWJ_HAS_JPEGLI
import awj.jpegli_codec;
#endif
import awj.jxl_codec;
import awj.large_image_plan;
import awj.native_visual_search;
import awj.png_codec;
import awj.resource_planner;
import awj.visual_quality;
import awj.webp_codec;

export namespace awj {

namespace native_backend_detail {

using Clock = std::chrono::steady_clock;

double elapsed_seconds(Clock::time_point started) {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

std::string format_timing_seconds(double seconds) {
  return seconds >= 0.0 ? std::format("{:.3f}", seconds) : std::string{""};
}

std::string format_timing_share(double part, double total) {
  return part >= 0.0 && total > 0.0 ? std::format("{:.1f}", part * 100.0 / total) : std::string{""};
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
  std::expected<ImageDecodeResult, std::string> decode(const fs::path&) const override {
    return std::unexpected{std::format("native backend 当前不支持该输入解码器: {}", id_)};
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
      return std::make_unique<WebPImageDecoder>();
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

std::expected<SourceFileTimes, std::string> capture_source_file_times(
    const fs::path& path) {
  const HANDLE raw_file = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw_file == INVALID_HANDLE_VALUE) {
    return std::unexpected{std::format("无法读取源文件时间 {}: {}",
                                       display_path_for_user(path),
                                       win32_error_message(GetLastError()))};
  }
  UniqueHandle file{raw_file};
  SourceFileTimes times{};
  if (!GetFileTime(file.get(), &times.creation, &times.access,
                   &times.modification)) {
    return std::unexpected{std::format("无法读取源文件时间 {}: {}",
                                       display_path_for_user(path),
                                       win32_error_message(GetLastError()))};
  }
  return times;
}

std::expected<void, std::string> apply_source_file_times(
    const fs::path& path, const SourceFileTimes& source,
    bool preserve_creation, bool preserve_modification, bool preserve_access) {
  const HANDLE raw_file = CreateFileW(
      path.c_str(), FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw_file == INVALID_HANDLE_VALUE) {
    return std::unexpected{std::format("无法写入输出文件时间 {}: {}",
                                       display_path_for_user(path),
                                       win32_error_message(GetLastError()))};
  }
  UniqueHandle file{raw_file};
  if (!SetFileTime(file.get(), preserve_creation ? &source.creation : nullptr,
                   preserve_access ? &source.access : nullptr,
                   preserve_modification ? &source.modification : nullptr)) {
    return std::unexpected{std::format("无法写入输出文件时间 {}: {}",
                                       display_path_for_user(path),
                                       win32_error_message(GetLastError()))};
  }
  return {};
}
#endif

std::expected<void, std::string> clear_transient_file_attributes(const fs::path& path,
                                                                 std::string_view label) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    return std::unexpected{std::format("无法读取{}属性 {}: {}",
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
    return std::unexpected{std::format("无法更新{}属性 {}: {}",
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

std::expected<void, TempOutputWriteFailure> write_file_bytes_exclusive(const fs::path& path,
                                                                         std::span<const std::byte> bytes) {
  if (bytes.size() > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("临时输出文件内容超过当前运行时上限: {}",
                               display_path_for_user(path))}};
  }
#ifdef _WIN32
  const auto raw_file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                   nullptr);
  if (raw_file == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("无法创建临时输出文件 {}: {}",
                               display_path_for_user(path),
                               win32_error_message(error)),
        .create_error = static_cast<int>(error)}};
  }
#else
  const int raw_file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (raw_file < 0) {
    const int error = errno;
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("无法创建临时输出文件 {}: {}",
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
      return std::unexpected{TempOutputWriteFailure{
          .message = std::format("写入临时输出文件失败 {}: {}",
                                 display_path_for_user(path),
                                 win32_error_message(GetLastError()))}};
    }
#else
    const auto written = ::write(file.get(), cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected{TempOutputWriteFailure{
          .message = std::format("写入临时输出文件失败 {}: {}",
                                 display_path_for_user(path),
                                 posix_error_message(errno))}};
    }
#endif
    if (written == 0) {
      return std::unexpected{TempOutputWriteFailure{
          .message = std::format("写入临时输出文件失败 {}", display_path_for_user(path))}};
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
#ifdef _WIN32
  if (!FlushFileBuffers(file.get())) {
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("刷新临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               win32_error_message(GetLastError()))}};
  }
  const HANDLE file_handle = file.get();
  if (!CloseHandle(file_handle)) {
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("关闭临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               win32_error_message(GetLastError()))}};
  }
#else
  const int file_handle = file.get();
  if (::fsync(file_handle) != 0) {
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("刷新临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               posix_error_message(errno))}};
  }
  if (::close(file_handle) != 0) {
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("关闭临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               posix_error_message(errno))}};
  }
#endif
  file.release();
  cleanup.release();
  return {};
}

std::expected<void, std::string> commit_temp_output(const fs::path& temp_path,
                                                    const fs::path& path,
                                                    bool replace_existing) {
#ifdef _WIN32
  const DWORD move_flags = replace_existing
                               ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                               : MOVEFILE_WRITE_THROUGH;
  if (!MoveFileExW(temp_path.c_str(), path.c_str(), move_flags)) {
    const auto error = GetLastError();
    if (!replace_existing && output_temp_name_collision(static_cast<int>(error))) {
      return std::unexpected{std::format("输出路径已存在，未覆盖 {}", display_path_for_user(path))};
    }
    return std::unexpected{std::format("替换输出文件失败 {}: {}", display_path_for_user(path),
                                       win32_error_message(error))};
  }
#else
  if (replace_existing) {
    if (::rename(temp_path.c_str(), path.c_str()) != 0) {
      return std::unexpected{std::format("替换输出文件失败 {}: {}", display_path_for_user(path),
                                         posix_error_message(errno))};
    }
  } else {
    if (::link(temp_path.c_str(), path.c_str()) != 0) {
      const int error = errno;
      if (error == EEXIST) {
        return std::unexpected{std::format("输出路径已存在，未覆盖 {}", display_path_for_user(path))};
      }
      return std::unexpected{std::format("替换输出文件失败 {}: {}", display_path_for_user(path),
                                         posix_error_message(error))};
    }
    (void)::unlink(temp_path.c_str());
  }
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

ChromaMode chroma_from_source_pixel_format(PixelFormat pixel_format) noexcept {
  switch (pixel_format) {
    case PixelFormat::yuv420:
      return ChromaMode::yuv420;
    case PixelFormat::yuv422:
      return ChromaMode::yuv422;
    case PixelFormat::yuv444:
      return ChromaMode::yuv444;
    case PixelFormat::rgb:
    case PixelFormat::rgba:
      return ChromaMode::yuv444;
    case PixelFormat::gray:
    case PixelFormat::unknown:
    default:
      return ChromaMode::yuv420;
  }
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
  return std::ranges::find_if(image.metadata, [kind](const MetadataBlock& block) {
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

std::uint8_t rgba_sample_to_u8(const std::byte* row, std::size_t sample,
                                int bit_depth) noexcept {
  if (bit_depth > 8) {
    std::uint16_t value{};
    std::memcpy(&value, row + sample * sizeof(value), sizeof(value));
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) + 128u) / 257u);
  }
  return std::to_integer<std::uint8_t>(row[sample]);
}

struct SdrImageConversion {
  ImageBuffer image{};
  bool hdr_tone_mapped{};
};

std::expected<SdrImageConversion, std::string> rgba_to_rgba8_for_sdr_only_format(
    const ImageBuffer& image) {
  if (image.pixel_format != PixelFormat::rgba ||
      (image.bit_depth != 8 && image.bit_depth != 16) || image.planes.empty()) {
    return std::unexpected{"HDR/高位深 -> SDR fallback 需要 RGBA ImageBuffer。"};
  }
  auto hdr_signal = hdr::has_explicit_hdr_signal(image);
  if (!hdr_signal) {
    return std::unexpected{hdr_signal.error()};
  }
  if (*hdr_signal) {
    auto tone_mapped = hdr::tone_map_to_sdr_srgb(image);
    if (!tone_mapped) {
      return std::unexpected{tone_mapped.error()};
    }
    return SdrImageConversion{.image = std::move(*tone_mapped), .hdr_tone_mapped = true};
  }
  if (image.sample_representation != SampleRepresentation::unorm) {
    return std::unexpected{"无法将缺少 HDR 标记的浮点 RGBA 当作 SDR 处理。"};
  }
  const auto& plane = image.planes.front();
  const auto stride = decoder_common::checked_rgba_stride(image.width, "HDR -> SDR fallback");
  if (!stride) {
    return std::unexpected{stride.error()};
  }
  const auto bytes = decoder_common::checked_image_bytes(*stride, image.height,
                                                         "HDR -> SDR fallback");
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  auto rgba8 = decoder_common::make_byte_buffer(*bytes, "HDR -> SDR fallback");
  if (!rgba8) {
    return std::unexpected{rgba8.error()};
  }

  auto* out = reinterpret_cast<std::uint8_t*>(rgba8->data());
  const std::size_t input_sample_stride = image.bit_depth > 8 ? sizeof(std::uint16_t) : 1;
  const auto expected_input_stride = decoder_common::checked_rgba_stride(
      image.width, "HDR -> SDR fallback", input_sample_stride);
  if (!expected_input_stride) {
    return std::unexpected{expected_input_stride.error()};
  }
  if (plane.stride < *expected_input_stride) {
    return std::unexpected{"HDR -> SDR fallback 输入 stride 无效。"};
  }

  for (std::size_t y = 0; y < image.height; ++y) {
    const auto* src = plane.bytes.data() + y * plane.stride;
    auto* dst = out + y * *stride;
    for (std::size_t x = 0; x < image.width; ++x) {
      const std::size_t sample = x * 4;
      dst[sample + 0] = rgba_sample_to_u8(src, sample + 0, image.bit_depth);
      dst[sample + 1] = rgba_sample_to_u8(src, sample + 1, image.bit_depth);
      dst[sample + 2] = rgba_sample_to_u8(src, sample + 2, image.bit_depth);
      dst[sample + 3] = rgba_sample_to_u8(src, sample + 3, image.bit_depth);
    }
  }

  ImageSourceInfo source_info{.pixel_format = PixelFormat::rgba,
                              .bit_depth = 8,
                              .color_primaries = 1,
                              .transfer_characteristics = 13,
                               .matrix_coefficients = 0,
                               .color_range = 1,
                               .has_hdr_metadata = false,
                               .color_metadata_source = "high-bit-depth-sdr"};
  auto fallback = decoder_common::make_rgba_image(
      image.width, image.height, std::move(*rgba8), image.alpha_mode,
      "HDR -> SDR fallback", source_info, 8);
  if (!fallback) {
    return std::unexpected{fallback.error()};
  }
  fallback->metadata = image.metadata;
  return SdrImageConversion{.image = std::move(*fallback), .hdr_tone_mapped = false};
}


std::optional<std::pair<std::size_t, std::size_t>> limited_dimensions(
    const ImageBuffer& image, const AppConfig& cfg) {
  if (image.width == 0 || image.height == 0 ||
      cfg.image_size_limit.mode == ImageSizeLimitMode::none) {
    return std::nullopt;
  }
  double scale = 1.0;
  const auto apply_edge = [&](std::optional<int> limit, std::size_t value) {
    if (limit && value > static_cast<std::size_t>(*limit)) {
      scale = std::min(scale, static_cast<double>(*limit) / static_cast<double>(value));
    }
  };
  if (cfg.image_size_limit.mode == ImageSizeLimitMode::automatic) {
    switch (cfg.output_format) {
      case OutputFormat::avif:
        // AOM/Grid preserves dimensions; only explicit manual limits resize AVIF.
        break;
      case OutputFormat::webp:
        apply_edge(16383, std::max(image.width, image.height));
        break;
      case OutputFormat::jpgli:
        apply_edge(65535, std::max(image.width, image.height));
        break;
      case OutputFormat::png:
      case OutputFormat::jxl:
      default:
        break;
    }
  } else {
    apply_edge(cfg.image_size_limit.max_width, image.width);
    apply_edge(cfg.image_size_limit.max_height, image.height);
    apply_edge(cfg.image_size_limit.max_long_edge, std::max(image.width, image.height));
    apply_edge(cfg.image_size_limit.max_short_edge, std::min(image.width, image.height));
  }
  if (scale >= 1.0) {
    return std::nullopt;
  }
  const auto width = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(image.width * scale)));
  const auto height = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(image.height * scale)));
  if (width == image.width && height == image.height) {
    return std::nullopt;
  }
  return std::pair{width, height};
}

std::expected<ImageBuffer, std::string> resize_rgba_image_nearest(
    const ImageBuffer& image, std::size_t new_width, std::size_t new_height) {
  if (image.pixel_format != PixelFormat::rgba || image.planes.empty() ||
      (image.bit_depth != 8 && image.bit_depth != 16)) {
    return std::unexpected{"图片尺寸限制当前只支持 8/16-bit RGBA buffer。"};
  }
  const auto bytes_per_sample = image.bit_depth > 8 ? std::size_t{2} : std::size_t{1};
  const auto stride = decoder_common::checked_rgba_stride(new_width, "image size limit", bytes_per_sample);
  if (!stride) {
    return std::unexpected{stride.error()};
  }
  const auto bytes = decoder_common::checked_image_bytes(*stride, new_height, "image size limit");
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  auto out = decoder_common::make_byte_buffer(*bytes, "image size limit");
  if (!out) {
    return std::unexpected{out.error()};
  }
  const auto& src = image.planes.front();
  const auto pixel_bytes = 4 * bytes_per_sample;
  for (std::size_t y = 0; y < new_height; ++y) {
    const auto sy = std::min(image.height - 1, y * image.height / new_height);
    const auto* src_row = src.bytes.data() + sy * src.stride;
    auto* dst_row = out->data() + y * *stride;
    for (std::size_t x = 0; x < new_width; ++x) {
      const auto sx = std::min(image.width - 1, x * image.width / new_width);
      std::ranges::copy_n(src_row + sx * pixel_bytes, pixel_bytes, dst_row + x * pixel_bytes);
    }
  }
  auto resized = decoder_common::make_rgba_image(new_width, new_height, std::move(*out),
                                                 image.alpha_mode, "image size limit",
                                                 image.source_info, image.bit_depth,
                                                 image.sample_representation);
  if (!resized) {
    return std::unexpected{resized.error()};
  }
  resized->metadata = image.metadata;
  resized->significant_bits = image.significant_bits; // Nearest-neighbor copies samples unchanged.
  return resized;
}

std::expected<void, std::string> apply_image_size_limit(ImageDecodeResult& decoded,
                                                       const AppConfig& cfg) {
  const auto dims = limited_dimensions(decoded.image, cfg);
  if (!dims) {
    return {};
  }
  auto resized = resize_rgba_image_nearest(decoded.image, dims->first, dims->second);
  if (!resized) {
    return std::unexpected{resized.error()};
  }
  decoded.image = std::move(*resized);
  return {};
}

std::string chroma_name_from_pixel_format(PixelFormat pixel_format) {
  const auto chroma = chroma_from_source_pixel_format(pixel_format);
  return chroma == ChromaMode::auto_keep ? "unknown" : chroma_mode_name(chroma);
}

std::string source_chroma_name(const ImageBuffer& image) {
  if (!image.source_info) {
    return "unknown";
  }
  return chroma_name_from_pixel_format(image.source_info->pixel_format);
}

ChromaMode lossless_source_chroma(const ImageBuffer& image) noexcept {
  if (image.source_info) {
    return chroma_from_source_pixel_format(image.source_info->pixel_format);
  }
  return ChromaMode::auto_keep;
}

AvifColorRepresentation effective_avif_color_representation(
    const AppConfig& cfg, const ImageBuffer& image) noexcept {
  if (cfg.avif_color_representation != AvifColorRepresentation::source) {
    return cfg.avif_color_representation;
  }
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

bool avif_lossless_passthrough_allowed(const AppConfig& cfg,
                                       const fs::path& path) {
  return avif_lossless_requested(cfg) && avif_lossless_passthrough_source(path) &&
         cfg.avif_encoder == AvifEncoderMode::automatic && cfg.chroma_mode == ChromaMode::auto_keep &&
         cfg.avif_color_representation != AvifColorRepresentation::rgb_identity &&
         !cfg.bit_depth && cfg.alpha_policy != AlphaModePolicy::off && !cfg.strip_metadata &&
         !has_user_color_settings(cfg);
}

bool jxl_jpeg_bitstream_transcode_allowed(const AppConfig& cfg,
                                          const fs::path& path) {
  return cfg.output_format == OutputFormat::jxl && jxl_jpeg_bitstream_source(path) &&
         !cfg.strip_metadata && !has_user_color_settings(cfg);
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

std::expected<void, std::string> populate_regular_alpha_decision(
    NativeEncodeSettings& settings,
    const ImageBuffer& image,
    const AppConfig& cfg) {
  const auto has_non_opaque_alpha = decoder_common::has_non_opaque_alpha(image,
                                                                         "native encoder");
  if (!has_non_opaque_alpha) {
    return std::unexpected{has_non_opaque_alpha.error()};
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

void populate_regular_color_decision(NativeEncodeSettings& settings, const AppConfig& cfg) {
  if (cfg.strip_metadata) {
    settings.applied_icc = settings.source_has_icc ? "stripped" : "none";
    settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? "stripped" : "none";
    settings.color_metadata_source = "stripped";
    settings.color_reason = "用户请求移除元数据";
    return;
  }

  settings.applied_icc = settings.source_has_icc ? "kept" : "none";
  settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? "not-written" : "none";
  if (settings.source_has_icc) {
    settings.color_metadata_source = "source-icc";
    settings.color_reason = "使用源图 ICC profile 写入编码器元数据";
  } else {
    settings.color_metadata_source = "encoder-default";
    settings.color_reason = settings.source_has_hdr_metadata || settings.source_color_primaries ||
                                    settings.source_transfer_characteristics ||
                                    settings.source_matrix_coefficients || settings.source_color_range
                                ? "当前编码器未写入源图 CICP/HDR 元数据，使用编码器默认值"
                                : "源图色彩元数据未知，使用编码器默认值";
  }
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
    return "stripped";
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
  if (cfg.strip_metadata) {
    settings.applied_color_primaries = cfg.color_primaries;
    settings.applied_transfer_characteristics = cfg.transfer_characteristics;
    settings.applied_matrix_coefficients = cfg.matrix_coefficients;
    settings.applied_color_range = cfg.color_range;
    settings.applied_icc = settings.source_has_icc ? "stripped" : "none";
    settings.applied_hdr_metadata = applied_hdr_metadata_name(settings, cfg);
    if (has_user_color_settings(cfg)) {
      settings.color_metadata_source = "user-cicp-settings";
      settings.color_reason = "已移除源图元数据，并使用用户 color/HDR 设置";
    } else {
      settings.color_metadata_source = "stripped";
      settings.color_reason = "用户请求移除元数据";
    }
    return;
  }

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
                             ? (user_color_settings ? "not-written" : "kept")
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
  return parent / std::format(L".awj-output-{}-{}.tmp", current_process_id(), id);
}

std::expected<void, std::string> write_output_bytes(const fs::path& path,
                                                    std::span<const std::byte> bytes,
                                                    bool replace_existing,
                                                    std::stop_token stop_token = {}) {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    if (bytes.empty()) {
      return std::unexpected{std::format("输出内容为空，无法写入输出文件: {}", display_path_for_user(path))};
    }
    if (bytes.size() > encoding_defaults::effective_max_input_file_bytes()) {
      return std::unexpected{std::format("输出内容超过当前运行时上限，无法写入输出文件: {}",
                                        display_path_for_user(path))};
    }
    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        return std::unexpected{std::format("无法创建输出目录 {}: {}",
                                           display_path_for_user(parent), ec.message())};
      }
    }
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
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
          return std::unexpected{std::move(failure.message)};
        }
      }
    }
    if (!temp_path) {
      return std::unexpected{std::format("无法创建唯一临时输出路径: {}",
                                         display_path_for_user(path))};
    }
    TempOutputFile temp{*temp_path};
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    if (auto attributes = clear_transient_file_attributes(*temp_path, "临时输出文件"); !attributes) {
      return std::unexpected{attributes.error()};
    }
    if (auto committed = commit_temp_output(*temp_path, path, replace_existing); !committed) {
      return std::unexpected{committed.error()};
    }
    temp.release();
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"写入输出文件时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"写入输出文件路径超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"写入输出文件时文件系统访问失败。"};
  }
}

std::expected<std::uintmax_t, std::string> copy_file_to_output(const fs::path& source,
                                                               const fs::path& path,
                                                               bool replace_existing,
                                                               std::stop_token stop_token = {}) {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    std::error_code source_size_error;
    const auto source_size = fs::file_size(source, source_size_error);
    if (source_size_error) {
      return std::unexpected{std::format("读取 AVIF 直通输入文件大小失败 {}: {}",
                                         display_path_for_user(source),
                                         source_size_error.message())};
    }
    if (source_size == 0) {
      return std::unexpected{std::format("AVIF 直通输入文件为空: {}",
                                         display_path_for_user(source))};
    }
    if (source_size > encoding_defaults::effective_max_input_file_bytes()) {
      return std::unexpected{std::format("AVIF 直通输入超过当前输入上限: {}",
                                         display_path_for_user(source))};
    }
    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        return std::unexpected{std::format("无法创建输出目录 {}: {}",
                                           display_path_for_user(parent), ec.message())};
      }
    }
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
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
        return std::unexpected{std::format("无法读取 AVIF 直通输入文件 {}: {}",
                                           display_path_for_user(source),
                                           win32_error_message(GetLastError()))};
      }
#else
      const int raw_source = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
      if (raw_source < 0) {
        return std::unexpected{std::format("无法读取 AVIF 直通输入文件 {}: {}",
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
          return std::unexpected{std::format("无法创建临时输出文件 {}: {}",
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
          return std::unexpected{std::format("无法创建临时输出文件 {}: {}",
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
        return std::unexpected{std::format("无法创建唯一临时输出路径: {}",
                                           display_path_for_user(path))};
      }

      std::vector<std::byte> buffer(encoding_defaults::output_copy_buffer_bytes);
      while (true) {
        if (stop_token.stop_requested()) {
          return std::unexpected{"任务已取消。"};
        }
#ifdef _WIN32
        DWORD read = 0;
        if (!ReadFile(source_file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
          return std::unexpected{std::format("读取 AVIF 直通输入文件失败 {}: {}",
                                             display_path_for_user(source),
                                             win32_error_message(GetLastError()))};
        }
#else
        const auto read = ::read(source_file.get(), buffer.data(), buffer.size());
        if (read < 0) {
          if (errno == EINTR) {
            continue;
          }
          return std::unexpected{std::format("读取 AVIF 直通输入文件失败 {}: {}",
                                             display_path_for_user(source),
                                             posix_error_message(errno))};
        }
#endif
        if (read == 0) {
          break;
        }
        if (copied_bytes > encoding_defaults::effective_max_input_file_bytes() - static_cast<std::uintmax_t>(read)) {
          return std::unexpected{std::format("AVIF 直通输入超过当前输入上限: {}",
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
            return std::unexpected{std::format("写入临时输出文件失败 {}: {}",
                                               display_path_for_user(*temp_path),
                                               win32_error_message(GetLastError()))};
          }
#else
          const auto written = ::write(output_file.get(), cursor, remaining);
          if (written < 0) {
            if (errno == EINTR) {
              continue;
            }
            return std::unexpected{std::format("写入临时输出文件失败 {}: {}",
                                               display_path_for_user(*temp_path),
                                               posix_error_message(errno))};
          }
#endif
          if (written == 0) {
            return std::unexpected{std::format("写入临时输出文件失败 {}", display_path_for_user(*temp_path))};
          }
          cursor += written;
          remaining -= written;
        }
        copied_bytes += static_cast<std::uintmax_t>(read);
      }
      if (copied_bytes == 0) {
        return std::unexpected{std::format("AVIF 直通输入文件为空: {}", display_path_for_user(source))};
      }
#ifdef _WIN32
      if (!FlushFileBuffers(output_file.get())) {
        return std::unexpected{std::format("刷新临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           win32_error_message(GetLastError()))};
      }
      const HANDLE output_handle = output_file.get();
      if (!CloseHandle(output_handle)) {
        return std::unexpected{std::format("关闭临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           win32_error_message(GetLastError()))};
      }
#else
      const int output_handle = output_file.get();
      if (::fsync(output_handle) != 0) {
        return std::unexpected{std::format("刷新临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           posix_error_message(errno))};
      }
      if (::close(output_handle) != 0) {
        return std::unexpected{std::format("关闭临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           posix_error_message(errno))};
      }
#endif
      output_file.release();
    }

    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    if (auto attributes = clear_transient_file_attributes(*temp_path, "临时输出文件"); !attributes) {
      return std::unexpected{attributes.error()};
    }
    if (auto committed = commit_temp_output(*temp_path, path, replace_existing); !committed) {
      return std::unexpected{committed.error()};
    }
    temp->release();
    return copied_bytes;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"复制输出文件时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"复制输出文件路径超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"复制输出文件时文件系统访问失败。"};
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
                              .visual_quality_gpu = cfg.visual_quality_gpu,
                              .jxl_jpeg_lossless_candidate = false,
                              .avif_tune_iq = encoding_defaults::default_avif_tune_iq,
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
  result.visual_quality_gpu_requested = native.diagnostics.visual_quality_gpu_requested;
  result.visual_quality_gpu_used = native.diagnostics.visual_quality_gpu_used;
  result.visual_quality_gpu_path = native.diagnostics.visual_quality_gpu_path;
  result.visual_quality_gpu_fallback_reason = native.diagnostics.visual_quality_gpu_fallback_reason;
  result.visual_quality_search_trace = native.diagnostics.visual_quality_search_trace;
  result.encoder_license = native.diagnostics.encoder_license;
  result.integration_mode = native.diagnostics.integration_mode;
  result.speed_parameter_kind = has_speed_mapping ? native.diagnostics.speed_mapping.codec_key : std::string{};
  result.applied_speed = has_speed_mapping ? native.diagnostics.speed_mapping.codec_value : -1;
  result.encoder_threads = native.diagnostics.encoder_threads;
  result.memory_budget_bytes = native.diagnostics.memory_budget_bytes;
  result.decode_seconds = native.diagnostics.timing.decode_seconds;
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
  result.visual_quality_gpu_fallback_count = native.diagnostics.timing.visual_quality_gpu_fallback_count;
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
  result.command = std::format("native:{}:{}:{} q{} speed={}",
                               format_name,
                               result.decoder_id,
                               native.diagnostics.encoder_id,
                               native.final_quality,
                               has_speed_mapping
                                   ? std::to_string(native.diagnostics.speed_mapping.user_speed)
                                   : std::string{"n/a"});
  if (native.diagnostics.encoder_id == "jpegli") {
    result.command += std::format(" chroma={} bit-depth={} progressive={} huffman={} xyb={}",
                                  result.applied_chroma.empty() ? "auto" : result.applied_chroma,
                                  result.applied_bit_depth ? std::format("{}", *result.applied_bit_depth) : std::string{"8"},
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
  result.visual_quality_gpu_requested = diagnostics.visual_quality_gpu_requested;
  result.visual_quality_gpu_used = diagnostics.visual_quality_gpu_used;
  result.visual_quality_gpu_path = diagnostics.visual_quality_gpu_path;
  result.visual_quality_gpu_fallback_reason = diagnostics.visual_quality_gpu_fallback_reason;
  result.speed = settings.speed;
  result.encoder_threads = settings.resources.encoder_threads_per_file;
  result.memory_budget_bytes = settings.resources.memory_limit_bytes;
}

}  // namespace native_backend_detail

class NativeBackend final {
  struct EncodeOverrides {
    std::optional<AvifEncoderMode> avif_encoder{};
    std::optional<GridPlan> avif_grid_plan{};
  };

 public:
  NativeBackend(const AppConfig& cfg, FileLogger& logger, ResourcePlan resources)
      : cfg_{cfg}, logger_{logger}, resources_{resources} {}

  EncodeResult encode(const ImageFile& image,
                      std::stop_token stop_token = {}) const {
    return encode_with_requested_file_times(image, EncodeOverrides{}, stop_token);
  }

  EncodeResult encode_avif_grid(const ImageFile& image,
                                GridPlan plan,
                                std::stop_token stop_token = {}) const {
    EncodeOverrides overrides{};
    overrides.avif_encoder = AvifEncoderMode::aom;
    overrides.avif_grid_plan = std::move(plan);
    return encode_with_requested_file_times(image, std::move(overrides), stop_token);
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

  static std::unexpected<PrepareError> prepare_failed(std::string message,
                                                       const NativeEncodeSettings& settings) {
    return std::unexpected{PrepareError{.message = std::move(message), .settings = settings}};
  }

  [[nodiscard]] EncodeResult initialize_result(const ImageFile& image) const {
    const auto planned_output_path = output_path_for(cfg_, image);
    auto output_path = image.output_path_resolved
                           ? std::expected<fs::path, std::string>{planned_output_path}
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
    result.message = std::move(message);
  }

  static bool cancel_if_requested(EncodeResult& result, std::stop_token stop_token) {
    if (!stop_token.stop_requested()) {
      return false;
    }
    result.canceled = true;
    result.processed = true;
    result.message = "任务已取消。";
    return true;
  }

  void append_success_warning(EncodeResult& result, std::string warning) const {
    if (warning.empty()) {
      return;
    }
    result.message = result.message.empty() || result.message == "OK"
                         ? std::move(warning)
                         : std::format("{}；{}", result.message, warning);
    try {
      logger_.warn(std::format("native encode warning: item={:04} {}",
                               result.index + 1, result.message));
    } catch (...) {
    }
  }

  EncodeResult encode_with_requested_file_times(
      const ImageFile& image, EncodeOverrides overrides,
      std::stop_token stop_token) const {
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
        time_warning = std::format("保留文件时间失败：{}", captured.error());
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
                             std::format("保留文件时间失败：{}", applied.error()));
    }
    return result;
#else
    return encode_with_overrides(image, std::move(overrides), stop_token);
#endif
  }

  [[nodiscard]] std::optional<EncodeResult> try_avif_lossless_passthrough(
      const ImageFile& image,
      const EncodeResult& base_result,
      EncodeStartedAt started,
      std::stop_token stop_token) const {
    if (!native_backend_detail::avif_lossless_passthrough_allowed(cfg_, image.path)) {
      return std::nullopt;
    }

    auto result = base_result;
    auto container_info = parse_avif_container_info(image.path);
    if (!container_info) {
      return std::nullopt;
    }
    if (!container_info->source_info ||
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
    const auto write_started = native_backend_detail::Clock::now();
    auto copied = native_backend_detail::copy_file_to_output(
        image.path, result.output_path, cfg_.collision_mode == CollisionMode::overwrite,
        stop_token);
    result.write_seconds = native_backend_detail::elapsed_seconds(write_started);
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
    native_backend_detail::populate_avif_passthrough_diagnostics(*container_info,
                                                                 cfg_.avif_encoder,
                                                                 cfg_.avif_color_representation,
                                                                 result);
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
      return std::format("native avif lossless passthrough ok: item={:04}",
                         result.index + 1);
    });
    return result;
  }

  [[nodiscard]] std::optional<EncodeResult> try_jxl_jpeg_bitstream_transcode(
      const ImageFile& image,
      const EncodeResult& base_result,
      EncodeStartedAt started,
      std::stop_token stop_token) const {
    if (!native_backend_detail::jxl_jpeg_bitstream_transcode_allowed(cfg_, image.path)) {
      return std::nullopt;
    }

    auto result = base_result;
    result.decoder_id = "jpeg-bitstream";
    result.encoder_id = "libjxl";
    result.integration_mode = "jxl-jpeg-bitstream-transcode";
    result.final_encoder_quality = 100;
    result.lossless = true;
    result.encoder_threads = resources_.encoder_threads_per_file;
    result.memory_budget_bytes = resources_.memory_limit_bytes;

    auto settings = native_backend_detail::settings_from_config(cfg_, resources_);
    settings.jxl_jpeg_lossless_candidate = true;

    auto bytes = decoder_common::read_file_bytes(image.path, "JPEG");
    if (!bytes) {
      mark_failed(result, redact_path_for_user(bytes.error(), image.path));
      return result;
    }
    if (native_backend_detail::inspect_jpeg_bitstream_source(*bytes).has_mpf) {
      return std::nullopt;
    }
    if (cancel_if_requested(result, stop_token)) {
      return result;
    }

    native_backend_detail::populate_jxl_jpeg_bitstream_source_diagnostics(
        settings, std::span<const std::byte>{*bytes}, cfg_);

    auto encoder = JXLImageEncoder{};
    const auto encode_started = native_backend_detail::Clock::now();
    auto encoded = encoder.encode_jpeg_bitstream(std::span<const std::byte>{*bytes},
                                                 settings, stop_token);
    result.encode_seconds = native_backend_detail::elapsed_seconds(encode_started);
    if (!encoded) {
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

    const auto write_started = native_backend_detail::Clock::now();
    if (auto written = native_backend_detail::write_output_bytes(
            result.output_path, std::span<const std::byte>{encoded->encoded.bytes},
            cfg_.collision_mode == CollisionMode::overwrite, stop_token);
        !written) {
      result.write_seconds = native_backend_detail::elapsed_seconds(write_started);
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
      } else {
        mark_failed(result, written.error());
      }
      return result;
    }
    result.write_seconds = native_backend_detail::elapsed_seconds(write_started);

    encoded->diagnostics.timing.encode_seconds = result.encode_seconds;
    encoded->diagnostics.timing.write_seconds = result.write_seconds;
    native_backend_detail::copy_native_result(*encoded, result);
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    result.processed = true;
    result.ok = true;
    result.message = "OK";
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return std::format("native jpeg bitstream transcode ok: item={:04}",
                         result.index + 1);
    });
    return result;
  }

  [[nodiscard]] std::expected<DecodedInput, std::string> decode_input(
      const ImageFile& image) const {
    auto decoded = decode_image_for_path(
        image.path,
        DecoderRegistryOptions{.allow_wic_fallback = cfg_.allow_wic_fallback,
                               .decode_threads = resources_.encoder_threads_per_file});
    if (!decoded) {
      return std::unexpected{redact_path_for_user(decoded.error(), image.path)};
    }

    const bool decoder_used_fallback = decoded->used_fallback;
    if (auto limited = native_backend_detail::apply_image_size_limit(*decoded, cfg_); !limited) {
      return std::unexpected{limited.error()};
    }
    return DecodedInput{.decoded = std::move(*decoded),
                        .decoder_used_fallback = decoder_used_fallback};
  }

  [[nodiscard]] std::expected<PreparedEncoding, PrepareError> prepare_encoding(
      const ImageFile& image,
      const ImageDecodeResult& decoded,
      EncodeOverrides overrides) const {
    PreparedEncoding prepared{};
    prepared.settings = native_backend_detail::settings_from_config(cfg_, resources_);
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
      if (prepared.settings.avif_grid_plan && cfg_.image_size_limit.mode == ImageSizeLimitMode::manual) {
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
           cfg_.chroma_mode == ChromaMode::yuv422)) {
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
        if (selection_requested_chroma == ChromaMode::auto_keep) {
          selection_requested_chroma = ChromaMode::yuv420;
        }
        prepared.settings.chroma_reason = std::format(
            "chroma auto 根据源图选择 {} chroma",
            chroma_mode_name(selection_requested_chroma));
      }

      std::optional<int> selection_requested_bit_depth{};
      if (cfg_.bit_depth) {
        selection_requested_bit_depth = cfg_.bit_depth;
        prepared.settings.bit_depth_reason = "用户明确请求 bit-depth";
      } else if (avif_lossless) {
        selection_requested_bit_depth =
            native_backend_detail::lossless_source_bit_depth(decoded.image);
        if (selection_requested_bit_depth &&
            *selection_requested_bit_depth < 10) {
          selection_requested_bit_depth = 10;
          prepared.settings.bit_depth_reason =
              "无损 auto 将低于 10-bit 的源图提升为 10-bit 输出";
        } else {
          prepared.settings.bit_depth_reason =
              native_backend_detail::lossless_uses_decoded_bit_depth(
                  decoded.image)
                  ? "无损模式继承解码后的 bit-depth"
                  : "无损模式继承源图 bit-depth";
        }
      } else if (prepared.settings.source_bit_depth && *prepared.settings.source_bit_depth >= 10) {
        selection_requested_bit_depth = prepared.settings.source_bit_depth;
        prepared.settings.bit_depth_reason = std::format(
            "lossy preserved source {}-bit depth", *prepared.settings.source_bit_depth);
      }
      if (!avif_lossless && !selection_requested_bit_depth &&
          prepared.settings.source_bit_depth && *prepared.settings.source_bit_depth == 8) {
        prepared.settings.bit_depth_reason = "有损 auto 可能将 8-bit 源图升至编码器首选 bit-depth";
      }
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
        return std::format(
            "AVIF decision: item={:04} user_encoder={} user_chroma={} source_chroma={} color_representation_requested={} color_representation_applied={} source_bit_depth={} alpha_policy={} source_alpha={} non_opaque_alpha={} selected_encoder={} requested_chroma={} applied_chroma={} requested_bit_depth={} applied_bit_depth={} applied_alpha={} fallback={} chroma_reason={} alpha_reason={} bit_depth_reason={} color_source={}",
            image.index + 1,
            prepared.settings.user_encoder_id,
            prepared.settings.user_chroma,
            prepared.settings.source_chroma,
            avif_color_representation_name(
                prepared.settings.requested_avif_color_representation),
            avif_color_representation_name(
                prepared.settings.avif_color_representation),
            prepared.settings.source_bit_depth ? std::format("{}", *prepared.settings.source_bit_depth) : std::string{""},
            prepared.settings.alpha_policy_name,
            prepared.settings.source_alpha_mode,
            *has_non_opaque_alpha ? "true" : "false",
            avif_encoder_mode_name(selection->applied_encoder),
            chroma_mode_name(selection->requested_chroma),
            chroma_mode_name(selection->applied_chroma),
            selection->requested_bit_depth ? std::format("{}", *selection->requested_bit_depth) : std::string{""},
            selection->applied_bit_depth ? std::format("{}", *selection->applied_bit_depth) : std::string{""},
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
        if (cfg_.output_format == OutputFormat::png && !cfg_.strip_metadata &&
            prepared.settings.applied_icc != "kept" && decoded.image.source_info &&
            (decoded.image.source_info->color_primaries ||
             decoded.image.source_info->transfer_characteristics)) {
          prepared.settings.applied_color_primaries = decoded.image.source_info->color_primaries;
          prepared.settings.applied_transfer_characteristics = decoded.image.source_info->transfer_characteristics;
          prepared.settings.applied_matrix_coefficients = 0;
          prepared.settings.applied_color_range = decoded.image.source_info->color_range.value_or(1);
          prepared.settings.applied_hdr_metadata = decoded.image.source_info->has_hdr_metadata ? "kept" : "none";
          prepared.settings.color_metadata_source = "png-cicp";
          prepared.settings.color_reason = "使用 PNG cICP 写入源图色彩/HDR 元数据";
        }
        if (cfg_.output_format == OutputFormat::png) {
          if (cfg_.bit_depth && *cfg_.bit_depth != decoded.image.bit_depth) {
            return prepare_failed(
                std::format("PNG 当前按解码 RGBA buffer 写入 {}-bit；请留空 bit-depth，或使用匹配的 {}。",
                            decoded.image.bit_depth, decoded.image.bit_depth),
                prepared.settings);
          }
          prepared.settings.bit_depth = decoded.image.bit_depth;
          prepared.settings.bit_depth_reason = cfg_.bit_depth
                                                   ? "用户明确请求 PNG 源图匹配 bit-depth"
                                                   : "PNG 继承解码 RGBA buffer bit-depth";
        }
        if (cfg_.output_format == OutputFormat::jpgli) {
          prepared.settings.user_chroma =
              chroma_mode_name(prepared.settings.requested_chroma_mode);
          prepared.settings.bit_depth = 8;
          prepared.settings.bit_depth_reason =
              cfg_.bit_depth ? "用户明确请求 JPGLI 8-bit 输出"
                             : "JPGLI 输出 JPEG 兼容 8-bit precision";
          prepared.settings.chroma_mode =
              cfg_.chroma_mode == ChromaMode::auto_keep && !cfg_.jpegli_xyb
                  ? ChromaMode::yuv444
                  : cfg_.chroma_mode;
          prepared.settings.chroma_reason =
              cfg_.chroma_mode == ChromaMode::auto_keep
                  ? (cfg_.jpegli_xyb ? "XYB 模式使用 Jpegli 默认 RGB/XYB 采样"
                                     : "JPGLI auto 使用 Jpegli 默认无色度降采样")
                  : "用户请求 JPGLI chroma";
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
      return prepare_failed(std::format("native backend 暂不支持输出格式: {}",
                                        output_format_name(cfg_.output_format)),
                            prepared.settings);
    }
    return std::move(prepared);
  }

  [[nodiscard]] std::expected<NativeEncodeResult, std::string> execute_encode(
      const ImageDecodeResult& decoded,
      ImageEncoder* encoder,
      const NativeEncodeSettings& settings,
      const fs::path& output_path,
      std::stop_token stop_token) const {
    const auto encode_started = native_backend_detail::Clock::now();
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    auto effective_settings = settings;
    const ImageBuffer* effective_image = &decoded.image;
    std::optional<native_backend_detail::SdrImageConversion> sdr_fallback;
    std::optional<ImageBuffer> materialized_scrgb_hdr;
    if (native_backend_detail::sdr_only_format_conversion_needed(cfg_.output_format,
                                                                 decoded.image)) {
      auto converted = native_backend_detail::rgba_to_rgba8_for_sdr_only_format(decoded.image);
      if (!converted) {
        return std::unexpected{converted.error()};
      }
      sdr_fallback = std::move(*converted);
      effective_image = &sdr_fallback->image;
      effective_settings.bit_depth = 8;
      if (sdr_fallback->hdr_tone_mapped) {
        effective_settings.applied_color_primaries = 1;
        effective_settings.applied_transfer_characteristics = 13;
        effective_settings.applied_matrix_coefficients = 0;
        effective_settings.applied_color_range = 1;
        effective_settings.applied_icc = "stripped-hdr-sdr";
        effective_settings.applied_hdr_metadata = "sdr-tone-map";
        effective_settings.color_metadata_source = "hdr-sdr-srgb";
        effective_settings.encoder_fallback_reason = "HDR -> SDR tone-map";
        effective_settings.color_reason = "目标格式不支持 HDR，已转换为 SDR sRGB";
      } else {
        effective_settings.bit_depth_reason = "目标格式只支持 8-bit，已按 SDR 缩减位深";
        if (effective_settings.source_has_hdr_metadata) {
          effective_settings.applied_hdr_metadata = "sdr-fallback";
          effective_settings.encoder_fallback_reason = "HDR -> SDR fallback";
          effective_settings.color_reason =
              "目标格式不支持源 HDR 元数据，已按 SDR 缩减位深";
        }
      }
    } else if (decoded.image.sample_representation == SampleRepresentation::ieee_half_float) {
      auto materialized = hdr::materialize_scrgb_as_hdr10(decoded.image);
      if (!materialized) {
        return std::unexpected{materialized.error()};
      }
      materialized_scrgb_hdr = std::move(*materialized);
      effective_image = &*materialized_scrgb_hdr;
      // materialize_scrgb_as_hdr10() produces a 16-bit UNORM RGB container for
      // libavif/JXL input conversion. For AVIF, the target bitstream depth was
      // already selected during prepare_encode() (for example 16-bit source ->
      // AOM 12-bit). Do not overwrite that encoder decision with the container
      // storage depth. Other HDR-capable formats still inherit the materialized
      // buffer depth as before.
      if (cfg_.output_format != OutputFormat::avif) {
        effective_settings.bit_depth = materialized_scrgb_hdr->bit_depth;
      }
      effective_settings.applied_color_primaries = 9;
      effective_settings.applied_transfer_characteristics = 16;
      effective_settings.applied_matrix_coefficients = 9;
      effective_settings.applied_color_range = 1;
      effective_settings.applied_icc = "stripped-scrgb-hdr";
      effective_settings.applied_hdr_metadata = "scrgb-to-hdr";
      effective_settings.color_metadata_source = "scrgb-linear-to-bt2020-pq";
      effective_settings.encoder_fallback_reason = "scRGB -> BT.2020/PQ";
      effective_settings.color_reason = "scRGB FP16 仅在 HDR 输出阶段转换为 BT.2020/PQ";
    }
    if (cfg_.visual_quality) {
      auto output_decoder = native_backend_detail::decoder_for_output_format(
          cfg_.output_format, effective_settings.resources.encoder_threads_per_file);
      const auto candidate_path = output_path.parent_path() /
                                  (output_path.filename().wstring() + L".candidate");


      auto search = encode_with_native_visual_quality_search(*effective_image, *encoder,
                                                             *output_decoder, effective_settings,
                                                             candidate_path, stop_token);
      if (!search) {
        return std::unexpected{search.error()};
      }
      search->encode_result.diagnostics.timing.encode_seconds =
          search->encode_result.diagnostics.timing.visual_quality_candidate_encode_seconds;
      search->encode_result.visual_quality_target_met = search->target_met;
      return std::move(search->encode_result);
    }


    auto encoded = encoder->encode(*effective_image, effective_settings, stop_token);
    if (encoded) {
      encoded->diagnostics.timing.encode_seconds =
          native_backend_detail::elapsed_seconds(encode_started);
    }
    return encoded;
  }

  [[nodiscard]] EncodeResult finalize_result(EncodeResult result,
                                             NativeEncodeResult encoded,
                                             const ImageDecodeResult& decoded,
                                             bool decoder_used_fallback,
                                             std::string avif_bit_depth_reason,
                                             EncodeStartedAt started,
                                             std::stop_token stop_token) const {
    const auto write_started = native_backend_detail::Clock::now();
    if (auto written = native_backend_detail::write_output_bytes(
            result.output_path, std::span<const std::byte>{encoded.encoded.bytes},
            cfg_.collision_mode == CollisionMode::overwrite, stop_token);
        !written) {
      result.write_seconds = native_backend_detail::elapsed_seconds(write_started);
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
      } else {
        mark_failed(result, written.error());
      }
      return result;
    }
    result.write_seconds = native_backend_detail::elapsed_seconds(write_started);

    encoded.diagnostics.decoder_id = decoded.decoder_id;
    encoded.diagnostics.used_decoder_fallback = decoder_used_fallback;
    native_backend_detail::merge_stage_timing(encoded, result);
    native_backend_detail::copy_native_result(encoded, result);
    if (!decoded.decoder_id.empty()) {
      result.decoder_id = decoded.decoder_id;
      result.command = std::format("native:{}:{}:{} q{} speed={}",
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
      return std::format("native encode ok: item={:04}", result.index + 1);
    });
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return std::format(
          "native timing: item={:04} total={}s decode={}s prepare={}s encode={}s vq={}s metrics={}s write={}s",
          result.index + 1,
          native_backend_detail::format_timing_seconds(result.seconds),
          native_backend_detail::format_timing_seconds(result.decode_seconds),
          native_backend_detail::format_timing_seconds(result.prepare_seconds),
          native_backend_detail::format_timing_seconds(result.encode_seconds),
          native_backend_detail::format_timing_seconds(result.visual_quality_search_seconds),
          native_backend_detail::format_timing_seconds(result.visual_quality_metric_seconds),
          native_backend_detail::format_timing_seconds(result.write_seconds));
    });
    if (result.visual_quality_search_seconds >= 0.0) {
      native_backend_detail::log_info_noexcept(logger_, [&] {
        return std::format(
            "native vq breakdown: item={:04} candidates={} memory_fallback_decodes={} gpu_requested={} gpu_used={} gpu_path={} gpu_fallbacks={} gpu_fallback_reason={} candidate_encode={}s candidate_decode={}s candidate_io={}s luma={}s gmsd={}s ms_ssim={}s metrics={}s metric_share={}%",
            result.index + 1,
            result.visual_quality_candidate_count,
            result.visual_quality_decode_memory_fallback_count,
            result.visual_quality_gpu_requested ? "true" : "false",
            result.visual_quality_gpu_used ? "true" : "false",
            result.visual_quality_gpu_path.empty() ? "-" : result.visual_quality_gpu_path,
            result.visual_quality_gpu_fallback_count,
            result.visual_quality_gpu_fallback_reason.empty()
                ? "-"
                : result.visual_quality_gpu_fallback_reason,
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
          return std::format("native vq trace: item={:04} {}",
                             result.index + 1,
                             result.visual_quality_search_trace);
        });
      }
    }
    return result;
  }

  EncodeResult encode_with_overrides(const ImageFile& image,
                                     EncodeOverrides overrides,
                                     std::stop_token stop_token = {}) const {
    try {
      const auto started = std::chrono::steady_clock::now();
      auto result = initialize_result(image);

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
          result.message = std::format("无法检查输出路径 {}: {}",
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

      if (auto passthrough = try_avif_lossless_passthrough(image, result, started, stop_token)) {
        return std::move(*passthrough);
      }

      if (auto transcode = try_jxl_jpeg_bitstream_transcode(image, result, started, stop_token)) {
        return std::move(*transcode);
      }

      const auto decode_started = native_backend_detail::Clock::now();
      auto decoded_input = decode_input(image);
      result.decode_seconds = native_backend_detail::elapsed_seconds(decode_started);
      if (!decoded_input) {
        mark_failed(result, decoded_input.error());
        return result;
      }

      if (cancel_if_requested(result, stop_token)) {
        return result;
      }

      const auto prepare_started = native_backend_detail::Clock::now();
      auto prepared = prepare_encoding(image, decoded_input->decoded, std::move(overrides));
      result.prepare_seconds = native_backend_detail::elapsed_seconds(prepare_started);
      if (!prepared) {
        native_backend_detail::copy_settings_diagnostics(prepared.error().settings, result);
        mark_failed(result, prepared.error().message);
        return result;
      }

      if (cancel_if_requested(result, stop_token)) {
        return result;
      }

      const auto encode_started = native_backend_detail::Clock::now();
      auto encoded = execute_encode(decoded_input->decoded, prepared->encoder.get(),
                                    prepared->settings, result.output_path, stop_token);
      result.encode_seconds = native_backend_detail::elapsed_seconds(encode_started);
      if (!encoded) {
        native_backend_detail::copy_settings_diagnostics(prepared->settings, result);
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

      return finalize_result(std::move(result), std::move(*encoded), decoded_input->decoded,
                             decoded_input->decoder_used_fallback,
                             std::move(prepared->avif_bit_depth_reason), started, stop_token);
    } catch (const std::bad_alloc&) {
      return failed_encode_result(image, "native backend 单项转换内存不足。");
    } catch (const std::length_error&) {
      return failed_encode_result(image, "native backend 单项转换数据超过运行时限制。");
    } catch (const std::filesystem::filesystem_error&) {
      return failed_encode_result(image, "native backend 单项转换文件系统访问失败。");
    }
  }

 private:
  const AppConfig& cfg_;
  FileLogger& logger_;
  ResourcePlan resources_;
};

}  // namespace awj
