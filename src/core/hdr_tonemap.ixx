module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/gamut_mapping.h>
#include <libplacebo/tone_mapping.h>
#include "hdr_cost.h"

export module awj.hdr_tonemap;

import awj.encoding_defaults;
import awj.image;
import awj.resource_planner;

export namespace awj::hdr {

namespace hdr_detail {

enum class SignalKind { none, scrgb, pq, hlg };

constexpr float kScRgbReferenceWhiteNits = 80.0F;
constexpr float kSdrTargetPeakNits = 100.0F;

bool finite_rgb(const std::array<float, 3>& rgb) noexcept {
  return std::isfinite(rgb[0]) && std::isfinite(rgb[1]) && std::isfinite(rgb[2]);
}

bool finite_matrix(const pl_matrix3x3& matrix) noexcept {
  for (int c = 0; c < 3; ++c) {
    std::array<float, 3> axis{};
    axis[c] = 1.0F;
    pl_matrix3x3_apply(&matrix, axis.data());
    if (!finite_rgb(axis)) return false;
  }
  return true;
}

std::expected<void, std::string> validate_rgba(const ImageBuffer& image,
                                               std::string_view context) {
  if (image.pixel_format != PixelFormat::rgba || image.planes.empty() ||
      (image.bit_depth != 8 && image.bit_depth != 16)) {
    return std::unexpected{std::format("{} 需要 8/16-bit RGBA 图像。", context)};
  }
  if (image.sample_representation == SampleRepresentation::ieee_half_float && image.bit_depth != 16) {
    return std::unexpected{std::format("{} 浮点 RGBA 位深无效。", context)};
  }
  const auto bytes_per_sample = image.bit_depth == 16 ? std::size_t{2} : std::size_t{1};
  const auto storage = image_storage_dimensions(image);
  if (image.width == 0 || image.height == 0 || !valid_image_geometry(image) ||
      image.width > std::numeric_limits<std::size_t>::max() / 4 / bytes_per_sample ||
      image.height > std::numeric_limits<std::size_t>::max() / image.width ||
      storage[0] == 0 || storage[1] == 0 ||
      storage[0] > std::numeric_limits<std::size_t>::max() / 4 / bytes_per_sample) {
    return std::unexpected{std::format("{} 图像尺寸无效。", context)};
  }
  const auto minimum_stride = storage[0] * 4 * bytes_per_sample;
  const auto& plane = image.planes.front();
  if (!plane.contains_rows(storage[1], minimum_stride)) {
    return std::unexpected{std::format("{} RGBA buffer 尺寸无效。", context)};
  }
  return {};
}

std::expected<std::vector<std::byte>, std::string> make_rgba_bytes(
    std::size_t width, std::size_t height, std::size_t bytes_per_sample,
    std::string_view context) {
  if (bytes_per_sample == 0 || width == 0 || height == 0 ||
      width > std::numeric_limits<std::size_t>::max() / 4 / bytes_per_sample) {
    return std::unexpected{std::format("{} 输出尺寸无效。", context)};
  }
  const auto stride = width * 4 * bytes_per_sample;
  if (height > std::numeric_limits<std::size_t>::max() / stride) {
    return std::unexpected{std::format("{} 输出尺寸过大。", context)};
  }
  const auto size = stride * height;
  if (static_cast<std::uint64_t>(size) > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{std::format("{} 输出 buffer 超过当前运行时上限。", context)};
  }
  try {
    return std::vector<std::byte>(size);
  } catch (const std::bad_alloc&) {
    return std::unexpected{std::format("{} 输出 buffer 内存不足。", context)};
  } catch (const std::length_error&) {
    return std::unexpected{std::format("{} 输出 buffer 尺寸超过运行时限制。", context)};
  }
}

std::expected<ImageBuffer, std::string> make_rgba_image(
    std::size_t width, std::size_t height, std::vector<std::byte> pixels,
    AlphaMode alpha_mode, int bit_depth, SampleRepresentation sample_representation,
    ImageSourceInfo source_info, std::string_view context) {
  const auto bytes_per_sample = bit_depth == 16 ? std::size_t{2} : std::size_t{1};
  if (width == 0 || height == 0 ||
      width > std::numeric_limits<std::size_t>::max() / 4 / bytes_per_sample) {
    return std::unexpected{std::format("{} 输出尺寸无效。", context)};
  }
  const auto stride = width * 4 * bytes_per_sample;
  if (height > std::numeric_limits<std::size_t>::max() / stride || pixels.size() != stride * height) {
    return std::unexpected{std::format("{} 输出 RGBA buffer 尺寸无效。", context)};
  }
  ImageBuffer output{.width = width, .height = height, .pixel_format = PixelFormat::rgba,
                     .alpha_mode = alpha_mode, .bit_depth = bit_depth,
                     .sample_representation = sample_representation,
                     .source_info = std::move(source_info)};
  try {
    output.planes.push_back(ImagePlane{.bytes = std::move(pixels), .stride = stride});
  } catch (const std::bad_alloc&) {
    return std::unexpected{std::format("{} 输出 plane 内存不足。", context)};
  } catch (const std::length_error&) {
    return std::unexpected{std::format("{} 输出 plane 尺寸超过运行时限制。", context)};
  }
  return output;
}

float binary16_to_float(std::uint16_t bits) noexcept {
  const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16;
  const std::uint32_t exponent = (bits >> 10) & 31u;
  const std::uint32_t mantissa = bits & 1023u;
  if (exponent == 31u) {
    if (mantissa) return std::numeric_limits<float>::quiet_NaN();
    return std::bit_cast<float>(sign | 0x7f800000u);
  }
  if (exponent) return std::bit_cast<float>(sign | ((exponent + 112u) << 23) | (mantissa << 13));
  if (!mantissa) return std::bit_cast<float>(sign);
  const auto shift = std::countl_zero(mantissa) - 21;
  return std::bit_cast<float>(sign | ((113u - shift) << 23) | (((mantissa << shift) & 1023u) << 13));
}

float read_sample(const ImageBuffer& image, const std::byte* row, std::size_t sample) noexcept {
  if (image.bit_depth == 8) {
    return static_cast<float>(std::to_integer<std::uint8_t>(row[sample])) / 255.0F;
  }
  std::uint16_t value{};
  std::memcpy(&value, row + sample * sizeof(value), sizeof(value));
  return image.sample_representation == SampleRepresentation::ieee_half_float
             ? binary16_to_float(value) : static_cast<float>(value) / 65535.0F;
}

void write_unorm16(std::byte* row, std::size_t sample, float value) noexcept {
  const auto result = static_cast<std::uint16_t>(std::clamp(
      std::lround(std::clamp(value, 0.0F, 1.0F) * 65535.0F), 0l, 65535l));
  std::memcpy(row + sample * sizeof(result), &result, sizeof(result));
}

const pl_raw_primaries* primaries_from_cicp(int value) noexcept {
  switch (value) {
    case 1: return pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
    case 9: return pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020);
    case 12: return pl_raw_primaries_get(PL_COLOR_PRIM_DISPLAY_P3);
    default: return nullptr;
  }
}

std::expected<SignalKind, std::string> signal_kind(const ImageBuffer& image) {
  const bool marked_scrgb = image.source_info &&
      (image.source_info->color_metadata_source == "wic-scrgb-half-linear" ||
       image.source_info->color_metadata_source == "wgc-scrgb-half-linear");
  if (image.sample_representation == SampleRepresentation::ieee_half_float) {
    const bool marked_linear = image.source_info &&
        image.source_info->transfer_characteristics == 8 && image.source_info->color_primaries &&
        primaries_from_cicp(*image.source_info->color_primaries) &&
        image.source_info->linear_reference_white_nits &&
        std::isfinite(*image.source_info->linear_reference_white_nits) &&
        *image.source_info->linear_reference_white_nits > 0;
    if (!marked_scrgb && !marked_linear) {
      return std::unexpected{"浮点 RGBA 缺少线性原色与参考白亮度，拒绝猜测 HDR 色彩空间。"};
    }
    if ((image.source_info->linear_reference_white_nits &&
         (!std::isfinite(*image.source_info->linear_reference_white_nits) ||
          *image.source_info->linear_reference_white_nits <= 0)) ||
        (image.source_info->color_primaries && !primaries_from_cicp(*image.source_info->color_primaries))) {
      return std::unexpected{"浮点 RGBA 的原色或线性参考白亮度无效。"};
    }
    return SignalKind::scrgb;
  }
  if (!image.source_info) return SignalKind::none;
  const auto& source = *image.source_info;
  const auto transfer = source.transfer_characteristics.value_or(0);
  if (transfer == 16 || transfer == 18) {
    if (!source.color_primaries || !primaries_from_cicp(*source.color_primaries)) {
      return std::unexpected{"HDR 输入缺少或使用了当前不支持的 CICP 色度原色，拒绝猜测。"};
    }
    return transfer == 16 ? SignalKind::pq : SignalKind::hlg;
  }
  if (source.has_hdr_metadata || source.content_light) {
    return std::unexpected{"HDR 元数据没有匹配的 PQ/HLG CICP 传递特性，拒绝按 BT.709 猜测。"};
  }
  return SignalKind::none;
}

struct SourceTransform {
  SignalKind kind{SignalKind::none};
  const pl_raw_primaries* primaries{};
  pl_color_space color_space{};
  float linear_reference_white_nits{kScRgbReferenceWhiteNits};
};

std::expected<SourceTransform, std::string> make_source_transform(const ImageBuffer& image) {
  auto kind = signal_kind(image);
  if (!kind) return std::unexpected{kind.error()};
  if (*kind == SignalKind::none) return std::unexpected{"输入不是带明确 CICP 语义的 HDR 图像。"};
  const auto* bt709 = pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
  if (!bt709) return std::unexpected{"libplacebo 未提供 BT.709 原色定义。"};
  SourceTransform result{.kind = *kind, .primaries = bt709};
  const auto& source = *image.source_info;
  if (*kind == SignalKind::scrgb) {
    if (source.color_primaries) result.primaries = primaries_from_cicp(*source.color_primaries);
    result.linear_reference_white_nits = source.linear_reference_white_nits.value_or(kScRgbReferenceWhiteNits);
    return result;
  }
  result.primaries = primaries_from_cicp(*source.color_primaries);
  result.color_space.primaries = *source.color_primaries == 9 ? PL_COLOR_PRIM_BT_2020
      : *source.color_primaries == 12 ? PL_COLOR_PRIM_DISPLAY_P3 : PL_COLOR_PRIM_BT_709;
  result.color_space.transfer = *kind == SignalKind::pq ? PL_COLOR_TRC_PQ : PL_COLOR_TRC_HLG;
  if (source.content_light && source.content_light->max_cll > 0 &&
      (source.content_light->max_pall == 0 || source.content_light->max_pall <= source.content_light->max_cll)) {
    result.color_space.hdr.max_luma = static_cast<float>(source.content_light->max_cll);
    result.color_space.hdr.max_cll = static_cast<float>(source.content_light->max_cll);
    result.color_space.hdr.max_fall = static_cast<float>(source.content_light->max_pall);
  } else if (*kind == SignalKind::hlg) {
    result.color_space.hdr.max_luma = PL_COLOR_HLG_PEAK;
  }
  if (*kind == SignalKind::hlg) {
    float peak{};
    const pl_nominal_luma_params params{.color = &result.color_space, .metadata = PL_HDR_METADATA_HDR10,
        .scaling = PL_HDR_NITS, .out_max = &peak};
    pl_color_space_nominal_luma_ex(&params);
    result.color_space.hdr.max_luma = peak;
  }
  return result;
}

// A borrowed, read-only adapter for native YUV/geometry readers. Its owner must
// outlive preparation and the joined tone call. Samples retain the original
// source transfer/reference-white semantics; there is no RGBA quantization or
// HDR10 materialization before tone mapping.
struct RgbaPixelReader {
  const void* context{};
  std::array<float, 4> (*read)(const void*, std::size_t, std::size_t) noexcept{};
};

std::array<float, 4> read_source_rgba(const ImageBuffer& image, const RgbaPixelReader& reader,
                                     std::size_t x, std::size_t y) noexcept {
  if (reader.read) return reader.read(reader.context, x, y);
  const auto coordinate = image_sample_coordinates(image, x, y);
  const auto* row = image.planes.front().read_bytes().data() + coordinate[1] * image.planes.front().stride;
  const auto sample = coordinate[0] * 4;
  std::array<float, 4> rgba{read_sample(image, row, sample), read_sample(image, row, sample + 1),
                            read_sample(image, row, sample + 2), read_sample(image, row, sample + 3)};
  if (image.alpha_mode == AlphaMode::premultiplied) {
    for (const auto value : rgba) if (!std::isfinite(value)) return rgba;
    for (std::size_t c = 0; c < 3; ++c) rgba[c] = rgba[3] > 0.0F ? rgba[c] / rgba[3] : 0.0F;
  }
  return rgba;
}

float read_source_alpha(const ImageBuffer& image, const RgbaPixelReader& reader,
                         std::size_t x, std::size_t y) noexcept {
  if (reader.read) return reader.read(reader.context, x, y)[3];
  const auto coordinate = image_sample_coordinates(image, x, y);
  const auto* row = image.planes.front().read_bytes().data() + coordinate[1] * image.planes.front().stride;
  return read_sample(image, row, coordinate[0] * 4 + 3);
}

std::expected<std::array<float, 3>, std::string> linear_nits(
    std::array<float, 3> rgb, const SourceTransform& transform) {
  if (!finite_rgb(rgb)) return std::unexpected{"HDR 输入包含非有限 RGB 浮点值。"};
  if (transform.kind == SignalKind::scrgb) {
    for (auto& value : rgb) value *= transform.linear_reference_white_nits;
  } else {
    pl_color_linearize(&transform.color_space, rgb.data());
    if (!finite_rgb(rgb)) return std::unexpected{"libplacebo HDR 线性化产生了非有限值。"};
    // 锁定版本的 HLG 线性化返回相对于显示峰值的值，PQ 才是 203-nit 归一标尺。
    for (auto& value : rgb) value = transform.kind == SignalKind::hlg
        ? value * transform.color_space.hdr.max_luma
        : pl_hdr_rescale(PL_HDR_NORM, PL_HDR_NITS, value);
  }
  if (!finite_rgb(rgb)) return std::unexpected{"HDR 参考白亮度缩放产生了非有限值。"};
  return rgb;
}

std::expected<std::array<float, 3>, std::string> linear_nits(
    const ImageBuffer& image, const SourceTransform& transform,
    const std::byte* row, std::size_t sample) {
  return linear_nits({read_sample(image, row, sample), read_sample(image, row, sample + 1),
                       read_sample(image, row, sample + 2)}, transform);
}

float luma_nits(std::array<float, 3> rgb, const pl_matrix3x3& matrix) noexcept {
  pl_matrix3x3_apply(&matrix, rgb.data());
  return finite_rgb(rgb) ? std::max(rgb[1], 0.0F) : std::numeric_limits<float>::quiet_NaN();
}

struct LuminanceStats {
  float peak{};
  float average{};
};

// Raw, scanned CIE Y only. Metadata and the tone mapper's 100-nit floor must
// never overwrite these measurements (in particular, not for JPEG capacity).
std::expected<LuminanceStats, std::string> luminance_stats(
    const ImageBuffer& image, const SourceTransform& transform,
    const pl_matrix3x3& luma_matrix, std::stop_token stop = {}, const RgbaPixelReader& reader = {}) {
  float peak{};
  double sum{};
  for (std::size_t y = 0; y < image.height; ++y) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    for (std::size_t x = 0; x < image.width; ++x) {
      if (x % hdr_cost::gamut_block_pixels == 0 && stop.stop_requested()) return std::unexpected{"任务已取消。"};
      const auto rgba = read_source_rgba(image, reader, x, y);
      auto rgb = linear_nits({rgba[0], rgba[1], rgba[2]}, transform);
      if (!rgb) return std::unexpected{rgb.error()};
      const auto luma = luma_nits(*rgb, luma_matrix);
      if (!std::isfinite(luma)) return std::unexpected{"HDR 亮度矩阵产生了非有限值。"};
      if (!std::isfinite(rgba[3])) return std::unexpected{"HDR 输入包含非有限 alpha 浮点值。"};
      peak = std::max(peak, luma);
      sum += luma;
    }
  }
  const auto average = static_cast<float>(sum / (image.width * image.height));
  if (!std::isfinite(average)) return std::unexpected{"HDR 平均亮度统计产生了非有限值。"};
  return LuminanceStats{.peak = peak, .average = average};
}

