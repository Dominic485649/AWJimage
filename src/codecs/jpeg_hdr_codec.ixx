module;

#include <algorithm>
#include <cmath>
#include <cstring>
#include <expected>
#include <memory>
#include <cstdint>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <ultrahdr_api.h>
#include <ultrahdr/ultrahdrcommon.h>
#include <ultrahdr/gainmapmath.h>
#include <ultrahdr/icc.h>
#include <libplacebo/colorspace.h>
#include "uhdr_thread_adapter.h"

export module awj.jpeg_hdr_codec;
import awj.config;
import awj.codec;
import awj.hdr_tonemap;
import awj.image;
import awj.jpegli_codec;

export namespace awj {

// The profile helper returns a JPEG ICC marker payload, including its 14-byte identifier.
inline std::expected<void, std::string> jpeg_hdr_icc(ImageBuffer& image,
    uhdr_color_transfer_t transfer, uhdr_color_gamut_t gamut) {
  auto icc = ultrahdr::IccHelper::writeIccProfile(transfer, gamut);
  if (!icc || icc->getLength() <= 14) return std::unexpected{"HDR JPEG ICC 生成失败。"};
  const auto* begin = reinterpret_cast<const std::byte*>(icc->getData()) + 14;
  image.metadata.push_back({MetadataKind::icc, {begin, begin + icc->getLength() - 14}});
  return {};
}

std::expected<NativeEncodeResult, std::string> encode_hdr_jpeg(
    const ImageBuffer& image, const NativeEncodeSettings& settings,
    std::stop_token stop = {}) {
  try {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    if (settings.visual_quality) return std::unexpected{"HDR JPEG 不支持视觉质量搜索；请使用 --quality，或 --jpeg-hdr sdr。"};
    auto valid = hdr::hdr_detail::validate_rgba(image, "HDR JPEG");
    if (!valid) return std::unexpected{valid.error()};
    const auto budget = settings.resources.memory_limit_bytes / std::max(1, settings.resources.file_parallelism);
    // Input + canonical HDR + base decode + JPEGli workspaces + output buffers.
    if (image.width > budget / 64 / image.height)
      return std::unexpected{"HDR JPEG 工作区超出内存预算。"};
    auto transform = hdr::hdr_detail::make_source_transform(image);
    if (!transform) return std::unexpected{transform.error()};
    auto sdr = hdr::tone_map_to_sdr_srgb(image, 8, stop);
    if (!sdr) return std::unexpected{sdr.error()};
    // Old gain-map/color metadata cannot describe the newly encoded base.
    std::erase_if(sdr->metadata, [](const auto& m) { return m.kind != MetadataKind::exif; });
    if (settings.strip_metadata) sdr->metadata.clear();
    if (auto icc = jpeg_hdr_icc(*sdr, UHDR_CT_SRGB, UHDR_CG_BT_709); !icc)
      return std::unexpected{icc.error()};
    auto base_settings = settings;
    base_settings.bit_depth = 8;
    base_settings.strip_metadata = false; // Essential ICC survives optional metadata stripping.
    base_settings.applied_icc = "kept";
    base_settings.jpegli_rgb8_input = {};
    JpegliImageEncoder jpegli;
    auto base = jpegli.encode(*sdr, base_settings, stop);
    if (!base) return std::unexpected{base.error()};
    JpegliImageDecoder decoder;
    auto reference = decoder.decode_memory(base->encoded.bytes, "HDR JPEG base", {.copy_metadata_payloads = false});
    if (!reference) return std::unexpected{reference.error()};
    sdr->planes.clear();

    ultrahdr::uhdr_raw_image_ext_t canonical(UHDR_IMG_FMT_64bppRGBAHalfFloat,
        UHDR_CG_BT_2100, UHDR_CT_LINEAR, UHDR_CR_FULL_RANGE,
        static_cast<unsigned>(image.width), static_cast<unsigned>(image.height), 1);
    const auto matrix = pl_get_color_mapping_matrix(transform->primaries,
        pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020), PL_INTENT_RELATIVE_COLORIMETRIC);
    for (std::size_t y = 0; y < image.height; ++y) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      const auto* row = image.planes.front().bytes.data() + y * image.planes.front().stride;
      auto* dst = static_cast<std::uint16_t*>(canonical.planes[0]) + y * canonical.stride[0] * 4;
      for (std::size_t x = 0; x < image.width; ++x) {
        auto rgb = hdr::hdr_detail::linear_nits(image, *transform, row, x * 4);
        if (!rgb) return std::unexpected{rgb.error()};
        pl_matrix3x3_apply(&matrix, rgb->data());
        for (int c = 0; c < 3; ++c) {
          const float value = std::max((*rgb)[c], 0.0F) / 203.0F;
          if (!std::isfinite(value) || value > 65504) return std::unexpected{"HDR JPEG 亮度超出 FP16 表示范围。"};
          dst[x * 4 + c] = ultrahdr::floatToHalf(value);
        }
        dst[x * 4 + 3] = ultrahdr::floatToHalf(1.0F);
      }
    }
    uhdr_raw_image_t base_pixels{};
    base_pixels.fmt = UHDR_IMG_FMT_32bppRGBA8888;
    base_pixels.cg = UHDR_CG_BT_709; base_pixels.ct = UHDR_CT_SRGB;
    base_pixels.range = UHDR_CR_FULL_RANGE;
    base_pixels.w = static_cast<unsigned>(image.width); base_pixels.h = static_cast<unsigned>(image.height);
    base_pixels.planes[0] = reference->image.planes.front().bytes.data();
    base_pixels.stride[0] = static_cast<unsigned>(reference->image.planes.front().stride / 4);
    struct Threads { unsigned previous; ~Threads() { awjUhdrThreadLimit(previous); } } threads{
        awjUhdrThreadLimit(static_cast<unsigned>(std::max(1, settings.resources.encoder_threads_per_file)))};
    const int scale = static_cast<int>(std::min<std::size_t>(4, std::min(image.width, image.height)));
    ultrahdr::UltraHdr generator(nullptr, scale, settings.quality, true, 1.0F, UHDR_USAGE_BEST_QUALITY);
    ultrahdr::uhdr_gainmap_metadata_ext_t metadata(ultrahdr::kJpegrVersion);
    std::unique_ptr<ultrahdr::uhdr_raw_image_ext_t> map;
    auto error = generator.generateGainMap(&base_pixels, &canonical, &metadata, map, false, false);
    if (error.error_code != UHDR_CODEC_OK || !map)
      return std::unexpected{std::string("HDR JPEG Gain Map 生成失败: ") + error.detail};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    if (map->fmt != UHDR_IMG_FMT_24bppRGB888) return std::unexpected{"HDR JPEG Gain Map 像素格式不支持。"};
    auto pixels = hdr::hdr_detail::make_rgba_bytes(map->w, map->h, 1, "HDR JPEG Gain Map");
    if (!pixels) return std::unexpected{pixels.error()};
    for (unsigned y = 0; y < map->h; ++y) {
      const auto* src = static_cast<const std::byte*>(map->planes[0]) + y * map->stride[0] * 3;
      auto* dst = pixels->data() + y * map->w * 4;
      for (unsigned x = 0; x < map->w; ++x) {
        std::memcpy(dst + x * 4, src + x * 3, 3); dst[x * 4 + 3] = std::byte{255};
      }
    }
    ImageBuffer gain{.width = map->w, .height = map->h, .pixel_format = PixelFormat::rgba,
        .planes = {{std::move(*pixels), map->w * 4}}};
    if (auto icc = jpeg_hdr_icc(gain, map->ct, map->cg); !icc) return std::unexpected{icc.error()};
    auto gain_settings = base_settings;
    gain_settings.chroma_mode = ChromaMode::yuv444;
    gain_settings.jpegli_xyb = false;
    gain_settings.jpegli_progressive_level = 0;
    gain_settings.jpegli_gain_map_samples = true;
    auto encoded_map = jpegli.encode(gain, gain_settings, stop);
    if (!encoded_map) return std::unexpected{encoded_map.error()};
    std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_encoder)> encoder(uhdr_create_encoder(), uhdr_release_encoder);
    if (!encoder) return std::unexpected{"无法创建 HDR JPEG 封装器。"};
    uhdr_compressed_image_t base_data{base->encoded.bytes.data(), base->encoded.bytes.size(), base->encoded.bytes.size(),
        UHDR_CG_BT_709, UHDR_CT_SRGB, UHDR_CR_FULL_RANGE};
    uhdr_compressed_image_t gain_data{encoded_map->encoded.bytes.data(), encoded_map->encoded.bytes.size(),
        encoded_map->encoded.bytes.size(), map->cg, map->ct, map->range};
    error = uhdr_enc_set_compressed_image(encoder.get(), &base_data, UHDR_BASE_IMG);
    if (error.error_code == UHDR_CODEC_OK) error = uhdr_enc_set_gainmap_image(encoder.get(), &gain_data, &metadata);
    if (error.error_code == UHDR_CODEC_OK) error = uhdr_encode(encoder.get());
    if (error.error_code != UHDR_CODEC_OK) return std::unexpected{std::string("HDR JPEG 封装失败: ") + error.detail};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    const auto* stream = uhdr_get_encoded_stream(encoder.get());
    if (!stream || stream->data_sz > budget / 3) return std::unexpected{"HDR JPEG 输出超出内存预算。"};
    const auto* begin = static_cast<const std::byte*>(stream->data);
    base->encoded.bytes.assign(begin, begin + stream->data_sz);
    base->encoded.codec_name = "jpegli-ultrahdr";
    base->diagnostics.integration_mode = "jpegli-ultrahdr-iso";
    base->diagnostics.applied_hdr_metadata = "iso21496-1-gain-map";
    base->diagnostics.color_reason = "8-bit SDR base + JPEGli Gain Map；线性 FP16 HDR 重建";
    base->diagnostics.applied_color_primaries = 1;
    base->diagnostics.applied_transfer_characteristics = 13;
    base->diagnostics.applied_matrix_coefficients = 0;
    base->diagnostics.applied_icc = "essential-base-and-alternate";
    return base;
  } catch (const std::bad_alloc&) { return std::unexpected{"HDR JPEG 编码内存不足。"}; }
    catch (const std::length_error&) { return std::unexpected{"HDR JPEG 数据超过运行时限制。"}; }
}
}
