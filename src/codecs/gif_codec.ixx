module;
#include "../compat/stdlib_compat.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gif_lib.h>

export module awj.gif_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace gif_detail {

std::uint16_t read_le_u16(const std::vector<std::byte>& bytes,
                          std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
                                    (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8));
}

awj::compat::expected<void, std::string> skip_sub_blocks(const std::vector<std::byte>& bytes,
                                                 std::size_t& offset,
                                                 std::string_view context) {
  for (;;) {
    if (offset >= bytes.size()) {
      return awj::compat::unexpected{awj::compat::format("GIF {} 数据不完整。", context)};
    }
    const auto block_size = std::to_integer<std::size_t>(bytes[offset]);
    ++offset;
    if (block_size == 0) {
      return {};
    }
    if (block_size > bytes.size() - offset) {
      return awj::compat::unexpected{awj::compat::format("GIF {} 数据不完整。", context)};
    }
    offset += block_size;
  }
}

awj::compat::expected<std::byte, std::string> read_stream_byte(std::ifstream& input,
                                                       std::string_view context) {
  char value{};
  input.read(&value, 1);
  if (input.bad()) {
    return awj::compat::unexpected{awj::compat::format("GIF {} 读取失败。", context)};
  }
  if (input.gcount() != 1) {
    return awj::compat::unexpected{awj::compat::format("GIF {} 数据不完整。", context)};
  }
  return std::byte{static_cast<unsigned char>(value)};
}

awj::compat::expected<void, std::string> skip_stream_bytes(std::ifstream& input,
                                                   std::size_t byte_count,
                                                   std::string_view context) {
  const auto count = static_cast<std::streamsize>(byte_count);
  input.ignore(count);
  if (input.bad()) {
    return awj::compat::unexpected{awj::compat::format("GIF {} 读取失败。", context)};
  }
  if (input.gcount() != count) {
    return awj::compat::unexpected{awj::compat::format("GIF {} 数据不完整。", context)};
  }
  return {};
}

awj::compat::expected<std::array<std::byte, 13>, std::string> read_stream_header(std::ifstream& input) {
  std::array<char, 13> raw{};
  input.read(raw.data(), static_cast<std::streamsize>(raw.size()));
  if (input.bad()) {
    return awj::compat::unexpected{"GIF 文件头读取失败。"};
  }
  if (input.gcount() != static_cast<std::streamsize>(raw.size())) {
    return awj::compat::unexpected{"GIF 文件头不完整。"};
  }
  std::array<std::byte, 13> header{};
  for (std::size_t index = 0; index < header.size(); ++index) {
    header[index] = std::byte{static_cast<unsigned char>(raw[index])};
  }
  return header;
}

awj::compat::expected<void, std::string> skip_sub_blocks(std::ifstream& input,
                                                 std::string_view context) {
  for (;;) {
    auto block_size = read_stream_byte(input, context);
    if (!block_size) {
      return awj::compat::unexpected{block_size.error()};
    }
    const auto size = std::to_integer<std::size_t>(*block_size);
    if (size == 0) {
      return {};
    }
    if (auto skipped = skip_stream_bytes(input, size, context); !skipped) {
      return awj::compat::unexpected{skipped.error()};
    }
  }
}

