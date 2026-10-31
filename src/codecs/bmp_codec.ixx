module;
#include "../compat/stdlib_compat.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module awj.bmp_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace bmp_detail {

constexpr std::uint32_t bi_rgb = 0;
constexpr std::uint32_t bi_rle8 = 1;
constexpr std::uint32_t bi_rle4 = 2;
constexpr std::uint32_t bi_bitfields = 3;
constexpr std::uint32_t bi_alphabitfields = 6;

struct PaletteEntry {
  std::uint8_t r{};
  std::uint8_t g{};
  std::uint8_t b{};
  std::uint8_t a{255};
};

struct BitMask {
  std::uint32_t mask{};
  int shift{};
  int bits{};
};

struct Header {
  std::size_t pixel_offset{};
  std::size_t palette_offset{};
  std::size_t palette_count{};
  std::uint32_t dib_header_size{};
  std::uint32_t compression{};
  std::uint32_t red_mask{};
  std::uint32_t green_mask{};
  std::uint32_t blue_mask{};
  std::uint32_t alpha_mask{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint16_t bits_per_pixel{};
  bool top_down{};
  bool has_alpha_mask{};
  bool uses_bitmap_file_header{};
};

bool has_range(const std::vector<std::byte>& bytes, std::size_t offset, std::size_t count) {
  return offset <= bytes.size() && count <= bytes.size() - offset;
}

std::uint8_t u8(const std::vector<std::byte>& bytes, std::size_t offset) {
  return static_cast<std::uint8_t>(bytes[offset]);
}

std::uint16_t le16(const std::vector<std::byte>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(u8(bytes, offset)) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(u8(bytes, offset + 1)) << 8);
}

std::uint32_t le32(const std::vector<std::byte>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(u8(bytes, offset)) |
         (static_cast<std::uint32_t>(u8(bytes, offset + 1)) << 8) |
         (static_cast<std::uint32_t>(u8(bytes, offset + 2)) << 16) |
         (static_cast<std::uint32_t>(u8(bytes, offset + 3)) << 24);
}

std::int32_t le_i32(const std::vector<std::byte>& bytes, std::size_t offset) {
  return static_cast<std::int32_t>(le32(bytes, offset));
}

bool is_bitmap_file(const std::vector<std::byte>& bytes) {
  return bytes.size() >= 14 && bytes[0] == std::byte{'B'} && bytes[1] == std::byte{'M'};
}

awj::compat::expected<std::uint32_t, std::string> positive_dimension(std::int32_t value,
                                                             std::string_view label,
                                                             std::string_view source_name) {
  if (value <= 0) {
    return awj::compat::unexpected{awj::compat::format("BMP {} 无效: {}", label, source_name)};
  }
  return static_cast<std::uint32_t>(value);
}

awj::compat::expected<std::uint32_t, std::string> absolute_height(std::int32_t value,
                                                          std::string_view source_name) {
  if (value == 0 || value == std::numeric_limits<std::int32_t>::min()) {
    return awj::compat::unexpected{awj::compat::format("BMP 高度无效: {}", source_name)};
  }
  return static_cast<std::uint32_t>(value < 0 ? -value : value);
}

