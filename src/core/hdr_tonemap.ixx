module;

#include <algorithm>
#include <array>
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
#include <utility>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/gamut_mapping.h>
#include <libplacebo/tone_mapping.h>

export module awj.hdr_tonemap;

import awj.encoding_defaults;
import awj.image;

export namespace awj::hdr {

namespace hdr_detail {

enum class SignalKind {
  none,
  scrgb,
  pq,
  hlg,
};

constexpr float kScRgbReferenceWhiteNits = 80.0F;
constexpr float kSdrTargetPeakNits = 100.0F;

std::expected<void, std::string> validate_rgba(const ImageBuffer& image,
                                               std::string_view context) {
  if (image.pixel_format != PixelFormat::rgba || image.planes.empty() ||
      (image.bit_depth != 8 && image.bit_depth != 16)) {
    return std::unexpected{std::format("{} 需要 8/16-bit RGBA 图像。", context)};
  }
  if (image.sample_representation == SampleRepresentation::ieee_half_float &&
      image.bit_depth != 16) {
    return std::unexpected{std::format("{} 浮点 RGBA 位深无效。", context)};
  }
  const auto bytes_per_sample = image.bit_depth == 16 ? std::size_t{2} : std::size_t{1};
  if (image.width == 0 || image.height == 0 ||
      image.width > std::numeric_limits<std::size_t>::max() / 4 / bytes_per_sample) {
    return std::unexpected{std::format("{} 图像尺寸无效。", context)};
  }
  const auto minimum_stride = image.width * 4 * bytes_per_sample;
  const auto& plane = image.planes.front();
  if (plane.stride < minimum_stride ||
      image.height > std::numeric_limits<std::size_t>::max() / plane.stride ||
      plane.bytes.size() < plane.stride * image.height) {
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
  if (height > std::numeric_limits<std::size_t>::max() / stride ||
      pixels.size() != stride * height) {
    return std::unexpected{std::format("{} 输出 RGBA buffer 尺寸无效。", context)};
  }
  ImageBuffer output{.width = width,
                     .height = height,
                     .pixel_format = PixelFormat::rgba,
                     .alpha_mode = alpha_mode,
                     .bit_depth = bit_depth,
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
  const auto sign = (bits >> 15) & 1U;
  const auto exponent = (bits >> 10) & 0x1fU;
  const auto mantissa = bits & 0x03ffU;
  const auto signed_value = [sign](float value) { return sign == 0 ? value : -value; };
  if (exponent == 0) {
    return signed_value(std::ldexp(static_cast<float>(mantissa), -24));
  }
  if (exponent == 0x1fU) {
    if (mantissa != 0) {
      return std::numeric_limits<float>::quiet_NaN();
    }
    return signed_value(std::numeric_limits<float>::infinity());
  }
  return signed_value(std::ldexp(1.0F + static_cast<float>(mantissa) / 1024.0F,
                                 static_cast<int>(exponent) - 15));
}

float read_sample(const ImageBuffer& image, const std::byte* row, std::size_t sample) noexcept {
  if (image.bit_depth == 8) {
    return static_cast<float>(std::to_integer<std::uint8_t>(row[sample])) / 255.0F;
  }
  std::uint16_t value{};
  std::memcpy(&value, row + sample * sizeof(value), sizeof(value));
  return image.sample_representation == SampleRepresentation::ieee_half_float
             ? binary16_to_float(value)
             : static_cast<float>(value) / 65535.0F;
}

void write_unorm16(std::byte* row, std::size_t sample, float value) noexcept {
  const auto result = static_cast<std::uint16_t>(std::clamp(
      std::lround(std::clamp(value, 0.0F, 1.0F) * 65535.0F), 0l, 65535l));
  std::memcpy(row + sample * sizeof(result), &result, sizeof(result));
}

const pl_raw_primaries* primaries_from_cicp(int value) noexcept {
  switch (value) {
    case 1:
      return pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
    case 9:
      return pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020);
    case 12:
      return pl_raw_primaries_get(PL_COLOR_PRIM_DISPLAY_P3);
    default:
      return nullptr;
  }
}

std::expected<SignalKind, std::string> signal_kind(const ImageBuffer& image) {
  const bool marked_scrgb = image.source_info &&
                            (image.source_info->color_metadata_source == "wic-scrgb-half-linear" ||
                             image.source_info->color_metadata_source == "wgc-scrgb-half-linear");
  if (image.sample_representation == SampleRepresentation::ieee_half_float) {
    const bool marked_linear = image.source_info &&
        image.source_info->transfer_characteristics == 8 &&
        image.source_info->color_primaries &&
        primaries_from_cicp(*image.source_info->color_primaries) &&
        image.source_info->linear_reference_white_nits &&
        std::isfinite(*image.source_info->linear_reference_white_nits) &&
        *image.source_info->linear_reference_white_nits > 0;
    if (!marked_scrgb && !marked_linear) {
      return std::unexpected{"浮点 RGBA 缺少线性原色与参考白亮度，拒绝猜测 HDR 色彩空间。"};
    }
    return SignalKind::scrgb;
  }
  if (!image.source_info) {
    return SignalKind::none;
  }
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
};

std::expected<SourceTransform, std::string> make_source_transform(const ImageBuffer& image) {
  auto kind = signal_kind(image);
  if (!kind) {
    return std::unexpected{kind.error()};
  }
  if (*kind == SignalKind::none) {
    return std::unexpected{"输入不是带明确 CICP 语义的 HDR 图像。"};
  }
  const auto* bt709 = pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
  if (!bt709) {
    return std::unexpected{"libplacebo 未提供 BT.709 原色定义。"};
  }
  SourceTransform result{.kind = *kind, .primaries = bt709};
  if (*kind == SignalKind::scrgb) {
    if (image.source_info && image.source_info->linear_reference_white_nits) {
      result.primaries = primaries_from_cicp(*image.source_info->color_primaries);
    }
    return result;
  }
  const auto& source = *image.source_info;
  result.primaries = primaries_from_cicp(*source.color_primaries);
  result.color_space.primaries = *source.color_primaries == 9 ? PL_COLOR_PRIM_BT_2020
      : *source.color_primaries == 12 ? PL_COLOR_PRIM_DISPLAY_P3 : PL_COLOR_PRIM_BT_709;
  result.color_space.transfer = *kind == SignalKind::pq ? PL_COLOR_TRC_PQ : PL_COLOR_TRC_HLG;
  if (source.content_light && source.content_light->max_cll > 0 &&
      (source.content_light->max_pall == 0 ||
       source.content_light->max_pall <= source.content_light->max_cll)) {
    result.color_space.hdr.max_luma = static_cast<float>(source.content_light->max_cll);
    result.color_space.hdr.max_cll = static_cast<float>(source.content_light->max_cll);
    result.color_space.hdr.max_fall = static_cast<float>(source.content_light->max_pall);
  } else if (*kind == SignalKind::hlg) {
    result.color_space.hdr.max_luma = PL_COLOR_HLG_PEAK;
  }
  return result;
}

std::expected<std::array<float, 3>, std::string> linear_nits(
    const ImageBuffer& image, const SourceTransform& transform,
    const std::byte* row, std::size_t sample) {
  std::array<float, 3> rgb{read_sample(image, row, sample),
                           read_sample(image, row, sample + 1),
                           read_sample(image, row, sample + 2)};
  if (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2])) {
    return std::unexpected{"HDR 输入包含非有限 RGB 浮点值。"};
  }
  if (transform.kind == SignalKind::scrgb) {
    for (auto& value : rgb) {
      value *= image.source_info->linear_reference_white_nits.value_or(kScRgbReferenceWhiteNits);
    }
    return rgb;
  }
  pl_color_linearize(&transform.color_space, rgb.data());
  for (auto& value : rgb) {
    value = pl_hdr_rescale(PL_HDR_NORM, PL_HDR_NITS, value);
  }
  if (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2])) {
    return std::unexpected{"libplacebo HDR 线性化产生了非有限值。"};
  }
  return rgb;
}

