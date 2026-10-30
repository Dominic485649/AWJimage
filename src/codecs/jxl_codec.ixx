module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
#include <string>
#include <string_view>
#include <stop_token>
#include <utility>
#include <vector>

#include <avif/avif.h>
#include <avif/internal.h>
#include <brotli/decode.h>
#include <jxl/cms.h>
#include <jxl/compressed_icc.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/gain_map.h>
#include <jxl/thread_parallel_runner.h>
#include <ultrahdr/gainmapmetadata.h>

export module awj.jxl_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.gain_map;
import awj.image;
import awj.resource_planner;
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
      image.sample_representation != SampleRepresentation::unorm || image.planes.empty()) {
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
  if (color.color_space != JXL_COLOR_SPACE_RGB && color.color_space != JXL_COLOR_SPACE_GRAY) return;
  if (color.white_point == JXL_WHITE_POINT_D65) {
    if (color.color_space == JXL_COLOR_SPACE_GRAY || color.primaries == JXL_PRIMARIES_SRGB)
      source.color_primaries = 1;
    else if (color.primaries == JXL_PRIMARIES_2100) source.color_primaries = 9;
    else if (color.primaries == JXL_PRIMARIES_P3) source.color_primaries = 12;
  }
  source.transfer_characteristics = static_cast<int>(color.transfer_function);
  source.matrix_coefficients = 0;
  source.color_range = 1;
  source.has_hdr_metadata = color.transfer_function == JXL_TRANSFER_FUNCTION_PQ ||
                            color.transfer_function == JXL_TRANSFER_FUNCTION_HLG;
  source.color_metadata_source = "jxl-color-encoding";
}

std::expected<JxlColorEncoding, std::string> color_encoding_from_source(
    const ImageSourceInfo* source, bool strict = false) {
  JxlColorEncoding color{};
  JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
  if (!source) {
    if (strict) return std::unexpected{"JXL Gain Map 端点缺少色彩空间。"};
    return color;
  }
  if (strict && (!source->color_primaries || !source->transfer_characteristics))
    return std::unexpected{"JXL Gain Map 端点缺少明确 CICP 色彩空间。"};
  const auto primaries = source->color_primaries.value_or(1);
  const auto transfer = source->transfer_characteristics.value_or(13);
  if ((source->has_hdr_metadata || transfer == 16 || transfer == 18) && !source->color_primaries)
    return std::unexpected{"JXL HDR 输入缺少明确原色，拒绝猜测。"};
  const bool known_primaries = primaries == 1 || primaries == 9 || primaries == 11 || primaries == 12;
  const bool known_transfer = transfer == 1 || transfer == 8 || transfer == 13 || transfer == 16 || transfer == 17 || transfer == 18;
  // Preserve the existing untagged/unspecified SDR behavior for ordinary JXL,
  // but never guess either endpoint of a gain-map pair or an explicit HDR input.
  if (!strict && !source->has_hdr_metadata && transfer != 16 && transfer != 18 &&
      (!known_primaries || !known_transfer)) return color;
  if (source->has_hdr_metadata && transfer != 16 && transfer != 18)
    return std::unexpected{"JXL HDR 输入缺少可保留的 PQ/HLG 色彩语义。"};
  switch (primaries) {
    case 1: color.primaries = JXL_PRIMARIES_SRGB; break;
    case 9: color.primaries = JXL_PRIMARIES_2100; break;
    case 11: color.primaries = JXL_PRIMARIES_P3; color.white_point = JXL_WHITE_POINT_DCI; break;
    case 12: color.primaries = JXL_PRIMARIES_P3; break;
    default: return std::unexpected{"JXL 输入 CICP 原色不受支持，拒绝改标为 sRGB。"};
  }
  switch (transfer) {
    case 1: color.transfer_function = JXL_TRANSFER_FUNCTION_709; break;
    case 8: color.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR; break;
    case 13: color.transfer_function = JXL_TRANSFER_FUNCTION_SRGB; break;
    case 16: color.transfer_function = JXL_TRANSFER_FUNCTION_PQ; break;
    case 17: color.transfer_function = JXL_TRANSFER_FUNCTION_DCI; break;
    case 18: color.transfer_function = JXL_TRANSFER_FUNCTION_HLG; break;
    default: return std::unexpected{"JXL 输入 CICP 传递特性不受支持，拒绝改标为 sRGB。"};
  }
  color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
  return color;
}

std::uint32_t read_be_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

constexpr JxlBoxType gain_map_box_type{'j', 'h', 'g', 'm'};
constexpr JxlBoxType brotli_box_type{'b', 'r', 'o', 'b'};
constexpr std::string_view gain_map_error_prefix = "Gain Map: ISO 21496-1";

std::string gain_map_error(std::string_view message) {
  if (message.starts_with(gain_map_error_prefix)) return std::string{message};
  return std::format("{} JXL: {}", gain_map_error_prefix, message);
}

bool box_type_matches(std::span<const std::byte> bytes, const JxlBoxType& type) noexcept {
  return bytes.size() == 4 && std::equal(bytes.begin(), bytes.end(), type,
      [](std::byte a, char b) { return std::to_integer<unsigned char>(a) == static_cast<unsigned char>(b); });
}

// These callbacks never throw across either C API. The limit also covers
// allocations made before a compressed ICC/Brotli stream reveals its size.
struct BoundedMetadataAllocator {
  struct alignas(std::max_align_t) Header { std::size_t bytes; };
  std::size_t allocated{};

  static void* allocate(void* opaque, std::size_t size) noexcept {
    auto& self = *static_cast<BoundedMetadataAllocator*>(opaque);
    constexpr auto limit = encoding_defaults::codec_metadata_max_bytes;
    if (size > limit - sizeof(Header) || self.allocated > limit - sizeof(Header) - size) return nullptr;
    const auto total = sizeof(Header) + size;
    auto* block = static_cast<Header*>(std::malloc(total));
    if (!block) return nullptr;
    ::new (block) Header{total};
    self.allocated += total;
    return block + 1;
  }

  static void release(void* opaque, void* address) noexcept {
    if (!address) return;
    auto& self = *static_cast<BoundedMetadataAllocator*>(opaque);
    auto* block = static_cast<Header*>(address) - 1;
    self.allocated -= block->bytes;
    std::free(block);
  }
};

std::expected<std::vector<std::byte>, std::string> decompress_gain_map_box(
    std::span<const std::byte> compressed) {
  BoundedMetadataAllocator allocator;
  using State = std::unique_ptr<BrotliDecoderState, decltype(&BrotliDecoderDestroyInstance)>;
  State state{BrotliDecoderCreateInstance(&BoundedMetadataAllocator::allocate,
      &BoundedMetadataAllocator::release, &allocator), &BrotliDecoderDestroyInstance};
  if (!state) return std::unexpected{gain_map_error("无法创建有界 Brotli decoder。")};
  auto output = decoder_common::make_byte_buffer(16 * 1024, "JXL jhgm");
  if (!output) return std::unexpected{gain_map_error(output.error())};
  auto available_in = compressed.size();
  const auto* next_in = reinterpret_cast<const std::uint8_t*>(compressed.data());
  std::size_t written = 0;
  while (true) {
    auto available_out = output->size() - written;
    auto* next_out = reinterpret_cast<std::uint8_t*>(output->data() + written);
    const auto result = BrotliDecoderDecompressStream(state.get(), &available_in, &next_in,
                                                     &available_out, &next_out, nullptr);
    written = output->size() - available_out;
    if (result == BROTLI_DECODER_RESULT_SUCCESS) {
      if (available_in != 0 || written == 0)
        return std::unexpected{gain_map_error("jhgm Brotli 内容为空或含尾随数据。")};
      output->resize(written);
      return std::move(*output);
    }
    if (result != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT ||
        output->size() >= encoding_defaults::codec_metadata_max_bytes)
      return std::unexpected{gain_map_error("jhgm Brotli 数据损坏、截断或超过 64 MiB 上限。")};
    const auto next_size = std::min(encoding_defaults::codec_metadata_max_bytes, output->size() * 2);
    if (auto resized = decoder_common::resize_buffer(*output, next_size, "JXL jhgm"); !resized)
      return std::unexpected{gain_map_error(resized.error())};
  }
}

