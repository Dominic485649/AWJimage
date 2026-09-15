module;

#include <algorithm>
#include <bit>
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

export module awj.heif_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
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
                                               const heif_image_handle* handle) {
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
      normalize_exif_orientation(std::span<std::byte>{payload->data() + start, payload->size() - start});
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

std::expected<ImageDecodeResult, std::string> decode_bytes(std::span<const std::byte> bytes,
                                                           std::string_view source_name,
                                                           int decode_threads,
                                                           bool copy_metadata_payloads,
                                                           std::stop_token stop_token = {}) {
  if (stop_token.stop_requested()) {
    return std::unexpected{"任务已取消。"};
  }
  ContextPtr context{heif_context_alloc()};
  if (!context) {
    return std::unexpected{"无法创建 libheif context。"};
  }
  const int threads = std::clamp(decode_threads, 1,
                                 encoding_defaults::max_automatic_thread_budget);
  // libde265 owns the per-image budget; parallel tiles must not multiply it.
  heif_context_set_max_decoding_threads(context.get(), 0);

  auto handle = open_primary_image(context.get(), bytes, source_name);
  if (!handle) {
    return std::unexpected{handle.error()};
  }
  auto source_info = source_info_from_handle(handle->get());
  const bool has_alpha = heif_image_handle_has_alpha_channel(handle->get()) != 0;
  const bool premultiplied = has_alpha &&
                             heif_image_handle_is_premultiplied_alpha(handle->get()) != 0;
  const int source_bit_depth = source_info.bit_depth > 0 ? source_info.bit_depth : 8;
  const int output_bit_depth = source_bit_depth > 8 ? 16 : 8;

  DecodingOptionsPtr options{heif_decoding_options_alloc()};
  if (!options) {
    return std::unexpected{"无法创建 libheif decoding options。"};
  }
  options->ignore_transformations = 0;
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
  const auto decode_error = heif_decode_image(handle->get(), &raw_image,
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
    if (auto copied = copy_metadata(*image, handle->get()); !copied) {
      return std::unexpected{copied.error()};
    }
  }
  return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libheif-libde265"};
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
