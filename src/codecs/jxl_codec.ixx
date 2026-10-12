module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <stop_token>
#include <utility>
#include <vector>

#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

export module awj.jxl_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.pixel_conversion;
import awj.large_image_plan;

export namespace awj {

namespace jxl_detail {

struct DecoderDeleter {
  void operator()(JxlDecoder* value) const noexcept {
    if (value != nullptr) {
      JxlDecoderDestroy(value);
    }
  }
};

struct EncoderDeleter {
  void operator()(JxlEncoder* value) const noexcept {
    if (value != nullptr) {
      JxlEncoderDestroy(value);
    }
  }
};

struct RunnerDeleter {
  void operator()(void* value) const noexcept {
    if (value != nullptr) {
      JxlThreadParallelRunnerDestroy(value);
    }
  }
};

using DecoderPtr = std::unique_ptr<JxlDecoder, DecoderDeleter>;
using EncoderPtr = std::unique_ptr<JxlEncoder, EncoderDeleter>;
using RunnerPtr = std::unique_ptr<void, RunnerDeleter>;

constexpr JxlBoxType exif_box_type{'E', 'x', 'i', 'f'};
constexpr JxlBoxType xmp_box_type{'x', 'm', 'l', ' '};

std::expected<bool, std::string> provide_basic_info_probe_input(
    JxlDecoder* decoder,
    std::ifstream& input,
    std::vector<std::uint8_t>& buffer,
    bool& input_closed,
    const fs::path& path) {
  if (input_closed) {
    return false;
  }

  const auto remaining = JxlDecoderReleaseInput(decoder);
  if (remaining > buffer.size()) {
    return std::unexpected{std::format("JXL decoder 未处理输入尺寸无效: {}", display_path_for_user(path))};
  }

  std::vector<std::uint8_t> next;
  if (remaining > 0) {
    next.insert(next.end(), buffer.end() - static_cast<std::ptrdiff_t>(remaining), buffer.end());
  }

  const auto hint = JxlDecoderSizeHintBasicInfo(decoder);
  const auto requested_size = std::clamp(
      hint == 0 ? encoding_defaults::jxl_min_basic_info_probe_bytes : hint,
      encoding_defaults::jxl_min_basic_info_probe_bytes,
      encoding_defaults::jxl_max_basic_info_probe_bytes);
  const auto previous_size = next.size();
  if (previous_size >= encoding_defaults::jxl_max_basic_info_probe_bytes) {
    return std::unexpected{std::format("JXL 尺寸探测输入超过 1 MiB 上限: {}",
                                       display_path_for_user(path))};
  }
  const auto read_size = std::min(
      requested_size,
      encoding_defaults::jxl_max_basic_info_probe_bytes - previous_size);
  if (read_size > std::numeric_limits<std::size_t>::max() - previous_size) {
    return std::unexpected{std::format("JXL 尺寸探测输入 buffer 过大: {}", display_path_for_user(path))};
  }
  try {
    next.resize(previous_size + read_size);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"JXL 尺寸探测输入 buffer 内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"JXL 尺寸探测输入 buffer 尺寸超过运行时限制。"};
  }
  input.read(reinterpret_cast<char*>(next.data() + previous_size),
             static_cast<std::streamsize>(read_size));
  const auto read_count = input.gcount();
  if (input.bad()) {
    return std::unexpected{std::format("读取 JXL 文件失败: {}", display_path_for_user(path))};
  }
  try {
    next.resize(previous_size + static_cast<std::size_t>(read_count));
  } catch (const std::length_error&) {
    return std::unexpected{"JXL 尺寸探测输入 buffer 尺寸超过运行时限制。"};
  }
  buffer = std::move(next);

  if (!buffer.empty() && JxlDecoderSetInput(decoder, buffer.data(), buffer.size()) !=
                             JXL_DEC_SUCCESS) {
    return std::unexpected{"设置 JXL 输入 buffer 失败。"};
  }
  if (input.eof()) {
    JxlDecoderCloseInput(decoder);
    input_closed = true;
  }
  return !buffer.empty();
}

std::expected<std::vector<std::byte>, std::string> read_file_bytes(
    const fs::path& path) {
  return decoder_common::read_file_bytes(path, "JXL");
}

std::expected<std::size_t, std::string> checked_rgba_stride(
    std::size_t width,
    std::string_view context,
    std::size_t bytes_per_sample = 1) {
  if (width == 0 || bytes_per_sample == 0) {
    return std::unexpected{std::format("{} 输入宽度无效。", context)};
  }
  if (width > std::numeric_limits<std::size_t>::max() / 4 / bytes_per_sample) {
    return std::unexpected{std::format("{} 输入宽度过大。", context)};
  }
  return width * 4 * bytes_per_sample;
}

std::expected<std::size_t, std::string> checked_image_bytes(std::size_t stride,
                                                           std::size_t height,
                                                           std::string_view context) {
  if (stride == 0 || height == 0) {
    return std::unexpected{std::format("{} 输入尺寸无效。", context)};
  }
  if (height > std::numeric_limits<std::size_t>::max() / stride) {
    return std::unexpected{std::format("{} 输入尺寸过大。", context)};
  }
  const auto byte_count = stride * height;
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{std::format("{} 图像 buffer 超过当前运行时上限。", context)};
  }
  return byte_count;
}

std::expected<const ImagePlane*, std::string> rgba_plane(const ImageBuffer& image) {
  if (image.pixel_format != PixelFormat::rgba ||
      (image.bit_depth != 8 && image.bit_depth != 16) ||
      image.planes.empty()) {
    return std::unexpected{"JXL encoder 当前需要 RGBA ImageBuffer。"};
  }
  const auto& plane = image.planes.front();
  const auto bytes_per_sample = image.bit_depth > 8 ? std::size_t{2} : std::size_t{1};
  const auto expected_stride = checked_rgba_stride(image.width, "JXL encoder", bytes_per_sample);
  if (!expected_stride) {
    return std::unexpected{expected_stride.error()};
  }
  if (plane.stride != *expected_stride) {
    return std::unexpected{"JXL encoder 当前需要紧凑排列的 RGBA buffer。"};
  }
  const auto expected_bytes = checked_image_bytes(plane.stride, image.height, "JXL encoder");
  if (!expected_bytes) {
    return std::unexpected{expected_bytes.error()};
  }
  if (plane.bytes.size() < *expected_bytes) {
    return std::unexpected{"JXL encoder 输入 RGBA buffer 尺寸无效。"};
  }
  return &plane;
}

std::expected<std::vector<std::byte>, std::string> copy_embedded_icc_profile(
    JxlDecoder* decoder,
    const JxlBasicInfo& info) {
  if (info.uses_original_profile != JXL_TRUE) {
    return std::vector<std::byte>{};
  }

  const auto encoded_status = JxlDecoderGetColorAsEncodedProfile(
      decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL, nullptr);
  if (encoded_status == JXL_DEC_SUCCESS) {
    return std::vector<std::byte>{};
  }
  if (encoded_status != JXL_DEC_ERROR) {
    return std::unexpected{"读取 JXL 色彩 profile 类型失败。"};
  }

  std::size_t icc_size = 0;
  const auto size_status = JxlDecoderGetICCProfileSize(
      decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &icc_size);
  if (size_status == JXL_DEC_ERROR || icc_size == 0) {
    return std::vector<std::byte>{};
  }
  if (size_status != JXL_DEC_SUCCESS) {
    return std::unexpected{"读取 JXL ICC profile 大小失败。"};
  }
  if (icc_size > encoding_defaults::codec_metadata_max_bytes) {
    return std::unexpected{"JXL ICC profile 超过 64 MiB 上限。"};
  }
  auto bytes = decoder_common::make_byte_buffer(icc_size, "JXL ICC profile");
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  if (JxlDecoderGetColorAsICCProfile(
          decoder,
          JXL_COLOR_PROFILE_TARGET_ORIGINAL,
          reinterpret_cast<std::uint8_t*>(bytes->data()),
          bytes->size()) != JXL_DEC_SUCCESS) {
    return std::unexpected{"读取 JXL ICC profile 失败。"};
  }
  return std::move(*bytes);
}

const MetadataBlock* first_metadata(const ImageBuffer& image, MetadataKind kind) noexcept {
  for (const auto& block : image.metadata) {
    if (block.kind == kind && !block.bytes.empty()) {
      return &block;
    }
  }
  return nullptr;
}

const MetadataBlock* first_icc_metadata(const ImageBuffer& image) noexcept {
  return first_metadata(image, MetadataKind::icc);
}

PixelFormat pixel_format_from_basic_info(const JxlBasicInfo& info) noexcept {
  if (info.alpha_bits != 0) {
    return PixelFormat::rgba;
  }
  if (info.num_color_channels == 1) {
    return PixelFormat::gray;
  }
  if (info.num_color_channels == 3) {
    return PixelFormat::rgb;
  }
  return PixelFormat::unknown;
}

int bit_depth_from_basic_info(const JxlBasicInfo& info) noexcept {
  if (info.bits_per_sample > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    return 0;
  }
  return static_cast<int>(info.bits_per_sample);
}

ImageSourceInfo source_info_from_basic_info(const JxlBasicInfo& info) {
  const int bit_depth = bit_depth_from_basic_info(info);
  return ImageSourceInfo{.pixel_format = pixel_format_from_basic_info(info),
                          .bit_depth = bit_depth,
                          .color_metadata_source = bit_depth > 8 ? "jxl-high-bit-depth" : ""};
}

void apply_encoded_color_profile(ImageSourceInfo& source, const JxlColorEncoding& color) {
  if (color.color_space != JXL_COLOR_SPACE_RGB) {
    return;
  }
  const auto transfer = static_cast<int>(color.transfer_function);
  if (color.primaries == JXL_PRIMARIES_SRGB || color.primaries == JXL_PRIMARIES_2100) {
    source.color_primaries = static_cast<int>(color.primaries);
  }
  source.transfer_characteristics = transfer;
  source.matrix_coefficients = 0;
  source.color_range = 1;
  source.has_hdr_metadata = color.transfer_function == JXL_TRANSFER_FUNCTION_PQ ||
                            color.transfer_function == JXL_TRANSFER_FUNCTION_HLG;
  source.color_metadata_source = "jxl-color-encoding";
}

std::uint32_t read_be_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

bool has_standard_metadata_boxes(const ImageBuffer& image, const NativeEncodeSettings& settings) noexcept {
  return !settings.strip_metadata &&
         (first_metadata(image, MetadataKind::exif) != nullptr ||
          first_metadata(image, MetadataKind::xmp) != nullptr);
}

std::expected<void, std::string> add_metadata_box(JxlEncoder* encoder,
                                                  const JxlBoxType& type,
                                                  std::span<const std::byte> bytes,
                                                  std::string_view context) {
  if (bytes.size() > encoding_defaults::codec_metadata_max_bytes) {
    return std::unexpected{std::format("JXL {} metadata 超过 64 MiB 上限。", context)};
  }
  if (JxlEncoderAddBox(encoder,
                       type,
                       reinterpret_cast<const std::uint8_t*>(bytes.data()),
                       bytes.size(),
                       JXL_FALSE) != JXL_ENC_SUCCESS) {
    return std::unexpected{std::format("写入 JXL {} box 失败。", context)};
  }
  return {};
}

std::expected<void, std::string> add_standard_metadata_boxes(JxlEncoder* encoder,
                                                            const ImageBuffer& image,
                                                            const NativeEncodeSettings& settings) {
  if (!has_standard_metadata_boxes(image, settings)) {
    return {};
  }

  if (const auto* exif = first_metadata(image, MetadataKind::exif); exif != nullptr) {
    if (exif->bytes.size() >
        encoding_defaults::codec_metadata_max_bytes - 4) {
      return std::unexpected{"JXL Exif metadata 超过 64 MiB 上限。"};
    }
    auto exif_payload = decoder_common::make_byte_buffer(exif->bytes.size() + 4, "JXL Exif metadata");
    if (!exif_payload) {
      return std::unexpected{exif_payload.error()};
    }
    std::ranges::copy(exif->bytes, exif_payload->begin() + 4);
    if (auto added = add_metadata_box(encoder, exif_box_type, *exif_payload, "Exif"); !added) {
      return std::unexpected{added.error()};
    }
  }

  if (const auto* xmp = first_metadata(image, MetadataKind::xmp); xmp != nullptr) {
    if (auto added = add_metadata_box(encoder, xmp_box_type, xmp->bytes, "XMP"); !added) {
      return std::unexpected{added.error()};
    }
  }

  return {};
}

std::expected<void, std::string> apply_color_profile(JxlEncoder* encoder,
                                                     const ImageBuffer& image,
                                                     const NativeEncodeSettings& settings) {
  if (!settings.strip_metadata && settings.applied_icc == "kept") {
    if (const auto* icc = first_icc_metadata(image); icc != nullptr) {
      if (icc->bytes.size() > encoding_defaults::codec_metadata_max_bytes) {
        return std::unexpected{"JXL ICC profile 超过 64 MiB 上限。"};
      }
      if (JxlEncoderSetICCProfile(
              encoder,
              reinterpret_cast<const std::uint8_t*>(icc->bytes.data()),
              icc->bytes.size()) != JXL_ENC_SUCCESS) {
        return std::unexpected{"设置 JXL ICC profile 失败。"};
      }
      return {};
    }
  }

  JxlColorEncoding color{};
  if (image.source_info && image.source_info->color_primaries == 9 &&
      image.source_info->transfer_characteristics == 16) {
    color.color_space = JXL_COLOR_SPACE_RGB;
    color.white_point = JXL_WHITE_POINT_D65;
    color.primaries = JXL_PRIMARIES_2100;
    color.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
    color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
  } else {
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
  }
  if (JxlEncoderSetColorEncoding(encoder, &color) != JXL_ENC_SUCCESS) {
    return std::unexpected{"设置 JXL 色彩信息失败。"};
  }
  return {};
}

std::expected<std::vector<std::byte>, std::string> make_rgb_buffer(const ImagePlane& plane,
                                                                  std::size_t width,
                                                                  std::size_t height,
                                                                  int bit_depth) {
  const auto bytes_per_sample = bit_depth > 8 ? std::size_t{2} : std::size_t{1};
  if (width == 0 || width > std::numeric_limits<std::size_t>::max() / 3 / bytes_per_sample) {
    return std::unexpected{"JXL encoder RGB 输入宽度无效。"};
  }
  const auto rgb_stride = width * 3 * bytes_per_sample;
  const auto rgb_size = checked_image_bytes(rgb_stride, height, "JXL encoder");
  if (!rgb_size) {
    return std::unexpected{rgb_size.error()};
  }
  auto rgb = decoder_common::make_byte_buffer(*rgb_size, "JXL encoder RGB");
  if (!rgb) {
    return std::unexpected{rgb.error()};
  }
  const auto* rgba_data = reinterpret_cast<const std::byte*>(plane.bytes.data());
  auto* rgb_data = reinterpret_cast<std::byte*>(rgb->data());
  for (std::size_t y = 0; y < height; ++y) {
    const auto* rgba_row = rgba_data + y * plane.stride;
    auto* rgb_row = rgb_data + y * rgb_stride;
    for (std::size_t x = 0; x < width; ++x) {
      const auto src = x * 4 * bytes_per_sample;
      const auto dst = x * 3 * bytes_per_sample;
      std::ranges::copy_n(rgba_row + src, bytes_per_sample, rgb_row + dst);
      std::ranges::copy_n(rgba_row + src + bytes_per_sample, bytes_per_sample,
                          rgb_row + dst + bytes_per_sample);
      std::ranges::copy_n(rgba_row + src + bytes_per_sample * 2, bytes_per_sample,
                          rgb_row + dst + bytes_per_sample * 2);
    }
  }
  return rgb;
}

struct MetadataBoxBudget {
  std::size_t count = 0;
  std::size_t bytes = 0;
};

std::expected<void, std::string> reserve_standard_metadata_box_bytes(
    MetadataBoxBudget& budget,
    std::size_t byte_count) {
  if (byte_count > encoding_defaults::codec_metadata_max_bytes ||
      budget.bytes > encoding_defaults::codec_metadata_max_bytes - byte_count) {
    return std::unexpected{"JXL metadata 累计大小超过 64 MiB 上限。"};
  }
  budget.bytes += byte_count;
  return {};
}

std::expected<void, std::string> begin_standard_metadata_box(
    MetadataBoxBudget& budget,
    std::size_t byte_count) {
  if (budget.count >= encoding_defaults::jxl_max_metadata_box_count) {
    return std::unexpected{"JXL metadata box 数量超过 16 个上限。"};
  }
  if (auto reserved = reserve_standard_metadata_box_bytes(budget, byte_count); !reserved) {
    return std::unexpected{reserved.error()};
  }
  ++budget.count;
  return {};
}

std::expected<void, std::string> append_standard_metadata_box(std::vector<MetadataBlock>& metadata,
                                                              const JxlBoxType& type,
                                                              const std::vector<std::byte>& bytes) {
  if (bytes.empty()) {
    return {};
  }
  if (std::ranges::equal(type, exif_box_type)) {
    if (bytes.size() <= 4) {
      return {};
    }
    const auto tiff_offset = static_cast<std::size_t>(read_be_u32(
        std::span<const std::byte>{bytes.data(), bytes.size()}, 0));
    if (tiff_offset >= bytes.size() - 4) {
      return {};
    }
    const auto exif_start = 4 + tiff_offset;
    const auto exif_size = bytes.size() - exif_start;
    auto exif = decoder_common::make_byte_buffer(exif_size, "JXL Exif metadata");
    if (!exif) {
      return std::unexpected{exif.error()};
    }
    std::ranges::copy(bytes.begin() + static_cast<std::ptrdiff_t>(exif_start),
                      bytes.end(), exif->begin());
    metadata.push_back(MetadataBlock{.kind = MetadataKind::exif,
                                     .bytes = std::move(*exif)});
    return {};
  }
  if (std::ranges::equal(type, xmp_box_type)) {
    metadata.push_back(MetadataBlock{.kind = MetadataKind::xmp,
                                     .bytes = bytes});
  }
  return {};
}

int codec_thread_count(int requested_threads) noexcept {
  return std::clamp(requested_threads, 1,
                    encoding_defaults::max_automatic_thread_budget);
}

std::expected<RunnerPtr, std::string> create_runner(int requested_threads) {
  const auto threads = static_cast<std::size_t>(codec_thread_count(requested_threads));
  RunnerPtr runner{JxlThreadParallelRunnerCreate(nullptr, threads)};
  if (!runner) {
    return std::unexpected{"创建 JXL 线程 runner 失败。"};
  }
  return runner;
}

std::expected<void, std::string> attach_decoder_runner(JxlDecoder* decoder,
                                                       void* runner) {
  if (JxlDecoderSetParallelRunner(decoder, JxlThreadParallelRunner, runner) !=
      JXL_DEC_SUCCESS) {
    return std::unexpected{"设置 JXL decoder 线程 runner 失败。"};
  }
  return {};
}

std::expected<void, std::string> attach_encoder_runner(JxlEncoder* encoder,
                                                       void* runner) {
  if (JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner, runner) !=
      JXL_ENC_SUCCESS) {
    return std::unexpected{"设置 JXL encoder 线程 runner 失败。"};
  }
  return {};
}

