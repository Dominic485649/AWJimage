module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <png.h>

#ifdef _MSC_VER
#pragma warning(disable : 4611)
#endif

export module awj.png_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace png_detail {

int quantization_bits(int storage_depth, int quality) noexcept {
  if (quality >= 100) return storage_depth;
  const int minimum = storage_depth == 16 ? 10 : 4;
  return minimum + (std::clamp(quality, 1, 99) - 1) * (storage_depth - minimum) / 99;
}

std::uint16_t quantize_sample(std::uint16_t value, int storage_depth, int bits) noexcept {
  const std::uint64_t full = (1u << storage_depth) - 1;
  const std::uint64_t levels = (1u << bits) - 1;
  const auto reduced = (value * levels + full / 2) / full;
  // PNG 12.4: nearest rounding, rescale to the full range, retain both endpoints.
  return static_cast<std::uint16_t>((reduced * full + levels / 2) / levels);
}

std::uint32_t read_be_u32(std::span<const std::byte> bytes,
                          std::size_t offset) noexcept {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

bool chunk_type_is(std::span<const std::byte> header, std::string_view type) noexcept;

void append_be_u32(std::vector<std::byte>& out, std::uint32_t value) {
  out.push_back(std::byte{static_cast<unsigned char>((value >> 24) & 0xffu)});
  out.push_back(std::byte{static_cast<unsigned char>((value >> 16) & 0xffu)});
  out.push_back(std::byte{static_cast<unsigned char>((value >> 8) & 0xffu)});
  out.push_back(std::byte{static_cast<unsigned char>(value & 0xffu)});
}

std::uint32_t png_crc_update(std::uint32_t crc, std::byte value) noexcept {
  crc ^= std::to_integer<std::uint8_t>(value);
  for (int k = 0; k < 8; ++k) {
    crc = (crc & 1u) != 0u ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
  }
  return crc;
}

std::uint32_t png_chunk_crc(std::string_view type,
                            std::span<const std::byte> payload) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (const char ch : type) {
    crc = png_crc_update(crc, std::byte{static_cast<unsigned char>(ch)});
  }
  for (const auto value : payload) {
    crc = png_crc_update(crc, value);
  }
  return crc ^ 0xffffffffu;
}

void append_png_chunk(std::vector<std::byte>& out, std::string_view type,
                      std::span<const std::byte> payload) {
  append_be_u32(out, static_cast<std::uint32_t>(payload.size()));
  for (const char ch : type) {
    out.push_back(std::byte{static_cast<unsigned char>(ch)});
  }
  out.insert(out.end(), payload.begin(), payload.end());
  append_be_u32(out, png_chunk_crc(type, payload));
}

struct PngColorChunks {
  std::optional<int> color_primaries{};
  std::optional<int> transfer_characteristics{};
  std::optional<int> matrix_coefficients{};
  std::optional<int> color_range{};
  std::optional<HdrContentLightMetadata> content_light{};
  bool has_cicp{};
};

std::expected<PngColorChunks, std::string> inspect_color_chunks(
    std::span<const std::byte> bytes, std::string_view source_name) {
  PngColorChunks chunks{};
  if (bytes.size() < 8 ||
      png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()), 0, 8) != 0) {
    return std::unexpected{std::format("PNG 签名无效: {}", source_name)};
  }
  std::size_t offset = 8;
  while (offset + 12 <= bytes.size()) {
    const auto chunk_size = read_be_u32(bytes, offset);
    if (chunk_size > bytes.size() - offset - 12) {
      return std::unexpected{std::format("PNG 文件截断: {}", source_name)};
    }
    const auto header = bytes.subspan(offset, 8);
    const auto data_offset = offset + 8;
    if (chunk_type_is(header, "cICP") && chunk_size == 4) {
      chunks.color_primaries = std::to_integer<int>(bytes[data_offset]);
      chunks.transfer_characteristics = std::to_integer<int>(bytes[data_offset + 1]);
      chunks.matrix_coefficients = std::to_integer<int>(bytes[data_offset + 2]);
      chunks.color_range = std::to_integer<int>(bytes[data_offset + 3]) != 0 ? 1 : 0;
      chunks.has_cicp = true;
    } else if (chunk_type_is(header, "cLLI") && chunk_size == 8) {
      const auto max_cll = read_be_u32(bytes, data_offset) / 10000u;
      const auto max_fall = read_be_u32(bytes, data_offset + 4) / 10000u;
      chunks.content_light = HdrContentLightMetadata{
          .max_cll = static_cast<std::uint16_t>(std::min<std::uint32_t>(max_cll, 65535u)),
          .max_pall = static_cast<std::uint16_t>(std::min<std::uint32_t>(max_fall, 65535u))};
    }
    if (chunk_type_is(header, "IDAT")) {
      break;
    }
    offset += 12 + chunk_size;
  }
  return chunks;
}

