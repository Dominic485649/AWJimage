module;
#include "../compat/stdlib_compat.h"
#include "hdr_cost.h"
#include "spline36_resample.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

export module awj.image_memory_plan;

import awj.config;
import awj.codec;
import awj.image;
import awj.large_image_plan;
import awj.pixel_conversion;
import awj.resource_planner;
import awj.hdr_tonemap;
import awj.core;

export namespace awj {

// 所有值均为预约模型，不是 allocator/RSS 硬上界；事实存储与经验余量单独保留。
struct ImageMemoryEstimate {
  std::uint64_t bytes{1};
  std::uint64_t source_resident_bytes{};
  std::uint64_t decode_bytes{};
  std::uint64_t prepare_bytes{};
  std::uint64_t encode_bytes{};
  std::uint64_t metric_bytes{};
  std::uint64_t output_bytes{};
  std::uint64_t mandatory_bytes{};
  std::uint64_t conversion_bytes{};
  std::uint64_t codec_workspace_bytes{};
  std::uint64_t output_capacity_bytes{};
  std::uint64_t layer_retained_bytes{};
  std::uint32_t source_width{};
  std::uint32_t source_height{};
  std::uint32_t target_width{};
  std::uint32_t target_height{};
  int target_bit_depth{};
  ChromaMode target_chroma{ChromaMode::auto_keep};
  bool target_alpha{};
  bool source_has_gain_map{};
  bool refined{};
  std::string_view profile{"dimensions-unknown"};
};

ImageDimensions target_dimensions_for_config(ImageDimensions source, const AppConfig& cfg) {
  if (const auto target = limited_dimensions(source.width, source.height, cfg)) {
    source.width = static_cast<std::uint32_t>(target->first);
    source.height = static_cast<std::uint32_t>(target->second);
  }
  source.pixel_count = static_cast<std::uint64_t>(source.width) * source.height;
  return source;
}

struct ResolvedMemoryInput {
  std::uint64_t source_bytes{};
  PixelFormat source_format{PixelFormat::unknown};
  bool conversion_required{true};
};

std::uint64_t resident_image_bytes(const ImageBuffer& image) noexcept {
  std::uint64_t bytes{};
  for (const auto& plane : image.planes)
    bytes = hdr_cost::add(bytes, std::max<std::uint64_t>(plane.bytes.capacity(), plane.read_bytes().size()));
  for (const auto& block : image.metadata) bytes = hdr_cost::add(bytes, block.bytes.capacity());
  return bytes;
}

ImageMemoryEstimate estimated_regular_memory(ImageDimensions dimensions, const AppConfig& cfg,
    std::uint64_t compressed_bytes = 0, bool grid = false,
    const ResolvedMemoryInput* resolved = nullptr) {
  using hdr_cost::add;
  using hdr_cost::multiply;
  constexpr std::uint64_t mib = 1024ull * 1024;
  const auto target = target_dimensions_for_config(dimensions, cfg);
  ImageMemoryEstimate e{};
  e.source_width = dimensions.width; e.source_height = dimensions.height;
  e.target_width = target.width; e.target_height = target.height;
  e.source_has_gain_map = dimensions.source_has_gain_map; e.refined = resolved != nullptr;
  if (!dimensions.width || !dimensions.height || !target.width || !target.height) {
    e.bytes = UINT64_MAX; e.profile = "dimensions-invalid"; return e;
  }
  const auto geometry = plan_image_geometry(dimensions.width, dimensions.height, cfg);
  if (!geometry) { e.bytes = UINT64_MAX; e.profile = "geometry-invalid"; return e; }
  const auto display_pixels = multiply(dimensions.width, dimensions.height);
  const auto n = std::max(display_pixels, dimensions.decoded_storage_bytes /
      (dimensions.source_bit_depth > 8 ? 8u : 4u));
  const auto t = multiply(target.width, target.height);
  const auto padded = hdr_cost::padded_pixels(target.width, target.height, 64);
  const auto workers = static_cast<std::uint64_t>(std::max(1, cfg.max_jobs));
  const bool known = dimensions.source_bit_depth > 0;
  const bool sdr = dimensions.source_dynamic_range == ImageDynamicRangeHint::sdr && !dimensions.source_has_gain_map;
  const bool sdr8 = sdr && known && dimensions.source_bit_depth <= 8;
  const bool dual = dimensions.source_has_gain_map && (cfg.output_format == OutputFormat::avif ||
      cfg.output_format == OutputFormat::jxl || (cfg.output_format == OutputFormat::jpgli && cfg.jpeg_hdr != JpegHdrMode::sdr));
  const bool possible_alpha = cfg.alpha_policy != AlphaModePolicy::off &&
      (cfg.alpha_policy == AlphaModePolicy::force || dimensions.source_has_alpha || !known ||
       dimensions.source_dynamic_range == ImageDynamicRangeHint::unknown || geometry->pads());
  const bool hdr_jpeg = cfg.output_format == OutputFormat::jpgli && cfg.jpeg_hdr != JpegHdrMode::sdr &&
      (!sdr || cfg.transfer_characteristics == 16 || cfg.transfer_characteristics == 18);
  const auto depth = cfg.output_format == OutputFormat::webp || (cfg.output_format == OutputFormat::jpgli && !hdr_jpeg)
      ? 8 : cfg.bit_depth.value_or(cfg.output_format == OutputFormat::avif ?
          (dimensions.source_has_gain_map || dimensions.source_bit_depth > 10 ? 12 : 10) :
          cfg.output_format == OutputFormat::jxl ? jxl_auto_bit_depth(
              dimensions.source_has_gain_map ? 16 : dimensions.source_bit_depth) :
          cfg.output_format == OutputFormat::png ? (sdr ? 8 : 16) : 8);
  const auto chroma = cfg.avif_color_representation == AvifColorRepresentation::rgb_identity ? ChromaMode::yuv444 :
      cfg.chroma_mode == ChromaMode::auto_keep ?
      (resolved ? avif_auto_chroma(resolved->source_format) : dimensions.opaque_sdr8_yuv420 ? ChromaMode::yuv420 : ChromaMode::yuv444)
      : cfg.chroma_mode;
  e.target_bit_depth = depth; e.target_chroma = chroma; e.target_alpha = possible_alpha;
  auto source = resolved ? resolved->source_bytes : dimensions.decoded_storage_bytes;
  if (!source) source = multiply(n, sdr && known ? (dimensions.source_bit_depth > 8 ? 8 : 4) : 16);
  if (!resolved && dimensions.source_has_gain_map) source = multiply(n, dual ? 24 : 16);
  e.source_resident_bytes = source;
  const auto compressed = multiply(compressed_bytes, 2);
  const auto fixed = add(16 * mib, multiply(workers, 256 * 1024));
  const auto common = add(source, compressed);
  // 解码结束后释放解码器；精化阶段不再次预约已经过去的解码峰值。
  if (!resolved) {
    auto scratch = multiply(n, sdr8 ? 8 : 16);
    if (dimensions.source_has_gain_map) {
      const auto gain = dimensions.gain_map_width && dimensions.gain_map_height ?
          multiply(dimensions.gain_map_width, dimensions.gain_map_height) : n;
      scratch = add(scratch, add(multiply(n, 20), multiply(gain, 8)));
    }
    e.decode_bytes = add(add(common, scratch), fixed);
  }
  const auto resized_source = !resolved && geometry->changes() ? multiply(t, dual ? 16 : known && dimensions.source_bit_depth <= 8 ? 4 : 8) : source;
  const auto geometry_scratch = !resolved && geometry->resamples() ? spline36_detail::scratch_bytes(
      dimensions.width, dimensions.height, geometry->content_width, geometry->content_height) : 0;
  e.prepare_bytes = add(common, fixed);
  if (!resolved && geometry->changes())
    e.prepare_bytes = add(add(add(common, resized_source), geometry_scratch), fixed);
  auto encoder_live = add(resized_source, compressed);
  std::uint64_t destination{};
  std::uint64_t workspace{};
  std::uint64_t output_bound{};
  const auto dual_storage = dual ? multiply(t, 16) : 0;
  const auto dual_compute = dual ? multiply(t, 12) : 0;
  switch (cfg.output_format) {
    case OutputFormat::avif: {
      const auto format = chroma == ChromaMode::yuv420 ? PixelFormat::yuv420 :
          chroma == ChromaMode::yuv422 ? PixelFormat::yuv422 : chroma == ChromaMode::yuv400 ? PixelFormat::gray : PixelFormat::yuv444;
      const PixelConversionRequest conversion{.format = format, .bit_depth = depth, .preserve_alpha = possible_alpha};
      const auto planes = pixel_detail::destination_pixel_bytes(target.width, target.height, conversion,
                                                   possible_alpha ? AlphaMode::straight : AlphaMode::none);
      const auto padded_planes = pixel_detail::destination_pixel_bytes((static_cast<std::uint64_t>(target.width) + 63) & ~63ull,
          (static_cast<std::uint64_t>(target.height) + 63) & ~63ull, conversion,
          possible_alpha ? AlphaMode::straight : AlphaMode::none);
      destination = resolved && !resolved->conversion_required ? 0 : planes;
      // AOM 的块级工作区与参考平面分别计费；系数为带余量的实测策略，不声称通用硬上限。
      const bool target_hdr = !sdr || cfg.transfer_characteristics == 16 || cfg.transfer_characteristics == 18;
      const auto block_scratch = !target_hdr && depth <= 10 ? (depth > 8 ? 48u : 32u) : 64u;
      workspace = add(multiply(padded, block_scratch), multiply(padded_planes, 3));
      bool lossless_layer = cfg.visual_quality.value_or(cfg.quality) >= 100;
      for (const auto& layer : cfg.avif_layers.layers)
        lossless_layer = lossless_layer || layer.quality.value_or(0) >= 100;
      if (cfg.speed.value_or(5) <= 3 || lossless_layer)
        workspace = add(workspace, multiply(padded, 8));
      workspace = add(workspace, multiply(workers - 1, mib));
      output_bound = add(padded_planes, 64 * 1024);
      e.profile = grid ? "v2-avif-grid" : dual ? "v2-avif-gain-map" :
          cfg.avif_layers.enabled() ? "v2-avif-layered" : "v2-avif-planes";
      if (cfg.color_primaries || cfg.transfer_characteristics)
        destination = add(destination, multiply(t, 8));
      if (cfg.avif_layers.enabled()) {
        const auto count = cfg.avif_layers.automatic ? 2 : cfg.avif_layers.count.value_or(1);
        if (count < 2 || count > max_avif_layers) { e.bytes = UINT64_MAX; return e; }
        // 1.3.2/AOM 3.15.3 的课表对照中 SVC Commit 明显高于单层，另计跨层块状态。
        workspace = add(workspace, multiply(padded, 24ull * static_cast<std::uint64_t>(count - 1)));
        // SVC 跨层保留参考与码流；手动物化仅保留当前预览层，而非全部层图像。
        e.layer_retained_bytes = multiply(planes, static_cast<std::uint64_t>(count - 1));
        if (!cfg.avif_layers.automatic) {
          std::uint64_t largest_preview{};
          for (int i = 0; i < count - 1; ++i) {
            const auto& layer = cfg.avif_layers.layers[i];
            auto w = static_cast<std::uint64_t>(target.width), h = static_cast<std::uint64_t>(target.height);
            if (layer.size) { w = layer.size->first; h = layer.size->second; }
            else if (layer.scale) {
              if (!layer.scale->second) { e.bytes = UINT64_MAX; return e; }
              w = (w * layer.scale->first + layer.scale->second - 1) / layer.scale->second;
              h = (h * layer.scale->first + layer.scale->second - 1) / layer.scale->second;
            }
            if (!w || !h || w > target.width || h > target.height) { e.bytes = UINT64_MAX; return e; }
            const auto preview = add(multiply(multiply(w, h), dimensions.source_bit_depth > 8 ? 8 : 4),
                pixel_detail::destination_pixel_bytes(w, h, conversion, possible_alpha ? AlphaMode::straight : AlphaMode::none));
            largest_preview = std::max(largest_preview, add(preview,
                spline36_detail::scratch_bytes(dimensions.width, dimensions.height, w, h)));
          }
          e.layer_retained_bytes = add(e.layer_retained_bytes, largest_preview);
        }
        output_bound = multiply(output_bound, static_cast<std::uint64_t>(count));
      }
      break;
    }
    case OutputFormat::png: {
      destination = multiply(t, depth > 8 ? 8 : 4);
      output_bound = add(add(destination, destination / 16), add(target.height, 64 * 1024));
      workspace = add(2 * mib, multiply(multiply(target.width, depth > 8 ? 8 : 4), 7));
      e.profile = depth > 8 ? "v2-png-rgba16" : "v2-png-rgba8";
      break;
    }
    case OutputFormat::webp: {
      const bool lossless = cfg.visual_quality.value_or(cfg.quality) >= 100;
      destination = multiply(t, 4);
      workspace = multiply(t, lossless ? 44 : possible_alpha ? 32 : 14);
      output_bound = add(destination, 64 * 1024);
      e.profile = lossless ? "v2-webp-lossless" : possible_alpha ? "v2-webp-alpha" : "v2-webp-sdr";
      break;
    }
    case OutputFormat::jxl: {
      const bool lossless = cfg.visual_quality.value_or(cfg.quality) >= 100;
      destination = multiply(t, depth > 8 ? 8 : 4);
      const auto scratch = depth > 8 ? (possible_alpha ? 112u : 80u) :
          possible_alpha ? (lossless ? 88u : 72u) : (lossless ? 64u : 48u);
      workspace = add(multiply(t, scratch), possible_alpha ? 0 : multiply(t, depth > 8 ? 6 : 3));
      workspace = add(workspace, multiply(workers - 1, 2 * mib));
      output_bound = add(destination, 64 * 1024);
      e.profile = dual ? "v2-jxl-gain-map" : depth > 8 ? "v2-jxl-high-depth" : "v2-jxl-sdr";
      break;
    }
    case OutputFormat::jpgli: {
      destination = multiply(t, hdr_jpeg ? 8 : 4);
      e.encode_bytes = hdr_jpeg ? hdr_cost::jpeg_working_set_bytes(target.width, target.height, compressed_bytes, workers, resized_source) :
          hdr_cost::jpeg_sdr_working_set_bytes(target.width, target.height, compressed_bytes, resized_source, dimensions.opaque_sdr8_yuv420);
      output_bound = add(multiply(hdr_cost::padded_pixels(target.width, target.height, 16), 6), 64 * 1024);
      e.profile = hdr_jpeg ? "v2-jpeg-hdr" : dimensions.source_has_gain_map ?
          "v2-jpeg-source-sdr-presentation" : "v2-jpeg-sdr";
      break;
    }
  }
  e.conversion_bytes = destination;
  e.codec_workspace_bytes = workspace;
  e.output_capacity_bytes = output_bound;
  encoder_live = add(encoder_live, add(destination, dual_storage));
  if (grid) encoder_live = add(encoder_live, multiply(padded, 8));
  e.prepare_bytes = std::max(e.prepare_bytes, add(add(encoder_live, dual_compute), fixed));
  e.mandatory_bytes = add(encoder_live, fixed);
  // libavif/各 codec 的完整码流和返回副本可能在 codec 状态释放前共存，不能假设互斥。
  const auto retained = add(encoder_live, e.layer_retained_bytes);
  e.encode_bytes = std::max(e.encode_bytes, add(add(retained, workspace), add(multiply(output_bound, 3), fixed)));
  if (dual && cfg.output_format != OutputFormat::jpgli)
    e.encode_bytes = std::max(e.encode_bytes, add(add(retained, multiply(t, 96)), add(multiply(output_bound, 3), fixed)));
  e.output_bytes = add(retained, add(multiply(output_bound, 3), fixed));
  if (cfg.visual_quality && !grid) {
    const auto metric = visual_quality_working_set_bytes_for_dimensions(target);
    e.encode_bytes = add(e.encode_bytes, add(metric / 2, output_bound));
    e.metric_bytes = add(add(retained, metric), add(multiply(output_bound, 3), fixed));
  }
  if (dimensions.source_has_gain_map) {
    e.prepare_bytes = add(e.prepare_bytes, dimensions.source_icc_bytes);
    if (!resolved) e.decode_bytes = add(e.decode_bytes, dimensions.source_icc_bytes);
  }
  e.bytes = std::max({e.decode_bytes, e.prepare_bytes, e.encode_bytes, e.metric_bytes, e.output_bytes});
  return e;
}

std::uint64_t estimated_regular_working_set_bytes(ImageDimensions dimensions, const AppConfig& cfg,
                                                 std::uint64_t compressed_bytes = 0) {
  return estimated_regular_memory(dimensions, cfg, compressed_bytes).bytes;
}

ImageMemoryEstimate refined_regular_memory(const ImageDecodeResult& decoded,
    const NativeEncodeSettings& settings, const AppConfig& original, std::uint64_t compressed_bytes) {
  const auto& image = decoded.image;
  auto cfg = original;
  cfg.bit_depth = settings.bit_depth;
  cfg.chroma_mode = settings.chroma_mode;
  cfg.alpha_policy = settings.applied_alpha == "kept" ? AlphaModePolicy::force : AlphaModePolicy::off;
  cfg.image_size_limit.mode = ImageSizeLimitMode::none;
  cfg.max_jobs = settings.resources.encoder_threads_per_file;
  auto dimensions = make_image_dimensions(static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height));
  dimensions.source_bit_depth = image.bit_depth;
  dimensions.source_has_alpha = settings.applied_alpha == "kept";
  const auto signal = hdr::has_explicit_hdr_signal(image);
  dimensions.source_dynamic_range = signal && !*signal ? ImageDynamicRangeHint::sdr : ImageDynamicRangeHint::hdr;
  dimensions.source_has_gain_map = decoded.gain_map_alternative.has_value();
  dimensions.opaque_sdr8_yuv420 = image.pixel_format == PixelFormat::yuv420 && image.bit_depth == 8 &&
      image.alpha_mode == AlphaMode::none && dimensions.source_dynamic_range == ImageDynamicRangeHint::sdr;
  auto source_bytes = resident_image_bytes(image);
  if (decoded.gain_map_alternative) source_bytes = hdr_cost::add(source_bytes, resident_image_bytes(decoded.gain_map_alternative->image));
  dimensions.decoded_storage_bytes = source_bytes;
  const auto chroma = settings.chroma_mode == ChromaMode::auto_keep ? avif_auto_chroma(image.source_info ? image.source_info->pixel_format : image.pixel_format) : settings.chroma_mode;
  const auto format = chroma == ChromaMode::yuv420 ? PixelFormat::yuv420 : chroma == ChromaMode::yuv422 ? PixelFormat::yuv422 : chroma == ChromaMode::yuv400 ? PixelFormat::gray : PixelFormat::yuv444;
  const PixelConversionRequest conversion{.format = format, .bit_depth = settings.bit_depth.value_or(10),
      .matrix_coefficients = settings.applied_matrix_coefficients.value_or(6), .color_range = settings.applied_color_range.value_or(1),
      .preserve_alpha = settings.applied_alpha == "kept"};
  const ResolvedMemoryInput actual{source_bytes, image.source_info ? image.source_info->pixel_format : image.pixel_format,
      settings.output_format != OutputFormat::avif || !pixels_match(image, conversion)};
  return estimated_regular_memory(dimensions, cfg, compressed_bytes, settings.avif_grid_plan.has_value(), &actual);
}