std::size_t initial_encoder_output_buffer_size(
    std::size_t input_size_hint) noexcept {
  if (input_size_hint == 0) {
    return encoding_defaults::jxl_min_encoder_output_buffer_bytes;
  }
  const auto scaled_hint = std::max(
      encoding_defaults::jxl_min_encoder_output_buffer_bytes,
      input_size_hint / 8);
  return std::min(
      scaled_hint,
      encoding_defaults::jxl_max_initial_encoder_output_buffer_bytes);
}

std::expected<std::vector<std::byte>, std::string> collect_encoder_output(
    JxlEncoder* encoder,
    std::size_t input_size_hint = 0,
    std::stop_token stop_token = {}) {
  std::vector<std::byte> output;
  const auto initial_size = initial_encoder_output_buffer_size(input_size_hint);
  try {
    output.resize(initial_size);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"JXL 编码输出缓冲区内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"JXL 编码输出缓冲区尺寸超过运行时限制。"};
  }
  auto* next_out = reinterpret_cast<std::uint8_t*>(output.data());
  auto avail_out = output.size();

  while (true) {
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    const auto status = JxlEncoderProcessOutput(encoder, &next_out, &avail_out);
    if (avail_out > output.size()) {
      return std::unexpected{"JXL 编码输出剩余尺寸无效。"};
    }
    const auto used = output.size() - avail_out;
    if (status == JXL_ENC_SUCCESS) {
      if (used == 0) {
        return std::unexpected{"JXL 编码输出为空。"};
      }
      output.resize(used);
      return std::move(output);
    }
    if (status != JXL_ENC_NEED_MORE_OUTPUT) {
      return std::unexpected{std::format("JXL 编码失败，错误码: {}",
                                         static_cast<int>(JxlEncoderGetError(encoder)))};
    }

    const auto old_size = output.size();
    if (old_size > std::numeric_limits<std::size_t>::max() / 2 ||
        static_cast<std::uint64_t>(old_size) > encoding_defaults::effective_max_input_file_bytes() / 2) {
      return std::unexpected{"JXL 编码输出过大。"};
    }
    try {
      output.resize(old_size * 2);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JXL 编码输出缓冲区内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JXL 编码输出缓冲区尺寸超过运行时限制。"};
    }
    next_out = reinterpret_cast<std::uint8_t*>(output.data() + used);
    avail_out = output.size() - used;
  }
}

}  // namespace jxl_detail