awj::compat::expected<Header, std::string> parse_header(const std::vector<std::byte>& bytes,
                                                std::string_view source_name) {
  const bool has_file_header = is_bitmap_file(bytes);
  const std::size_t dib_offset = has_file_header ? 14 : 0;
  if (!has_range(bytes, dib_offset, 40)) {
    return awj::compat::unexpected{awj::compat::format("BMP/DIB 文件头过短: {}", source_name)};
  }

  Header header{};
  header.uses_bitmap_file_header = has_file_header;
  if (has_file_header) {
    const auto declared_size = le32(bytes, 2);
    if (declared_size != 0 && declared_size > bytes.size()) {
      return awj::compat::unexpected{awj::compat::format("BMP 文件大小字段无效: {}", source_name)};
    }
    header.pixel_offset = le32(bytes, 10);
  }

  header.dib_header_size = le32(bytes, dib_offset);
  if (header.dib_header_size < 40 || header.dib_header_size > bytes.size() - dib_offset) {
    return awj::compat::unexpected{awj::compat::format("暂不支持此 BMP DIB header: {}", source_name)};
  }

  const auto width = positive_dimension(le_i32(bytes, dib_offset + 4), "宽度", source_name);
  if (!width) {
    return awj::compat::unexpected{width.error()};
  }
  const auto height = absolute_height(le_i32(bytes, dib_offset + 8), source_name);
  if (!height) {
    return awj::compat::unexpected{height.error()};
  }
  header.width = *width;
  header.height = *height;
  header.top_down = le_i32(bytes, dib_offset + 8) < 0;

  const auto planes = le16(bytes, dib_offset + 12);
  if (planes != 1) {
    return awj::compat::unexpected{awj::compat::format("BMP planes 字段无效: {}", source_name)};
  }
  header.bits_per_pixel = le16(bytes, dib_offset + 14);
  header.compression = le32(bytes, dib_offset + 16);
  const auto colors_used = le32(bytes, dib_offset + 32);

  switch (header.bits_per_pixel) {
    case 1:
    case 4:
    case 8:
    case 16:
    case 24:
    case 32:
      break;
    default:
      return awj::compat::unexpected{awj::compat::format("暂不支持 {} bpp BMP: {}",
                                         header.bits_per_pixel,
                                         source_name)};
  }

  if (header.compression == bi_rle8 && header.bits_per_pixel != 8) {
    return awj::compat::unexpected{awj::compat::format("BMP RLE8 只能用于 8 bpp: {}", source_name)};
  }
  if (header.compression == bi_rle4 && header.bits_per_pixel != 4) {
    return awj::compat::unexpected{awj::compat::format("BMP RLE4 只能用于 4 bpp: {}", source_name)};
  }
  if (header.top_down && (header.compression == bi_rle8 || header.compression == bi_rle4)) {
    return awj::compat::unexpected{awj::compat::format("暂不支持 top-down RLE BMP: {}", source_name)};
  }
  if (header.compression != bi_rgb && header.compression != bi_rle8 &&
      header.compression != bi_rle4 && header.compression != bi_bitfields &&
      header.compression != bi_alphabitfields) {
    return awj::compat::unexpected{awj::compat::format("暂不支持此 BMP 压缩方式 {}: {}",
                                       header.compression,
                                       source_name)};
  }
  if ((header.compression == bi_bitfields || header.compression == bi_alphabitfields) &&
      header.bits_per_pixel != 16 && header.bits_per_pixel != 32) {
    return awj::compat::unexpected{awj::compat::format("BMP bitfields 只能用于 16/32 bpp: {}", source_name)};
  }

  std::size_t palette_offset = dib_offset + header.dib_header_size;
  if (header.compression == bi_bitfields || header.compression == bi_alphabitfields) {
    if (header.dib_header_size == 40) {
      const auto mask_words = header.compression == bi_alphabitfields ? std::size_t{4}
                                                                      : std::size_t{3};
      if (!has_range(bytes, palette_offset, mask_words * 4)) {
        return awj::compat::unexpected{awj::compat::format("BMP bitfields mask 不完整: {}", source_name)};
      }
      header.red_mask = le32(bytes, palette_offset);
      header.green_mask = le32(bytes, palette_offset + 4);
      header.blue_mask = le32(bytes, palette_offset + 8);
      if (mask_words == 4) {
        header.alpha_mask = le32(bytes, palette_offset + 12);
        header.has_alpha_mask = header.alpha_mask != 0;
      }
      palette_offset += mask_words * 4;
    } else {
      if (!has_range(bytes, dib_offset + 40, 16)) {
        return awj::compat::unexpected{awj::compat::format("BMP V4/V5 bitfields mask 不完整: {}", source_name)};
      }
      header.red_mask = le32(bytes, dib_offset + 40);
      header.green_mask = le32(bytes, dib_offset + 44);
      header.blue_mask = le32(bytes, dib_offset + 48);
      header.alpha_mask = le32(bytes, dib_offset + 52);
      header.has_alpha_mask = header.alpha_mask != 0;
    }
  } else if (header.bits_per_pixel == 16) {
    header.red_mask = 0x00007C00u;
    header.green_mask = 0x000003E0u;
    header.blue_mask = 0x0000001Fu;
  } else if (header.bits_per_pixel == 32) {
    header.red_mask = 0x00FF0000u;
    header.green_mask = 0x0000FF00u;
    header.blue_mask = 0x000000FFu;
    header.alpha_mask = 0xFF000000u;
  }

  if (header.bits_per_pixel <= 8) {
    const auto default_count = 1u << header.bits_per_pixel;
    header.palette_count = colors_used == 0 ? default_count : colors_used;
    if (header.palette_count == 0 || header.palette_count > default_count) {
      return awj::compat::unexpected{awj::compat::format("BMP palette 大小无效: {}", source_name)};
    }
    header.palette_offset = palette_offset;
    if (!has_range(bytes, header.palette_offset, header.palette_count * 4)) {
      return awj::compat::unexpected{awj::compat::format("BMP palette 不完整: {}", source_name)};
    }
    palette_offset += header.palette_count * 4;
  } else {
    header.palette_offset = palette_offset;
  }

  if (!has_file_header) {
    header.pixel_offset = palette_offset;
  }
  if (header.pixel_offset < palette_offset || header.pixel_offset > bytes.size()) {
    return awj::compat::unexpected{awj::compat::format("BMP 像素偏移无效: {}", source_name)};
  }

  return header;
}

