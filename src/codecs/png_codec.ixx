module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <png.h>
#include <zlib.h>
#include <lcms2.h>

#ifdef _MSC_VER
#pragma warning(disable : 4611)
#endif

export module awj.png_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.hdr_tonemap;
import awj.image;
import awj.pixel_conversion;
import awj.large_image_plan;

export namespace awj {

namespace png_detail {

std::expected<int, std::string> encoding_bit_depth(
    const ImageBuffer& image, const NativeEncodeSettings& settings) {
  if (settings.bit_depth) {
    if (*settings.bit_depth != 8 && *settings.bit_depth != 16)
      return std::unexpected{"PNG bit-depth 只支持 8 或 16。"};
    return *settings.bit_depth;
  }
  if (settings.tone_map_to_sdr_pixels) return 8;
  auto signal = hdr::has_explicit_hdr_signal(image);
  if (!signal) return std::unexpected{signal.error()};
  // SDR compatibility is intentional, even when the working buffer is RGBA16.
  return *signal ? 16 : 8;
}

const MetadataBlock* first_metadata(const ImageBuffer& image, MetadataKind kind) noexcept;

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
  const auto crc = crc32_z(0, reinterpret_cast<const Bytef*>(type.data()), type.size());
  if (payload.empty()) return static_cast<std::uint32_t>(crc);
  return static_cast<std::uint32_t>(crc32_z(crc, reinterpret_cast<const Bytef*>(payload.data()), payload.size()));
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
  // CICP describes the pixels, not optional provenance. Ordinary HDR and
  // non-sRGB SDR need it just as much as a rendered Gain Map does. A retained
  // ICC is authoritative instead; do not emit potentially conflicting CICP.
  if (settings.applied_icc == "kept" && first_metadata(image, MetadataKind::icc)) {
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
  if (!settings.strip_metadata && image.source_info && image.source_info->content_light) {
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

inline constexpr std::size_t apng_max_frames = 100000;
inline constexpr std::size_t png_scan_block_bytes = 64 * 1024;

bool is_global_decoding_chunk(std::string_view type) noexcept {
  return type == "PLTE" || type == "tRNS" || type == "iCCP" || type == "sRGB" ||
      type == "gAMA" || type == "cHRM" || type == "cICP" || type == "cLLI" ||
      type == "sBIT" || type == "eXIf";
}

bool valid_ihdr(std::span<const std::byte> payload) noexcept {
  if (payload.size() != 13) return false;
  const auto width = read_be_u32(payload, 0), height = read_be_u32(payload, 4);
  if (!width || !height || width > 0x7fffffffu || height > 0x7fffffffu ||
      payload[10] != std::byte{0} || payload[11] != std::byte{0} ||
      std::to_integer<unsigned>(payload[12]) > 1) return false;
  const auto depth = std::to_integer<unsigned>(payload[8]);
  switch (std::to_integer<unsigned>(payload[9])) {
    case PNG_COLOR_TYPE_GRAY: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case PNG_COLOR_TYPE_PALETTE: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case PNG_COLOR_TYPE_RGB:
    case PNG_COLOR_TYPE_GRAY_ALPHA:
    case PNG_COLOR_TYPE_RGB_ALPHA: return depth == 8 || depth == 16;
    default: return false;
  }
}

// The file probe and APNG reader share structural validation. An independent
// default image is legal APNG; static-first routing is an AWJimage product rule.
struct PngAnimationLayout {
  std::uint32_t width{}, height{}, declared_frames{}, plays{};
  std::uint64_t sequence{}, idat_bytes{}, frame_data_bytes{}, inherited_bytes{};
  std::size_t frames{};
  bool ihdr{}, actl{}, idat{}, idat_closed{}, first_uses_idat{}, ended{};

  bool uses_animation_reader() const noexcept { return actl && first_uses_idat; }

  std::expected<void, std::string> accept(std::string_view type,
      std::span<const std::byte> prefix, std::uint32_t size, std::uint64_t payload_offset) {
    if (idat && type != "IDAT") idat_closed = true;
    if (type == "IHDR") {
      if (ihdr || payload_offset != 16 || !valid_ihdr(prefix))
        return std::unexpected{"PNG IHDR 无效。"};
      ihdr = true;
      width = read_be_u32(prefix, 0); height = read_be_u32(prefix, 4);
    } else if (!ihdr) {
      return std::unexpected{"PNG 首个 chunk 必须是 IHDR。"};
    } else if (type == "acTL") {
      if (actl || idat || size != 8) return std::unexpected{"APNG acTL 位置或长度无效。"};
      actl = true;
      if (inherited_bytes > encoding_defaults::codec_metadata_max_bytes + 1024ull)
        return std::unexpected{"APNG 全局 metadata 过大。"};
      declared_frames = read_be_u32(prefix, 0); plays = read_be_u32(prefix, 4);
      if (!declared_frames || declared_frames > apng_max_frames ||
          plays > static_cast<unsigned>(std::numeric_limits<int>::max()))
        return std::unexpected{"APNG 帧数/loop 超出上限。"};
    } else if (type == "fcTL") {
      if (!actl || size != 26 || read_be_u32(prefix, 0) != sequence++)
        return std::unexpected{"APNG fcTL 顺序或长度无效。"};
      if (frames && !frame_data_bytes) return std::unexpected{"APNG 帧缺少数据。"};
      if (frames == declared_frames) return std::unexpected{"APNG 帧数超过 acTL 声明。"};
      const auto w = read_be_u32(prefix, 4), h = read_be_u32(prefix, 8);
      const auto x = read_be_u32(prefix, 12), y = read_be_u32(prefix, 16);
      if (!w || !h || x > width || y > height || w > width - x || h > height - y ||
          std::to_integer<unsigned>(prefix[24]) > 2 || std::to_integer<unsigned>(prefix[25]) > 1)
        return std::unexpected{"APNG frame rect/blend/dispose 无效。"};
      if (!frames && !idat) {
        first_uses_idat = true;
        if (x || y || w != width || h != height)
          return std::unexpected{"APNG IDAT 首帧必须与默认 PNG 画布一致。"};
      }
      ++frames;
      frame_data_bytes = 0;
    } else if (type == "IDAT") {
      if (idat_closed) return std::unexpected{"APNG IDAT 必须连续。"};
      if (frames && (!first_uses_idat || frames != 1))
        return std::unexpected{"APNG IDAT 出现在非默认帧。"};
      idat = true;
      idat_bytes += size;
      if (frames) frame_data_bytes += size;
    } else if (type == "fdAT") {
      if (!idat || !frames || size < 4 || (first_uses_idat && frames == 1) ||
          read_be_u32(prefix, 0) != sequence++) return std::unexpected{"APNG fdAT 顺序无效。"};
      frame_data_bytes += size - 4;
    } else if (type == "IEND") {
      if (size || !idat_bytes) return std::unexpected{"PNG IEND 或默认图像数据无效。"};
      if (actl && (frames != declared_frames || !frames || !frame_data_bytes))
        return std::unexpected{"APNG 帧数据/计数不完整。"};
      ended = true;
    } else if (is_global_decoding_chunk(type)) {
      if (idat && type != "eXIf") return std::unexpected{"PNG 色彩/解码 chunk 必须位于 IDAT 前。"};
      constexpr auto limit = encoding_defaults::codec_metadata_max_bytes + 1024ull;
      if (actl && (inherited_bytes > limit || size + 12ull > limit - inherited_bytes))
        return std::unexpected{"APNG 全局 metadata 过大。"};
      inherited_bytes += size + 12ull;
    } else if ((type[0] & 32) == 0) {
      return std::unexpected{"PNG 使用未知关键 chunk。"};
    }
    return {};
  }
};

// Read at most one fixed-size block at a time. Only a 26-byte control prefix is
// retained, except bounded iCCP metadata requested by a header-only probe.
// Visitors must not retain prefix spans; the APNG index borrows the original bytes.
template <class Read, class Visit>
std::expected<void, std::string> scan_png_chunks(std::uint64_t input_size,
    Read&& read, Visit&& visit, std::string_view source_name,
    bool metadata_only, std::stop_token stop) {
  if (input_size > encoding_defaults::effective_max_input_file_bytes())
    return std::unexpected{std::format("PNG 文件超过当前输入上限: {}", source_name)};
  if (input_size < 8) return std::unexpected{std::format("PNG 签名无效: {}", source_name)};
  auto signature = read(0, 8);
  if (!signature) return std::unexpected{signature.error()};
  if (png_sig_cmp(reinterpret_cast<png_const_bytep>(signature->data()), 0, 8) != 0)
    return std::unexpected{std::format("PNG 签名无效: {}", source_name)};
  std::vector<std::byte> prefix;
  std::size_t chunks{};
  for (std::uint64_t pos = 8; pos < input_size;) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    if (++chunks > 1000000) return std::unexpected{"PNG chunk 数量超过上限。"};
    if (input_size - pos < 12) return std::unexpected{"PNG chunk 截断。"};
    auto header = read(pos, 8);
    if (!header) return std::unexpected{header.error()};
    const auto size = read_be_u32(*header, 0);
    if (size > 0x7fffffffu || size > input_size - pos - 12)
      return std::unexpected{"PNG chunk 长度无效或越界。"};
    std::array<char, 4> tag{};
    std::memcpy(tag.data(), header->data() + 4, tag.size());
    for (const char c : tag) {
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
        return std::unexpected{"PNG chunk 类型无效。"};
    }
    const std::string_view type{tag.data(), tag.size()};
    if (metadata_only && type == "IDAT") return {};
    auto retained = std::min<std::size_t>(size, 26);
    if (metadata_only && type == "iCCP") {
      if (size > encoding_defaults::codec_metadata_max_bytes)
        return std::unexpected{"PNG ICC chunk 超过 64 MiB 上限。"};
      retained = size;
    }
    prefix.resize(retained);
    auto crc = crc32_z(0, reinterpret_cast<const Bytef*>(tag.data()), tag.size());
    for (std::uint64_t consumed = 0; consumed < size;) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size - consumed, png_scan_block_bytes));
      auto block = read(pos + 8 + consumed, count);
      if (!block) return std::unexpected{block.error()};
      crc = crc32_z(crc, reinterpret_cast<const Bytef*>(block->data()), block->size());
      if (consumed < retained) {
        const auto copy = std::min<std::size_t>(count, retained - static_cast<std::size_t>(consumed));
        std::copy_n(block->begin(), copy, prefix.begin() + static_cast<std::size_t>(consumed));
      }
      consumed += count;
    }
    auto stored_crc = read(pos + 8 + size, 4);
    if (!stored_crc) return std::unexpected{stored_crc.error()};
    if (crc != read_be_u32(*stored_crc, 0)) return std::unexpected{"PNG chunk CRC 错误。"};
    if (type == "IEND" && (size || pos + 12 != input_size || metadata_only))
      return std::unexpected{"PNG IEND 无效或缺少 IDAT。"};
    if (auto accepted = visit(type, std::span<const std::byte>{prefix}, size, pos + 8); !accepted)
      return std::unexpected{accepted.error()};
    if (type == "IEND") return {};
    pos += size + 12ull;
  }
  return std::unexpected{"PNG 缺少 IEND 或文件截断。"};
}

