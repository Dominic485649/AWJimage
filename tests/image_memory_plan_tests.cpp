#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>
#include <cstdint>
#include <limits>
#include <string>
#include <memory>
#include <span>
#include <vector>
#include "../src/core/spline36_resample.h"

import awj.codec;
import awj.config;
import awj.image;
import awj.large_image_plan;
import awj.image_memory_plan;
import awj.pixel_conversion;
import awj.resource_planner;

namespace {
int check_geometry_memory() {
  const auto fail = [](const char* message) { std::fprintf(stderr, "%s\n", message); return 1; };
  using namespace awj::spline36_detail;
  const awj::memory_policy::Scope finite_policy{*awj::memory_policy::resolve(false, false, 1024ull * 1024 * 1024)};
  constexpr std::size_t width = 512, height = 256, tw = 257, th = 129;
  auto owner = std::make_shared<std::vector<std::byte>>(width * height * 4, std::byte{127});
  awj::ImageBuffer source{.width = width, .height = height, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 8};
  source.planes.push_back({.stride = width * 4, .borrowed = std::span<const std::byte>{*owner}, .lease = owner});
  source.planes[0].bytes.reserve(128); // Owned capacity remains live alongside a different borrowed allocation.
  source.metadata.push_back({awj::MetadataKind::xmp, std::vector<std::byte>(4096)});
  source.metadata[0].bytes.reserve(8192);
  const auto source_resident = awj::resident_image_bytes(source);
  if (source_resident < owner->size() + source.planes[0].bytes.capacity() + source.metadata[0].bytes.capacity() ||
      source_resident != awj::pixel_detail::resident_image_bytes(source)) return fail("借用平面/保留容量/元数据未统一计费。");
  const auto resident = awj::pixel_detail::geometry_resident_bytes(source, tw * th * 4);
  if (resident < source_resident + tw * th * 4 + source.metadata[0].bytes.capacity())
    return fail("几何准入遗漏了源图、输出或复制的元数据。");
  const auto full = make_plan(width, height, tw, th, 8);
  awj::ResourcePlan resources{.file_parallelism = 1, .encoder_threads_per_file = 8,
      .global_thread_budget = 8, .memory_limit_bytes = resident + full.bytes};
  awj::GeometryTransformDiagnostics diagnostic;
  auto parallel = awj::resize_native_pixels(source, tw, th, {}, resources, &diagnostic);
  if (!parallel || diagnostic.workers != 8 || diagnostic.scratch_bytes > full.bytes)
    return fail("满足预算的几何 worker 或 scratch 诊断错误。");
  const auto check_same = [&](const awj::ImageBuffer& other) { return parallel->planes[0].bytes == other.planes[0].bytes; };
  const auto minimum = full.coefficients + shared_control_bytes + worker_bytes(width, tw, 1, false);
  resources.memory_limit_bytes = resident + minimum;
  auto serial = awj::resize_native_pixels(source, tw, th, {}, resources, &diagnostic);
  if (!serial || !check_same(*serial) || diagnostic.workers != 1 || diagnostic.scratch_bytes > minimum)
    return fail("紧预算没有回退到一致的串行逐 tap 读取。");
  resources.memory_limit_bytes -= 1;
  diagnostic = {42, 99};
  if (awj::resize_native_pixels(source, tw, th, {}, resources, &diagnostic) || diagnostic.workers)
    return fail("必要存储不足未拒绝，或失败保留了旧 worker 诊断。");
  resources.memory_limit_bytes = resident - 1;
  if (awj::resize_native_pixels(source, tw, th, {}, resources)) return fail("仅输入输出常驻字节已超预算仍被接受。");
  resources.file_parallelism = 2;
  resources.global_thread_budget = 5;
  resources.memory_limit_bytes = 2 * (resident + full.bytes);
  auto capped = awj::resize_native_pixels(source, tw, th, {}, resources, &diagnostic);
  if (!capped || !check_same(*capped) || diagnostic.workers != 2)
    return fail("忽略了 global_thread_budget/file_parallelism 上限。");
  resources.memory_limit_bytes = 1; // Finite division truncating to zero must not become unlimited.
  if (awj::resize_native_pixels(source, tw, th, {}, resources)) return fail("有限预算除法的零余量变成了无限预算。");
  const auto parent = awj::memory_policy::current();
  {
    const awj::memory_policy::Scope unlocked{*awj::memory_policy::resolve(true, false, 0)};
    auto result = awj::resize_native_pixels(source, tw, th, {}, resources, &diagnostic);
    if (!result || !check_same(*result) || diagnostic.workers != 2) return fail("解除内存限制未覆盖旧有限资源计划。");
  }
  {
    const awj::memory_policy::Scope input_only{*awj::memory_policy::resolve(false, true, 0)};
    if (awj::resize_native_pixels(source, tw, th, {}, resources)) return fail("仅解除输入限制错误地解除工作集预算。");
  }
  if (awj::memory_policy::current() != parent) return fail("几何处理泄漏了线程内存策略。");
  auto cfg = awj::default_app_config();
  cfg.output_format = awj::OutputFormat::png;
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::fixed, .max_width = 256, .max_height = 128};
  auto dimensions = awj::make_image_dimensions(width, height);
  dimensions.source_bit_depth = 8; dimensions.source_dynamic_range = awj::ImageDynamicRangeHint::sdr;
  dimensions.decoded_storage_bytes = width * height * 4;
  const auto geometry = awj::plan_image_geometry(width, height, cfg);
  if (!geometry) return fail("几何预约夹具计划失败。");
  for (const int workers : {1, 2, 8}) {
    cfg.max_jobs = workers;
    const auto estimate = awj::estimated_regular_memory(dimensions, cfg);
    const auto expected = dimensions.decoded_storage_bytes + geometry->canvas_width * geometry->canvas_height * 4 +
        scratch_bytes(width, height, geometry->content_width, geometry->content_height, workers) +
        16ull * 1024 * 1024 + static_cast<std::uint64_t>(workers) * 256 * 1024;
    if (estimate.prepare_bytes != expected) return fail("阶段预约与几何 worker/系数/行缓存模型不一致。");
  }
  // 依据显式解析后的预算（含自动预算）选择计划，不受首选 max_jobs 缓存大小支配。
  auto wide = awj::make_image_dimensions(32768, 64);
  wide.source_bit_depth = 8;
  wide.source_dynamic_range = awj::ImageDynamicRangeHint::sdr;
  wide.decoded_storage_bytes = wide.pixel_count * 4;
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::manual, .scale_percent = 50};
  cfg.max_jobs = 8;
  constexpr auto budget = 64ull * 1024 * 1024;
  const auto preferred = awj::estimated_regular_memory(wide, cfg);
  const auto bounded = awj::estimated_regular_memory(wide, cfg, 0, false, nullptr, budget);
  if (preferred.prepare_bytes <= budget || bounded.bytes > budget ||
      bounded.geometry_minimum_scratch_bytes >= bounded.geometry_preferred_scratch_bytes ||
      bounded.geometry_scratch_bytes < bounded.geometry_minimum_scratch_bytes ||
      bounded.geometry_scratch_bytes >= bounded.geometry_preferred_scratch_bytes)
    return fail("宽矮图的首选缓存仍被当成必要存储，或预算选择没有传播到阶段预约。");
  cfg.max_jobs = 1;
  if (awj::estimated_regular_memory(wide, cfg, 0, false, nullptr, budget).bytes > budget)
    return fail("预算 1 的同一紧预算夹具无法进入预约。");
  cfg.max_jobs = 8;
  cfg.unlock_memory_limit = true;
  if (awj::estimated_regular_memory(wide, cfg, 0, false, nullptr, 1).geometry_scratch_bytes !=
      preferred.geometry_preferred_scratch_bytes)
    return fail("完整解除内存限制引入了新的有限 scratch 上限。");
  return 0;
}