class JXLImageDecoder final : public ImageDecoder {
 public:
  explicit JXLImageDecoder(int decode_threads = 1)
      : decode_threads_{jxl_detail::codec_thread_count(decode_threads)} {}

  [[nodiscard]] std::string_view id() const noexcept override { return "libjxl"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    auto ext = path.extension().wstring();
    std::ranges::transform(ext, ext.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    return ext == L".jxl";
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      std::ifstream input{path, std::ios::binary};
      if (!input) {
        return std::unexpected{std::format("无法读取 JXL 文件: {}", display_path_for_user(path))};
      }
      input.seekg(0, std::ios::end);
      if (!input) {
        return std::unexpected{std::format("读取 JXL 文件大小失败: {}", display_path_for_user(path))};
      }
      const auto size = input.tellg();
      if (size < 0) {
        return std::unexpected{std::format("读取 JXL 文件大小失败: {}", display_path_for_user(path))};
      }
      if (size == 0) {
        return std::unexpected{std::format("JXL 文件为空: {}", display_path_for_user(path))};
      }
      const auto file_size = static_cast<std::uint64_t>(size);
      if (file_size > encoding_defaults::effective_max_input_file_bytes()) {
        return std::unexpected{std::format(
            "JXL 文件超过当前输入上限: {}", display_path_for_user(path))};
      }
      input.seekg(0, std::ios::beg);
      if (!input) {
        return std::unexpected{std::format("读取 JXL 文件失败: {}", display_path_for_user(path))};
      }

      jxl_detail::DecoderPtr decoder{JxlDecoderCreate(nullptr)};
      if (!decoder) {
        return std::unexpected{"创建 JXL decoder 失败。"};
      }
      auto runner = jxl_detail::create_runner(1);
      if (!runner) {
        return std::unexpected{runner.error()};
      }
      if (auto attached = jxl_detail::attach_decoder_runner(decoder.get(), runner->get());
          !attached) {
        return std::unexpected{attached.error()};
      }
      if (JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS) {
        return std::unexpected{"订阅 JXL decoder 事件失败。"};
      }

      std::vector<std::uint8_t> probe_input;
      bool input_closed = false;
      auto provided = jxl_detail::provide_basic_info_probe_input(
          decoder.get(), input, probe_input, input_closed, path);
      if (!provided) {
        return std::unexpected{provided.error()};
      }
      if (!*provided) {
        return std::unexpected{std::format("JXL 文件为空: {}", display_path_for_user(path))};
      }

      while (true) {
        const auto status = JxlDecoderProcessInput(decoder.get());
        if (status == JXL_DEC_ERROR) {
          return std::unexpected{std::format("JXL 读取尺寸失败: {}", display_path_for_user(path))};
        }
        if (status == JXL_DEC_NEED_MORE_INPUT) {
          if (input_closed) {
            return std::unexpected{std::format("JXL 输入不完整: {}", display_path_for_user(path))};
          }
          provided = jxl_detail::provide_basic_info_probe_input(
              decoder.get(), input, probe_input, input_closed, path);
          if (!provided) {
            return std::unexpected{provided.error()};
          }
          continue;
        }
        if (status == JXL_DEC_BASIC_INFO) {
          JxlBasicInfo info{};
          if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS) {
            return std::unexpected{std::format("JXL 基础信息无效: {}", display_path_for_user(path))};
          }
          return decoder_common::make_image_dimensions_checked(info.xsize, info.ysize,
                                                               "JXL");
        }
        if (status == JXL_DEC_SUCCESS) {
          return std::unexpected{std::format("JXL 未返回基础信息: {}", display_path_for_user(path))};
        }
      }
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JXL 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JXL 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"JXL 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode_memory(
      std::span<const std::byte> bytes,
      std::string_view source_name,
      DecodeOptions options = {}) const override {
    return decode_bytes(bytes, source_name, decode_threads_,
                        options.copy_metadata_payloads.value_or(false));
  }