awj::compat::expected<std::vector<PaletteEntry>, std::string> read_palette(
    const std::vector<std::byte>& bytes,
    const Header& header,
    std::string_view source_name) {
  std::vector<PaletteEntry> palette;
  if (header.palette_count == 0) {
    return palette;
  }
  try {
    palette.reserve(header.palette_count);
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"BMP palette 内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"BMP palette 尺寸超过运行时限制。"};
  }
  for (std::size_t index = 0; index < header.palette_count; ++index) {
    const auto offset = header.palette_offset + index * 4;
    if (!has_range(bytes, offset, 4)) {
      return awj::compat::unexpected{awj::compat::format("BMP palette 不完整: {}", source_name)};
    }
    palette.push_back(PaletteEntry{.r = u8(bytes, offset + 2),
                                   .g = u8(bytes, offset + 1),
                                   .b = u8(bytes, offset),
                                   .a = 255});
  }
  return palette;
}

awj::compat::expected<std::size_t, std::string> checked_row_stride(const Header& header) {
  const auto bits_per_row = static_cast<std::uint64_t>(header.width) * header.bits_per_pixel;
  const auto stride = ((bits_per_row + 31u) / 32u) * 4u;
  if (stride > std::numeric_limits<std::size_t>::max()) {
    return awj::compat::unexpected{"BMP 行跨度超过运行时限制。"};
  }
  return static_cast<std::size_t>(stride);
}

BitMask make_mask(std::uint32_t mask) {
  BitMask result{.mask = mask};
  if (mask == 0) {
    return result;
  }
  while (((mask >> result.shift) & 1u) == 0u && result.shift < 32) {
    ++result.shift;
  }
  auto shifted = mask >> result.shift;
  while ((shifted & 1u) != 0u && result.bits < 32) {
    ++result.bits;
    shifted >>= 1u;
  }
  return result;
}

std::uint8_t scale_masked_component(std::uint32_t value, const BitMask& mask) {
  if (mask.mask == 0 || mask.bits <= 0) {
    return 0;
  }
  const auto raw = (value & mask.mask) >> mask.shift;
  if (mask.bits >= 8) {
    return static_cast<std::uint8_t>(raw >> (mask.bits - 8));
  }
  const auto max_value = (1u << mask.bits) - 1u;
  return static_cast<std::uint8_t>((raw * 255u + max_value / 2u) / max_value);
}

awj::compat::expected<void, std::string> write_palette_pixel(std::vector<std::byte>& rgba,
                                                     std::size_t offset,
                                                     std::uint8_t index,
                                                     const std::vector<PaletteEntry>& palette,
                                                     std::string_view source_name) {
  if (index >= palette.size()) {
    return awj::compat::unexpected{awj::compat::format("BMP palette index 越界: {}", source_name)};
  }
  const auto& color = palette[index];
  rgba[offset] = std::byte{color.r};
  rgba[offset + 1] = std::byte{color.g};
  rgba[offset + 2] = std::byte{color.b};
  rgba[offset + 3] = std::byte{color.a};
  return {};
}

