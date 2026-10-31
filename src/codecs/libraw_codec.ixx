module;
#include "../compat/stdlib_compat.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <libraw/libraw.h>

export module awj.libraw_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace libraw_detail {

struct ProcessedImageCleanup {
  libraw_processed_image_t* image{};
  ~ProcessedImageCleanup() {
    if (image != nullptr) {
      LibRaw::dcraw_clear_mem(image);
    }
  }
};

int open_file(LibRaw& raw, const fs::path& path) {
#ifdef _WIN32
  return raw.open_file(path.native().c_str());
#else
  return raw.open_file(path.string().c_str());
#endif
}

awj::compat::expected<void, std::string> check_input_file_size(const fs::path& path) {
  std::error_code ec;
  const auto file_size = fs::file_size(path, ec);
  if (ec) {
    return awj::compat::unexpected{awj::compat::format("读取 RAW 文件大小失败: {}；系统错误：{}",
                                       display_path_for_user(path), ec.message())};
  }
  if (file_size > static_cast<std::uintmax_t>(encoding_defaults::effective_max_input_file_bytes())) {
    return awj::compat::unexpected{awj::compat::format("RAW 文件超过当前输入上限: {}",
                                       display_path_for_user(path))};
  }
  return {};
}

awj::compat::expected<void, std::string> reject_multiple_raw_images(
    const LibRaw& raw,
    std::string_view source_name) {
  if (raw.imgdata.idata.raw_count > 1) {
    return awj::compat::unexpected{awj::compat::format("暂不支持多图 RAW 输入: {}", source_name)};
  }
  return {};
}

}  // namespace libraw_detail

class LibRawImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libraw"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {
        L".dng", L".cr2", L".cr3", L".nef", L".arw", L".rw2", L".orf",
        L".raf", L".pef", L".srw", L".x3f", L".3fr", L".erf", L".kdc",
        L".mrw", L".raw"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  awj::compat::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      if (auto file_size = libraw_detail::check_input_file_size(path); !file_size) {
        return awj::compat::unexpected{file_size.error()};
      }
      LibRaw raw;
      const auto opened = libraw_detail::open_file(raw, path);
      if (opened != LIBRAW_SUCCESS) {
        return awj::compat::unexpected{awj::compat::format("LibRaw 打开失败: {}", libraw_strerror(opened))};
      }
      if (auto images = libraw_detail::reject_multiple_raw_images(raw, display_path_for_user(path));
          !images) {
        return awj::compat::unexpected{images.error()};
      }
      const auto& sizes = raw.imgdata.sizes;
      const auto width = sizes.width > 0 ? sizes.width : sizes.raw_width;
      const auto height = sizes.height > 0 ? sizes.height : sizes.raw_height;
      return decoder_common::make_image_dimensions_checked(static_cast<std::uint64_t>(width),
                                                           static_cast<std::uint64_t>(height),
                                                           "LibRaw");
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"LibRaw 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"LibRaw 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"LibRaw 尺寸探测文件系统访问失败。"};
    }
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      if (auto file_size = libraw_detail::check_input_file_size(path); !file_size) {
        return awj::compat::unexpected{file_size.error()};
      }
      LibRaw raw;
      const auto opened = libraw_detail::open_file(raw, path);
      if (opened != LIBRAW_SUCCESS) {
        return awj::compat::unexpected{awj::compat::format("LibRaw 打开失败: {}", libraw_strerror(opened))};
      }
      if (auto images = libraw_detail::reject_multiple_raw_images(raw, display_path_for_user(path));
          !images) {
        return awj::compat::unexpected{images.error()};
      }
      const auto unpacked = raw.unpack();
      if (unpacked != LIBRAW_SUCCESS) {
        return awj::compat::unexpected{awj::compat::format("LibRaw 解包失败: {}", libraw_strerror(unpacked))};
      }
      raw.imgdata.params.output_bps = 8;
      raw.imgdata.params.output_color = 1;
      raw.imgdata.params.no_auto_bright = 1;
      raw.imgdata.params.use_camera_wb = 1;
      const auto processed = raw.dcraw_process();
      if (processed != LIBRAW_SUCCESS) {
        return awj::compat::unexpected{awj::compat::format("LibRaw 处理失败: {}", libraw_strerror(processed))};
      }

      int process_warnings = 0;
      libraw_processed_image_t* processed_image = raw.dcraw_make_mem_image(&process_warnings);
      libraw_detail::ProcessedImageCleanup cleanup{.image = processed_image};
      if (processed_image == nullptr) {
        return awj::compat::unexpected{"LibRaw 未产生图像输出。"};
      }

      if (processed_image->type != LIBRAW_IMAGE_BITMAP || processed_image->colors < 3 || processed_image->bits != 8 ||
          processed_image->width <= 0 || processed_image->height <= 0) {
        return awj::compat::unexpected{"LibRaw 输出格式无效。"};
      }

      const auto width = static_cast<std::size_t>(processed_image->width);
      const auto height = static_cast<std::size_t>(processed_image->height);
      const auto stride = decoder_common::checked_rgba_stride(width, "LibRaw decoder");
      if (!stride) {
        return awj::compat::unexpected{stride.error()};
      }
      const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "LibRaw decoder");
      if (!byte_count) {
        return awj::compat::unexpected{byte_count.error()};
      }

      auto rgba = decoder_common::make_byte_buffer(*byte_count, "LibRaw decoder");
      if (!rgba) {
        return awj::compat::unexpected{rgba.error()};
      }
      const auto channels = static_cast<std::size_t>(processed_image->colors);
      if (height != 0 && width > std::numeric_limits<std::size_t>::max() / height) {
        return awj::compat::unexpected{"LibRaw 输出像素数溢出。"};
      }
      const auto pixel_count = width * height;
      if (channels != 0 && pixel_count > std::numeric_limits<std::size_t>::max() / channels) {
        return awj::compat::unexpected{"LibRaw 输出数据大小溢出。"};
      }
      const auto required_source_bytes = pixel_count * channels;
      if (processed_image->data_size < required_source_bytes) {
        return awj::compat::unexpected{"LibRaw 输出数据短于声明尺寸。"};
      }
      const auto* source = processed_image->data;
      for (std::size_t i = 0; i < pixel_count; ++i) {
        (*rgba)[i * 4 + 0] = static_cast<std::byte>(source[i * channels + 0]);
        (*rgba)[i * 4 + 1] = static_cast<std::byte>(source[i * channels + 1]);
        (*rgba)[i * 4 + 2] = static_cast<std::byte>(source[i * channels + 2]);
        (*rgba)[i * 4 + 3] = std::byte{0xff};
      }

      auto image = decoder_common::make_rgba_image(width, height, std::move(*rgba),
                                                   AlphaMode::none, "LibRaw decoder");
      if (!image) {
        return awj::compat::unexpected{image.error()};
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libraw"};
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"LibRaw 解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"LibRaw 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"LibRaw 解码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
