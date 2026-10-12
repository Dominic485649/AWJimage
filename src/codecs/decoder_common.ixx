module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module awj.decoder_common;

import awj.core;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace decoder_common {

std::wstring lower_extension(const fs::path& path) {
  auto ext = path.extension().wstring();
  std::ranges::transform(ext, ext.begin(),
                         [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return ext;
}

bool extension_is_one_of(const fs::path& path, std::span<const std::wstring_view> extensions) {
  const auto ext = lower_extension(path);
  return std::ranges::any_of(extensions, [&](std::wstring_view value) { return ext == value; });
}

std::expected<std::vector<std::byte>, std::string> read_file_prefix(
    const fs::path& path,
    std::size_t byte_count,
    std::string_view codec_name) {
  try {
    if (byte_count == 0) {
      return std::unexpected{std::format("{} 前缀读取大小无效: {}", codec_name, display_path_for_user(path))};
    }
    if (byte_count > encoding_defaults::effective_max_input_file_bytes()) {
      return std::unexpected{std::format(
          "{} 前缀读取大小超过当前运行时上限: {}", codec_name, display_path_for_user(path))};
    }
    if (byte_count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
      return std::unexpected{std::format(
          "{} 前缀读取大小超过流读取 API 限制: {}", codec_name, display_path_for_user(path))};
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
      return std::unexpected{std::format("无法读取 {} 文件: {}", codec_name, display_path_for_user(path))};
    }
    std::vector<std::byte> bytes;
    try {
      bytes.resize(byte_count);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"前缀缓冲区内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"前缀缓冲区尺寸超过运行时限制。"};
    }
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto read_count = input.gcount();
    if (input.bad()) {
      return std::unexpected{std::format("读取 {} 文件前缀失败: {}", codec_name, display_path_for_user(path))};
    }
    if (read_count <= 0) {
      return std::unexpected{std::format("{} 文件为空: {}", codec_name, display_path_for_user(path))};
    }
    bytes.resize(static_cast<std::size_t>(read_count));
    return bytes;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"读取文件前缀时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"读取文件前缀时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"读取文件前缀时文件系统访问失败。"};
  }
}

std::expected<std::vector<std::byte>, std::string> read_file_bytes(
    const fs::path& path,
    std::string_view codec_name) {
  try {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
      return std::unexpected{std::format("无法读取 {} 文件: {}", codec_name, display_path_for_user(path))};
    }
    input.seekg(0, std::ios::end);
    if (!input) {
      return std::unexpected{std::format("读取 {} 文件大小失败: {}", codec_name, display_path_for_user(path))};
    }
    const auto size = input.tellg();
    if (size < 0) {
      return std::unexpected{std::format("读取 {} 文件大小失败: {}", codec_name, display_path_for_user(path))};
    }
    if (size == 0) {
      return std::unexpected{std::format("{} 文件为空: {}", codec_name, display_path_for_user(path))};
    }
    const auto file_size = static_cast<std::uint64_t>(size);
    if (file_size > encoding_defaults::effective_max_input_file_bytes()) {
      return std::unexpected{std::format(
          "{} 文件超过当前输入上限: {}", codec_name, display_path_for_user(path))};
    }
    if (file_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
      return std::unexpected{std::format(
          "{} 文件超过流读取 API 限制: {}", codec_name, display_path_for_user(path))};
    }
    input.seekg(0, std::ios::beg);
    if (!input) {
      return std::unexpected{std::format("读取 {} 文件失败: {}", codec_name, display_path_for_user(path))};
    }
    std::vector<std::byte> bytes;
    try {
      bytes.resize(static_cast<std::size_t>(file_size));
    } catch (const std::bad_alloc&) {
      return std::unexpected{"输入缓冲区内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"输入缓冲区尺寸超过运行时限制。"};
    }
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
      return std::unexpected{std::format("读取 {} 文件失败: {}", codec_name, display_path_for_user(path))};
    }
    return bytes;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"读取文件时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"读取文件时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"读取文件时文件系统访问失败。"};
  }
}

std::expected<std::size_t, std::string> checked_rgba_stride(
    std::size_t width,
    std::string_view context,
    std::size_t bytes_per_sample = 1) {
  if (bytes_per_sample == 0 || width == 0 ||
      width > std::numeric_limits<std::size_t>::max() / 4 / bytes_per_sample) {
    return std::unexpected{std::format("{} 输入宽度无效。", context)};
  }
  return width * 4 * bytes_per_sample;
}

std::expected<std::size_t, std::string> checked_image_bytes(std::size_t stride,
                                                           std::size_t height,
                                                           std::string_view context) {
  if (stride == 0 || height == 0 || height > std::numeric_limits<std::size_t>::max() / stride) {
    return std::unexpected{std::format("{} 输入尺寸过大。", context)};
  }
  const auto byte_count = stride * height;
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{std::format("{} 图像 buffer 超过当前运行时上限。", context)};
  }
  return byte_count;
}

std::expected<void, std::string> resize_buffer(std::vector<std::byte>& buffer,
                                               std::size_t byte_count,
                                               std::string_view context) {
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{std::format("{} 输出缓冲区超过当前运行时上限。", context)};
  }
  try {
    buffer.resize(byte_count);
  } catch (const std::bad_alloc&) {
    return std::unexpected{std::format("{} 输出缓冲区内存不足。", context)};
  } catch (const std::length_error&) {
    return std::unexpected{std::format("{} 输出缓冲区尺寸超过运行时限制。", context)};
  }
  return {};
}