int check_visual_reference_memory() {
  const auto fail = [](const char* message) { std::fprintf(stderr, "%s\n", message); return 1; };
  constexpr std::uint64_t width = 8192, height = 64;
  auto cfg = awj::default_app_config();
  cfg.output_format = awj::OutputFormat::avif;
  cfg.quality = 70;
  cfg.visual_quality = 90;
  cfg.speed = 10;
  cfg.max_jobs = 2;
  cfg.bit_depth = 12;
  cfg.chroma_mode = awj::ChromaMode::yuv444;
  cfg.alpha_policy = awj::AlphaModePolicy::off;
  cfg.image_size_limit.mode = awj::ImageSizeLimitMode::none;
  awj::ImageDecodeResult decoded;
  decoded.image = {.width = width, .height = height, .pixel_format = awj::PixelFormat::rgba,
      .bit_depth = 16, .sample_representation = awj::SampleRepresentation::ieee_half_float,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
          .color_primaries = 1, .transfer_characteristics = 8, .matrix_coefficients = 0,
          .color_range = 1, .has_hdr_metadata = true, .color_metadata_source = "wgc-scrgb-half-linear",
          .linear_reference_white_nits = 80.0F}};
  decoded.image.planes.push_back({.bytes = std::vector<std::byte>(width * height * 8), .stride = width * 8});
  decoded.image.metadata.push_back({awj::MetadataKind::xmp, std::vector<std::byte>(1024)});
  decoded.image.metadata.front().bytes.reserve(8192);
  awj::NativeEncodeSettings settings;
  settings.output_format = cfg.output_format;
  settings.bit_depth = cfg.bit_depth;
  settings.chroma_mode = cfg.chroma_mode;
  settings.resources = {.file_parallelism = 2, .encoder_threads_per_file = 8, .global_thread_budget = 5};
  const auto source_bytes = awj::resident_image_bytes(decoded.image);
  const auto predicted = awj::refined_regular_memory(decoded, settings, cfg, 0);
  if (predicted.source_resident_bytes != source_bytes ||
      predicted.visual_reference_bytes != awj::estimated_visual_reference_bytes(decoded.image, 16))
    return fail("FP16 原图与独立参考的容量没有分别进入精化模型。");

  auto reference = decoded.image;
  reference.sample_representation = awj::SampleRepresentation::unorm;
  reference.source_info->color_primaries = 9;
  reference.source_info->transfer_characteristics = 16;
  reference.planes.front().bytes.reserve(reference.planes.front().bytes.size() + 4096);
  const auto exact = awj::refined_regular_memory(decoded, settings, cfg, 0, UINT64_MAX, 0, &reference);
  decoded.image.sample_representation = awj::SampleRepresentation::unorm;
  decoded.image.source_info->color_primaries = 9;
  decoded.image.source_info->transfer_characteristics = 16;
  const auto without_reference = awj::refined_regular_memory(decoded, settings, cfg, 0);
  decoded.image.sample_representation = awj::SampleRepresentation::ieee_half_float;
  decoded.image.source_info->color_primaries = 1;
  decoded.image.source_info->transfer_characteristics = 8;
  const auto extra = awj::resident_image_bytes(reference);
  if (exact.visual_reference_bytes != extra || without_reference.visual_reference_bytes ||
      exact.source_resident_bytes != without_reference.source_resident_bytes ||
      exact.encode_bytes != without_reference.encode_bytes + extra ||
      exact.metric_bytes != without_reference.metric_bytes + extra ||
      exact.output_bytes != without_reference.output_bytes + extra ||
      exact.mandatory_bytes != without_reference.mandatory_bytes + extra ||
      exact.visual_encode_retained_bytes != without_reference.visual_encode_retained_bytes)
    return fail("稳定参考的实际容量未恰好计费一次，或被指标缓存余量抵消。");

  cfg.avif_layers.count = 2;
  cfg.avif_layers.layers[0].scale = std::pair{1u, 2u};
  settings.avif_layers = cfg.avif_layers;
  const auto preferred = awj::refined_regular_memory(decoded, settings, cfg, 0, UINT64_MAX, 0, &reference);
  const auto scratch = awj::geometry_scratch_requirements(width, height, width / 2, height / 2, 2);
  const auto fixed = preferred.encode_bytes - scratch.preferred_bytes;
  const auto ticket = fixed + scratch.minimum_bytes + 64 * 1024;
  const auto bounded = awj::refined_regular_memory(decoded, settings, cfg, 0, ticket, 0, &reference);
  const auto selected_scratch = bounded.layer_retained_bytes -
      (preferred.layer_retained_bytes - scratch.preferred_bytes);
  if (preferred.bytes <= ticket || bounded.bytes > ticket || selected_scratch < scratch.minimum_bytes ||
      selected_scratch > ticket - fixed || selected_scratch >= scratch.preferred_bytes ||
      bounded.visual_reference_bytes != extra || bounded.encode_bytes != fixed + selected_scratch)
    return fail("FP16 稳定参考尚未在手动预览 scratch 选择前扣除，或紧 ticket 没有降缓存。");
  if (awj::refined_regular_memory(decoded, settings, cfg, 0, fixed + scratch.minimum_bytes - 1,
          0, &reference).bytes <= fixed + scratch.minimum_bytes - 1)
    return fail("FP16 原图、参考与最低预览存储真正不足时仍被模型准入。");

  auto dimensions = awj::make_image_dimensions(width, height);
  dimensions.source_bit_depth = 16;
  dimensions.source_dynamic_range = awj::ImageDynamicRangeHint::hdr;
  dimensions.decoded_storage_bytes = source_bytes;
  const auto probe = awj::estimated_regular_memory(dimensions, cfg);
  auto ordinary_cfg = cfg;
  ordinary_cfg.visual_quality.reset();
  const auto ordinary = awj::estimated_regular_memory(dimensions, ordinary_cfg);
  if (!probe.visual_reference_bytes || probe.decode_bytes != ordinary.decode_bytes ||
      probe.prepare_bytes != ordinary.prepare_bytes || probe.mandatory_bytes != ordinary.mandatory_bytes ||
      probe.bytes <= std::max({probe.decode_bytes, probe.prepare_bytes, probe.mandatory_bytes}))
    return fail("未知采样类型的参考推测阻断了普通 16-bit 整数源的解码后精化机会。");
  decoded.image.sample_representation = awj::SampleRepresentation::unorm;
  decoded.image.source_info->color_primaries = 9;
  decoded.image.source_info->transfer_characteristics = 16;
  const auto integer_preferred = awj::refined_regular_memory(decoded, settings, cfg, 0);
  const auto integer_ticket = integer_preferred.encode_bytes - scratch.preferred_bytes + scratch.minimum_bytes + 64 * 1024;
  const auto integer_bounded = awj::refined_regular_memory(decoded, settings, cfg, 0, integer_ticket);
  const auto conservative = awj::estimated_regular_memory(dimensions, cfg, 0, false, nullptr, integer_ticket);
  decoded.image.sample_representation = awj::SampleRepresentation::ieee_half_float;
  decoded.image.source_info->color_primaries = 1;
  decoded.image.source_info->transfer_characteristics = 8;
  if (integer_bounded.bytes > integer_ticket || integer_bounded.visual_reference_bytes ||
      conservative.bytes <= integer_ticket || conservative.bytes == UINT64_MAX ||
      conservative.decode_bytes > integer_ticket || conservative.prepare_bytes > integer_ticket ||
      conservative.mandatory_bytes > integer_ticket)
    return fail("普通 16-bit 整数输入没有保留先独占解码、再移除推测参考的准入通路。");
  const awj::ResolvedMemoryInput overflow{source_bytes, awj::PixelFormat::rgba, true, UINT64_MAX};
  if (awj::estimated_regular_memory(dimensions, cfg, 0, false, &overflow).bytes != UINT64_MAX)
    return fail("独立视觉参考的存储溢出未饱和拒绝。");
  dimensions.source_bit_depth = 0;
  dimensions.source_dynamic_range = awj::ImageDynamicRangeHint::unknown;
  dimensions.decoded_storage_bytes = 0;
  if (!awj::estimated_regular_memory(dimensions, cfg).visual_reference_bytes)
    return fail("未知源没有保守预估独立视觉参考。");

  auto tone_cfg = cfg;
  tone_cfg.output_format = awj::OutputFormat::webp;
  tone_cfg.avif_layers = {};
  auto tone_settings = settings;
  tone_settings.output_format = tone_cfg.output_format;
  tone_settings.avif_layers = {};
  const auto tone = awj::refined_regular_memory(decoded, tone_settings, tone_cfg, 0);
  if (tone.visual_reference_bytes != awj::estimated_visual_reference_bytes(decoded.image, 8) ||
      tone.source_resident_bytes != source_bytes)
    return fail("色调映射的稳定 RGBA8 参考与仍存活 FP16 原图未独立计费。");
  return 0;
}
} // namespace

