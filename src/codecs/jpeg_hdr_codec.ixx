module;

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>
#include <ultrahdr_api.h>
#include <ultrahdr/ultrahdrcommon.h>
#include <ultrahdr/gainmapmath.h>
#include <ultrahdr/icc.h>
#include <avif/internal.h>
#include <libplacebo/colorspace.h>
#include "uhdr_thread_adapter.h"
#include "../core/hdr_cost.h"

export module awj.jpeg_hdr_codec;
import awj.config;
import awj.codec;
import awj.encoding_defaults;
import awj.hdr_tonemap;
import awj.image;
import awj.jpegli_codec;
import awj.pixel_conversion;

export namespace awj {

namespace jpeg_hdr_detail {

// These are distinct units: scRGB uses 80 nits, the engine's canonical linear
// intent uses 203 nits, and the SDR base is tone-mapped to 100 nits.
constexpr float canonical_white_nits = 203.0F;
constexpr float engine_peak_nits = 10'000.0F;
constexpr float minimum_headroom_stops = 1.0F / 1'024.0F;

std::expected<float, std::string> capacity_target_peak(float actual_canonical_peak_nits) {
  if (!std::isfinite(actual_canonical_peak_nits) || actual_canonical_peak_nits < 0.0F) {
    return std::unexpected{"HDR JPEG canonical RGB 分量峰值无效。"};
  }
  if (actual_canonical_peak_nits > engine_peak_nits) {
    return std::unexpected{"HDR JPEG 实际 canonical RGB 分量超过引擎支持的 10000 nits，拒绝裁剪高光。"};
  }
  return std::max(actual_canonical_peak_nits,
      canonical_white_nits * std::exp2(minimum_headroom_stops));
}

bool valid_gain_metadata(const uhdr_gainmap_metadata_t& metadata) noexcept {
  if (!std::isfinite(metadata.hdr_capacity_min) || !std::isfinite(metadata.hdr_capacity_max) ||
      metadata.hdr_capacity_min != 1.0F || metadata.hdr_capacity_max <= metadata.hdr_capacity_min) return false;
  for (int c = 0; c < 3; ++c) {
    if (!std::isfinite(metadata.min_content_boost[c]) || !std::isfinite(metadata.max_content_boost[c]) ||
        !std::isfinite(metadata.gamma[c]) || !std::isfinite(metadata.offset_sdr[c]) ||
        !std::isfinite(metadata.offset_hdr[c]) || metadata.min_content_boost[c] <= 0.0F ||
        metadata.max_content_boost[c] < metadata.min_content_boost[c] || metadata.gamma[c] <= 0.0F ||
        metadata.offset_sdr[c] < 0.0F || metadata.offset_hdr[c] < 0.0F) return false;
  }
  return true;
}

std::expected<void, std::string> validate_serialized_capacity(
    std::span<const std::byte> bytes, float target_peak_nits, std::stop_token stop) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> decoder(
      uhdr_create_decoder(), uhdr_release_decoder);
  if (!decoder) return std::unexpected{"无法创建 HDR JPEG ISO 元数据校验器。"};
  uhdr_compressed_image_t data{const_cast<std::byte*>(bytes.data()), bytes.size(), bytes.size(),
      UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
  auto error = uhdr_dec_set_image(decoder.get(), &data);
  if (error.error_code == UHDR_CODEC_OK) error = uhdr_dec_probe(decoder.get());
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (error.error_code != UHDR_CODEC_OK) return std::unexpected{"HDR JPEG ISO 元数据序列化重读失败。"};
  const auto* metadata = uhdr_dec_get_gainmap_metadata(decoder.get());
  if (!metadata || !valid_gain_metadata(*metadata)) return std::unexpected{"HDR JPEG ISO 重读后的 Gain Map 参数无效。"};
  const auto expected = target_peak_nits / canonical_white_nits;
  if (std::abs(metadata->hdr_capacity_max - expected) > expected * 1.0e-5F) {
    return std::unexpected{"HDR JPEG ISO 重读后的 capacity 与实际 canonical RGB 峰值不一致。"};
  }
  return {};
}

struct EngineThreadLimit {
  unsigned previous;
  explicit EngineThreadLimit(unsigned limit) : previous(awjUhdrThreadLimit(limit)) {}
  ~EngineThreadLimit() { awjUhdrThreadLimit(previous); }
  EngineThreadLimit(const EngineThreadLimit&) = delete;
  EngineThreadLimit& operator=(const EngineThreadLimit&) = delete;
};

bool supported_sdr_transfer(int transfer) noexcept {
  // libavif's transfer getter falls back to BT.709 for unknown values. Reject
  // those values explicitly rather than silently relabeling an ICC-only source.
  switch (transfer) {
    case 1: case 4: case 5: case 6: case 7: case 8: case 9: case 10:
    case 11: case 12: case 13: case 14: case 15: case 17:
      return true;
    default:
      return false;
  }
}

std::expected<ImageBuffer, std::string> source_sdr_base(
    const ImageBuffer& source, const NativeEncodeSettings& settings, std::stop_token stop) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  const bool has_icc = std::ranges::any_of(source.metadata,
      [](const MetadataBlock& block) { return block.kind == MetadataKind::icc; });
  if (!source.source_info || !source.source_info->color_primaries ||
      !source.source_info->transfer_characteristics ||
      *source.source_info->color_primaries == 2 ||
      *source.source_info->transfer_characteristics == 0 ||
      *source.source_info->transfer_characteristics == 2) {
    return std::unexpected{has_icc
        ? "HDR JPEG 的源 SDR 呈现只有 ICC、缺少明确 CICP；当前路径不支持任意 ICC 色彩转换，拒绝假作 sRGB。"
        : "HDR JPEG 的源 SDR 呈现缺少明确的 CICP 原色或传递特性。"};
  }
  const int primaries = *source.source_info->color_primaries;
  const int transfer = *source.source_info->transfer_characteristics;
  const auto* input_primaries = hdr::hdr_detail::primaries_from_cicp(primaries);
  const auto* output_primaries = hdr::hdr_detail::primaries_from_cicp(1);
  if (!input_primaries || !output_primaries || !supported_sdr_transfer(transfer)) {
    return std::unexpected{"HDR JPEG 的源 SDR 呈现使用了当前不支持的 CICP 原色或 SDR 传递特性。"};
  }
  if (source.sample_representation != SampleRepresentation::unorm) {
    return std::unexpected{"HDR JPEG 的源 SDR 呈现需要有明确 CICP 的整数采样；不能把浮点 HDR 端点当作 SDR。"};
  }
  const auto to_linear = avifTransferCharacteristicsGetGammaToLinearFunction(
      static_cast<avifTransferCharacteristics>(transfer));
  if (!to_linear) return std::unexpected{"HDR JPEG 的源 SDR 传递函数不可用。"};
  const bool same_primaries = pl_raw_primaries_equal(input_primaries, output_primaries);
  const auto matrix = same_primaries
      ? pl_matrix3x3{.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}}
      : pl_get_color_mapping_matrix(input_primaries, output_primaries, PL_INTENT_RELATIVE_COLORIMETRIC);
  if (!hdr::hdr_detail::finite_matrix(matrix)) {
    return std::unexpected{"HDR JPEG 的源 SDR 原色转换矩阵包含非有限值。"};
  }
  // This materializes geometry/YUV/unpremultiplication without tone mapping and
  // retains high-depth precision until the final sRGB8 quantization. Explicit
  // decoder CICP describes the samples; no color space is inferred from ICC.
  auto packed = convert_image_pixels(source, PixelConversionRequest{
      .format = PixelFormat::rgba, .bit_depth = source.bit_depth > 8 ? 16 : 8,
      .preserve_alpha = false, .tone_map_to_sdr = false}, stop, settings.resources);
  if (!packed) return std::unexpected{packed.error()};
  // The source still owns these payloads. Drop the normalization copy before
  // retaining only the new base's Exif and newly generated essential ICC.
  packed->metadata.clear();
  auto pixels = hdr::hdr_detail::make_rgba_bytes(source.width, source.height, 1, "HDR JPEG source SDR");
  if (!pixels) return std::unexpected{pixels.error()};
  const bool already_srgb = same_primaries && transfer == 13;
  for (std::size_t y = 0; y < source.height; ++y) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    auto* row = pixels->data() + y * source.width * 4;
    for (std::size_t x = 0; x < source.width; ++x) {
      if (x % hdr_cost::gamut_block_pixels == 0 && stop.stop_requested())
        return std::unexpected{"任务已取消。"};
      const auto rgba = hdr::hdr_detail::read_source_rgba(*packed, {}, x, y);
      std::array<float, 3> rgb{rgba[0], rgba[1], rgba[2]};
      if (!hdr::hdr_detail::finite_rgb(rgb)) return std::unexpected{"HDR JPEG 的源 SDR 包含非有限 RGB。"};
      if (!already_srgb) {
        for (auto& value : rgb) value = to_linear(value);
        if (!hdr::hdr_detail::finite_rgb(rgb)) return std::unexpected{"HDR JPEG 的源 SDR 线性化产生了非有限 RGB。"};
        pl_matrix3x3_apply(&matrix, rgb.data());
        if (!hdr::hdr_detail::finite_rgb(rgb)) return std::unexpected{"HDR JPEG 的源 SDR 原色转换产生了非有限 RGB。"};
        for (auto& value : rgb) value = hdr::hdr_detail::srgb_encode(value);
      }
      for (std::size_t c = 0; c < 3; ++c) {
        const auto sample = std::clamp(std::lround(std::clamp(rgb[c], 0.0F, 1.0F) * 255.0F), 0l, 255l);
        row[x * 4 + c] = std::byte{static_cast<std::uint8_t>(sample)};
      }
      row[x * 4 + 3] = std::byte{255};
    }
  }
  ImageSourceInfo info{.pixel_format = PixelFormat::rgba, .bit_depth = 8,
      .color_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 0,
      .color_range = 1, .has_hdr_metadata = false,
      .color_metadata_source = "gain-map-source-sdr-cicp-to-srgb"};
  auto output = hdr::hdr_detail::make_rgba_image(source.width, source.height,
      std::move(*pixels), AlphaMode::none, 8, SampleRepresentation::unorm,
      std::move(info), "HDR JPEG source SDR");
  if (!output) return std::unexpected{output.error()};
  for (const auto& block : source.metadata) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    if (block.kind == MetadataKind::exif) output->metadata.push_back(block);
  }
  return output;
}

}  // namespace jpeg_hdr_detail

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
    const ImageBuffer& image, const NativeEncodeSettings& settings, std::stop_token stop = {}) {
  try {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    if (settings.visual_quality) return std::unexpected{"HDR JPEG 不支持视觉质量搜索；请使用 --quality，或 --jpeg-hdr sdr。"};
    if (auto valid = hdr::hdr_detail::validate_rgba(image, "HDR JPEG"); !valid) return std::unexpected{valid.error()};
    if (image.width > std::numeric_limits<unsigned>::max() || image.height > std::numeric_limits<unsigned>::max()) {
      return std::unexpected{"HDR JPEG 尺寸超过引擎 unsigned API 范围。"};
    }
    const auto engine_threads = static_cast<unsigned>(hdr::hdr_detail::tone_worker_budget(settings.resources));
    const ImageBuffer* source_sdr = nullptr;
    if (const auto* alternative = settings.gain_map_alternative) {
      if (!std::isfinite(alternative->main_hdr_headroom_log2) ||
          !std::isfinite(alternative->alternate_hdr_headroom_log2) ||
          alternative->main_hdr_headroom_log2 < 0 || alternative->alternate_hdr_headroom_log2 < 0) {
        return std::unexpected{"HDR JPEG 的双呈现 headroom 无效。"};
      }
      // A lower endpoint can itself still be HDR. Only a genuine zero-headroom
      // SDR rendition replaces tone mapping, never a second HDR presentation.
      if (alternative->alternate_hdr_headroom_log2 == 0) {
        auto signal = hdr::has_explicit_hdr_signal(alternative->image);
        if (!signal) return std::unexpected{signal.error()};
        if (*signal) return std::unexpected{"HDR JPEG 的零 headroom SDR 呈现与其 HDR 色彩标记冲突。"};
        if (alternative->image.width != image.width || alternative->image.height != image.height) {
          return std::unexpected{"HDR JPEG 的 HDR/SDR 两端逻辑尺寸不一致。"};
        }
        source_sdr = &alternative->image;
      }
    }
    std::uint64_t source_bytes{};
    const auto reserve_source = [&](const ImageBuffer& resident) {
      for (const auto& plane : resident.planes) source_bytes = hdr_cost::add(source_bytes, plane.read_bytes().size());
      for (const auto& block : resident.metadata)
        source_bytes = hdr_cost::add(source_bytes, hdr_cost::multiply(block.bytes.size(), 2));
    };
    reserve_source(image);
    // Both buffers remain owned by decoded_input for this synchronous call,
    // even when the alternate is HDR and the base must still be tone-mapped.
    if (settings.gain_map_alternative) reserve_source(settings.gain_map_alternative->image);
    const auto estimated = hdr_cost::jpeg_working_set_bytes(image.width, image.height, 0, engine_threads, source_bytes);
    const auto limit = settings.resources.memory_limit_bytes;
    // limit == 0 means unlimited. Admission owns concurrency; do not divide the
    // global limit by file_parallelism and reject an already-admitted big file.
    if (estimated == UINT64_MAX || (limit && estimated > limit)) return std::unexpected{"HDR JPEG 工作集估算超出内存预算。"};
    auto prepared = hdr::prepare_tone_map(image, stop);
    if (!prepared) return std::unexpected{prepared.error()};
    auto base_settings = settings;
    // Freeze auto from the actual base source before normalization relabels it.
    if (base_settings.chroma_mode == ChromaMode::auto_keep && !base_settings.jpegli_xyb) {
      base_settings.chroma_mode = source_sdr ? jpeg_auto_chroma(*source_sdr) : ChromaMode::yuv444;
    }
    base_settings.bit_depth = 8;
    base_settings.rendition.selected = "sdr";
    base_settings.rendition.origin = source_sdr && source_sdr->source_info
        ? std::string{rendition_origin_name(source_sdr->source_info->rendition_origin)} : "tone-mapped";
    base_settings.rendition.reference_chroma = chroma_mode_name(base_settings.chroma_mode);
    base_settings.rendition.reference_bit_depth = source_sdr ? rendition_reference_depth(*source_sdr) : 8;
    base_settings.rendition.reused_sdr = source_sdr != nullptr;
    base_settings.rendition.tone_mapped = source_sdr == nullptr;
    base_settings.strip_metadata = false; // Essential ICC survives optional stripping.
    base_settings.applied_icc = "kept";
    base_settings.jpegli_rgb8_input = {};
    base_settings.jxl_rgb8_input = {};
    base_settings.gain_map_alternative = nullptr;
    base_settings.tone_map_to_sdr_pixels = false;
    auto sdr = source_sdr
        ? jpeg_hdr_detail::source_sdr_base(*source_sdr, settings, stop)
        : hdr::tone_map_to_sdr_srgb(image, *prepared, 8, stop, settings.resources);
    if (!sdr) return std::unexpected{sdr.error()};
    if (!sdr->source_info || sdr->source_info->color_primaries != 1 ||
        sdr->source_info->transfer_characteristics != 13 || sdr->source_info->color_range != 1) {
      return std::unexpected{"HDR JPEG 的归一化 SDR base 不是完整范围 BT.709/sRGB。"};
    }
    const int base_primaries = *sdr->source_info->color_primaries;
    const int base_transfer_cicp = *sdr->source_info->transfer_characteristics;
    const auto base_gamut = UHDR_CG_BT_709;
    const auto base_transfer = UHDR_CT_SRGB;
    base_settings.applied_color_primaries = base_primaries;
    base_settings.applied_transfer_characteristics = base_transfer_cicp;
    base_settings.applied_matrix_coefficients = base_settings.jpegli_xyb ? 0 : 6;
    base_settings.applied_color_range = 1;
    base_settings.color_metadata_source = sdr->source_info->color_metadata_source;
    // Old gain-map/color metadata cannot describe the newly encoded base.
    std::erase_if(sdr->metadata, [](const auto& m) { return m.kind != MetadataKind::exif; });
    if (settings.strip_metadata) sdr->metadata.clear();
    if (auto icc = jpeg_hdr_icc(*sdr, base_transfer, base_gamut); !icc) return std::unexpected{icc.error()};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    JpegliImageEncoder jpegli;
    auto base = jpegli.encode(*sdr, base_settings, stop);
    if (!base) return std::unexpected{base.error()};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    JpegliImageDecoder decoder;
    auto reference = decoder.decode_memory(base->encoded.bytes, "HDR JPEG base", {.copy_metadata_payloads = false});
    if (!reference) return std::unexpected{reference.error()};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    if (auto valid = hdr::hdr_detail::validate_rgba(reference->image, "HDR JPEG base decode"); !valid) {
      return std::unexpected{valid.error()};
    }
    if (reference->image.width != image.width || reference->image.height != image.height ||
        reference->image.bit_depth != 8 || reference->image.sample_representation != SampleRepresentation::unorm ||
        reference->image.planes.front().stride / 4 > std::numeric_limits<unsigned>::max() ||
        reference->image.planes.front().stride % 4 != 0) {
      return std::unexpected{"HDR JPEG base decode 的尺寸、格式或 stride 无效。"};
    }
    sdr->planes.clear();
    ultrahdr::uhdr_gainmap_metadata_ext_t metadata(ultrahdr::kJpegrVersion);
    std::unique_ptr<ultrahdr::uhdr_raw_image_ext_t> map;
    float canonical_component_peak{};
    float canonical_y_peak{};
    float target_peak{};
    const auto compute_started = std::chrono::steady_clock::now();
    {
      ultrahdr::uhdr_raw_image_ext_t canonical(UHDR_IMG_FMT_64bppRGBAHalfFloat,
          UHDR_CG_BT_2100, UHDR_CT_LINEAR, UHDR_CR_FULL_RANGE,
          static_cast<unsigned>(image.width), static_cast<unsigned>(image.height), 1);
      for (std::size_t y = 0; y < image.height; ++y) {
        if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
        auto* dst = static_cast<std::uint16_t*>(canonical.planes[0]) + y * static_cast<std::size_t>(canonical.stride[0]) * 4;
        for (std::size_t x = 0; x < image.width; ++x) {
          if (x % hdr_cost::gamut_block_pixels == 0 && stop.stop_requested()) return std::unexpected{"任务已取消。"};
          const auto rgba = hdr::hdr_detail::read_source_rgba(image, prepared->reader, x, y);
          auto rgb = hdr::hdr_detail::linear_nits({rgba[0], rgba[1], rgba[2]}, prepared->transform);
          if (!rgb) return std::unexpected{rgb.error()};
          pl_matrix3x3_apply(&prepared->source_to_bt2020, rgb->data());
          if (!hdr::hdr_detail::finite_rgb(*rgb)) return std::unexpected{"HDR JPEG canonical 色彩矩阵产生了非有限 RGB。"};
          const auto y_nits = hdr::hdr_detail::luma_nits(*rgb, prepared->canonical_to_xyz);
          if (!std::isfinite(y_nits)) return std::unexpected{"HDR JPEG canonical Y 产生了非有限值。"};
          canonical_y_peak = std::max(canonical_y_peak, y_nits);
          for (int c = 0; c < 3; ++c) {
            const auto positive_nits = std::max((*rgb)[c], 0.0F);
            const auto value = positive_nits / jpeg_hdr_detail::canonical_white_nits;
            if (!std::isfinite(value) || value > 65'504.0F) {
              return std::unexpected{"HDR JPEG canonical 亮度超出有限 FP16 范围（引擎最高支持 10000 nits）。"};
            }
            if (positive_nits > jpeg_hdr_detail::engine_peak_nits) {
              return std::unexpected{"HDR JPEG 实际 canonical RGB 分量超过引擎支持的 10000 nits，拒绝裁剪高光。"};
            }
            const auto half = ultrahdr::floatToHalf(value);
            const auto canonical_nits = hdr::hdr_detail::binary16_to_float(half) * jpeg_hdr_detail::canonical_white_nits;
            if (!std::isfinite(canonical_nits)) return std::unexpected{"HDR JPEG canonical FP16 量化产生了非有限值。"};
            // Cover the actual FP16 intent consumed by the engine as well as
            // the pre-quantized component, without a second full-image scan.
            canonical_component_peak = std::max({canonical_component_peak, positive_nits, canonical_nits});
            dst[x * 4 + c] = half;
          }
          dst[x * 4 + 3] = ultrahdr::floatToHalf(1.0F);
        }
      }
      auto target = jpeg_hdr_detail::capacity_target_peak(canonical_component_peak);
      if (!target) return std::unexpected{target.error()};
      target_peak = *target;
      uhdr_raw_image_t base_pixels{};
      base_pixels.fmt = UHDR_IMG_FMT_32bppRGBA8888;
      base_pixels.cg = base_gamut; base_pixels.ct = base_transfer;
      base_pixels.range = UHDR_CR_FULL_RANGE;
      base_pixels.w = static_cast<unsigned>(image.width); base_pixels.h = static_cast<unsigned>(image.height);
      base_pixels.planes[0] = reference->image.planes.front().bytes.data();
      base_pixels.stride[0] = static_cast<unsigned>(reference->image.planes.front().stride / 4);
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      // Arguments 7/8 retain the engine defaults. The ninth argument controls
      // generated metadata here; changing the later C wrapper cannot fix it.
      const int scale = static_cast<int>(std::min<std::size_t>({4, image.width, image.height}));
      ultrahdr::UltraHdr generator(nullptr, scale, settings.quality, true, 1.0F, UHDR_USAGE_BEST_QUALITY,
          std::numeric_limits<float>::min(), std::numeric_limits<float>::max(), target_peak);
      jpeg_hdr_detail::EngineThreadLimit threads(engine_threads);
      auto error = generator.generateGainMap(&base_pixels, &canonical, &metadata, map, false, false);
      if (error.error_code != UHDR_CODEC_OK || !map) {
        return std::unexpected{std::string("HDR JPEG Gain Map 生成失败: ") + error.detail};
      }
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      if (!jpeg_hdr_detail::valid_gain_metadata(metadata) ||
          std::abs(metadata.hdr_capacity_max - target_peak / jpeg_hdr_detail::canonical_white_nits) > 1.0e-5F) {
        return std::unexpected{"HDR JPEG 生成后的 Gain Map 参数或 capacity 无效。"};
      }
    } // Tone workers have joined; canonical and engine worker scratch now die.
    base->diagnostics.timing.gain_map_compute_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - compute_started).count();
    reference->image.planes.clear();
    if (map->fmt != UHDR_IMG_FMT_24bppRGB888 || !map->w || !map->h || !map->planes[0] ||
        map->stride[0] < map->w || static_cast<std::size_t>(map->stride[0]) > SIZE_MAX / 3 / map->h) {
      return std::unexpected{"HDR JPEG Gain Map 像素格式、尺寸或 stride 无效。"};
    }
    const auto gain_gamut = map->cg;
    const auto gain_transfer = map->ct;
    const auto gain_range = map->range;
    auto pixels = hdr::hdr_detail::make_rgba_bytes(map->w, map->h, 1, "HDR JPEG Gain Map");
    if (!pixels) return std::unexpected{pixels.error()};
    for (unsigned y = 0; y < map->h; ++y) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      const auto* src = static_cast<const std::byte*>(map->planes[0]) + static_cast<std::size_t>(y) * map->stride[0] * 3;
      auto* dst = pixels->data() + static_cast<std::size_t>(y) * map->w * 4;
      for (unsigned x = 0; x < map->w; ++x) {
        std::memcpy(dst + static_cast<std::size_t>(x) * 4, src + static_cast<std::size_t>(x) * 3, 3);
        dst[static_cast<std::size_t>(x) * 4 + 3] = std::byte{255};
      }
    }
    ImageBuffer gain{.width = map->w, .height = map->h, .pixel_format = PixelFormat::rgba,
        .planes = {{std::move(*pixels), static_cast<std::size_t>(map->w) * 4}}};
    map.reset();
    if (auto icc = jpeg_hdr_icc(gain, gain_transfer, gain_gamut); !icc) return std::unexpected{icc.error()};
    auto gain_settings = base_settings;
    gain_settings.chroma_mode = ChromaMode::yuv444;
    gain_settings.jpegli_xyb = false;
    gain_settings.jpegli_progressive_level = 0;
    gain_settings.jpegli_gain_map_samples = true;
    auto encoded_map = jpegli.encode(gain, gain_settings, stop);
    if (!encoded_map) return std::unexpected{encoded_map.error()};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    gain.planes.clear();
    std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_encoder)> encoder(uhdr_create_encoder(), uhdr_release_encoder);
    if (!encoder) return std::unexpected{"无法创建 HDR JPEG ISO 封装器。"};
    uhdr_compressed_image_t base_data{base->encoded.bytes.data(), base->encoded.bytes.size(), base->encoded.bytes.size(),
        base_gamut, base_transfer, UHDR_CR_FULL_RANGE};
    uhdr_compressed_image_t gain_data{encoded_map->encoded.bytes.data(), encoded_map->encoded.bytes.size(),
        encoded_map->encoded.bytes.size(), gain_gamut, gain_transfer, gain_range};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    auto error = uhdr_enc_set_compressed_image(encoder.get(), &base_data, UHDR_BASE_IMG);
    if (error.error_code == UHDR_CODEC_OK) error = uhdr_enc_set_gainmap_image(encoder.get(), &gain_data, &metadata);
    if (error.error_code == UHDR_CODEC_OK) error = uhdr_encode(encoder.get());
    if (error.error_code != UHDR_CODEC_OK) return std::unexpected{std::string("HDR JPEG ISO 封装失败: ") + error.detail};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    const auto* stream = uhdr_get_encoded_stream(encoder.get());
    if (!stream || !stream->data || !stream->data_sz ||
        stream->data_sz > encoding_defaults::effective_max_input_file_bytes()) return std::unexpected{"HDR JPEG ISO 输出尺寸无效或超过运行时上限。"};
    const auto assembly = hdr_cost::jpeg_assembly_bytes(
        hdr_cost::jpeg_source_reservation_bytes(image.width, image.height, source_bytes),
        base_data.data_sz, gain_data.data_sz, stream->data_sz);
    if (assembly == UINT64_MAX || (limit && assembly > limit)) return std::unexpected{"HDR JPEG ISO 封装输出工作集超出内存预算。"};
    const auto* begin = static_cast<const std::byte*>(stream->data);
    base->encoded.bytes.assign(begin, begin + stream->data_sz);
    encoder.reset();
    std::vector<std::byte>{}.swap(encoded_map->encoded.bytes);
    if (auto checked = jpeg_hdr_detail::validate_serialized_capacity(base->encoded.bytes, target_peak, stop); !checked) {
      return std::unexpected{checked.error()};
    }
    base->encoded.codec_name = "jpegli-iso21496-1";
    base->diagnostics.integration_mode = "jpegli-iso21496-1";
    base->diagnostics.applied_hdr_metadata = "iso21496-1-gain-map";
    base->diagnostics.color_reason = std::format(
        "8-bit SDR base + ISO 21496-1 Gain Map；base={}；base CICP={}/{}；raw Y peak={:.3f} nits；canonical Y peak={:.3f} nits；canonical RGB peak={:.3f} nits；target={:.3f} nits",
        source_sdr ? "source-sdr-rendition" : "hdr-tone-map", base_primaries, base_transfer_cicp,
        prepared->scanned_y.peak, canonical_y_peak, canonical_component_peak, target_peak);
    base->diagnostics.applied_color_primaries = base_primaries;
    base->diagnostics.applied_transfer_characteristics = base_transfer_cicp;
    base->diagnostics.applied_matrix_coefficients = base_settings.jpegli_xyb ? 0 : 6;
    base->diagnostics.applied_color_range = 1;
    base->diagnostics.applied_icc = "essential-base-and-alternate";
    base->diagnostics.rendition.gain_map_generated = true;
    base->diagnostics.rendition.gain_map_width = static_cast<std::uint32_t>(gain.width);
    base->diagnostics.rendition.gain_map_height = static_cast<std::uint32_t>(gain.height);
    base->diagnostics.rendition.gain_map_bit_depth = 8;
    return base;
  } catch (const std::bad_alloc&) { return std::unexpected{"HDR JPEG 编码内存不足。"}; }
    catch (const std::length_error&) { return std::unexpected{"HDR JPEG 数据超过运行时限制。"}; }
}

}  // namespace awj
