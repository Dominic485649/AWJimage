#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/gamut_mapping.h>
#include <libplacebo/tone_mapping.h>
#include "../src/core/hdr_cost.h"

import awj.hdr_tonemap;
import awj.image;
import awj.resource_planner;

namespace {

int fail(std::string_view message) { std::cerr << message << '\n'; return 1; }

float half_reference(std::uint16_t bits) {
  const auto exponent = (bits >> 10) & 31u;
  const auto mantissa = bits & 1023u;
  const auto sign = (bits & 0x8000u) ? -1.0F : 1.0F;
  return exponent == 0 ? sign * std::ldexp(static_cast<float>(mantissa), -24)
      : exponent == 31 ? (mantissa ? std::numeric_limits<float>::quiet_NaN() : sign * std::numeric_limits<float>::infinity())
      : sign * std::ldexp(1.0F + mantissa / 1024.0F, static_cast<int>(exponent) - 15);
}

std::uint16_t make_half(float value) {
  const auto bits = std::bit_cast<std::uint32_t>(value);
  const auto sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
  const auto mantissa = bits & 0x7fffffu;
  const int exponent = static_cast<int>((bits >> 23) & 255u) - 112;
  if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u | (mantissa ? 0x0200u : 0u));
  if (exponent < -10) return sign;
  const auto significand = exponent <= 0 ? mantissa | 0x800000u : mantissa;
  const auto shift = exponent <= 0 ? 14 - exponent : 13;
  const auto remainder_mask = (std::uint32_t{1} << shift) - 1;
  auto rounded = significand >> shift;
  const auto remainder = significand & remainder_mask;
  const auto midpoint = std::uint32_t{1} << (shift - 1);
  if (remainder > midpoint || (remainder == midpoint && (rounded & 1u))) ++rounded;
  return static_cast<std::uint16_t>(sign | ((exponent > 0 ? static_cast<unsigned>(exponent) << 10 : 0) + rounded));
}

void store16(std::byte* destination, std::uint16_t value) { std::memcpy(destination, &value, 2); }

float input_sample(const awj::ImageBuffer& image, std::size_t x, std::size_t y, std::size_t channel) {
  const auto* row = image.planes[0].bytes.data() + y * image.planes[0].stride;
  if (image.bit_depth == 8) return std::to_integer<unsigned>(row[x * 4 + channel]) / 255.0F;
  std::uint16_t bits{};
  std::memcpy(&bits, row + (x * 4 + channel) * 2, 2);
  return image.sample_representation == awj::SampleRepresentation::ieee_half_float ? half_reference(bits) : bits / 65535.0F;
}

awj::ImageBuffer linear_fixture(int primaries = 1, std::size_t width = 73, std::size_t height = 29) {
  awj::ImageBuffer image{.width = width, .height = height, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 16,
      .sample_representation = awj::SampleRepresentation::ieee_half_float,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
          .color_primaries = primaries, .transfer_characteristics = 8, .matrix_coefficients = 0,
          .color_range = 1, .has_hdr_metadata = true,
          .color_metadata_source = primaries == 1 ? "wic-scrgb-half-linear" : "test-explicit-linear",
          .linear_reference_white_nits = 80.0F}};
  awj::ImagePlane plane{.stride = width * 8 + 24};
  plane.bytes.resize(plane.stride * height, std::byte{0xa5});
  constexpr std::array<std::array<float, 4>, 20> colors{{
      {0, 0, 0, 1}, {0x1p-24F, 0x1p-24F, 0x1p-24F, 0},
      {0.00003F, 0.00003F, 0.00003F, 0.25F}, {0.001F, 0.001F, 0.001F, 0.5F},
      {0.125F, 0.125F, 0.125F, 1}, {0.5F, 0.5F, 0.5F, 1}, {1, 1, 1, 1},
      {2.5375F, 2.5375F, 2.5375F, 1}, {12.5F, 12.5F, 12.5F, 1},
      {1.25F, 0, 0, 1}, {0, 1.25F, 0, 1}, {0, 0, 1.25F, 1},
      {0, 1.25F, 1.25F, 1}, {1.25F, 0, 1.25F, 1}, {1.25F, 1.25F, 0, 1},
      {0.5F, -0.25F, 2, 1}, {-0.125F, 1.5F, 0.125F, 0.5F},
      {125, 0.125F, -0.25F, 1}, {0.0001F, -0.00003F, 0.0002F, 1},
      {-0.25F, -0.125F, -0.5F, 1}}};
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto& rgba = colors[(x * 7 + y * 11) % colors.size()];
      for (std::size_t c = 0; c < 4; ++c) store16(plane.bytes.data() + y * plane.stride + (x * 4 + c) * 2, make_half(rgba[c]));
    }
  }
  image.planes.push_back(std::move(plane));
  return image;
}

