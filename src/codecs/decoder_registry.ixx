module;
#include "../compat/stdlib_compat.h"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module awj.decoder_registry;

import awj.avif_aom_codec;
import awj.bmp_codec;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.gif_codec;
import awj.gain_map;
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
  awj::compat::stop_token stop_token{};
  bool native_pixels{};
  GainMapDecodeMode gain_map_mode{GainMapDecodeMode::hdr_only};
  DecodeDiagnostics* diagnostics{};
  // 0 表示调用方没有单独提供尺寸预检预约，不改变既有直接调用语义。
  std::uint64_t probe_memory_reservation_bytes{};
};

inline constexpr std::uint64_t dimension_probe_fixed_bytes = 8ull * 1024 * 1024;
inline constexpr std::uint64_t dimension_probe_input_copies = 4;

std::uint64_t dimension_probe_working_set_bytes(const fs::path&,
                                               std::uint64_t input_bytes) {
  // 内容嗅探可跨后缀进入 Gain Map。包含应用输入、库内整包、主/增益 JPEG 和 metadata 副本；
  // capacity=1 的探测重开先销毁旧 decoder，不把两套库内载荷叠在同一预约上。
  return input_bytes > (std::numeric_limits<std::uint64_t>::max() - dimension_probe_fixed_bytes) / dimension_probe_input_copies
      ? std::numeric_limits<std::uint64_t>::max()
      : dimension_probe_fixed_bytes + input_bytes * dimension_probe_input_copies;
}

struct DecoderSelection {
  std::unique_ptr<ImageDecoder> decoder{};
  bool fallback{};
};

namespace decoder_registry_detail {

template <class Decoder>
bool try_select(const fs::path& path, DecoderSelection& selection, int decode_threads,
                awj::compat::stop_token stop_token = {}) {
  auto decoder = [&] {
    if constexpr (std::constructible_from<Decoder, int, awj::compat::stop_token>) {
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
  return awj::compat::format("{}: {}；{}失败: {}",
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
  return awj::compat::format("{}；libjpeg-turbo 回退失败: {}",
                     std::move(primary_error),
                     fallback_error);
}

}  // namespace decoder_registry_detail

awj::compat::expected<DecoderSelection, std::string> select_decoder_for_path(
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
    return awj::compat::unexpected{awj::compat::format("native backend 暂不支持输入格式: {}", extension_label)};
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"选择输入解码器时内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"选择输入解码器时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return awj::compat::unexpected{"选择输入解码器时文件系统访问失败。"};
  }
}

awj::compat::expected<ImageDecodeResult, std::string> decode_image_for_path(
    const fs::path& path,
    DecoderRegistryOptions options) {
  try {
    if (options.diagnostics) {
      *options.diagnostics = DecodeDiagnostics{.stage = "decode.gain-map-probe"};
    }
    auto enhanced = decode_jpeg_gain_map(path, options.stop_token, options.decode_threads, options.gain_map_mode);
    if (!enhanced) return awj::compat::unexpected{enhanced.error()};
    if (*enhanced) {
      if (options.diagnostics) {
        capture_decode_source(*options.diagnostics, (**enhanced).image);
        options.diagnostics->decoder_id = (**enhanced).decoder_id;
        options.diagnostics->stage = "complete";
      }
      return std::move(**enhanced);
    }
    if (options.diagnostics) options.diagnostics->stage = "decode.select";
    auto selected = select_decoder_for_path(path, options);
    if (!selected) {
      return awj::compat::unexpected{selected.error()};
    }

    if (options.diagnostics) {
      options.diagnostics->decoder_id = selected->decoder->id();
      options.diagnostics->stage = "decode.pixels";
    }
    auto decoded = selected->decoder->decode(path, DecodeOptions{
        .native_pixels = options.native_pixels, .gain_map_mode = options.gain_map_mode,
        .diagnostics = options.diagnostics, .stop_token = options.stop_token});
    if (decoded) {
      decoded->used_fallback = decoded->used_fallback || selected->fallback;
      if (options.diagnostics) {
        if (!options.diagnostics->source_info) capture_decode_source(*options.diagnostics, decoded->image);
        options.diagnostics->decoder_id = decoded->decoder_id.empty()
            ? std::string{selected->decoder->id()} : decoded->decoder_id;
        options.diagnostics->stage = "complete";
      }
      return decoded;
    }

    if (!options.allow_wic_fallback || selected->fallback) {
      return awj::compat::unexpected{decoded.error()};
    }

    if (decoded.error().starts_with("Gain Map:") ||
        decoder_registry_detail::is_unsupported_multi_image_error(decoded.error())) {
      return awj::compat::unexpected{decoded.error()};
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
          if (options.diagnostics) {
            *options.diagnostics = DecodeDiagnostics{.decoder_id = jpeg_turbo_decoded->decoder_id,
                                                      .stage = "complete"};
            capture_decode_source(*options.diagnostics, jpeg_turbo_decoded->image);
          }
          return jpeg_turbo_decoded;
        }
        primary_error = decoder_registry_detail::with_jpeg_turbo_fallback_error(
            std::move(primary_error), jpeg_turbo_decoded.error());
      }
    }

    DecoderSelection fallback{};
    const auto decode_threads = std::max(1, options.decode_threads);
    if (!decoder_registry_detail::try_select_wic_fallback(path, fallback, decode_threads)) {
      return awj::compat::unexpected{primary_error};
    }

    auto fallback_decoded = fallback.decoder->decode(path);
    if (!fallback_decoded) {
      return awj::compat::unexpected{decoder_registry_detail::fallback_error(
          "原生解码失败", primary_error, "WIC 兜底", fallback_decoded.error())};
    }
    fallback_decoded->used_fallback = true;
    if (options.diagnostics) {
      *options.diagnostics = DecodeDiagnostics{.decoder_id = fallback_decoded->decoder_id,
                                                .stage = "complete"};
      capture_decode_source(*options.diagnostics, fallback_decoded->image);
    }
    return fallback_decoded;
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"解码输入图片时内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"解码输入图片时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return awj::compat::unexpected{"解码输入图片时文件系统访问失败。"};
  }
}

