module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <avif/avif.h>
#include <avif/internal.h>
#include <libplacebo/colorspace.h>
#include "hdr_cost.h"

export module awj.pixel_conversion;

import awj.decoder_common;
import awj.encoding_defaults;
import awj.hdr_tonemap;
import awj.image;
import awj.resource_planner;

export namespace awj {

struct PixelConversionRequest {
  PixelFormat format{PixelFormat::rgba};
  int bit_depth{8};
  int matrix_coefficients{6};
  int color_range{1};
  bool preserve_alpha{true};
  bool tone_map_to_sdr{};
  std::optional<int> color_primaries{};
  std::optional<int> transfer_characteristics{};
};

bool is_yuv_format(PixelFormat format) noexcept {
  return format == PixelFormat::yuv444 || format == PixelFormat::yuv422 ||
         format == PixelFormat::yuv420 || format == PixelFormat::gray;
}

bool pixels_match(const ImageBuffer& image, const PixelConversionRequest& request) noexcept {
  return image.pixel_format == request.format && image.bit_depth == request.bit_depth &&
      image.sample_representation == SampleRepresentation::unorm && !request.tone_map_to_sdr && !image.geometry &&
      image.alpha_mode != AlphaMode::premultiplied &&
      (request.preserve_alpha || image.alpha_mode == AlphaMode::none) &&
      (!request.color_primaries || (image.source_info && image.source_info->color_primaries == request.color_primaries)) &&
      (!request.transfer_characteristics || (image.source_info && image.source_info->transfer_characteristics == request.transfer_characteristics)) &&
      (!is_yuv_format(request.format) || (image.source_info &&
       image.source_info->matrix_coefficients.value_or(6) == request.matrix_coefficients &&
       image.source_info->color_range.value_or(1) == request.color_range));
}

namespace pixel_detail {

avifPixelFormat avif_format(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::yuv444: return AVIF_PIXEL_FORMAT_YUV444;
    case PixelFormat::yuv422: return AVIF_PIXEL_FORMAT_YUV422;
    case PixelFormat::yuv420: return AVIF_PIXEL_FORMAT_YUV420;
    case PixelFormat::gray: return AVIF_PIXEL_FORMAT_YUV400;
    default: return AVIF_PIXEL_FORMAT_NONE;
  }
}

std::array<std::size_t, 2> subsampling(PixelFormat format) noexcept {
  return {format == PixelFormat::yuv420 || format == PixelFormat::yuv422 ? 2u : 1u,
          format == PixelFormat::yuv420 ? 2u : 1u};
}

std::array<std::size_t, 2> plane_size(const ImageBuffer& image, std::size_t channel) noexcept {
  const auto storage = image_storage_dimensions(image);
  if (channel == 0 || channel == 3 || !is_yuv_format(image.pixel_format))
    return storage;
  const auto factor = subsampling(image.pixel_format);
  return {(storage[0] + factor[0] - 1) / factor[0],
          (storage[1] + factor[1] - 1) / factor[1]};
}

std::uint64_t destination_pixel_bytes(std::size_t width, std::size_t height,
    const PixelConversionRequest& request, AlphaMode source_alpha) noexcept {
  const auto pixels = hdr_cost::multiply(width, height);
  const auto sample_bytes = request.bit_depth > 8 ? 2u : 1u;
  if (request.format == PixelFormat::rgba) return hdr_cost::multiply(pixels, 4 * sample_bytes);
  if (!is_yuv_format(request.format)) return UINT64_MAX;
  auto samples = pixels;
  if (request.format != PixelFormat::gray) {
    const auto factor = subsampling(request.format);
    // Overflow-safe ceil division, including odd dimensions.
    const auto chroma_width = width / factor[0] + (width % factor[0] != 0);
    const auto chroma_height = height / factor[1] + (height % factor[1] != 0);
    samples = hdr_cost::add(samples, hdr_cost::multiply(hdr_cost::multiply(chroma_width, chroma_height), 2));
  }
  if (request.preserve_alpha && source_alpha != AlphaMode::none) samples = hdr_cost::add(samples, pixels);
  return hdr_cost::multiply(samples, sample_bytes);
}

std::expected<void, std::string> validate(const ImageBuffer& image) {
  if (image.width == 0 || image.height == 0 || image.planes.empty() ||
      image.width > UINT32_MAX || image.height > UINT32_MAX || !valid_image_geometry(image) ||
      (image.bit_depth != 8 && image.bit_depth != 10 && image.bit_depth != 12 && image.bit_depth != 16))
    return std::unexpected{"像素转换输入尺寸或位深无效。"};
  if (image.sample_representation == SampleRepresentation::ieee_half_float &&
      (image.pixel_format != PixelFormat::rgba || image.bit_depth != 16))
    return std::unexpected{"浮点像素转换需要 16-bit RGBA。"};
  const bool yuv = is_yuv_format(image.pixel_format);
  if (!yuv && image.pixel_format != PixelFormat::rgba && image.pixel_format != PixelFormat::rgb)
    return std::unexpected{"像素转换输入布局不受支持。"};
  const auto channels = image.pixel_format == PixelFormat::rgb ? 3u : 4u;
  const auto count = yuv ? (image.alpha_mode == AlphaMode::none ? 3u : 4u) : 1u;
  if (image.planes.size() < count) return std::unexpected{"像素转换输入缺少平面。"};
  for (std::size_t c = 0; c < count; ++c) {
    if (image.pixel_format == PixelFormat::gray && (c == 1 || c == 2)) continue;
    const auto size = plane_size(image, c);
    const auto samples = yuv ? 1u : channels;
    const auto bytes = image.bit_depth > 8 ? 2u : 1u;
    if (size[0] > std::numeric_limits<std::size_t>::max() / samples / bytes)
      return std::unexpected{"像素转换平面宽度溢出。"};
    const auto& plane = image.planes[c];
    if (!plane.contains_rows(size[1], size[0] * samples * bytes))
      return std::unexpected{"像素转换输入平面尺寸无效。"};
  }
  return {};
}

float sample(const ImageBuffer& image, std::size_t channel, std::size_t x, std::size_t y) noexcept {
  const auto* row = image.planes[channel].read_bytes().data() + y * image.planes[channel].stride;
  if (image.bit_depth == 8) return static_cast<float>(std::to_integer<unsigned char>(row[x]));
  std::uint16_t value{};
  std::memcpy(&value, row + x * 2, 2);
  return static_cast<float>(value);
}

float chroma(const ImageBuffer& image, std::size_t channel, std::size_t x, std::size_t y) noexcept {
  const auto factors = subsampling(image.pixel_format);
  const auto size = plane_size(image, channel);
  const int location = image.source_info ? image.source_info->chroma_location.value_or(2) : 2;
  const bool left = location == 1 || location == 3 || location == 5;
  const float center_x = factors[0] == 1 || left ? 0.0F : 0.5F;
  const float center_y = factors[1] == 1 || location == 3 || location == 4 ? 0.0F
      : location == 5 || location == 6 ? 1.0F : 0.5F;
  const float fx = (static_cast<float>(x) - center_x) / static_cast<float>(factors[0]);
  const float fy = (static_cast<float>(y) - center_y) / static_cast<float>(factors[1]);
  const auto x0 = static_cast<std::ptrdiff_t>(std::floor(fx));
  const auto y0 = static_cast<std::ptrdiff_t>(std::floor(fy));
  const auto at = [&](std::ptrdiff_t px, std::ptrdiff_t py) {
    return sample(image, channel,
        static_cast<std::size_t>(std::clamp(px, std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(size[0] - 1))),
        static_cast<std::size_t>(std::clamp(py, std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(size[1] - 1))));
  };
  const float dx = fx - static_cast<float>(x0), dy = fy - static_cast<float>(y0);
  return std::lerp(std::lerp(at(x0, y0), at(x0 + 1, y0), dx),
                   std::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), dx), dy);
}