awj::ImageBuffer transfer_fixture(int transfer) {
  auto image = linear_fixture(9, 17, 9);
  image.sample_representation = awj::SampleRepresentation::unorm;
  image.source_info->transfer_characteristics = transfer;
  image.source_info->linear_reference_white_nits.reset();
  image.source_info->color_metadata_source = "test-cicp-hdr";
  image.source_info->content_light = awj::HdrContentLightMetadata{1000, 120};
  constexpr std::array<float, 9> values{0, 0.00001F, 0.001F, 0.01F, 0.1F, 0.25F, 0.5F, 0.75F, 1};
  for (std::size_t y = 0; y < image.height; ++y) for (std::size_t x = 0; x < image.width; ++x) {
    for (std::size_t c = 0; c < 4; ++c) {
      const auto value = c == 3 ? (x % 2 ? 0.5F : 1.0F) : values[(x + y * 3 + c * 2) % values.size()];
      store16(image.planes[0].bytes.data() + y * image.planes[0].stride + (x * 4 + c) * 2,
          static_cast<std::uint16_t>(std::lround(value * 65535.0F)));
    }
  }
  return image;
}

// 独立 ST 2084 标量参考，不调用应用换算函数或查表路径。
double oracle_tone_pq(double nits) {
  if (nits <= 0.0) return 0.0;
  const double normalized = std::pow(nits * 0.0001, 0.1593017578125);
  return std::pow((0.8359375 + 18.8515625 * normalized) / (1.0 + 18.6875 * normalized), 78.84375);
}

double oracle_tone_nits(double pq) {
  if (pq <= 0.0) return 0.0;
  const double signal = std::pow(pq, 1.0 / 78.84375);
  return 10000.0 * std::pow(std::max(signal - 0.8359375, 0.0) /
      (18.8515625 - 18.6875 * signal), 1.0 / 0.1593017578125);
}

float oracle_tone_scalar(float nits, const pl_tone_map_params& params) {
  auto native = params;
  native.input_min = static_cast<float>(oracle_tone_pq(params.input_min));
  native.input_max = static_cast<float>(oracle_tone_pq(params.input_max));
  native.input_avg = static_cast<float>(oracle_tone_pq(params.input_avg));
  native.output_min = static_cast<float>(oracle_tone_pq(params.output_min));
  native.output_max = static_cast<float>(oracle_tone_pq(params.output_max));
  native.input_scaling = native.output_scaling = PL_HDR_PQ;
  const auto input = static_cast<float>(oracle_tone_pq(std::clamp(nits, params.input_min, params.input_max)));
  return static_cast<float>(oracle_tone_nits(pl_tone_map_sample(input, &native)));
}

struct Oracle {
  const awj::ImageBuffer& image;
  const pl_raw_primaries* primaries;
  pl_color_space color{};
  pl_matrix3x3 xyz{}, rgb_to_lms{}, lms_to_rgb{};
  pl_tone_map_params tone{};
  pl_gamut_map_params gamut{};
  float scanned_peak{}, scanned_average{};

  std::array<float, 3> nits(std::size_t x, std::size_t y) const {
    std::array<float, 3> rgb{input_sample(image, x, y, 0), input_sample(image, x, y, 1), input_sample(image, x, y, 2)};
    if (image.sample_representation == awj::SampleRepresentation::ieee_half_float) {
      for (auto& c : rgb) c *= image.source_info->linear_reference_white_nits.value_or(80.0F);
    } else if (color.transfer == PL_COLOR_TRC_HLG) {
      // BT.2100 OETF^-1 和显示 OOTF 的独立标尺，不能复用缺少峰值缩放的库函数。
      const float peak = std::clamp(color.hdr.max_luma, PL_COLOR_HDR_BLACK, 10000.0F);
      const float gamma = 1.2F * std::pow(1.111F, std::log2(peak / 1000.0F));
      // 锁定新版的无限对比度哨兵不再作为非零信号黑点参与 HLG OETF。
      constexpr float black = 0.0F;
      for (auto& c : rgb) {
        c = (1.0F - black) * c + black;
        c = c > 0.5F ? std::exp((c - 0.55991073F) / 0.17883277F) + 0.28466892F : 4.0F * c * c;
      }
      const float y = xyz.m[1][0] * rgb[0] + xyz.m[1][1] * rgb[1] + xyz.m[1][2] * rgb[2];
      const float scale = std::pow(std::max(y / 12.0F, 0.0F), gamma - 1.0F);
      for (auto& c : rgb) c = (scale * c / 12.0F) * peak;
    } else {
      pl_color_linearize(&color, rgb.data());
      for (auto& c : rgb) c = pl_hdr_rescale(PL_HDR_NORM, PL_HDR_NITS, c);
    }
    return rgb;
  }