  std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const override {
    try {
      auto bytes = jxl_detail::read_file_bytes(path);
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      return decode_bytes(*bytes, display_path_for_user(path), decode_threads_, true);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JXL 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JXL 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"JXL 解码文件系统访问失败。"};
    }
  }

 private:
  static std::expected<ImageDecodeResult, std::string> decode_bytes(
      std::span<const std::byte> bytes,
      std::string_view source_name,
      int decode_threads,
      bool copy_metadata_payloads) {
    try {
      if (bytes.empty()) {
        return std::unexpected{std::format("JXL 输入为空: {}", source_name)};
      }
      jxl_detail::DecoderPtr decoder{JxlDecoderCreate(nullptr)};
      if (!decoder) {
        return std::unexpected{"创建 JXL decoder 失败。"};
      }
      if (JxlDecoderSetUnpremultiplyAlpha(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS) {
        return std::unexpected{"启用 JXL alpha 去预乘失败。"};
      }
      const bool decompress_metadata_boxes =
          copy_metadata_payloads && JxlDecoderSetDecompressBoxes(decoder.get(), JXL_TRUE) == JXL_DEC_SUCCESS;

      auto runner = jxl_detail::create_runner(decode_threads);
      if (!runner) {
        return std::unexpected{runner.error()};
      }
      if (auto attached = jxl_detail::attach_decoder_runner(decoder.get(), runner->get());
          !attached) {
        return std::unexpected{attached.error()};
      }

      int subscribed_events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE;
      if (copy_metadata_payloads) {
        subscribed_events |= JXL_DEC_BOX | JXL_DEC_BOX_COMPLETE;
      }
      if (JxlDecoderSubscribeEvents(decoder.get(), subscribed_events) != JXL_DEC_SUCCESS) {
        return std::unexpected{"订阅 JXL decoder 事件失败。"};
      }

      const auto* input = reinterpret_cast<const std::uint8_t*>(bytes.data());
      if (JxlDecoderSetInput(decoder.get(), input, bytes.size()) != JXL_DEC_SUCCESS) {
        return std::unexpected{"设置 JXL 输入 buffer 失败。"};
      }
      JxlDecoderCloseInput(decoder.get());

      JxlBasicInfo info{};
      std::optional<JxlColorEncoding> encoded_color_profile;
      std::vector<std::byte> rgba;
      std::vector<std::byte> icc_profile;
      std::vector<MetadataBlock> metadata_boxes;
      jxl_detail::MetadataBoxBudget metadata_budget;
      JxlBoxType current_box_type{};
      std::vector<std::byte> current_box;
      bool current_box_active = false;
      bool full_image_received = false;
      JxlPixelFormat format{.num_channels = 4,
                            .data_type = JXL_TYPE_UINT8,
                            .endianness = JXL_NATIVE_ENDIAN,
                            .align = 0};

      auto finish_current_box = [&]() -> std::expected<void, std::string> {
        if (!current_box_active) {
          return {};
        }
        const auto remaining = JxlDecoderReleaseBoxBuffer(decoder.get());
        if (remaining > current_box.size()) {
          return std::unexpected{"JXL metadata box 输出尺寸无效。"};
        }
        current_box.resize(current_box.size() - remaining);
        if (auto appended = jxl_detail::append_standard_metadata_box(
                metadata_boxes, current_box_type, current_box);
            !appended) {
          return std::unexpected{appended.error()};
        }
        current_box.clear();
        current_box_active = false;
        return {};
      };

      auto finish_image = [&]() -> std::expected<ImageDecodeResult, std::string> {
        if (auto finished = finish_current_box(); !finished) {
          return std::unexpected{finished.error()};
        }
        if (!full_image_received) {
          return std::unexpected{std::format("JXL 解码结束但未得到完整图像: {}",
                                             source_name)};
        }
        const auto stride = jxl_detail::checked_rgba_stride(
            static_cast<std::size_t>(info.xsize), "JXL decoder",
            info.bits_per_sample > 8 ? 2 : 1);
        if (!stride) {
          return std::unexpected{stride.error()};
        }
        const auto alpha_mode =
            info.alpha_bits == 0 ? AlphaMode::none : AlphaMode::straight;
        auto source_info = jxl_detail::source_info_from_basic_info(info);
        if (encoded_color_profile) {
          jxl_detail::apply_encoded_color_profile(source_info, *encoded_color_profile);
        }
        ImagePlane plane{.bytes = std::move(rgba), .stride = *stride};
        ImageBuffer image{.width = info.xsize,
                          .height = info.ysize,
                          .pixel_format = PixelFormat::rgba,
                          .alpha_mode = alpha_mode,
                           .bit_depth = info.bits_per_sample > 8 ? 16 : 8,
                           .source_info = std::move(source_info)};
        if (!icc_profile.empty()) {
          image.metadata.push_back(MetadataBlock{.kind = MetadataKind::icc,
                                                 .bytes = std::move(icc_profile)});
        }
        for (auto& metadata : metadata_boxes) {
          image.metadata.push_back(std::move(metadata));
        }
        image.planes.push_back(std::move(plane));
        return ImageDecodeResult{.image = std::move(image), .decoder_id = "libjxl"};
      };

      while (true) {
        const auto status = JxlDecoderProcessInput(decoder.get());
        if (status == JXL_DEC_ERROR) {
          return std::unexpected{std::format("JXL 解码失败: {}", source_name)};
        }
        if (status == JXL_DEC_NEED_MORE_INPUT) {
          return std::unexpected{std::format("JXL 输入不完整: {}", source_name)};
        }
        if (status == JXL_DEC_BASIC_INFO) {
          if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS ||
              info.xsize == 0 || info.ysize == 0) {
            return std::unexpected{std::format("JXL 基础信息无效: {}", source_name)};
          }
          const int output_bit_depth = info.bits_per_sample > 8 ? 16 : 8;
          format.data_type = output_bit_depth > 8 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8;
          size_t output_size = 0;
          if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &output_size) !=
                  JXL_DEC_SUCCESS ||
              output_size == 0) {
            return std::unexpected{"计算 JXL RGBA 输出 buffer 大小失败。"};
          }
          const auto stride = jxl_detail::checked_rgba_stride(
              static_cast<std::size_t>(info.xsize), "JXL decoder",
              output_bit_depth > 8 ? 2 : 1);
          if (!stride) {
            return std::unexpected{stride.error()};
          }
          const auto byte_count = jxl_detail::checked_image_bytes(
              *stride, static_cast<std::size_t>(info.ysize), "JXL decoder");
          if (!byte_count) {
            return std::unexpected{byte_count.error()};
          }
          if (output_size != *byte_count) {
            return std::unexpected{"JXL RGBA 输出 buffer 大小无效。"};
          }
          auto resized = decoder_common::resize_buffer(rgba, output_size, "JXL decoder");
          if (!resized) {
            return std::unexpected{resized.error()};
          }
          if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, rgba.data(),
                                          rgba.size()) != JXL_DEC_SUCCESS) {
            return std::unexpected{"设置 JXL RGBA 输出 buffer 失败。"};
          }
        }
        if (status == JXL_DEC_COLOR_ENCODING) {
          JxlColorEncoding profile{};
          if (JxlDecoderGetColorAsEncodedProfile(decoder.get(),
                                                  JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                                  &profile) == JXL_DEC_SUCCESS) {
            encoded_color_profile = profile;
          }
          if (copy_metadata_payloads) {
            auto icc = jxl_detail::copy_embedded_icc_profile(decoder.get(), info);
            if (!icc) {
              return std::unexpected{icc.error()};
            }
            icc_profile = std::move(*icc);
          }
        }
        if (copy_metadata_payloads && status == JXL_DEC_BOX) {
          if (auto finished = finish_current_box(); !finished) {
            return std::unexpected{finished.error()};
          }
          JxlBoxType box_type{};
          if (JxlDecoderGetBoxType(decoder.get(), box_type,
                                   decompress_metadata_boxes ? JXL_TRUE : JXL_FALSE) !=
              JXL_DEC_SUCCESS) {
            return std::unexpected{"读取 JXL metadata box 类型失败。"};
          }
          const bool standard_metadata_box = std::ranges::equal(box_type, jxl_detail::exif_box_type) ||
                                             std::ranges::equal(box_type, jxl_detail::xmp_box_type);
          if (standard_metadata_box) {
            std::uint64_t box_size = 0;
            std::size_t buffer_size = 16 * 1024;
            if (JxlDecoderGetBoxSizeContents(decoder.get(), &box_size) == JXL_DEC_SUCCESS) {
              if (box_size > encoding_defaults::codec_metadata_max_bytes) {
                return std::unexpected{"JXL metadata box 超过 64 MiB 上限。"};
              }
              if (box_size == 0) {
                continue;
              }
              buffer_size = static_cast<std::size_t>(box_size);
            }
            if (auto begun = jxl_detail::begin_standard_metadata_box(metadata_budget, buffer_size); !begun) {
              return std::unexpected{begun.error()};
            }
            auto box = decoder_common::make_byte_buffer(buffer_size, "JXL metadata box");
            if (!box) {
              return std::unexpected{box.error()};
            }
            current_box_type[0] = box_type[0];
            current_box_type[1] = box_type[1];
            current_box_type[2] = box_type[2];
            current_box_type[3] = box_type[3];
            current_box = std::move(*box);
            current_box_active = true;
            if (JxlDecoderSetBoxBuffer(decoder.get(),
                                       reinterpret_cast<std::uint8_t*>(current_box.data()),
                                       current_box.size()) != JXL_DEC_SUCCESS) {
              return std::unexpected{"设置 JXL metadata box 输出 buffer 失败。"};
            }
          }
        }
        if (copy_metadata_payloads && status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
          if (!current_box_active) {
            return std::unexpected{"JXL metadata box 输出状态无效。"};
          }
          const auto remaining = JxlDecoderReleaseBoxBuffer(decoder.get());
          if (remaining > current_box.size()) {
            return std::unexpected{"JXL metadata box 输出尺寸无效。"};
          }
          const auto written = current_box.size() - remaining;
          if (written >= encoding_defaults::codec_metadata_max_bytes) {
            return std::unexpected{"JXL metadata box 超过 64 MiB 上限。"};
          }
          const auto grow_by = std::max<std::size_t>(16 * 1024, written);
          const auto new_size = std::min(
              encoding_defaults::codec_metadata_max_bytes, written + grow_by);
          if (new_size <= written) {
            return std::unexpected{"JXL metadata box 超过 64 MiB 上限。"};
          }
          if (new_size <= current_box.size()) {
            return std::unexpected{"JXL metadata box 输出尺寸无效。"};
          }
          const auto reserved_bytes = current_box.size();
          auto resized = decoder_common::resize_buffer(current_box, new_size, "JXL metadata box");
          if (!resized) {
            return std::unexpected{resized.error()};
          }
          if (auto reserved = jxl_detail::reserve_standard_metadata_box_bytes(
                  metadata_budget, current_box.size() - reserved_bytes);
              !reserved) {
            return std::unexpected{reserved.error()};
          }
          if (JxlDecoderSetBoxBuffer(
                  decoder.get(),
                  reinterpret_cast<std::uint8_t*>(current_box.data() + written),
                  current_box.size() - written) != JXL_DEC_SUCCESS) {
            return std::unexpected{"设置 JXL metadata box 输出 buffer 失败。"};
          }
        }
        if (copy_metadata_payloads && status == JXL_DEC_BOX_COMPLETE) {
          if (auto finished = finish_current_box(); !finished) {
            return std::unexpected{finished.error()};
          }
        }
        if (status == JXL_DEC_FULL_IMAGE) {
          if (rgba.empty()) {
            return std::unexpected{"JXL 解码未产生 RGBA 输出。"};
          }
          full_image_received = true;
          if (info.have_animation) {
            return finish_image();
          }
        }
        if (status == JXL_DEC_SUCCESS) {
          return finish_image();
        }
      }
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JXL 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JXL 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"JXL 解码文件系统访问失败。"};
    }
  }
  int decode_threads_{1};
};