void store_memory_estimate(EncodeResult& result, const ImageMemoryEstimate& e) {
  result.estimated_working_set_bytes = e.bytes;
  result.memory_estimate_profile = e.profile;
  result.memory_source_width = e.source_width; result.memory_source_height = e.source_height;
  result.memory_target_width = e.target_width; result.memory_target_height = e.target_height;
  result.memory_source_has_gain_map = e.source_has_gain_map;
  auto& plan = result.memory_plan;
  plan.refined = e.refined;
  plan.reserved_peak_bytes = 0;
  if (!e.refined) plan.initial_bytes = e.bytes;
  plan.mandatory_bytes = e.mandatory_bytes; plan.conversion_bytes = e.conversion_bytes;
  plan.codec_workspace_bytes = e.codec_workspace_bytes; plan.output_capacity_bytes = e.output_capacity_bytes;
  plan.layer_retained_bytes = e.layer_retained_bytes;
  plan.decode_bytes = e.decode_bytes; plan.prepare_bytes = e.prepare_bytes;
  plan.encode_bytes = e.encode_bytes; plan.metric_bytes = e.metric_bytes; plan.output_bytes = e.output_bytes;
  plan.target_bit_depth = e.target_bit_depth; plan.target_chroma = chroma_mode_name(e.target_chroma);
  plan.target_alpha = e.target_alpha;
}

} // namespace awj