  explicit Oracle(const awj::ImageBuffer& input) : image(input), primaries(pl_raw_primaries_get(
      input.source_info->color_primaries == 9 ? PL_COLOR_PRIM_BT_2020 :
      input.source_info->color_primaries == 12 ? PL_COLOR_PRIM_DISPLAY_P3 : PL_COLOR_PRIM_BT_709)) {
    color.primaries = input.source_info->color_primaries == 9 ? PL_COLOR_PRIM_BT_2020 :
        input.source_info->color_primaries == 12 ? PL_COLOR_PRIM_DISPLAY_P3 : PL_COLOR_PRIM_BT_709;
    color.transfer = input.source_info->transfer_characteristics == 16 ? PL_COLOR_TRC_PQ : PL_COLOR_TRC_HLG;
    if (input.source_info->content_light && input.source_info->content_light->max_cll > 0 &&
        (input.source_info->content_light->max_pall == 0 || input.source_info->content_light->max_pall <= input.source_info->content_light->max_cll)) {
      color.hdr.max_luma = color.hdr.max_cll = input.source_info->content_light->max_cll;
      color.hdr.max_fall = input.source_info->content_light->max_pall;
    } else if (color.transfer == PL_COLOR_TRC_HLG) color.hdr.max_luma = PL_COLOR_HLG_PEAK;
    xyz = pl_get_rgb2xyz_matrix(primaries);
    rgb_to_lms = pl_ipt_rgb2lms(primaries);
    lms_to_rgb = pl_ipt_lms2rgb(pl_raw_primaries_get(PL_COLOR_PRIM_BT_709));
    double sum{};
    for (std::size_t y = 0; y < image.height; ++y) for (std::size_t x = 0; x < image.width; ++x) {
      auto rgb = nits(x, y);
      pl_matrix3x3_apply(&xyz, rgb.data());
      const auto luma = std::max(0.0F, rgb[1]);
      scanned_peak = std::max(scanned_peak, luma); sum += luma;
    }
    scanned_average = static_cast<float>(sum / (image.width * image.height));
    float peak = scanned_peak, average = scanned_average;
    if (image.source_info->content_light) {
      const auto cll = *image.source_info->content_light;
      if (cll.max_cll > 0 && (cll.max_pall == 0 || cll.max_pall <= cll.max_cll)) {
        peak = cll.max_cll;
        if (cll.max_pall > 0) average = cll.max_pall;
      }
    }
    peak = std::max(peak, 100.0F); average = std::clamp(average, 0.0F, peak);
    tone.function = &pl_tone_map_spline; tone.constants = {PL_TONE_MAP_CONSTANTS};
    tone.input_scaling = tone.output_scaling = PL_HDR_NITS;
    tone.input_max = peak; tone.input_avg = average; tone.output_max = 100.0F;
    pl_tone_map_params_infer(&tone);
    gamut.function = &pl_gamut_map_perceptual;
    gamut.input_gamut = *primaries; gamut.output_gamut = *pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
    gamut.max_luma = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, 100.0F);
    gamut.constants.perceptual_deadzone = 0.30F; gamut.constants.perceptual_strength = 0.80F;
    gamut.constants.colorimetric_gamma = 1.80F; gamut.constants.softclip_knee = 0.70F;
    gamut.constants.softclip_desat = 0.35F;
  }

  std::array<float, 3> srgb(std::size_t x, std::size_t y) const {
    auto rgb = nits(x, y), luma = rgb;
    pl_matrix3x3_apply(&xyz, luma.data());
    if (luma[1] > 0) {
      const auto scale = oracle_tone_scalar(luma[1], tone) / luma[1];
      for (auto& c : rgb) c *= scale;
    }
    // 同色域合法显示体积应是恒等映射；新版近似 PQ 往返不作为黑点参考。
    // 其余情况仍独立调用公开标量接口，不复用应用转换或批处理 helper。
    const bool identity = pl_gamut_map_params_noop(&gamut) &&
        std::ranges::all_of(rgb, [](float c) { return c >= 0.0F && c <= 100.0F; });
    if (!identity) {
      pl_matrix3x3_apply(&rgb_to_lms, rgb.data());
      for (auto& c : rgb) c = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, c);
      pl_matrix3x3_apply(&pl_ipt_lms2ipt, rgb.data());
      pl_gamut_map_sample(rgb.data(), &gamut);
      pl_matrix3x3_apply(&pl_ipt_ipt2lms, rgb.data());
      for (auto& c : rgb) c = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, c);
      pl_matrix3x3_apply(&lms_to_rgb, rgb.data());
    }
    for (auto& c : rgb) {
      c = std::clamp(c / 100.0F, 0.0F, 1.0F);
      c = c <= 0.0031308F ? 12.92F * c : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
    }
    return rgb;
  }
};