class JXLImageEncoder final : public ImageEncoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libjxl"; }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::jxl,
                             .features = CodecFeature::lossless |
                                         CodecFeature::alpha |
                                         CodecFeature::thread_control |
                                         CodecFeature::visual_quality_search,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 0,
                             .max_speed = 10,
                             .bit_depths = {8, 16}};
  }

  std::expected<NativeEncodeResult, std::string> encode_jpeg_bitstream(
      std::span<const std::byte> jpeg_bytes,
      const NativeEncodeSettings& settings,
      std::stop_token stop_token = {}) const {
    try {
      if (jpeg_bytes.empty()) {
        return std::unexpected{"JPEG 原始码流为空。"};
      }
      if (settings.strip_metadata) {
        return std::unexpected{"JPEG 原始码流级无损转封装需要保留 JPEG reconstruction metadata，不能与 --strip 同时使用。"};
      }

      jxl_detail::EncoderPtr encoder{JxlEncoderCreate(nullptr)};
      if (!encoder) {
        return std::unexpected{"创建 JXL encoder 失败。"};
      }

      const int encoder_threads = jxl_detail::codec_thread_count(
          settings.resources.encoder_threads_per_file);
      auto runner = jxl_detail::create_runner(encoder_threads);
      if (!runner) {
        return std::unexpected{runner.error()};
      }
      if (auto attached = jxl_detail::attach_encoder_runner(encoder.get(), runner->get());
          !attached) {
        return std::unexpected{attached.error()};
      }

      if (JxlEncoderUseContainer(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS) {
        return std::unexpected{"启用 JXL container 失败。"};
      }
      if (JxlEncoderStoreJPEGMetadata(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS) {
        return std::unexpected{"启用 JXL JPEG reconstruction metadata 失败。"};
      }

      auto* frame_settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
      if (frame_settings == nullptr) {
        return std::unexpected{"创建 JXL frame settings 失败。"};
      }

      const auto speed_mapping = map_jxl_speed_to_effort(settings.speed);
      if (JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_EFFORT,
                                           speed_mapping.codec_value) != JXL_ENC_SUCCESS) {
        return std::unexpected{"设置 JXL effort 失败。"};
      }

      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      const auto* input = reinterpret_cast<const std::uint8_t*>(jpeg_bytes.data());
      if (JxlEncoderAddJPEGFrame(frame_settings, input, jpeg_bytes.size()) != JXL_ENC_SUCCESS) {
        return std::unexpected{"添加 JXL JPEG frame 失败。"};
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      JxlEncoderCloseInput(encoder.get());

      auto output = jxl_detail::collect_encoder_output(
          encoder.get(), jpeg_bytes.size(), stop_token);
      if (!output) {
        return std::unexpected{output.error()};
      }

      auto diagnostics = diagnostics_from_settings(settings);
      diagnostics.decoder_id = "jpeg-bitstream";
      diagnostics.encoder_id = "libjxl";
      diagnostics.integration_mode = "jxl-jpeg-bitstream-transcode";
      diagnostics.speed_mapping = speed_mapping;
      diagnostics.encoder_threads = encoder_threads;
      diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;
      return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(*output),
                                                        .codec_name = "libjxl"},
                                .diagnostics = std::move(diagnostics),
                                .final_quality = 100,
                                .lossless = true,
                                .search_attempt_count = 1};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JXL JPEG 转封装内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JXL JPEG 转封装数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"JXL JPEG 转封装文件系统访问失败。"};
    }
  }

  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings,
      std::stop_token stop_token = {}) const override {
    try {
      if (image.geometry || image.pixel_format != PixelFormat::rgba || image.sample_representation == SampleRepresentation::ieee_half_float ||
          settings.tone_map_to_sdr_pixels) {
        auto converted = convert_image_pixels(image, PixelConversionRequest{.format = PixelFormat::rgba,
            .bit_depth = image.bit_depth > 8 ? 16 : 8, .preserve_alpha = settings.applied_alpha == "kept",
            .tone_map_to_sdr = settings.tone_map_to_sdr_pixels}, stop_token);
        if (!converted) return std::unexpected{converted.error()};
        auto normalized = settings; normalized.tone_map_to_sdr_pixels = false;
        return encode(*converted, normalized, stop_token);
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      auto plane = jxl_detail::rgba_plane(image);
      if (!plane) {
        return std::unexpected{plane.error()};
      }
      if (image.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
          image.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::unexpected{"JXL encoder 输入尺寸超过 libjxl API 限制。"};
      }

      jxl_detail::EncoderPtr encoder{JxlEncoderCreate(nullptr)};
      if (!encoder) {
        return std::unexpected{"创建 JXL encoder 失败。"};
      }

      const int encoder_threads = jxl_detail::codec_thread_count(
          settings.resources.encoder_threads_per_file);
      auto runner = jxl_detail::create_runner(encoder_threads);
      if (!runner) {
        return std::unexpected{runner.error()};
      }
      if (auto attached = jxl_detail::attach_encoder_runner(encoder.get(), runner->get());
          !attached) {
        return std::unexpected{attached.error()};
      }

      const bool encode_metadata_boxes = jxl_detail::has_standard_metadata_boxes(image, settings);
      if (encode_metadata_boxes) {
        if (JxlEncoderUseContainer(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS) {
          return std::unexpected{"启用 JXL container 失败。"};
        }
        if (JxlEncoderUseBoxes(encoder.get()) != JXL_ENC_SUCCESS) {
          return std::unexpected{"启用 JXL metadata boxes 失败。"};
        }
      }

      const bool keep_alpha = settings.applied_alpha == "kept";
      std::size_t encoder_input_size_hint = 0;
      JxlBasicInfo info{};
      JxlEncoderInitBasicInfo(&info);
      info.xsize = static_cast<std::uint32_t>(image.width);
      info.ysize = static_cast<std::uint32_t>(image.height);
      info.bits_per_sample = static_cast<std::uint32_t>(image.bit_depth);
      info.num_color_channels = 3;
      if (keep_alpha) {
        info.num_extra_channels = 1;
        info.alpha_bits = static_cast<std::uint32_t>(image.bit_depth);
        info.alpha_premultiplied =
            image.alpha_mode == AlphaMode::premultiplied ? JXL_TRUE : JXL_FALSE;
      }
      info.uses_original_profile = JXL_TRUE;

      if (JxlEncoderSetBasicInfo(encoder.get(), &info) != JXL_ENC_SUCCESS) {
        return std::unexpected{"设置 JXL 基础信息失败。"};
      }
      const int required_codestream_level = JxlEncoderGetRequiredCodestreamLevel(encoder.get());
      if (required_codestream_level < 0) {
        return std::unexpected{std::format(
            "JXL encoder 输入尺寸 {}x{} 超过 JPEG XL codestream level 支持范围。",
            image.width, image.height)};
      }
      if (required_codestream_level == 10 &&
          JxlEncoderSetCodestreamLevel(encoder.get(), 10) != JXL_ENC_SUCCESS) {
        return std::unexpected{"设置 JXL codestream level 10 失败。"};
      }
      if (required_codestream_level != 5 && required_codestream_level != 10) {
        return std::unexpected{std::format("JXL encoder codestream level 需求无效: {}。",
                                           required_codestream_level)};
      }

      if (auto color_profile = jxl_detail::apply_color_profile(encoder.get(), image, settings);
          !color_profile) {
        return std::unexpected{color_profile.error()};
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }

      auto* frame_settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
      if (frame_settings == nullptr) {
        return std::unexpected{"创建 JXL frame settings 失败。"};
      }

      const auto speed_mapping = map_jxl_speed_to_effort(settings.speed);
      if (JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_EFFORT,
                                           speed_mapping.codec_value) != JXL_ENC_SUCCESS) {
        return std::unexpected{"设置 JXL effort 失败。"};
      }

      const bool lossless = settings.visual_quality ? *settings.visual_quality >= 100 : settings.quality >= 100;
      const int final_quality = lossless ? 100 : std::clamp(settings.quality, 1, 99);
      if (lossless) {
        if (JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE) != JXL_ENC_SUCCESS) {
          return std::unexpected{"设置 JXL lossless 失败。"};
        }
      } else {
        const auto distance = JxlEncoderDistanceFromQuality(static_cast<float>(final_quality));
        if (JxlEncoderSetFrameDistance(frame_settings, distance) != JXL_ENC_SUCCESS) {
          return std::unexpected{"设置 JXL distance 失败。"};
        }
      }

      if (keep_alpha) {
        JxlPixelFormat format{.num_channels = 4,
                              .data_type = image.bit_depth > 8 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8,
                              .endianness = JXL_NATIVE_ENDIAN,
                              .align = 0};
        const auto& rgba = (*plane)->bytes;
        const auto size = jxl_detail::checked_image_bytes((*plane)->stride, image.height, "JXL encoder");
        if (!size) {
          return std::unexpected{size.error()};
        }
        if (stop_token.stop_requested()) {
          return std::unexpected{"任务已取消。"};
        }
        encoder_input_size_hint = *size;
        if (JxlEncoderAddImageFrame(frame_settings, &format, rgba.data(), *size) !=
            JXL_ENC_SUCCESS) {
          return std::unexpected{"添加 JXL RGBA frame 失败。"};
        }
        if (stop_token.stop_requested()) {
          return std::unexpected{"任务已取消。"};
        }
      } else {
        std::vector<std::byte> rgb;
        auto rgb_input = settings.jxl_rgb8_input;
        if (rgb_input.empty()) {
          auto prepared_rgb = jxl_detail::make_rgb_buffer(**plane, image.width, image.height,
                                                          image.bit_depth);
          if (!prepared_rgb) {
            return std::unexpected{prepared_rgb.error()};
          }
          rgb = std::move(*prepared_rgb);
          rgb_input = std::span<const std::byte>{rgb};
        } else {
          const auto bytes_per_sample = image.bit_depth > 8 ? std::size_t{2} : std::size_t{1};
          if (image.width == 0 ||
              image.width > std::numeric_limits<std::size_t>::max() / 3 / bytes_per_sample) {
            return std::unexpected{"JXL encoder RGB 输入宽度无效。"};
          }
          const auto expected_rgb_size = jxl_detail::checked_image_bytes(
                                                                         image.width * 3 * bytes_per_sample,
                                                                         image.height,
                                                                         "JXL encoder");
          if (!expected_rgb_size) {
            return std::unexpected{expected_rgb_size.error()};
          }
          if (rgb_input.size() != *expected_rgb_size) {
            return std::unexpected{"JXL encoder RGB 输入缓存尺寸无效。"};
          }
        }
        JxlPixelFormat format{.num_channels = 3,
                              .data_type = image.bit_depth > 8 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8,
                              .endianness = JXL_NATIVE_ENDIAN,
                              .align = 0};
        if (stop_token.stop_requested()) {
          return std::unexpected{"任务已取消。"};
        }
        encoder_input_size_hint = rgb_input.size();
        if (JxlEncoderAddImageFrame(frame_settings, &format, rgb_input.data(), rgb_input.size()) !=
            JXL_ENC_SUCCESS) {
          return std::unexpected{"添加 JXL RGB frame 失败。"};
        }
        if (stop_token.stop_requested()) {
          return std::unexpected{"任务已取消。"};
        }
      }

      if (encode_metadata_boxes) {
        if (stop_token.stop_requested()) {
          return std::unexpected{"任务已取消。"};
        }
        if (auto metadata_boxes = jxl_detail::add_standard_metadata_boxes(encoder.get(), image, settings);
            !metadata_boxes) {
          return std::unexpected{metadata_boxes.error()};
        }
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      JxlEncoderCloseInput(encoder.get());

      auto output = jxl_detail::collect_encoder_output(
          encoder.get(), encoder_input_size_hint, stop_token);
      if (!output) {
        return std::unexpected{output.error()};
      }

      auto diagnostics = diagnostics_from_settings(settings);
      diagnostics.encoder_id = "libjxl";
      diagnostics.speed_mapping = speed_mapping;
      diagnostics.encoder_threads = encoder_threads;
      diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;
      return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(*output),
                                                        .codec_name = "libjxl"},
                                .diagnostics = std::move(diagnostics),
                                .final_quality = final_quality,
                                .lossless = lossless,
                                .search_attempt_count = 1};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JXL 编码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JXL 编码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"JXL 编码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