template <class Visit>
std::expected<void, std::string> visit_png_chunks(std::span<const std::byte> bytes,
    std::string_view source_name, Visit&& visit, std::stop_token stop = {}) {
  auto read = [&](std::uint64_t offset, std::size_t count)
      -> std::expected<std::span<const std::byte>, std::string> {
    return bytes.subspan(static_cast<std::size_t>(offset), count);
  };
  return scan_png_chunks(bytes.size(), read, visit, source_name, false, stop);
}

template <class Visit>
std::expected<void, std::string> visit_png_file_chunks(const fs::path& path,
    Visit&& visit, bool metadata_only = false, std::stop_token stop = {}) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) return std::unexpected{std::format("无法读取 PNG 文件: {}", display_path_for_user(path))};
  const auto end = input.tellg();
  if (end < 0) return std::unexpected{"无法读取 PNG 文件大小。"};
  const auto input_size = static_cast<std::uint64_t>(end);
  input.seekg(0, std::ios::beg);
  if (!input) return std::unexpected{"无法读取 PNG 文件头。"};
  std::array<std::byte, png_scan_block_bytes> buffer{};
  std::uint64_t position{};
  auto read = [&](std::uint64_t offset, std::size_t count)
      -> std::expected<std::span<const std::byte>, std::string> {
    if (offset != position || count > buffer.size()) return std::unexpected{"PNG 扫描偏移无效。"};
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
    if (input.bad() || input.gcount() != static_cast<std::streamsize>(count))
      return std::unexpected{"PNG 文件截断。"};
    position += count;
    return std::span<const std::byte>{buffer}.first(count);
  };
  return scan_png_chunks(input_size, read, visit, display_path_for_user(path), metadata_only, stop);
}