std::expected<void, std::string> insert_png_chunks_after_ihdr(
    std::vector<std::byte>& png_bytes, std::vector<std::byte> chunks) {
  if (chunks.empty()) {
    return {};
  }
  if (png_bytes.size() < 33 ||
      png_sig_cmp(reinterpret_cast<png_const_bytep>(png_bytes.data()), 0, 8) != 0) {
    return std::unexpected{"PNG encoder 输出签名无效，无法写入 cICP/cLLI。"};
  }
  const auto ihdr_size = read_be_u32(png_bytes, 8);
  if (ihdr_size != 13 || !chunk_type_is(std::span<const std::byte>{png_bytes.data(), png_bytes.size()}.subspan(8, 8), "IHDR")) {
    return std::unexpected{"PNG encoder 输出缺少 IHDR，无法写入 cICP/cLLI。"};
  }
  const std::size_t insert_at = 8 + 12 + ihdr_size;
  if (insert_at > png_bytes.size()) {
    return std::unexpected{"PNG encoder 输出截断，无法写入 cICP/cLLI。"};
  }
  png_bytes.insert(png_bytes.begin() + static_cast<std::ptrdiff_t>(insert_at),
                   chunks.begin(), chunks.end());
  return {};
}

std::expected<void, std::string> add_png_hdr_chunks(
    std::vector<std::byte>& png_bytes,
    const ImageBuffer& image,
    const NativeEncodeSettings& settings) {
  if (settings.strip_metadata || settings.applied_icc == "kept") {
    return {};
  }
  const auto primaries = settings.applied_color_primaries
                             ? settings.applied_color_primaries
                             : (image.source_info ? image.source_info->color_primaries : std::optional<int>{});
  const auto transfer = settings.applied_transfer_characteristics
                            ? settings.applied_transfer_characteristics
                            : (image.source_info ? image.source_info->transfer_characteristics : std::optional<int>{});
  if (!primaries || !transfer) {
    return {};
  }
  const int range = settings.applied_color_range.value_or(
      image.source_info && image.source_info->color_range ? *image.source_info->color_range : 1);
  if (*primaries < 0 || *primaries > 255 || *transfer < 0 || *transfer > 255) {
    return std::unexpected{"PNG cICP 色彩字段超过 1-byte 范围。"};
  }
  std::vector<std::byte> chunks;
  const int matrix = 0;
  if (matrix < 0 || matrix > 255) {
    return std::unexpected{"PNG cICP matrix 字段超过 1-byte 范围。"};
  }
  std::array<std::byte, 4> cicp{
      std::byte{static_cast<unsigned char>(*primaries)},
      std::byte{static_cast<unsigned char>(*transfer)},
      std::byte{static_cast<unsigned char>(matrix)},
      std::byte{static_cast<unsigned char>(range != 0 ? 1 : 0)}};
  append_png_chunk(chunks, "cICP", cicp);
  if (image.source_info && image.source_info->content_light) {
    std::vector<std::byte> clli;
    clli.reserve(8);
    append_be_u32(clli, static_cast<std::uint32_t>(image.source_info->content_light->max_cll) * 10000u);
    append_be_u32(clli, static_cast<std::uint32_t>(image.source_info->content_light->max_pall) * 10000u);
    append_png_chunk(chunks, "cLLI", clli);
  }
  return insert_png_chunks_after_ihdr(png_bytes, std::move(chunks));
}

bool chunk_type_is(std::span<const std::byte> header, std::string_view type) noexcept {
  return header.size() >= 8 &&
         header[4] == std::byte{static_cast<unsigned char>(type[0])} &&
         header[5] == std::byte{static_cast<unsigned char>(type[1])} &&
         header[6] == std::byte{static_cast<unsigned char>(type[2])} &&
         header[7] == std::byte{static_cast<unsigned char>(type[3])};
}

