module;
#include "../compat/stdlib_compat.h"

#include <algorithm>
#include <chrono>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <libheif/heif.h>
#include <avif/avif.h>
#include <avif/internal.h>
#include <libheif/heif_items.h>
#include <libheif/heif_properties.h>
#include <libheif/heif_tiling.h>
#include "heif_gain_map_adapter.h"

export module awj.heif_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.gain_map;
import awj.hdr_tonemap;
import awj.image;
import awj.pixel_conversion;
import awj.resource_planner;
import awj.large_image_plan;

export namespace awj {

namespace heif_detail {

struct ContextDeleter {
  void operator()(heif_context* value) const noexcept { heif_context_free(value); }
};
struct HandleDeleter {
  void operator()(heif_image_handle* value) const noexcept { heif_image_handle_release(value); }
};
struct ImageDeleter {
  void operator()(heif_image* value) const noexcept { heif_image_release(value); }
};
struct NclxDeleter {
  void operator()(heif_color_profile_nclx* value) const noexcept { heif_nclx_color_profile_free(value); }
};
struct DecodingOptionsDeleter {
  void operator()(heif_decoding_options* value) const noexcept { heif_decoding_options_free(value); }
};

using ContextPtr = std::unique_ptr<heif_context, ContextDeleter>;
using HandlePtr = std::unique_ptr<heif_image_handle, HandleDeleter>;
using ImagePtr = std::unique_ptr<heif_image, ImageDeleter>;
using NclxPtr = std::unique_ptr<heif_color_profile_nclx, NclxDeleter>;
using DecodingOptionsPtr = std::unique_ptr<heif_decoding_options, DecodingOptionsDeleter>;

std::string error_text(const heif_error& error) {
  return awj::compat::format("{} (code={}, subcode={})", error.message,
                     static_cast<int>(error.code), static_cast<int>(error.subcode));
}

PixelFormat source_pixel_format(heif_colorspace colorspace, heif_chroma chroma) noexcept {
  if (colorspace == heif_colorspace_monochrome) {
    return PixelFormat::gray;
  }
  if (colorspace == heif_colorspace_RGB) {
    return PixelFormat::rgb;
  }
  if (colorspace != heif_colorspace_YCbCr) {
    return PixelFormat::unknown;
  }
  switch (chroma) {
    case heif_chroma_420:
      return PixelFormat::yuv420;
    case heif_chroma_422:
      return PixelFormat::yuv422;
    case heif_chroma_444:
      return PixelFormat::yuv444;
    default:
      return PixelFormat::unknown;
  }
}

std::optional<int> cicp_value(int value) noexcept {
  return value == 2 ? std::optional<int>{} : std::optional<int>{value};
}

bool hdr_cicp(const ImageSourceInfo& source) noexcept {
  return source.color_primaries == 9 || source.transfer_characteristics == 16 ||
         source.transfer_characteristics == 18;
}

void apply_nclx(ImageSourceInfo& source, const heif_color_profile_nclx& nclx) noexcept {
  source.color_primaries = cicp_value(static_cast<int>(nclx.color_primaries));
  source.transfer_characteristics =
      cicp_value(static_cast<int>(nclx.transfer_characteristics));
  source.matrix_coefficients = cicp_value(static_cast<int>(nclx.matrix_coefficients));
  source.color_range = nclx.full_range_flag ? 1 : 0;
  source.color_metadata_source = "heif-nclx";
}

ImageSourceInfo source_info_from_handle(const heif_image_handle* handle) noexcept {
  ImageSourceInfo source{};
  heif_colorspace colorspace = heif_colorspace_undefined;
  heif_chroma chroma = heif_chroma_undefined;
  const auto preferred =
      heif_image_handle_get_preferred_decoding_colorspace(handle, &colorspace, &chroma);
  if (preferred.code == heif_error_Ok) {
    source.pixel_format = source_pixel_format(colorspace, chroma);
  }

  const int luma_bits = heif_image_handle_get_luma_bits_per_pixel(handle);
  source.bit_depth = luma_bits > 0 ? luma_bits : 0;

  heif_color_profile_nclx* raw_nclx = nullptr;
  const auto nclx_error = heif_image_handle_get_nclx_color_profile(handle, &raw_nclx);
  NclxPtr nclx{raw_nclx};
  if (nclx_error.code == heif_error_Ok && nclx) {
    apply_nclx(source, *nclx);
  }

  if (heif_image_handle_has_content_light_level(handle)) {
    heif_content_light_level clli{};
    if (heif_image_handle_get_content_light_level(handle, &clli)) {
      source.content_light = HdrContentLightMetadata{
          .max_cll = clli.max_content_light_level,
          .max_pall = clli.max_pic_average_light_level};
    }
  }
  source.has_hdr_metadata = hdr_cicp(source) || source.content_light.has_value() ||
                            heif_image_handle_has_mastering_display_colour_volume(handle) != 0;
  return source;
}

void fill_missing_nclx_from_decoded(ImageSourceInfo& source, const heif_image* image,
                                    bool has_input_icc = false) noexcept {
  heif_color_profile_nclx* raw_nclx = nullptr;
  const auto error = heif_image_get_nclx_color_profile(image, &raw_nclx);
  NclxPtr nclx{raw_nclx};
  if (error.code != heif_error_Ok || !nclx) {
    return;
  }
  // ICC 自己解释 RGB 色度和曲线。解码库补出的默认 NCLX 不等于输入信令，
  // 不能据此把 ICC 主图误标为 sRGB，或制造与真实 ICC 的假冲突。
  if (!has_input_icc && !source.color_primaries) {
    source.color_primaries = cicp_value(static_cast<int>(nclx->color_primaries));
  }
  if (!has_input_icc && !source.transfer_characteristics) {
    source.transfer_characteristics =
        cicp_value(static_cast<int>(nclx->transfer_characteristics));
  }
  if (!source.matrix_coefficients) {
    source.matrix_coefficients = cicp_value(static_cast<int>(nclx->matrix_coefficients));
  }
  if (!source.color_range) {
    source.color_range = nclx->full_range_flag ? 1 : 0;
  }
  if (!has_input_icc && source.color_metadata_source.empty()) {
    source.color_metadata_source = "heif-nclx";
  }
  source.has_hdr_metadata = source.has_hdr_metadata || hdr_cicp(source);
}

void begin_decode_diagnostics(DecodeDiagnostics* diagnostics, std::string_view stage) {
  if (diagnostics) {
    *diagnostics = DecodeDiagnostics{.decoder_id = "libheif-libde265", .stage = std::string{stage}};
  }
}

void capture_handle_diagnostics(DecodeDiagnostics* diagnostics, const heif_image_handle* handle) {
  if (!diagnostics) return;
  diagnostics->source_info = source_info_from_handle(handle);
  diagnostics->source_has_icc = heif_image_handle_get_raw_color_profile_size(handle) != 0;
  diagnostics->source_has_alpha_channel = heif_image_handle_has_alpha_channel(handle) != 0;
  diagnostics->source_alpha_mode = !diagnostics->source_has_alpha_channel ? AlphaMode::none :
      heif_image_handle_is_premultiplied_alpha(handle) ? AlphaMode::premultiplied : AlphaMode::straight;
  if (diagnostics->source_has_icc) {
    auto& source = *diagnostics->source_info;
    source.color_metadata_source = source.color_primaries || source.transfer_characteristics
        ? "heif-icc+nclx" : "heif-icc";
  }
}

awj::compat::expected<void, std::string> append_metadata(ImageBuffer& image, MetadataKind kind,
                                                 std::span<const std::byte> bytes,
                                                 std::size_t& total_bytes,
                                                 std::string_view label) {
  if (bytes.empty()) {
    return {};
  }
  if (bytes.size() > encoding_defaults::codec_metadata_max_bytes ||
      total_bytes > encoding_defaults::codec_metadata_max_bytes - bytes.size()) {
    return awj::compat::unexpected{awj::compat::format("HEIC {} metadata 超过运行时上限。", label)};
  }
  auto owned = decoder_common::make_byte_buffer(bytes.size(), "HEIC metadata");
  if (!owned) {
    return awj::compat::unexpected{owned.error()};
  }
  std::memcpy(owned->data(), bytes.data(), bytes.size());
  image.metadata.push_back(MetadataBlock{.kind = kind, .bytes = std::move(*owned)});
  total_bytes += bytes.size();
  return {};
}

std::uint32_t read_be_u32(std::span<const std::byte> bytes) noexcept {
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) << 24) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 8) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3]));
}