awj::compat::expected<void, std::string> decode_indexed_pixel_row(
    const std::vector<std::byte>& bytes,
    const Header& header,
    const std::vector<PaletteEntry>& palette,
    std::size_t row_offset,
    std::size_t dest_y,
    std::vector<std::byte>& rgba,
    std::string_view source_name) {
  const auto row_stride = static_cast<std::size_t>(header.width) * std::size_t{4};
  for (std::uint32_t x = 0; x < header.width; ++x) {
    std::uint8_t palette_index = 0;
    if (header.bits_per_pixel == 8) {
      palette_index = u8(bytes, row_offset + x);
    } else if (header.bits_per_pixel == 4) {
      const auto packed = u8(bytes, row_offset + x / 2u);
      palette_index = (x % 2u == 0u) ? static_cast<std::uint8_t>(packed >> 4u)
                                     : static_cast<std::uint8_t>(packed & 0x0Fu);
    } else {
      const auto packed = u8(bytes, row_offset + x / 8u);
      const auto bit = 7u - (x % 8u);
      palette_index = static_cast<std::uint8_t>((packed >> bit) & 1u);
    }
    const auto pixel_offset = dest_y * row_stride + static_cast<std::size_t>(x) * std::size_t{4};
    auto written = write_palette_pixel(rgba, pixel_offset, palette_index, palette, source_name);
    if (!written) {
      return awj::compat::unexpected{written.error()};
    }
  }
  return {};
}

awj::compat::expected<void, std::string> decode_truecolor_pixel_row(
    const std::vector<std::byte>& bytes,
    const Header& header,
    std::size_t row_offset,
    std::size_t dest_y,
    std::vector<std::byte>& rgba,
    bool& saw_alpha,
    bool& saw_nonzero_legacy_alpha) {
  const auto row_stride = static_cast<std::size_t>(header.width) * std::size_t{4};
  const auto red = make_mask(header.red_mask);
  const auto green = make_mask(header.green_mask);
  const auto blue = make_mask(header.blue_mask);
  const auto alpha = make_mask(header.alpha_mask);
  for (std::uint32_t x = 0; x < header.width; ++x) {
    const auto pixel_offset = dest_y * row_stride + static_cast<std::size_t>(x) * std::size_t{4};
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    if (header.bits_per_pixel == 24) {
      const auto source = row_offset + static_cast<std::size_t>(x) * std::size_t{3};
      b = u8(bytes, source);
      g = u8(bytes, source + 1);
      r = u8(bytes, source + 2);
    } else if (header.bits_per_pixel == 32) {
      const auto source = row_offset + static_cast<std::size_t>(x) * std::size_t{4};
      const auto value = le32(bytes, source);
      r = scale_masked_component(value, red);
      g = scale_masked_component(value, green);
      b = scale_masked_component(value, blue);
      if (header.has_alpha_mask || header.compression == bi_rgb) {
        a = scale_masked_component(value, alpha);
        saw_alpha = saw_alpha || a != 255;
        saw_nonzero_legacy_alpha = saw_nonzero_legacy_alpha || a != 0;
      }
    } else {
      const auto source = row_offset + static_cast<std::size_t>(x) * std::size_t{2};
      const auto value = le16(bytes, source);
      r = scale_masked_component(value, red);
      g = scale_masked_component(value, green);
      b = scale_masked_component(value, blue);
      if (header.has_alpha_mask) {
        a = scale_masked_component(value, alpha);
        saw_alpha = saw_alpha || a != 255;
      }
    }
    rgba[pixel_offset] = std::byte{r};
    rgba[pixel_offset + 1] = std::byte{g};
    rgba[pixel_offset + 2] = std::byte{b};
    rgba[pixel_offset + 3] = std::byte{a};
  }
  return {};
}

