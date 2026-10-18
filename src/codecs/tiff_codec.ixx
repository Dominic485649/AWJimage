module;

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <tiffio.h>

export module awj.tiff_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace tiff_detail {

struct TiffDeleter {
  void operator()(TIFF* value) const noexcept {
    if (value != nullptr) {
      TIFFClose(value);
    }
  }
};

using TiffPtr = std::unique_ptr<TIFF, TiffDeleter>;

TiffPtr open_read(const fs::path& path) {
#ifdef _WIN32
  return TiffPtr{TIFFOpenW(path.native().c_str(), "r")};
#else
  const auto narrow_path = path.string();
  return TiffPtr{TIFFOpen(narrow_path.c_str(), "r")};
#endif
}

std::expected<void, std::string> check_input_file_size(const fs::path& path) {
  std::error_code ec;
  const auto file_size = fs::file_size(path, ec);
  if (ec) {
    return std::unexpected{std::format("读取 TIFF 文件大小失败: {}；系统错误：{}",
                                       display_path_for_user(path), ec.message())};
  }
  if (file_size > static_cast<std::uintmax_t>(encoding_defaults::effective_max_input_file_bytes())) {
    return std::unexpected{std::format("TIFF 文件超过当前输入上限: {}",
                                       display_path_for_user(path))};
  }
  return {};
}

std::expected<void, std::string> check_decoded_rgba_size(std::size_t byte_count) {
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{"TIFF 解码 RGBA buffer 超过当前运行时上限。"};
  }
  return {};
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

std::expected<std::vector<std::byte>, std::string> copy_icc_profile(TIFF* tiff) {
  std::uint32_t profile_size = 0;
  void* profile = nullptr;
  if (TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &profile_size, &profile) != 1 ||
      profile == nullptr || profile_size == 0) {
    return std::vector<std::byte>{};
  }
  return copy_metadata_payload(profile, profile_size, "TIFF ICC profile");
}

std::expected<std::vector<std::byte>, std::string> copy_xmp_metadata(TIFF* tiff) {
  std::uint32_t xmp_size = 0;
  void* xmp = nullptr;
  if (TIFFGetField(tiff, TIFFTAG_XMLPACKET, &xmp_size, &xmp) != 1 ||
      xmp == nullptr || xmp_size == 0) {
    return std::vector<std::byte>{};
  }
  return copy_metadata_payload(xmp, xmp_size, "TIFF XMP metadata");
}

PixelFormat ycbcr_pixel_format_for_tiff(TIFF* tiff) noexcept {
  std::uint16_t horizontal = 0;
  std::uint16_t vertical = 0;
  TIFFGetFieldDefaulted(tiff, TIFFTAG_YCBCRSUBSAMPLING, &horizontal, &vertical);
  if (horizontal == 1 && vertical == 1) {
    return PixelFormat::yuv444;
  }
  if (horizontal == 2 && vertical == 1) {
    return PixelFormat::yuv422;
  }
  if (horizontal == 2 && vertical == 2) {
    return PixelFormat::yuv420;
  }
  return PixelFormat::unknown;
}

PixelFormat source_pixel_format_for_tiff(TIFF* tiff,
                                         std::uint16_t photometric,
                                         bool source_has_alpha) noexcept {
  switch (photometric) {
    case PHOTOMETRIC_MINISWHITE:
    case PHOTOMETRIC_MINISBLACK:
      return source_has_alpha ? PixelFormat::rgba : PixelFormat::gray;
    case PHOTOMETRIC_RGB:
    case PHOTOMETRIC_PALETTE:
      return source_has_alpha ? PixelFormat::rgba : PixelFormat::rgb;
    case PHOTOMETRIC_YCBCR:
      return ycbcr_pixel_format_for_tiff(tiff);
    default:
      return PixelFormat::unknown;
  }
}

ImageSourceInfo source_info_from_directory(TIFF* tiff, bool source_has_alpha) noexcept {
  std::uint16_t bits_per_sample = 0;
  std::uint16_t photometric = 0;
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits_per_sample);
  auto pixel_format = PixelFormat::unknown;
  if (TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric) == 1) {
    pixel_format = source_pixel_format_for_tiff(tiff, photometric, source_has_alpha);
  }
  return ImageSourceInfo{.pixel_format = pixel_format,
                         .bit_depth = static_cast<int>(bits_per_sample)};
}

}  // namespace tiff_detail

class TiffImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libtiff"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".tif", L".tiff"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      if (auto file_size = tiff_detail::check_input_file_size(path); !file_size) {
        return std::unexpected{file_size.error()};
      }
      tiff_detail::TiffPtr tiff{tiff_detail::open_read(path)};
      if (!tiff) {
        return std::unexpected{std::format("打开 TIFF 失败: {}", display_path_for_user(path))};
      }
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      if (TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width) != 1 ||
          TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height) != 1) {
        return std::unexpected{std::format("TIFF 尺寸无效: {}", display_path_for_user(path))};
      }
      return decoder_common::make_image_dimensions_checked(width, height, "TIFF");
    } catch (const std::bad_alloc&) {
      return std::unexpected{"TIFF 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"TIFF 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"TIFF 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      if (auto file_size = tiff_detail::check_input_file_size(path); !file_size) {
        return std::unexpected{file_size.error()};
      }
      tiff_detail::TiffPtr tiff{tiff_detail::open_read(path)};
      if (!tiff) {
        return std::unexpected{std::format("打开 TIFF 失败: {}", display_path_for_user(path))};
      }
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      if (TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width) != 1 ||
          TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height) != 1 ||
          width == 0 || height == 0) {
        return std::unexpected{std::format("TIFF 尺寸无效: {}", display_path_for_user(path))};
      }

      bool source_has_alpha = false;
      std::uint16_t extra_sample_count = 0;
      std::uint16_t* extra_samples = nullptr;
      if (TIFFGetField(tiff.get(), TIFFTAG_EXTRASAMPLES, &extra_sample_count,
                       &extra_samples) == 1 && extra_samples != nullptr) {
        for (std::uint16_t i = 0; i < extra_sample_count; ++i) {
          if (extra_samples[i] == EXTRASAMPLE_ASSOCALPHA ||
              extra_samples[i] == EXTRASAMPLE_UNASSALPHA) {
            source_has_alpha = true;
            break;
          }
        }
      }
      const auto source_info = tiff_detail::source_info_from_directory(tiff.get(), source_has_alpha);

      const auto stride = decoder_common::checked_rgba_stride(width, "TIFF decoder");
      if (!stride) {
        return std::unexpected{stride.error()};
      }
      const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "TIFF decoder");
      if (!byte_count) {
        return std::unexpected{byte_count.error()};
      }

      if (auto decoded_size = tiff_detail::check_decoded_rgba_size(*byte_count); !decoded_size) {
        return std::unexpected{decoded_size.error()};
      }

      auto rgba = decoder_common::make_byte_buffer(*byte_count, "TIFF decoder");
      if (!rgba) {
        return std::unexpected{rgba.error()};
      }
      if constexpr (std::endian::native == std::endian::little) {
        if (TIFFReadRGBAImageOriented(tiff.get(), width, height,
                                      reinterpret_cast<std::uint32_t*>(rgba->data()),
                                      ORIENTATION_TOPLEFT, 0) == 0) {
          return std::unexpected{std::format("TIFF 解码失败: {}", display_path_for_user(path))};
        }
      } else {
        const auto pixel_count = *byte_count / sizeof(std::uint32_t);
        std::vector<std::uint32_t> raster(pixel_count);
        if (TIFFReadRGBAImageOriented(tiff.get(), width, height, raster.data(), ORIENTATION_TOPLEFT, 0) == 0) {
          return std::unexpected{std::format("TIFF 解码失败: {}", display_path_for_user(path))};
        }
        for (std::size_t i = 0; i < pixel_count; ++i) {
          const auto pixel = raster[i];
          (*rgba)[i * 4 + 0] = static_cast<std::byte>(TIFFGetR(pixel));
          (*rgba)[i * 4 + 1] = static_cast<std::byte>(TIFFGetG(pixel));
          (*rgba)[i * 4 + 2] = static_cast<std::byte>(TIFFGetB(pixel));
          (*rgba)[i * 4 + 3] = static_cast<std::byte>(TIFFGetA(pixel));
        }
      }

      auto icc_profile = tiff_detail::copy_icc_profile(tiff.get());
      if (!icc_profile) {
        return std::unexpected{icc_profile.error()};
      }
      auto xmp_metadata = tiff_detail::copy_xmp_metadata(tiff.get());
      if (!xmp_metadata) {
        return std::unexpected{xmp_metadata.error()};
      }

      auto image = decoder_common::make_rgba_image(
          width, height, std::move(*rgba),
          source_has_alpha ? AlphaMode::straight : AlphaMode::none, "TIFF decoder",
          source_info);
      if (!image) {
        return std::unexpected{image.error()};
      }
      if (!icc_profile->empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::icc,
                                                .bytes = std::move(*icc_profile)});
      }
      if (!xmp_metadata->empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::xmp,
                                                .bytes = std::move(*xmp_metadata)});
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libtiff"};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"TIFF 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"TIFF 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"TIFF 解码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