// Only BMFF framing is inspected here; the bundle and ISO metadata themselves
// are parsed by the pinned libjxl/libultrahdr APIs below. Seeking past jxlc/jxlp
// lets the file probe find a trailing jhgm without decoding or reading the image.
template <typename ReadAt>
std::expected<std::optional<std::vector<std::byte>>, std::string> read_gain_map_box(
    std::uint64_t file_size, ReadAt&& read_at) {
  std::array<std::byte, 12> signature{};
  const auto signature_size = static_cast<std::size_t>(std::min<std::uint64_t>(file_size, signature.size()));
  if (signature_size == 0) return std::optional<std::vector<std::byte>>{};
  if (auto read = read_at(0, std::span{signature}.first(signature_size)); !read)
    return std::unexpected{gain_map_error(read.error())};
  if (JxlSignatureCheck(reinterpret_cast<const std::uint8_t*>(signature.data()), signature_size) != JXL_SIG_CONTAINER)
    return std::optional<std::vector<std::byte>>{};

  std::optional<std::vector<std::byte>> found;
  std::uint64_t offset = 0;
  std::size_t count = 0;
  while (offset < file_size) {
    if (++count > 65536) return std::unexpected{gain_map_error("容器 box 数量超过安全上限。")};
    if (file_size - offset < 8) return std::unexpected{gain_map_error("容器 box header 截断。")};
    std::array<std::byte, 16> header{};
    if (auto read = read_at(offset, std::span{header}.first(8)); !read)
      return std::unexpected{gain_map_error(read.error())};
    const auto size32 = read_be_u32(header, 0);
    std::uint64_t box_size = size32;
    std::uint64_t header_size = 8;
    if (size32 == 1) {
      if (file_size - offset < 16) return std::unexpected{gain_map_error("容器 extended box header 截断。")};
      if (auto read = read_at(offset + 8, std::span{header}.subspan(8, 8)); !read)
        return std::unexpected{gain_map_error(read.error())};
      box_size = (static_cast<std::uint64_t>(read_be_u32(header, 8)) << 32) | read_be_u32(header, 12);
      header_size = 16;
    } else if (size32 == 0) {
      box_size = file_size - offset;
    }
    if (box_size < header_size || box_size > file_size - offset)
      return std::unexpected{gain_map_error("容器 box 尺寸无效或内容截断。")};
    auto payload_offset = offset + header_size;
    auto payload_size = box_size - header_size;
    const auto type = std::span{header}.subspan(4, 4);
    const bool compressed = box_type_matches(type, brotli_box_type);
    bool is_gain_map = box_type_matches(type, gain_map_box_type);
    if (compressed) {
      if (payload_size < 4) return std::unexpected{gain_map_error("Brotli box 缺少内部类型。")};
      std::array<std::byte, 4> inner_type{};
      if (auto read = read_at(payload_offset, std::span{inner_type}); !read)
        return std::unexpected{gain_map_error(read.error())};
      is_gain_map = box_type_matches(inner_type, gain_map_box_type);
      payload_offset += 4;
      payload_size -= 4;
    }
    if (is_gain_map) {
      if (found) return std::unexpected{gain_map_error("不允许多个 jhgm Gain Map。")};
      if (payload_size == 0 || payload_size > encoding_defaults::codec_metadata_max_bytes)
        return std::unexpected{gain_map_error("jhgm 为空或超过 64 MiB 上限。")};
      auto payload = decoder_common::make_byte_buffer(static_cast<std::size_t>(payload_size), "JXL jhgm");
      if (!payload) return std::unexpected{gain_map_error(payload.error())};
      if (auto read = read_at(payload_offset, std::span{*payload}); !read)
        return std::unexpected{gain_map_error(read.error())};
      if (compressed) {
        auto decompressed = decompress_gain_map_box(*payload);
        if (!decompressed) return std::unexpected{decompressed.error()};
        found = std::move(*decompressed);
      } else {
        found = std::move(*payload);
      }
    }
    offset += box_size;
  }
  return found;
}

std::expected<std::optional<std::vector<std::byte>>, std::string> read_gain_map_box(
    std::span<const std::byte> bytes) {
  return read_gain_map_box(bytes.size(), [&](std::uint64_t offset, std::span<std::byte> output)
      -> std::expected<void, std::string> {
    if (offset > bytes.size() || output.size() > bytes.size() - static_cast<std::size_t>(offset))
      return std::unexpected{"容器读取范围无效。"};
    std::ranges::copy(bytes.subspan(static_cast<std::size_t>(offset), output.size()), output.begin());
    return {};
  });
}

std::expected<std::vector<std::byte>, std::string> transcode_alt_icc(
    std::span<const std::byte> input, bool compress) {
  if (input.empty() || input.size() > encoding_defaults::codec_metadata_max_bytes)
    return std::unexpected{gain_map_error("alternate ICC 尺寸无效。")};
  BoundedMetadataAllocator allocator;
  JxlMemoryManager manager{&allocator, &BoundedMetadataAllocator::allocate, &BoundedMetadataAllocator::release};
  std::uint8_t* raw = nullptr;
  std::size_t size = 0;
  const auto* data = reinterpret_cast<const std::uint8_t*>(input.data());
  const auto success = compress ? JxlICCProfileEncode(&manager, data, input.size(), &raw, &size)
                                : JxlICCProfileDecode(&manager, data, input.size(), &raw, &size);
  auto release = [&](std::uint8_t* p) noexcept { BoundedMetadataAllocator::release(&allocator, p); };
  std::unique_ptr<std::uint8_t, decltype(release)> owner{raw, release};
  if (success != JXL_TRUE || !raw || size == 0 || size > encoding_defaults::codec_metadata_max_bytes)
    return std::unexpected{gain_map_error("alternate ICC 压缩数据无效或超过安全上限。")};
  auto output = decoder_common::make_byte_buffer(size, "JXL alternate ICC");
  if (!output) return std::unexpected{gain_map_error(output.error())};
  std::memcpy(output->data(), raw, size);
  return std::move(*output);
}