awj::compat::expected<bool, std::string> has_multiple_image_frames(const std::vector<std::byte>& bytes) {
  if (bytes.size() < 13) {
    return awj::compat::unexpected{"GIF 文件头不完整。"};
  }

  std::size_t offset = 13;
  const auto logical_screen_flags = std::to_integer<unsigned int>(bytes[10]);
  if ((logical_screen_flags & 0x80U) != 0) {
    const auto color_table_size = static_cast<std::size_t>(3) << ((logical_screen_flags & 0x07U) + 1U);
    if (color_table_size > bytes.size() - offset) {
      return awj::compat::unexpected{"GIF 全局调色板数据不完整。"};
    }
    offset += color_table_size;
  }

  int frame_count = 0;
  while (offset < bytes.size()) {
    const auto record = bytes[offset];
    ++offset;
    if (record == std::byte{0x3b}) {
      return frame_count > 1;
    }
    if (record == std::byte{0x21}) {
      if (offset >= bytes.size()) {
        return awj::compat::unexpected{"GIF 扩展块数据不完整。"};
      }
      ++offset;
      if (auto skipped = skip_sub_blocks(bytes, offset, "扩展块"); !skipped) {
        return awj::compat::unexpected{skipped.error()};
      }
      continue;
    }
    if (record != std::byte{0x2c}) {
      return awj::compat::unexpected{"GIF 记录类型无效。"};
    }

    if (bytes.size() - offset < 9) {
      return awj::compat::unexpected{"GIF 图像描述数据不完整。"};
    }
    const auto image_flags = std::to_integer<unsigned int>(bytes[offset + 8]);
    offset += 9;
    if ((image_flags & 0x80U) != 0) {
      const auto color_table_size = static_cast<std::size_t>(3) << ((image_flags & 0x07U) + 1U);
      if (color_table_size > bytes.size() - offset) {
        return awj::compat::unexpected{"GIF 局部调色板数据不完整。"};
      }
      offset += color_table_size;
    }
    if (offset >= bytes.size()) {
      return awj::compat::unexpected{"GIF 图像数据不完整。"};
    }
    ++offset;
    if (auto skipped = skip_sub_blocks(bytes, offset, "图像"); !skipped) {
      return awj::compat::unexpected{skipped.error()};
    }
    ++frame_count;
    if (frame_count > 1) {
      return true;
    }
  }

  return awj::compat::unexpected{"GIF 文件缺少结束标记。"};
}

awj::compat::expected<bool, std::string> has_multiple_image_frames(const fs::path& path) {
  std::error_code ec;
  const auto file_size = fs::file_size(path, ec);
  if (ec) {
    return awj::compat::unexpected{awj::compat::format("读取 GIF 文件大小失败: {}；系统错误：{}",
                                       display_path_for_user(path), ec.message())};
  }
  if (file_size > static_cast<std::uintmax_t>(encoding_defaults::effective_max_input_file_bytes())) {
    return awj::compat::unexpected{awj::compat::format("GIF 文件超过当前输入上限: {}",
                                       display_path_for_user(path))};
  }

  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return awj::compat::unexpected{"无法读取 GIF 文件"};
  }

  auto header = read_stream_header(input);
  if (!header) {
    return awj::compat::unexpected{header.error()};
  }
  const auto logical_screen_flags = std::to_integer<unsigned int>((*header)[10]);
  if ((logical_screen_flags & 0x80U) != 0) {
    const auto color_table_size = static_cast<std::size_t>(3) << ((logical_screen_flags & 0x07U) + 1U);
    if (auto skipped = skip_stream_bytes(input, color_table_size, "全局调色板"); !skipped) {
      return awj::compat::unexpected{skipped.error()};
    }
  }

  int frame_count = 0;
  for (;;) {
    const auto next = input.peek();
    if (next == std::char_traits<char>::eof()) {
      if (input.bad()) {
        return awj::compat::unexpected{"GIF 记录读取失败。"};
      }
      return awj::compat::unexpected{"GIF 文件缺少结束标记。"};
    }
    auto record = read_stream_byte(input, "记录");
    if (!record) {
      return awj::compat::unexpected{record.error()};
    }
    if (*record == std::byte{0x3b}) {
      return frame_count > 1;
    }
    if (*record == std::byte{0x21}) {
      if (auto skipped_label = read_stream_byte(input, "扩展块"); !skipped_label) {
        return awj::compat::unexpected{skipped_label.error()};
      }
      if (auto skipped = skip_sub_blocks(input, "扩展块"); !skipped) {
        return awj::compat::unexpected{skipped.error()};
      }
      continue;
    }
    if (*record != std::byte{0x2c}) {
      return awj::compat::unexpected{"GIF 记录类型无效。"};
    }

    if (auto skipped = skip_stream_bytes(input, 8, "图像描述"); !skipped) {
      return awj::compat::unexpected{skipped.error()};
    }
    auto image_flags = read_stream_byte(input, "图像描述");
    if (!image_flags) {
      return awj::compat::unexpected{image_flags.error()};
    }
    if ((*image_flags & std::byte{0x80}) != std::byte{}) {
      const auto color_table_size = static_cast<std::size_t>(3) << ((std::to_integer<unsigned int>(*image_flags) & 0x07U) + 1U);
      if (auto skipped = skip_stream_bytes(input, color_table_size, "局部调色板"); !skipped) {
        return awj::compat::unexpected{skipped.error()};
      }
    }
    if (auto skipped_lzw_size = read_stream_byte(input, "图像数据"); !skipped_lzw_size) {
      return awj::compat::unexpected{skipped_lzw_size.error()};
    }
    if (auto skipped = skip_sub_blocks(input, "图像"); !skipped) {
      return awj::compat::unexpected{skipped.error()};
    }
    ++frame_count;
    if (frame_count > 1) {
      return true;
    }
  }
}