LuminanceStats effective_tone_stats(const ImageBuffer& image, LuminanceStats scanned) noexcept {
  auto effective = scanned;
  if (image.source_info && image.source_info->content_light) {
    const auto& cll = *image.source_info->content_light;
    if (cll.max_cll > 0 && (cll.max_pall == 0 || cll.max_pall <= cll.max_cll)) {
      effective.peak = static_cast<float>(cll.max_cll);
      if (cll.max_pall > 0) effective.average = static_cast<float>(cll.max_pall);
    }
  }
  effective.peak = std::max(effective.peak, kSdrTargetPeakNits);
  effective.average = std::clamp(effective.average, 0.0F, effective.peak);
  return effective;
}

void legalize_gamut_constants(pl_gamut_map_constants& c) noexcept {
  // The direct public callback does not run pl_gamut_map_sample's fix_constants.
  const auto finite_clamp = [](float x, float high) {
    return std::isfinite(x) ? std::clamp(x, 0.0F, high) : 0.0F;
  };
  c.perceptual_deadzone = finite_clamp(c.perceptual_deadzone, 1.0F);
  c.perceptual_strength = finite_clamp(c.perceptual_strength, 1.0F);
  c.colorimetric_gamma = finite_clamp(c.colorimetric_gamma, 10.0F);
  c.softclip_knee = finite_clamp(c.softclip_knee, 1.0F);
  c.softclip_desat = finite_clamp(c.softclip_desat, 1.0F);
}