int quantized_oracle_check(const awj::ImageBuffer& input, const awj::ImageBuffer& output, const Oracle& oracle) {
  constexpr std::array<int, 16> bayer{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
  if (output.width != input.width || output.height != input.height || output.alpha_mode != input.alpha_mode ||
      output.sample_representation != awj::SampleRepresentation::unorm || !output.source_info ||
      output.source_info->color_primaries != 1 || output.source_info->transfer_characteristics != 13 || output.source_info->has_hdr_metadata)
    return fail("Tone output layout/color contract differs.");
  for (std::size_t y = 0; y < input.height; ++y) for (std::size_t x = 0; x < input.width; ++x) {
    const auto reference = oracle.srgb(x, y);
    const auto* row = output.planes[0].bytes.data() + y * output.planes[0].stride;
    for (std::size_t c = 0; c < 3; ++c) {
      if (!std::isfinite(reference[c])) return fail("Independent scalar oracle is not finite.");
      if (output.bit_depth == 16) {
        std::uint16_t actual{}; std::memcpy(&actual, row + (x * 4 + c) * 2, 2);
        if (std::abs(actual / 65535.0F - reference[c]) > 2.0e-4F + 0.5F / 65535.0F) {
          std::cerr << "tone pixel=" << x << ',' << y << " channel=" << c << " actual=" << actual / 65535.0F
                    << " reference=" << reference[c] << " primaries=" << input.source_info->color_primaries.value_or(0)
                    << " transfer=" << input.source_info->transfer_characteristics.value_or(0) << '\n';
          return fail("16-bit tone/gamut exceeds independent sRGB oracle tolerance.");
        }
      } else {
        const auto dither = (static_cast<float>(bayer[(y % 4) * 4 + x % 4]) - 7.5F) / 16.0F;
        const auto expected = static_cast<int>(std::clamp(std::floor(reference[c] * 255.0F + 0.5F + dither), 0.0F, 255.0F));
        if (std::abs(static_cast<int>(std::to_integer<unsigned>(row[x * 4 + c])) - expected) > 1)
          return fail("8-bit tone/gamut differs by more than one code from oracle.");
      }
    }
    const auto alpha = std::clamp(input_sample(input, x, y, 3), 0.0F, 1.0F);
    if (output.bit_depth == 16) {
      std::uint16_t actual{}; std::memcpy(&actual, row + (x * 4 + 3) * 2, 2);
      if (actual != std::lround(alpha * 65535.0F)) return fail("16-bit alpha changed.");
    } else if (std::to_integer<unsigned>(row[x * 4 + 3]) != static_cast<unsigned>(std::lround(alpha * 255.0F))) return fail("8-bit alpha was dithered or changed.");
  }
  return 0;
}

int check_lut() {
  for (const auto [peak, average] : std::array<std::array<float, 2>, 8>{{
      {100, 0}, {203, 0}, {203, 40}, {215, 100}, {1000, 0}, {1000, 220}, {10000, 1800}, {100, 100}}}) {
    pl_tone_map_params params{};
    params.function = &pl_tone_map_spline; params.constants = {PL_TONE_MAP_CONSTANTS};
    params.input_scaling = params.output_scaling = PL_HDR_NITS;
    params.input_max = peak; params.input_avg = average; params.output_max = 100;
    pl_tone_map_params_infer(&params);
    auto curve = awj::hdr::hdr_detail::make_tone_curve(params, true, UINT64_MAX);
    if (!curve || curve->nits.size() != 16'385 || curve->pq_params.input_scaling != PL_HDR_PQ ||
        curve->pq_params.output_scaling != PL_HDR_NITS ||
        curve->pq_params.input_avg != static_cast<float>(oracle_tone_pq(average))) return fail("PQ tone LUT scaling/size contract differs.");
    float previous{};
    float previous_input{};
    float previous_scalar{};
    for (std::size_t i = 0; i < 16'384; ++i) for (const float fraction : {0.0F, 0.499F, 0.999F}) {
      const auto pq = curve->pq_params.input_max * (static_cast<float>(i) + fraction) / 16'384.0F;
      const auto nits = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, pq);
      const auto expected = oracle_tone_scalar(nits, params);
      const auto actual = curve->sample(nits);
      if (!std::isfinite(actual) || std::abs(actual - expected) > 0.001F) {
        std::cerr << "tone peak=" << peak << " avg=" << average << " input=" << nits
                  << " actual=" << actual << " scalar=" << expected << '\n';
        return fail("Tone LUT exceeds 0.001-nit scalar error.");
      }
      if (actual + 0.001F < previous) {
        std::cerr << "monotonic peak=" << peak << " avg=" << average << " i=" << i << " fraction=" << fraction
                  << " input=" << nits << " previous_input=" << previous_input << " actual=" << actual
                  << " previous=" << previous << " scalar=" << expected << " previous_scalar=" << previous_scalar << '\n';
        return fail("Gray tone curve is not monotonic.");
      }
      previous = actual;
      previous_input = nits;
      previous_scalar = expected;
    }
    for (float nits : {-1.0F, 0.0F, 1.0e-12F, 1.0e-8F, peak, peak * 1.001F, peak * 4}) {
      if (curve->sample(nits) != oracle_tone_scalar(nits, params)) return fail("Tone black/near-black/endpoint/out-of-domain fallback differs from scalar.");
    }
    auto scalar = awj::hdr::hdr_detail::make_tone_curve(params, true, 0);
    if (!scalar || !scalar->nits.empty() || scalar->sample(peak * 0.37F) != oracle_tone_scalar(peak * 0.37F, params)) return fail("Low-memory tone fallback reduced quality.");
  }
  pl_tone_map_params shifted{};
  shifted.function = &pl_tone_map_spline; shifted.constants = {PL_TONE_MAP_CONSTANTS};
  shifted.input_scaling = shifted.output_scaling = PL_HDR_NITS;
  shifted.input_min = 0.01F; shifted.input_max = 1000; shifted.input_avg = 120;
  shifted.output_min = 0.005F; shifted.output_max = 100;
  pl_tone_map_params_infer(&shifted);
  auto nonzero_black = awj::hdr::hdr_detail::make_tone_curve(shifted, true, UINT64_MAX);
  if (!nonzero_black || nonzero_black->pq_params.input_min != static_cast<float>(oracle_tone_pq(shifted.input_min)) ||
      nonzero_black->pq_params.input_max != static_cast<float>(oracle_tone_pq(shifted.input_max)) ||
      nonzero_black->pq_params.input_avg != static_cast<float>(oracle_tone_pq(shifted.input_avg)) ||
      nonzero_black->pq_params.output_min != shifted.output_min || nonzero_black->pq_params.output_max != shifted.output_max)
    return fail("Tone LUT did not rescale input min/max/average together or changed output nits units.");
  for (std::size_t i = 0; i <= 4096; ++i) {
    const auto pq = std::lerp(nonzero_black->pq_params.input_min, nonzero_black->pq_params.input_max,
        static_cast<float>(i) / 4096.0F);
    const auto nits = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, pq);
    if (std::abs(nonzero_black->sample(nits) - oracle_tone_scalar(nits, shifted)) > 0.001F)
      return fail("Tone LUT with a nonzero black point exceeds 0.001 nits.");
  }
  auto invalid = shifted; invalid.input_avg = std::numeric_limits<float>::infinity();
  if (awj::hdr::hdr_detail::make_tone_curve(invalid, true, UINT64_MAX)) return fail("Non-finite tone statistics were accepted.");
  return 0;
}