std::expected<bool, std::string> contains_animation_control_chunk(
    std::span<const std::byte> bytes,
    std::string_view source_name) {
  if (bytes.size() < 8 ||
      png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()), 0, 8) != 0) {
    return std::unexpected{std::format("PNG 签名无效: {}", source_name)};
  }

  std::size_t offset = 8;
  while (offset + 12 <= bytes.size()) {
    const auto chunk_size = read_be_u32(bytes, offset);
    if (chunk_size > bytes.size() - offset - 12) {
      return std::unexpected{std::format("PNG 文件截断: {}", source_name)};
    }
    const auto header = bytes.subspan(offset, 8);
    if (chunk_type_is(header, "acTL")) {
      return true;
    }
    if (chunk_type_is(header, "IDAT")) {
      return false;
    }
    offset += 12 + chunk_size;
  }
  return std::unexpected{std::format("PNG 文件截断: {}", source_name)};
}

std::expected<bool, std::string> file_contains_animation_control_chunk(const fs::path& path) {
  std::error_code ec;
  const auto file_size = fs::file_size(path, ec);
  if (ec) {
    return std::unexpected{std::format("读取 PNG 文件大小失败: {}；系统错误：{}",
                                       display_path_for_user(path), ec.message())};
  }
  if (file_size > static_cast<std::uintmax_t>(encoding_defaults::effective_max_input_file_bytes())) {
    return std::unexpected{std::format("PNG 文件超过当前输入上限: {}",
                                       display_path_for_user(path))};
  }

  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{std::format("无法读取 PNG 文件: {}", display_path_for_user(path))};
  }

  std::array<std::byte, 8> signature{};
  input.read(reinterpret_cast<char*>(signature.data()), static_cast<std::streamsize>(signature.size()));
  if (input.gcount() != static_cast<std::streamsize>(signature.size()) || input.bad() ||
      png_sig_cmp(reinterpret_cast<png_const_bytep>(signature.data()), 0, 8) != 0) {
    return std::unexpected{std::format("PNG 签名无效: {}", display_path_for_user(path))};
  }

  while (true) {
    std::array<std::byte, 8> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size()) || input.bad()) {
      return std::unexpected{std::format("PNG 文件截断: {}", display_path_for_user(path))};
    }
    if (chunk_type_is(header, "acTL")) {
      return true;
    }
    if (chunk_type_is(header, "IDAT")) {
      return false;
    }
    const auto chunk_size = read_be_u32(header, 0);
    input.seekg(static_cast<std::streamoff>(chunk_size) + 4, std::ios::cur);
    if (!input) {
      return std::unexpected{std::format("PNG 文件截断: {}", display_path_for_user(path))};
    }
  }
}

struct ReadState {
  const std::byte* data{};
  std::size_t size{};
  std::size_t offset{};
};

struct DecodeContext {
  std::vector<std::byte> rgba{};
  std::vector<png_bytep> rows{};
  std::vector<std::byte> icc_profile{};
  std::vector<std::byte> exif_metadata{};
  std::vector<std::byte> xmp_metadata{};
  std::optional<SignificantBits> significant_bits{};
};

struct WriteState {
  std::vector<std::byte> bytes{};
  std::vector<std::byte> row{};
};

struct PngReadDeleter {
  void operator()(png_structp value) const noexcept {
    if (value != nullptr) {
      png_destroy_read_struct(&value, nullptr, nullptr);
    }
  }
};

struct PngWriteDeleter {
  void operator()(png_structp value) const noexcept {
    if (value != nullptr) {
      png_destroy_write_struct(&value, nullptr);
    }
  }
};

struct PngInfoDeleter {
  png_structp png{};
  void operator()(png_infop value) const noexcept {
    if (png != nullptr && value != nullptr) {
      png_destroy_info_struct(png, &value);
    }
  }
};

using PngReadPtr = std::unique_ptr<png_struct, PngReadDeleter>;
using PngWritePtr = std::unique_ptr<png_struct, PngWriteDeleter>;
using PngInfoPtr = std::unique_ptr<png_info, PngInfoDeleter>;

void read_callback(png_structp png, png_bytep out, png_size_t count) {
  auto* state = static_cast<ReadState*>(png_get_io_ptr(png));
  if (state == nullptr || count > state->size - state->offset) {
    png_error(png, "PNG input is truncated");
    return;
  }
  std::ranges::copy_n(reinterpret_cast<const png_byte*>(state->data + state->offset),
                      count, out);
  state->offset += count;
}