avifImage description(const ImageBuffer& image) noexcept {
  avifImage result{};
  result.width = static_cast<std::uint32_t>(image.width);
  result.height = static_cast<std::uint32_t>(image.height);
  result.depth = static_cast<std::uint32_t>(image.bit_depth);
  result.yuvFormat = avif_format(image.pixel_format);
  result.yuvRange = image.source_info && image.source_info->color_range == 0 ? AVIF_RANGE_LIMITED : AVIF_RANGE_FULL;
  result.colorPrimaries = static_cast<avifColorPrimaries>(image.source_info ? image.source_info->color_primaries.value_or(2) : 2);
  result.matrixCoefficients = static_cast<avifMatrixCoefficients>(image.source_info ? image.source_info->matrix_coefficients.value_or(6) : 6);
  return result;
}

struct Reader {
  const ImageBuffer& image;
  avifYUVColorSpaceInfo color{};

  std::array<float, 4> operator()(std::size_t x, std::size_t y) const noexcept {
    const auto coordinate = image_sample_coordinates(image, x, y);
    x = coordinate[0]; y = coordinate[1];
    std::array<float, 4> rgba{0, 0, 0, 1};
    const float maximum = static_cast<float>((1u << image.bit_depth) - 1);
    if (!is_yuv_format(image.pixel_format)) {
      const auto channels = image.pixel_format == PixelFormat::rgb ? 3u : 4u;
      const auto* row = image.planes[0].read_bytes().data() + y * image.planes[0].stride;
      for (std::size_t c = 0; c < channels; ++c) {
        if (image.sample_representation == SampleRepresentation::ieee_half_float)
          rgba[c] = hdr::hdr_detail::read_sample(image, row, x * channels + c);
        else rgba[c] = sample(image, 0, x * channels + c, y) / maximum;
      }
    } else {
      const float luma = (sample(image, 0, x, y) - color.biasY) / color.rangeY;
      if (image.pixel_format == PixelFormat::gray) rgba[0] = rgba[1] = rgba[2] = luma;
      else if (image.source_info && image.source_info->matrix_coefficients == 0) {
        rgba[0] = chroma(image, 2, x, y) / maximum;
        rgba[1] = luma;
        rgba[2] = chroma(image, 1, x, y) / maximum;
      } else {
        const float u = (chroma(image, 1, x, y) - color.biasUV) / color.rangeUV;
        const float v = (chroma(image, 2, x, y) - color.biasUV) / color.rangeUV;
        if (image.source_info && image.source_info->matrix_coefficients == 8) {
          rgba[0] = luma - u + v; rgba[1] = luma + u; rgba[2] = luma - u - v;
        } else {
          rgba[0] = luma + 2 * (1 - color.kr) * v;
          rgba[2] = luma + 2 * (1 - color.kb) * u;
          rgba[1] = (luma - color.kr * rgba[0] - color.kb * rgba[2]) / color.kg;
        }
      }
      if (image.alpha_mode != AlphaMode::none) rgba[3] = sample(image, 3, x, y) / maximum;
    }
    if (image.alpha_mode == AlphaMode::premultiplied) {
      // Do not hide a non-finite source component behind transparent alpha.
      for (const auto value : rgba) if (!std::isfinite(value)) return rgba;
      for (std::size_t c = 0; c < 3; ++c) rgba[c] = rgba[3] > 0 ? rgba[c] / rgba[3] : 0;
    }
    return rgba;
  }
};