int main() {
  if (const auto result = check_geometry_memory()) return result;
  if (const auto result = check_visual_reference_memory()) return result;
  const auto fail = [](const char* message) { std::fprintf(stderr, "%s\n", message); return 1; };
  auto cfg = awj::default_app_config();
  cfg.max_jobs = 12;
  cfg.image_size_limit.mode = awj::ImageSizeLimitMode::none;
  auto dims = awj::make_image_dimensions(9233, 6527);
  dims.source_bit_depth = 8; dims.source_has_alpha = true;
  dims.source_dynamic_range = awj::ImageDynamicRangeHint::sdr;
  dims.decoded_storage_bytes = dims.pixel_count * 4;
  const auto probe = awj::estimated_regular_memory(dims, cfg, 1301294);
  if (probe.conversion_bytes != dims.pixel_count * 8 || probe.source_resident_bytes != dims.pixel_count * 4)
    return fail("RGBA8→AVIF10 的源图或真实目标平面计费不符。");
  if (probe.bytes != std::max({probe.decode_bytes, probe.prepare_bytes, probe.encode_bytes, probe.metric_bytes, probe.output_bytes}))
    return fail("总预约不等于各阶段同时存活峰值的最大值。");
  auto opaque_cfg = cfg;
  opaque_cfg.alpha_policy = awj::AlphaModePolicy::off;
  const awj::ResolvedMemoryInput actual{dims.pixel_count * 4, awj::PixelFormat::rgba, true};
  const auto opaque = awj::estimated_regular_memory(dims, opaque_cfg, 1301294, false, &actual);
  if (!opaque.refined || opaque.decode_bytes || opaque.target_alpha ||
      opaque.conversion_bytes != dims.pixel_count * 6 || opaque.bytes >= probe.bytes)
    return fail("全不透明 alpha 精化未移除目标 alpha，或再次计入过去的解码峰值。");
  if (opaque.bytes > 6578ull * 1024 * 1024)
    return fail("课表普通编码精化计划仍无法进入原有限预算。");
  if (opaque.mandatory_bytes >= opaque.bytes || !opaque.codec_workspace_bytes || !opaque.output_capacity_bytes)
    return fail("事实像素、codec 余量与输出容量未独立表达。");
  std::printf("课表 v2 probe=%llu prepared=%llu source=%llu conversion=%llu codec=%llu output=%llu\n",
      static_cast<unsigned long long>(probe.bytes), static_cast<unsigned long long>(opaque.bytes),
      static_cast<unsigned long long>(opaque.source_resident_bytes), static_cast<unsigned long long>(opaque.conversion_bytes),
      static_cast<unsigned long long>(opaque.codec_workspace_bytes), static_cast<unsigned long long>(opaque.output_capacity_bytes));

  auto native_dims = awj::make_image_dimensions(129, 97);
  native_dims.source_bit_depth = 8; native_dims.source_dynamic_range = awj::ImageDynamicRangeHint::sdr;
  native_dims.opaque_sdr8_yuv420 = true;
  native_dims.decoded_storage_bytes = 129 * 97 + 2 * 65 * 49;
  auto native_cfg = opaque_cfg; native_cfg.bit_depth = 8; native_cfg.chroma_mode = awj::ChromaMode::yuv420;
  const awj::ResolvedMemoryInput native{native_dims.decoded_storage_bytes, awj::PixelFormat::yuv420, false};
  const auto reused = awj::estimated_regular_memory(native_dims, native_cfg, 0, false, &native);
  if (reused.conversion_bytes) return fail("兼容 native YUV 共享视图被重复计费。");
  auto converted = native; converted.conversion_required = true;
  const auto planes = awj::estimated_regular_memory(native_dims, native_cfg, 0, false, &converted);
  if (planes.conversion_bytes != native_dims.decoded_storage_bytes)
    return fail("奇数宽高 420 目标平面未按 ceil 采样计费。");
  native_cfg.chroma_mode = awj::ChromaMode::yuv422;
  const auto planes422 = awj::estimated_regular_memory(native_dims, native_cfg, 0, false, &converted);
  if (planes422.conversion_bytes != 129 * 97 + 2 * 65 * 97)
    return fail("奇数宽度 422 目标平面计费错误。");

  auto layered_cfg = opaque_cfg; layered_cfg.avif_layers.automatic = true;
  const auto layered = awj::estimated_regular_memory(dims, layered_cfg, 1301294, false, &actual);
  if (layered.bytes <= opaque.bytes || !layered.layer_retained_bytes ||
      layered.codec_workspace_bytes <= opaque.codec_workspace_bytes)
    return fail("渐进层保留状态与 SVC 工作区未计入。");
  if (layered.bytes < 7494823936ull)
    return fail("分层计划低于本轮 AOM 3.15.3 课表实测 Commit。");
  auto visual_cfg = opaque_cfg; visual_cfg.visual_quality = 90;
  const auto visual = awj::estimated_regular_memory(dims, visual_cfg, 0, false, &actual);
  if (!visual.metric_bytes || !visual.visual_encode_retained_bytes ||
      visual.bytes <= opaque.bytes || opaque.visual_encode_retained_bytes)
    return fail("视觉搜索缓存/指标峰值丢失，或普通编码伪造了视觉保留量。");
  auto high = dims; high.source_bit_depth = 16; high.source_dynamic_range = awj::ImageDynamicRangeHint::hdr;
  high.decoded_storage_bytes = dims.pixel_count * 8;
  const auto hdr = awj::estimated_regular_memory(high, cfg);
  if (hdr.target_bit_depth != 12 || hdr.source_resident_bytes < dims.pixel_count * 8)
    return fail("HDR 精度或源存储被降估。");
  auto unknown = dims; unknown.source_bit_depth = 0; unknown.source_dynamic_range = awj::ImageDynamicRangeHint::unknown;
  unknown.decoded_storage_bytes = 0;
  if (awj::estimated_regular_memory(unknown, cfg).source_resident_bytes < dims.pixel_count * 16)
    return fail("未知动态范围被当作小型 SDR 输入。");
  auto gain = high; gain.source_has_gain_map = true; gain.gain_map_width = 2309; gain.gain_map_height = 1632;
  if (awj::estimated_regular_memory(gain, cfg).bytes <= hdr.bytes) return fail("Gain Map 双端与辅助工作区未计入。");
  auto jxl_cfg = cfg; jxl_cfg.output_format = awj::OutputFormat::jxl;
  const std::array<std::pair<int, int>, 7> jxl_depths{{
      {0, 16}, {8, 10}, {10, 10}, {11, 12}, {12, 12}, {13, 16}, {16, 16}}};
  for (const auto [source_depth, target_depth] : jxl_depths) {
    auto jxl_dims = dims; jxl_dims.source_bit_depth = source_depth;
    if (awj::estimated_regular_memory(jxl_dims, jxl_cfg).target_bit_depth != target_depth)
      return fail("JXL 内存预检与自动位深矩阵不一致。");
  }
  auto jxl_gain = dims; jxl_gain.source_has_gain_map = true;
  if (awj::estimated_regular_memory(jxl_gain, jxl_cfg).target_bit_depth != 16)
    return fail("JXL 重建 HDR 被原 8-bit 底图低估。");
  jxl_cfg.bit_depth = 8;
  if (awj::estimated_regular_memory(jxl_gain, jxl_cfg).target_bit_depth != 8)
    return fail("JXL 显式位深未覆盖内存预检自动值。");

  auto invalid = dims; invalid.width = 0;
  if (awj::estimated_regular_memory(invalid, cfg).bytes != UINT64_MAX) return fail("非法尺寸没有安全拒绝。");
  invalid = awj::make_image_dimensions(UINT32_MAX, UINT32_MAX);
  if (awj::estimated_regular_memory(invalid, cfg).bytes != UINT64_MAX) return fail("尺寸乘法溢出没有饱和拒绝。");
  return 0;
}