void write_callback(png_structp png, png_bytep data, png_size_t count) {
  auto* state = static_cast<WriteState*>(png_get_io_ptr(png));
  if (state == nullptr) {
    png_error(png, "PNG output state is missing");
    return;
  }
  if (count > encoding_defaults::effective_max_input_file_bytes() - state->bytes.size()) {
    png_error(png, "PNG output exceeds runtime limit");
    return;
  }
  const auto* begin = reinterpret_cast<const std::byte*>(data);
  state->bytes.insert(state->bytes.end(), begin, begin + count);
}

void flush_callback(png_structp) {}

std::expected<const ImagePlane*, std::string> rgba_plane(const ImageBuffer& image) {
  if (image.pixel_format != PixelFormat::rgba ||
      (image.bit_depth != 8 && image.bit_depth != 16) || image.planes.empty() ||
      image.sample_representation != SampleRepresentation::unorm) {
    return std::unexpected{"PNG encoder 当前需要 RGBA ImageBuffer。"};
  }
  const auto& plane = image.planes.front();
  const auto bytes_per_sample = image.bit_depth > 8 ? std::size_t{2} : std::size_t{1};
  const auto expected_stride = decoder_common::checked_rgba_stride(image.width, "PNG encoder", bytes_per_sample);
  if (!expected_stride) {
    return std::unexpected{expected_stride.error()};
  }
  if (plane.stride != *expected_stride) {
    return std::unexpected{"PNG encoder 当前需要紧凑排列的 RGBA buffer。"};
  }
  const auto expected_bytes = decoder_common::checked_image_bytes(plane.stride, image.height, "PNG encoder");
  if (!expected_bytes) {
    return std::unexpected{expected_bytes.error()};
  }
  if (plane.bytes.size() < *expected_bytes) {
    return std::unexpected{"PNG encoder 输入 RGBA buffer 尺寸无效。"};
  }
  return &plane;
}

const MetadataBlock* first_metadata(const ImageBuffer& image, MetadataKind kind) noexcept {
  for (const auto& block : image.metadata) {
    if (block.kind == kind && !block.bytes.empty()) {
      return &block;
    }
  }
  return nullptr;
}

PixelFormat source_pixel_format_for_png(int color_type) noexcept {
  switch (color_type) {
    case PNG_COLOR_TYPE_GRAY:
      return PixelFormat::gray;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
      return PixelFormat::rgba;
    case PNG_COLOR_TYPE_RGB:
      return PixelFormat::rgb;
    case PNG_COLOR_TYPE_RGB_ALPHA:
      return PixelFormat::rgba;
    case PNG_COLOR_TYPE_PALETTE:
      return PixelFormat::rgb;
    default:
      return PixelFormat::unknown;
  }
}

std::expected<std::vector<std::byte>, std::string> copy_metadata_payload(
    const void* data,
    std::size_t size,
    std::string_view context) {
  if (data == nullptr || size == 0) {
    return std::vector<std::byte>{};
  }
  if (size > encoding_defaults::codec_metadata_max_bytes) {
    return std::unexpected{std::format("{} 超过 64 MiB 上限。", context)};
  }
  auto bytes = decoder_common::make_byte_buffer(size, context);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  std::ranges::copy_n(static_cast<const std::byte*>(data), size, bytes->begin());
  return std::move(*bytes);
}

void clamp_chunk_limits(png_structp png) noexcept {
#ifdef PNG_SET_USER_LIMITS_SUPPORTED
  if (png == nullptr) {
    return;
  }
  const auto current_cache_limit = png_get_chunk_cache_max(png);
  if (current_cache_limit == 0 ||
      current_cache_limit > encoding_defaults::png_max_cached_metadata_chunks) {
    png_set_chunk_cache_max(
        png,
        static_cast<png_uint_32>(
            encoding_defaults::png_max_cached_metadata_chunks));
  }
  constexpr auto max_chunk_malloc_bytes =
      static_cast<png_alloc_size_t>(
          encoding_defaults::codec_metadata_max_bytes);
  const auto current_malloc_limit = png_get_chunk_malloc_max(png);
  if (current_malloc_limit == 0 || current_malloc_limit > max_chunk_malloc_bytes) {
    png_set_chunk_malloc_max(png, max_chunk_malloc_bytes);
  }
#else
  (void)png;
#endif
}