// 仅在要求真实颜色转换或物化FP16时启用；普通布局／位深转换不改变传递函数。
struct HdrMap {
  hdr::hdr_detail::SourceTransform source{};
  pl_matrix3x3 matrix{};
  pl_color_space output_space{};
  float (*to_linear)(float){};
  float (*from_linear)(float){};
  int primaries{9};
  int transfer{16};
  bool enabled{};
  bool source_hdr{};
  bool target_hdr{};

  std::expected<std::array<float, 4>, std::string> operator()(std::array<float, 4> rgba) const {
    for (float value : rgba) if (!std::isfinite(value)) return std::unexpected{"像素输入含非有限值。"};
    if (!enabled) return rgba;
    std::array<float, 3> rgb{rgba[0], rgba[1], rgba[2]};
    if (source_hdr) {
      auto nits = hdr::hdr_detail::linear_nits(rgb, source);
      if (!nits) return std::unexpected{nits.error()};
      rgb = *nits;
    } else for (auto& value : rgb) value = to_linear(value) * 203.0F;
    pl_matrix3x3_apply(&matrix, rgb.data());
    if (!hdr::hdr_detail::finite_rgb(rgb)) return std::unexpected{"颜色转换矩阵产生了非有限值。"};
    if (target_hdr && transfer == 18) {
      for (auto& value : rgb) value = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_NORM, std::max(value, 0.0F));
      // 锁定 libplacebo 的逆 HLG OOTF 在零亮度会产生 inf*0；使用黑点的连续极限。
      if (rgb[0] == 0.0F && rgb[1] == 0.0F && rgb[2] == 0.0F) rgb.fill(0.0F);
      else pl_color_delinearize(&output_space, rgb.data());
    } else for (auto& value : rgb) value = target_hdr
        ? pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, value) : from_linear(value / 203.0F);
    if (!hdr::hdr_detail::finite_rgb(rgb)) return std::unexpected{"目标传递函数产生了非有限值。"};
    std::copy(rgb.begin(), rgb.end(), rgba.begin());
    return rgba;
  }
};

std::expected<HdrMap, std::string> hdr_map(const Reader& reader,
    const PixelConversionRequest& request, std::stop_token stop) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  HdrMap result{};
  const bool floating = reader.image.sample_representation == SampleRepresentation::ieee_half_float;
  const auto info = reader.image.source_info;
  const bool changed = (request.color_primaries && (!info || info->color_primaries != request.color_primaries)) ||
      (request.transfer_characteristics && (!info || info->transfer_characteristics != request.transfer_characteristics));
  if (!floating && !changed) return result;
  auto signal = hdr::has_explicit_hdr_signal(reader.image);
  if (!signal) return std::unexpected{signal.error()};
  result.source_hdr = *signal;
  const pl_raw_primaries* input = nullptr;
  if (*signal) {
    auto source = hdr::hdr_detail::make_source_transform(reader.image);
    if (!source) return std::unexpected{source.error()};
    result.source = *source;
    input = source->primaries;
  } else {
    if (!info || !info->color_primaries || !info->transfer_characteristics)
      return std::unexpected{"颜色转换缺少明确的源原色／传递函数，拒绝只重标标签。"};
    const int tc = *info->transfer_characteristics;
    if (!(tc == 1 || (tc >= 4 && tc <= 15) || tc == 17))
      return std::unexpected{"颜色转换不支持该 SDR 传递函数。"};
    result.to_linear = avifTransferCharacteristicsGetGammaToLinearFunction(static_cast<avifTransferCharacteristics>(tc));
    input = hdr::hdr_detail::primaries_from_cicp(*info->color_primaries);
  }
  result.primaries = request.color_primaries.value_or(floating ? 9 : info->color_primaries.value_or(0));
  result.transfer = request.transfer_characteristics.value_or(floating ? 16 : info->transfer_characteristics.value_or(0));
  const auto* output = hdr::hdr_detail::primaries_from_cicp(result.primaries);
  if (!input || !output) return std::unexpected{"颜色转换不支持该源／目标原色。"};
  result.target_hdr = result.transfer == 16 || result.transfer == 18;
  if (!result.target_hdr && *signal) return std::unexpected{"HDR 转 SDR 需要明确的 tone-map，不能仅覆盖传递函数。"};
  if (!result.target_hdr) {
    if (!(result.transfer == 1 || (result.transfer >= 4 && result.transfer <= 15) || result.transfer == 17))
      return std::unexpected{"颜色转换不支持该目标传递函数。"};
    result.from_linear = avifTransferCharacteristicsGetLinearToGammaFunction(static_cast<avifTransferCharacteristics>(result.transfer));
  }
  result.output_space.primaries = result.primaries == 9 ? PL_COLOR_PRIM_BT_2020
      : result.primaries == 12 ? PL_COLOR_PRIM_DISPLAY_P3 : PL_COLOR_PRIM_BT_709;
  result.output_space.transfer = PL_COLOR_TRC_HLG;
  result.output_space.hdr.max_luma = PL_COLOR_HLG_PEAK;
  result.enabled = true;
  result.matrix = pl_raw_primaries_equal(input, output)
      ? pl_matrix3x3{.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}}
      : pl_get_color_mapping_matrix(input, output, PL_INTENT_RELATIVE_COLORIMETRIC);
  if (!hdr::hdr_detail::finite_matrix(result.matrix)) return std::unexpected{"颜色矩阵包含非有限值。"};
  return result;
}