int check_gamut_partitions() {
  auto source = linear_fixture(9, 1, 1);
  Oracle oracle(source);
  constexpr std::size_t count = 1'061, stride = 5;
  std::vector<float> original(count * stride, 123.0F), scalar = original;
  for (std::size_t i = 0; i < count; ++i) {
    // Adjacent hues straddle upstream's 1e-3 peak-cache threshold and +/-pi.
    const auto hue = i % 3 == 0 ? -std::numbers::pi_v<float> + static_cast<float>(i % 17) * 0.00049F
        : i % 3 == 1 ? std::numbers::pi_v<float> - static_cast<float>(i % 17) * 0.00051F
        : -2.8F + static_cast<float>(i % 47) * 0.00099F;
    const auto chroma = 0.03F + static_cast<float>(i % 7) * 0.017F;
    original[i * stride] = 0.15F + static_cast<float>(i % 13) * 0.024F;
    original[i * stride + 1] = chroma * std::cos(hue);
    original[i * stride + 2] = chroma * std::sin(hue);
  }
  scalar = original;
  for (std::size_t i = 0; i < count; ++i) pl_gamut_map_sample(scalar.data() + i * stride, &oracle.gamut);
  const auto encode = [&](const float* input) {
    std::array<float, 3> rgb{input[0], input[1], input[2]};
    pl_matrix3x3_apply(&pl_ipt_ipt2lms, rgb.data());
    for (auto& c : rgb) c = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, c);
    pl_matrix3x3_apply(&oracle.lms_to_rgb, rgb.data());
    for (auto& c : rgb) { c = std::clamp(c / 100.0F, 0.0F, 1.0F); c = c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F; }
    return rgb;
  };
  for (const std::size_t block : {1, 17, 1024}) {
    auto batch = original;
    for (std::size_t first = 0; first < count; first += block) {
      if (!awj::hdr::hdr_detail::map_gamut_block(batch.data() + first * stride,
          std::min(block, count - first), stride, oracle.gamut)) return fail("Bounded gamut callback failed.");
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto reference = encode(scalar.data() + i * stride), actual = encode(batch.data() + i * stride);
      for (std::size_t c = 0; c < 3; ++c) {
        if (!std::isfinite(actual[c]) || std::abs(actual[c] - reference[c]) > 2.0e-4F ||
            std::abs(std::lround(actual[c] * 255) - std::lround(reference[c] * 255)) > 1) {
          std::cerr << "gamut block=" << block << " pixel=" << i << " channel=" << c << " actual=" << actual[c]
                    << " scalar=" << reference[c] << " ipt=[" << original[i * stride] << ','
                    << original[i * stride + 1] << ',' << original[i * stride + 2] << "]\n";
          return fail("Partitioned gamut mapping exceeds pre-quantization sRGB/code tolerance.");
        }
      }
      if (batch[i * stride + 3] != 123 || batch[i * stride + 4] != 123) return fail("Gamut callback ignored float stride.");
    }
  }
  if (awj::hdr::hdr_detail::map_gamut_block(original.data(), 1025, 3, oracle.gamut) ||
      awj::hdr::hdr_detail::map_gamut_block(original.data(), 2, SIZE_MAX, oracle.gamut) ||
      awj::hdr::hdr_detail::map_gamut_block(original.data(), 1, 2, oracle.gamut) ||
      awj::hdr::hdr_detail::map_gamut_block(nullptr, 1, 3, oracle.gamut)) return fail("Invalid gamut count/stride was accepted.");
  original[0] = std::numeric_limits<float>::quiet_NaN();
  if (awj::hdr::hdr_detail::map_gamut_block(original.data(), 1, 3, oracle.gamut)) return fail("Non-finite IPT was accepted.");
  return 0;
}