struct GifDeleter {
  void operator()(GifFileType* value) const noexcept {
    if (value != nullptr) {
      int error = 0;
      DGifCloseFile(value, &error);
    }
  }
};

using GifPtr = std::unique_ptr<GifFileType, GifDeleter>;

struct ReadState {
  const std::byte* data{};
  std::size_t size{};
  std::size_t offset{};
};

int read_callback(GifFileType* gif, GifByteType* out, int count) {
  auto* state = static_cast<ReadState*>(gif->UserData);
  if (state == nullptr || count < 0) {
    return 0;
  }
  if (state->offset >= state->size) {
    return 0;
  }
  const auto requested = static_cast<std::size_t>(count);
  const auto available = state->size - state->offset;
  const auto copied = std::min(requested, available);
  awj::compat::ranges::copy_n(reinterpret_cast<const GifByteType*>(state->data + state->offset),
                      copied, out);
  state->offset += copied;
  return static_cast<int>(copied);
}

ColorMapObject* active_color_map(const GifFileType* gif, const GifImageDesc& image) noexcept {
  return image.ColorMap != nullptr ? image.ColorMap : gif->SColorMap;
}

struct FirstFrame {
  GifImageDesc image{};
  std::vector<GifByteType> raster{};
  std::optional<unsigned char> transparent_color_index{};
};

awj::compat::expected<std::optional<unsigned char>, std::string> skip_extension(GifFileType* gif) {
  int extension_code = 0;
  GifByteType* extension = nullptr;
  if (DGifGetExtension(gif, &extension_code, &extension) != GIF_OK) {
    return awj::compat::unexpected{awj::compat::format("GIF 扩展块读取失败，错误码: {}", gif->Error)};
  }

  std::optional<unsigned char> transparent_color_index;
  while (extension != nullptr) {
    if (extension_code == GRAPHICS_EXT_FUNC_CODE && extension[0] >= 4 &&
        (extension[1] & 0x01) != 0) {
      transparent_color_index = static_cast<unsigned char>(extension[4]);
    }
    if (DGifGetExtensionNext(gif, &extension) != GIF_OK) {
      return awj::compat::unexpected{awj::compat::format("GIF 扩展块读取失败，错误码: {}", gif->Error)};
    }
  }
  return transparent_color_index;
}