void write_sample(ImageBuffer& output, std::size_t c, std::size_t x, std::size_t y, float value) noexcept {
  auto* row = output.planes[c].bytes.data() + y * output.planes[c].stride;
  const float maximum = static_cast<float>((1u << output.bit_depth) - 1);
  const auto encoded = static_cast<std::uint16_t>(std::clamp(std::floor(value + 0.5F), 0.0F, maximum));
  if (output.bit_depth == 8) row[x] = std::byte{static_cast<unsigned char>(encoded)};
  else std::memcpy(row + x * 2, &encoded, 2);
}

} // namespace pixel_detail

std::expected<void, std::string> validate_image_pixels(const ImageBuffer& image) {
  return pixel_detail::validate(image);
}

std::expected<ImageBuffer, std::string> resize_native_pixels(
    const ImageBuffer& source, std::size_t width, std::size_t height, std::stop_token stop = {}) {
  if (auto valid = pixel_detail::validate(source); !valid) return std::unexpected{valid.error()};
  if (!width || !height || width > UINT32_MAX || height > UINT32_MAX) return std::unexpected{"调整尺寸无效。"};
  if (source.geometry && is_yuv_format(source.pixel_format))
    return std::unexpected{"YUV 几何变换应在目标像素转换中处理。"};
  try {
    ImageBuffer output{.width = width, .height = height, .pixel_format = source.pixel_format,
        .alpha_mode = source.alpha_mode, .bit_depth = source.bit_depth,
        .sample_representation = source.sample_representation, .source_info = source.source_info,
        .significant_bits = source.significant_bits, .metadata = source.metadata};
    output.planes.resize(source.planes.size());
    for (std::size_t c = 0; c < output.planes.size(); ++c) {
      if (source.planes[c].read_bytes().empty()) continue;
      const auto input_size = pixel_detail::plane_size(source, c);
      const auto output_size = pixel_detail::plane_size(output, c);
      const auto sample_bytes = (source.bit_depth > 8 ? 2u : 1u) *
          (is_yuv_format(source.pixel_format) ? 1u : source.pixel_format == PixelFormat::rgb ? 3u : 4u);
      if (output_size[0] > SIZE_MAX / sample_bytes) return std::unexpected{"调整尺寸宽度溢出。"};
      const auto stride = output_size[0] * sample_bytes;
      auto count = decoder_common::checked_image_bytes(stride, output_size[1], "native resize");
      if (!count) return std::unexpected{count.error()};
      auto bytes = decoder_common::make_byte_buffer(*count, "native resize");
      if (!bytes) return std::unexpected{bytes.error()};
      for (std::size_t y = 0; y < output_size[1]; ++y) {
        if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
        const auto sy = std::min(input_size[1] - 1, y * input_size[1] / output_size[1]);
        for (std::size_t x = 0; x < output_size[0]; ++x) {
          const auto sx = std::min(input_size[0] - 1, x * input_size[0] / output_size[0]);
          const auto point = source.geometry ? image_sample_coordinates(source,
              x * source.width / width, y * source.height / height) : std::array{sx, sy};
          std::memcpy(bytes->data() + y * stride + x * sample_bytes,
              source.planes[c].read_bytes().data() + point[1] * source.planes[c].stride + point[0] * sample_bytes, sample_bytes);
        }
      }
      output.planes[c] = ImagePlane{.bytes = std::move(*bytes), .stride = stride};
    }
    return output;
  } catch (const std::bad_alloc&) { return std::unexpected{"调整尺寸内存不足。"}; }
    catch (const std::length_error&) { return std::unexpected{"调整尺寸超过限制。"}; }
}