void copy_rgba16_row(std::span<std::byte> output, const std::uint8_t* input,
                     int significant_bits) noexcept {
  const auto maximum = (1u << significant_bits) - 1u;
  for (std::size_t offset = 0; offset < output.size(); offset += 2) {
    std::uint16_t value;
    std::memcpy(&value, input + offset, sizeof(value));
    value = static_cast<std::uint16_t>(
        (std::min<std::uint32_t>(value, maximum) * 65535u + maximum / 2u) / maximum);
    std::memcpy(output.data() + offset, &value, sizeof(value));
  }
}

void normalize_exif_orientation(std::span<std::byte> exif) noexcept {
  std::size_t offset = exif.size();
  if (avifGetExifOrientationOffset(reinterpret_cast<const std::uint8_t*>(exif.data()),
                                  exif.size(), &offset) == AVIF_RESULT_OK && offset < exif.size()) {
    exif[offset] = std::byte{1};
  }
}

awj::compat::expected<void, std::string> copy_metadata(ImageBuffer& image,
                                               const heif_image_handle* handle, bool normalize_orientation = true) {
  std::size_t total_bytes = 0;

  const auto icc_size = heif_image_handle_get_raw_color_profile_size(handle);
  if (icc_size > 0) {
    if (icc_size > encoding_defaults::codec_metadata_max_bytes) {
      return awj::compat::unexpected{"HEIC ICC metadata 超过运行时上限。"};
    }
    auto icc = decoder_common::make_byte_buffer(icc_size, "HEIC ICC metadata");
    if (!icc) {
      return awj::compat::unexpected{icc.error()};
    }
    const auto error = heif_image_handle_get_raw_color_profile(handle, icc->data());
    if (error.code != heif_error_Ok) {
      return awj::compat::unexpected{awj::compat::format("HEIC ICC 读取失败: {}", error_text(error))};
    }
    auto appended = append_metadata(image, MetadataKind::icc, *icc, total_bytes, "ICC");
    if (!appended) {
      return awj::compat::unexpected{appended.error()};
    }
    if (image.source_info) {
      image.source_info->color_metadata_source = "source-icc";
    }
  }

  const int metadata_count = heif_image_handle_get_number_of_metadata_blocks(handle, nullptr);
  if (metadata_count <= 0) {
    return {};
  }
  constexpr int max_metadata_blocks = 4096;
  if (metadata_count > max_metadata_blocks) {
    return awj::compat::unexpected{"HEIC metadata block 数量超过运行时上限。"};
  }
  std::vector<heif_item_id> ids(static_cast<std::size_t>(metadata_count));
  const int id_count = heif_image_handle_get_list_of_metadata_block_IDs(
      handle, nullptr, ids.data(), metadata_count);
  if (id_count < 0 || id_count > metadata_count) {
    return awj::compat::unexpected{"HEIC metadata block 索引无效。"};
  }

  bool copied_exif = false;
  bool copied_xmp = false;
  for (int i = 0; i < id_count && (!copied_exif || !copied_xmp); ++i) {
    const auto id = ids[static_cast<std::size_t>(i)];
    const char* raw_type = heif_image_handle_get_metadata_type(handle, id);
    const std::string type = raw_type != nullptr ? raw_type : "";
    const char* raw_content_type = heif_image_handle_get_metadata_content_type(handle, id);
    const std::string content_type = raw_content_type != nullptr ? raw_content_type : "";

    const bool is_exif = !copied_exif && type == "Exif";
    const bool is_xmp = !copied_xmp &&
                        (type == "XMP" || content_type == "application/rdf+xml");
    if (!is_exif && !is_xmp) {
      continue;
    }

    const auto size = heif_image_handle_get_metadata_size(handle, id);
    if (size == 0) {
      continue;
    }
    if (size > encoding_defaults::codec_metadata_max_bytes ||
        total_bytes > encoding_defaults::codec_metadata_max_bytes - size) {
      return awj::compat::unexpected{"HEIC metadata payload 超过运行时上限。"};
    }
    auto payload = decoder_common::make_byte_buffer(size, "HEIC metadata payload");
    if (!payload) {
      return awj::compat::unexpected{payload.error()};
    }
    const auto error = heif_image_handle_get_metadata(handle, id, payload->data());
    if (error.code != heif_error_Ok) {
      return awj::compat::unexpected{awj::compat::format("HEIC metadata 读取失败: {}", error_text(error))};
    }

    if (is_exif) {
      if (payload->size() <= 4) {
        continue;
      }
      const auto offset = static_cast<std::size_t>(
          read_be_u32(std::span<const std::byte>{payload->data(), 4}));
      if (offset >= payload->size() - 4) {
        continue;
      }
      const auto start = 4 + offset;
      // libheif already applied HEIF's irot/imir; do not let an output reader rotate again.
      if (normalize_orientation) normalize_exif_orientation(std::span<std::byte>{payload->data() + start, payload->size() - start});
      auto appended = append_metadata(
          image, MetadataKind::exif,
          std::span<const std::byte>{payload->data() + start, payload->size() - start},
          total_bytes, "Exif");
      if (!appended) {
        return awj::compat::unexpected{appended.error()};
      }
      copied_exif = true;
    } else {
      auto appended = append_metadata(image, MetadataKind::xmp, *payload, total_bytes, "XMP");
      if (!appended) {
        return awj::compat::unexpected{appended.error()};
      }
      copied_xmp = true;
    }
  }
  return {};
}

awj::compat::expected<HandlePtr, std::string> open_primary_image(heif_context* context,
                                                        std::span<const std::byte> bytes,
                                                        std::string_view source_name) {
  const auto read_error = heif_context_read_from_memory_without_copy(
      context, bytes.data(), bytes.size(), nullptr);
  if (read_error.code != heif_error_Ok) {
    return awj::compat::unexpected{
        awj::compat::format("HEIC 容器读取失败: {}: {}", source_name, error_text(read_error))};
  }
  heif_image_handle* raw_handle = nullptr;
  const auto handle_error = heif_context_get_primary_image_handle(context, &raw_handle);
  if (handle_error.code != heif_error_Ok || raw_handle == nullptr) {
    return awj::compat::unexpected{
        awj::compat::format("HEIC 主图读取失败: {}: {}", source_name, error_text(handle_error))};
  }
  return HandlePtr{raw_handle};
}

