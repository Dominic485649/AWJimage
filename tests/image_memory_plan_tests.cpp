#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>

import awj.config;
import awj.image;
import awj.large_image_plan;
import awj.image_memory_plan;

int main() {
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
  if (!visual.metric_bytes || visual.bytes <= opaque.bytes) return fail("视觉搜索缓存/指标峰值丢失。");
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
  auto invalid = dims; invalid.width = 0;
  if (awj::estimated_regular_memory(invalid, cfg).bytes != UINT64_MAX) return fail("非法尺寸没有安全拒绝。");
  invalid = awj::make_image_dimensions(UINT32_MAX, UINT32_MAX);
  if (awj::estimated_regular_memory(invalid, cfg).bytes != UINT64_MAX) return fail("尺寸乘法溢出没有饱和拒绝。");
  return 0;
}