std::expected<void, std::string> validate_gain_map_metadata(const avifGainMap& metadata) {
  avifDiagnostics diagnostics{};
  if (avifGainMapValidateMetadata(&metadata, &diagnostics) != AVIF_RESULT_OK)
    return std::unexpected{gain_map_error(std::format("metadata 无效: {}", diagnostics.error))};
  const auto base = static_cast<float>(metadata.baseHdrHeadroom.n) / metadata.baseHdrHeadroom.d;
  const auto alternate = static_cast<float>(metadata.alternateHdrHeadroom.n) / metadata.alternateHdrHeadroom.d;
  if (static_cast<std::uint64_t>(metadata.baseHdrHeadroom.n) * metadata.alternateHdrHeadroom.d ==
      static_cast<std::uint64_t>(metadata.alternateHdrHeadroom.n) * metadata.baseHdrHeadroom.d || base == alternate)
    return std::unexpected{gain_map_error("base 与 alternate 的 HDR headroom 相同或超出可区分精度。")};
  const auto maximum_float = static_cast<double>(std::numeric_limits<float>::max());
  const auto maximum_headroom = std::log2(maximum_float);
  if (base > maximum_headroom || alternate > maximum_headroom)
    return std::unexpected{gain_map_error("HDR headroom 超过有限浮点运算范围。")};
  for (int channel = 0; channel < 3; ++channel) {
    const auto offset = std::max(std::abs(static_cast<double>(metadata.baseOffset[channel].n) / metadata.baseOffset[channel].d),
        std::abs(static_cast<double>(metadata.alternateOffset[channel].n) / metadata.alternateOffset[channel].d));
    // Check both directions: HDR-base reconstruction applies a negative weight.
    // PQ spans at most 10000/203 SDR-white units; leave matrix/channel-sum room.
    const auto maximum_gain = std::log2(maximum_float / (16.0 * (1.0 + 10000.0 / 203.0 + offset)));
    for (const auto fraction : {metadata.gainMapMin[channel], metadata.gainMapMax[channel]}) {
      if (std::abs(static_cast<double>(fraction.n) / fraction.d) > maximum_gain)
        return std::unexpected{gain_map_error("Gain Map 倍率或 offset 超过有限浮点运算范围。")};
    }
  }
  return {};
}

std::expected<std::vector<std::uint8_t>, std::string> encode_iso_gain_map_metadata(
    const avifGainMap& metadata) {
  if (auto valid = validate_gain_map_metadata(metadata); !valid) return std::unexpected{valid.error()};
  ultrahdr::uhdr_gainmap_metadata_frac raw{};
  for (int channel = 0; channel < 3; ++channel) {
    raw.gainMapMinN[channel] = metadata.gainMapMin[channel].n;
    raw.gainMapMinD[channel] = metadata.gainMapMin[channel].d;
    raw.gainMapMaxN[channel] = metadata.gainMapMax[channel].n;
    raw.gainMapMaxD[channel] = metadata.gainMapMax[channel].d;
    raw.gainMapGammaN[channel] = metadata.gainMapGamma[channel].n;
    raw.gainMapGammaD[channel] = metadata.gainMapGamma[channel].d;
    raw.baseOffsetN[channel] = metadata.baseOffset[channel].n;
    raw.baseOffsetD[channel] = metadata.baseOffset[channel].d;
    raw.alternateOffsetN[channel] = metadata.alternateOffset[channel].n;
    raw.alternateOffsetD[channel] = metadata.alternateOffset[channel].d;
  }
  raw.baseHdrHeadroomN = metadata.baseHdrHeadroom.n;
  raw.baseHdrHeadroomD = metadata.baseHdrHeadroom.d;
  raw.alternateHdrHeadroomN = metadata.alternateHdrHeadroom.n;
  raw.alternateHdrHeadroomD = metadata.alternateHdrHeadroom.d;
  raw.backwardDirection = static_cast<std::uint64_t>(raw.baseHdrHeadroomN) * raw.alternateHdrHeadroomD >
                          static_cast<std::uint64_t>(raw.alternateHdrHeadroomN) * raw.baseHdrHeadroomD;
  raw.useBaseColorSpace = metadata.useBaseColorSpace != AVIF_FALSE;
  std::vector<std::uint8_t> bytes;
  const auto status = ultrahdr::uhdr_gainmap_metadata_frac::encodeGainmapMetadata(&raw, bytes);
  if (status.error_code != UHDR_CODEC_OK || bytes.empty() || bytes.size() > UINT16_MAX)
    return std::unexpected{gain_map_error("编码 ISO metadata fraction 失败。")};
  return bytes;
}

std::expected<IsoGainMapPtr, std::string> decode_iso_gain_map_metadata(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 5) return std::unexpected{gain_map_error("ISO metadata header 截断。")};
  // The raw upstream decoder does not report bytes consumed or reject reserved
  // flags. Check its version-0 envelope, not a second fraction/JPEG parser.
  if (bytes[0] != 0 || bytes[1] != 0 || bytes[2] != 0 || bytes[3] != 0 || (bytes[4] & 0x33u) != 0)
    return std::unexpected{gain_map_error("不支持的 ISO metadata 版本或 flags。")};
  const std::size_t channels = (bytes[4] & ultrahdr::kIsMultiChannelMask) ? 3 : 1;
  const std::size_t expected_size = (bytes[4] & 8u) ? 5 + 12 + channels * 20 : 5 + 16 + channels * 40;
  if (bytes.size() != expected_size)
    return std::unexpected{gain_map_error("ISO metadata fraction 截断或包含尾随数据。")};
  const std::vector<std::uint8_t> input{bytes.begin(), bytes.end()};
  ultrahdr::uhdr_gainmap_metadata_frac raw{};
  const auto status = ultrahdr::uhdr_gainmap_metadata_frac::decodeGainmapMetadata(input, &raw);
  if (status.error_code != UHDR_CODEC_OK)
    return std::unexpected{gain_map_error("解码 ISO metadata fraction 失败。")};
  IsoGainMapPtr metadata{avifGainMapCreate(), &avifGainMapDestroy};
  if (!metadata) return std::unexpected{gain_map_error("无法分配 ISO metadata。")};
  for (int channel = 0; channel < 3; ++channel) {
    metadata->gainMapMin[channel] = {raw.gainMapMinN[channel], raw.gainMapMinD[channel]};
    metadata->gainMapMax[channel] = {raw.gainMapMaxN[channel], raw.gainMapMaxD[channel]};
    metadata->gainMapGamma[channel] = {raw.gainMapGammaN[channel], raw.gainMapGammaD[channel]};
    metadata->baseOffset[channel] = {raw.baseOffsetN[channel], raw.baseOffsetD[channel]};
    metadata->alternateOffset[channel] = {raw.alternateOffsetN[channel], raw.alternateOffsetD[channel]};
  }
  metadata->baseHdrHeadroom = {raw.baseHdrHeadroomN, raw.baseHdrHeadroomD};
  metadata->alternateHdrHeadroom = {raw.alternateHdrHeadroomN, raw.alternateHdrHeadroomD};
  metadata->useBaseColorSpace = raw.useBaseColorSpace ? AVIF_TRUE : AVIF_FALSE;
  if (auto valid = validate_gain_map_metadata(*metadata); !valid) return std::unexpected{valid.error()};
  const bool backward = static_cast<std::uint64_t>(raw.baseHdrHeadroomN) * raw.alternateHdrHeadroomD >
                        static_cast<std::uint64_t>(raw.alternateHdrHeadroomN) * raw.baseHdrHeadroomD;
  if (raw.backwardDirection != backward)
    return std::unexpected{gain_map_error("backwardDirection 与 HDR headroom 方向不一致。")};
  // Do not call gainmapMetadataFractionToFloat: it rejects HDR-base maps.
  return std::move(metadata);
}

struct ParsedGainMapBundle {
  // Borrowed only while the read_gain_map_box() payload owner remains alive.
  JxlGainMapBundle bundle{};
  IsoGainMapPtr metadata{nullptr, &avifGainMapDestroy};
};