awj::compat::expected<ImageDecodeResult, std::string> decode_handle(const heif_image_handle* handle,
                                                           std::string_view source_name,
                                                           int decode_threads,
                                                           bool copy_metadata_payloads,
                                                           awj::compat::stop_token stop_token = {}, bool ignore_transformations = false,
                                                           bool native_pixels = false, bool preserve_icc_signal = false) {
  if (stop_token.stop_requested()) {
    return awj::compat::unexpected{"任务已取消。"};
  }
  const int threads = std::clamp(decode_threads, 1,
                                 encoding_defaults::max_automatic_thread_budget);
  // 此处接收已扣除外层 tile 并行后的 codec 预算，不能再次乘上文件预算。
  auto source_info = source_info_from_handle(handle);
  const bool has_input_icc = heif_image_handle_get_raw_color_profile_size(handle) != 0;
  const bool has_alpha = heif_image_handle_has_alpha_channel(handle) != 0;
  const bool premultiplied = has_alpha &&
                             heif_image_handle_is_premultiplied_alpha(handle) != 0;
  const int source_bit_depth = source_info.bit_depth > 0 ? source_info.bit_depth : 8;
  const int output_bit_depth = source_bit_depth > 8 ? 16 : 8;
  native_pixels = native_pixels && (source_bit_depth == 8 || source_bit_depth == 10 ||
                                   source_bit_depth == 12 || source_bit_depth == 16);

  DecodingOptionsPtr options{heif_decoding_options_alloc()};
  if (!options) {
    return awj::compat::unexpected{"无法创建 libheif decoding options。"};
  }
  options->ignore_transformations = ignore_transformations ? 1 : 0;
  options->convert_hdr_to_8bit = 0;
  options->strict_decoding = 1;
  options->num_codec_threads = threads;
  options->progress_user_data = &stop_token;
  options->cancel_decoding = [](void* user_data) -> int {
    return static_cast<const awj::compat::stop_token*>(user_data)->stop_requested() ? 1 : 0;
  };
  options->output_image_nclx_profile = nullptr;
  options->output_image_nclx_profile_passthrough = 1;

  const auto rgba_chroma = output_bit_depth > 8
      ? (std::endian::native == std::endian::little
             ? heif_chroma_interleaved_RRGGBBAA_LE
             : heif_chroma_interleaved_RRGGBBAA_BE)
      : heif_chroma_interleaved_RGBA;
  heif_image* raw_image = nullptr;
  const auto decode_error = heif_decode_image(handle, &raw_image,
                                               native_pixels ? heif_colorspace_undefined : heif_colorspace_RGB,
                                               native_pixels ? heif_chroma_undefined : rgba_chroma,
                                               options.get());
  if (stop_token.stop_requested()) {
    if (raw_image != nullptr) heif_image_release(raw_image);
    return awj::compat::unexpected{"任务已取消。"};
  }
  ImagePtr decoded{raw_image};
  if (decode_error.code != heif_error_Ok || !decoded) {
    return awj::compat::unexpected{
        awj::compat::format("HEIC 解码失败: {}: {}", source_name, error_text(decode_error))};
  }
  fill_missing_nclx_from_decoded(source_info, decoded.get(), preserve_icc_signal && has_input_icc);

  if (native_pixels && (heif_image_get_colorspace(decoded.get()) == heif_colorspace_YCbCr ||
      heif_image_get_colorspace(decoded.get()) == heif_colorspace_monochrome)) {
    avifImage view{};
    const auto chroma = heif_image_get_chroma_format(decoded.get());
    view.yuvFormat = chroma == heif_chroma_420 ? AVIF_PIXEL_FORMAT_YUV420
        : chroma == heif_chroma_422 ? AVIF_PIXEL_FORMAT_YUV422
        : chroma == heif_chroma_444 ? AVIF_PIXEL_FORMAT_YUV444 : AVIF_PIXEL_FORMAT_YUV400;
    const int width = heif_image_get_width(decoded.get(), heif_channel_Y);
    const int height = heif_image_get_height(decoded.get(), heif_channel_Y);
    const int depth = heif_image_get_bits_per_pixel_range(decoded.get(), heif_channel_Y);
    if (width <= 0 || height <= 0 || (depth != 8 && depth != 10 && depth != 12 && depth != 16))
      return awj::compat::unexpected{"HEIF 原生 YUV 尺寸或位深无效。"};
    view.width = width; view.height = height; view.depth = depth;
    view.matrixCoefficients = static_cast<avifMatrixCoefficients>(source_info.matrix_coefficients.value_or(6));
    view.colorPrimaries = static_cast<avifColorPrimaries>(source_info.color_primaries.value_or(2));
    view.transferCharacteristics = static_cast<avifTransferCharacteristics>(source_info.transfer_characteristics.value_or(2));
    view.yuvRange = source_info.color_range == 0 ? AVIF_RANGE_LIMITED : AVIF_RANGE_FULL;
    avifYUVColorSpaceInfo color{};
    if (!avifGetYUVColorSpaceInfo(&view, &color)) {
      decoded.reset();
      return decode_handle(handle, source_name, decode_threads, copy_metadata_payloads,
                           stop_token, ignore_transformations, false, preserve_icc_signal);
    }
    std::vector<std::byte> scaled_alpha;
    const std::array<heif_channel, 4> channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr, heif_channel_Alpha};
    for (std::size_t c = 0; c < 4; ++c) {
      if ((view.yuvFormat == AVIF_PIXEL_FORMAT_YUV400 && (c == 1 || c == 2)) || (c == 3 && !has_alpha)) continue;
      std::size_t stride{};
      const auto* pixels = heif_image_get_plane_readonly2(decoded.get(), channels[c], &stride);
      const int plane_depth = heif_image_get_bits_per_pixel_range(decoded.get(), channels[c]);
      if (!pixels || stride > UINT32_MAX || plane_depth <= 0 || plane_depth > 16 || (c != 3 && plane_depth != depth))
        return awj::compat::unexpected{"HEIF 原生 YUV 平面无效。"};
      if (c == 3 && plane_depth != depth) {
        const auto source_bytes = plane_depth > 8 ? 2u : 1u;
        const auto target_bytes = depth > 8 ? 2u : 1u;
        if (stride < static_cast<std::size_t>(width) * source_bytes ||
            heif_image_get_width(decoded.get(), channels[c]) != width ||
            heif_image_get_height(decoded.get(), channels[c]) != height)
          return awj::compat::unexpected{"HEIF alpha 尺寸无效。"};
        auto count = decoder_common::checked_image_bytes(static_cast<std::size_t>(width) * target_bytes, height, "HEIF alpha");
        if (!count) return awj::compat::unexpected{count.error()};
        auto storage = decoder_common::make_byte_buffer(*count, "HEIF alpha");
        if (!storage) return awj::compat::unexpected{storage.error()};
        scaled_alpha = std::move(*storage);
        const std::uint32_t source_max = (1u << plane_depth) - 1, target_max = (1u << depth) - 1;
        for (int y = 0; y < height; ++y) {
          if (stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
          for (int x = 0; x < width; ++x) {
            std::uint16_t sample{};
            if (source_bytes == 1) sample = pixels[y * stride + x];
            else std::memcpy(&sample, pixels + y * stride + x * 2, 2);
            const auto value = static_cast<std::uint16_t>((static_cast<std::uint64_t>(sample) * target_max + source_max / 2) / source_max);
            if (target_bytes == 1) scaled_alpha[static_cast<std::size_t>(y) * width + x] = std::byte{static_cast<unsigned char>(value)};
            else std::memcpy(scaled_alpha.data() + (static_cast<std::size_t>(y) * width + x) * 2, &value, 2);
          }
        }
        pixels = reinterpret_cast<const std::uint8_t*>(scaled_alpha.data());
        stride = static_cast<std::size_t>(width) * target_bytes;
      }
      if (c == 3) { view.alphaPlane = const_cast<std::uint8_t*>(pixels); view.alphaRowBytes = static_cast<std::uint32_t>(stride); }
      else { view.yuvPlanes[c] = const_cast<std::uint8_t*>(pixels); view.yuvRowBytes[c] = static_cast<std::uint32_t>(stride); }
    }
    view.alphaPremultiplied = premultiplied ? AVIF_TRUE : AVIF_FALSE;
    auto image = copy_yuv_pixels(view, stop_token);
    if (!image) return awj::compat::unexpected{image.error()};
    source_info.pixel_format = image->pixel_format; image->source_info = source_info;
    if (copy_metadata_payloads) {
      if (auto copied = copy_metadata(*image, handle, !ignore_transformations); !copied) return awj::compat::unexpected{copied.error()};
    }
    return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libheif-libde265"};
  }
  if (native_pixels && heif_image_get_colorspace(decoded.get()) == heif_colorspace_RGB) {
    const auto chroma = heif_image_get_chroma_format(decoded.get());
    const bool packed = chroma == heif_chroma_interleaved_RGB || chroma == heif_chroma_interleaved_RGBA ||
        chroma == heif_chroma_interleaved_RRGGBB_LE || chroma == heif_chroma_interleaved_RRGGBB_BE ||
        chroma == heif_chroma_interleaved_RRGGBBAA_LE || chroma == heif_chroma_interleaved_RRGGBBAA_BE;
    const bool packed_alpha = chroma == heif_chroma_interleaved_RGBA ||
        chroma == heif_chroma_interleaved_RRGGBBAA_LE || chroma == heif_chroma_interleaved_RRGGBBAA_BE;
    const auto first = packed ? heif_channel_interleaved : heif_channel_R;
    const int width = heif_image_get_width(decoded.get(), first), height = heif_image_get_height(decoded.get(), first);
    const int precision = heif_image_get_bits_per_pixel_range(decoded.get(), first);
    if (width <= 0 || height <= 0 || precision <= 0 || precision > 16) return awj::compat::unexpected{"HEIF 原生 RGB 尺寸无效。"};
    const int storage_depth = precision > 8 ? 16 : 8;
    const auto bytes_per_sample = storage_depth / 8;
    auto count = decoder_common::checked_image_bytes(static_cast<std::size_t>(width) * 4 * bytes_per_sample, height, "HEIF native RGB");
    if (!count) return awj::compat::unexpected{count.error()};
    auto storage = decoder_common::make_byte_buffer(*count, "HEIF native RGB");
    if (!storage) return awj::compat::unexpected{storage.error()};
    const std::array<heif_channel, 4> channels{heif_channel_R, heif_channel_G, heif_channel_B, heif_channel_Alpha};
    std::array<const std::uint8_t*, 4> planes{}; std::array<std::size_t, 4> strides{};
    std::array<int, 4> precisions{precision, precision, precision, precision};
    for (std::size_t c = 0; c < (has_alpha ? 4u : 3u); ++c) {
      const auto channel = packed && (c < 3 || packed_alpha) ? first : channels[c];
      planes[c] = heif_image_get_plane_readonly2(decoded.get(), channel, &strides[c]);
      precisions[c] = heif_image_get_bits_per_pixel_range(decoded.get(), channel);
      if (!planes[c] || precisions[c] <= 0 || precisions[c] > 16 ||
          heif_image_get_width(decoded.get(), channel) != width || heif_image_get_height(decoded.get(), channel) != height ||
          strides[c] < static_cast<std::size_t>(width) * (precisions[c] > 8 ? 2u : 1u) *
              (channel == first && packed ? packed_alpha ? 4u : 3u : 1u))
        return awj::compat::unexpected{"HEIF 原生 RGB 平面无效。"};
    }
    const auto maximum = (1u << storage_depth) - 1;
    const bool big_endian = chroma == heif_chroma_interleaved_RRGGBB_BE || chroma == heif_chroma_interleaved_RRGGBBAA_BE;
    for (int y = 0; y < height; ++y) {
      if (stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
      for (int x = 0; x < width; ++x) for (std::size_t c = 0; c < 4; ++c) {
        std::uint16_t sample = static_cast<std::uint16_t>(maximum);
        if (c < 3 || has_alpha) {
          const bool interleaved = packed && (c < 3 || packed_alpha);
          const auto source_bytes = precisions[c] > 8 ? 2u : 1u;
          const auto* position = planes[c] + y * strides[c] +
              (interleaved ? static_cast<std::size_t>(x) * (packed_alpha ? 4u : 3u) + c : x) * source_bytes;
          if (source_bytes == 1) sample = *position;
          else { std::memcpy(&sample, position, 2); if (interleaved && big_endian != (std::endian::native == std::endian::big)) sample = std::byteswap(sample); }
          const auto source_maximum = (1u << precisions[c]) - 1;
          sample = static_cast<std::uint16_t>((static_cast<std::uint64_t>(sample) * maximum + source_maximum / 2) / source_maximum);
        }
        const auto offset = (static_cast<std::size_t>(y) * width * 4 + x * 4 + c) * bytes_per_sample;
        if (bytes_per_sample == 1) (*storage)[offset] = std::byte{static_cast<unsigned char>(sample)};
        else std::memcpy(storage->data() + offset, &sample, 2);
      }
    }
    ImageBuffer image{.width = static_cast<std::size_t>(width), .height = static_cast<std::size_t>(height),
        .pixel_format = PixelFormat::rgba, .alpha_mode = has_alpha ? premultiplied ? AlphaMode::premultiplied : AlphaMode::straight : AlphaMode::none,
        .bit_depth = storage_depth, .source_info = source_info};
    image.planes.push_back(ImagePlane{.bytes = std::move(*storage), .stride = static_cast<std::size_t>(width) * 4 * bytes_per_sample});
    image.significant_bits = SignificantBits{precisions[0], precisions[1], precisions[2], precisions[3]};
    if (copy_metadata_payloads) { if (auto copied = copy_metadata(image, handle, !ignore_transformations); !copied) return awj::compat::unexpected{copied.error()}; }
    return ImageDecodeResult{.image = std::move(image), .decoder_id = "libheif-libde265"};
  }

  const int width_i = heif_image_get_width(decoded.get(), heif_channel_interleaved);
  const int height_i = heif_image_get_height(decoded.get(), heif_channel_interleaved);
  if (width_i <= 0 || height_i <= 0) {
    return awj::compat::unexpected{awj::compat::format("HEIC 解码尺寸无效: {}", source_name)};
  }
  auto dimensions = decoder_common::make_image_dimensions_checked(
      static_cast<std::uint32_t>(width_i), static_cast<std::uint32_t>(height_i),
      "HEIC decoder");
  if (!dimensions) {
    return awj::compat::unexpected{dimensions.error()};
  }

  const std::size_t bytes_per_sample = output_bit_depth > 8 ? 2 : 1;
  const auto packed_stride = decoder_common::checked_rgba_stride(
      dimensions->width, "HEIC decoder", bytes_per_sample);
  if (!packed_stride) {
    return awj::compat::unexpected{packed_stride.error()};
  }
  const auto byte_count = decoder_common::checked_image_bytes(
      *packed_stride, dimensions->height, "HEIC decoder");
  if (!byte_count) {
    return awj::compat::unexpected{byte_count.error()};
  }
  auto rgba = decoder_common::make_byte_buffer(*byte_count, "HEIC decoder");
  if (!rgba) {
    return awj::compat::unexpected{rgba.error()};
  }
  std::size_t source_stride = 0;
  const auto* source_pixels = heif_image_get_plane_readonly2(
      decoded.get(), heif_channel_interleaved, &source_stride);
  if (source_pixels == nullptr || source_stride < *packed_stride) {
    return awj::compat::unexpected{awj::compat::format("HEIC RGBA plane 无效: {}", source_name)};
  }
  const int decoded_bits = heif_image_get_bits_per_pixel_range(
      decoded.get(), heif_channel_interleaved);
  if (decoded_bits <= 0 || decoded_bits > output_bit_depth) {
    return awj::compat::unexpected{"HEIC RGBA sample precision is invalid."};
  }
  for (std::size_t row = 0; row < dimensions->height; ++row) {
    if (stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    // libheif stores 10/12-bit samples in 16-bit words without expanding them.
    if (output_bit_depth == 16 && decoded_bits < 16) {
      copy_rgba16_row(std::span<std::byte>{rgba->data() + row * *packed_stride, *packed_stride},
                      source_pixels + row * source_stride, decoded_bits);
    } else {
      std::memcpy(rgba->data() + row * *packed_stride,
                  source_pixels + row * source_stride, *packed_stride);
    }
  }

  auto image = decoder_common::make_rgba_image(
      dimensions->width, dimensions->height, std::move(*rgba),
      has_alpha ? (premultiplied ? AlphaMode::premultiplied : AlphaMode::straight)
                : AlphaMode::none,
      "HEIC decoder", source_info, output_bit_depth);
  if (!image) {
    return awj::compat::unexpected{image.error()};
  }
  if (source_bit_depth > 0) {
    image->significant_bits = SignificantBits{
        .red = source_bit_depth,
        .green = source_bit_depth,
        .blue = source_bit_depth,
        .alpha = source_bit_depth};
  }
  if (copy_metadata_payloads) {
    if (auto copied = copy_metadata(*image, handle, !ignore_transformations); !copied) {
      return awj::compat::unexpected{copied.error()};
    }
  }
  return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libheif-libde265"};
}

awj::compat::expected<void, std::string> parse_iso_metadata(std::span<const std::byte> bytes, avifGainMap& map) {
  // ToneMapImage + GainMapMetadata, ISO 21496-1 C.2.2; same syntax as libavif read.c.
  if (bytes.size() < 22 || bytes[0] != std::byte{0})
    return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: ISO metadata version 不支持或截断。"};
  const auto minimum_version = (std::to_integer<unsigned>(bytes[1]) << 8) | std::to_integer<unsigned>(bytes[2]);
  const auto writer_version = (std::to_integer<unsigned>(bytes[3]) << 8) | std::to_integer<unsigned>(bytes[4]);
  if (minimum_version != 0 || writer_version < minimum_version)
    return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: ISO minimum metadata version 不支持。"};
  const auto flags = std::to_integer<unsigned>(bytes[5]);
  const auto channels = flags & 128 ? 3u : 1u;
  const auto required = 22 + channels * 40;
  // Match pinned libavif: future writers may append fields when minimum=0;
  // only a writer version whose complete syntax we know requires exact EOF.
  if ((flags & 63) || bytes.size() < required || (writer_version == 0 && bytes.size() != required))
    return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: ISO metadata 长度/保留位无效。"};
  map.useBaseColorSpace = flags & 64 ? AVIF_TRUE : AVIF_FALSE;
  std::size_t pos = 6;
  auto u32 = [&] { const auto result = read_be_u32(bytes.subspan(pos, 4)); pos += 4; return result; };
  auto unsigned_fraction = [&] { return avifUnsignedFraction{u32(), u32()}; };
  auto signed_fraction = [&] { const auto numerator = std::bit_cast<std::int32_t>(u32()); return avifSignedFraction{numerator, u32()}; };
  map.baseHdrHeadroom = unsigned_fraction();
  map.alternateHdrHeadroom = unsigned_fraction();
  for (unsigned c = 0; c < channels; ++c) {
    map.gainMapMin[c] = signed_fraction(); map.gainMapMax[c] = signed_fraction();
    map.gainMapGamma[c] = unsigned_fraction(); map.baseOffset[c] = signed_fraction();
    map.alternateOffset[c] = signed_fraction();
  }
  for (unsigned c = channels; c < 3; ++c) {
    map.gainMapMin[c] = map.gainMapMin[0]; map.gainMapMax[c] = map.gainMapMax[0];
    map.gainMapGamma[c] = map.gainMapGamma[0]; map.baseOffset[c] = map.baseOffset[0];
    map.alternateOffset[c] = map.alternateOffset[0];
  }
  avifDiagnostics diagnostics{};
  if (avifGainMapValidateMetadata(&map, &diagnostics) != AVIF_RESULT_OK)
    return awj::compat::unexpected{std::string{"Gain Map: ISO 21496-1: HEIF metadata 无效: "} + diagnostics.error};
  return {};
}

awj::compat::expected<void, std::string> apply_transformations(heif_context* context, heif_item_id id, ImageBuffer& image,
    bool apply_exif = true) {
  const auto count = heif_item_get_transformation_properties(context, id, nullptr, 0);
  if (count < 0 || count > 16) return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: 方向属性数量无效。"};
  std::vector<heif_property_id> properties(count);
  if (heif_item_get_transformation_properties(context, id, properties.data(), count) != count)
    return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: 方向属性索引无效。"};
  bool oriented = false;
  for (auto property : properties) {
    const auto type = heif_item_get_property_type(context, id, property);
    awj::compat::expected<void, std::string> changed;
    if (type == heif_item_property_type_transform_rotation) {
      const auto angle = heif_item_get_property_transform_rotation_ccw(context, id, property);
      if (angle < 0 || angle % 90) return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: rotation 无效。"};
      changed = gain_map_detail::transform_rgba(image, angle / 90);
      oriented = true;
    } else if (type == heif_item_property_type_transform_mirror) {
      const auto axis = heif_item_get_property_transform_mirror(context, id, property);
      if (axis == heif_transform_mirror_direction_invalid) return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: mirror 无效。"};
      changed = gain_map_detail::transform_rgba(image, 0, static_cast<int>(axis));
      oriented = true;
    } else if (type == heif_item_property_type_transform_crop) {
      int left{}, top{}, right{}, bottom{};
      heif_item_get_property_transform_crop_borders(context, id, property,
          static_cast<int>(image.width), static_cast<int>(image.height), &left, &top, &right, &bottom);
      if (left < 0 || top < 0 || right < 0 || bottom < 0 ||
          static_cast<std::size_t>(left) + right >= image.width || static_cast<std::size_t>(top) + bottom >= image.height)
        return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: crop 无效。"};
      changed = gain_map_detail::crop_rgba(image, left, top, image.width - left - right, image.height - top - bottom);
    } else return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: 使用不支持的变换属性。"};
    if (!changed) return changed;
  }
  if (oriented) gain_map_detail::normalize_exif_orientation(image);
  else if (apply_exif) return gain_map_detail::apply_exif_orientation(image);
  return {};
}

void make_alpha_straight(ImageBuffer& image) {
  if (image.alpha_mode != AlphaMode::premultiplied) return;
  auto& plane = image.planes.front();
  const auto maximum = image.bit_depth == 16 ? 65535u : 255u;
  const auto sample_bytes = image.bit_depth == 16 ? 2u : 1u;
  for (std::size_t y = 0; y < image.height; ++y) {
    auto* row = plane.bytes.data() + y * plane.stride;
    for (std::size_t x = 0; x < image.width; ++x) {
      const auto alpha = hdr::hdr_detail::read_sample(image, row, x * 4 + 3);
      for (std::size_t c = 0; c < 3; ++c) {
        const auto value = static_cast<std::uint16_t>(std::clamp(
            std::lround((alpha > 0 ? hdr::hdr_detail::read_sample(image, row, x * 4 + c) / alpha : 0) * maximum), 0l, static_cast<long>(maximum)));
        if (sample_bytes == 1) row[x * 4 + c] = std::byte{static_cast<unsigned char>(value)};
        else std::memcpy(row + (x * 4 + c) * 2, &value, 2);
      }
    }
  }
  image.alpha_mode = AlphaMode::straight;
}

using GainMapPtr = std::unique_ptr<avifGainMap, decltype(&avifGainMapDestroy)>;
struct GainMapSelection {
  heif_item_id base_id{};
  std::optional<heif_item_id> tmap_id, auxiliary_id;
  GainMapPtr metadata{nullptr, &avifGainMapDestroy};
};

// Shared by file/memory decoding and dimension probing. Relationships, not an
// auxiliary's vendor URN, decide whether ISO reconstruction is available.
awj::compat::expected<GainMapSelection, std::string> select_gain_map(
    heif_context* context, const heif_image_handle* primary) {
  GainMapSelection selected;
  if (heif_context_get_primary_image_ID(context, &selected.base_id).code != heif_error_Ok)
    return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF primary ID 读取失败。"};
  const int item_count = heif_context_get_number_of_items(context);
  if (item_count < 0 || item_count > 16384) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF item 数量超出上限。"};
  std::vector<heif_item_id> items(item_count);
  if (heif_context_get_list_of_item_IDs(context, items.data(), item_count) != item_count)
    return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF item 索引无效。"};
  for (auto id : items) {
    if (heif_item_get_item_type(context, id) != heif_fourcc('t','m','a','p')) continue;
    bool has_dimg = false;
    for (int index = 0; index < 16; ++index) {
      uint32_t type{}; heif_item_id* references = nullptr;
      const auto count = heif_context_get_item_references(context, id, index, &type, &references);
      struct References { heif_context* ctx; heif_item_id** data; ~References() { heif_release_item_references(ctx, data); } } guard{context, &references};
      if (!count) break;
      if (type != heif_fourcc('d','i','m','g')) continue;
      has_dimg = true;
      if (count != 2 || !references || references[0] == references[1])
        return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF tmap 必须恰好引用主图与一个增益图。"};
      if (references[0] != selected.base_id) continue;
      if (selected.tmap_id) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF primary 存在多个 ISO gain map。"};
      selected.tmap_id = id; selected.auxiliary_id = references[1];
    }
    if (!has_dimg) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF tmap 缺少 dimg 关系。"};
  }
  const int filter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA | LIBHEIF_AUX_IMAGE_FILTER_OMIT_DEPTH;
  const int count = heif_image_handle_get_number_of_auxiliary_images(primary, filter);
  if (count < 0 || count > 4096) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF auxiliary 数量无效。"};
  std::vector<heif_item_id> auxiliary_ids(count);
  if (heif_image_handle_get_list_of_auxiliary_image_IDs(primary, filter, auxiliary_ids.data(), count) != count)
    return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF auxiliary 索引无效。"};
  for (auto id : auxiliary_ids) {
    heif_image_handle* raw_handle = nullptr;
    if (heif_image_handle_get_auxiliary_image_handle(primary, id, &raw_handle).code != heif_error_Ok || !raw_handle)
      return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF auxiliary handle 无效。"};
    HandlePtr handle{raw_handle};
    const char* type = nullptr;
    const auto error = heif_image_handle_get_auxiliary_type(handle.get(), &type);
    const std::string urn = type ? type : "";
    if (type) heif_image_handle_release_auxiliary_type(handle.get(), &type);
    if (error.code != heif_error_Ok) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF auxiliary 类型无效。"};
    if (urn == "urn:com:apple:photo:2020:aux:hdrgainmap") {
      if (selected.tmap_id && selected.auxiliary_id == id) continue;
      if (selected.tmap_id) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF primary 存在多个不同的 gain map。"};
      return awj::compat::unexpected{std::string{gain_map_detail::legacy_gain_map_error}};
    }
    if (urn.find("21496") != urn.npos && (!selected.tmap_id || selected.auxiliary_id != id))
      return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF auxiliary 缺少支持的 tmap 元数据。"};
  }
  if (!selected.tmap_id) return selected;
  uint8_t* raw_data = nullptr; std::size_t size{};
  auto error = heif_item_get_item_data(context, *selected.tmap_id, nullptr, &raw_data, &size);
  struct Data { heif_context* ctx; uint8_t** data; ~Data() { heif_release_item_data(ctx, data); } } guard{context, &raw_data};
  if (error.code != heif_error_Ok || !raw_data || size > encoding_defaults::codec_metadata_max_bytes)
    return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF metadata 缺失或过大。"};
  selected.metadata.reset(avifGainMapCreate());
  if (!selected.metadata) return awj::compat::unexpected{"Gain Map: ISO 21496-1: 无法分配 metadata。"};
  if (auto parsed = parse_iso_metadata({reinterpret_cast<const std::byte*>(raw_data), size}, *selected.metadata); !parsed)
    return awj::compat::unexpected{parsed.error()};
  int primaries{}, transfer{}, matrix{};
  const auto color = awjHeifItemNclx(context, *selected.tmap_id, &primaries, &transfer, &matrix);
  if (color < 0) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF alternate 色彩 profile 暂不支持。"};
  if (color) {
    selected.metadata->altColorPrimaries = static_cast<avifColorPrimaries>(primaries);
    selected.metadata->altTransferCharacteristics = static_cast<avifTransferCharacteristics>(transfer);
    selected.metadata->altMatrixCoefficients = static_cast<avifMatrixCoefficients>(matrix);
  }
  return selected;
}

awj::compat::expected<ImageDimensions, std::string> gain_map_dimensions(
    heif_context* context, const heif_image_handle* handle, const GainMapSelection& selected) {
  const int width = heif_image_handle_get_width(handle), height = heif_image_handle_get_height(handle);
  if (width <= 0 || height <= 0) return awj::compat::unexpected{"HEIC 图像尺寸无效。"};
  auto dimensions = decoder_common::make_image_dimensions_checked(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), "HEIC");
  if (!dimensions || !selected.tmap_id) return dimensions;
  const auto count = heif_item_get_transformation_properties(context, *selected.tmap_id, nullptr, 0);
  if (count < 0 || count > 16) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF 方向属性数量无效。"};
  std::vector<heif_property_id> properties(count);
  if (heif_item_get_transformation_properties(context, *selected.tmap_id, properties.data(), count) != count)
    return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF 方向属性索引无效。"};
  for (auto property : properties) {
    const auto type = heif_item_get_property_type(context, *selected.tmap_id, property);
    if (type == heif_item_property_type_transform_rotation) {
      const auto angle = heif_item_get_property_transform_rotation_ccw(context, *selected.tmap_id, property);
      if (angle < 0 || angle % 90) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF rotation 无效。"};
      if ((angle / 90) & 1) std::swap(dimensions->width, dimensions->height);
    } else if (type == heif_item_property_type_transform_mirror) {
      if (heif_item_get_property_transform_mirror(context, *selected.tmap_id, property) == heif_transform_mirror_direction_invalid)
        return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF mirror 无效。"};
    } else if (type == heif_item_property_type_transform_crop) {
      int left{}, top{}, right{}, bottom{};
      heif_item_get_property_transform_crop_borders(context, *selected.tmap_id, property,
          static_cast<int>(dimensions->width), static_cast<int>(dimensions->height), &left, &top, &right, &bottom);
      if (left < 0 || top < 0 || right < 0 || bottom < 0 ||
          static_cast<std::size_t>(left) >= dimensions->width || static_cast<std::size_t>(right) >= dimensions->width - left ||
          static_cast<std::size_t>(top) >= dimensions->height || static_cast<std::size_t>(bottom) >= dimensions->height - top)
        return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF crop 无效。"};
      dimensions->width -= left + right; dimensions->height -= top + bottom;
    } else return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF 使用不支持的变换属性。"};
  }
  // Match reconstruction's normalization rule: base/tmap irot or imir wins
  // over Exif; otherwise apply Exif once, after tmap crop/rotation geometry.
  bool oriented = false;
  for (auto id : {selected.base_id, *selected.tmap_id}) {
    const auto transform_count = heif_item_get_transformation_properties(context, id, nullptr, 0);
    if (transform_count < 0 || transform_count > 16) return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF 方向属性数量无效。"};
    std::vector<heif_property_id> transforms(transform_count);
    if (heif_item_get_transformation_properties(context, id, transforms.data(), transform_count) != transform_count)
      return awj::compat::unexpected{"Gain Map: ISO 21496-1: HEIF 方向属性索引无效。"};
    for (auto transform : transforms) {
      const auto type = heif_item_get_property_type(context, id, transform);
      oriented |= type == heif_item_property_type_transform_rotation || type == heif_item_property_type_transform_mirror;
    }
  }
  if (!oriented) {
    ImageBuffer metadata;
    if (auto copied = copy_metadata(metadata, handle, false); !copied)
      return awj::compat::unexpected{"Gain Map: ISO 21496-1: " + copied.error()};
    for (const auto& block : metadata.metadata) if (block.kind == MetadataKind::exif) {
      std::size_t offset{};
      if (avifGetExifOrientationOffset(reinterpret_cast<const std::uint8_t*>(block.bytes.data()), block.bytes.size(), &offset) != AVIF_RESULT_OK ||
          offset >= block.bytes.size()) continue;
      const auto orientation = std::to_integer<unsigned>(block.bytes[offset]);
      if (orientation < 1 || orientation > 8) return awj::compat::unexpected{"Gain Map: ISO 21496-1: Exif orientation 无效。"};
      if (orientation >= 5) std::swap(dimensions->width, dimensions->height);
      break;
    }
  }
  return dimensions;
}

awj::compat::expected<ImageDecodeResult, std::string> decode_bytes(std::span<const std::byte> bytes,
    std::string_view source_name, int decode_threads, bool copy_metadata_payloads, awj::compat::stop_token stop_token = {},
    bool native_pixels = false, GainMapDecodeMode gain_map_mode = GainMapDecodeMode::hdr_only,
    DecodeDiagnostics* diagnostics = nullptr) {
  if (diagnostics) diagnostics->stage = "heif.metadata";
  ContextPtr context{heif_context_alloc()};
  if (!context) return awj::compat::unexpected{"无法创建 libheif context。"};
  heif_context_set_max_decoding_threads(context.get(), 0);
  auto primary = open_primary_image(context.get(), bytes, source_name);
  if (!primary) {
    const std::string_view raw{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    if (raw.find("urn:com:apple:photo:2020:aux:hdrgainmap") != raw.npos || raw.find("tmap") != raw.npos)
      return awj::compat::unexpected{"Gain Map: ISO 21496-1: " + primary.error()};
    return awj::compat::unexpected{primary.error()};
  }
  capture_handle_diagnostics(diagnostics, primary->get());
  const auto decode_with_budget = [&](const heif_image_handle* handle, bool copy, bool ignore, bool native,
                                      bool preserve_icc, bool auxiliary = false) {
    heif_image_tiling tiling{};
    tiling.version = 1;
    const auto layout = heif_image_handle_get_image_tiling(handle, 0, &tiling);
    const auto plan = layout.code == heif_error_Ok
        ? plan_heif_decode_threads(decode_threads, static_cast<std::uint64_t>(tiling.num_columns) * tiling.num_rows,
            static_cast<std::uint64_t>(tiling.tile_width) * tiling.tile_height)
        : plan_heif_decode_threads(decode_threads, 1, 0);
    heif_context_set_max_decoding_threads(context.get(), plan.tile_parallelism > 1 ? plan.tile_parallelism : 0);
    if (diagnostics && !auxiliary) {
      diagnostics->heif_tile_workers = plan.tile_parallelism;
      diagnostics->heif_codec_threads = plan.codec_threads;
    }
    const auto started = std::chrono::steady_clock::now();
    auto decoded = decode_handle(handle, source_name, plan.codec_threads, copy, stop_token, ignore, native, preserve_icc);
    if (diagnostics) {
      auto& elapsed = auxiliary ? diagnostics->timing.auxiliary_decode_seconds : diagnostics->timing.base_decode_seconds;
      elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
    return decoded;
  };
  auto selected = select_gain_map(context.get(), primary->get());
  if (!selected) return awj::compat::unexpected{selected.error()};
  const auto base_id = selected->base_id;
  const auto tmap_id = selected->tmap_id;
  const auto auxiliary_id = selected->auxiliary_id;
  if (!auxiliary_id) {
    if (diagnostics) diagnostics->stage = "heif.base-decode";
    auto decoded = decode_with_budget(primary->get(), copy_metadata_payloads, false, native_pixels, false);
    if (decoded && diagnostics) diagnostics->stage = "complete";
    return decoded;
  }
  if (diagnostics) {
    diagnostics->decoder_id = "libheif-iso21496-1-gain-map-enhanced";
    diagnostics->stage = "heif.gain-map.base-decode";
    diagnostics->source_info->source_has_gain_map = true;
    diagnostics->source_info->has_hdr_metadata = diagnostics->source_info->has_hdr_metadata ||
        selected->metadata->baseHdrHeadroom.n != 0 || selected->metadata->alternateHdrHeadroom.n != 0;
  }
  heif_image_handle* raw_auxiliary = nullptr;
  auto aux_error = heif_context_get_image_handle(context.get(), *auxiliary_id, &raw_auxiliary);
  if (aux_error.code != heif_error_Ok || !raw_auxiliary) return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: 无法取得 gain map 图像。"};
  HandlePtr auxiliary{raw_auxiliary};
  // Gain Map 主图必须使用原始 ICC/NCLX 信令，不能补入解码库生成的默认 RGB 标签。
  auto base = decode_with_budget(primary->get(), true, true, false, true);
  if (!base) return awj::compat::unexpected{"Gain Map: ISO 21496-1: " + base.error()};
  if (diagnostics) diagnostics->stage = "heif.gain-map.map-decode";
  auto gain = decode_with_budget(auxiliary.get(), false, true, false, false, true);
  if (!gain) return awj::compat::unexpected{"Gain Map: ISO 21496-1: " + gain.error()};
  if (diagnostics) diagnostics->stage = "heif.gain-map.transforms";
  const auto auxiliary_transforms = heif_item_get_transformation_properties(context.get(), *auxiliary_id, nullptr, 0);
  if (auxiliary_transforms < 0) return awj::compat::unexpected{"Gain Map: ISO 21496-1 HEIF: auxiliary 变换属性无效。"};
  // Auxiliary-local transforms define its own displayed sample canvas. Otherwise
  // the base transform is shared and applied once after reconstruction.
  if (auxiliary_transforms) {
    if (auto transformed = apply_transformations(context.get(), base_id, base->image, false); !transformed)
      return awj::compat::unexpected{transformed.error()};
    if (auto transformed = apply_transformations(context.get(), *auxiliary_id, gain->image, false); !transformed)
      return awj::compat::unexpected{transformed.error()};
  }
  make_alpha_straight(base->image);
  if (diagnostics) diagnostics->stage = "heif.gain-map.render";
  const int thread_budget = std::clamp(decode_threads, 1, encoding_defaults::max_automatic_thread_budget);
  const ResourcePlan render_resources{.encoder_threads_per_file = thread_budget, .global_thread_budget = thread_budget};
  auto composed = render_iso_gain_map(std::move(base->image), gain->image, *selected->metadata,
      gain_map_mode, stop_token, render_resources, diagnostics ? &diagnostics->timing : nullptr);
  if (!composed) return awj::compat::unexpected{composed.error()};
  if (diagnostics) {
    diagnostics->icc_normalized_to_cicp = diagnostics->source_has_icc;
    diagnostics->stage = "heif.gain-map.transforms";
  }
  const auto transform = [&](ImageBuffer& image) -> awj::compat::expected<void, std::string> {
    if (!auxiliary_transforms) {
      if (auto transformed = apply_transformations(context.get(), base_id, image, false); !transformed)
        return awj::compat::unexpected{transformed.error()};
    }
    if (tmap_id) {
      if (auto transformed = apply_transformations(context.get(), *tmap_id, image, false); !transformed)
        return awj::compat::unexpected{transformed.error()};
    }
    if (auto transformed = gain_map_detail::apply_exif_orientation(image); !transformed) return transformed;
    if (!copy_metadata_payloads) image.metadata.clear();
    return {};
  };
  if (auto transformed = transform(composed->image); !transformed) return awj::compat::unexpected{transformed.error()};
  if (composed->gain_map_alternative) {
    if (auto transformed = transform(composed->gain_map_alternative->image); !transformed)
      return awj::compat::unexpected{transformed.error()};
  }
  composed->decoder_id = "libheif-iso21496-1-gain-map-enhanced";
  if (diagnostics) diagnostics->stage = "complete";
  return composed;
}

}  // namespace heif_detail

class HeifImageDecoder final : public ImageDecoder {
 public:
  explicit HeifImageDecoder(int decode_threads = 1, awj::compat::stop_token stop_token = {})
      : decode_threads_{decode_threads}, stop_token_{stop_token} {}

  [[nodiscard]] std::string_view id() const noexcept override { return "libheif-libde265"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".heic", L".heif"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  awj::compat::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_bytes(path, "HEIC");
      if (!bytes) {
        return awj::compat::unexpected{bytes.error()};
      }
      heif_detail::ContextPtr context{heif_context_alloc()};
      if (!context) {
        return awj::compat::unexpected{"无法创建 libheif context。"};
      }
      auto handle = heif_detail::open_primary_image(
          context.get(), *bytes, display_path_for_user(path));
      if (!handle) {
        const std::string_view raw{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
        if (raw.find("urn:com:apple:photo:2020:aux:hdrgainmap") != raw.npos || raw.find("tmap") != raw.npos)
          return awj::compat::unexpected{"Gain Map: ISO 21496-1: " + handle.error()};
        return awj::compat::unexpected{handle.error()};
      }
      auto selected = heif_detail::select_gain_map(context.get(), handle->get());
      if (!selected) return awj::compat::unexpected{selected.error()};
      auto dimensions = heif_detail::gain_map_dimensions(context.get(), handle->get(), *selected);
      if (!dimensions) return dimensions;
      const auto source = heif_detail::source_info_from_handle(handle->get());
      dimensions->source_bit_depth = source.bit_depth;
      dimensions->source_has_alpha = heif_image_handle_has_alpha_channel(handle->get()) != 0;
      dimensions->source_icc_bytes = heif_image_handle_get_raw_color_profile_size(handle->get());
      const bool has_icc = dimensions->source_icc_bytes != 0;
      dimensions->source_dynamic_range = source.has_hdr_metadata ? ImageDynamicRangeHint::hdr :
          !has_icc && source.transfer_characteristics && *source.transfer_characteristics != 0 &&
                  *source.transfer_characteristics != 2 ? ImageDynamicRangeHint::sdr : ImageDynamicRangeHint::unknown;
      dimensions->source_has_gain_map = selected->auxiliary_id.has_value();
      if (selected->auxiliary_id) {
        dimensions->source_bit_depth = 16;
        const auto& metadata = *selected->metadata;
        dimensions->source_dynamic_range = metadata.baseHdrHeadroom.n || metadata.alternateHdrHeadroom.n ||
                source.has_hdr_metadata ? ImageDynamicRangeHint::hdr : ImageDynamicRangeHint::sdr;
        heif_image_handle* raw_map = nullptr;
        const auto error = heif_context_get_image_handle(context.get(), *selected->auxiliary_id, &raw_map);
        heif_detail::HandlePtr map{raw_map};
        if (error.code != heif_error_Ok || !map) return awj::compat::unexpected{"Gain Map: ISO 21496-1: 无法读取辅助图尺寸。"};
        const int map_width = heif_image_handle_get_ispe_width(map.get());
        const int map_height = heif_image_handle_get_ispe_height(map.get());
        if (map_width <= 0 || map_height <= 0) return awj::compat::unexpected{"Gain Map: ISO 21496-1: 辅助图尺寸无效。"};
        dimensions->gain_map_width = static_cast<std::uint32_t>(map_width);
        dimensions->gain_map_height = static_cast<std::uint32_t>(map_height);
      }
      dimensions->opaque_sdr8_yuv420 = !dimensions->source_has_gain_map && !has_icc &&
          dimensions->source_dynamic_range == ImageDynamicRangeHint::sdr && source.bit_depth == 8 &&
          source.pixel_format == PixelFormat::yuv420 && !dimensions->source_has_alpha;
      const int coded_width = heif_image_handle_get_ispe_width(handle->get());
      const int coded_height = heif_image_handle_get_ispe_height(handle->get());
      if (coded_width <= 0 || coded_height <= 0) return awj::compat::unexpected{"HEIC 编码尺寸无效。"};
      const auto pixels = static_cast<std::uint64_t>(coded_width) * coded_height;
      const auto bytes_per_pixel = dimensions->source_bit_depth > 8 ? 8u : 4u;
      dimensions->decoded_storage_bytes = pixels > UINT64_MAX / bytes_per_pixel ? UINT64_MAX : pixels * bytes_per_pixel;
      return dimensions;
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"HEIC 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"HEIC 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"HEIC 尺寸探测文件系统访问失败。"};
    }
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode_memory(
      std::span<const std::byte> bytes, std::string_view source_name,
      DecodeOptions options = {}) const override {
    try {
      heif_detail::begin_decode_diagnostics(options.diagnostics, "heif.metadata");
      return heif_detail::decode_bytes(bytes, source_name, decode_threads_,
                                       options.copy_metadata_payloads.value_or(false), stop_token_, options.native_pixels,
                                       options.gain_map_mode, options.diagnostics);
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"HEIC 解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"HEIC 解码数据超过运行时限制。"};
    }
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    return decode(path, {});
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(const fs::path& path, DecodeOptions options) const override {
    try {
      heif_detail::begin_decode_diagnostics(options.diagnostics, "heif.read");
      auto bytes = decoder_common::read_file_bytes(path, "HEIC");
      if (!bytes) {
        return awj::compat::unexpected{bytes.error()};
      }
      return heif_detail::decode_bytes(*bytes, display_path_for_user(path), decode_threads_,
                                       options.copy_metadata_payloads.value_or(true),
                                       stop_token_, options.native_pixels, options.gain_map_mode, options.diagnostics);
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"HEIC 解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"HEIC 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"HEIC 解码文件系统访问失败。"};
    }
  }

 private:
  int decode_threads_{1};
  awj::compat::stop_token stop_token_{};
};

}  // namespace awj
