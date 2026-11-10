module;
#include "../compat/stdlib_compat.h"
#include "hdr_cost.h"
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
  // 搜索候选编码期间仍存活的视觉缓存与保留候选码流。
  std::uint64_t visual_encode_retained_bytes{};
  // 独立物化的稳定 RGB 参考；原图仍由 source_resident_bytes 单独计费。
  std::uint64_t visual_reference_bytes{};
  // 可选缓存仅在准入额度内选择，不作为硬性必要存储。
  std::uint64_t geometry_minimum_scratch_bytes{};
  std::uint64_t geometry_preferred_scratch_bytes{};
  std::uint64_t geometry_scratch_bytes{};
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
  std::uint64_t visual_reference_bytes{};
};

std::uint64_t resident_image_bytes(const ImageBuffer& image) noexcept {
  return pixel_detail::resident_image_bytes(image);
}

std::uint64_t estimated_visual_reference_bytes(const ImageBuffer& image, int bit_depth) noexcept {
  const auto pixels = hdr_cost::multiply(hdr_cost::multiply(image.width, image.height), bit_depth > 8 ? 8u : 4u);
  const auto combined = pixel_detail::geometry_resident_bytes(image, pixels);
  if (combined == UINT64_MAX) return UINT64_MAX;
  // 转换会复制元数据及 source_info；为更新后的颜色来源字符串保留小额余量。
  // 参考创建后仍须用实际 capacity 精化，不把这一模型当作分配器硬上界。
  return hdr_cost::add(combined - resident_image_bytes(image), 64);
}