std::expected<ParsedGainMapBundle, std::string> parse_gain_map_bundle(std::span<const std::byte> bytes) {
  ParsedGainMapBundle parsed;
  std::size_t read = 0;
  if (JxlGainMapReadBundle(&parsed.bundle, reinterpret_cast<const std::uint8_t*>(bytes.data()),
                           bytes.size(), &read) != JXL_TRUE || read != bytes.size() || parsed.bundle.jhgm_version != 0)
    return std::unexpected{gain_map_error("jhgm bundle 损坏、截断或版本不支持。")};
  const auto within_payload = [&](const std::uint8_t* pointer, std::size_t size) {
    if (size == 0) return true;
    const auto start = reinterpret_cast<std::uintptr_t>(bytes.data());
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return pointer && address >= start && address - start <= bytes.size() && size <= bytes.size() - (address - start);
  };
  const auto& bundle = parsed.bundle;
  if (!within_payload(bundle.gain_map_metadata, bundle.gain_map_metadata_size) ||
      !within_payload(bundle.alt_icc, bundle.alt_icc_size) || !within_payload(bundle.gain_map, bundle.gain_map_size) ||
      bundle.gain_map_metadata_size == 0 || bundle.gain_map_size == 0)
    return std::unexpected{gain_map_error("jhgm bundle 借用范围无效。")};
  if (JxlSignatureCheck(bundle.gain_map, bundle.gain_map_size) != JXL_SIG_CODESTREAM)
    return std::unexpected{gain_map_error("辅助 Gain Map 必须是裸 JXL codestream，禁止嵌套容器或 Gain Map。")};
  auto metadata = decode_iso_gain_map_metadata({bundle.gain_map_metadata, bundle.gain_map_metadata_size});
  if (!metadata) return std::unexpected{metadata.error()};
  parsed.metadata = std::move(*metadata);
  if (bundle.has_color_encoding == JXL_TRUE) {
    ImageSourceInfo color;
    apply_encoded_color_profile(color, bundle.color_encoding);
    if (!color.color_primaries || !color.transfer_characteristics ||
        (*color.color_primaries != 1 && *color.color_primaries != 9 && *color.color_primaries != 12) ||
        *color.transfer_characteristics == 2 || *color.transfer_characteristics == JXL_TRANSFER_FUNCTION_GAMMA)
      return std::unexpected{gain_map_error("alternate structured 色彩空间不受支持。")};
    parsed.metadata->altColorPrimaries = static_cast<avifColorPrimaries>(*color.color_primaries);
    parsed.metadata->altTransferCharacteristics = static_cast<avifTransferCharacteristics>(*color.transfer_characteristics);
    parsed.metadata->altMatrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
    parsed.metadata->altYUVRange = AVIF_RANGE_FULL;
  }
  if (bundle.alt_icc_size) {
    auto icc = transcode_alt_icc({reinterpret_cast<const std::byte*>(bundle.alt_icc), bundle.alt_icc_size}, false);
    if (!icc) return std::unexpected{icc.error()};
    if (avifRWDataSet(&parsed.metadata->altICC, reinterpret_cast<const std::uint8_t*>(icc->data()), icc->size()) != AVIF_RESULT_OK)
      return std::unexpected{gain_map_error("无法复制 alternate ICC。")};
  }
  if (bundle.has_color_encoding != JXL_TRUE && bundle.alt_icc_size == 0)
    return std::unexpected{gain_map_error("alternate 缺少色彩空间。")};
  return std::move(parsed);
}

std::expected<JxlBasicInfo, std::string> probe_gain_map_codestream(const JxlGainMapBundle& bundle) {
  DecoderPtr decoder{JxlDecoderCreate(nullptr)};
  if (!decoder || JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
      JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS)
    return std::unexpected{gain_map_error("无法创建辅助 header decoder。")};
  const auto input_size = std::min<std::size_t>(bundle.gain_map_size, encoding_defaults::jxl_max_basic_info_probe_bytes);
  if (JxlDecoderSetInput(decoder.get(), bundle.gain_map, input_size) != JXL_DEC_SUCCESS)
    return std::unexpected{gain_map_error("无法设置辅助 header 输入。")};
  if (input_size == bundle.gain_map_size) JxlDecoderCloseInput(decoder.get());
  while (true) {
    const auto status = JxlDecoderProcessInput(decoder.get());
    if (status == JXL_DEC_BASIC_INFO) {
      JxlBasicInfo info{};
      if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS || info.xsize == 0 || info.ysize == 0 ||
          info.have_animation || info.orientation != JXL_ORIENT_IDENTITY || info.num_extra_channels != 0 ||
          (info.num_color_channels != 1 && info.num_color_channels != 3) ||
          info.exponent_bits_per_sample != 0 || info.bits_per_sample == 0 || info.bits_per_sample > 16)
        return std::unexpected{gain_map_error("辅助图像尺寸、方向、通道或样本类型无效。")};
      const auto stride = checked_rgba_stride(info.xsize, "JXL Gain Map", info.bits_per_sample > 8 ? 2 : 1);
      if (!stride) return std::unexpected{gain_map_error(stride.error())};
      if (auto count = checked_image_bytes(*stride, info.ysize, "JXL Gain Map"); !count)
        return std::unexpected{gain_map_error(count.error())};
      return info;
    }
    if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT || status == JXL_DEC_SUCCESS)
      return std::unexpected{gain_map_error("辅助 codestream header 无效、截断或超过探测上限。")};
  }
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
  // "kept" means that the profile still describes these pixels and is needed
  // for their interpretation, independently of optional metadata stripping.
  if (settings.applied_icc == "kept") {
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

  auto source = image.source_info.value_or(ImageSourceInfo{});
  if (settings.applied_color_primaries) source.color_primaries = settings.applied_color_primaries;
  if (settings.applied_transfer_characteristics) source.transfer_characteristics = settings.applied_transfer_characteristics;
  auto color = color_encoding_from_source(&source);
  if (!color) return std::unexpected{color.error()};
  if (JxlEncoderSetColorEncoding(encoder, &*color) != JXL_ENC_SUCCESS) {
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
    std::stop_token stop_token = {},
    std::size_t maximum_output_bytes = 0) {
  const auto runtime_limit = std::min<std::uint64_t>(encoding_defaults::effective_max_input_file_bytes(),
                                                     std::numeric_limits<std::size_t>::max());
  const auto output_limit = static_cast<std::size_t>(maximum_output_bytes == 0 ? runtime_limit :
      std::min<std::uint64_t>(runtime_limit, maximum_output_bytes));
  if (output_limit == 0) return std::unexpected{"JXL 编码输出上限无效。"};
  std::vector<std::byte> output;
  const auto initial_size = std::min(initial_encoder_output_buffer_size(input_size_hint), output_limit);
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
    if (old_size >= output_limit) return std::unexpected{"JXL 编码输出过大。"};
    const auto next_size = old_size > output_limit / 2 ? output_limit : old_size * 2;
    try {
      output.resize(next_size);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JXL 编码输出缓冲区内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JXL 编码输出缓冲区尺寸超过运行时限制。"};
    }
    next_out = reinterpret_cast<std::uint8_t*>(output.data() + used);
    avail_out = output.size() - used;
  }
}