struct PreparedToneMap {
  // Bound to an immutable image for the duration of this conversion. This is
  // per-image state, not a global color cache, and owns no full-image buffers.
  const ImageBuffer* source{};
  const std::byte* source_data{};
  std::size_t width{}, height{}, stride{}, source_size{};
  int source_depth{};
  SampleRepresentation source_representation{};
  RgbaPixelReader reader{};
  SourceTransform transform{};
  LuminanceStats scanned_y{};
  LuminanceStats effective_tone{};
  pl_matrix3x3 source_to_xyz{};
  pl_matrix3x3 source_to_lms{};
  pl_matrix3x3 output_lms_to_rgb{};
  pl_matrix3x3 source_to_bt2020{};
  pl_matrix3x3 canonical_to_xyz{};
  pl_tone_map_params tone_params{};
  pl_gamut_map_params gamut_params{};
  bool same_primaries_and_white{};
};

bool matches_source(const PreparedToneMap& context, const ImageBuffer& image) noexcept {
  return context.source == &image && !image.planes.empty() && context.width == image.width &&
      context.height == image.height && context.source_depth == image.bit_depth &&
      context.source_representation == image.sample_representation &&
      context.stride == image.planes.front().stride && context.source_size == image.planes.front().read_bytes().size() &&
      context.source_data == image.planes.front().read_bytes().data();
}

