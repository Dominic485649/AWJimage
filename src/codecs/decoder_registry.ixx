module;

#include <algorithm>
#include <concepts>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <new>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module awj.decoder_registry;

import awj.avif_aom_codec;
import awj.bmp_codec;
import awj.codec;
import awj.config;
import awj.core;
import awj.gif_codec;
import awj.heif_codec;
import awj.image;
import awj.large_image_plan;
import awj.jpeg_codec;
#if AWJ_HAS_JPEGLI
import awj.jpegli_codec;
#endif
#if AWJ_HAS_WINDOWS_CODECS
import awj.jxr_codec;
#endif
import awj.jxl_codec;
import awj.libraw_codec;
import awj.png_codec;
#if AWJ_HAS_AWJ_RAW_CODEC
import awj.raw_codec;
#endif
import awj.tiff_codec;
import awj.webp_codec;
#if AWJ_HAS_WINDOWS_CODECS
import awj.wic_codec;
#endif

export namespace awj {

struct DecoderRegistryOptions {
  bool allow_wic_fallback{true};
  int decode_threads{1};
  std::stop_token stop_token{};
};

struct DecoderSelection {
  std::unique_ptr<ImageDecoder> decoder{};
  bool fallback{};
};

namespace decoder_registry_detail {

template <class Decoder>
bool try_select(const fs::path& path, DecoderSelection& selection, int decode_threads,
                std::stop_token stop_token = {}) {
  auto decoder = [&] {
    if constexpr (std::constructible_from<Decoder, int, std::stop_token>) {
      return std::make_unique<Decoder>(decode_threads, stop_token);
    } else if constexpr (std::constructible_from<Decoder, int>) {
      return std::make_unique<Decoder>(decode_threads);
    } else {
      return std::make_unique<Decoder>();
    }
  }();
  if (!decoder->can_decode(path)) {
    return false;
  }
  selection.decoder = std::move(decoder);
  return true;
}

bool try_select_avif(const fs::path& path, DecoderSelection& selection, int decode_threads) {
  auto decoder = make_avif_image_decoder(decode_threads);
  if (!decoder || !decoder->can_decode(path)) {
    return false;
  }
  selection.decoder = std::move(decoder);
  return true;
}

bool try_select_wic_fallback(const fs::path& path,
                             DecoderSelection& selection,
                             int decode_threads) {
#if AWJ_HAS_WINDOWS_CODECS
  DecoderSelection fallback{};
  if (!try_select<WicImageDecoder>(path, fallback, decode_threads)) {
    return false;
  }
  fallback.fallback = true;
  selection = std::move(fallback);
  return true;
#else
  (void)path;
  (void)selection;
  (void)decode_threads;
  return false;
#endif
}

std::string fallback_error(std::string_view primary_context,
                           const std::string& primary_error,
                           std::string_view fallback_context,
                           const std::string& fallback_error) {
  return std::format("{}: {}；{}失败: {}",
                     primary_context,
                     primary_error,
                     fallback_context,
                     fallback_error);
}

bool is_unsupported_multi_image_error(std::string_view error) noexcept {
  return error.starts_with("暂不支持动画 ") || error.starts_with("暂不支持多图 ") ||
         error.starts_with("暂不支持多帧 ") || error.starts_with("暂不支持多页 ");
}

bool selected_jpegli_decoder(const DecoderSelection& selection) noexcept {
  return selection.decoder != nullptr && selection.decoder->id() == "jpegli";
}

std::string with_jpeg_turbo_fallback_error(std::string primary_error,
                                           const std::string& fallback_error) {
  return std::format("{}；libjpeg-turbo 回退失败: {}",
                     std::move(primary_error),
                     fallback_error);
}

}  // namespace decoder_registry_detail

std::expected<DecoderSelection, std::string> select_decoder_for_path(
    const fs::path& path,
    DecoderRegistryOptions options) {
  try {
    DecoderSelection selection{};
    const auto decode_threads = std::max(1, options.decode_threads);
    if (decoder_registry_detail::try_select<WebPImageDecoder>(path, selection, decode_threads) ||
        decoder_registry_detail::try_select<JXLImageDecoder>(path, selection, decode_threads) ||
        decoder_registry_detail::try_select_avif(path, selection, decode_threads) ||
        decoder_registry_detail::try_select<PngImageDecoder>(path, selection, decode_threads) ||
        decoder_registry_detail::try_select<BmpImageDecoder>(path, selection, decode_threads) ||
#if AWJ_HAS_JPEGLI
        decoder_registry_detail::try_select<JpegliImageDecoder>(path, selection, decode_threads) ||
#endif
        decoder_registry_detail::try_select<JpegImageDecoder>(path, selection, decode_threads) ||
#if AWJ_HAS_WINDOWS_CODECS
        decoder_registry_detail::try_select<JxrImageDecoder>(path, selection, decode_threads) ||
#endif
        decoder_registry_detail::try_select<GifImageDecoder>(path, selection, decode_threads) ||
        decoder_registry_detail::try_select<TiffImageDecoder>(path, selection, decode_threads) ||
        decoder_registry_detail::try_select<HeifImageDecoder>(path, selection, decode_threads,
                                                               options.stop_token) ||
#if AWJ_HAS_AWJ_RAW_CODEC
        decoder_registry_detail::try_select<RawImageDecoder>(path, selection, decode_threads) ||
#endif
        decoder_registry_detail::try_select<LibRawImageDecoder>(path, selection, decode_threads)) {
      return selection;
    }

    if (options.allow_wic_fallback &&
        decoder_registry_detail::try_select_wic_fallback(path, selection, decode_threads)) {
      return selection;
    }

    const auto extension = path.extension();
    const auto extension_label = extension.empty() ? std::string{"<无扩展名>"}
                                                  : display_path_for_user(extension);
    return std::unexpected{std::format("native backend 暂不支持输入格式: {}", extension_label)};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"选择输入解码器时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"选择输入解码器时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"选择输入解码器时文件系统访问失败。"};
  }
}