std::expected<std::vector<std::byte>, std::string> naked_gain_map_codestream(std::vector<std::byte> encoded) {
  const auto signature = JxlSignatureCheck(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
  if (signature == JXL_SIG_CODESTREAM) return encoded;
  if (signature != JXL_SIG_CONTAINER)
    return std::unexpected{gain_map_error("辅助编码未产生有效 JXL codestream。")};
  // libjxl automatically wraps level-10 output even with UseContainer(false).
  // Unwrap only our freshly encoded, stripped auxiliary's standard codestream
  // boxes; never permit another jhgm/metadata bundle inside the gain samples.
  constexpr JxlBoxType whole_type{'j', 'x', 'l', 'c'};
  constexpr JxlBoxType part_type{'j', 'x', 'l', 'p'};
  constexpr JxlBoxType signature_type{'J', 'X', 'L', ' '};
  constexpr JxlBoxType file_type{'f', 't', 'y', 'p'};
  constexpr JxlBoxType level_type{'j', 'x', 'l', 'l'};
  constexpr JxlBoxType index_type{'j', 'x', 'l', 'i'};
  constexpr JxlBoxType padding_type{'f', 'r', 'e', 'e'};
  const std::span<const std::byte> container{encoded};
  std::vector<std::byte> codestream;
  bool whole = false, segmented = false, last_part = false;
  std::uint32_t next_part = 0;
  std::size_t offset = 0, count = 0;
  auto append = [&](std::span<const std::byte> bytes) -> std::expected<void, std::string> {
    if (bytes.size() > encoding_defaults::codec_metadata_max_bytes ||
        codestream.size() > encoding_defaults::codec_metadata_max_bytes - bytes.size())
      return std::unexpected{gain_map_error("辅助裸 codestream 超过 64 MiB 上限。")};
    codestream.insert(codestream.end(), bytes.begin(), bytes.end());
    return {};
  };
  while (offset < container.size()) {
    if (++count > 65536 || container.size() - offset < 8)
      return std::unexpected{gain_map_error("辅助 container header 无效。")};
    const auto size32 = read_be_u32(container, offset);
    std::size_t header_size = 8;
    std::uint64_t size = size32;
    if (size32 == 1) {
      if (container.size() - offset < 16)
        return std::unexpected{gain_map_error("辅助 extended container header 截断。")};
      size = (static_cast<std::uint64_t>(read_be_u32(container, offset + 8)) << 32) | read_be_u32(container, offset + 12);
      header_size = 16;
    } else if (size32 == 0) size = container.size() - offset;
    if (size < header_size || size > container.size() - offset)
      return std::unexpected{gain_map_error("辅助 container box 尺寸无效。")};
    const auto type = container.subspan(offset + 4, 4);
    const auto payload = container.subspan(offset + header_size, static_cast<std::size_t>(size) - header_size);
    if (box_type_matches(type, whole_type)) {
      if (whole || segmented) return std::unexpected{gain_map_error("辅助 codestream box 重复。")};
      whole = true;
      if (auto added = append(payload); !added) return std::unexpected{added.error()};
    } else if (box_type_matches(type, part_type)) {
      if (whole || last_part || payload.size() < 4)
        return std::unexpected{gain_map_error("辅助分段 codestream 无效。")};
      const auto index = read_be_u32(payload, 0);
      if ((index & 0x7fffffffu) != next_part++)
        return std::unexpected{gain_map_error("辅助 codestream 分段顺序无效。")};
      segmented = true;
      last_part = (index & 0x80000000u) != 0;
      if (auto added = append(payload.subspan(4)); !added) return std::unexpected{added.error()};
    } else if (!box_type_matches(type, signature_type) && !box_type_matches(type, file_type) &&
               !box_type_matches(type, level_type) && !box_type_matches(type, index_type) && !box_type_matches(type, padding_type)) {
      return std::unexpected{gain_map_error("辅助 container 含嵌套 metadata/Gain Map。")};
    }
    offset += static_cast<std::size_t>(size);
  }
  if ((!whole && !segmented) || (segmented && !last_part) ||
      JxlSignatureCheck(reinterpret_cast<const std::uint8_t*>(codestream.data()), codestream.size()) != JXL_SIG_CODESTREAM)
    return std::unexpected{gain_map_error("辅助裸 codestream 为空或不完整。")};
  return codestream;
}

std::expected<void, std::string> apply_gain_map_orientation(ImageBuffer& image, JxlOrientation orientation) {
  if (orientation == JXL_ORIENT_IDENTITY) return {};
  ImageGeometry geometry{.stored_width = image.width, .stored_height = image.height,
      .crop_width = image.width, .crop_height = image.height};
  switch (orientation) {
    case JXL_ORIENT_FLIP_HORIZONTAL: geometry.mirror_axis = 1; break;
    case JXL_ORIENT_ROTATE_180: geometry.rotation = 2; break;
    case JXL_ORIENT_FLIP_VERTICAL: geometry.mirror_axis = 0; break;
    case JXL_ORIENT_TRANSPOSE: geometry.rotation = 1; geometry.mirror_axis = 0; break;
    case JXL_ORIENT_ROTATE_90_CW: geometry.rotation = 3; break;
    case JXL_ORIENT_ANTI_TRANSPOSE: geometry.rotation = 1; geometry.mirror_axis = 1; break;
    case JXL_ORIENT_ROTATE_90_CCW: geometry.rotation = 1; break;
    default: return std::unexpected{gain_map_error("主图像方向无效。")};
  }
  if (geometry.rotation & 1u) std::swap(image.width, image.height);
  image.geometry = geometry;
  return {};
}

std::expected<ImageBuffer, std::string> gain_map_image_for_encode(const avifImage& map, int threads) {
  const int output_depth = map.depth > 8 ? 16 : 8;
  const auto stride = checked_rgba_stride(map.width, "JXL Gain Map", output_depth > 8 ? 2 : 1);
  if (!stride) return std::unexpected{gain_map_error(stride.error())};
  if (*stride > UINT32_MAX) return std::unexpected{gain_map_error("辅助图像 stride 超过 API 上限。")};
  const auto count = checked_image_bytes(*stride, map.height, "JXL Gain Map");
  if (!count) return std::unexpected{gain_map_error(count.error())};
  auto pixels = decoder_common::make_byte_buffer(*count, "JXL Gain Map");
  if (!pixels) return std::unexpected{gain_map_error(pixels.error())};
  avifRGBImage rgb{};
  avifRGBImageSetDefaults(&rgb, &map);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = output_depth;
  rgb.ignoreAlpha = AVIF_TRUE;
  rgb.maxThreads = codec_thread_count(threads);
  rgb.rowBytes = static_cast<std::uint32_t>(*stride);
  rgb.pixels = reinterpret_cast<std::uint8_t*>(pixels->data());
  if (avifImageYUVToRGB(&map, &rgb) != AVIF_RESULT_OK)
    return std::unexpected{gain_map_error("辅助图像 RGB 转换失败。")};
  ImageBuffer image{.width = map.width, .height = map.height, .pixel_format = PixelFormat::rgba,
      .alpha_mode = AlphaMode::none, .bit_depth = output_depth};
  image.planes.push_back(ImagePlane{.bytes = std::move(*pixels), .stride = *stride});
  return image;
}

std::expected<ImageDimensions, std::string> finish_dimensions_probe(
    std::ifstream& input, std::uint64_t file_size, const JxlBasicInfo& info,
    const std::optional<JxlColorEncoding>& color, std::uint64_t main_icc_bytes = 0) {
  auto dimensions = decoder_common::make_image_dimensions_checked(info.xsize, info.ysize, "JXL");
  if (!dimensions) return std::unexpected{dimensions.error()};
  if (info.orientation >= JXL_ORIENT_TRANSPOSE) std::swap(dimensions->width, dimensions->height);
  dimensions->source_bit_depth = bit_depth_from_basic_info(info);
  dimensions->source_has_alpha = info.alpha_bits != 0;
  dimensions->source_icc_bytes = main_icc_bytes;
  const auto sample_bytes = info.bits_per_sample > 8 ? 2u : 1u;
  if (dimensions->pixel_count > UINT64_MAX / 4 / sample_bytes)
    return std::unexpected{"JXL 尺寸探测 RGBA 字节数溢出。"};
  dimensions->decoded_storage_bytes = dimensions->pixel_count * 4 * sample_bytes;
  if (color) {
    if (color->transfer_function == JXL_TRANSFER_FUNCTION_PQ || color->transfer_function == JXL_TRANSFER_FUNCTION_HLG)
      dimensions->source_dynamic_range = ImageDynamicRangeHint::hdr;
    else if (color->transfer_function == JXL_TRANSFER_FUNCTION_SRGB || color->transfer_function == JXL_TRANSFER_FUNCTION_709)
      dimensions->source_dynamic_range = ImageDynamicRangeHint::sdr;
  }
  auto payload = read_gain_map_box(file_size, [&](std::uint64_t offset, std::span<std::byte> output)
      -> std::expected<void, std::string> {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        output.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
      return std::unexpected{"容器文件读取范围超过 stream API 上限。"};
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!input) return std::unexpected{"容器文件读取失败或截断。"};
    return {};
  });
  if (!payload) return std::unexpected{payload.error()};
  if (*payload) {
    auto parsed = parse_gain_map_bundle(**payload);
    if (!parsed) return std::unexpected{parsed.error()};
    auto map = probe_gain_map_codestream(parsed->bundle);
    if (!map) return std::unexpected{map.error()};
    if (info.have_animation || map->xsize > info.xsize || map->ysize > info.ysize)
      return std::unexpected{gain_map_error("不支持动画 Gain Map，或辅助尺寸超过主图像。")};
    dimensions->source_dynamic_range = ImageDynamicRangeHint::hdr;
    dimensions->source_has_gain_map = true;
    dimensions->source_icc_bytes = std::max<std::uint64_t>(dimensions->source_icc_bytes, parsed->metadata->altICC.size);
    dimensions->gain_map_width = map->xsize;
    dimensions->gain_map_height = map->ysize;
    if (info.orientation >= JXL_ORIENT_TRANSPOSE)
      std::swap(dimensions->gain_map_width, dimensions->gain_map_height);
    const auto map_pixels = static_cast<std::uint64_t>(map->xsize) * map->ysize;
    const auto map_bytes = map_pixels * 4 * (map->bits_per_sample > 8 ? 2u : 1u);
    // Admission must cover either requested rendition and preserve_both, not
    // mistake the narrow base's depth for the reconstructed HDR working set.
    dimensions->decoded_storage_bytes = dimensions->pixel_count > (UINT64_MAX - map_bytes) / 16
        ? UINT64_MAX : dimensions->pixel_count * 16 + map_bytes;
  }
  return dimensions;
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
      if (JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
          JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING) != JXL_DEC_SUCCESS) {
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

      std::optional<JxlBasicInfo> basic_info;
      std::optional<JxlColorEncoding> color;
      std::uint64_t main_icc_bytes{};
      auto finish = [&]() -> std::expected<ImageDimensions, std::string> {
        if (!basic_info) return std::unexpected{"JXL 未返回基础信息。"};
        return jxl_detail::finish_dimensions_probe(input, file_size, *basic_info, color, main_icc_bytes);
      };
      while (true) {
        const auto status = JxlDecoderProcessInput(decoder.get());
        if (status == JXL_DEC_ERROR || status == JXL_DEC_SUCCESS) {
          // A dimensions probe does not validate image pixels; retain a valid
          // basic header even if a large/unsupported color profile is unavailable.
          if (basic_info) return finish();
          return std::unexpected{std::format("JXL 读取尺寸失败: {}", display_path_for_user(path))};
        }
        if (status == JXL_DEC_NEED_MORE_INPUT) {
          if (basic_info && (input_closed || probe_input.size() >= encoding_defaults::jxl_max_basic_info_probe_bytes))
            return finish();
          if (input_closed) {
            return std::unexpected{std::format("JXL 输入不完整: {}", display_path_for_user(path))};
          }
          provided = jxl_detail::provide_basic_info_probe_input(
              decoder.get(), input, probe_input, input_closed, path);
          if (!provided) return std::unexpected{provided.error()};
          continue;
        }
        if (status == JXL_DEC_BASIC_INFO) {
          JxlBasicInfo info{};
          if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS) {
            return std::unexpected{std::format("JXL 基础信息无效: {}", display_path_for_user(path))};
          }
          basic_info = info;
        }
        if (status == JXL_DEC_COLOR_ENCODING) {
          JxlColorEncoding profile{};
          if (JxlDecoderGetColorAsEncodedProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &profile) == JXL_DEC_SUCCESS)
            color = profile;
          std::size_t profile_bytes{};
          if (JxlDecoderGetICCProfileSize(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &profile_bytes) == JXL_DEC_SUCCESS)
            main_icc_bytes = profile_bytes;
          return finish();
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
                        options.copy_metadata_payloads.value_or(false), options.gain_map_mode, 0, options.diagnostics, options.stop_token);
  }

  std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const override {
    return decode(path, DecodeOptions{});
  }

  std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path, DecodeOptions options) const override {
    try {
      if (options.stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
      auto bytes = jxl_detail::read_file_bytes(path);
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      return decode_bytes(*bytes, display_path_for_user(path), decode_threads_,
                          options.copy_metadata_payloads.value_or(true), options.gain_map_mode, 0, options.diagnostics, options.stop_token);
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
      bool copy_metadata_payloads,
      GainMapDecodeMode gain_map_mode = GainMapDecodeMode::hdr_only,
      unsigned gain_map_depth = 0, DecodeDiagnostics* diagnostics = nullptr, std::stop_token stop_token = {}) {
    bool has_gain_map = false;
    auto decoded = decode_bytes_impl(bytes, source_name, decode_threads, copy_metadata_payloads,
                                     gain_map_mode, gain_map_depth, has_gain_map, diagnostics, stop_token);
    if (!decoded && has_gain_map)
      return std::unexpected{jxl_detail::gain_map_error(decoded.error())};
    return decoded;
  }

  static std::expected<ImageDecodeResult, std::string> decode_bytes_impl(
      std::span<const std::byte> bytes, std::string_view source_name, int decode_threads,
      bool copy_metadata_payloads, GainMapDecodeMode gain_map_mode,
      unsigned gain_map_depth, bool& has_gain_map, DecodeDiagnostics* diagnostics, std::stop_token stop_token) {
    try {
      if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
      if (gain_map_depth > 1)
        return std::unexpected{jxl_detail::gain_map_error("辅助解码递归深度超过 1。")};
      if (bytes.empty()) {
        return std::unexpected{std::format("JXL 输入为空: {}", source_name)};
      }
      if (bytes.size() > encoding_defaults::effective_max_input_file_bytes())
        return std::unexpected{"JXL 输入超过当前运行时上限。"};
      // Functional image data is independent of copy_metadata_payloads/strip.
      // This owner outlives every borrowed pointer returned by ReadBundle.
      auto gain_map_payload = jxl_detail::read_gain_map_box(bytes);
      if (!gain_map_payload) return std::unexpected{gain_map_payload.error()};
      std::optional<jxl_detail::ParsedGainMapBundle> gain_map;
      std::optional<JxlBasicInfo> gain_map_info;
      if (*gain_map_payload) {
        has_gain_map = true;
        if (gain_map_depth != 0)
          return std::unexpected{jxl_detail::gain_map_error("禁止嵌套 Gain Map。")};
        auto parsed = jxl_detail::parse_gain_map_bundle(**gain_map_payload);
        if (!parsed) return std::unexpected{parsed.error()};
        auto auxiliary_info = jxl_detail::probe_gain_map_codestream(parsed->bundle);
        if (!auxiliary_info) return std::unexpected{auxiliary_info.error()};
        gain_map_info = *auxiliary_info;
        gain_map.emplace(std::move(*parsed));
        if (gain_map->metadata->altICC.size)
          return std::unexpected{jxl_detail::gain_map_error("真实 alternate ICC 合成空间暂不支持，不能按 CICP 猜测。")};
      }
      jxl_detail::DecoderPtr decoder{JxlDecoderCreate(nullptr)};
      if (!decoder) {
        return std::unexpected{"创建 JXL decoder 失败。"};
      }
      if (has_gain_map) {
        const auto* cms = JxlGetDefaultCms();
        if (!cms || JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
            JxlDecoderSetCms(decoder.get(), *cms) != JXL_DEC_SUCCESS)
          return std::unexpected{jxl_detail::gain_map_error("无法保留主图像方向或色彩空间。")};
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
        if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
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
        if (gain_map) {
          const int threads = jxl_detail::codec_thread_count(decode_threads);
          const ResourcePlan render_resources{.file_parallelism = 1,
              .encoder_threads_per_file = threads, .global_thread_budget = threads};
          const auto& bundle = gain_map->bundle;
          auto auxiliary = decode_bytes({reinterpret_cast<const std::byte*>(bundle.gain_map), bundle.gain_map_size},
              "JXL ISO 21496-1 auxiliary", decode_threads, false, GainMapDecodeMode::hdr_only,
              gain_map_depth + 1, nullptr, stop_token);
          if (!auxiliary) return std::unexpected{jxl_detail::gain_map_error(auxiliary.error())};
          if (auto oriented = jxl_detail::apply_gain_map_orientation(image, info.orientation); !oriented)
            return std::unexpected{oriented.error()};
          if (auto oriented = jxl_detail::apply_gain_map_orientation(auxiliary->image, info.orientation); !oriented)
            return std::unexpected{oriented.error()};
          if (auxiliary->image.geometry) {
            auto normalized = convert_image_pixels(auxiliary->image, PixelConversionRequest{
                .format = PixelFormat::rgba, .bit_depth = auxiliary->image.bit_depth, .preserve_alpha = false},
                stop_token, render_resources);
            if (!normalized) return std::unexpected{jxl_detail::gain_map_error(normalized.error())};
            auxiliary->image = std::move(*normalized);
          }
          auto rendered = render_iso_gain_map(std::move(image), auxiliary->image, *gain_map->metadata,
              gain_map_mode, stop_token, render_resources, diagnostics ? &diagnostics->timing : nullptr);
          if (!rendered) return std::unexpected{jxl_detail::gain_map_error(rendered.error())};
          rendered->decoder_id = "libjxl";
          return rendered;
        }
        return ImageDecodeResult{.image = std::move(image), .decoder_id = "libjxl"};
      };

      while (true) {
        // libjxl has no stop-token API for a single ProcessInput call. Check
        // both boundaries; the subsequent Gain Map math is interruptible.
        if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
        const auto status = JxlDecoderProcessInput(decoder.get());
        if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
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
          if (gain_map && (!gain_map_info || info.have_animation ||
              gain_map_info->xsize > info.xsize || gain_map_info->ysize > info.ysize))
            return std::unexpected{jxl_detail::gain_map_error("不支持动画 Gain Map，或辅助尺寸超过主图像。")};
          if (gain_map && info.exponent_bits_per_sample != 0)
            return std::unexpected{jxl_detail::gain_map_error("浮点主 codestream 不能按 UNORM16 裁剪解码；当前仅支持整数 PQ/HLG 主图。")};
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
            if (gain_map) {
              ImageSourceInfo source_color;
              jxl_detail::apply_encoded_color_profile(source_color, profile);
              if (auto color = jxl_detail::color_encoding_from_source(&source_color, true); !color)
                return std::unexpected{jxl_detail::gain_map_error(color.error())};
              if (info.uses_original_profile != JXL_TRUE &&
                  JxlDecoderSetOutputColorProfile(decoder.get(), &profile, nullptr, 0) != JXL_DEC_SUCCESS)
                return std::unexpected{jxl_detail::gain_map_error("无法保留 XYB 主图像的原始 HDR 色彩空间。")};
            }
            encoded_color_profile = profile;
          }
          if (copy_metadata_payloads || has_gain_map) {
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
          if (gain_map_depth != 0 && JxlDecoderReleaseInput(decoder.get()) != 0)
            return std::unexpected{jxl_detail::gain_map_error("裸辅助 codestream 含尾随或嵌套数据。")};
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
                             .bit_depths = {8, 10, 12, 16}};
  }

  std::expected<NativeEncodeResult, std::string> encode_jpeg_bitstream(
      std::span<const std::byte> jpeg_bytes,
      const NativeEncodeSettings& settings,
      std::stop_token stop_token = {}) const {
    try {
      if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
      if (settings.bit_depth.value_or(16) != 8)
        return std::unexpected{"JPEG 原始码流转封装只兼容明确的 JXL 8-bit 输出计划；auto 16-bit 必须像素重编码。"};
      if (jpeg_bytes.empty()) {
        return std::unexpected{"JPEG 原始码流为空。"};
      }
      if (settings.gain_map_alternative)
        return std::unexpected{jxl_detail::gain_map_error("双呈现输入必须重新编码两端，不能只转封装 JPEG 主码流。")};
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
    auto encoded = encode_pixels(image, settings, stop_token, 0);
    if (!encoded && settings.gain_map_alternative)
      return std::unexpected{jxl_detail::gain_map_error(encoded.error())};
    return encoded;
  }

 private:
  std::expected<std::vector<std::byte>, std::string> encode_gain_map_bundle(
      const ImageBuffer& image, const NativeEncodeSettings& settings,
      std::stop_token stop_token, unsigned gain_map_depth, EncodeTimingDiagnostics& timing) const {
    if (gain_map_depth != 0 || !settings.gain_map_alternative)
      return std::unexpected{jxl_detail::gain_map_error("禁止递归编码嵌套 Gain Map。")};
    // Full-resolution gain samples avoid introducing a second spatial
    // downsampling loss in addition to the explicitly quantized 8-bit map.
    auto metadata = compute_iso_gain_map(image, *settings.gain_map_alternative,
        static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height), 8,
        stop_token, settings.resources, &timing);
    if (!metadata) return std::unexpected{jxl_detail::gain_map_error(metadata.error())};
    const auto& gain = **metadata;
    if (!gain.image) return std::unexpected{jxl_detail::gain_map_error("重新计算的辅助图像为空。")};
    auto map_image = jxl_detail::gain_map_image_for_encode(*gain.image, settings.resources.encoder_threads_per_file);
    if (!map_image) return std::unexpected{map_image.error()};
    auto auxiliary_settings = settings;
    auxiliary_settings.gain_map_alternative = nullptr;
    auxiliary_settings.jxl_rgb8_input = {};
    auxiliary_settings.strip_metadata = true;
    auxiliary_settings.applied_icc = "dropped";
    auxiliary_settings.applied_alpha = "dropped";
    auxiliary_settings.tone_map_to_sdr_pixels = false;
    auxiliary_settings.bit_depth = 8; // Auxiliary policy is independent of the primary's auto-16 plan.
    auto encoded = encode_pixels(*map_image, auxiliary_settings, stop_token, gain_map_depth + 1);
    if (!encoded) return std::unexpected{jxl_detail::gain_map_error(encoded.error())};
    auto naked = jxl_detail::naked_gain_map_codestream(std::move(encoded->encoded.bytes));
    if (!naked) return std::unexpected{naked.error()};
    const auto& codestream = *naked;
    if (codestream.empty() || codestream.size() > UINT32_MAX)
      return std::unexpected{jxl_detail::gain_map_error("辅助裸 JXL codestream 尺寸超过 API 上限。")};
    auto iso_metadata = jxl_detail::encode_iso_gain_map_metadata(gain);
    if (!iso_metadata) return std::unexpected{iso_metadata.error()};
    JxlGainMapBundle bundle{};
    bundle.jhgm_version = 0;
    bundle.gain_map_metadata_size = static_cast<std::uint16_t>(iso_metadata->size());
    bundle.gain_map_metadata = iso_metadata->data();
    bundle.gain_map_size = static_cast<std::uint32_t>(codestream.size());
    bundle.gain_map = reinterpret_cast<const std::uint8_t*>(codestream.data());
    std::vector<std::byte> compressed_icc;
    if (gain.altICC.size) {
      auto icc = jxl_detail::transcode_alt_icc({reinterpret_cast<const std::byte*>(gain.altICC.data), gain.altICC.size}, true);
      if (!icc) return std::unexpected{icc.error()};
      compressed_icc = std::move(*icc);
      if (compressed_icc.size() > UINT32_MAX)
        return std::unexpected{jxl_detail::gain_map_error("alternate ICC 超过 API 上限。")};
      bundle.alt_icc = reinterpret_cast<const std::uint8_t*>(compressed_icc.data());
      bundle.alt_icc_size = static_cast<std::uint32_t>(compressed_icc.size());
    } else {
      ImageSourceInfo alternate_color{.color_primaries = static_cast<int>(gain.altColorPrimaries),
          .transfer_characteristics = static_cast<int>(gain.altTransferCharacteristics)};
      auto color = jxl_detail::color_encoding_from_source(&alternate_color, true);
      if (!color) return std::unexpected{jxl_detail::gain_map_error(color.error())};
      bundle.has_color_encoding = JXL_TRUE;
      bundle.color_encoding = *color;
    }
    std::size_t size = 0, written = 0;
    if (JxlGainMapGetBundleSize(&bundle, &size) != JXL_TRUE || size == 0 ||
        size > encoding_defaults::codec_metadata_max_bytes)
      return std::unexpected{jxl_detail::gain_map_error("jhgm bundle 为空或超过 64 MiB 上限。")};
    auto payload = decoder_common::make_byte_buffer(size, "JXL jhgm");
    if (!payload) return std::unexpected{jxl_detail::gain_map_error(payload.error())};
    if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
    if (JxlGainMapWriteBundle(&bundle, reinterpret_cast<std::uint8_t*>(payload->data()), payload->size(), &written) != JXL_TRUE ||
        written != payload->size())
      return std::unexpected{jxl_detail::gain_map_error("写入标准 jhgm bundle 失败。")};
    return std::move(*payload);
  }

  std::expected<NativeEncodeResult, std::string> encode_pixels(
      const ImageBuffer& image, const NativeEncodeSettings& settings,
      std::stop_token stop_token, unsigned gain_map_depth) const {
    try {
      if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
      const int bit_depth = settings.bit_depth.value_or(16);
      if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12 && bit_depth != 16)
        return std::unexpected{"JXL bit-depth 只支持 8、10、12 或 16。"};
      if (gain_map_depth > 1 || (gain_map_depth != 0 && settings.gain_map_alternative))
        return std::unexpected{jxl_detail::gain_map_error("辅助编码递归深度超过 1。")};
      if (settings.gain_map_alternative && settings.tone_map_to_sdr_pixels)
        return std::unexpected{jxl_detail::gain_map_error("双呈现 JXL 不能将主 HDR 图像默默改为 SDR。")};
      if (image.geometry || image.pixel_format != PixelFormat::rgba ||
          (image.bit_depth != 8 && image.bit_depth != 16) ||
          image.sample_representation == SampleRepresentation::ieee_half_float || settings.tone_map_to_sdr_pixels) {
        auto converted = convert_image_pixels(image, PixelConversionRequest{.format = PixelFormat::rgba,
            .bit_depth = image.bit_depth > 8 ? 16 : 8, .preserve_alpha = settings.applied_alpha == "kept",
            .tone_map_to_sdr = settings.tone_map_to_sdr_pixels}, stop_token, settings.resources);
        if (!converted) return std::unexpected{converted.error()};
        auto normalized = settings;
        normalized.tone_map_to_sdr_pixels = false;
        normalized.jxl_rgb8_input = {};
        return encode_pixels(*converted, normalized, stop_token, gain_map_depth);
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
      std::optional<std::vector<std::byte>> gain_map_payload;
      EncodeTimingDiagnostics gain_map_timing{};
      if (settings.gain_map_alternative) {
        auto payload = encode_gain_map_bundle(image, settings, stop_token, gain_map_depth, gain_map_timing);
        if (!payload) return std::unexpected{payload.error()};
        gain_map_payload = std::move(*payload);
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

      const bool encode_metadata_boxes = gain_map_payload.has_value() ||
                                         jxl_detail::has_standard_metadata_boxes(image, settings);
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
      info.bits_per_sample = static_cast<std::uint32_t>(bit_depth);
      info.num_color_channels = 3;
      if (keep_alpha) {
        info.num_extra_channels = 1;
        info.alpha_bits = static_cast<std::uint32_t>(bit_depth);
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

      // libjxl 0.12.0 encode.h/types.h: BasicInfo is the codestream depth;
      // FROM_PIXEL_FORMAT describes the *input* scale. RGBA16/RGB16 samples
      // remain full-range 0..65535 for 10/12-bit output (not unscaled 0..1023/
      // 4095). libjxl performs the real rescaling/quantization, including alpha.
      const JxlBitDepth input_bit_depth{.type = JXL_BIT_DEPTH_FROM_PIXEL_FORMAT};
      if (JxlEncoderSetFrameBitDepth(frame_settings, &input_bit_depth) != JXL_ENC_SUCCESS)
        return std::unexpected{"设置 JXL 输入像素位深标尺失败。"};

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
        if (gain_map_payload) {
          if (auto added = jxl_detail::add_metadata_box(encoder.get(), jxl_detail::gain_map_box_type, *gain_map_payload, "jhgm"); !added)
            return std::unexpected{jxl_detail::gain_map_error(added.error())};
        }
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      JxlEncoderCloseInput(encoder.get());

      auto output = jxl_detail::collect_encoder_output(
          encoder.get(), encoder_input_size_hint, stop_token,
          gain_map_depth != 0 ? encoding_defaults::codec_metadata_max_bytes : 0);
      if (!output) {
        return std::unexpected{output.error()};
      }

      auto diagnostics = diagnostics_from_settings(settings);
      diagnostics.encoder_id = "libjxl";
      diagnostics.applied_bit_depth = bit_depth;
      if (gain_map_payload) {
        diagnostics.integration_mode = "jxl-iso21496-1-jhgm";
        diagnostics.rendition.gain_map_generated = true;
        diagnostics.rendition.gain_map_width = static_cast<std::uint32_t>(image.width);
        diagnostics.rendition.gain_map_height = static_cast<std::uint32_t>(image.height);
        diagnostics.rendition.gain_map_bit_depth = 8;
        diagnostics.timing.gain_map_compute_seconds = gain_map_timing.gain_map_compute_seconds;
        diagnostics.timing.gain_map_compute_workers = gain_map_timing.gain_map_compute_workers;
      }
      diagnostics.speed_mapping = speed_mapping;
      diagnostics.encoder_threads = encoder_threads;
      diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;
      return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(*output),
                                                        .codec_name = "libjxl"},
                                .diagnostics = std::move(diagnostics),
                                .final_quality = final_quality,
                                // A lossless primary coding mode cannot undo precision
                                // reduction, and a regenerated 8-bit map cannot promise
                                // both original endpoints sample-for-sample.
                                .lossless = lossless && bit_depth >= image.bit_depth && !gain_map_payload,
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