std::expected<avifImage, std::string> yuv_plane_view(const ImageBuffer& image) {
  if (image.geometry) return std::unexpected{"原生 YUV 视图需要先应用几何变换。"};
  if (!is_yuv_format(image.pixel_format)) return std::unexpected{"需要原生 YUV 平面。"};
  if (auto valid = pixel_detail::validate(image); !valid) return std::unexpected{valid.error()};
  if (image.width > UINT32_MAX || image.height > UINT32_MAX) return std::unexpected{"YUV 尺寸超过 API 限制。"};
  auto view = pixel_detail::description(image);
  for (std::size_t c = 0; c < image.planes.size() && c < 4; ++c) {
    if (image.planes[c].stride > UINT32_MAX) return std::unexpected{"YUV stride 超过 API 限制。"};
    auto* data = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(image.planes[c].read_bytes().data()));
    if (c == 3) { view.alphaPlane = data; view.alphaRowBytes = static_cast<std::uint32_t>(image.planes[c].stride); }
    else { view.yuvPlanes[c] = image.planes[c].read_bytes().empty() ? nullptr : data; view.yuvRowBytes[c] = static_cast<std::uint32_t>(image.planes[c].stride); }
  }
  view.alphaPremultiplied = image.alpha_mode == AlphaMode::premultiplied ? AVIF_TRUE : AVIF_FALSE;
  return view;
}

std::expected<ImageBuffer, std::string> copy_yuv_pixels(const avifImage& source, std::stop_token stop = {}) {
  try {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    PixelFormat format = source.yuvFormat == AVIF_PIXEL_FORMAT_YUV444 ? PixelFormat::yuv444
        : source.yuvFormat == AVIF_PIXEL_FORMAT_YUV422 ? PixelFormat::yuv422
        : source.yuvFormat == AVIF_PIXEL_FORMAT_YUV420 ? PixelFormat::yuv420 : PixelFormat::gray;
    ImageBuffer output{.width = source.width, .height = source.height, .pixel_format = format,
        .alpha_mode = source.alphaPlane ? (source.alphaPremultiplied ? AlphaMode::premultiplied : AlphaMode::straight) : AlphaMode::none,
        .bit_depth = static_cast<int>(source.depth), .source_info = ImageSourceInfo{
            .pixel_format = format, .bit_depth = static_cast<int>(source.depth),
            .color_primaries = static_cast<int>(source.colorPrimaries),
            .transfer_characteristics = static_cast<int>(source.transferCharacteristics),
            .matrix_coefficients = static_cast<int>(source.matrixCoefficients), .color_range = source.yuvRange == AVIF_RANGE_FULL ? 1 : 0}};
    output.planes.resize(source.alphaPlane ? 4 : 3);
    for (std::size_t c = 0; c < output.planes.size(); ++c) {
      if (format == PixelFormat::gray && (c == 1 || c == 2)) continue;
      const auto size = pixel_detail::plane_size(output, c);
      const auto stride = size[0] * (source.depth > 8 ? 2u : 1u);
      const auto* data = c == 3 ? source.alphaPlane : source.yuvPlanes[c];
      const auto source_stride = c == 3 ? source.alphaRowBytes : source.yuvRowBytes[c];
      if (!data || source_stride < stride) return std::unexpected{"解码器 YUV 平面无效。"};
      auto bytes = decoder_common::checked_image_bytes(stride, size[1], "native YUV");
      if (!bytes) return std::unexpected{bytes.error()};
      auto storage = decoder_common::make_byte_buffer(*bytes, "native YUV");
      if (!storage) return std::unexpected{storage.error()};
      for (std::size_t y = 0; y < size[1]; ++y) {
        if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
        std::memcpy(storage->data() + y * stride, data + y * source_stride, stride);
      }
      output.planes[c] = ImagePlane{.bytes = std::move(*storage), .stride = stride};
    }
    if (auto valid = pixel_detail::validate(output); !valid) return std::unexpected{valid.error()};
    return output;
  } catch (const std::bad_alloc&) { return std::unexpected{"YUV 平面内存不足。"}; }
    catch (const std::length_error&) { return std::unexpected{"YUV 平面尺寸超过限制。"}; }
}