awj::compat::expected<std::vector<std::byte>, std::string> decode_uncompressed(
    const std::vector<std::byte>& bytes,
    const Header& header,
    const std::vector<PaletteEntry>& palette,
    bool& has_alpha,
    std::string_view source_name) {
  auto stride = checked_row_stride(header);
  if (!stride) {
    return awj::compat::unexpected{stride.error()};
  }
  const auto pixel_bytes = decoder_common::checked_image_bytes(*stride, header.height, "BMP decoder");
  if (!pixel_bytes) {
    return awj::compat::unexpected{pixel_bytes.error()};
  }
  if (!has_range(bytes, header.pixel_offset, *pixel_bytes)) {
    return awj::compat::unexpected{awj::compat::format("BMP 像素数据不完整: {}", source_name)};
  }
  const auto rgba_stride = decoder_common::checked_rgba_stride(header.width, "BMP decoder");
  if (!rgba_stride) {
    return awj::compat::unexpected{rgba_stride.error()};
  }
  const auto rgba_bytes = decoder_common::checked_image_bytes(*rgba_stride,
                                                              header.height,
                                                              "BMP decoder");
  if (!rgba_bytes) {
    return awj::compat::unexpected{rgba_bytes.error()};
  }
  auto rgba = decoder_common::make_byte_buffer(*rgba_bytes, "BMP decoder", std::byte{255});
  if (!rgba) {
    return awj::compat::unexpected{rgba.error()};
  }

  bool saw_nonzero_legacy_alpha = false;
  for (std::uint32_t dest_y = 0; dest_y < header.height; ++dest_y) {
    const auto file_y = header.top_down ? dest_y : header.height - 1u - dest_y;
    const auto row_offset = header.pixel_offset + static_cast<std::size_t>(file_y) * *stride;
    if (header.bits_per_pixel <= 8) {
      auto row = decode_indexed_pixel_row(bytes, header, palette, row_offset, dest_y, *rgba,
                                          source_name);
      if (!row) {
        return awj::compat::unexpected{row.error()};
      }
    } else {
      auto row = decode_truecolor_pixel_row(bytes, header, row_offset, dest_y, *rgba,
                                            has_alpha, saw_nonzero_legacy_alpha);
      if (!row) {
        return awj::compat::unexpected{row.error()};
      }
    }
  }

  if (header.bits_per_pixel == 32 && header.compression == bi_rgb &&
      !header.has_alpha_mask && !saw_nonzero_legacy_alpha) {
    for (std::size_t offset = 3; offset < rgba->size(); offset += 4) {
      (*rgba)[offset] = std::byte{255};
    }
    has_alpha = false;
  }
  return std::move(*rgba);
}

std::size_t output_offset_for(std::uint32_t x, std::uint32_t file_y, const Header& header) {
  const auto dest_y = header.top_down ? file_y : header.height - 1u - file_y;
  return (static_cast<std::size_t>(dest_y) * header.width + x) * std::size_t{4};
}

awj::compat::expected<void, std::string> write_rle_index(std::vector<std::byte>& rgba,
                                                 const Header& header,
                                                 const std::vector<PaletteEntry>& palette,
                                                 std::uint32_t& x,
                                                 std::uint32_t& y,
                                                 std::uint8_t index,
                                                 std::string_view source_name) {
  if (x >= header.width || y >= header.height) {
    return awj::compat::unexpected{awj::compat::format("BMP RLE 数据越界: {}", source_name)};
  }
  const auto offset = output_offset_for(x, y, header);
  auto written = write_palette_pixel(rgba, offset, index, palette, source_name);
  if (!written) {
    return awj::compat::unexpected{written.error()};
  }
  ++x;
  return {};
}

awj::compat::expected<void, std::string> skip_rle_pad(std::size_t& offset,
                                              std::size_t byte_count,
                                              const std::vector<std::byte>& bytes,
                                              std::string_view source_name) {
  if ((byte_count % std::size_t{2}) == std::size_t{1}) {
    if (!has_range(bytes, offset, 1)) {
      return awj::compat::unexpected{awj::compat::format("BMP RLE padding 不完整: {}", source_name)};
    }
    ++offset;
  }
  return {};
}