float luma_nits(std::array<float, 3> rgb, const pl_raw_primaries& primaries) noexcept {
  const auto matrix = pl_get_rgb2xyz_matrix(&primaries);
  pl_matrix3x3_apply(&matrix, rgb.data());
  return std::max(rgb[1], 0.0F);
}

struct LuminanceStats {
  float peak{kSdrTargetPeakNits};
  float average{};
};

std::expected<LuminanceStats, std::string> luminance_stats(
    const ImageBuffer& image, const SourceTransform& transform, std::stop_token stop = {}) {
  float scanned_peak{};
  double sum{};
  std::size_t count{};
  const auto& plane = image.planes.front();
  for (std::size_t y = 0; y < image.height; ++y) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    const auto* row = plane.bytes.data() + y * plane.stride;
    for (std::size_t x = 0; x < image.width; ++x) {
      auto rgb = linear_nits(image, transform, row, x * 4);
      if (!rgb) {
        return std::unexpected{rgb.error()};
      }
      const auto luma = luma_nits(*rgb, *transform.primaries);
      scanned_peak = std::max(scanned_peak, luma);
      sum += luma;
      ++count;
    }
  }
  const auto scanned_average = count == 0 ? 0.0F : static_cast<float>(sum / count);
  if (image.source_info && image.source_info->content_light) {
    const auto& cll = *image.source_info->content_light;
    if (cll.max_cll > 0 && (cll.max_pall == 0 || cll.max_pall <= cll.max_cll)) {
      return LuminanceStats{.peak = static_cast<float>(cll.max_cll),
                            .average = cll.max_pall > 0 ? static_cast<float>(cll.max_pall)
                                                         : scanned_average};
    }
  }
  return LuminanceStats{.peak = std::max(scanned_peak, kSdrTargetPeakNits),
                        .average = scanned_average};
}