std::expected<PreparedToneMap, std::string> prepare_tone_context(
    const ImageBuffer& image, std::stop_token stop, RgbaPixelReader reader = {}) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (!reader.read) {
    if (auto valid = validate_rgba(image, "HDR -> SDR"); !valid) return std::unexpected{valid.error()};
  } else if (!image.width || !image.height || image.planes.empty() || !valid_image_geometry(image) ||
      image.width > SIZE_MAX / image.height) {
    // The native reader owner validates all its plane layouts before binding.
    return std::unexpected{"HDR borrowed reader 的图像尺寸无效。"};
  }
  auto transform = make_source_transform(image);
  if (!transform) return std::unexpected{transform.error()};
  const auto* bt709 = pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
  const auto* bt2020 = pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020);
  if (!bt709 || !bt2020) return std::unexpected{"libplacebo 未提供所需 BT.709/BT.2020 原色定义。"};
  PreparedToneMap context;
  context.source = &image;
  context.source_data = image.planes.front().read_bytes().data();
  context.width = image.width; context.height = image.height;
  context.stride = image.planes.front().stride;
  context.source_size = image.planes.front().read_bytes().size();
  context.source_depth = image.bit_depth;
  context.source_representation = image.sample_representation;
  context.reader = reader;
  context.transform = *transform;
  context.source_to_xyz = pl_get_rgb2xyz_matrix(transform->primaries);
  context.source_to_lms = pl_ipt_rgb2lms(transform->primaries);
  context.output_lms_to_rgb = pl_ipt_lms2rgb(bt709);
  context.source_to_bt2020 = pl_raw_primaries_equal(transform->primaries, bt2020)
      ? pl_matrix3x3{.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}}
      : pl_get_color_mapping_matrix(transform->primaries, bt2020, PL_INTENT_RELATIVE_COLORIMETRIC);
  context.canonical_to_xyz = pl_get_rgb2xyz_matrix(bt2020);
  if (!finite_matrix(context.source_to_xyz) || !finite_matrix(context.source_to_lms) ||
      !finite_matrix(context.output_lms_to_rgb) || !finite_matrix(context.source_to_bt2020) ||
      !finite_matrix(context.canonical_to_xyz)) return std::unexpected{"HDR 色彩矩阵包含非有限值。"};
  auto stats = luminance_stats(image, *transform, context.source_to_xyz, stop, reader);
  if (!stats) return std::unexpected{stats.error()};
  context.scanned_y = *stats;
  context.effective_tone = effective_tone_stats(image, *stats);
  auto& tone = context.tone_params;
  tone.function = &pl_tone_map_spline;
  tone.constants = {PL_TONE_MAP_CONSTANTS};
  tone.input_scaling = tone.output_scaling = PL_HDR_NITS;
  tone.input_min = 0.0F;
  tone.input_max = context.effective_tone.peak;
  tone.input_avg = context.effective_tone.average;
  tone.output_min = 0.0F;
  tone.output_max = kSdrTargetPeakNits;
  pl_tone_map_params_infer(&tone);
  auto& gamut = context.gamut_params;
  gamut.function = &pl_gamut_map_perceptual;
  gamut.input_gamut = *transform->primaries;
  gamut.output_gamut = *bt709;
  gamut.min_luma = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, 0.0F);
  gamut.max_luma = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, kSdrTargetPeakNits);
  // The upstream C macro's designators are not in C++ declaration order.
  gamut.constants.perceptual_deadzone = 0.30F;
  gamut.constants.perceptual_strength = 0.80F;
  gamut.constants.colorimetric_gamma = 1.80F;
  gamut.constants.softclip_knee = 0.70F;
  gamut.constants.softclip_desat = 0.35F;
  legalize_gamut_constants(gamut.constants);
  context.same_primaries_and_white = pl_raw_primaries_equal(transform->primaries, bt709);
  return context;
}

double tone_nits_to_pq(double value) noexcept {
  if (value <= 0.0) return 0.0;
  const double power = std::pow(value / 10'000.0, 2610.0 / 16384.0);
  return std::pow((3424.0 / 4096.0 + (2413.0 / 128.0) * power) /
      (1.0 + (2392.0 / 128.0) * power), 2523.0 / 32.0);
}

double tone_pq_to_nits(double value) noexcept {
  if (value <= 0.0) return 0.0;
  const double power = std::pow(value, 32.0 / 2523.0);
  return 10'000.0 * std::pow(std::max(power - 3424.0 / 4096.0, 0.0) /
      (2413.0 / 128.0 - (2392.0 / 128.0) * power), 16384.0 / 2610.0);
}

struct ToneCurve {
  pl_tone_map_params scalar_params{};
  pl_tone_map_params pq_params{};
  pl_tone_map_params mapping_params{};
  std::vector<float> nits{};
  double index_scale{};

  float scalar_sample(float value) const noexcept {
    const auto clamped = std::clamp(value, scalar_params.input_min, scalar_params.input_max);
    const auto encoded = static_cast<float>(tone_nits_to_pq(clamped));
    return static_cast<float>(tone_pq_to_nits(pl_tone_map_sample(encoded, &mapping_params)));
  }

  float sample(float value) const noexcept {
    if (nits.empty() || value <= scalar_params.input_min || value >= scalar_params.input_max) {
      return scalar_sample(value);
    }
    const auto pq = tone_nits_to_pq(value);
    const auto position = (pq - pq_params.input_min) * index_scale;
    // PQ's black singularity and its first few intervals are deliberately
    // scalar. Domain/endpoint handling must have exactly scalar semantics.
    if (!std::isfinite(position) || position < 8.0F || position >= static_cast<float>(nits.size() - 1)) {
      return scalar_sample(value);
    }
    const auto lower = static_cast<std::size_t>(position);
    const auto fraction = static_cast<float>(position - static_cast<double>(lower));
    return std::lerp(nits[lower], nits[lower + 1], fraction);
  }
};