awj::compat::expected<std::vector<std::byte>, std::string> decode_rle8(
    const std::vector<std::byte>& bytes,
    const Header& header,
    const std::vector<PaletteEntry>& palette,
    std::string_view source_name) {
  const auto rgba_stride = decoder_common::checked_rgba_stride(header.width, "BMP RLE8 decoder");
  if (!rgba_stride) {
    return awj::compat::unexpected{rgba_stride.error()};
  }
  const auto rgba_bytes = decoder_common::checked_image_bytes(*rgba_stride,
                                                              header.height,
                                                              "BMP RLE8 decoder");
  if (!rgba_bytes) {
    return awj::compat::unexpected{rgba_bytes.error()};
  }
  auto rgba = decoder_common::make_byte_buffer(*rgba_bytes, "BMP RLE8 decoder", std::byte{255});
  if (!rgba) {
    return awj::compat::unexpected{rgba.error()};
  }

  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::size_t offset = header.pixel_offset;
  bool finished = false;
  while (!finished) {
    if (!has_range(bytes, offset, 2)) {
      return awj::compat::unexpected{awj::compat::format("BMP RLE8 数据不完整: {}", source_name)};
    }
    const auto count = u8(bytes, offset++);
    const auto value = u8(bytes, offset++);
    if (count > 0) {
      for (std::uint8_t i = 0; i < count; ++i) {
        auto written = write_rle_index(*rgba, header, palette, x, y, value, source_name);
        if (!written) {
          return awj::compat::unexpected{written.error()};
        }
      }
      continue;
    }
    if (value == 0) {
      x = 0;
      ++y;
      if (y > header.height) {
        return awj::compat::unexpected{awj::compat::format("BMP RLE8 行越界: {}", source_name)};
      }
    } else if (value == 1) {
      finished = true;
    } else if (value == 2) {
      if (!has_range(bytes, offset, 2)) {
        return awj::compat::unexpected{awj::compat::format("BMP RLE8 delta 不完整: {}", source_name)};
      }
      x += u8(bytes, offset++);
      y += u8(bytes, offset++);
      if (x > header.width || y > header.height) {
        return awj::compat::unexpected{awj::compat::format("BMP RLE8 delta 越界: {}", source_name)};
      }
    } else {
      const auto absolute_count = static_cast<std::size_t>(value);
      if (!has_range(bytes, offset, absolute_count)) {
        return awj::compat::unexpected{awj::compat::format("BMP RLE8 absolute 数据不完整: {}", source_name)};
      }
      for (std::size_t i = 0; i < absolute_count; ++i) {
        auto written = write_rle_index(*rgba, header, palette, x, y, u8(bytes, offset + i),
                                       source_name);
        if (!written) {
          return awj::compat::unexpected{written.error()};
        }
      }
      offset += absolute_count;
      auto padding = skip_rle_pad(offset, absolute_count, bytes, source_name);
      if (!padding) {
        return awj::compat::unexpected{padding.error()};
      }
    }
  }
  return std::move(*rgba);
}

awj::compat::expected<std::vector<std::byte>, std::string> decode_rle4(
    const std::vector<std::byte>& bytes,
    const Header& header,
    const std::vector<PaletteEntry>& palette,
    std::string_view source_name) {
  const auto rgba_stride = decoder_common::checked_rgba_stride(header.width, "BMP RLE4 decoder");
  if (!rgba_stride) {
    return awj::compat::unexpected{rgba_stride.error()};
  }
  const auto rgba_bytes = decoder_common::checked_image_bytes(*rgba_stride,
                                                              header.height,
                                                              "BMP RLE4 decoder");
  if (!rgba_bytes) {
    return awj::compat::unexpected{rgba_bytes.error()};
  }
  auto rgba = decoder_common::make_byte_buffer(*rgba_bytes, "BMP RLE4 decoder", std::byte{255});
  if (!rgba) {
    return awj::compat::unexpected{rgba.error()};
  }

  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::size_t offset = header.pixel_offset;
  bool finished = false;
  while (!finished) {
    if (!has_range(bytes, offset, 2)) {
      return awj::compat::unexpected{awj::compat::format("BMP RLE4 数据不完整: {}", source_name)};
    }
    const auto count = u8(bytes, offset++);
    const auto value = u8(bytes, offset++);
    if (count > 0) {
      for (std::uint8_t i = 0; i < count; ++i) {
        const auto index = (i % 2u == 0u) ? static_cast<std::uint8_t>(value >> 4u)
                                          : static_cast<std::uint8_t>(value & 0x0Fu);
        auto written = write_rle_index(*rgba, header, palette, x, y, index, source_name);
        if (!written) {
          return awj::compat::unexpected{written.error()};
        }
      }
      continue;
    }
    if (value == 0) {
      x = 0;
      ++y;
      if (y > header.height) {
        return awj::compat::unexpected{awj::compat::format("BMP RLE4 行越界: {}", source_name)};
      }
    } else if (value == 1) {
      finished = true;
    } else if (value == 2) {
      if (!has_range(bytes, offset, 2)) {
        return awj::compat::unexpected{awj::compat::format("BMP RLE4 delta 不完整: {}", source_name)};
      }
      x += u8(bytes, offset++);
      y += u8(bytes, offset++);
      if (x > header.width || y > header.height) {
        return awj::compat::unexpected{awj::compat::format("BMP RLE4 delta 越界: {}", source_name)};
      }
    } else {
      const auto pixel_count = static_cast<std::size_t>(value);
      const auto byte_count = (pixel_count + std::size_t{1}) / std::size_t{2};
      if (!has_range(bytes, offset, byte_count)) {
        return awj::compat::unexpected{awj::compat::format("BMP RLE4 absolute 数据不完整: {}", source_name)};
      }
      for (std::size_t i = 0; i < pixel_count; ++i) {
        const auto packed = u8(bytes, offset + i / std::size_t{2});
        const auto index = (i % std::size_t{2} == std::size_t{0})
                               ? static_cast<std::uint8_t>(packed >> 4u)
                               : static_cast<std::uint8_t>(packed & 0x0Fu);
        auto written = write_rle_index(*rgba, header, palette, x, y, index, source_name);
        if (!written) {
          return awj::compat::unexpected{written.error()};
        }
      }
      offset += byte_count;
      auto padding = skip_rle_pad(offset, byte_count, bytes, source_name);
      if (!padding) {
        return awj::compat::unexpected{padding.error()};
      }
    }
  }
  return std::move(*rgba);
}