std::expected<std::vector<std::byte>, std::string> make_byte_buffer(std::size_t byte_count,
                                                                    std::string_view context,
                                                                    std::byte value = std::byte{}) {
  std::vector<std::byte> buffer;
  auto resized = resize_buffer(buffer, byte_count, context);
  if (!resized) {
    return std::unexpected{resized.error()};
  }
  if (value != std::byte{}) {
    std::ranges::fill(buffer, value);
  }
  return buffer;
}

std::expected<ImageBuffer, std::string> make_rgba_image(std::size_t width,
                                                        std::size_t height,
                                                        std::vector<std::byte> rgba,
                                                        AlphaMode alpha_mode,
                                                         std::string_view context,
                                                         std::optional<ImageSourceInfo> source_info = {},
                                                         int bit_depth = 8,
                                                         SampleRepresentation sample_representation =
                                                             SampleRepresentation::unorm) {
  if (bit_depth != 8 && bit_depth != 16) {
    return std::unexpected{std::format("{} RGBA bit-depth 不受支持。", context)};
  }
  if (sample_representation == SampleRepresentation::ieee_half_float && bit_depth != 16) {
    return std::unexpected{std::format("{} 浮点 RGBA 必须为 16-bit。", context)};
  }
  const auto stride = checked_rgba_stride(width, context, bit_depth > 8 ? 2 : 1);
  if (!stride) {
    return std::unexpected{stride.error()};
  }
  const auto byte_count = checked_image_bytes(*stride, height, context);
  if (!byte_count) {
    return std::unexpected{byte_count.error()};
  }
  if (rgba.size() < *byte_count) {
    return std::unexpected{std::format("{} RGBA buffer 尺寸无效。", context)};
  }
  auto resized = resize_buffer(rgba, *byte_count, context);
  if (!resized) {
    return std::unexpected{resized.error()};
  }
  ImagePlane plane{.bytes = std::move(rgba), .stride = *stride};
  ImageBuffer image{.width = width,
                    .height = height,
                    .pixel_format = PixelFormat::rgba,
                    .alpha_mode = alpha_mode,
                    .bit_depth = bit_depth,
                    .sample_representation = sample_representation,
                    .source_info = std::move(source_info)};
  try {
    image.planes.push_back(std::move(plane));
  } catch (const std::bad_alloc&) {
    return std::unexpected{std::format("{} plane list 内存不足。", context)};
  } catch (const std::length_error&) {
    return std::unexpected{std::format("{} plane list 尺寸超过运行时限制。", context)};
  }
  return image;
}

std::expected<bool, std::string> has_non_opaque_alpha(const ImageBuffer& image,
                                                       std::string_view context) {
  if (image.alpha_mode == AlphaMode::none) return false;
  const bool planar = image.pixel_format == PixelFormat::yuv444 || image.pixel_format == PixelFormat::yuv422 ||
      image.pixel_format == PixelFormat::yuv420 || image.pixel_format == PixelFormat::gray;
  if (!valid_image_geometry(image) || (image.bit_depth != 8 && image.bit_depth != 10 && image.bit_depth != 12 && image.bit_depth != 16) ||
      (!planar && image.pixel_format != PixelFormat::rgba) || image.planes.size() < (planar ? 4u : 1u))
    return std::unexpected{std::format("{} alpha 检测需要有效 RGBA/YUV ImageBuffer。", context)};
  const auto size = image_storage_dimensions(image);
  const auto bytes = image.bit_depth > 8 ? 2u : 1u;
  const auto channels = planar ? 1u : 4u;
  const auto& plane = image.planes[planar ? 3 : 0];
  if (size[0] > std::numeric_limits<std::size_t>::max() / channels / bytes || plane.stride == 0 ||
      plane.stride < size[0] * channels * bytes || size[1] > std::numeric_limits<std::size_t>::max() / plane.stride ||
      plane.bytes.size() < size[1] * plane.stride)
    return std::unexpected{std::format("{} alpha buffer 尺寸无效。", context)};
  const auto opaque = image.sample_representation == SampleRepresentation::ieee_half_float ? 0x3c00u : (1u << image.bit_depth) - 1;
  for (std::size_t y = 0; y < image.height; ++y) for (std::size_t x = 0; x < image.width; ++x) {
    const auto point = image_sample_coordinates(image, x, y);
    const auto offset = point[1] * plane.stride + (point[0] * channels + (planar ? 0 : 3)) * bytes;
    std::uint16_t alpha{};
    if (bytes == 1) alpha = std::to_integer<unsigned char>(plane.bytes[offset]);
    else std::memcpy(&alpha, plane.bytes.data() + offset, 2);
    if (alpha != opaque) return true;
  }
  return false;
}

std::expected<ImageDimensions, std::string> make_image_dimensions_checked(std::uint64_t width,
                                                                            std::uint64_t height,
                                                                            std::string_view context) {
  if (width == 0 || height == 0 ||
      width > std::numeric_limits<std::uint32_t>::max() ||
      height > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{std::format("{} 尺寸无效。", context)};
  }
  if (width > std::numeric_limits<std::uint64_t>::max() / height) {
    return std::unexpected{std::format("{} 尺寸过大。", context)};
  }
  return make_image_dimensions(static_cast<std::uint32_t>(width),
                               static_cast<std::uint32_t>(height));
}

}  // namespace decoder_common

}  // namespace awj
