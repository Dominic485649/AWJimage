module;

#include <algorithm>
#include <array>
#include <cmath>
#include <lcms2.h>
#include <avif/internal.h>
#include <libplacebo/colorspace.h>
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
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

export module awj.webp_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.gain_map;
import awj.image;
import awj.hdr_tonemap;
import awj.pixel_conversion;
import awj.large_image_plan;

export namespace awj {

namespace webp_detail {

std::size_t initial_encoder_output_capacity(std::size_t input_bytes) noexcept {
  if (input_bytes == 0) {
    return encoding_defaults::webp_min_encoder_output_capacity;
  }
  const auto scaled_hint =
      std::max(encoding_defaults::webp_min_encoder_output_capacity,
               input_bytes / 8);
  return std::min(
      scaled_hint, encoding_defaults::webp_max_initial_encoder_output_capacity);
}

struct WebPFreeDeleter {
  void operator()(void* value) const noexcept {
    if (value != nullptr) {
      WebPFree(value);
    }
  }
};

struct WebPAnimDecoderDeleter {
  void operator()(WebPAnimDecoder* value) const noexcept {
    if (value != nullptr) {
      WebPAnimDecoderDelete(value);
    }
  }
};

struct WebPVectorWriter {
  std::vector<std::byte> bytes;
  const char* error{};
  std::stop_token* stop_token{};
};

int write_webp_bytes(const std::uint8_t* data,
                     std::size_t data_size,
                     const WebPPicture* picture) noexcept {
  if (picture == nullptr || picture->custom_ptr == nullptr) {
    return 0;
  }
  auto* writer = static_cast<WebPVectorWriter*>(picture->custom_ptr);
  if (writer->stop_token != nullptr && writer->stop_token->stop_requested()) {
    writer->error = "任务已取消。";
    return 0;
  }
  if (data_size == 0) {
    return 1;
  }
  if (data == nullptr) {
    writer->error = "WebP encoder 输出数据无效。";
    return 0;
  }
  const auto old_size = writer->bytes.size();
  if (data_size > std::numeric_limits<std::size_t>::max() - old_size) {
    writer->error = "WebP encoder 输出过大。";
    return 0;
  }
  if (old_size + data_size > encoding_defaults::effective_max_input_file_bytes()) {
    writer->error = "WebP encoder 输出超过当前运行时上限。";
    return 0;
  }
  try {
    writer->bytes.resize(old_size + data_size);
  } catch (const std::bad_alloc&) {
    writer->error = "WebP encoder 输出缓冲区内存不足。";
    return 0;
  } catch (const std::length_error&) {
    writer->error = "WebP encoder 输出缓冲区尺寸超过运行时限制。";
    return 0;
  }
  if (writer->stop_token != nullptr && writer->stop_token->stop_requested()) {
    writer->error = "任务已取消。";
    return 0;
  }
  std::ranges::copy_n(reinterpret_cast<const std::byte*>(data), data_size,
                      writer->bytes.data() + old_size);
  return 1;
}

struct WebPPictureGuard {
  WebPPicture picture{};
  bool initialized{WebPPictureInit(&picture) != 0};
  WebPPictureGuard() = default;
  ~WebPPictureGuard() {
    if (initialized) {
      WebPPictureFree(&picture);
    }
  }
  WebPPictureGuard(const WebPPictureGuard&) = delete;
  WebPPictureGuard& operator=(const WebPPictureGuard&) = delete;
};

struct WebPDataGuard {
  WebPDataGuard() = default;
  WebPData data{};
  ~WebPDataGuard() { WebPDataClear(&data); }
  WebPDataGuard(const WebPDataGuard&) = delete;
  WebPDataGuard& operator=(const WebPDataGuard&) = delete;
};

using WebPBytes = std::unique_ptr<std::uint8_t, WebPFreeDeleter>;

std::expected<std::vector<std::byte>, std::string> read_file_bytes(
    const fs::path& path) {
  return decoder_common::read_file_bytes(path, "WebP");
}

std::expected<std::size_t, std::string> checked_rgba_stride(std::size_t width,
                                                            std::string_view context) {
  if (width == 0) {
    return std::unexpected{std::format("{} 输入宽度无效。", context)};
  }
  if (width > std::numeric_limits<std::size_t>::max() / 4) {
    return std::unexpected{std::format("{} 输入宽度过大。", context)};
  }
  return width * 4;
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
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
      image.planes.empty()) {
    return std::unexpected{"WebP encoder 当前需要 8-bit RGBA ImageBuffer。"};
  }
  const auto& plane = image.planes.front();
  const auto expected_stride = checked_rgba_stride(image.width, "WebP encoder");
  if (!expected_stride) {
    return std::unexpected{expected_stride.error()};
  }
  const auto expected_bytes = checked_image_bytes(plane.stride, image.height, "WebP encoder");
  if (!expected_bytes) {
    return std::unexpected{expected_bytes.error()};
  }
  if (plane.stride < *expected_stride || plane.bytes.size() < *expected_bytes) {
    return std::unexpected{"WebP encoder 输入 RGBA buffer 尺寸无效。"};
  }
  return &plane;
}

struct WebPMetadataChunk {
  MetadataKind kind{};
  const char* fourcc{};
  std::string_view name{};
};

constexpr WebPMetadataChunk webp_metadata_chunks[] = {
    {.kind = MetadataKind::icc, .fourcc = "ICCP", .name = "ICC"},
    {.kind = MetadataKind::exif, .fourcc = "EXIF", .name = "Exif"},
    {.kind = MetadataKind::xmp, .fourcc = "XMP ", .name = "XMP"},
};

std::uint32_t read_le_u32(std::span<const std::byte> bytes,
                          std::size_t offset) noexcept {
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool fourcc_is(std::span<const std::byte> bytes,
               std::size_t offset,
               std::string_view fourcc) noexcept {
  return fourcc.size() == 4 && offset <= bytes.size() && bytes.size() - offset >= 4 &&
         bytes[offset] == std::byte{static_cast<unsigned char>(fourcc[0])} &&
         bytes[offset + 1] == std::byte{static_cast<unsigned char>(fourcc[1])} &&
         bytes[offset + 2] == std::byte{static_cast<unsigned char>(fourcc[2])} &&
         bytes[offset + 3] == std::byte{static_cast<unsigned char>(fourcc[3])};
}

std::size_t metadata_chunk_index(std::span<const std::byte> header) noexcept {
  for (std::size_t index = 0; index < std::size(webp_metadata_chunks); ++index) {
    if (fourcc_is(header, 0, webp_metadata_chunks[index].fourcc)) {
      return index;
    }
  }
  return std::size(webp_metadata_chunks);
}

const MetadataBlock* first_metadata(const ImageBuffer& image, MetadataKind kind) noexcept {
  for (const auto& block : image.metadata) {
    if (block.kind == kind && !block.bytes.empty()) {
      return &block;
    }
  }
  return nullptr;
}

PixelFormat source_pixel_format_for_webp(const WebPBitstreamFeatures& features) noexcept {
  if (features.format == 1) {
    return PixelFormat::yuv420;
  }
  if (features.format == 2) {
    return features.has_alpha ? PixelFormat::rgba : PixelFormat::rgb;
  }
  return PixelFormat::unknown;
}

ImageSourceInfo source_info_from_features(const WebPBitstreamFeatures& features) noexcept {
  return ImageSourceInfo{.pixel_format = source_pixel_format_for_webp(features),
                         .bit_depth = 8};
}

bool has_metadata_to_mux(const ImageBuffer& image,
                         const NativeEncodeSettings& settings) noexcept {
  if (settings.applied_icc == "kept" && first_metadata(image, MetadataKind::icc) != nullptr) {
    return true;
  }
  return first_metadata(image, MetadataKind::exif) != nullptr ||
         first_metadata(image, MetadataKind::xmp) != nullptr;
}

std::expected<std::vector<std::byte>, std::string> copy_metadata_payload(
    std::span<const std::byte> payload,
    std::string_view context) {
  if (payload.empty()) {
    return std::vector<std::byte>{};
  }
  if (payload.size() > encoding_defaults::codec_metadata_max_bytes) {
    return std::unexpected{std::format("{} 超过 64 MiB 上限。", context)};
  }
  auto bytes = decoder_common::make_byte_buffer(payload.size(), context);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  std::ranges::copy_n(payload.data(), payload.size(), bytes->begin());
  return std::move(*bytes);
}

std::expected<void, std::string> copy_metadata(ImageBuffer& image,
                                               std::span<const std::byte> encoded) {
  if (encoded.size() < 12 || !fourcc_is(encoded, 0, "RIFF") ||
      !fourcc_is(encoded, 8, "WEBP")) {
    return {};
  }

  const auto riff_payload_size = static_cast<std::size_t>(read_le_u32(encoded, 4));
  if (riff_payload_size < 4 || riff_payload_size > encoded.size() - 8) {
    return {};
  }
  const auto riff_end = 8 + riff_payload_size;
  std::array<std::vector<std::byte>, std::size(webp_metadata_chunks)> metadata_payloads{};
  std::array<bool, std::size(webp_metadata_chunks)> seen_metadata{};

  std::size_t offset = 12;
  while (offset + 8 <= riff_end) {
    const auto payload_size = static_cast<std::size_t>(read_le_u32(encoded, offset + 4));
    const auto payload_offset = offset + 8;
    if (payload_size > riff_end - payload_offset) {
      return {};
    }

    const auto chunk_index = metadata_chunk_index(encoded.subspan(offset, 4));
    if (chunk_index < std::size(webp_metadata_chunks)) {
      const auto& chunk = webp_metadata_chunks[chunk_index];
      if (payload_size > encoding_defaults::codec_metadata_max_bytes) {
        return std::unexpected{std::format("WebP {} metadata 超过 64 MiB 上限。", chunk.name)};
      }
      if (!seen_metadata[chunk_index]) {
        seen_metadata[chunk_index] = true;
        auto payload = copy_metadata_payload(encoded.subspan(payload_offset, payload_size),
                                             std::format("WebP {} metadata", chunk.name));
        if (!payload) {
          return std::unexpected{payload.error()};
        }
        metadata_payloads[chunk_index] = std::move(*payload);
      }
    }

    const auto padded_payload_size = payload_size + (payload_size & 1U);
    if (padded_payload_size > riff_end - payload_offset) {
      return {};
    }
    offset = payload_offset + padded_payload_size;
  }

  for (std::size_t index = 0; index < std::size(webp_metadata_chunks); ++index) {
    if (metadata_payloads[index].empty()) {
      continue;
    }
    MetadataBlock block{.kind = webp_metadata_chunks[index].kind,
                        .bytes = std::move(metadata_payloads[index])};
    try {
      image.metadata.push_back(std::move(block));
    } catch (const std::bad_alloc&) {
      return std::unexpected{std::format("WebP {} metadata 内存不足。",
                                         webp_metadata_chunks[index].name)};
    } catch (const std::length_error&) {
      return std::unexpected{std::format("WebP {} metadata 尺寸超过运行时限制。",
                                         webp_metadata_chunks[index].name)};
    }
  }
  return {};
}

std::expected<void, std::string> set_metadata_chunk(WebPMux* mux,
                                                    const ImageBuffer& image,
                                                    const WebPMetadataChunk& chunk) {
  const auto* metadata = first_metadata(image, chunk.kind);
  if (metadata == nullptr) {
    return {};
  }
  if (metadata->bytes.size() > encoding_defaults::codec_metadata_max_bytes) {
    return std::unexpected{std::format("WebP {} metadata 超过 64 MiB 上限。", chunk.name)};
  }
  WebPData chunk_data{.bytes = reinterpret_cast<const std::uint8_t*>(metadata->bytes.data()),
                      .size = metadata->bytes.size()};
  if (WebPMuxSetChunk(mux, chunk.fourcc, &chunk_data, 1) != WEBP_MUX_OK) {
    return std::unexpected{std::format("WebP mux 设置 {} 失败。", chunk.name)};
  }
  return {};
}

std::expected<std::vector<std::byte>, std::string> sdr_cicp_icc(
    const ImageSourceInfo& info, std::stop_token stop = {}) {
  return gain_map_detail::sdr_presentation_icc(info, stop);
}

std::expected<std::vector<std::byte>, std::string> mux_metadata(
    std::span<const std::byte> encoded,
    const ImageBuffer& image,
    const NativeEncodeSettings& settings,
    std::stop_token stop_token = {}, const MetadataBlock* essential_icc = nullptr) {
  if (stop_token.stop_requested()) {
    return std::unexpected{"任务已取消。"};
  }
  WebPData image_data{.bytes = reinterpret_cast<const std::uint8_t*>(encoded.data()),
                      .size = encoded.size()};
  WebPDataGuard assembled{};
  WebPMux* mux = WebPMuxNew();
  if (mux == nullptr) {
    return std::unexpected{"无法创建 WebP mux。"};
  }
  const auto mux_guard = std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)>{mux, &WebPMuxDelete};
  if (WebPMuxSetImage(mux, &image_data, 1) != WEBP_MUX_OK) {
    return std::unexpected{"WebP mux 设置图像失败。"};
  }
  if (essential_icc) {
    if (essential_icc->bytes.size() > encoding_defaults::codec_metadata_max_bytes)
      return std::unexpected{"WebP essential ICC 超过元数据上限。"};
    const WebPData icc{reinterpret_cast<const std::uint8_t*>(essential_icc->bytes.data()), essential_icc->bytes.size()};
    if (WebPMuxSetChunk(mux, "ICCP", &icc, 1) != WEBP_MUX_OK)
      return std::unexpected{"WebP essential ICC 封装失败。"};
  } else if (settings.applied_icc == "kept") {
    if (auto set = set_metadata_chunk(mux, image, webp_metadata_chunks[0]); !set) {
      return std::unexpected{set.error()};
    }
  }
  for (std::size_t index = 1; !settings.strip_metadata && index < std::size(webp_metadata_chunks); ++index) {
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    if (auto set = set_metadata_chunk(mux, image, webp_metadata_chunks[index]); !set) {
      return std::unexpected{set.error()};
    }
  }
  if (stop_token.stop_requested()) {
    return std::unexpected{"任务已取消。"};
  }
  if (WebPMuxAssemble(mux, &assembled.data) != WEBP_MUX_OK ||
      assembled.data.bytes == nullptr || assembled.data.size == 0) {
    return std::unexpected{"WebP mux 输出失败。"};
  }
  if (assembled.data.size > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{"WebP mux 输出超过当前运行时上限。"};
  }
  if (stop_token.stop_requested()) {
    return std::unexpected{"任务已取消。"};
  }
  auto bytes = decoder_common::make_byte_buffer(assembled.data.size, "WebP mux");
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  if (stop_token.stop_requested()) {
    return std::unexpected{"任务已取消。"};
  }
  std::ranges::copy_n(reinterpret_cast<const std::byte*>(assembled.data.bytes),
                      assembled.data.size, bytes->begin());
  return std::move(*bytes);
}

}  // namespace webp_detail

class WebPImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libwebp"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    auto ext = path.extension().wstring();
    std::ranges::transform(ext, ext.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    return ext == L".webp";
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_prefix(path, 30, "WebP");
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      WebPBitstreamFeatures features{};
      const auto* data = reinterpret_cast<const std::uint8_t*>(bytes->data());
      if (WebPGetFeatures(data, bytes->size(), &features) != VP8_STATUS_OK ||
          features.width <= 0 || features.height <= 0) {
        return std::unexpected{std::format("WebP 文件信息无效: {}", display_path_for_user(path))};
      }
      auto dimensions = decoder_common::make_image_dimensions_checked(static_cast<std::uint32_t>(features.width),
          static_cast<std::uint32_t>(features.height), "WebP");
      if (!dimensions) return dimensions;
      // VP8X 的 ICC 标志位可在固定头部中读取，不扫描或解压整幅图像。
      const bool extended = bytes->size() >= 21 && webp_detail::fourcc_is(*bytes, 12, "VP8X");
      const bool has_icc = extended && (std::to_integer<unsigned>((*bytes)[20]) & 0x20u) != 0;
      dimensions->source_bit_depth = 8;
      dimensions->source_has_alpha = features.has_alpha != 0;
      dimensions->source_dynamic_range = has_icc ? ImageDynamicRangeHint::unknown : ImageDynamicRangeHint::sdr;
      dimensions->opaque_sdr8_yuv420 = features.format == 1 && !dimensions->source_has_alpha && !has_icc;
      const auto pixels = static_cast<std::uint64_t>(features.width) * features.height;
      dimensions->decoded_storage_bytes = pixels * 4;
      return dimensions;
    } catch (const std::bad_alloc&) {
      return std::unexpected{"WebP 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"WebP 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"WebP 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode_memory(
      std::span<const std::byte> bytes,
      std::string_view source_name,
      DecodeOptions options = {}) const override {
    return decode_bytes(bytes, source_name,
                        options.copy_metadata_payloads.value_or(false), options.native_pixels);
  }

  std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const override {
    return decode(path, {});
  }

  std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path, DecodeOptions options) const override {
    try {
      auto bytes = webp_detail::read_file_bytes(path);
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      return decode_bytes(*bytes, display_path_for_user(path), true, options.native_pixels);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"WebP 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"WebP 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"WebP 解码文件系统访问失败。"};
    }
  }

 private:
  static std::expected<ImageDecodeResult, std::string> decode_bytes(
      std::span<const std::byte> bytes,
      std::string_view source_name,
      bool copy_metadata_payloads, bool native_pixels = false) {
    try {
      if (bytes.empty()) {
        return std::unexpected{std::format("WebP 输入为空: {}", source_name)};
      }
      WebPBitstreamFeatures features{};
      const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
      if (WebPGetFeatures(data, bytes.size(), &features) != VP8_STATUS_OK ||
          features.width <= 0 || features.height <= 0) {
        return std::unexpected{std::format("WebP 文件信息无效: {}", source_name)};
      }
      const int width = features.width;
      const int height = features.height;
      if (native_pixels && features.format == 1 && !features.has_animation) {
        ImageBuffer image{.width = static_cast<std::size_t>(width), .height = static_cast<std::size_t>(height),
            .pixel_format = PixelFormat::yuv420,
            .alpha_mode = features.has_alpha ? AlphaMode::straight : AlphaMode::none, .bit_depth = 8,
            .source_info = webp_detail::source_info_from_features(features)};
        image.source_info->matrix_coefficients = 6; image.source_info->color_range = 0;
        image.planes.resize(features.has_alpha ? 4 : 3);
        for (std::size_t c = 0; c < image.planes.size(); ++c) {
          const auto w = c == 1 || c == 2 ? (image.width + 1) / 2 : image.width;
          const auto h = c == 1 || c == 2 ? (image.height + 1) / 2 : image.height;
          auto count = decoder_common::checked_image_bytes(w, h, "WebP native YUV");
          if (!count) return std::unexpected{count.error()};
          auto storage = decoder_common::make_byte_buffer(*count, "WebP native YUV");
          if (!storage) return std::unexpected{storage.error()};
          image.planes[c] = ImagePlane{.bytes = std::move(*storage), .stride = w};
        }
        WebPDecoderConfig config{};
        if (!WebPInitDecoderConfig(&config)) return std::unexpected{"WebP decoder 配置失败。"};
        config.output.colorspace = features.has_alpha ? MODE_YUVA : MODE_YUV;
        config.output.is_external_memory = 1;
        auto& yuv = config.output.u.YUVA;
        yuv.y = reinterpret_cast<std::uint8_t*>(image.planes[0].bytes.data());
        yuv.u = reinterpret_cast<std::uint8_t*>(image.planes[1].bytes.data());
        yuv.v = reinterpret_cast<std::uint8_t*>(image.planes[2].bytes.data());
        yuv.y_stride = width; yuv.u_stride = yuv.v_stride = (width + 1) / 2;
        yuv.y_size = image.planes[0].bytes.size(); yuv.u_size = image.planes[1].bytes.size(); yuv.v_size = image.planes[2].bytes.size();
        if (features.has_alpha) {
          yuv.a = reinterpret_cast<std::uint8_t*>(image.planes[3].bytes.data());
          yuv.a_stride = width; yuv.a_size = image.planes[3].bytes.size();
        }
        const auto result = WebPDecode(data, bytes.size(), &config);
        WebPFreeDecBuffer(&config.output);
        if (result != VP8_STATUS_OK) return std::unexpected{"WebP 原生 YUV 解码失败。"};
        if (copy_metadata_payloads) {
          if (auto copied = webp_detail::copy_metadata(image, bytes); !copied) return std::unexpected{copied.error()};
        }
        return ImageDecodeResult{.image = std::move(image), .decoder_id = "libwebp"};
      }

      const auto stride = webp_detail::checked_rgba_stride(static_cast<std::size_t>(width), "WebP decoder");
      if (!stride) {
        return std::unexpected{stride.error()};
      }
      const auto byte_count = webp_detail::checked_image_bytes(*stride, static_cast<std::size_t>(height), "WebP decoder");
      if (!byte_count) {
        return std::unexpected{byte_count.error()};
      }
      if (*stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected{"WebP decoder 输出步长超过 libwebp API 限制。"};
      }

      ImagePlane plane{.stride = *stride};
      auto resized = decoder_common::resize_buffer(plane.bytes, *byte_count, "WebP decoder");
      if (!resized) {
        return std::unexpected{resized.error()};
      }
      auto* output = reinterpret_cast<std::uint8_t*>(plane.bytes.data());
      if (features.has_animation) {
        WebPAnimDecoderOptions options{};
        if (!WebPAnimDecoderOptionsInit(&options)) {
          return std::unexpected{"初始化 WebP 动画首帧 decoder 失败。"};
        }
        options.color_mode = MODE_RGBA;
        options.use_threads = 1;
        const WebPData encoded{.bytes = data, .size = bytes.size()};
        std::unique_ptr<WebPAnimDecoder, webp_detail::WebPAnimDecoderDeleter>
            decoder{WebPAnimDecoderNew(&encoded, &options)};
        std::uint8_t* frame = nullptr;
        int timestamp = 0;
        if (!decoder || !WebPAnimDecoderGetNext(decoder.get(), &frame, &timestamp) ||
            frame == nullptr) {
          return std::unexpected{std::format("WebP 动画首帧解码失败: {}", source_name)};
        }
        std::ranges::copy_n(reinterpret_cast<const std::byte*>(frame),
                            plane.bytes.size(), plane.bytes.begin());
      } else if (WebPDecodeRGBAInto(data, bytes.size(), output, plane.bytes.size(),
                                    static_cast<int>(*stride)) == nullptr) {
        return std::unexpected{std::format("WebP 解码失败: {}", source_name)};
      }

      ImageBuffer image{.width = static_cast<std::size_t>(width),
                        .height = static_cast<std::size_t>(height),
                        .pixel_format = PixelFormat::rgba,
                        .alpha_mode = features.has_alpha ? AlphaMode::straight : AlphaMode::none,
                        .bit_depth = 8,
                        .source_info = webp_detail::source_info_from_features(features)};
      image.planes.push_back(std::move(plane));
      if (copy_metadata_payloads) {
        if (auto copied = webp_detail::copy_metadata(image, bytes); !copied) {
          return std::unexpected{copied.error()};
        }
      }
      return ImageDecodeResult{.image = std::move(image), .decoder_id = "libwebp"};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"WebP 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"WebP 解码数据超过运行时限制。"};
    }
  }
};

