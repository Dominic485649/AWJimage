module;

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <libheif/heif.h>
#include <avif/avif.h>
#include <libheif/heif_items.h>
#include <libheif/heif_properties.h>
#include "heif_gain_map_adapter.h"

export module awj.heif_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.gain_map;
import awj.hdr_tonemap;
import awj.image;
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
  return std::format("{} (code={}, subcode={})", error.message,
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

void fill_missing_nclx_from_decoded(ImageSourceInfo& source, const heif_image* image) noexcept {
  heif_color_profile_nclx* raw_nclx = nullptr;
  const auto error = heif_image_get_nclx_color_profile(image, &raw_nclx);
  NclxPtr nclx{raw_nclx};
  if (error.code != heif_error_Ok || !nclx) {
    return;
  }
  if (!source.color_primaries) {
    source.color_primaries = cicp_value(static_cast<int>(nclx->color_primaries));
  }
  if (!source.transfer_characteristics) {
    source.transfer_characteristics =
        cicp_value(static_cast<int>(nclx->transfer_characteristics));
  }
  if (!source.matrix_coefficients) {
    source.matrix_coefficients = cicp_value(static_cast<int>(nclx->matrix_coefficients));
  }
  if (!source.color_range) {
    source.color_range = nclx->full_range_flag ? 1 : 0;
  }
  if (source.color_metadata_source.empty()) {
    source.color_metadata_source = "heif-nclx";
  }
  source.has_hdr_metadata = source.has_hdr_metadata || hdr_cicp(source);
}

std::expected<void, std::string> append_metadata(ImageBuffer& image, MetadataKind kind,
                                                 std::span<const std::byte> bytes,
                                                 std::size_t& total_bytes,
                                                 std::string_view label) {
  if (bytes.empty()) {
    return {};
  }
  if (bytes.size() > encoding_defaults::codec_metadata_max_bytes ||
      total_bytes > encoding_defaults::codec_metadata_max_bytes - bytes.size()) {
    return std::unexpected{std::format("HEIC {} metadata 超过运行时上限。", label)};
  }
  auto owned = decoder_common::make_byte_buffer(bytes.size(), "HEIC metadata");
  if (!owned) {
    return std::unexpected{owned.error()};
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

std::expected<void, std::string> copy_metadata(ImageBuffer& image,
                                               const heif_image_handle* handle, bool normalize_orientation = true) {
  std::size_t total_bytes = 0;

  const auto icc_size = heif_image_handle_get_raw_color_profile_size(handle);
  if (icc_size > 0) {
    if (icc_size > encoding_defaults::codec_metadata_max_bytes) {
      return std::unexpected{"HEIC ICC metadata 超过运行时上限。"};
    }
    auto icc = decoder_common::make_byte_buffer(icc_size, "HEIC ICC metadata");
    if (!icc) {
      return std::unexpected{icc.error()};
    }
    const auto error = heif_image_handle_get_raw_color_profile(handle, icc->data());
    if (error.code != heif_error_Ok) {
      return std::unexpected{std::format("HEIC ICC 读取失败: {}", error_text(error))};
    }
    auto appended = append_metadata(image, MetadataKind::icc, *icc, total_bytes, "ICC");
    if (!appended) {
      return std::unexpected{appended.error()};
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
    return std::unexpected{"HEIC metadata block 数量超过运行时上限。"};
  }
  std::vector<heif_item_id> ids(static_cast<std::size_t>(metadata_count));
  const int id_count = heif_image_handle_get_list_of_metadata_block_IDs(
      handle, nullptr, ids.data(), metadata_count);
  if (id_count < 0 || id_count > metadata_count) {
    return std::unexpected{"HEIC metadata block 索引无效。"};
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
      return std::unexpected{"HEIC metadata payload 超过运行时上限。"};
    }
    auto payload = decoder_common::make_byte_buffer(size, "HEIC metadata payload");
    if (!payload) {
      return std::unexpected{payload.error()};
    }
    const auto error = heif_image_handle_get_metadata(handle, id, payload->data());
    if (error.code != heif_error_Ok) {
      return std::unexpected{std::format("HEIC metadata 读取失败: {}", error_text(error))};
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
        return std::unexpected{appended.error()};
      }
      copied_exif = true;
    } else {
      auto appended = append_metadata(image, MetadataKind::xmp, *payload, total_bytes, "XMP");
      if (!appended) {
        return std::unexpected{appended.error()};
      }
      copied_xmp = true;
    }
  }
  return {};
}

std::expected<HandlePtr, std::string> open_primary_image(heif_context* context,
                                                        std::span<const std::byte> bytes,
                                                        std::string_view source_name) {
  const auto read_error = heif_context_read_from_memory_without_copy(
      context, bytes.data(), bytes.size(), nullptr);
  if (read_error.code != heif_error_Ok) {
    return std::unexpected{
        std::format("HEIC 容器读取失败: {}: {}", source_name, error_text(read_error))};
  }
  heif_image_handle* raw_handle = nullptr;
  const auto handle_error = heif_context_get_primary_image_handle(context, &raw_handle);
  if (handle_error.code != heif_error_Ok || raw_handle == nullptr) {
    return std::unexpected{
        std::format("HEIC 主图读取失败: {}: {}", source_name, error_text(handle_error))};
  }
  return HandlePtr{raw_handle};
}

std::expected<ImageDecodeResult, std::string> decode_handle(const heif_image_handle* handle,
                                                           std::string_view source_name,
                                                           int decode_threads,
                                                           bool copy_metadata_payloads,
                                                           std::stop_token stop_token = {}, bool ignore_transformations = false) {
  if (stop_token.stop_requested()) {
    return std::unexpected{"任务已取消。"};
  }
  const int threads = std::clamp(decode_threads, 1,
                                 encoding_defaults::max_automatic_thread_budget);
  // libde265 owns the per-image budget; parallel tiles must not multiply it.
  auto source_info = source_info_from_handle(handle);
  const bool has_alpha = heif_image_handle_has_alpha_channel(handle) != 0;
  const bool premultiplied = has_alpha &&
                             heif_image_handle_is_premultiplied_alpha(handle) != 0;
  const int source_bit_depth = source_info.bit_depth > 0 ? source_info.bit_depth : 8;
  const int output_bit_depth = source_bit_depth > 8 ? 16 : 8;

  DecodingOptionsPtr options{heif_decoding_options_alloc()};
  if (!options) {
    return std::unexpected{"无法创建 libheif decoding options。"};
  }
  options->ignore_transformations = ignore_transformations ? 1 : 0;
  options->convert_hdr_to_8bit = 0;
  options->strict_decoding = 1;
  options->num_codec_threads = threads;
  options->progress_user_data = &stop_token;
  options->cancel_decoding = [](void* user_data) -> int {
    return static_cast<const std::stop_token*>(user_data)->stop_requested() ? 1 : 0;
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
                                               heif_colorspace_RGB, rgba_chroma,
                                               options.get());
  if (stop_token.stop_requested()) {
    if (raw_image != nullptr) heif_image_release(raw_image);
    return std::unexpected{"任务已取消。"};
  }
  ImagePtr decoded{raw_image};
  if (decode_error.code != heif_error_Ok || !decoded) {
    return std::unexpected{
        std::format("HEIC 解码失败: {}: {}", source_name, error_text(decode_error))};
  }
  fill_missing_nclx_from_decoded(source_info, decoded.get());

  const int width_i = heif_image_get_width(decoded.get(), heif_channel_interleaved);
  const int height_i = heif_image_get_height(decoded.get(), heif_channel_interleaved);
  if (width_i <= 0 || height_i <= 0) {
    return std::unexpected{std::format("HEIC 解码尺寸无效: {}", source_name)};
  }
  auto dimensions = decoder_common::make_image_dimensions_checked(
      static_cast<std::uint32_t>(width_i), static_cast<std::uint32_t>(height_i),
      "HEIC decoder");
  if (!dimensions) {
    return std::unexpected{dimensions.error()};
  }

  const std::size_t bytes_per_sample = output_bit_depth > 8 ? 2 : 1;
  const auto packed_stride = decoder_common::checked_rgba_stride(
      dimensions->width, "HEIC decoder", bytes_per_sample);
  if (!packed_stride) {
    return std::unexpected{packed_stride.error()};
  }
  const auto byte_count = decoder_common::checked_image_bytes(
      *packed_stride, dimensions->height, "HEIC decoder");
  if (!byte_count) {
    return std::unexpected{byte_count.error()};
  }
  auto rgba = decoder_common::make_byte_buffer(*byte_count, "HEIC decoder");
  if (!rgba) {
    return std::unexpected{rgba.error()};
  }
  std::size_t source_stride = 0;
  const auto* source_pixels = heif_image_get_plane_readonly2(
      decoded.get(), heif_channel_interleaved, &source_stride);
  if (source_pixels == nullptr || source_stride < *packed_stride) {
    return std::unexpected{std::format("HEIC RGBA plane 无效: {}", source_name)};
  }
  const int decoded_bits = heif_image_get_bits_per_pixel_range(
      decoded.get(), heif_channel_interleaved);
  if (decoded_bits <= 0 || decoded_bits > output_bit_depth) {
    return std::unexpected{"HEIC RGBA sample precision is invalid."};
  }
  for (std::size_t row = 0; row < dimensions->height; ++row) {
    if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
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
    return std::unexpected{image.error()};
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
      return std::unexpected{copied.error()};
    }
  }
  return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libheif-libde265"};
}

std::expected<void, std::string> parse_iso_metadata(std::span<const std::byte> bytes, avifGainMap& map) {
  // ToneMapImage + GainMapMetadata, ISO 21496-1 C.2.2; same syntax as libavif read.c.
  if (bytes.size() < 22 || bytes[0] != std::byte{0} || bytes[1] != std::byte{0} ||
      bytes[2] != std::byte{0} || bytes[3] != std::byte{0} || bytes[4] != std::byte{0})
    return std::unexpected{"Gain Map: HEIF ISO metadata version 不支持或截断。"};
  const auto flags = std::to_integer<unsigned>(bytes[5]);
  const auto channels = flags & 128 ? 3u : 1u;
  if ((flags & 63) || bytes.size() != 22 + channels * 40)
    return std::unexpected{"Gain Map: HEIF ISO metadata 长度/保留位无效。"};
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
  return {};
}

std::expected<void, std::string> apply_transformations(heif_context* context, heif_item_id id, ImageBuffer& image,
    bool apply_exif = true) {
  const auto count = heif_item_get_transformation_properties(context, id, nullptr, 0);
  if (count < 0 || count > 16) return std::unexpected{"Gain Map: HEIF 方向属性数量无效。"};
  std::vector<heif_property_id> properties(count);
  if (heif_item_get_transformation_properties(context, id, properties.data(), count) != count)
    return std::unexpected{"Gain Map: HEIF 方向属性索引无效。"};
  bool oriented = false;
  for (auto property : properties) {
    const auto type = heif_item_get_property_type(context, id, property);
    std::expected<void, std::string> changed;
    if (type == heif_item_property_type_transform_rotation) {
      const auto angle = heif_item_get_property_transform_rotation_ccw(context, id, property);
      if (angle < 0 || angle % 90) return std::unexpected{"Gain Map: HEIF rotation 无效。"};
      changed = gain_map_detail::transform_rgba(image, angle / 90);
      oriented = true;
    } else if (type == heif_item_property_type_transform_mirror) {
      const auto axis = heif_item_get_property_transform_mirror(context, id, property);
      if (axis == heif_transform_mirror_direction_invalid) return std::unexpected{"Gain Map: HEIF mirror 无效。"};
      changed = gain_map_detail::transform_rgba(image, 0, static_cast<int>(axis));
      oriented = true;
    } else if (type == heif_item_property_type_transform_crop) {
      int left{}, top{}, right{}, bottom{};
      heif_item_get_property_transform_crop_borders(context, id, property,
          static_cast<int>(image.width), static_cast<int>(image.height), &left, &top, &right, &bottom);
      if (left < 0 || top < 0 || right < 0 || bottom < 0 ||
          static_cast<std::size_t>(left) + right >= image.width || static_cast<std::size_t>(top) + bottom >= image.height)
        return std::unexpected{"Gain Map: HEIF crop 无效。"};
      changed = gain_map_detail::crop_rgba(image, left, top, image.width - left - right, image.height - top - bottom);
    } else return std::unexpected{"Gain Map: HEIF 使用不支持的变换属性。"};
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

std::expected<ImageDecodeResult, std::string> decode_bytes(std::span<const std::byte> bytes,
    std::string_view source_name, int decode_threads, bool copy_metadata_payloads, std::stop_token stop_token = {}) {
  ContextPtr context{heif_context_alloc()};
  if (!context) return std::unexpected{"无法创建 libheif context。"};
  heif_context_set_max_decoding_threads(context.get(), 0);
  auto primary = open_primary_image(context.get(), bytes, source_name);
  if (!primary) {
    const std::string_view raw{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    if (raw.find("urn:com:apple:photo:2020:aux:hdrgainmap") != raw.npos || raw.find("tmap") != raw.npos)
      return std::unexpected{"Gain Map: " + primary.error()};
    return std::unexpected{primary.error()};
  }
  heif_item_id base_id{};
  if (heif_context_get_primary_image_ID(context.get(), &base_id).code != heif_error_Ok)
    return std::unexpected{"Gain Map: HEIF primary ID 读取失败。"};
  std::optional<heif_item_id> tmap_id, auxiliary_id;
  bool apple = false;
  const int item_count = heif_context_get_number_of_items(context.get());
  if (item_count < 0 || item_count > 16384) return std::unexpected{"HEIF item 数量超出上限。"};
  std::vector<heif_item_id> items(item_count);
  if (heif_context_get_list_of_item_IDs(context.get(), items.data(), item_count) != item_count)
    return std::unexpected{"HEIF item 索引无效。"};
  for (auto id : items) {
    if (heif_item_get_item_type(context.get(), id) != heif_fourcc('t','m','a','p')) continue;
    bool found = false;
    for (int index = 0; index < 16; ++index) {
      uint32_t type{}; heif_item_id* references = nullptr;
      const auto count = heif_context_get_item_references(context.get(), id, index, &type, &references);
      struct References { heif_context* ctx; heif_item_id** data; ~References() { heif_release_item_references(ctx, data); } } guard{context.get(), &references};
      if (!count) break;
      if (type != heif_fourcc('d','i','m','g')) continue;
      if (count != 2 || !references || references[0] == references[1])
        return std::unexpected{"Gain Map: HEIF tmap 必须恰好引用主图与一个增益图。"};
      if (references[0] != base_id) continue;
      if (tmap_id || found) return std::unexpected{"Gain Map: HEIF primary 存在多个 ISO gain map。"};
      tmap_id = id; auxiliary_id = references[1]; found = true;
    }
  }
  const int filter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA | LIBHEIF_AUX_IMAGE_FILTER_OMIT_DEPTH;
  const int aux_count = heif_image_handle_get_number_of_auxiliary_images(primary->get(), filter);
  if (aux_count < 0 || aux_count > 4096) return std::unexpected{"Gain Map: HEIF auxiliary 数量无效。"};
  std::vector<heif_item_id> auxiliary_ids(aux_count);
  if (heif_image_handle_get_list_of_auxiliary_image_IDs(primary->get(), filter, auxiliary_ids.data(), aux_count) != aux_count)
    return std::unexpected{"Gain Map: HEIF auxiliary 索引无效。"};
  for (auto id : auxiliary_ids) {
    heif_image_handle* raw_handle = nullptr;
    if (heif_image_handle_get_auxiliary_image_handle(primary->get(), id, &raw_handle).code != heif_error_Ok || !raw_handle)
      return std::unexpected{"Gain Map: HEIF auxiliary handle 无效。"};
    HandlePtr handle{raw_handle};
    const char* type = nullptr;
    const auto error = heif_image_handle_get_auxiliary_type(handle.get(), &type);
    const std::string urn = type ? type : "";
    if (type) heif_image_handle_release_auxiliary_type(handle.get(), &type);
    if (error.code != heif_error_Ok) return std::unexpected{"Gain Map: HEIF auxiliary 类型无效。"};
    if (urn == "urn:com:apple:photo:2020:aux:hdrgainmap") {
      if (tmap_id && auxiliary_id == id) continue; // Prefer ISO for the same map.
      if (auxiliary_id) return std::unexpected{"Gain Map: HEIF primary 存在多个 gain map。"};
      auxiliary_id = id; apple = true;
    } else if (urn.find("21496") != urn.npos && (!tmap_id || auxiliary_id != id)) {
      return std::unexpected{"Gain Map: HEIF ISO auxiliary 缺少支持的 tmap 元数据。"};
    }
  }
  if (!auxiliary_id) return decode_handle(primary->get(), source_name, decode_threads, copy_metadata_payloads, stop_token);
  heif_image_handle* raw_auxiliary = nullptr;
  auto aux_error = heif_context_get_image_handle(context.get(), *auxiliary_id, &raw_auxiliary);
  if (aux_error.code != heif_error_Ok || !raw_auxiliary) return std::unexpected{"Gain Map: HEIF 无法取得 gain map 图像。"};
  HandlePtr auxiliary{raw_auxiliary};
  auto base = decode_handle(primary->get(), source_name, decode_threads, true, stop_token, true);
  if (!base) return std::unexpected{"Gain Map: " + base.error()};
  auto gain = decode_handle(auxiliary.get(), source_name, decode_threads, apple, stop_token, true);
  if (!gain) return std::unexpected{"Gain Map: " + gain.error()};
  const auto auxiliary_transforms = heif_item_get_transformation_properties(context.get(), *auxiliary_id, nullptr, 0);
  if (auxiliary_transforms < 0) return std::unexpected{"Gain Map: HEIF auxiliary 变换属性无效。"};
  // Auxiliary-local transforms define its own displayed sample canvas. Otherwise
  // the base transform is shared and applied once after reconstruction.
  if (auxiliary_transforms) {
    if (auto transformed = apply_transformations(context.get(), base_id, base->image, false); !transformed)
      return std::unexpected{transformed.error()};
    if (auto transformed = apply_transformations(context.get(), *auxiliary_id, gain->image, false); !transformed)
      return std::unexpected{transformed.error()};
  }
  make_alpha_straight(base->image);
  std::expected<ImageBuffer, std::string> composed = std::unexpected{"Gain Map: 未选择合成方式。"};
  if (apple) composed = compose_apple_gain_map(base->image, gain->image, stop_token);
  else {
    uint8_t* raw_data = nullptr; std::size_t size{};
    auto data_error = heif_item_get_item_data(context.get(), *tmap_id, nullptr, &raw_data, &size);
    struct Data { heif_context* ctx; uint8_t** data; ~Data() { heif_release_item_data(ctx, data); } } guard{context.get(), &raw_data};
    if (data_error.code != heif_error_Ok || !raw_data || size > 142)
      return std::unexpected{"Gain Map: HEIF ISO metadata 缺失或过大。"};
    using GainMap = std::unique_ptr<avifGainMap, decltype(&avifGainMapDestroy)>;
    GainMap metadata{avifGainMapCreate(), &avifGainMapDestroy};
    if (!metadata) return std::unexpected{"Gain Map: 无法分配 ISO metadata。"};
    auto parsed = parse_iso_metadata({reinterpret_cast<const std::byte*>(raw_data), size}, *metadata);
    if (!parsed) return std::unexpected{parsed.error()};
    int primaries{}, transfer{}, matrix{};
    const auto color = awjHeifItemNclx(context.get(), *tmap_id, &primaries, &transfer, &matrix);
    if (color < 0) return std::unexpected{"Gain Map: HEIF ISO alternate 色彩 profile 暂不支持。"};
    if (color) {
      metadata->altColorPrimaries = static_cast<avifColorPrimaries>(primaries);
      metadata->altTransferCharacteristics = static_cast<avifTransferCharacteristics>(transfer);
      metadata->altMatrixCoefficients = static_cast<avifMatrixCoefficients>(matrix);
    }
    composed = compose_iso_gain_map(base->image, gain->image, *metadata);
  }
  if (!composed) return std::unexpected{composed.error()};
  if (!auxiliary_transforms) {
    if (auto transformed = apply_transformations(context.get(), base_id, *composed, false); !transformed)
      return std::unexpected{transformed.error()};
  }
  if (tmap_id) {
    if (auto transformed = apply_transformations(context.get(), *tmap_id, *composed, false); !transformed)
      return std::unexpected{transformed.error()};
  }
  if (auto transformed = gain_map_detail::apply_exif_orientation(*composed); !transformed)
    return std::unexpected{transformed.error()};
  if (!copy_metadata_payloads) composed->metadata.clear();
  return ImageDecodeResult{.image = std::move(*composed),
      .decoder_id = apple ? "libheif-apple-gain-map-enhanced" : "libheif-iso-gain-map-enhanced"};
}

}  // namespace heif_detail

class HeifImageDecoder final : public ImageDecoder {
 public:
  explicit HeifImageDecoder(int decode_threads = 1, std::stop_token stop_token = {})
      : decode_threads_{decode_threads}, stop_token_{stop_token} {}

  [[nodiscard]] std::string_view id() const noexcept override { return "libheif-libde265"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".heic", L".heif"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_bytes(path, "HEIC");
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      heif_detail::ContextPtr context{heif_context_alloc()};
      if (!context) {
        return std::unexpected{"无法创建 libheif context。"};
      }
      auto handle = heif_detail::open_primary_image(
          context.get(), *bytes, display_path_for_user(path));
      if (!handle) {
        return std::unexpected{handle.error()};
      }
      const int width = heif_image_handle_get_width(handle->get());
      const int height = heif_image_handle_get_height(handle->get());
      if (width <= 0 || height <= 0) {
        return std::unexpected{"HEIC 图像尺寸无效。"};
      }
      return decoder_common::make_image_dimensions_checked(
          static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), "HEIC");
    } catch (const std::bad_alloc&) {
      return std::unexpected{"HEIC 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"HEIC 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"HEIC 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode_memory(
      std::span<const std::byte> bytes, std::string_view source_name,
      DecodeOptions options = {}) const override {
    try {
      return heif_detail::decode_bytes(bytes, source_name, decode_threads_,
                                       options.copy_metadata_payloads.value_or(false), stop_token_);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"HEIC 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"HEIC 解码数据超过运行时限制。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_bytes(path, "HEIC");
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      return heif_detail::decode_bytes(*bytes, display_path_for_user(path), decode_threads_, true,
                                       stop_token_);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"HEIC 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"HEIC 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"HEIC 解码文件系统访问失败。"};
    }
  }

 private:
  int decode_threads_{1};
  std::stop_token stop_token_{};
};

}  // namespace awj