awj::compat::expected<ImageBuffer, std::string> decode_bmp(const std::vector<std::byte>& bytes,
                                                   const Header& header,
                                                   std::string_view source_name) {
  const auto palette = read_palette(bytes, header, source_name);
  if (!palette) {
    return awj::compat::unexpected{palette.error()};
  }

  bool has_alpha = header.has_alpha_mask;
  std::vector<std::byte> rgba_bytes;
  if (header.compression == bi_rle8) {
    auto rgba = decode_rle8(bytes, header, *palette, source_name);
    if (!rgba) {
      return awj::compat::unexpected{rgba.error()};
    }
    rgba_bytes = std::move(*rgba);
  } else if (header.compression == bi_rle4) {
    auto rgba = decode_rle4(bytes, header, *palette, source_name);
    if (!rgba) {
      return awj::compat::unexpected{rgba.error()};
    }
    rgba_bytes = std::move(*rgba);
  } else {
    auto rgba = decode_uncompressed(bytes, header, *palette, has_alpha, source_name);
    if (!rgba) {
      return awj::compat::unexpected{rgba.error()};
    }
    rgba_bytes = std::move(*rgba);
  }

  ImageSourceInfo source_info{.pixel_format = PixelFormat::rgb,
                              .bit_depth = header.bits_per_pixel == 16 ? 5 : 8};
  const auto alpha_mode = has_alpha ? AlphaMode::straight : AlphaMode::none;
  return decoder_common::make_rgba_image(header.width,
                                         header.height,
                                         std::move(rgba_bytes),
                                         alpha_mode,
                                         "BMP decoder",
                                         source_info);
}

}  // namespace bmp_detail

class BmpImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "awj-bmp"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".bmp", L".dib", L".rle"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  awj::compat::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_bytes(path, "BMP");
      if (!bytes) {
        return awj::compat::unexpected{bytes.error()};
      }
      auto header = bmp_detail::parse_header(*bytes, display_path_for_user(path));
      if (!header) {
        return awj::compat::unexpected{header.error()};
      }
      auto dimensions = decoder_common::make_image_dimensions_checked(header->width, header->height, "BMP");
      if (dimensions) {
        dimensions->source_bit_depth = 8;
        dimensions->source_has_alpha = header->has_alpha_mask || header->bits_per_pixel == 32;
        // 当前 BMP 解码路径只物化普通 RGBA8，不附加 HDR 语义或 ICC 数学变换。
        dimensions->source_dynamic_range = ImageDynamicRangeHint::sdr;
        const auto pixels = static_cast<std::uint64_t>(header->width) * header->height;
        dimensions->decoded_storage_bytes = pixels > UINT64_MAX / 4 ? UINT64_MAX : pixels * 4;
      }
      return dimensions;
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"BMP 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"BMP 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"BMP 尺寸探测文件系统访问失败。"};
    }
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_bytes(path, "BMP");
      if (!bytes) {
        return awj::compat::unexpected{bytes.error()};
      }
      const auto source_name = display_path_for_user(path);
      auto header = bmp_detail::parse_header(*bytes, source_name);
      if (!header) {
        return awj::compat::unexpected{header.error()};
      }
      auto image = bmp_detail::decode_bmp(*bytes, *header, source_name);
      if (!image) {
        return awj::compat::unexpected{image.error()};
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "awj-bmp"};
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"BMP 解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"BMP 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"BMP 解码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