std::expected<ToneCurve, std::string> make_tone_curve(const pl_tone_map_params& params,
    bool use_lut, std::uint64_t scratch_budget, std::stop_token stop = {}) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (!params.function || params.input_scaling != PL_HDR_NITS || params.output_scaling != PL_HDR_NITS ||
      !std::isfinite(params.input_min) || !std::isfinite(params.input_max) || !std::isfinite(params.input_avg) ||
      !std::isfinite(params.output_min) || !std::isfinite(params.output_max) || params.input_min < 0.0F ||
      params.input_max <= params.input_min || params.input_avg < 0.0F || params.input_avg > params.input_max ||
      params.output_min < 0.0F || params.output_max <= params.output_min) {
    return std::unexpected{"HDR tone curve 需要有限、合法的 nits 输入与输出参数。"};
  }
  ToneCurve curve{.scalar_params = params, .pq_params = params};
  auto& pq = curve.pq_params;
  pq.input_min = static_cast<float>(tone_nits_to_pq(params.input_min));
  pq.input_max = static_cast<float>(tone_nits_to_pq(params.input_max));
  pq.input_avg = static_cast<float>(tone_nits_to_pq(params.input_avg));
  pq.input_scaling = PL_HDR_PQ;
  pq.lut_size = hdr_cost::tone_lut_entries;
  // Outputs intentionally remain nits, including output min/max scaling.
  if (!std::isfinite(pq.input_min) || !std::isfinite(pq.input_max) ||
      !std::isfinite(pq.input_avg) || pq.input_max <= pq.input_min) {
    return std::unexpected{"HDR tone LUT 的 PQ 输入范围无效。"};
  }
  curve.index_scale = static_cast<double>(pq.lut_size - 1) /
      (static_cast<double>(pq.input_max) - pq.input_min);
  if (!std::isfinite(curve.index_scale)) return std::unexpected{"HDR tone LUT 索引缩放无效。"};
  curve.mapping_params = pq;
  curve.mapping_params.output_scaling = PL_HDR_PQ;
  curve.mapping_params.output_min = static_cast<float>(tone_nits_to_pq(params.output_min));
  curve.mapping_params.output_max = static_cast<float>(tone_nits_to_pq(params.output_max));
  if (!use_lut || scratch_budget < hdr_cost::tone_lut_bytes +
      hdr_cost::tone_scratch_bytes(1, false, 1)) return curve;
  try {
    curve.nits.resize(pq.lut_size);
  } catch (const std::bad_alloc&) {
    return curve; // Scalar is equally accurate, not a reduced-quality fallback.
  } catch (const std::length_error&) {
    return curve;
  }
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  // 库的映射核心保留在 PQ 域；双精度换算消除单精度 PQ 往返噪声，表值仍为 nits。
  pl_tone_map_generate(curve.nits.data(), &curve.mapping_params);
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  for (std::size_t i = 0; i < curve.nits.size(); ++i) {
    if (i % 1024 == 0 && stop.stop_requested()) return std::unexpected{"任务已取消。"};
    curve.nits[i] = static_cast<float>(tone_pq_to_nits(curve.nits[i]));
  }
  if (!std::ranges::all_of(curve.nits, [](float x) { return std::isfinite(x); })) {
    return std::unexpected{"HDR tone LUT 包含非有限值。"};
  }
  // PQ 单精度往返在相邻采样点会产生微小反向噪声，曲线保持物理单调。
  for (std::size_t i = 1; i < curve.nits.size(); ++i) {
    curve.nits[i] = std::max(curve.nits[i], curve.nits[i - 1]);
  }
  return curve;
}

// The nonlinear operation belongs to LMS, not RGB. This is the ordering in
// libplacebo v7.360.1 src/tests/tone_mapping.c:131-169 (IPTPQc4 CPU oracle).
bool rgb_nits_to_ipt(std::array<float, 3>& rgb, const pl_matrix3x3& rgb_to_lms) noexcept {
  if (!finite_rgb(rgb)) return false;
  pl_matrix3x3_apply(&rgb_to_lms, rgb.data());
  if (!finite_rgb(rgb)) return false;
  for (auto& value : rgb) value = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, value);
  if (!finite_rgb(rgb)) return false;
  pl_matrix3x3_apply(&pl_ipt_lms2ipt, rgb.data());
  return finite_rgb(rgb);
}

bool ipt_to_rgb_nits(std::array<float, 3>& ipt, const pl_matrix3x3& lms_to_rgb) noexcept {
  if (!finite_rgb(ipt)) return false;
  pl_matrix3x3_apply(&pl_ipt_ipt2lms, ipt.data());
  if (!finite_rgb(ipt)) return false;
  for (auto& value : ipt) value = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, value);
  if (!finite_rgb(ipt)) return false;
  pl_matrix3x3_apply(&lms_to_rgb, ipt.data());
  return finite_rgb(ipt);
}

std::expected<void, std::string> map_gamut_block(float* values, std::size_t count,
    std::size_t stride, const pl_gamut_map_params& params, std::stop_token stop = {}) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (!values || count == 0 || count > hdr_cost::gamut_block_pixels || stride < 3 ||
      stride > static_cast<std::size_t>(std::numeric_limits<int>::max()) / count) {
    return std::unexpected{"HDR gamut 块尺寸或 float stride 无效。"};
  }
  if (!std::isfinite(params.min_luma) || !std::isfinite(params.max_luma) ||
      params.min_luma < 0.0F || params.max_luma <= params.min_luma) {
    return std::unexpected{"HDR gamut 亮度范围无效。"};
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (!std::isfinite(values[i * stride]) || !std::isfinite(values[i * stride + 1]) ||
        !std::isfinite(values[i * stride + 2])) return std::unexpected{"HDR gamut 输入包含非有限 IPT。"};
  }
  auto fixed = params;
  legalize_gamut_constants(fixed.constants);
  fixed.lut_size_C = fixed.lut_size_h = 1;
  fixed.lut_stride = static_cast<int>(stride);
  for (std::size_t first = 0; first < count;) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    std::size_t last = first + 1;
    float previous_hue = std::atan2(values[first * stride + 2], values[first * stride + 1]);
    for (; last < count; ++last) {
      const float hue = std::atan2(values[last * stride + 2], values[last * stride + 1]);
      // 冻结库对 1e-3 内的近邻色相复用近似峰值；分段重置缓存，完全相同的色相仍可批处理。
      if (hue != previous_hue && std::abs(hue - previous_hue) < 1.0e-3F) break;
      previous_hue = hue;
    }
    fixed.lut_size_I = static_cast<int>(last - first);
    // 直接映射已有 IPT，不生成三维表，也不启动库内独立线程池。
    pl_gamut_map_perceptual.map(values + first * stride, &fixed);
    first = last;
  }
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  for (std::size_t i = 0; i < count; ++i) {
    if (!std::isfinite(values[i * stride]) || !std::isfinite(values[i * stride + 1]) ||
        !std::isfinite(values[i * stride + 2])) return std::unexpected{"HDR gamut 输出包含非有限 IPT。"};
  }
  return {};
}