awj::compat::expected<ImageDimensions, std::string> probe_image_dimensions_for_path(
    const fs::path& path,
    DecoderRegistryOptions options) {
  try {
    if (options.stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    std::error_code size_error;
    const auto input_bytes = fs::file_size(path, size_error);
    if (size_error) return awj::compat::unexpected{awj::compat::format(
        "读取待探测输入文件大小失败: {}；系统错误：{}。", display_path_for_user(path), size_error.message())};
    if (auto within_limit = check_scanned_input_file_size(path, input_bytes); !within_limit)
      return awj::compat::unexpected{within_limit.error()};
    const auto probe_bytes = dimension_probe_working_set_bytes(path, input_bytes);
    if (options.probe_memory_reservation_bytes && probe_bytes > options.probe_memory_reservation_bytes)
      return awj::compat::unexpected{awj::compat::format(
          "尺寸探测工作集 {} 超过预检预约 {}，未读取完整输入。",
          format_size(probe_bytes), format_size(options.probe_memory_reservation_bytes))};
    const auto file_read_limit = !options.probe_memory_reservation_bytes ? UINT64_MAX
        : options.probe_memory_reservation_bytes > dimension_probe_fixed_bytes
            ? (options.probe_memory_reservation_bytes - dimension_probe_fixed_bytes) / dimension_probe_input_copies : 0;
    const decoder_common::ScopedProbeFileReadLimit read_scope{file_read_limit};
    auto enhanced = probe_jpeg_gain_map_dimensions(path, options.stop_token);
    if (!enhanced) return awj::compat::unexpected{enhanced.error()};
    if (options.stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (*enhanced) return **enhanced;
    auto selected = select_decoder_for_path(path, options);
    if (!selected) {
      return awj::compat::unexpected{selected.error()};
    }

    auto dimensions = selected->decoder->id() == "libpng"
        ? PngImageDecoder{}.probe_dimensions(path, options.probe_memory_reservation_bytes, options.stop_token)
        : selected->decoder->probe_dimensions(path);
    if (dimensions) {
      return dimensions;
    }
    if (dimensions.error().starts_with("尺寸探测工作集 ") || dimensions.error().starts_with("PNG ICC "))
      return awj::compat::unexpected{dimensions.error()};

    if (!options.allow_wic_fallback || selected->fallback) {
      return awj::compat::unexpected{dimensions.error()};
    }

    if (dimensions.error().starts_with("Gain Map:") ||
        decoder_registry_detail::is_unsupported_multi_image_error(dimensions.error())) {
      return awj::compat::unexpected{dimensions.error()};
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
      return awj::compat::unexpected{primary_error};
    }

    auto fallback_dimensions = fallback.decoder->probe_dimensions(path);
    if (!fallback_dimensions) {
      return awj::compat::unexpected{decoder_registry_detail::fallback_error(
          "原生尺寸探测失败", primary_error, "WIC 兜底", fallback_dimensions.error())};
    }
    return fallback_dimensions;
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"探测输入图片尺寸时内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"探测输入图片尺寸时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return awj::compat::unexpected{"探测输入图片尺寸时文件系统访问失败。"};
  }
}

}  // namespace awj