int check_fixture(const awj::ImageBuffer& source) {
  Oracle oracle(source);
  auto context = awj::hdr::prepare_tone_map(source);
  if (!context || std::abs(context->scanned_y.peak - oracle.scanned_peak) > 0.001F ||
      std::abs(context->scanned_y.average - oracle.scanned_average) > 0.001F) return fail("Prepared raw CIE Y statistics differ from independent scan.");
  for (const auto block : {std::size_t{1}, std::size_t{17}, std::size_t{1024}}) {
    for (const auto workers : {1, 4}) for (const auto depth : {8, 16}) {
      awj::ResourcePlan resources{.file_parallelism = 2, .encoder_threads_per_file = workers,
          .global_thread_budget = workers * 2, .memory_limit_bytes = 0};
      auto mapped = awj::hdr::hdr_detail::tone_map_prepared(source, *context, depth, {}, resources,
          {.block_pixels = block, .use_lut = true, .lut_min_pixels = 0});
      if (!mapped) return fail(mapped.error());
      if (const auto result = quantized_oracle_check(source, *mapped, oracle)) return result;
    }
  }
  awj::ResourcePlan tight{.file_parallelism = 64, .encoder_threads_per_file = 8, .global_thread_budget = 128,
      .memory_limit_bytes = source.planes[0].bytes.size() + source.width * source.height * 8 + 32};
  auto low_memory = awj::hdr::tone_map_to_sdr_srgb(source, *context, 16, {}, tight);
  if (!low_memory) return fail(low_memory.error());
  if (const auto result = quantized_oracle_check(source, *low_memory, oracle)) return result;
  auto copy = source;
  if (awj::hdr::tone_map_to_sdr_srgb(copy, *context)) return fail("Prepared context was reused on another image.");
  return 0;
}

}  // namespace