float srgb_encode(float linear) noexcept {
  linear = std::clamp(linear, 0.0F, 1.0F);
  return linear <= 0.0031308F ? linear * 12.92F
                              : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

std::uint8_t dithered_unorm8(float value, std::size_t x, std::size_t y) noexcept {
  constexpr std::array<int, 16> bayer4x4{0, 8, 2, 10,
                                          12, 4, 14, 6,
                                          3, 11, 1, 9,
                                          15, 7, 13, 5};
  const auto dither = (static_cast<float>(bayer4x4[(y % 4) * 4 + x % 4]) - 7.5F) / 16.0F;
  return static_cast<std::uint8_t>(std::clamp(
      std::floor(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F + dither), 0.0F, 255.0F));
}

std::expected<void, std::string> copy_non_color_metadata(ImageBuffer& destination,
                                                          const ImageBuffer& source) {
  try {
    destination.metadata.reserve(source.metadata.size());
    for (const auto& block : source.metadata) {
      if (block.kind == MetadataKind::exif || block.kind == MetadataKind::xmp) {
        destination.metadata.push_back(block);
      }
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected{"复制非色彩元数据时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"复制非色彩元数据时尺寸超过运行时限制。"};
  }
  return {};
}

}  // namespace hdr_detail

std::expected<bool, std::string> has_explicit_hdr_signal(const ImageBuffer& image) {
  auto kind = hdr_detail::signal_kind(image);
  if (!kind) {
    return std::unexpected{kind.error()};
  }
  return *kind != hdr_detail::SignalKind::none;
}

std::expected<ImageBuffer, std::string> tone_map_to_sdr_srgb(
    const ImageBuffer& image, int output_depth = 8, std::stop_token stop = {}) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (output_depth != 8 && output_depth != 16) return std::unexpected{"SDR 输出位深必须为 8 或 16。"};
  if (auto valid = hdr_detail::validate_rgba(image, "HDR -> SDR"); !valid) {
    return std::unexpected{valid.error()};
  }
  auto transform = hdr_detail::make_source_transform(image);
  if (!transform) {
    return std::unexpected{transform.error()};
  }
  auto stats = hdr_detail::luminance_stats(image, *transform, stop);
  if (!stats) {
    return std::unexpected{stats.error()};
  }
  const auto* bt709 = pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
  if (!bt709) {
    return std::unexpected{"libplacebo 未提供 BT.709 原色定义。"};
  }

  pl_tone_map_params tone_params{};
  tone_params.function = &pl_tone_map_spline;
  tone_params.constants = {PL_TONE_MAP_CONSTANTS};
  tone_params.input_scaling = PL_HDR_NITS;
  tone_params.output_scaling = PL_HDR_NITS;
  tone_params.input_min = 0.0F;
  tone_params.input_max = stats->peak;
  tone_params.input_avg = stats->average;
  tone_params.output_min = 0.0F;
  tone_params.output_max = hdr_detail::kSdrTargetPeakNits;
  pl_tone_map_params_infer(&tone_params);

  pl_gamut_map_params gamut_params{};
  gamut_params.function = &pl_gamut_map_perceptual;
  gamut_params.input_gamut = *transform->primaries;
  gamut_params.output_gamut = *bt709;
  gamut_params.min_luma = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, 0.0F);
  gamut_params.max_luma = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, hdr_detail::kSdrTargetPeakNits);
  // PL_GAMUT_MAP_CONSTANTS is a C designated-initializer macro whose source
  // order intentionally differs from its declaration order. Assign the same
  // upstream defaults explicitly so this is valid C++ as well.
  gamut_params.constants.perceptual_deadzone = 0.30F;
  gamut_params.constants.perceptual_strength = 0.80F;
  gamut_params.constants.colorimetric_gamma = 1.80F;
  gamut_params.constants.softclip_knee = 0.70F;
  gamut_params.constants.softclip_desat = 0.35F;

  auto pixels = hdr_detail::make_rgba_bytes(image.width, image.height, output_depth / 8, "HDR -> SDR");
  if (!pixels) {
    return std::unexpected{pixels.error()};
  }
  auto input_to_lms = pl_ipt_rgb2lms(transform->primaries);
  auto output_to_rgb = pl_ipt_lms2rgb(bt709);
  const auto& input_plane = image.planes.front();
  for (std::size_t y = 0; y < image.height; ++y) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    const auto* input_row = input_plane.bytes.data() + y * input_plane.stride;
    auto* output_row = pixels->data() + y * image.width * 4 * (output_depth / 8);
    for (std::size_t x = 0; x < image.width; ++x) {
      const auto sample = x * 4;
      auto rgb = hdr_detail::linear_nits(image, *transform, input_row, sample);
      if (!rgb) {
        return std::unexpected{rgb.error()};
      }
      const auto source_luma = hdr_detail::luma_nits(*rgb, *transform->primaries);
      if (source_luma > 0.0F) {
        const auto mapped_luma = pl_tone_map_sample(source_luma, &tone_params);
        const auto scale = std::max(mapped_luma, 0.0F) / source_luma;
        for (auto& value : *rgb) {
          value *= scale;
        }
      }

      // The libplacebo perceptual mapper consumes IPTPQc4.  Negative scRGB
      // values remain linear until this final, non-negative PQ representation.
      std::array<float, 3> ipt{pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, std::max((*rgb)[0], 0.0F)),
                               pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, std::max((*rgb)[1], 0.0F)),
                               pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, std::max((*rgb)[2], 0.0F))};
      pl_matrix3x3_apply(&input_to_lms, ipt.data());
      pl_matrix3x3_apply(&pl_ipt_lms2ipt, ipt.data());
      pl_gamut_map_sample(ipt.data(), &gamut_params);
      pl_matrix3x3_apply(&pl_ipt_ipt2lms, ipt.data());
      pl_matrix3x3_apply(&output_to_rgb, ipt.data());
      for (auto& value : ipt) {
        value = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, std::max(value, 0.0F));
      }

      for (std::size_t c = 0; c < 3; ++c) {
        const auto encoded = hdr_detail::srgb_encode(ipt[c] / hdr_detail::kSdrTargetPeakNits);
        if (output_depth == 16) hdr_detail::write_unorm16(output_row, sample + c, encoded);
        else output_row[sample + c] = std::byte{hdr_detail::dithered_unorm8(encoded, x, y)};
      }
      const auto alpha = hdr_detail::read_sample(image, input_row, sample + 3);
      if (!std::isfinite(alpha)) {
        return std::unexpected{"HDR 输入包含非有限 alpha 浮点值。"};
      }
      if (output_depth == 16) hdr_detail::write_unorm16(output_row, sample + 3, alpha);
      else output_row[sample + 3] = std::byte{static_cast<std::uint8_t>(std::clamp(
          std::lround(std::clamp(alpha, 0.0F, 1.0F) * 255.0F), 0l, 255l))};
    }
  }

  ImageSourceInfo source_info{.pixel_format = PixelFormat::rgba,
                               .bit_depth = output_depth,
                               .color_primaries = 1,
                               .transfer_characteristics = 13,
                               .matrix_coefficients = 0,
                               .color_range = 1,
                               .has_hdr_metadata = false,
                               .color_metadata_source = "hdr-sdr-libplacebo-spline-perceptual"};
  auto output = hdr_detail::make_rgba_image(
      image.width, image.height, std::move(*pixels), image.alpha_mode, output_depth,
      SampleRepresentation::unorm, std::move(source_info), "HDR -> SDR");
  if (!output) {
    return std::unexpected{output.error()};
  }
  if (auto copied = hdr_detail::copy_non_color_metadata(*output, image); !copied) {
    return std::unexpected{copied.error()};
  }
  return output;
}

