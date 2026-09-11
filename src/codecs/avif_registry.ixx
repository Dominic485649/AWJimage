module;

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module awj.avif_registry;

import awj.codec;
import awj.config;
import awj.encoding_defaults;

export namespace awj {

namespace avif_registry_detail {

bool contains_chroma(const AvifEncoderCapability& capability, ChromaMode chroma) {
  return std::ranges::find(capability.chroma_modes, chroma) != capability.chroma_modes.end();
}

bool contains_bit_depth(const AvifEncoderCapability& capability, int bit_depth) {
  return std::ranges::find(capability.bit_depths, bit_depth) != capability.bit_depths.end();
}

std::optional<int> max_bit_depth_for(const AvifEncoderCapability& capability) {
  if (capability.bit_depths.empty()) {
    return {};
  }
  return *std::ranges::max_element(capability.bit_depths);
}

std::optional<int> nearest_supported_bit_depth_not_exceeding(
    const AvifEncoderCapability& capability,
    int requested) {
  std::optional<int> best{};
  for (const int bit_depth : capability.bit_depths) {
    if (bit_depth <= requested && (!best || bit_depth > *best)) {
      best = bit_depth;
    }
  }
  return best;
}

ChromaMode applied_chroma_for(const AvifEncoderCapability& capability,
                              ChromaMode requested) {
  if (requested == ChromaMode::auto_keep) {
    return contains_chroma(capability, ChromaMode::yuv420) ? ChromaMode::yuv420
                                                          : capability.chroma_modes.front();
  }
  return requested;
}

struct AppliedBitDepth {
  std::optional<int> value{};
  std::string reason{};
};

AppliedBitDepth applied_bit_depth_for(const AvifEncoderCapability& capability,
                                      const AvifEncoderSelectionRequest& request) {
  if (request.requested_bit_depth) {
    const int requested = *request.requested_bit_depth;
    const auto requested_reason =
        request.requested_bit_depth_reason.empty()
            ? (request.requested_bit_depth_explicit ? std::string{"用户明确请求 bit-depth"}
                                                    : std::format("源图继承 {}-bit bit-depth", requested))
            : request.requested_bit_depth_reason;
    if (contains_bit_depth(capability, requested) || request.requested_bit_depth_explicit) {
      return AppliedBitDepth{.value = requested, .reason = requested_reason};
    }
    const auto clamped = nearest_supported_bit_depth_not_exceeding(capability, requested);
    if (clamped) {
      const auto max_supported = max_bit_depth_for(capability);
      const auto clamp_reason =
          max_supported && requested > *max_supported
              ? std::format("源图 {}-bit 超过 {} 支持上限，限制为 {}-bit 输出",
                            requested, capability.id, *clamped)
              : std::format("源图 {}-bit 不受 {} 支持，限制为 {}-bit 输出",
                            requested, capability.id, *clamped);
      return AppliedBitDepth{.value = *clamped,
                             .reason = requested_reason.empty()
                                           ? clamp_reason
                                           : std::format("{}；{}", requested_reason, clamp_reason)};
    }
    return AppliedBitDepth{.value = requested, .reason = requested_reason};
  }
  if (contains_bit_depth(capability, 10)) {
    return AppliedBitDepth{.value = 10,
                           .reason = "auto 选择首选 10-bit 输出"};
  }
  if (contains_bit_depth(capability, 8)) {
    return AppliedBitDepth{.value = 8,
                           .reason = "auto 回退到 8-bit，因为当前编码器不支持 10-bit"};
  }
  return AppliedBitDepth{.value = capability.bit_depths.empty()
                                      ? std::optional<int>{}
                                      : std::optional<int>{capability.bit_depths.front()},
                         .reason = "auto 选择第一个受支持 bit-depth"};
}

bool capability_matches(const AvifEncoderCapability& capability,
                        const AvifEncoderSelectionRequest& request) {
  if (!capability.enabled) {
    return false;
  }
  if (request.must_preserve_alpha && !capability.supports_alpha) {
    return false;
  }
  const auto chroma = applied_chroma_for(capability, request.requested_chroma);
  if (!contains_chroma(capability, chroma)) {
    return false;
  }
  const auto bit_depth = applied_bit_depth_for(capability, request);
  if (bit_depth.value && !contains_bit_depth(capability, *bit_depth.value)) {
    return false;
  }
  if (capability.max_single_image_width && request.width > *capability.max_single_image_width) {
    return false;
  }
  if (capability.max_single_image_height && request.height > *capability.max_single_image_height) {
    return false;
  }
  if (capability.mode == AvifEncoderMode::aom &&
      request.pixel_count > encoding_defaults::avif_single_image_max_pixels) {
    return false;
  }
  return true;
}

std::string explicit_rejection_reason(const AvifEncoderCapability& capability,
                                      const AvifEncoderSelectionRequest& request) {
  if (!capability.enabled) {
    if (!capability.unavailable_reason.empty()) {
      return capability.unavailable_reason;
    }
    return std::format("AVIF encoder {} 在当前构建中不可用。",
                       capability.id);
  }

  if (request.must_preserve_alpha && !capability.supports_alpha) {
    return std::format("AVIF encoder {} 不支持保留 alpha。", capability.id);
  }
  const auto chroma = applied_chroma_for(capability, request.requested_chroma);
  if (!contains_chroma(capability, chroma)) {
    return std::format("AVIF encoder {} 不支持请求的 chroma {}。",
                       capability.id, chroma_mode_name(request.requested_chroma));
  }
  const auto bit_depth = applied_bit_depth_for(capability, request);
  if (bit_depth.value && !contains_bit_depth(capability, *bit_depth.value)) {
    return std::format("AVIF encoder {} 不支持请求的 {}-bit 输出。",
                       capability.id, *bit_depth.value);
  }
  if (capability.max_single_image_width && request.width > *capability.max_single_image_width) {
    return std::format("AVIF encoder {} 不支持输入宽度 {} 超过 {}。",
                       capability.id, request.width, *capability.max_single_image_width);
  }
  if (capability.max_single_image_height && request.height > *capability.max_single_image_height) {
    return std::format("AVIF encoder {} 不支持输入高度 {} 超过 {}。",
                       capability.id, request.height, *capability.max_single_image_height);
  }
  if (capability.mode == AvifEncoderMode::aom &&
      request.pixel_count > encoding_defaults::avif_single_image_max_pixels) {
    return std::format("AOM/libaom AVIF 单图上限为 65536 边 / {} 像素；将自动走 AOM Grid 大图链路。",
                       encoding_defaults::avif_single_image_max_pixels);
  }
  return std::format("AVIF encoder {} 不适用于当前请求。", capability.id);
}

AvifEncoderSelection make_selection(const AvifEncoderCapability& capability,
                                    const AvifEncoderSelectionRequest& request,
                                    std::string fallback_reason) {
  const auto bit_depth = applied_bit_depth_for(capability, request);
  return AvifEncoderSelection{
      .requested_encoder = request.requested_encoder,
      .applied_encoder = capability.mode,
      .requested_chroma = request.requested_chroma,
      .applied_chroma = applied_chroma_for(capability, request.requested_chroma),
      .requested_bit_depth = request.requested_bit_depth,
      .applied_bit_depth = bit_depth.value,
      .bit_depth_reason = std::move(bit_depth.reason),
      .pixel_count = request.pixel_count,
      .speed = request.speed_explicit ? request.speed : capability.default_speed,
      .license = capability.license,
      .fallback_reason = std::move(fallback_reason)};
}

const AvifEncoderCapability* find_capability(std::span<const AvifEncoderCapability> capabilities,
                                             AvifEncoderMode mode) {
  const auto it = std::ranges::find(capabilities, mode, &AvifEncoderCapability::mode);
  return it == capabilities.end() ? nullptr : &*it;
}

}  // namespace awj_registry_detail

std::vector<AvifEncoderCapability> avif_encoder_capabilities_for_build(bool aom_available) {
  return {AvifEncoderCapability{
      .mode = AvifEncoderMode::aom,
      .id = "aom",
      .chroma_modes = {ChromaMode::yuv420, ChromaMode::yuv422, ChromaMode::yuv444},
      .bit_depths = {8, 10, 12},
      .supports_alpha = true,
      .supports_avif_grid = true,
      .max_single_image_width = encoding_defaults::avif_single_image_max_dimension,
      .max_single_image_height = encoding_defaults::avif_single_image_max_dimension,
      .enabled = aom_available,
      .unavailable_reason = aom_available ? "" : "当前构建没有可用的 AOM 编码器。",
      .license = "BSD-2-Clause",
      .default_speed = encoding_defaults::default_aom_cpu_used}};
}

std::vector<AvifEncoderCapability> avif_encoder_capabilities() {
  return avif_encoder_capabilities_for_build(true);
}

std::expected<AvifEncoderSelection, std::string> select_avif_encoder_from_capabilities(
    const AvifEncoderSelectionRequest& request,
    std::span<const AvifEncoderCapability> capabilities) {
  if (request.requested_bit_depth && request.requested_bit_depth_explicit &&
      *request.requested_bit_depth > 12) {
    return std::unexpected{std::format("AVIF encoder 不支持请求的 {}-bit 输出。",
                                       *request.requested_bit_depth)};
  }
  const auto mode = request.requested_encoder == AvifEncoderMode::automatic
                        ? AvifEncoderMode::aom : request.requested_encoder;
  const auto* capability = avif_registry_detail::find_capability(capabilities, mode);
  if (!capability) return std::unexpected{"请求的 AVIF 编码器已移除或未注册；请使用 auto/aom。"};
  if (!avif_registry_detail::capability_matches(*capability, request)) {
    return std::unexpected{avif_registry_detail::explicit_rejection_reason(*capability, request)};
  }
  return avif_registry_detail::make_selection(*capability, request, {});
}

std::expected<AvifEncoderSelection, std::string> select_avif_encoder(
    const AvifEncoderSelectionRequest& request) {
  const auto capabilities = avif_encoder_capabilities();
  return select_avif_encoder_from_capabilities(request, capabilities);
}

SpeedMapping avif_speed_mapping_for(const AvifEncoderSelection& selection) {
  return SpeedMapping{.user_speed = selection.speed,
                      .codec_value = selection.speed,
                      .codec_key = "aom:cpu-used"};
}

EncodeDiagnostics diagnostics_from_avif_selection(
    const AvifEncoderSelection& selection) {
  return EncodeDiagnostics{.encoder_id = avif_encoder_mode_name(selection.applied_encoder),
                           .requested_encoder_id = avif_encoder_mode_name(selection.requested_encoder),
                           .requested_chroma = chroma_mode_name(selection.requested_chroma),
                           .applied_chroma = chroma_mode_name(selection.applied_chroma),
                           .requested_bit_depth = selection.requested_bit_depth,
                           .applied_bit_depth = selection.applied_bit_depth,
                           .bit_depth_reason = selection.bit_depth_reason,
                           .fallback_reason = selection.fallback_reason,
                           .encoder_license = selection.license,
                           .speed_mapping = avif_speed_mapping_for(selection)};
}

}  // namespace awj