std::expected<ImageBuffer, std::string> convert_image_pixels(
    const ImageBuffer& source, const PixelConversionRequest& request, std::stop_token stop = {},
    const ResourcePlan& resources = {}) {
  try {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    if (auto valid = pixel_detail::validate(source); !valid) return std::unexpected{valid.error()};
    if ((request.bit_depth != 8 && request.bit_depth != 10 && request.bit_depth != 12 && request.bit_depth != 16) ||
        (!is_yuv_format(request.format) && request.format != PixelFormat::rgba) ||
        (request.color_range != 0 && request.color_range != 1)) return std::unexpected{"目标像素布局无效。"};
    pixel_detail::Reader reader{source};
    if (is_yuv_format(source.pixel_format)) {
      const auto view = pixel_detail::description(source);
      if (!avifGetYUVColorSpaceInfo(&view, &reader.color)) return std::unexpected{"源 YUV 色彩矩阵不受支持。"};
    }
    if (request.tone_map_to_sdr) {
      auto signal = hdr::has_explicit_hdr_signal(source);
      if (!signal) return std::unexpected{signal.error()};
      if (*signal) {
        if (reader.color.depth && source.source_info &&
            (source.source_info->matrix_coefficients == AVIF_MATRIX_COEFFICIENTS_YCGCO_RE ||
             source.source_info->matrix_coefficients == AVIF_MATRIX_COEFFICIENTS_YCGCO_RO)) {
          return std::unexpected{"可逆 YCgCo 的 HDR 色调映射需要相匹配的整数转换。"};
        }
        // Native YUV destinations retain a 16-bit SDR RGB intermediate; do not
        // quantize/dither RGB to 8-bit before the final YUV transform.
        const int depth = request.format == PixelFormat::rgba && request.bit_depth == 8 ? 8 : 16;
        auto tone_resources = resources;
        if (request.format != PixelFormat::rgba || request.bit_depth != depth) {
          // The recursive call cannot release its caller's original source.
          // Reserve the final destination (including its metadata copy) before
          // tone chooses its LUT/workers from the remaining finite budget.
          auto target = pixel_detail::destination_pixel_bytes(source.width, source.height, request, source.alpha_mode);
          auto resident = hdr_cost::multiply(hdr_cost::multiply(source.width, source.height), 4 * (depth / 8));
          for (const auto& plane : source.planes) resident = hdr_cost::add(resident, plane.read_bytes().size());
          for (const auto& block : source.metadata) {
            resident = hdr_cost::add(resident, hdr_cost::multiply(block.bytes.size(), 2));
            if (block.kind == MetadataKind::exif || block.kind == MetadataKind::xmp)
              target = hdr_cost::add(target, block.bytes.size());
          }
          // At least one scalar IPT slot plus control state must fit. Larger
          // tone scratch is bounded by the reduced plan and dies before the
          // destination conversion, whose 16-bit RGB path uses a fixed 2x2 block.
          const auto minimum_scratch = hdr_cost::tone_scratch_bytes(1, false, 1);
          const auto required = hdr_cost::add(resident, hdr_cost::add(target, minimum_scratch));
          if (required == UINT64_MAX) return std::unexpected{"HDR 递归转换工作集尺寸溢出。"};
          if (resources.memory_limit_bytes) {
            if (required > resources.memory_limit_bytes || target >= resources.memory_limit_bytes)
              return std::unexpected{"HDR 递归转换的 source、SDR、目标及 scratch 同时工作集超出内存预算。"};
            // A finite exhausted remainder must never be passed as zero:
            // ResourcePlan's zero means unlimited, not no available capacity.
            tone_resources.memory_limit_bytes = resources.memory_limit_bytes - target;
          }
        }
        // Reader applies geometry and unpremultiplication once, in the source
        // signal space. The borrowed callback remains alive until tone workers
        // have joined; negative scRGB never passes through a clipped HDR10 copy.
        const hdr::hdr_detail::RgbaPixelReader pixels{.context = &reader,
            .read = [](const void* context, std::size_t x, std::size_t y) noexcept {
              return (*static_cast<const pixel_detail::Reader*>(context))(x, y);
            }};
        auto prepared = hdr::hdr_detail::prepare_tone_context(source, stop, pixels);
        if (!prepared) return std::unexpected{prepared.error()};
        auto mapped = hdr::tone_map_to_sdr_srgb(source, *prepared, depth, stop, tone_resources);
        if (!mapped) return std::unexpected{mapped.error()};
        const bool remaining_color_conversion =
            (request.color_primaries && mapped->source_info->color_primaries != request.color_primaries) ||
            (request.transfer_characteristics && mapped->source_info->transfer_characteristics != request.transfer_characteristics);
        if (request.format == PixelFormat::rgba && request.bit_depth == depth && !remaining_color_conversion) {
          if (!request.preserve_alpha) {
            mapped->alpha_mode = AlphaMode::none;
            for (std::size_t y = 0; y < mapped->height; ++y) {
              if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
              auto* row = mapped->planes[0].bytes.data() + y * mapped->planes[0].stride;
              for (std::size_t x = 0; x < mapped->width; ++x) {
                if (x % 1024 == 0 && stop.stop_requested()) return std::unexpected{"任务已取消。"};
                if (depth == 8) row[x * 4 + 3] = std::byte{255};
                else hdr::hdr_detail::write_unorm16(row, x * 4 + 3, 1.0F);
              }
            }
          }
          mapped->source_info->color_range = request.color_range;
          return mapped;
        }
        auto destination = request;
        destination.tone_map_to_sdr = false;
        return convert_image_pixels(*mapped, destination, stop, resources);
      }
    }
    auto mapping = pixel_detail::hdr_map(reader, request, stop);
    if (!mapping) return std::unexpected{mapping.error()};
    ImageBuffer output{.width = source.width, .height = source.height, .pixel_format = request.format,
        .alpha_mode = request.preserve_alpha && source.alpha_mode != AlphaMode::none ? AlphaMode::straight : AlphaMode::none,
        .bit_depth = request.bit_depth, .source_info = source.source_info, .metadata = source.metadata};
    if (!output.source_info) output.source_info = ImageSourceInfo{};
    output.source_info->matrix_coefficients = is_yuv_format(request.format) ? request.matrix_coefficients : 0;
    output.source_info->color_range = request.color_range;
    if (mapping->enabled) {
      output.source_info->color_primaries = mapping->primaries;
      output.source_info->transfer_characteristics = mapping->transfer;
      output.source_info->has_hdr_metadata = mapping->target_hdr;
      output.source_info->content_light.reset();
      output.source_info->color_metadata_source = source.sample_representation == SampleRepresentation::ieee_half_float
          ? "scrgb-linear-to-bt2020-pq" : "converted-cicp-pixels";
      std::erase_if(output.metadata, [](const MetadataBlock& block) { return block.kind == MetadataKind::icc; });
    }
    const bool yuv = is_yuv_format(request.format);
    output.planes.resize(yuv ? (output.alpha_mode == AlphaMode::none ? 3u : 4u) : 1u);
    std::uint64_t allocation{};
    for (std::size_t c = 0; c < output.planes.size(); ++c) {
      if (request.format == PixelFormat::gray && (c == 1 || c == 2)) continue;
      const auto size = pixel_detail::plane_size(output, c);
      const auto channels = yuv ? 1u : 4u;
      if (size[0] > SIZE_MAX / channels / (request.bit_depth > 8 ? 2u : 1u)) return std::unexpected{"目标像素宽度溢出。"};
      const auto stride = size[0] * channels * (request.bit_depth > 8 ? 2u : 1u);
      auto count = decoder_common::checked_image_bytes(stride, size[1], "pixel conversion");
      if (!count) return std::unexpected{count.error()};
      if (*count > encoding_defaults::effective_max_input_file_bytes() - allocation) return std::unexpected{"目标像素超过内存上限。"};
      allocation += *count;
      auto bytes = decoder_common::make_byte_buffer(*count, "pixel conversion");
      if (!bytes) return std::unexpected{bytes.error()};
      output.planes[c] = ImagePlane{.bytes = std::move(*bytes), .stride = stride};
    }
    const float maximum = static_cast<float>((1u << request.bit_depth) - 1);
    avifYUVColorSpaceInfo target{};
    if (yuv) {
      const auto view = pixel_detail::description(output);
      if (!avifGetYUVColorSpaceInfo(&view, &target)) return std::unexpected{"目标 YUV 色彩矩阵不受支持。"};
    }
    if (!mapping->enabled && !source.geometry && source.pixel_format == PixelFormat::rgba && yuv &&
        source.sample_representation == SampleRepresentation::unorm) {
      auto view = yuv_plane_view(output);
      if (!view) return std::unexpected{view.error()};
      avifRGBImage rgb{};
      avifRGBImageSetDefaults(&rgb, &*view);
      rgb.format = AVIF_RGB_FORMAT_RGBA; rgb.depth = source.bit_depth;
      rgb.pixels = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(source.planes[0].read_bytes().data()));
      if (source.planes[0].stride > UINT32_MAX) return std::unexpected{"RGBA stride 超过 API 限制。"};
      rgb.rowBytes = static_cast<std::uint32_t>(source.planes[0].stride);
      rgb.alphaPremultiplied = source.alpha_mode == AlphaMode::premultiplied ? AVIF_TRUE : AVIF_FALSE;
      rgb.ignoreAlpha = output.alpha_mode == AlphaMode::none ? AVIF_TRUE : AVIF_FALSE;
      rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_AVERAGE;
      const auto converted = avifImageRGBToYUV(&*view, &rgb);
      if (converted != AVIF_RESULT_OK) return std::unexpected{std::string{"RGB 转 YUV 失败: "} + avifResultToString(converted)};
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      return output;
    }
    if (!mapping->enabled && !source.geometry && is_yuv_format(source.pixel_format) && !yuv &&
        (!source.source_info || !source.source_info->chroma_location || source.source_info->chroma_location == 2)) {
      auto view = yuv_plane_view(source);
      if (!view) return std::unexpected{view.error()};
      avifRGBImage rgb{};
      avifRGBImageSetDefaults(&rgb, &*view);
      rgb.format = AVIF_RGB_FORMAT_RGBA; rgb.depth = request.bit_depth;
      rgb.pixels = reinterpret_cast<std::uint8_t*>(output.planes[0].bytes.data());
      if (output.planes[0].stride > UINT32_MAX) return std::unexpected{"RGBA stride 超过 API 限制。"};
      rgb.rowBytes = static_cast<std::uint32_t>(output.planes[0].stride);
      rgb.alphaPremultiplied = AVIF_FALSE;
      rgb.ignoreAlpha = output.alpha_mode == AlphaMode::none ? AVIF_TRUE : AVIF_FALSE;
      const auto converted = avifImageYUVToRGB(&*view, &rgb);
      if (converted != AVIF_RESULT_OK) return std::unexpected{std::string{"YUV 转 RGB 失败: "} + avifResultToString(converted)};
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      return output;
    }
    if (!mapping->enabled && !source.geometry && is_yuv_format(source.pixel_format) && yuv &&
        source.source_info && source.source_info->matrix_coefficients == request.matrix_coefficients &&
        source.alpha_mode != AlphaMode::premultiplied) {
      const auto factors = pixel_detail::subsampling(output.pixel_format);
      for (std::size_t c = 0; c < output.planes.size(); ++c) {
        if (output.pixel_format == PixelFormat::gray && (c == 1 || c == 2)) continue;
        const auto size = pixel_detail::plane_size(output, c);
        for (std::size_t y = 0; y < size[1]; ++y) {
          if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
          for (std::size_t x = 0; x < size[0]; ++x) {
            float value{};
            if (c == 3) value = pixel_detail::sample(source, 3, x, y) /
                static_cast<float>((1u << source.bit_depth) - 1) * maximum;
            else if (c == 0) value = (pixel_detail::sample(source, 0, x, y) - reader.color.biasY) /
                reader.color.rangeY * target.rangeY + target.biasY;
            else {
              if (source.pixel_format == PixelFormat::gray) value = reader.color.biasUV;
              else if (source.pixel_format == output.pixel_format) value = pixel_detail::sample(source, c, x, y);
              else {
                std::size_t count{};
                for (std::size_t py = y * factors[1]; py < std::min((y + 1) * factors[1], source.height); ++py)
                  for (std::size_t px = x * factors[0]; px < std::min((x + 1) * factors[0], source.width); ++px) {
                    value += pixel_detail::chroma(source, c, px, py); ++count;
                  }
                value /= static_cast<float>(count);
              }
              const bool identity = request.matrix_coefficients == 0;
              value = (value - (identity ? 0 : reader.color.biasUV)) / reader.color.rangeUV *
                  target.rangeUV + (identity ? 0 : target.biasUV);
            }
            pixel_detail::write_sample(output, c, x, y, value);
          }
        }
      }
      return output;
    }
    if (reader.color.depth && source.source_info &&
        (source.source_info->matrix_coefficients == AVIF_MATRIX_COEFFICIENTS_YCGCO_RE ||
         source.source_info->matrix_coefficients == AVIF_MATRIX_COEFFICIENTS_YCGCO_RO))
      return std::unexpected{"可逆 YCgCo 的矩阵转换需要相匹配的整数位深。"};
    if (request.matrix_coefficients == AVIF_MATRIX_COEFFICIENTS_YCGCO_RE ||
        request.matrix_coefficients == AVIF_MATRIX_COEFFICIENTS_YCGCO_RO)
      return std::unexpected{"可逆 YCgCo 的矩阵转换需要相匹配的整数位深。"};
    if (!mapping->enabled && source.pixel_format == PixelFormat::rgba && request.format == PixelFormat::rgba &&
        source.bit_depth == request.bit_depth && source.alpha_mode != AlphaMode::premultiplied)
      output.significant_bits = source.significant_bits;
    const auto factors = yuv ? pixel_detail::subsampling(request.format) : std::array<std::size_t, 2>{1, 1};
    for (std::size_t by = 0; by < source.height; by += factors[1]) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      for (std::size_t bx = 0; bx < source.width; bx += factors[0]) {
        float sum_u{}, sum_v{}; std::size_t count{};
        for (std::size_t y = by; y < std::min(by + factors[1], source.height); ++y) {
          for (std::size_t x = bx; x < std::min(bx + factors[0], source.width); ++x) {
            auto rgba = (*mapping)(reader(x, y));
            if (!rgba) return std::unexpected{rgba.error()};
            if (!yuv) {
              for (std::size_t c = 0; c < 4; ++c) {
                const float value = c == 3 && output.alpha_mode == AlphaMode::none ? 1 : (*rgba)[c];
                pixel_detail::write_sample(output, 0, x * 4 + c, y, value * maximum);
              }
              continue;
            }
            const float r = std::clamp((*rgba)[0], 0.0F, 1.0F), g = std::clamp((*rgba)[1], 0.0F, 1.0F), b = std::clamp((*rgba)[2], 0.0F, 1.0F);
            float luma{}, u{}, v{};
            if (request.matrix_coefficients == 0) { luma = g; u = b; v = r; }
            else if (request.matrix_coefficients == 8) { luma = (r + 2 * g + b) / 4; u = (-r + 2 * g - b) / 4; v = (r - b) / 2; }
            else { luma = target.kr * r + target.kg * g + target.kb * b; u = (b - luma) / (2 * (1 - target.kb)); v = (r - luma) / (2 * (1 - target.kr)); }
            pixel_detail::write_sample(output, 0, x, y, luma * target.rangeY + target.biasY);
            sum_u += u; sum_v += v; ++count;
            if (output.alpha_mode != AlphaMode::none) pixel_detail::write_sample(output, 3, x, y, (*rgba)[3] * maximum);
          }
        }
        if (yuv && request.format != PixelFormat::gray) {
          const float bias = request.matrix_coefficients == 0 ? 0 : target.biasUV;
          pixel_detail::write_sample(output, 1, bx / factors[0], by / factors[1], sum_u / count * target.rangeUV + bias);
          pixel_detail::write_sample(output, 2, bx / factors[0], by / factors[1], sum_v / count * target.rangeUV + bias);
        }
      }
    }
    return output;
  } catch (const std::bad_alloc&) { return std::unexpected{"像素转换内存不足。"}; }
    catch (const std::length_error&) { return std::unexpected{"像素转换尺寸超过限制。"}; }
}

} // namespace awj