int main() {
  auto hlg_white = transfer_fixture(18);
  auto hlg_transform = awj::hdr::hdr_detail::make_source_transform(hlg_white);
  if (!hlg_transform) return fail(hlg_transform.error());
  const auto hlg_nits = awj::hdr::hdr_detail::linear_nits({1.0F, 1.0F, 1.0F}, *hlg_transform);
  if (!hlg_nits || std::abs((*hlg_nits)[0] - hlg_transform->color_space.hdr.max_luma) > 0.01F)
    return fail("HLG white was treated as 203-nit normalized light instead of the declared display peak.");
  for (std::uint32_t bits = 0; bits <= 65535; ++bits) {
    const auto reference = half_reference(static_cast<std::uint16_t>(bits));
    const auto value = awj::hdr::hdr_detail::binary16_to_float(static_cast<std::uint16_t>(bits));
    if (std::isnan(reference) ? !std::isnan(value) : std::bit_cast<std::uint32_t>(reference) != std::bit_cast<std::uint32_t>(value))
      return fail("FP16 conversion differs from the independent formula.");
  }
  if (const auto result = check_lut()) return result;
  if (const auto result = check_gamut_partitions()) return result;
  for (const auto primaries : {1, 9, 12}) {
    auto source = linear_fixture(primaries);
    if (const auto result = check_fixture(source)) return result;
    source.source_info->content_light = awj::HdrContentLightMetadata{203, 80};
    if (const auto result = check_fixture(source)) return result;
  }
  for (const auto transfer : {16, 18}) if (const auto result = check_fixture(transfer_fixture(transfer))) return result;

  auto source = linear_fixture(1, 1, 1);
  // Explicitly retain the original scRGB negative/extended sample regression.
  const std::array<std::uint16_t, 4> original{0x3800u, 0xb400u, 0x4000u, 0x3c00u};
  for (std::size_t c = 0; c < 4; ++c) store16(source.planes[0].bytes.data() + c * 2, original[c]);
  awj::compat::stop_source canceled; canceled.request_stop();
  auto context = awj::hdr::prepare_tone_map(source);
  if (!context) return fail(context.error());
  if (awj::hdr::tone_map_to_sdr_srgb(source, 8, canceled.get_token()) ||
      awj::hdr::prepare_tone_map(source, canceled.get_token()) ||
      awj::hdr::tone_map_to_sdr_srgb(source, *context, 16, canceled.get_token()) ||
      awj::hdr::hdr_detail::make_tone_curve(context->tone_params, true, UINT64_MAX, canceled.get_token())) return fail("HDR cancellation was ignored.");
  auto signal = awj::hdr::has_explicit_hdr_signal(source);
  if (!signal || !*signal) return fail("scRGB HDR signal was not recognized.");
  if (context->transform.linear_reference_white_nits != 80.0F) return fail("scRGB 1.0 no longer means 80 nits.");
  auto hdr10 = awj::hdr::materialize_scrgb_as_hdr10(source);
  if (!hdr10 || hdr10->bit_depth != 16 || hdr10->sample_representation != awj::SampleRepresentation::unorm ||
      !hdr10->source_info || hdr10->source_info->color_primaries != 9 ||
      hdr10->source_info->transfer_characteristics != 16 || hdr10->source_info->matrix_coefficients != 9) return fail("HDR10 materialization color contract differs.");
  auto owned = source;
  const auto* storage = owned.planes[0].bytes.data();
  auto inplace = awj::hdr::materialize_scrgb_as_hdr10(std::move(owned));
  if (!inplace || inplace->planes[0].bytes.data() != storage || inplace->planes[0].bytes != hdr10->planes[0].bytes) return fail("HDR conversion did not reuse input storage or changed samples.");
  if (awj::hdr::materialize_scrgb_as_hdr10(awj::ImageBuffer{source}, canceled.get_token())) return fail("HDR materialization ignored cancellation.");
  auto ambiguous = source; ambiguous.source_info->color_metadata_source.clear(); ambiguous.source_info->linear_reference_white_nits.reset();
  if (awj::hdr::has_explicit_hdr_signal(ambiguous)) return fail("Ambiguous float HDR was accepted.");
  auto nonfinite = source; store16(nonfinite.planes[0].bytes.data(), 0x7c00u);
  if (awj::hdr::tone_map_to_sdr_srgb(nonfinite)) return fail("Infinite source RGB was accepted.");
  nonfinite = source; store16(nonfinite.planes[0].bytes.data() + 6, 0x7e00u);
  if (awj::hdr::tone_map_to_sdr_srgb(nonfinite)) return fail("NaN alpha was accepted.");
  nonfinite = source; nonfinite.source_info->linear_reference_white_nits = std::numeric_limits<float>::infinity();
  if (awj::hdr::tone_map_to_sdr_srgb(nonfinite)) return fail("Non-finite reference white was accepted.");
  nonfinite = source; nonfinite.source_info->linear_reference_white_nits = std::numeric_limits<float>::max();
  if (awj::hdr::tone_map_to_sdr_srgb(nonfinite)) return fail("Overflowing nits scaling was accepted.");
  auto huge = source; huge.width = SIZE_MAX;
  if (awj::hdr::tone_map_to_sdr_srgb(huge)) return fail("Overflowing RGBA dimensions were accepted.");
  if (awj::hdr::tone_map_to_sdr_srgb(source, 10)) return fail("Unsupported SDR depth was accepted.");

  const std::array<float, 3> inside{1, 5, 50}, negative{-0.001F, 5, 50}, extended{101, 5, 50};
  if (!awj::hdr::hdr_detail::can_skip_gamut(inside, *context) || awj::hdr::hdr_detail::can_skip_gamut(negative, *context) ||
      awj::hdr::hdr_detail::can_skip_gamut(extended, *context)) return fail("Same-gamut fast-path legal-volume contract differs.");
  auto wide = linear_fixture(9, 1, 1); auto wide_context = awj::hdr::prepare_tone_map(wide);
  if (!wide_context || awj::hdr::hdr_detail::can_skip_gamut(inside, *wide_context)) return fail("Wide gamut incorrectly bypassed mapping.");
  if (awj::hdr::hdr_detail::tone_worker_budget({.file_parallelism = 2, .encoder_threads_per_file = 8, .global_thread_budget = 6}) != 3)
    return fail("Tone workers exceeded total plan budget.");
  if (awj::hdr_cost::multiply(UINT64_MAX, 2) != UINT64_MAX || awj::hdr_cost::add(UINT64_MAX, 1) != UINT64_MAX ||
      awj::hdr_cost::jpeg_working_set_bytes(SIZE_MAX, SIZE_MAX) != UINT64_MAX ||
      awj::hdr_cost::jpeg_working_set_bytes(73, 29, 1000) <= awj::hdr_cost::jpeg_working_set_bytes(73, 29, 0)) return fail("HDR working-set estimate overflow/compressed-input contract differs.");

  auto cancel_image = linear_fixture(9, 1025, 128);
  auto cancel_context = awj::hdr::prepare_tone_map(cancel_image);
  if (!cancel_context) return fail(cancel_context.error());
  awj::compat::stop_source during;
  struct CancelProgress {
    const awj::ImageBuffer* image;
    awj::compat::stop_source* stop;
    mutable std::atomic<std::size_t> entered{}, completed{}, active{};
  } progress{&cancel_image, &during};
  // Bind after preparation: cancellation is raised at an actual tone pixel
  // read, not during statistics or after a scheduler-dependent sleep. There is
  // no rendezvous/helper thread that can deadlock on allocation/spawn failure.
  cancel_context->reader = {.context = &progress,
      .read = [](const void* context, std::size_t x, std::size_t y) noexcept {
        const auto& state = *static_cast<const CancelProgress*>(context);
        state.active.fetch_add(1, std::memory_order_relaxed);
        struct CompleteRead {
          const CancelProgress& state;
          ~CompleteRead() {
            state.completed.fetch_add(1, std::memory_order_relaxed);
            state.active.fetch_sub(1, std::memory_order_release);
          }
        } finished{state};
        const auto first = state.entered.fetch_add(1, std::memory_order_relaxed) == 0;
        const auto rgba = awj::hdr::hdr_detail::read_source_rgba(*state.image, {}, x, y);
        if (first) state.stop->request_stop();
        return rgba;
      }};
  auto interrupted = awj::hdr::tone_map_to_sdr_srgb(cancel_image, *cancel_context, 16, during.get_token(),
      {.file_parallelism = 1, .encoder_threads_per_file = 4, .global_thread_budget = 4});
  if (interrupted || !during.stop_requested() || progress.entered.load() == 0)
    return fail("Deterministic in-flight tone cancellation was not reached.");
  if (progress.active.load(std::memory_order_acquire) != 0 || progress.entered.load() != progress.completed.load())
    return fail("Tone returned before all borrowed reader calls completed/joined.");
  cancel_image.planes.clear(); // The joined API must no longer access the source.
  return 0;
}