std::expected<std::vector<std::byte>, std::string> copy_icc_profile(png_structp png,
                                                                     png_infop info) {
  png_charp name = nullptr;
  int compression_type = 0;
  png_bytep profile = nullptr;
  png_uint_32 profile_size = 0;
  if (png_get_iCCP(png, info, &name, &compression_type, &profile, &profile_size) == 0 ||
      profile == nullptr || profile_size == 0) {
    return std::vector<std::byte>{};
  }
  return copy_metadata_payload(profile, profile_size, "PNG ICC profile");
}

std::expected<std::vector<std::byte>, std::string> copy_exif_metadata(png_structp png,
                                                                      png_infop info) {
  png_uint_32 exif_size = 0;
  png_bytep exif = nullptr;
  if (png_get_eXIf_1(png, info, &exif_size, &exif) == 0 || exif == nullptr || exif_size == 0) {
    return std::vector<std::byte>{};
  }
  return copy_metadata_payload(exif, exif_size, "PNG Exif metadata");
}

std::expected<std::vector<std::byte>, std::string> copy_exif_metadata(png_structp png,
                                                                      png_infop info,
                                                                      png_infop end_info) {
  auto bytes = copy_exif_metadata(png, info);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  if (!bytes->empty() || end_info == nullptr) {
    return std::move(*bytes);
  }
  return copy_exif_metadata(png, end_info);
}

std::size_t text_payload_size(const png_text& text) noexcept {
  switch (text.compression) {
    case PNG_ITXT_COMPRESSION_NONE:
    case PNG_ITXT_COMPRESSION_zTXt:
      return text.itxt_length;
    case PNG_TEXT_COMPRESSION_NONE:
    case PNG_TEXT_COMPRESSION_zTXt:
      return text.text_length;
    default:
      return 0;
  }
}

std::expected<std::vector<std::byte>, std::string> copy_xmp_metadata(png_structp png,
                                                                     png_infop info) {
  static constexpr std::string_view xmp_keyword = "XML:com.adobe.xmp";

  png_textp text = nullptr;
  int text_count = 0;
  if (png_get_text(png, info, &text, &text_count) <= 0 || text == nullptr) {
    return std::vector<std::byte>{};
  }
  for (int index = 0; index < text_count; ++index) {
    const auto& entry = text[index];
    if (entry.key == nullptr || std::string_view{entry.key} != xmp_keyword || entry.text == nullptr) {
      continue;
    }
    const auto payload_size = text_payload_size(entry);
    if (payload_size == 0) {
      continue;
    }
    return copy_metadata_payload(entry.text, payload_size, "PNG XMP metadata");
  }
  return std::vector<std::byte>{};
}

std::expected<std::vector<std::byte>, std::string> copy_xmp_metadata(png_structp png,
                                                                     png_infop info,
                                                                     png_infop end_info) {
  auto bytes = copy_xmp_metadata(png, info);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  if (!bytes->empty() || end_info == nullptr) {
    return std::move(*bytes);
  }
  return copy_xmp_metadata(png, end_info);
}

}  // namespace png_detail

class PngImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libpng"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".png"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_prefix(path, 24, "PNG");
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      if (bytes->size() < 24 ||
          png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes->data()), 0, 8) != 0 ||
          png_detail::read_be_u32(*bytes, 8) != 13 ||
          (*bytes)[12] != std::byte{'I'} || (*bytes)[13] != std::byte{'H'} ||
          (*bytes)[14] != std::byte{'D'} || (*bytes)[15] != std::byte{'R'}) {
        return std::unexpected{std::format("PNG 文件头无效: {}", display_path_for_user(path))};
      }
      return decoder_common::make_image_dimensions_checked(png_detail::read_be_u32(*bytes, 16),
                                                           png_detail::read_be_u32(*bytes, 20),
                                                           "PNG");
    } catch (const std::bad_alloc&) {
      return std::unexpected{"PNG 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"PNG 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"PNG 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_bytes(path, "PNG");
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      if (bytes->size() < 8 || png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes->data()), 0, 8) != 0) {
        return std::unexpected{std::format("PNG 签名无效: {}", display_path_for_user(path))};
      }
      std::unique_ptr<png_detail::DecodeContext> context;
      try {
        context = std::make_unique<png_detail::DecodeContext>();
      } catch (const std::bad_alloc&) {
        return std::unexpected{"PNG decoder 内存不足。"};
      }

      png_detail::PngReadPtr png{png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr)};
      if (!png) {
        return std::unexpected{"创建 PNG decoder 失败。"};
      }
      png_detail::clamp_chunk_limits(png.get());
      png_detail::PngInfoPtr info{png_create_info_struct(png.get()), png_detail::PngInfoDeleter{.png = png.get()}};
      if (!info) {
        return std::unexpected{"创建 PNG info 失败。"};
      }
      png_detail::PngInfoPtr end_info{png_create_info_struct(png.get()),
                                       png_detail::PngInfoDeleter{.png = png.get()}};
      if (!end_info) {
        return std::unexpected{"创建 PNG end info 失败。"};
      }

      png_detail::ReadState state{.data = bytes->data(), .size = bytes->size()};
      png_set_read_fn(png.get(), &state, png_detail::read_callback);

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
      if (setjmp(png_jmpbuf(png.get())) != 0) {
        return std::unexpected{std::format("PNG 解码失败: {}", display_path_for_user(path))};
      }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

      png_read_info(png.get(), info.get());
      png_uint_32 width = 0;
      png_uint_32 height = 0;
      int bit_depth = 0;
      int color_type = 0;
      png_get_IHDR(png.get(), info.get(), &width, &height, &bit_depth, &color_type,
                   nullptr, nullptr, nullptr);
      if (width == 0 || height == 0) {
        return std::unexpected{std::format("PNG 尺寸无效: {}", display_path_for_user(path))};
      }

      const bool source_has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 ||
                                    png_get_valid(png.get(), info.get(), PNG_INFO_tRNS) != 0;

      png_color_8p bits = nullptr;
      if (png_get_sBIT(png.get(), info.get(), &bits) && bits) {
        const bool gray = (color_type & PNG_COLOR_MASK_COLOR) == 0;
        context->significant_bits = SignificantBits{
            gray ? bits->gray : bits->red, gray ? bits->gray : bits->green,
            gray ? bits->gray : bits->blue,
            (color_type & PNG_COLOR_MASK_ALPHA) ? bits->alpha : (bit_depth == 16 ? 16 : 8)};
      }

      if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png.get());
      }
      if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png.get());
      }
      if (png_get_valid(png.get(), info.get(), PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png.get());
      }
      if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png.get());
      }
      if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 && !png_get_valid(png.get(), info.get(), PNG_INFO_tRNS)) {
        png_set_filler(png.get(), bit_depth == 16 ? 0xffff : 0xff, PNG_FILLER_AFTER);
      }
      png_set_interlace_handling(png.get());
      png_read_update_info(png.get(), info.get());
      const int output_bit_depth = bit_depth == 16 ? 16 : 8;
      if (output_bit_depth == 16 && std::endian::native == std::endian::little) {
        png_set_swap(png.get());
      }

      std::size_t stride = 0;
      {
        const auto checked_stride = decoder_common::checked_rgba_stride(
            width, "PNG decoder", output_bit_depth > 8 ? 2 : 1);
        if (!checked_stride) {
          return std::unexpected{checked_stride.error()};
        }
        stride = *checked_stride;
      }
      if (png_get_rowbytes(png.get(), info.get()) != stride) {
        return std::unexpected{std::format("PNG 转换后行字节数无效: {}", display_path_for_user(path))};
      }
      {
        const auto byte_count = decoder_common::checked_image_bytes(stride, height, "PNG decoder");
        if (!byte_count) {
          return std::unexpected{byte_count.error()};
        }
        auto rgba = decoder_common::make_byte_buffer(*byte_count, "PNG decoder");
        if (!rgba) {
          return std::unexpected{rgba.error()};
        }
        context->rgba = std::move(*rgba);
      }
      const auto row_pointer_bytes = static_cast<std::uint64_t>(height) * sizeof(png_bytep);
      if (row_pointer_bytes > encoding_defaults::effective_max_input_file_bytes()) {
        return std::unexpected{"PNG decoder 行指针 buffer 超过当前运行时上限。"};
      }
      try {
        context->rows.resize(height);
      } catch (const std::bad_alloc&) {
        return std::unexpected{"PNG decoder 行指针内存不足。"};
      } catch (const std::length_error&) {
        return std::unexpected{"PNG decoder 行指针数量超过运行时限制。"};
      }
      for (png_uint_32 y = 0; y < height; ++y) {
        context->rows[y] = reinterpret_cast<png_bytep>(context->rgba.data() + y * stride);
      }
      png_read_image(png.get(), context->rows.data());
      png_read_end(png.get(), end_info.get());

      {
        auto icc_profile = png_detail::copy_icc_profile(png.get(), info.get());
        if (!icc_profile) {
          return std::unexpected{icc_profile.error()};
        }
        context->icc_profile = std::move(*icc_profile);
      }
      {
        auto exif_metadata = png_detail::copy_exif_metadata(png.get(), info.get(), end_info.get());
        if (!exif_metadata) {
          return std::unexpected{exif_metadata.error()};
        }
        context->exif_metadata = std::move(*exif_metadata);
      }
      {
        auto xmp_metadata = png_detail::copy_xmp_metadata(png.get(), info.get(), end_info.get());
        if (!xmp_metadata) {
          return std::unexpected{xmp_metadata.error()};
        }
        context->xmp_metadata = std::move(*xmp_metadata);
      }

      auto color_chunks = png_detail::inspect_color_chunks(*bytes, display_path_for_user(path));
      if (!color_chunks) {
        return std::unexpected{color_chunks.error()};
      }
      ImageSourceInfo source_info{.pixel_format = png_detail::source_pixel_format_for_png(color_type),
                                  .bit_depth = bit_depth};
      if (color_chunks->has_cicp) {
        source_info.color_primaries = color_chunks->color_primaries;
        source_info.transfer_characteristics = color_chunks->transfer_characteristics;
        source_info.matrix_coefficients = color_chunks->matrix_coefficients;
        source_info.color_range = color_chunks->color_range;
        source_info.has_hdr_metadata = source_info.transfer_characteristics == 16 ||
                                       source_info.transfer_characteristics == 18;
        source_info.color_metadata_source = "png-cicp";
      }
      if (color_chunks->content_light) {
        source_info.content_light = color_chunks->content_light;
        source_info.has_hdr_metadata = true;
        if (source_info.color_metadata_source.empty()) {
          source_info.color_metadata_source = "png-clli";
        }
      }

      auto image = decoder_common::make_rgba_image(
          width, height, std::move(context->rgba),
          source_has_alpha ? AlphaMode::straight : AlphaMode::none, "PNG decoder",
          source_info, output_bit_depth);
      if (!image) {
        return std::unexpected{image.error()};
      }
      image->significant_bits = context->significant_bits;
      if (!context->icc_profile.empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::icc,
                                                .bytes = std::move(context->icc_profile)});
      }
      if (!context->exif_metadata.empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::exif,
                                                .bytes = std::move(context->exif_metadata)});
      }
      if (!context->xmp_metadata.empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::xmp,
                                                .bytes = std::move(context->xmp_metadata)});
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libpng"};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"PNG 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"PNG 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"PNG 解码文件系统访问失败。"};
    }
  }
};