std::expected<ImageBuffer, std::string> materialize_scrgb_as_hdr10(const ImageBuffer& image) {
  if (auto valid = hdr_detail::validate_rgba(image, "scRGB -> HDR"); !valid) {
    return std::unexpected{valid.error()};
  }
  auto kind = hdr_detail::signal_kind(image);
  if (!kind) {
    return std::unexpected{kind.error()};
  }
  if (*kind != hdr_detail::SignalKind::scrgb) {
    return std::unexpected{"只可将具有明确线性语义的 FP16 图像转换为 HDR 输出。"};
  }
  auto transform = hdr_detail::make_source_transform(image);
  if (!transform) return std::unexpected{transform.error()};
  const auto* bt2020 = pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020);
  if (!bt2020) {
    return std::unexpected{"libplacebo 未提供所需 BT.709/BT.2020 原色定义。"};
  }
  const auto matrix = pl_get_color_mapping_matrix(transform->primaries, bt2020, PL_INTENT_RELATIVE_COLORIMETRIC);
  auto pixels = hdr_detail::make_rgba_bytes(image.width, image.height, 2, "scRGB -> HDR");
  if (!pixels) {
    return std::unexpected{pixels.error()};
  }
  const auto& input_plane = image.planes.front();
  for (std::size_t y = 0; y < image.height; ++y) {
    const auto* input_row = input_plane.bytes.data() + y * input_plane.stride;
    auto* output_row = pixels->data() + y * image.width * 4 * sizeof(std::uint16_t);
    for (std::size_t x = 0; x < image.width; ++x) {
      const auto sample = x * 4;
      auto linear = hdr_detail::linear_nits(image, *transform, input_row, sample);
      if (!linear) return std::unexpected{linear.error()};
      auto rgb = *linear;
      pl_matrix3x3_apply(&matrix, rgb.data());
      for (std::size_t channel = 0; channel < 3; ++channel) {
        hdr_detail::write_unorm16(output_row, sample + channel,
                                  pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ,
                                                 std::max(rgb[channel], 0.0F)));
      }
      const auto alpha = hdr_detail::read_sample(image, input_row, sample + 3);
      if (!std::isfinite(alpha)) {
        return std::unexpected{"scRGB 输入包含非有限 alpha 浮点值。"};
      }
      hdr_detail::write_unorm16(output_row, sample + 3, alpha);
    }
  }
  ImageSourceInfo source_info{.pixel_format = PixelFormat::rgba,
                               .bit_depth = 16,
                               .color_primaries = 9,
                               .transfer_characteristics = 16,
                               .matrix_coefficients = 9,
                               .color_range = 1,
                               .has_hdr_metadata = true,
                               .color_metadata_source = "scrgb-linear-to-bt2020-pq"};
  auto output = hdr_detail::make_rgba_image(
      image.width, image.height, std::move(*pixels), image.alpha_mode, 16,
      SampleRepresentation::unorm, std::move(source_info), "scRGB -> HDR");
  if (!output) {
    return std::unexpected{output.error()};
  }
  if (auto copied = hdr_detail::copy_non_color_metadata(*output, image); !copied) {
    return std::unexpected{copied.error()};
  }
  return output;
}

}  // namespace awj::hdr