class WebPImageEncoder final : public ImageEncoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libwebp"; }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::webp,
                             .features = CodecFeature::lossless |
                                         CodecFeature::alpha |
                                         CodecFeature::visual_quality_search,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 0,
                             .max_speed = 10,
                             .bit_depths = {8}};
  }

  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings,
      std::stop_token stop_token = {}) const override {
    try {
      const bool requested_lossless = settings.visual_quality ? *settings.visual_quality >= 100 : settings.quality >= 100;
      const PixelConversionRequest request{.format = requested_lossless ? PixelFormat::rgba : PixelFormat::yuv420,
          .bit_depth = 8, .matrix_coefficients = 6, .color_range = requested_lossless ? 1 : 0,
          .preserve_alpha = settings.applied_alpha == "kept", .tone_map_to_sdr = settings.tone_map_to_sdr_pixels};
      if ((image.geometry || is_yuv_format(image.pixel_format) || image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
          image.sample_representation != SampleRepresentation::unorm || settings.tone_map_to_sdr_pixels) &&
          !pixels_match(image, request)) {
        auto converted = convert_image_pixels(image, request, stop_token, settings.resources);
        if (!converted) return std::unexpected{converted.error()};
        auto normalized = settings; normalized.tone_map_to_sdr_pixels = false;
        return encode(*converted, normalized, stop_token);
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      if (is_yuv_format(image.pixel_format)) {
        if (auto valid = validate_image_pixels(image); !valid) return std::unexpected{valid.error()};
      }
      auto plane = is_yuv_format(image.pixel_format)
          ? std::expected<const ImagePlane*, std::string>{&image.planes.front()} : webp_detail::rgba_plane(image);
      if (!plane) {
        return std::unexpected{plane.error()};
      }
      if (image.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
          image.height > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
          (*plane)->stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected{"WebP encoder 输入尺寸超过 libwebp API 限制。"};
      }
      if (image.width > static_cast<std::size_t>(WEBP_MAX_DIMENSION) ||
          image.height > static_cast<std::size_t>(WEBP_MAX_DIMENSION)) {
        return std::unexpected{std::format(
            "WebP encoder 输入尺寸 {}x{} 超过 libwebp 边长上限 {}。",
            image.width, image.height, WEBP_MAX_DIMENSION)};
      }

      const auto* rgba = reinterpret_cast<const std::uint8_t*>((*plane)->bytes.data());
      const int width = static_cast<int>(image.width);
      const int height = static_cast<int>(image.height);
      const int stride = static_cast<int>((*plane)->stride);
      const bool lossless = settings.visual_quality ? *settings.visual_quality >= 100 : settings.quality >= 100;

      WebPConfig config{};
      if (WebPConfigInit(&config) == 0) {
        return std::unexpected{"WebP config 初始化失败。"};
      }
      const auto speed_mapping = map_webp_speed_to_method(settings.speed);
      const int method = speed_mapping.codec_value;
      const int final_quality = lossless ? 100 : std::clamp(settings.quality, 1, 100);
      config.quality = static_cast<float>(final_quality);
      config.lossless = lossless ? 1 : 0;
      config.method = method;
      config.alpha_quality = 100;
      config.thread_level = settings.resources.encoder_threads_per_file > 1 ? 1 : 0;
      if (WebPValidateConfig(&config) == 0) {
        return std::unexpected{"WebP config 参数无效。"};
      }

      webp_detail::WebPPictureGuard picture{};
      if (!picture.initialized) {
        return std::unexpected{"WebP picture 初始化失败。"};
      }
      picture.picture.use_argb = is_yuv_format(image.pixel_format) ? 0 : 1;
      picture.picture.width = width;
      picture.picture.height = height;
      webp_detail::WebPVectorWriter writer{.stop_token = &stop_token};
      writer.bytes.reserve(
          webp_detail::initial_encoder_output_capacity((*plane)->bytes.size()));
      picture.picture.writer = webp_detail::write_webp_bytes;
      picture.picture.custom_ptr = &writer;
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      if (is_yuv_format(image.pixel_format)) {
        picture.picture.colorspace = image.alpha_mode == AlphaMode::none ? WEBP_YUV420 : WEBP_YUV420A;
        picture.picture.y = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(image.planes[0].bytes.data()));
        picture.picture.u = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(image.planes[1].bytes.data()));
        picture.picture.v = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(image.planes[2].bytes.data()));
        picture.picture.y_stride = static_cast<int>(image.planes[0].stride);
        picture.picture.uv_stride = static_cast<int>(image.planes[1].stride);
        if (image.alpha_mode != AlphaMode::none) {
          picture.picture.a = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(image.planes[3].bytes.data()));
          picture.picture.a_stride = static_cast<int>(image.planes[3].stride);
        }
      } else if (settings.applied_alpha == "kept") {
        if (WebPPictureImportRGBA(&picture.picture, rgba, stride) == 0) {
          return std::unexpected{"WebP picture 导入 RGBA 失败。"};
        }
      } else {
        if (WebPPictureImportRGBX(&picture.picture, rgba, stride) == 0) {
          return std::unexpected{"WebP picture 导入 RGBX 失败。"};
        }
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      if (WebPEncode(&config, &picture.picture) == 0) {
        if (writer.error != nullptr) {
          return std::unexpected{writer.error};
        }
        return std::unexpected{std::format("WebP 编码失败，错误码 {}。", static_cast<int>(picture.picture.error_code))};
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }

      if (writer.error != nullptr) {
        return std::unexpected{writer.error};
      }
      if (writer.bytes.empty()) {
        return std::unexpected{"WebP encoder 输出失败。"};
      }
      EncodedImage encoded{.codec_name = "libwebp"};
      encoded.bytes = std::move(writer.bytes);
      std::optional<MetadataBlock> generated_icc;
      const MetadataBlock* essential_icc = nullptr;
      if (settings.applied_icc == "kept" || (image.source_info && image.source_info->source_has_gain_map))
        essential_icc = webp_detail::first_metadata(image, MetadataKind::icc);
      if (!essential_icc && image.source_info) {
        const auto& source = *image.source_info;
        if (source.color_primaries && source.transfer_characteristics &&
            (source.color_primaries != 1 || source.transfer_characteristics != 13)) {
          auto profile = webp_detail::sdr_cicp_icc(source, stop_token);
          if (!profile) return std::unexpected{profile.error()};
          generated_icc = MetadataBlock{MetadataKind::icc, std::move(*profile)};
          essential_icc = &*generated_icc;
        }
      }
      if (essential_icc || (!settings.strip_metadata && webp_detail::has_metadata_to_mux(image, settings))) {
        auto muxed = webp_detail::mux_metadata(encoded.bytes, image, settings, stop_token, essential_icc);
        if (!muxed) {
          return std::unexpected{muxed.error()};
        }
        encoded.bytes = std::move(*muxed);
      }

      auto diagnostics = diagnostics_from_settings(settings);
      diagnostics.encoder_id = "libwebp";
      if (essential_icc) {
        diagnostics.applied_icc = generated_icc ? "generated-essential-sdr-cicp" : "essential-sdr-rendition";
        if (image.source_info) {
          diagnostics.applied_color_primaries = image.source_info->color_primaries;
          diagnostics.applied_transfer_characteristics = image.source_info->transfer_characteristics;
        }
        diagnostics.color_reason = "写入解释所选 SDR 呈现所需的 ICC；strip 仅移除可选元数据";
      }
      diagnostics.speed_mapping = speed_mapping;
      diagnostics.encoder_threads = settings.resources.encoder_threads_per_file;
      diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;
      return NativeEncodeResult{.encoded = std::move(encoded),
                                .diagnostics = std::move(diagnostics),
                                .final_quality = final_quality,
                                .lossless = lossless,
                                .search_attempt_count = 1};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"WebP 编码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"WebP 编码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"WebP 编码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