class PngImageEncoder final : public ImageEncoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libpng"; }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::png,
                             .features = CodecFeature::lossless | CodecFeature::alpha,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 0,
                             .max_speed = 10,
                             .bit_depths = {8, 16}};
  }

  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings,
      std::stop_token stop_token = {}) const override {
    try {
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      if (settings.quality < 1 || settings.quality > 100) {
        return std::unexpected{"PNG quality 必须在 1–100 范围内。"};
      }
      auto plane = png_detail::rgba_plane(image);
      if (!plane) {
        return std::unexpected{plane.error()};
      }
      if (image.width > std::numeric_limits<png_uint_32>::max() ||
          image.height > std::numeric_limits<png_uint_32>::max()) {
        return std::unexpected{"PNG encoder 输入尺寸超过 libpng API 限制。"};
      }

      const int bit_depth = image.bit_depth;
      const auto source_bits = image.significant_bits.value_or(
          SignificantBits{bit_depth, bit_depth, bit_depth, bit_depth});
      const std::array<int, 4> depths{source_bits.red, source_bits.green, source_bits.blue, source_bits.alpha};
      if (std::ranges::any_of(depths, [bit_depth](int value) { return value < 1 || value > bit_depth; })) {
        return std::unexpected{"PNG 有效位数必须在 1 到存储位深之间。"};
      }
      const int target_bits = png_detail::quantization_bits(bit_depth, settings.quality);
      const std::array<int, 3> rgb_bits{std::min(source_bits.red, target_bits),
          std::min(source_bits.green, target_bits), std::min(source_bits.blue, target_bits)};
      const bool quantize = rgb_bits[0] < source_bits.red || rgb_bits[1] < source_bits.green ||
                            rgb_bits[2] < source_bits.blue;
      auto state = std::make_unique<png_detail::WriteState>();
      state->bytes.reserve(std::min((*plane)->bytes.size(), std::size_t{1u << 20}));
      if (quantize) state->row.resize((*plane)->stride);
      png_detail::PngWritePtr png{png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr)};
      if (!png) {
        return std::unexpected{"创建 PNG encoder 失败。"};
      }
      png_detail::PngInfoPtr info{png_create_info_struct(png.get()),
                                  png_detail::PngInfoDeleter{.png = png.get()}};
      if (!info) {
        return std::unexpected{"创建 PNG info 失败。"};
      }
      if (setjmp(png_jmpbuf(png.get())) != 0) {
        return std::unexpected{"PNG 编码失败。"};
      }

      png_set_write_fn(png.get(), state.get(), png_detail::write_callback,
                       png_detail::flush_callback);
      png_set_IHDR(png.get(), info.get(), static_cast<png_uint_32>(image.width),
                   static_cast<png_uint_32>(image.height), bit_depth,
                   PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                   PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
      if (quantize || image.significant_bits) {
        png_color_8 bits{};
        bits.red = static_cast<png_byte>(rgb_bits[0]);
        bits.green = static_cast<png_byte>(rgb_bits[1]);
        bits.blue = static_cast<png_byte>(rgb_bits[2]);
        bits.alpha = static_cast<png_byte>(source_bits.alpha);
        png_set_sBIT(png.get(), info.get(), &bits);
      }
      if (!settings.strip_metadata && settings.applied_icc == "kept") {
        if (const auto* icc = png_detail::first_metadata(image, MetadataKind::icc)) {
          png_set_iCCP(png.get(), info.get(), "ICC profile", PNG_COMPRESSION_TYPE_BASE,
                       reinterpret_cast<png_const_bytep>(icc->bytes.data()),
                       static_cast<png_uint_32>(icc->bytes.size()));
        }
      }
      png_write_info(png.get(), info.get());
      if (bit_depth == 16 && std::endian::native == std::endian::little) {
        png_set_swap(png.get());
      }

      for (std::size_t y = 0; y < image.height; ++y) {
        if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
        const auto* row = (*plane)->bytes.data() + y * (*plane)->stride;
        if (quantize) {
          std::memcpy(state->row.data(), row, state->row.size());
          const auto sample_bytes = static_cast<std::size_t>(bit_depth / 8);
          for (std::size_t x = 0; x < image.width; ++x) {
            for (std::size_t c = 0; c < 3; ++c) {
              if (rgb_bits[c] >= depths[c]) continue;
              auto* sample = state->row.data() + (x * 4 + c) * sample_bytes;
              std::uint16_t value{};
              if (bit_depth == 16) std::memcpy(&value, sample, sizeof(value));
              else value = std::to_integer<std::uint8_t>(*sample);
              value = png_detail::quantize_sample(value, bit_depth, rgb_bits[c]);
              if (bit_depth == 16) std::memcpy(sample, &value, sizeof(value));
              else *sample = static_cast<std::byte>(value);
            }
          }
          row = state->row.data();
        }
        png_write_row(png.get(), reinterpret_cast<png_const_bytep>(row));
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      png_write_end(png.get(), nullptr);
      if (auto hdr_chunks = png_detail::add_png_hdr_chunks(state->bytes, image, settings);
          !hdr_chunks) {
        return std::unexpected{hdr_chunks.error()};
      }
      if (state->bytes.empty()) {
        return std::unexpected{"PNG encoder 输出失败。"};
      }

      auto diagnostics = diagnostics_from_settings(settings);
      diagnostics.encoder_id = "libpng";
      diagnostics.integration_mode = "libpng";
      diagnostics.encoder_threads = 1;
      diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;
      return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(state->bytes),
                                                        .codec_name = "libpng"},
                                .diagnostics = std::move(diagnostics),
                                .final_quality = settings.quality,
                                .lossless = !quantize,
                                .search_attempt_count = 1};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"PNG 编码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"PNG 编码数据超过运行时限制。"};
    }
  }
};

}  // namespace awj