std::expected<ImageDecodeResult, std::string> decode_image_for_path(
    const fs::path& path,
    DecoderRegistryOptions options) {
  try {
    auto selected = select_decoder_for_path(path, options);
    if (!selected) {
      return std::unexpected{selected.error()};
    }

    auto decoded = selected->decoder->decode(path);
    if (decoded) {
      decoded->used_fallback = decoded->used_fallback || selected->fallback;
      return decoded;
    }

    if (!options.allow_wic_fallback || selected->fallback) {
      return std::unexpected{decoded.error()};
    }

    if (decoder_registry_detail::is_unsupported_multi_image_error(decoded.error())) {
      return std::unexpected{decoded.error()};
    }

    auto primary_error = decoded.error();
    if (decoder_registry_detail::selected_jpegli_decoder(*selected)) {
      DecoderSelection jpeg_turbo{};
      const auto decode_threads = std::max(1, options.decode_threads);
      if (decoder_registry_detail::try_select<JpegImageDecoder>(path,
                                                                jpeg_turbo,
                                                                decode_threads)) {
        auto jpeg_turbo_decoded = jpeg_turbo.decoder->decode(path);
        if (jpeg_turbo_decoded) {
          jpeg_turbo_decoded->used_fallback = true;
          return jpeg_turbo_decoded;
        }
        primary_error = decoder_registry_detail::with_jpeg_turbo_fallback_error(
            std::move(primary_error), jpeg_turbo_decoded.error());
      }
    }

    DecoderSelection fallback{};
    const auto decode_threads = std::max(1, options.decode_threads);
    if (!decoder_registry_detail::try_select_wic_fallback(path, fallback, decode_threads)) {
      return std::unexpected{primary_error};
    }

    auto fallback_decoded = fallback.decoder->decode(path);
    if (!fallback_decoded) {
      return std::unexpected{decoder_registry_detail::fallback_error(
          "原生解码失败", primary_error, "WIC 兜底", fallback_decoded.error())};
    }
    fallback_decoded->used_fallback = true;
    return fallback_decoded;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"解码输入图片时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"解码输入图片时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"解码输入图片时文件系统访问失败。"};
  }
}

std::expected<ImageDimensions, std::string> probe_image_dimensions_for_path(
    const fs::path& path,
    DecoderRegistryOptions options) {
  try {
    auto selected = select_decoder_for_path(path, options);
    if (!selected) {
      return std::unexpected{selected.error()};
    }

    auto dimensions = selected->decoder->probe_dimensions(path);
    if (dimensions) {
      return dimensions;
    }

    if (!options.allow_wic_fallback || selected->fallback) {
      return std::unexpected{dimensions.error()};
    }

    if (decoder_registry_detail::is_unsupported_multi_image_error(dimensions.error())) {
      return std::unexpected{dimensions.error()};
    }

    auto primary_error = dimensions.error();
    if (decoder_registry_detail::selected_jpegli_decoder(*selected)) {
      DecoderSelection jpeg_turbo{};
      const auto decode_threads = std::max(1, options.decode_threads);
      if (decoder_registry_detail::try_select<JpegImageDecoder>(path,
                                                                jpeg_turbo,
                                                                decode_threads)) {
        auto jpeg_turbo_dimensions = jpeg_turbo.decoder->probe_dimensions(path);
        if (jpeg_turbo_dimensions) {
          return jpeg_turbo_dimensions;
        }
        primary_error = decoder_registry_detail::with_jpeg_turbo_fallback_error(
            std::move(primary_error), jpeg_turbo_dimensions.error());
      }
    }

    DecoderSelection fallback{};
    const auto decode_threads = std::max(1, options.decode_threads);
    if (!decoder_registry_detail::try_select_wic_fallback(path, fallback, decode_threads)) {
      return std::unexpected{primary_error};
    }

    auto fallback_dimensions = fallback.decoder->probe_dimensions(path);
    if (!fallback_dimensions) {
      return std::unexpected{decoder_registry_detail::fallback_error(
          "原生尺寸探测失败", primary_error, "WIC 兜底", fallback_dimensions.error())};
    }
    return fallback_dimensions;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"探测输入图片尺寸时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"探测输入图片尺寸时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"探测输入图片尺寸时文件系统访问失败。"};
  }
}

}  // namespace awj