bool can_skip_gamut(const std::array<float, 3>& rgb, const PreparedToneMap& context) noexcept {
  if (!context.same_primaries_and_white || !finite_rgb(rgb)) return false;
  // Stay inside the display volume, away from numeric boundary classification.
  // Exact black is safe; negative, extended and wide-gamut RGB always map.
  constexpr float guard = 0.001F;
  return std::ranges::all_of(rgb, [](float x) {
    return x == 0.0F || (x >= guard && x <= kSdrTargetPeakNits - guard);
  });
}

float srgb_encode(float linear) noexcept {
  linear = std::clamp(linear, 0.0F, 1.0F);
  return linear <= 0.0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

std::uint8_t dithered_unorm8(float value, std::size_t x, std::size_t y) noexcept {
  constexpr std::array<int, 16> bayer4x4{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
  const auto dither = (static_cast<float>(bayer4x4[(y % 4) * 4 + x % 4]) - 7.5F) / 16.0F;
  return static_cast<std::uint8_t>(std::clamp(
      std::floor(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F + dither), 0.0F, 255.0F));
}

std::expected<void, std::string> copy_non_color_metadata(ImageBuffer& destination, const ImageBuffer& source) {
  try {
    destination.metadata.reserve(source.metadata.size());
    for (const auto& block : source.metadata) {
      if (block.kind == MetadataKind::exif || block.kind == MetadataKind::xmp) destination.metadata.push_back(block);
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected{"复制非色彩元数据时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"复制非色彩元数据时尺寸超过运行时限制。"};
  }
  return {};
}

int tone_worker_budget(const ResourcePlan& resources) noexcept {
  const auto global = std::max(1, resources.global_thread_budget);
  const auto per_file = std::max(1, global / std::max(1, resources.file_parallelism));
  return std::max(1, std::min(resources.encoder_threads_per_file, per_file));
}

struct ToneRunOptions {
  // Detail-only controls for partition/quality tests; production uses defaults.
  std::size_t block_pixels{hdr_cost::gamut_block_pixels};
  bool use_lut{true};
  std::size_t lut_min_pixels{hdr_cost::tone_lut_entries};
};

struct ToneScratch {
  std::array<float, 3> single_ipt{};
  std::size_t single_position{};
  std::vector<float> ipt{};
  std::vector<std::size_t> positions{};

  void allocate(std::size_t count) {
    if (count > 1) { ipt.resize(count * 3); positions.resize(count); }
  }
  float* values() noexcept { return ipt.empty() ? single_ipt.data() : ipt.data(); }
  std::size_t* indices() noexcept { return positions.empty() ? &single_position : positions.data(); }
};

std::expected<ImageBuffer, std::string> tone_map_prepared(const ImageBuffer& image,
    const PreparedToneMap& context, int output_depth, std::stop_token stop,
    const ResourcePlan& resources, ToneRunOptions options = {}) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (output_depth != 8 && output_depth != 16) return std::unexpected{"SDR 输出位深必须为 8 或 16。"};
  if (!context.reader.read) {
    if (auto valid = validate_rgba(image, "HDR -> SDR"); !valid) return std::unexpected{valid.error()};
  }
  if (!matches_source(context, image)) return std::unexpected{"HDR prepared context 不属于当前未修改的图像。"};
  if (!options.block_pixels || options.block_pixels > hdr_cost::gamut_block_pixels) {
    return std::unexpected{"HDR tone 块大小必须在 1 到 1024 之间。"};
  }
  const auto total = image.width * image.height;
  const auto output_bytes = hdr_cost::multiply(total, 4 * (output_depth / 8));
  std::uint64_t resident = output_bytes;
  for (const auto& plane : image.planes) resident = hdr_cost::add(resident, plane.read_bytes().size());
  for (const auto& block : image.metadata) resident = hdr_cost::add(resident, hdr_cost::multiply(block.bytes.size(), 2));
  // The scheduler already admits concurrent files. This is a whole-plan check,
  // not limit/file_parallelism, so an admitted large file may use spare capacity.
  if (resident == UINT64_MAX || (resources.memory_limit_bytes && resident > resources.memory_limit_bytes)) {
    return std::unexpected{"HDR -> SDR 输入与输出工作集超出内存预算。"};
  }
  auto available = resources.memory_limit_bytes ? resources.memory_limit_bytes - resident : UINT64_MAX;
  auto curve = make_tone_curve(context.tone_params,
      options.use_lut && total >= options.lut_min_pixels, available, stop);
  if (!curve) return std::unexpected{curve.error()};
  if (!curve->nits.empty()) available -= hdr_cost::tone_lut_bytes;
  std::size_t block = options.block_pixels;
  auto workers = std::min<std::size_t>(tone_worker_budget(resources), (total - 1) / block + 1);
  const auto per_worker = hdr_cost::tone_scratch_bytes(1, false, block);
  if (available < per_worker) {
    workers = 1;
    block = static_cast<std::size_t>(std::min<std::uint64_t>(block,
        available > 1'024 ? (available - 1'024) / hdr_cost::tone_pixel_scratch_bytes : 1));
    block = std::max<std::size_t>(1, block);
  } else {
    workers = std::max<std::size_t>(1, std::min<std::uint64_t>(workers, available / per_worker));
  }
  ToneScratch single;
  std::vector<ToneScratch> scratch;
  try {
    if (workers > 1 || block > 1) {
      scratch.resize(workers);
      for (auto& entry : scratch) entry.allocate(block);
    }
  } catch (const std::bad_alloc&) {
    std::vector<ToneScratch>{}.swap(scratch); workers = 1; block = 1;
  } catch (const std::length_error&) {
    std::vector<ToneScratch>{}.swap(scratch); workers = 1; block = 1;
  }
  auto pixels = make_rgba_bytes(image.width, image.height, output_depth / 8, "HDR -> SDR");
  if (!pixels) return std::unexpected{pixels.error()};
  std::atomic<std::size_t> next{};
  std::atomic<const char*> error{};
  const auto set_error = [&](const char* text) noexcept {
    const char* empty = nullptr;
    error.compare_exchange_strong(empty, text, std::memory_order_relaxed);
  };
  const auto write_pixel = [&](std::size_t position, const std::array<float, 3>& rgb) -> const char* {
    if (!finite_rgb(rgb)) return "HDR gamut 逆变换产生了非有限 RGB。";
    const auto y = position / image.width;
    const auto x = position % image.width;
    const auto sample = x * 4;
    auto* output = pixels->data() + y * image.width * 4 * (output_depth / 8);
    for (std::size_t c = 0; c < 3; ++c) {
      const auto encoded = srgb_encode(rgb[c] / kSdrTargetPeakNits);
      if (!std::isfinite(encoded)) return "HDR sRGB 编码产生了非有限值。";
      if (output_depth == 16) write_unorm16(output, sample + c, encoded);
      else output[sample + c] = std::byte{dithered_unorm8(encoded, x, y)};
    }
    const auto alpha = read_source_alpha(image, context.reader, x, y);
    if (!std::isfinite(alpha)) return "HDR 输入包含非有限 alpha 浮点值。";
    if (output_depth == 16) write_unorm16(output, sample + 3, alpha);
    else output[sample + 3] = std::byte{static_cast<std::uint8_t>(std::clamp(
        std::lround(std::clamp(alpha, 0.0F, 1.0F) * 255.0F), 0l, 255l))};
    return nullptr;
  };
  const auto work = [&](std::size_t worker) noexcept {
    try {
      auto& buffers = scratch.empty() ? single : scratch[worker];
      auto* values = buffers.values();
      auto* indices = buffers.indices();
      while (!stop.stop_requested() && !error.load(std::memory_order_relaxed)) {
        const auto first = next.fetch_add(block, std::memory_order_relaxed);
        if (first >= total) break;
        const auto end = first + std::min(block, total - first);
        std::size_t mapped{};
        for (auto position = first; position < end; ++position) {
          const auto y = position / image.width;
          const auto x = position % image.width;
          const auto rgba = read_source_rgba(image, context.reader, x, y);
          auto rgb = linear_nits({rgba[0], rgba[1], rgba[2]}, context.transform);
          if (!rgb) { set_error("HDR 线性化或参考白缩放失败。"); return; }
          const auto luma = luma_nits(*rgb, context.source_to_xyz);
          if (!std::isfinite(luma)) { set_error("HDR 亮度矩阵产生了非有限值。"); return; }
          if (luma > 0.0F) {
            const auto mapped_luma = curve->sample(luma);
            const auto scale = mapped_luma / luma;
            if (!std::isfinite(mapped_luma) || mapped_luma < 0.0F || !std::isfinite(scale)) {
              set_error("HDR tone 亮度或比例产生了非有限值。"); return;
            }
            for (auto& value : *rgb) value *= scale;
          }
          if (!finite_rgb(*rgb)) { set_error("HDR tone RGB 缩放产生了非有限值。"); return; }
          if (can_skip_gamut(*rgb, context)) {
            if (const auto* failure = write_pixel(position, *rgb)) { set_error(failure); return; }
          } else {
            if (!rgb_nits_to_ipt(*rgb, context.source_to_lms)) {
              set_error("HDR RGB -> LMS -> PQ -> IPT 产生了非有限值。"); return;
            }
            std::copy_n(rgb->data(), 3, values + mapped * 3);
            indices[mapped++] = position;
          }
        }
        if (stop.stop_requested() || error.load(std::memory_order_relaxed)) break;
        if (mapped) {
          if (auto result = map_gamut_block(values, mapped, 3, context.gamut_params, stop); !result) {
            if (!stop.stop_requested()) set_error("HDR 有界 gamut 映射失败。");
            return;
          }
          for (std::size_t i = 0; i < mapped; ++i) {
            std::array<float, 3> rgb{values[i * 3], values[i * 3 + 1], values[i * 3 + 2]};
            if (!ipt_to_rgb_nits(rgb, context.output_lms_to_rgb)) {
              set_error("HDR IPT -> LMS' -> inverse PQ -> RGB 产生了非有限值。"); return;
            }
            if (const auto* failure = write_pixel(indices[i], rgb)) { set_error(failure); return; }
          }
        }
        if (stop.stop_requested()) break;
      }
    } catch (const std::bad_alloc&) { set_error("HDR tone worker 内存不足。"); }
      catch (const std::exception&) { set_error("HDR tone worker 处理失败。"); }
      catch (...) { set_error("HDR tone worker 处理失败。"); }
  };
  // The caller is worker zero. Never query hardware concurrency or detach a
  // task; all image access and scratch are joined before any later codec stage.
  std::vector<std::jthread> threads;
  try {
    threads.reserve(workers - 1);
    for (std::size_t i = 1; i < workers; ++i) threads.emplace_back([&, i] { work(i); });
  } catch (const std::system_error&) {
    // Existing workers and the caller can finish the same queue within budget.
  } catch (const std::bad_alloc&) {
    // Correct serial/partial-pool fallback, with the same LUT and math.
  }
  work(0);
  for (auto& thread : threads) thread.join();
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (const auto* failure = error.load(std::memory_order_relaxed)) return std::unexpected{std::string(failure)};
  ImageSourceInfo source_info{.pixel_format = PixelFormat::rgba, .bit_depth = output_depth,
      .color_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 0,
      .color_range = 1, .has_hdr_metadata = false,
      .color_metadata_source = "hdr-sdr-libplacebo-spline-perceptual",
      .rendition_origin = RenditionOrigin::tone_mapped};
  const auto alpha_mode = image.alpha_mode == AlphaMode::premultiplied ? AlphaMode::straight : image.alpha_mode;
  auto output = make_rgba_image(image.width, image.height, std::move(*pixels), alpha_mode,
      output_depth, SampleRepresentation::unorm, std::move(source_info), "HDR -> SDR");
  if (!output) return std::unexpected{output.error()};
  if (auto copied = copy_non_color_metadata(*output, image); !copied) return std::unexpected{copied.error()};
  return output;
}

}  // namespace hdr_detail

using PreparedToneMap = hdr_detail::PreparedToneMap;

std::expected<bool, std::string> has_explicit_hdr_signal(const ImageBuffer& image) {
  auto kind = hdr_detail::signal_kind(image);
  if (!kind) return std::unexpected{kind.error()};
  return *kind != hdr_detail::SignalKind::none;
}

std::expected<PreparedToneMap, std::string> prepare_tone_map(
    const ImageBuffer& image, std::stop_token stop = {}) {
  try {
    return hdr_detail::prepare_tone_context(image, stop);
  } catch (const std::bad_alloc&) { return std::unexpected{"HDR prepared context 内存不足。"}; }
    catch (const std::length_error&) { return std::unexpected{"HDR prepared context 数据超过运行时限制。"}; }
}

std::expected<ImageBuffer, std::string> tone_map_to_sdr_srgb(const ImageBuffer& image,
    const PreparedToneMap& context, int output_depth = 8, std::stop_token stop = {},
    const ResourcePlan& resources = {}) {
  try {
    return hdr_detail::tone_map_prepared(image, context, output_depth, stop, resources);
  } catch (const std::bad_alloc&) { return std::unexpected{"HDR -> SDR 内存不足。"}; }
    catch (const std::length_error&) { return std::unexpected{"HDR -> SDR 数据超过运行时限制。"}; }
}

// Compatible with the existing image/depth/stop API. The optional fourth
// argument is the current file's plan; its thread count includes this caller.
std::expected<ImageBuffer, std::string> tone_map_to_sdr_srgb(const ImageBuffer& image,
    int output_depth = 8, std::stop_token stop = {}, const ResourcePlan& resources = {}) {
  if (output_depth != 8 && output_depth != 16) return std::unexpected{"SDR 输出位深必须为 8 或 16。"};
  auto context = prepare_tone_map(image, stop);
  if (!context) return std::unexpected{context.error()};
  return tone_map_to_sdr_srgb(image, *context, output_depth, stop, resources);
}

std::expected<ImageBuffer, std::string> materialize_scrgb_as_hdr10(ImageBuffer&& image,
    std::stop_token stop_token = {}) {
  if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
  if (auto valid = hdr_detail::validate_rgba(image, "scRGB -> HDR"); !valid) return std::unexpected{valid.error()};
  auto kind = hdr_detail::signal_kind(image);
  if (!kind) return std::unexpected{kind.error()};
  if (*kind != hdr_detail::SignalKind::scrgb) {
    return std::unexpected{"只可将具有明确线性语义的 FP16 图像转换为 HDR 输出。"};
  }
  auto transform = hdr_detail::make_source_transform(image);
  if (!transform) return std::unexpected{transform.error()};
  const auto* bt2020 = pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020);
  if (!bt2020) return std::unexpected{"libplacebo 未提供所需 BT.709/BT.2020 原色定义。"};
  const auto matrix = pl_raw_primaries_equal(transform->primaries, bt2020)
      ? pl_matrix3x3{.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}}
      : pl_get_color_mapping_matrix(transform->primaries, bt2020, PL_INTENT_RELATIVE_COLORIMETRIC);
  std::optional<std::vector<std::byte>> transformed;
  if (image.geometry || !image.planes.front().borrowed.empty()) {
    auto bytes = hdr_detail::make_rgba_bytes(image.width, image.height, 2, "scRGB borrowed/geometry -> HDR");
    if (!bytes) return std::unexpected{bytes.error()};
    transformed.emplace(std::move(*bytes));
  }
  for (std::size_t y = 0; y < image.height; ++y) {
    if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
    auto* row = transformed ? transformed->data() + y * image.width * 8
        : image.planes.front().bytes.data() + y * image.planes.front().stride;
    for (std::size_t x = 0; x < image.width; ++x) {
      if (x % hdr_cost::gamut_block_pixels == 0 && stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
      const auto sample = x * 4;
      const auto rgba = hdr_detail::read_source_rgba(image, {}, x, y);
      auto rgb = hdr_detail::linear_nits({rgba[0], rgba[1], rgba[2]}, *transform);
      if (!rgb) return std::unexpected{rgb.error()};
      if (!std::isfinite(rgba[3])) return std::unexpected{"scRGB 输入包含非有限 alpha 浮点值。"};
      pl_matrix3x3_apply(&matrix, rgb->data());
      if (!hdr_detail::finite_rgb(*rgb)) return std::unexpected{"scRGB -> HDR 色彩矩阵产生了非有限值。"};
      for (std::size_t c = 0; c < 3; ++c) {
        const auto pq = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, (*rgb)[c]);
        if (!std::isfinite(pq)) return std::unexpected{"scRGB -> HDR PQ 编码产生了非有限值。"};
        hdr_detail::write_unorm16(row, sample + c, pq);
      }
      hdr_detail::write_unorm16(row, sample + 3, rgba[3]);
    }
  }
  if (transformed) {
    // The existing plane vector already has capacity for this single plane.
    image.planes.clear();
    image.planes.push_back({std::move(*transformed), image.width * 8});
  }
  ImageSourceInfo source_info{.pixel_format = PixelFormat::rgba, .bit_depth = 16,
      .color_primaries = 9, .transfer_characteristics = 16, .matrix_coefficients = 9,
      .color_range = 1, .has_hdr_metadata = true, .color_metadata_source = "scrgb-linear-to-bt2020-pq"};
  if (image.alpha_mode == AlphaMode::premultiplied) image.alpha_mode = AlphaMode::straight;
  image.geometry.reset();
  image.sample_representation = SampleRepresentation::unorm;
  image.source_info = std::move(source_info);
  image.significant_bits.reset();
  std::erase_if(image.metadata, [](const MetadataBlock& block) {
    return block.kind != MetadataKind::exif && block.kind != MetadataKind::xmp;
  });
  return std::move(image);
}

std::expected<ImageBuffer, std::string> materialize_scrgb_as_hdr10(
    const ImageBuffer& image, std::stop_token stop_token = {}) {
  if (stop_token.stop_requested()) return std::unexpected{"任务已取消。"};
  try {
    return materialize_scrgb_as_hdr10(ImageBuffer{image}, stop_token);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"scRGB -> HDR 输入复制内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"scRGB -> HDR 输入复制超过运行时限制。"};
  }
}

}  // namespace awj::hdr