std::expected<bool, std::string> contains_animation_control_chunk(
    std::span<const std::byte> bytes, std::string_view source_name) {
  PngAnimationLayout layout;
  auto scanned = visit_png_chunks(bytes, source_name, [&](auto type, auto prefix, auto size, auto offset) {
    return layout.accept(type, prefix, size, offset);
  });
  if (!scanned) return std::unexpected{scanned.error()};
  return layout.uses_animation_reader();
}

std::expected<bool, std::string> file_contains_animation_control_chunk(
    const fs::path& path, std::stop_token stop = {}) {
  PngAnimationLayout layout;
  auto scanned = visit_png_file_chunks(path, [&](auto type, auto prefix, auto size, auto offset) {
    return layout.accept(type, prefix, size, offset);
  }, false, stop);
  if (!scanned) return std::unexpected{scanned.error()};
  return layout.uses_animation_reader();
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
    case PNG_COLOR_TYPE_GRAY_ALPHA:
      return PixelFormat::gray;
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

// gAMA/cHRM describe encoded pixels. Synthesize an RGB ICC profile so AVIF
// retains those semantics rather than silently assigning sRGB to the samples.
std::expected<std::vector<std::byte>, std::string> gamma_chromaticity_profile(png_structp png, png_infop info) {
  int intent{};
  if (png_get_sRGB(png, info, &intent)) return std::vector<std::byte>{};
  double gamma{};
  double wx = 0.3127, wy = 0.3290, rx = 0.64, ry = 0.33, gx = 0.30, gy = 0.60, bx = 0.15, by = 0.06;
  const bool has_gamma = png_get_gAMA(png, info, &gamma) != 0;
  const bool has_chroma = png_get_cHRM(png, info, &wx, &wy, &rx, &ry, &gx, &gy, &bx, &by) != 0;
  if (!has_gamma && !has_chroma) return std::vector<std::byte>{};
  if (!has_gamma) gamma = 1.0 / 2.2;
  if (!std::isfinite(gamma) || gamma <= 0) return std::unexpected{"PNG gAMA 无效。"};
  cmsCIExyY white{wx, wy, 1};
  cmsCIExyYTRIPLE primaries{{rx, ry, 1}, {gx, gy, 1}, {bx, by, 1}};
  auto* curve = cmsBuildGamma(nullptr, 1.0 / gamma);
  if (!curve) return std::unexpected{"无法创建 PNG gamma profile。"};
  cmsToneCurve* curves[]{curve, curve, curve};
  auto* profile = cmsCreateRGBProfile(&white, &primaries, curves);
  cmsFreeToneCurve(curve);
  if (!profile) return std::unexpected{"PNG cHRM 无效。"};
  struct Guard { cmsHPROFILE profile; ~Guard() { cmsCloseProfile(profile); } } guard{profile};
  cmsUInt32Number size{};
  if (!cmsSaveProfileToMem(profile, nullptr, &size) || size > encoding_defaults::codec_metadata_max_bytes)
    return std::unexpected{"无法序列化 PNG 色彩 profile。"};
  auto bytes = decoder_common::make_byte_buffer(size, "PNG gamma/chromaticity ICC");
  if (!bytes) return std::unexpected{bytes.error()};
  if (!cmsSaveProfileToMem(profile, bytes->data(), &size)) return std::unexpected{"无法写入 PNG 色彩 profile。"};
  return bytes;
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

ImageDynamicRangeHint dynamic_range_for_transfer(int transfer) noexcept {
  switch (transfer) {
    case 16:
    case 18: return ImageDynamicRangeHint::hdr;
    case 1:
    case 4:
    case 5:
    case 6:
    case 7:
    case 13:
    case 14:
    case 15: return ImageDynamicRangeHint::sdr;
    // Unspecified, linear, log and unrecognized transfer functions do not prove SDR.
    default: return ImageDynamicRangeHint::unknown;
  }
}

std::expected<ImageDynamicRangeHint, std::string> inspect_icc_dynamic_range(
    std::span<const std::byte> payload) {
  if (payload.size() < 3) return ImageDynamicRangeHint::unknown;
  const auto name_end = std::ranges::find(payload, std::byte{0});
  const auto name_size = static_cast<std::size_t>(name_end - payload.begin());
  if (!name_size || name_size > 79 || payload.size() < name_size + 3 ||
      payload[name_size + 1] != std::byte{0}) return ImageDynamicRangeHint::unknown;
  const auto compressed = payload.subspan(name_size + 2);
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  if (inflateInit(&stream) != Z_OK) return std::unexpected{"PNG ICC 解压初始化失败。"};
  struct InflateGuard { z_stream* stream; ~InflateGuard() { inflateEnd(stream); } } inflate_guard{&stream};
  std::vector<std::byte> profile;
  std::array<std::byte, 4096> block{};
  for (;;) {
    stream.next_out = reinterpret_cast<Bytef*>(block.data());
    stream.avail_out = static_cast<uInt>(block.size());
    const auto status = inflate(&stream, Z_NO_FLUSH);
    const auto count = block.size() - stream.avail_out;
    if (count > encoding_defaults::codec_metadata_max_bytes - profile.size())
      return std::unexpected{"PNG ICC profile 超过 64 MiB 上限。"};
    profile.insert(profile.end(), block.begin(), block.begin() + count);
    if (status == Z_STREAM_END) {
      if (stream.avail_in) return ImageDynamicRangeHint::unknown;
      break;
    }
    if (status != Z_OK || (!count && !stream.avail_in)) return ImageDynamicRangeHint::unknown;
  }
  if (profile.empty()) return ImageDynamicRangeHint::unknown;
  auto* icc = cmsOpenProfileFromMem(profile.data(), static_cast<cmsUInt32Number>(profile.size()));
  if (!icc) return ImageDynamicRangeHint::unknown;
  struct ProfileGuard { cmsHPROFILE profile; ~ProfileGuard() { cmsCloseProfile(profile); } } profile_guard{icc};
  if (const auto* cicp = static_cast<const cmsVideoSignalType*>(cmsReadTag(icc, cmsSigcicpTag))) {
    if (cicp->MatrixCoefficients == 0 && cicp->VideoFullRangeFlag <= 1)
      return dynamic_range_for_transfer(cicp->TransferCharacteristics);
  }
  // An arbitrary ICC profile, its description or the storage depth alone does
  // not establish SDR. Keep unrecognized profiles conservative for admission.
  return ImageDynamicRangeHint::unknown;
}

std::expected<ImageDimensions, std::string> probe_png_dimensions(const fs::path& path) {
  PngAnimationLayout layout;
  int bit_depth{}, color_type{};
  bool transparency{}, has_cicp{}, has_icc{}, has_srgb{}, has_content_light{};
  std::optional<std::uint32_t> gamma;
  auto cicp_range = ImageDynamicRangeHint::unknown;
  auto icc_range = ImageDynamicRangeHint::unknown;
  auto scanned = visit_png_file_chunks(path,
      [&](std::string_view type, std::span<const std::byte> prefix,
          std::uint32_t size, std::uint64_t offset) -> std::expected<void, std::string> {
    if (auto accepted = layout.accept(type, prefix, size, offset); !accepted) return accepted;
    if (type == "IHDR") {
      bit_depth = std::to_integer<int>(prefix[8]); color_type = std::to_integer<int>(prefix[9]);
    } else if (type == "tRNS") {
      if ((color_type == PNG_COLOR_TYPE_GRAY && size != 2) ||
          (color_type == PNG_COLOR_TYPE_RGB && size != 6) ||
          (color_type == PNG_COLOR_TYPE_PALETTE && (!size || size > 256)) ||
          (color_type & PNG_COLOR_MASK_ALPHA)) return std::unexpected{"PNG tRNS 无效。"};
      transparency = true;
    } else if (type == "cICP") {
      if (has_cicp || size != 4) return std::unexpected{"PNG cICP 重复或长度无效。"};
      has_cicp = true;
      if (prefix[2] == std::byte{0} && std::to_integer<unsigned>(prefix[3]) <= 1)
        cicp_range = dynamic_range_for_transfer(std::to_integer<int>(prefix[1]));
    } else if (type == "iCCP") {
      if (has_icc) return std::unexpected{"PNG iCCP 重复。"};
      has_icc = true;
      auto range = inspect_icc_dynamic_range(prefix);
      if (!range) return std::unexpected{range.error()};
      icc_range = *range;
    } else if (type == "sRGB") {
      if (size != 1 || std::to_integer<unsigned>(prefix[0]) > 3)
        return std::unexpected{"PNG sRGB 无效。"};
      has_srgb = true;
    } else if (type == "gAMA") {
      if (gamma || size != 4 || !read_be_u32(prefix, 0)) return std::unexpected{"PNG gAMA 重复或无效。"};
      gamma = read_be_u32(prefix, 0);
    } else if (type == "cLLI") {
      if (size != 8) return std::unexpected{"PNG cLLI 长度无效。"};
      has_content_light = true;
    }
    return {};
  }, true);
  if (!scanned) return std::unexpected{scanned.error()};
  auto dimensions = decoder_common::make_image_dimensions_checked(layout.width, layout.height, "PNG");
  if (!dimensions) return std::unexpected{dimensions.error()};
  auto stride = decoder_common::checked_rgba_stride(layout.width, "PNG probe", bit_depth == 16 ? 2 : 1);
  if (!stride) return std::unexpected{stride.error()};
  auto bytes = decoder_common::checked_image_bytes(*stride, layout.height, "PNG probe");
  if (!bytes) return std::unexpected{bytes.error()};
  dimensions->source_bit_depth = bit_depth;
  dimensions->source_has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 || transparency;
  dimensions->decoded_storage_bytes = *bytes;
  if (has_content_light) dimensions->source_dynamic_range = ImageDynamicRangeHint::hdr;
  else if (has_cicp) dimensions->source_dynamic_range = cicp_range;
  else if (has_icc) dimensions->source_dynamic_range = icc_range;
  // 无色彩块的低位深 PNG 在本解码器中明确按 sRGB 解读，规划须匹配这条实际解码分支。
  // 显式未知 CICP/ICC、未知 gamma 和高位深无标记输入不进入该默认分支。
  else if (has_srgb || gamma == 45455u || gamma == 50000u || gamma == 55556u ||
           (!gamma && bit_depth <= 8))
    dimensions->source_dynamic_range = ImageDynamicRangeHint::sdr;
  return *dimensions;
}

}  // namespace png_detail

class PngImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libpng"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".png", L".apng"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      return png_detail::probe_png_dimensions(path);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"PNG 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"PNG 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"PNG 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    auto bytes = decoder_common::read_file_bytes(path, "PNG");
    if (!bytes) return std::unexpected{bytes.error()};
    return decode_memory(*bytes, display_path_for_user(path));
  }

  std::expected<ImageDecodeResult, std::string> decode_memory(
      std::span<const std::byte> input, std::string_view source_name,
      DecodeOptions options = {}) const override {
    const bool copy_payloads = options.copy_metadata_payloads.value_or(true);
    const fs::path path{std::string{source_name}};
    try {
      const auto* bytes = &input;
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

      if (copy_payloads) {
        auto icc_profile = png_detail::copy_icc_profile(png.get(), info.get());
        if (!icc_profile) {
          return std::unexpected{icc_profile.error()};
        }
        context->icc_profile = std::move(*icc_profile);
      }
      if (copy_payloads) {
        auto exif_metadata = png_detail::copy_exif_metadata(png.get(), info.get(), end_info.get());
        if (!exif_metadata) {
          return std::unexpected{exif_metadata.error()};
        }
        context->exif_metadata = std::move(*exif_metadata);
      }
      if (copy_payloads) {
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
      if (!color_chunks->has_cicp && context->icc_profile.empty()) {
        if (copy_payloads) {
          auto profile = png_detail::gamma_chromaticity_profile(png.get(), info.get());
          if (!profile) return std::unexpected{profile.error()};
          context->icc_profile = std::move(*profile);
        }
        source_info.color_primaries = 1;
        source_info.transfer_characteristics = 13;
        source_info.matrix_coefficients = 0;
        source_info.color_range = 1;
        source_info.color_metadata_source = context->icc_profile.empty() ? "png-srgb-default" : "png-gamma-chromaticity-icc";
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
      if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
      if (settings.quality < 1 || settings.quality > 100) {
        return std::unexpected{"PNG quality 必须在 1–100 范围内。"};
      }
      auto depth = png_detail::encoding_bit_depth(image, settings);
      if (!depth) return std::unexpected{depth.error()};
      if (image.significant_bits) {
        const auto bits = *image.significant_bits;
        const std::array<int, 4> channels{bits.red, bits.green, bits.blue, bits.alpha};
        if (std::ranges::any_of(channels, [&](int value) { return value < 1 || value > image.bit_depth; }))
          return std::unexpected{"PNG 有效位数必须在 1 到存储位深之间。"};
      }
      if (image.geometry || image.pixel_format != PixelFormat::rgba || image.bit_depth != *depth ||
          image.sample_representation == SampleRepresentation::ieee_half_float ||
          image.alpha_mode == AlphaMode::premultiplied || settings.tone_map_to_sdr_pixels) {
        const bool preserve_alpha = settings.applied_alpha.empty() || settings.applied_alpha == "kept";
        auto converted = convert_image_pixels(image, PixelConversionRequest{.format = PixelFormat::rgba,
            .bit_depth = *depth, .preserve_alpha = preserve_alpha,
            .tone_map_to_sdr = settings.tone_map_to_sdr_pixels}, stop_token, settings.resources);
        if (!converted) return std::unexpected{converted.error()};
        if (image.bit_depth != *depth && image.pixel_format == PixelFormat::rgba &&
            image.sample_representation == SampleRepresentation::unorm &&
            image.alpha_mode != AlphaMode::premultiplied && !settings.tone_map_to_sdr_pixels) {
          const auto bits = image.significant_bits.value_or(
              SignificantBits{image.bit_depth, image.bit_depth, image.bit_depth, image.bit_depth});
          converted->significant_bits = SignificantBits{std::min(bits.red, *depth), std::min(bits.green, *depth),
              std::min(bits.blue, *depth), preserve_alpha ? std::min(bits.alpha, *depth) : *depth};
        }
        auto normalized = settings;
        normalized.bit_depth = *depth;
        normalized.tone_map_to_sdr_pixels = false;
        auto encoded = encode(*converted, normalized, stop_token);
        // PNG compression may be lossless while the requested precision change is not.
        if (encoded && *depth < image.bit_depth) encoded->lossless = false;
        return encoded;
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
      if (settings.applied_icc == "kept") {
        if (const auto* icc = png_detail::first_metadata(image, MetadataKind::icc)) {
          if (icc->bytes.size() > encoding_defaults::codec_metadata_max_bytes)
            return std::unexpected{"PNG ICC profile 超过 64 MiB 上限。"};
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
      diagnostics.applied_bit_depth = bit_depth;
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