awj::compat::expected<FirstFrame, std::string> read_first_frame(GifFileType* gif) {
  std::optional<unsigned char> transparent_color_index;

  for (;;) {
    GifRecordType record_type = UNDEFINED_RECORD_TYPE;
    if (DGifGetRecordType(gif, &record_type) != GIF_OK) {
      return awj::compat::unexpected{awj::compat::format("GIF 记录读取失败，错误码: {}", gif->Error)};
    }

    if (record_type == TERMINATE_RECORD_TYPE) {
      return awj::compat::unexpected{"GIF 不包含图像帧。"};
    }
    if (record_type == EXTENSION_RECORD_TYPE) {
      auto skipped = skip_extension(gif);
      if (!skipped) {
        return awj::compat::unexpected{skipped.error()};
      }
      if (*skipped) {
        transparent_color_index = *skipped;
      }
      continue;
    }
    if (record_type != IMAGE_DESC_RECORD_TYPE) {
      return awj::compat::unexpected{"GIF 记录类型无效。"};
    }

    if (DGifGetImageDesc(gif) != GIF_OK) {
      return awj::compat::unexpected{awj::compat::format("GIF 图像描述读取失败，错误码: {}", gif->Error)};
    }

    FirstFrame frame{.image = gif->Image, .transparent_color_index = transparent_color_index};
    if (frame.image.Width <= 0 || frame.image.Height <= 0) {
      return awj::compat::unexpected{"GIF 首帧尺寸无效。"};
    }

    const auto frame_width = static_cast<std::size_t>(frame.image.Width);
    const auto frame_height = static_cast<std::size_t>(frame.image.Height);
    const auto pixel_count = decoder_common::checked_image_bytes(frame_width, frame_height,
                                                                 "GIF decoder");
    if (!pixel_count) {
      return awj::compat::unexpected{pixel_count.error()};
    }
    try {
      frame.raster.resize(*pixel_count);
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"GIF decoder raster 内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"GIF decoder raster 尺寸超过运行时限制。"};
    }

    if (frame.image.Interlace) {
      static constexpr int offsets[] = {0, 4, 2, 1};
      static constexpr int jumps[] = {8, 8, 4, 2};
      for (int pass = 0; pass < 4; ++pass) {
        for (int y = offsets[pass]; y < frame.image.Height; y += jumps[pass]) {
          auto* row = frame.raster.data() + static_cast<std::size_t>(y) * frame_width;
          if (DGifGetLine(gif, row, frame.image.Width) != GIF_OK) {
            return awj::compat::unexpected{awj::compat::format("GIF 像素读取失败，错误码: {}", gif->Error)};
          }
        }
      }
    } else {
      for (int y = 0; y < frame.image.Height; ++y) {
        auto* row = frame.raster.data() + static_cast<std::size_t>(y) * frame_width;
        if (DGifGetLine(gif, row, frame.image.Width) != GIF_OK) {
          return awj::compat::unexpected{awj::compat::format("GIF 像素读取失败，错误码: {}", gif->Error)};
        }
      }
    }

    return frame;
  }
}

ImageSourceInfo source_info_from_frame(const GifFileType& gif,
                                      const FirstFrame& frame) noexcept {
  const auto* colors = active_color_map(&gif, frame.image);
  return ImageSourceInfo{.pixel_format = PixelFormat::rgb,
                         .bit_depth = colors != nullptr ? colors->BitsPerPixel : 0};
}

}  // namespace gif_detail

class GifImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "giflib"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".gif"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  awj::compat::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_prefix(path, 10, "GIF");
      if (!bytes) {
        return awj::compat::unexpected{bytes.error()};
      }
      const bool valid_signature = bytes->size() >= 10 &&
                                   (*bytes)[0] == std::byte{'G'} &&
                                   (*bytes)[1] == std::byte{'I'} &&
                                   (*bytes)[2] == std::byte{'F'} &&
                                   (*bytes)[3] == std::byte{'8'} &&
                                   ((*bytes)[4] == std::byte{'7'} || (*bytes)[4] == std::byte{'9'}) &&
                                   (*bytes)[5] == std::byte{'a'};
      if (!valid_signature) {
        return awj::compat::unexpected{awj::compat::format("GIF 文件头无效: {}", display_path_for_user(path))};
      }
      return decoder_common::make_image_dimensions_checked(gif_detail::read_le_u16(*bytes, 6),
                                                           gif_detail::read_le_u16(*bytes, 8),
                                                           "GIF");
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"GIF 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"GIF 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"GIF 尺寸探测文件系统访问失败。"};
    }
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_bytes(path, "GIF");
      if (!bytes) {
        return awj::compat::unexpected{bytes.error()};
      }
      gif_detail::ReadState state{.data = bytes->data(), .size = bytes->size()};
      int error = 0;
      gif_detail::GifPtr gif{DGifOpen(&state, gif_detail::read_callback, &error)};
      if (!gif) {
        return awj::compat::unexpected{awj::compat::format("打开 GIF 失败，错误码: {}", error)};
      }
      auto frame = gif_detail::read_first_frame(gif.get());
      if (!frame) {
        return awj::compat::unexpected{frame.error()};
      }
      if (gif->SWidth <= 0 || gif->SHeight <= 0) {
        return awj::compat::unexpected{awj::compat::format("GIF 尺寸无效: {}", display_path_for_user(path))};
      }

      const auto width = static_cast<std::size_t>(gif->SWidth);
      const auto height = static_cast<std::size_t>(gif->SHeight);
      const auto stride = decoder_common::checked_rgba_stride(width, "GIF decoder");
      if (!stride) {
        return awj::compat::unexpected{stride.error()};
      }
      const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "GIF decoder");
      if (!byte_count) {
        return awj::compat::unexpected{byte_count.error()};
      }
      auto rgba = decoder_common::make_byte_buffer(*byte_count, "GIF decoder", std::byte{0xff});
      if (!rgba) {
        return awj::compat::unexpected{rgba.error()};
      }

      const auto* colors = gif_detail::active_color_map(gif.get(), frame->image);
      if (colors == nullptr || colors->ColorCount <= 0) {
        return awj::compat::unexpected{"GIF 缺少调色板。"};
      }
      const auto frame_width = frame->image.Width;
      const auto frame_height = frame->image.Height;
      const auto frame_left = static_cast<std::int64_t>(frame->image.Left);
      const auto frame_top = static_cast<std::int64_t>(frame->image.Top);
      const auto frame_width_64 = static_cast<std::int64_t>(frame_width);
      const auto frame_height_64 = static_cast<std::int64_t>(frame_height);
      const auto canvas_width = static_cast<std::int64_t>(gif->SWidth);
      const auto canvas_height = static_cast<std::int64_t>(gif->SHeight);
      if (frame_width <= 0 || frame_height <= 0 || frame_left < 0 || frame_top < 0 ||
          frame_left + frame_width_64 > canvas_width || frame_top + frame_height_64 > canvas_height) {
        return awj::compat::unexpected{"GIF 首帧范围无效。"};
      }

      for (int y = 0; y < frame_height; ++y) {
        const auto source_row = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame_width);
        const auto target_y = static_cast<std::size_t>(frame_top + y);
        for (int x = 0; x < frame_width; ++x) {
          const auto index = static_cast<unsigned char>(frame->raster[source_row + static_cast<std::size_t>(x)]);
          if (index >= colors->ColorCount) {
            return awj::compat::unexpected{"GIF 像素索引超出调色板范围。"};
          }
          const auto& color = colors->Colors[index];
          const auto target_x = static_cast<std::size_t>(frame_left + x);
          const auto out = target_y * *stride + target_x * 4;
          (*rgba)[out + 0] = static_cast<std::byte>(color.Red);
          (*rgba)[out + 1] = static_cast<std::byte>(color.Green);
          (*rgba)[out + 2] = static_cast<std::byte>(color.Blue);
          if (frame->transparent_color_index && index == *frame->transparent_color_index) {
            (*rgba)[out + 3] = std::byte{0x00};
          } else {
            (*rgba)[out + 3] = std::byte{0xff};
          }
        }
      }

      const auto alpha_mode = frame->transparent_color_index ? AlphaMode::straight : AlphaMode::none;
      const auto source_info = gif_detail::source_info_from_frame(*gif, *frame);
      auto image = decoder_common::make_rgba_image(width, height, std::move(*rgba),
                                                   alpha_mode,
                                                   "GIF decoder", source_info);
      if (!image) {
        return awj::compat::unexpected{image.error()};
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "giflib"};
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"GIF 解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"GIF 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"GIF 解码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