ImageMemoryEstimate estimated_regular_memory(ImageDimensions dimensions, const AppConfig& cfg,
    std::uint64_t compressed_bytes = 0, bool grid = false,
    const ResolvedMemoryInput* resolved = nullptr, std::uint64_t memory_budget = UINT64_MAX) {
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
  const auto fixed = regular_memory_fixed_bytes(cfg.max_jobs);
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
  // 文件探测没有采样表示信息，高位深/未知源保守预留独立参考。
  // 此推测只进入后续编码峰值，不阻断普通整数源的独占解码精化机会。
  const bool possible_visual_reference = cfg.visual_quality && !grid && !hdr_jpeg && !sdr8;
  const auto reference_depth_bytes = cfg.output_format == OutputFormat::webp || cfg.output_format == OutputFormat::jpgli ? 4u : 8u;
  e.visual_reference_bytes = !cfg.visual_quality || grid ? 0 : resolved ? resolved->visual_reference_bytes :
      possible_visual_reference ? add(multiply(t, reference_depth_bytes),
          add(dimensions.source_icc_bytes, sizeof(ImagePlane) + 64)) : 0;
  // 可选缓存仅在已解析的会话预算内规划；不足时报告串行最低需求，
  // 只有最低需求也不足才可在解码前拒绝。
  if (!memory_budget || cfg.unlock_memory_limit) memory_budget = UINT64_MAX;
  e.prepare_bytes = add(common, fixed);
  if (!resolved && geometry->changes()) {
    const auto live = add(add(common, resized_source), fixed);
    const auto with_metadata = add(live, dimensions.source_has_gain_map ? dimensions.source_icc_bytes : 0);
    if (geometry->resamples()) {
      const auto scratch = geometry_scratch_requirements(dimensions.width, dimensions.height,
          geometry->content_width, geometry->content_height, std::max(1, cfg.max_jobs),
          with_metadata < memory_budget ? memory_budget - with_metadata : 0);
      e.geometry_minimum_scratch_bytes = scratch.minimum_bytes;
      e.geometry_preferred_scratch_bytes = scratch.preferred_bytes;
      e.geometry_scratch_bytes = scratch.selected_bytes;
    }
    e.prepare_bytes = add(live, e.geometry_scratch_bytes);
  }
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
      bool lossless_layer = cfg.visual_quality.value_or(cfg.quality) >= 100;
      for (const auto& layer : cfg.avif_layers.layers)
        lossless_layer = lossless_layer || layer.quality.value_or(0) >= 100 || layer.alpha_quality.value_or(0) >= 100;
      workspace = avif_codec_workspace_bytes(padded, padded_planes, cfg.max_jobs, depth,
          target_hdr, cfg.speed.value_or(5) <= 3 || lossless_layer);
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
            const auto preview = add(multiply(multiply(w, h), known && dimensions.source_bit_depth <= 8 ? 4 : 8),
                pixel_detail::destination_pixel_bytes(w, h, conversion, possible_alpha ? AlphaMode::straight : AlphaMode::none));
            // 手动预览物化期间，全分辨率转换结果、SVC 状态、引用及输出流仍存活。
            const auto live = add(add(add(encoder_live, destination), dual_storage),
                add(add(e.layer_retained_bytes, workspace),
                    add(multiply(output_bound, 3ull * static_cast<std::uint64_t>(count)), fixed)));
            const auto base = add(live, preview);
            // 稳定参考、视觉缓存与上一候选码流也会跨越下一次编码，
            // 必须一起先扣除再选择可选缓存；完整阶段峰值仍在下方各加一次。
            const auto visual_retained = cfg.visual_quality && !grid ? add(
                visual_quality_working_set_bytes_for_dimensions(target) / 2,
                multiply(output_bound, static_cast<std::uint64_t>(count))) : 0;
            const auto selection_base = add(base, add(visual_retained, e.visual_reference_bytes));
            const auto scratch = w == target.width && h == target.height ? 0 :
                geometry_scratch_requirements(target.width, target.height, w, h, std::max(1, cfg.max_jobs),
                    selection_base < memory_budget ? memory_budget - selection_base : 0).selected_bytes;
            largest_preview = std::max(largest_preview, add(preview, scratch));
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
  e.mandatory_bytes = add(add(encoder_live, fixed), resolved ? e.visual_reference_bytes : 0);
  // libavif/各 codec 的完整码流和返回副本可能在 codec 状态释放前共存，不能假设互斥。
  // 原图已在 encoder_live，独立稳定参考仅在这里加入一次，不能挪用指标余量抵消。
  const auto retained = add(add(encoder_live, e.layer_retained_bytes), e.visual_reference_bytes);
  e.encode_bytes = std::max(e.encode_bytes, add(add(retained, workspace), add(multiply(output_bound, 3), fixed)));
  if (dual && cfg.output_format != OutputFormat::jpgli)
    e.encode_bytes = std::max(e.encode_bytes, add(add(retained, multiply(t, 96)), add(multiply(output_bound, 3), fixed)));
  e.output_bytes = add(retained, add(multiply(output_bound, 3), fixed));
  if (cfg.visual_quality && !grid) {
    const auto metric = visual_quality_working_set_bytes_for_dimensions(target);
    e.visual_encode_retained_bytes = add(metric / 2, output_bound);
    e.encode_bytes = add(e.encode_bytes, e.visual_encode_retained_bytes);
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
    std::uint64_t compressed_bytes = 0, std::uint64_t memory_budget = UINT64_MAX) {
  return estimated_regular_memory(dimensions, cfg, compressed_bytes, false, nullptr, memory_budget).bytes;
}

ImageMemoryEstimate refined_regular_memory(const ImageDecodeResult& decoded,
    const NativeEncodeSettings& settings, const AppConfig& original, std::uint64_t compressed_bytes,
    std::uint64_t memory_budget = UINT64_MAX, std::uint64_t additional_resident_bytes = 0,
    const ImageBuffer* visual_reference = nullptr) {
  const auto& image = decoded.image;
  auto cfg = original;
  cfg.bit_depth = settings.bit_depth;
  cfg.chroma_mode = settings.chroma_mode;
  cfg.alpha_policy = settings.applied_alpha == "kept" ? AlphaModePolicy::force : AlphaModePolicy::off;
  cfg.image_size_limit.mode = ImageSizeLimitMode::none;
  cfg.max_jobs = per_file_thread_budget(settings.resources);
  auto dimensions = make_image_dimensions(static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height));
  dimensions.source_bit_depth = image.bit_depth;
  dimensions.source_has_alpha = settings.applied_alpha == "kept";
  const auto signal = hdr::has_explicit_hdr_signal(image);
  dimensions.source_dynamic_range = signal && !*signal ? ImageDynamicRangeHint::sdr : ImageDynamicRangeHint::hdr;
  dimensions.source_has_gain_map = decoded.gain_map_alternative.has_value();
  dimensions.opaque_sdr8_yuv420 = image.pixel_format == PixelFormat::yuv420 && image.bit_depth == 8 &&
      image.alpha_mode == AlphaMode::none && dimensions.source_dynamic_range == ImageDynamicRangeHint::sdr;
  auto source_bytes = hdr_cost::add(resident_image_bytes(image), additional_resident_bytes);
  if (decoded.gain_map_alternative) source_bytes = hdr_cost::add(source_bytes, resident_image_bytes(decoded.gain_map_alternative->image));
  dimensions.decoded_storage_bytes = source_bytes;
  const auto chroma = settings.chroma_mode == ChromaMode::auto_keep ? avif_auto_chroma(image.source_info ? image.source_info->pixel_format : image.pixel_format) : settings.chroma_mode;
  const auto format = chroma == ChromaMode::yuv420 ? PixelFormat::yuv420 : chroma == ChromaMode::yuv422 ? PixelFormat::yuv422 : chroma == ChromaMode::yuv400 ? PixelFormat::gray : PixelFormat::yuv444;
  const PixelConversionRequest conversion{.format = format, .bit_depth = settings.bit_depth.value_or(10),
      .matrix_coefficients = settings.applied_matrix_coefficients.value_or(6), .color_range = settings.applied_color_range.value_or(1),
      .preserve_alpha = settings.applied_alpha == "kept"};
  const bool hdr_jpeg = settings.output_format == OutputFormat::jpgli && original.jpeg_hdr != JpegHdrMode::sdr && signal && *signal;
  const bool tone_mapped_reference = (settings.output_format == OutputFormat::webp || settings.output_format == OutputFormat::jpgli) && signal && *signal;
  const bool reference_required = original.visual_quality && !settings.avif_grid_plan && !hdr_jpeg &&
      (visual_reference || tone_mapped_reference || image.sample_representation == SampleRepresentation::ieee_half_float);
  const auto reference_bytes = reference_required ? (visual_reference ? resident_image_bytes(*visual_reference) :
      estimated_visual_reference_bytes(image, tone_mapped_reference ? 8 : 16)) : 0;
  const ResolvedMemoryInput actual{source_bytes, image.source_info ? image.source_info->pixel_format : image.pixel_format,
      settings.output_format != OutputFormat::avif || !pixels_match(image, conversion), reference_bytes};
  return estimated_regular_memory(dimensions, cfg, compressed_bytes, settings.avif_grid_plan.has_value(), &actual, memory_budget);
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
