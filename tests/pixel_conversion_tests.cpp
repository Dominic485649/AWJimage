#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include <avif/avif.h>
#include <avif/internal.h>
#include "codec_color_fixture.h"
#include "spline36_legacy_reference.h"

import awj.image;
import awj.pixel_conversion;
import awj.hdr_tonemap;
import awj.resource_planner;
import awj.codec;
import awj.config;
import awj.avif_aom_codec;
import awj.png_codec;
import awj.webp_codec;
import awj.jxl_codec;
import awj.gain_map;
import awj.heif_codec;
#if AWJ_HAS_JPEGLI
import awj.jpegli_codec;
#endif

namespace {

int fail(const char* message) { std::fprintf(stderr, "%s\n", message); return 1; }

awj::ImageBuffer rgba() {
  awj::ImageBuffer result{.width = 5, .height = 3, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 8};
  awj::ImagePlane plane{.stride = 20};
  for (unsigned i = 0; i < 15; ++i) {
    plane.bytes.push_back(std::byte{static_cast<unsigned char>(i * 13)});
    plane.bytes.push_back(std::byte{static_cast<unsigned char>(255 - i * 11)});
    plane.bytes.push_back(std::byte{static_cast<unsigned char>(i * 7 + 40)});
    plane.bytes.push_back(std::byte{static_cast<unsigned char>(i * 17)});
  }
  result.planes.push_back(std::move(plane));
  return result;
}

bool same_planes(const awj::ImageBuffer& a, const awj::ImageBuffer& b) {
  if (a.planes.size() != b.planes.size()) return false;
  for (std::size_t c = 0; c < a.planes.size(); ++c)
    if (a.planes[c].bytes != b.planes[c].bytes || a.planes[c].stride != b.planes[c].stride) return false;
  return true;
}

awj::ImageBuffer scrgb() {
  awj::ImageBuffer result{.width = 2, .height = 2, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 16,
      .sample_representation = awj::SampleRepresentation::ieee_half_float,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
          .color_primaries = 1, .transfer_characteristics = 8, .matrix_coefficients = 0, .color_range = 1,
          .has_hdr_metadata = true, .color_metadata_source = "wic-scrgb-half-linear"}};
  awj::ImagePlane plane{.stride = 16};
  const std::array<std::uint16_t, 16> values{0x3800, 0xb400, 0x4000, 0x3c00,
      0x3c00, 0x4000, 0x4200, 0x3800, 0x0000, 0x3400, 0x3c00, 0x3c00,
      0x4900, 0x4800, 0x4700, 0x0000};
  plane.bytes.resize(sizeof(values)); std::memcpy(plane.bytes.data(), values.data(), sizeof(values));
  result.planes.push_back(std::move(plane));
  return result;
}

std::unique_ptr<awj::ImageEncoder> encoder(awj::OutputFormat format) {
  switch (format) {
    case awj::OutputFormat::avif: return awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
    case awj::OutputFormat::png: return std::make_unique<awj::PngImageEncoder>();
    case awj::OutputFormat::webp: return std::make_unique<awj::WebPImageEncoder>();
    case awj::OutputFormat::jxl: return std::make_unique<awj::JXLImageEncoder>();
#if AWJ_HAS_JPEGLI
    case awj::OutputFormat::jpgli: return std::make_unique<awj::JpegliImageEncoder>();
#endif
    default: return {};
  }
}

std::unique_ptr<awj::ImageDecoder> decoder(awj::OutputFormat format) {
  switch (format) {
    case awj::OutputFormat::avif: return awj::make_avif_image_decoder(1);
    case awj::OutputFormat::png: return std::make_unique<awj::PngImageDecoder>();
    case awj::OutputFormat::webp: return std::make_unique<awj::WebPImageDecoder>();
    case awj::OutputFormat::jxl: return std::make_unique<awj::JXLImageDecoder>();
#if AWJ_HAS_JPEGLI
    case awj::OutputFormat::jpgli: return std::make_unique<awj::JpegliImageDecoder>();
#endif
    default: return {};
  }
}

float half_reference(std::uint16_t bits) {
  const auto exponent = (bits >> 10) & 31u, fraction = bits & 1023u;
  const auto sign = bits & 0x8000u ? -1.0F : 1.0F;
  return exponent ? sign * std::ldexp(1.0F + fraction / 1024.0F, static_cast<int>(exponent) - 15)
      : sign * std::ldexp(static_cast<float>(fraction), -24);
}

// Geometry fixtures are finite dyadic values, so this independent conversion
// is exact and cannot hide the loss of a negative/extended linear component.
std::uint16_t dyadic_half(float value) {
  const auto sign = std::signbit(value) ? 0x8000u : 0u;
  value = std::abs(value);
  if (value == 0) return static_cast<std::uint16_t>(sign);
  int exponent{};
  const auto mantissa = std::frexp(value, &exponent);
  return static_cast<std::uint16_t>(sign | (static_cast<unsigned>(exponent + 14) << 10) |
      static_cast<unsigned>(std::lround((mantissa * 2.0F - 1.0F) * 1024.0F)));
}

awj::ImageBuffer premultiplied_scrgb() {
  auto result = scrgb();
  result.width = 3; result.height = 2; result.alpha_mode = awj::AlphaMode::premultiplied;
  result.planes[0].stride = 40;
  result.planes[0].bytes.assign(80, std::byte{0xa5});
  constexpr std::array<std::array<float, 4>, 6> values{{
      {0.25F, -0.125F, 1, 0.5F}, {0.5F, 0.25F, 0.125F, 1}, {0, 0, 0, 0},
      {2, 1, 0.5F, 0.5F}, {-0.25F, 0.5F, 0.125F, 0.5F}, {0.0625F, 0.125F, 0.25F, 1}}};
  for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 3; ++x) for (std::size_t c = 0; c < 4; ++c) {
    const auto half = dyadic_half(values[y * 3 + x][c]);
    std::memcpy(result.planes[0].bytes.data() + y * 40 + (x * 4 + c) * 2, &half, 2);
  }
  return result;
}

awj::ImageBuffer reference_straight_geometry(const awj::ImageBuffer& view) {
  awj::ImageBuffer result{.width = view.width, .height = view.height, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 16,
      .sample_representation = awj::SampleRepresentation::ieee_half_float, .source_info = view.source_info};
  result.planes.push_back({std::vector<std::byte>(view.width * view.height * 8), view.width * 8});
  const auto& g = *view.geometry;
  const auto rw = g.rotation % 2 ? g.crop_height : g.crop_width;
  const auto rh = g.rotation % 2 ? g.crop_width : g.crop_height;
  for (std::size_t y = 0; y < view.height; ++y) for (std::size_t x = 0; x < view.width; ++x) {
    auto px = x * rw / view.width, py = y * rh / view.height;
    if (g.mirror_axis == 0) py = rh - 1 - py;
    if (g.mirror_axis == 1) px = rw - 1 - px;
    std::size_t sx{}, sy{};
    switch (g.rotation) {
      case 1: sx = g.crop_width - 1 - py; sy = px; break;
      case 2: sx = g.crop_width - 1 - px; sy = g.crop_height - 1 - py; break;
      case 3: sx = py; sy = g.crop_height - 1 - px; break;
      default: sx = px; sy = py; break;
    }
    std::array<float, 4> rgba{};
    const auto* input = view.planes[0].bytes.data() + (sy + g.crop_y) * view.planes[0].stride + (sx + g.crop_x) * 8;
    for (std::size_t c = 0; c < 4; ++c) {
      std::uint16_t half{}; std::memcpy(&half, input + c * 2, 2); rgba[c] = half_reference(half);
    }
    for (std::size_t c = 0; c < 3; ++c) rgba[c] = rgba[3] > 0 ? rgba[c] / rgba[3] : 0;
    for (std::size_t c = 0; c < 4; ++c) {
      const auto half = dyadic_half(rgba[c]);
      std::memcpy(result.planes[0].bytes.data() + (y * view.width * 4 + x * 4 + c) * 2, &half, 2);
    }
  }
  return result;
}

int check_hdr_native_gray() {
  awj::ImageBuffer source{.width = 2, .height = 3, .pixel_format = awj::PixelFormat::gray,
      .bit_depth = 12, .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::gray,
          .bit_depth = 12, .color_primaries = 9, .transfer_characteristics = 16,
          .matrix_coefficients = 9, .color_range = 1,
          .content_light = awj::HdrContentLightMetadata{1000, 0}, .has_hdr_metadata = true},
      .geometry = awj::ImageGeometry{.stored_width = 3, .stored_height = 2,
          .crop_width = 3, .crop_height = 2, .rotation = 1}};
  source.planes.resize(3);
  source.planes[0].stride = 8; source.planes[0].bytes.resize(16);
  constexpr std::array<std::uint16_t, 6> codes{0, 64, 256, 1024, 2048, 4095};
  for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 3; ++x)
    std::memcpy(source.planes[0].bytes.data() + y * 8 + x * 2, &codes[y * 3 + x], 2);
  auto logical = source; logical.geometry.reset();
  logical.planes[0].stride = 4; logical.planes[0].bytes.resize(12);
  for (std::size_t y = 0; y < 3; ++y) for (std::size_t x = 0; x < 2; ++x) {
    const auto value = codes[x * 3 + 2 - y];
    std::memcpy(logical.planes[0].bytes.data() + y * 4 + x * 2, &value, 2);
  }
  const awj::ResourcePlan resources{.file_parallelism = 1, .encoder_threads_per_file = 2,
      .global_thread_budget = 2, .memory_limit_bytes = 0};
  const awj::PixelConversionRequest request{.bit_depth = 16, .tone_map_to_sdr = true};
  auto expected = awj::convert_image_pixels(logical, request, {}, resources);
  auto converted = awj::convert_image_pixels(source, request, {}, resources);
  if (!expected || !converted || !same_planes(*expected, *converted) || converted->geometry ||
      converted->source_info->transfer_characteristics != 13)
    return fail("Native 12-bit PQ gray geometry did not use the shared HDR reader.");
  auto yuv = awj::convert_image_pixels(logical, {.format = awj::PixelFormat::yuv420,
      .bit_depth = 12, .matrix_coefficients = 9}, {}, resources);
  if (!yuv) return fail("Native HDR YUV420 fixture conversion failed.");
  auto tone_yuv = awj::convert_image_pixels(*yuv, request, {}, resources);
  if (!tone_yuv || tone_yuv->source_info->transfer_characteristics != 13) return fail("Native HDR YUV tone failed.");
  for (std::size_t i = 0; i < expected->planes[0].bytes.size(); i += 2) {
    std::uint16_t a{}, b{};
    std::memcpy(&a, expected->planes[0].bytes.data() + i, 2);
    std::memcpy(&b, tone_yuv->planes[0].bytes.data() + i, 2);
    if (std::abs(static_cast<int>(a) - b) / 65535.0F > 2.0e-4F)
      return fail("Native HDR YUV reader exceeded pre-quantization sRGB tolerance.");
  }
  return 0;
}

int check_hdr_geometry() {
  if (const auto result = check_hdr_native_gray()) return result;
  const auto stored = premultiplied_scrgb();
  const awj::ResourcePlan resources{.file_parallelism = 2, .encoder_threads_per_file = 3,
      .global_thread_budget = 6, .memory_limit_bytes = 0};
  for (unsigned rotation = 0; rotation < 4; ++rotation) for (int mirror = -1; mirror <= 1; ++mirror) {
    for (const bool crop : {false, true}) {
      auto view = stored;
      view.geometry = awj::ImageGeometry{.stored_width = 3, .stored_height = 2,
          .crop_x = crop ? 1u : 0u, .crop_y = 0, .crop_width = crop ? 2u : 3u,
          .crop_height = 2, .rotation = rotation, .mirror_axis = mirror};
      view.width = rotation % 2 ? 2 : view.geometry->crop_width;
      view.height = rotation % 2 ? view.geometry->crop_width : 2;
      auto reference = reference_straight_geometry(view);
      auto hdr_view = awj::hdr::materialize_scrgb_as_hdr10(awj::ImageBuffer{view});
      auto hdr_reference = awj::hdr::materialize_scrgb_as_hdr10(awj::ImageBuffer{reference});
      if (!hdr_view || !hdr_reference || !same_planes(*hdr_view, *hdr_reference) ||
          hdr_view->geometry || hdr_view->alpha_mode != awj::AlphaMode::straight)
        return fail("HDR materialization changed geometry or premultiplied linear samples.");
      for (const int depth : {8, 16}) {
        auto expected = awj::hdr::tone_map_to_sdr_srgb(reference, depth, {}, resources);
        auto converted = awj::convert_image_pixels(view, {.bit_depth = depth, .tone_map_to_sdr = true}, {}, resources);
        auto direct = awj::hdr::tone_map_to_sdr_srgb(view, depth, {}, resources);
        if (!expected || !converted || !direct) return fail("Geometry/premultiplied linear HDR tone conversion failed.");
        if (!same_planes(*expected, *converted) || !same_planes(*expected, *direct) ||
            converted->alpha_mode != awj::AlphaMode::straight || converted->geometry ||
            converted->source_info->transfer_characteristics != 13)
          return fail("Geometry/unpremultiply changed negative scRGB tone input, alpha or dither coordinates.");
      }
      const awj::PixelConversionRequest yuv{.format = awj::PixelFormat::yuv420, .bit_depth = 10,
          .matrix_coefficients = 1, .color_range = 1, .tone_map_to_sdr = true};
      auto full_precision = awj::hdr::tone_map_to_sdr_srgb(reference, 16, {}, resources);
      if (!full_precision) return fail("Reference HDR tone failed.");
      auto without_tone = yuv; without_tone.tone_map_to_sdr = false;
      auto expected_yuv = awj::convert_image_pixels(*full_precision, without_tone, {}, resources);
      auto converted_yuv = awj::convert_image_pixels(view, yuv, {}, resources);
      if (!expected_yuv || !converted_yuv || !same_planes(*expected_yuv, *converted_yuv))
        return fail("Native YUV tone conversion bypassed shared HDR core or lost precision.");
      auto discarded = awj::convert_image_pixels(view,
          {.bit_depth = 16, .preserve_alpha = false, .tone_map_to_sdr = true}, {}, resources);
      if (!discarded || discarded->alpha_mode != awj::AlphaMode::none) return fail("Tone alpha discard failed.");
      for (std::size_t y = 0; y < discarded->height; ++y) for (std::size_t x = 0; x < discarded->width; ++x) {
        std::uint16_t alpha{}; std::memcpy(&alpha, discarded->planes[0].bytes.data() + y * discarded->planes[0].stride + x * 8 + 6, 2);
        if (alpha != 65535) return fail("Discarded tone alpha was not opaque.");
      }
    }
  }
  auto hidden_nan = stored;
  const std::uint16_t nan = 0x7e00u, zero = 0;
  std::memcpy(hidden_nan.planes[0].bytes.data(), &nan, 2);
  std::memcpy(hidden_nan.planes[0].bytes.data() + 6, &zero, 2);
  if (awj::convert_image_pixels(hidden_nan, {.tone_map_to_sdr = true}, {}, resources) ||
      awj::hdr::tone_map_to_sdr_srgb(hidden_nan, 8, {}, resources))
    return fail("Transparent premultiplied alpha hid a non-finite source component.");
  awj::compat::stop_source stop; stop.request_stop();
  if (awj::convert_image_pixels(stored, {.tone_map_to_sdr = true}, stop.get_token(), resources))
    return fail("Shared HDR conversion ignored resource-aware cancellation.");
  return 0;
}

int check_hdr_recursive_budget() {
  auto source = scrgb();
  source.width = source.height = 64;
  source.alpha_mode = awj::AlphaMode::none;
  source.metadata.clear();
  source.planes[0].stride = 64 * 8;
  source.planes[0].bytes.resize(64 * 64 * 8);
  const std::array<std::uint16_t, 4> rgba{0x3800u, 0x3800u, 0x3800u, 0x3c00u};
  for (std::size_t i = 0; i < 64 * 64; ++i)
    std::memcpy(source.planes[0].bytes.data() + i * 8, rgba.data(), sizeof(rgba));
  const awj::PixelConversionRequest yuv{.format = awj::PixelFormat::yuv420, .bit_depth = 8,
      .preserve_alpha = false, .tone_map_to_sdr = true};
  awj::ResourcePlan resources{.file_parallelism = 1, .encoder_threads_per_file = 1,
      .global_thread_budget = 1, .memory_limit_bytes = 68 * 1024};
  // Source RGBA16 + tone RGBA16 + final 420 = 32 + 32 + 6 KiB,
  // before scratch. The original source remains alive through recursion.
  if (awj::convert_image_pixels(source, yuv, {}, resources))
    return fail("HDR recursion admitted 70 KiB of pixels into a 68 KiB budget.");
  resources.memory_limit_bytes = 6 * 1024; // Exactly the final target reservation.
  if (awj::convert_image_pixels(source, yuv, {}, resources))
    return fail("A zero remaining recursive budget became unlimited.");
  resources.memory_limit_bytes = 70 * 1024; // Pixels alone fit, scratch does not.
  if (awj::convert_image_pixels(source, yuv, {}, resources))
    return fail("HDR recursion admitted no room for necessary scratch.");
  resources.memory_limit_bytes = 78 * 1024;
  if (!awj::convert_image_pixels(source, yuv, {}, resources))
    return fail("A fitting finite HDR recursive budget was rejected.");
  resources.memory_limit_bytes = 0;
  auto unlimited = awj::convert_image_pixels(source, yuv, {}, resources);
  if (!unlimited || unlimited->pixel_format != awj::PixelFormat::yuv420)
    return fail("Unlimited HDR recursion was rejected.");
  const awj::PixelConversionRequest rgba12{.bit_depth = 12, .preserve_alpha = false, .tone_map_to_sdr = true};
  resources.memory_limit_bytes = 90 * 1024; // Three RGBA16-sized planes need 96 KiB.
  if (awj::convert_image_pixels(source, rgba12, {}, resources))
    return fail("Secondary RGBA recursion omitted the live original source/target.");
  resources.memory_limit_bytes = 0;
  if (!awj::convert_image_pixels(source, rgba12, {}, resources))
    return fail("Unlimited secondary RGBA recursion failed.");
  source.metadata.push_back({awj::MetadataKind::exif, std::vector<std::byte>(4096)});
  source.metadata.push_back({awj::MetadataKind::xmp, std::vector<std::byte>(4096)});
  resources.memory_limit_bytes = 80 * 1024;
  if (awj::convert_image_pixels(source, yuv, {}, resources))
    return fail("HDR recursion omitted simultaneously-live metadata copies.");
  resources.memory_limit_bytes = 0;
  if (!awj::convert_image_pixels(source, yuv, {}, resources))
    return fail("Unlimited HDR recursion with metadata failed.");
  // Cover the 4096-square reproduction's arithmetic without allocating a
  // 128-MiB source in this small regression test.
  const auto large_target = awj::pixel_detail::destination_pixel_bytes(4096, 4096, yuv, awj::AlphaMode::none);
  if (large_target != 24ull * 1024 * 1024 ||
      2 * (4096ull * 4096 * 8) + large_target != 280ull * 1024 * 1024 ||
      2 * (4096ull * 4096 * 8) + large_target <= 272ull * 1024 * 1024)
    return fail("HDR recursion's 4096-square working-set arithmetic differs.");
  return 0;
}

int check_requested_color_conversion() {
  auto source = rgba();
  source.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 8,
      .color_primaries = 12, .transfer_characteristics = 13, .matrix_coefficients = 0, .color_range = 1};
  const auto original = source.planes[0].bytes;
  const awj::PixelConversionRequest request{.bit_depth = 16, .color_primaries = 9, .transfer_characteristics = 16};
  auto converted = awj::convert_image_pixels(source, request);
  if (!converted || !converted->source_info || converted->source_info->color_primaries != 9 ||
      converted->source_info->transfer_characteristics != 16 || !converted->source_info->has_hdr_metadata ||
      source.planes[0].bytes != original)
    return fail(converted ? "Requested color conversion did not preserve ownership/color semantics." : converted.error().c_str());
  // 标准sRGB EOTF、P3-D65到BT.2020矩阵和PQ OETF，独立于生产转换函数。
  constexpr double matrix[3][3]{{0.7538330344, 0.1985973691, 0.0475695965},
      {0.0457438489, 0.9417772198, 0.0124789313}, {-0.0012103404, 0.0176017173, 0.9836086231}};
  for (std::size_t y = 0; y < source.height; ++y) for (std::size_t x = 0; x < source.width; ++x) {
    double linear[3]{};
    for (int c = 0; c < 3; ++c) {
      const double v = std::to_integer<unsigned>(source.planes[0].bytes[y * source.planes[0].stride + x * 4 + c]) / 255.0;
      linear[c] = (v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)) * 203.0;
    }
    for (int c = 0; c < 3; ++c) {
      const double nits = std::max(0.0, matrix[c][0] * linear[0] + matrix[c][1] * linear[1] + matrix[c][2] * linear[2]);
      const double p = std::pow(nits / 10000.0, 2610.0 / 16384.0);
      const double expected = std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) /
          (1.0 + 2392.0 / 128.0 * p), 2523.0 / 32.0);
      std::uint16_t sample{};
      std::memcpy(&sample, converted->planes[0].bytes.data() + y * converted->planes[0].stride + (x * 4 + c) * 2, 2);
      if (std::abs(sample / 65535.0 - expected) > 4.0 / 65535.0)
        return fail("CICP override changed only labels, or diverged from independent P3/sRGB-to-2020/PQ math.");
    }
  }
  auto unknown = rgba();
  if (awj::convert_image_pixels(unknown, request)) return fail("Unknown source color was guessed during conversion.");
  if (awj::convert_image_pixels(*converted, {.bit_depth = 16, .transfer_characteristics = 13}))
    return fail("HDR was converted to SDR by relabeling instead of an explicit tone map.");
  return 0;
}

std::uint16_t sample16(const awj::ImageBuffer& image, std::size_t pixel, std::size_t channel) {
  std::uint16_t value{};
  std::memcpy(&value, image.planes[0].bytes.data() + (pixel / image.width) * image.planes[0].stride +
      (pixel % image.width * 4 + channel) * 2, 2);
  return value;
}

double pq_reference(double nits) {
  const double p = std::pow(std::max(0.0, nits) / 10000.0, 2610.0 / 16384.0);
  return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) /
      (1.0 + 2392.0 / 128.0 * p), 2523.0 / 32.0);
}

int check_hlg_and_tone_color_boundaries() {
  awj::ImageBuffer white{.width = 2, .height = 2, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 16,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
          .color_primaries = 9, .transfer_characteristics = 18, .matrix_coefficients = 0,
          .color_range = 1, .has_hdr_metadata = true}};
  white.planes = {{std::vector<std::byte>(32, std::byte{255}), 16}};
  auto pq = awj::convert_image_pixels(white, {.bit_depth = 16, .transfer_characteristics = 16});
  if (!pq) return fail(pq.error().c_str());
  for (std::size_t pixel = 0; pixel < 4; ++pixel) for (std::size_t c = 0; c < 3; ++c)
    if (std::abs(sample16(*pq, pixel, c) / 65535.0 - pq_reference(1000.0)) > 4.0 / 65535.0)
      return fail("HLG white-to-PQ lost the 1000-nit peak scale.");

  auto source = rgba();
  source.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 8,
      .color_primaries = 9, .transfer_characteristics = 13, .matrix_coefficients = 0, .color_range = 1};
  constexpr std::array<std::array<unsigned, 3>, 4> colors{{{0, 0, 0}, {4, 4, 4}, {100, 0, 0}, {255, 255, 255}}};
  for (std::size_t pixel = 0; pixel < source.width * source.height; ++pixel) {
    for (std::size_t c = 0; c < 3; ++c) source.planes[0].bytes[pixel * 4 + c] = std::byte(colors[pixel % 4][c]);
    source.planes[0].bytes[pixel * 4 + 3] = std::byte{255};
  }
  auto hlg = awj::convert_image_pixels(source, {.bit_depth = 16, .transfer_characteristics = 18});
  if (!hlg) return fail(hlg.error().c_str());
  for (std::size_t pixel = 0; pixel < source.width * source.height; ++pixel) {
    double nits[3]{};
    for (int c = 0; c < 3; ++c) {
      const double v = colors[pixel % 4][c] / 255.0;
      nits[c] = 203.0 * (v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4));
    }
    // BT.2100 1000-nit 逆 OOTF/OETF；与生产 libplacebo 调用独立。
    const double luma = .2627 * nits[0] + .6780 * nits[1] + .0593 * nits[2];
    // v7.387.0 将 HDR_BLACK 当作无限对比度哨兵，信号黑点实际为零。
    constexpr double black = 0.0;
    for (std::size_t c = 0; c < 3; ++c) {
      const double linear = luma > 0 ? nits[c] / 1000.0 * std::pow(luma / 1000.0, (1.0 - 1.2) / 1.2) : 0;
      const double signal = linear <= 1.0 / 12.0 ? std::sqrt(3.0 * linear)
          : .17883277 * std::log(12.0 * linear - .28466892) + .55991073;
      const double expected = luma > 0 ? std::clamp((signal - black) / (1.0 - black), 0.0, 1.0) : 0;
      if (std::abs(sample16(*hlg, pixel, c) / 65535.0 - expected) > 4.0 / 65535.0) {
        std::fprintf(stderr, "HLG pixel=%zu channel=%zu actual=%.9g reference=%.9g luma_nits=%.9g black=%.9g\n",
            pixel, c, sample16(*hlg, pixel, c) / 65535.0, expected, luma, black);
        return fail("Target HLG black/gray/primary differs from the independent inverse OOTF/OETF.");
      }
    }
  }
  auto black_pq = white;
  black_pq.source_info->transfer_characteristics = 16;
  for (std::size_t pixel = 0; pixel < 4; ++pixel) for (std::size_t c = 0; c < 3; ++c) {
    const std::uint16_t zero{}; std::memcpy(black_pq.planes[0].bytes.data() + (pixel * 4 + c) * 2, &zero, 2);
  }
  auto black_hlg = awj::convert_image_pixels(black_pq, {.bit_depth = 16, .transfer_characteristics = 18});
  if (!black_hlg) return fail(black_hlg.error().c_str());
  for (std::size_t pixel = 0; pixel < 4; ++pixel) for (std::size_t c = 0; c < 3; ++c)
    if (sample16(*black_hlg, pixel, c) != 0) return fail("PQ black did not map to finite HLG black.");

  auto linear = scrgb();
  for (int depth : {8, 16}) {
    auto mapped = awj::convert_image_pixels(linear, {.bit_depth = depth, .tone_map_to_sdr = true});
    if (!mapped) return fail(mapped.error().c_str());
    auto reference = awj::convert_image_pixels(*mapped, {.bit_depth = depth, .color_primaries = 12, .transfer_characteristics = 4});
    auto direct = awj::convert_image_pixels(linear, {.bit_depth = depth, .tone_map_to_sdr = true,
        .color_primaries = 12, .transfer_characteristics = 4});
    if (!reference || !direct || !same_planes(*reference, *direct) ||
        direct->source_info->color_primaries != 12 || direct->source_info->transfer_characteristics != 4)
      return fail("RGBA tone-map fast path ignored the remaining target CICP request.");
  }
  for (int primaries : {1, 12}) {
    auto explicit_half = awj::convert_image_pixels(linear, {.bit_depth = 16, .color_primaries = primaries});
    if (!explicit_half || explicit_half->source_info->color_primaries != primaries ||
        explicit_half->source_info->transfer_characteristics != 16)
      return fail("FP16 materialization ignored an explicit source-matching primaries request.");
  }
  if (awj::convert_image_pixels(linear, {.bit_depth = 16, .transfer_characteristics = 8}))
    return fail("Unsupported extended linear HDR output silently became PQ.");
  return 0;
}

int check_icc_native_yuv_conversion() {
  auto profile = awj::test::display_p3_srgb_icc();
  if (!profile) return fail(profile.error().c_str());
  for (auto format : {awj::PixelFormat::yuv420, awj::PixelFormat::yuv422, awj::PixelFormat::yuv444})
    for (const auto values : {std::array<unsigned, 3>{16, 128, 128}, std::array<unsigned, 3>{235, 128, 128},
                             std::array<unsigned, 3>{81, 90, 240}}) {
      awj::ImageBuffer native{.width = 4, .height = 4, .pixel_format = format, .bit_depth = 8,
          .source_info = awj::ImageSourceInfo{.pixel_format = format, .bit_depth = 8,
              .matrix_coefficients = 6, .color_range = 0}};
      const std::size_t cw = format == awj::PixelFormat::yuv444 ? 4 : 2;
      const std::size_t ch = format == awj::PixelFormat::yuv420 ? 2 : 4;
      native.planes = {{std::vector<std::byte>(16, std::byte(values[0])), 4},
          {std::vector<std::byte>(cw * ch, std::byte(values[1])), cw},
          {std::vector<std::byte>(cw * ch, std::byte(values[2])), cw}};
      native.metadata.push_back({awj::MetadataKind::icc, *profile});
      auto packed = awj::convert_image_pixels(native, {.bit_depth = 16});
      if (!packed) return fail(packed.error().c_str());
      auto verified = awj::gain_map_detail::resolve_iso_color(native, {});
      if (!verified) return fail(verified.error().c_str());
      awj::gain_map_detail::normalize_iso_base(native, *verified);
      awj::gain_map_detail::normalize_iso_base(*packed, *verified);
      if (native.source_info->matrix_coefficients != 6 || native.source_info->color_range != 0)
        return fail("ICC equivalence normalization relabeled native YUV planes as RGB.");
      const awj::PixelConversionRequest request{.bit_depth = 16, .color_primaries = 9, .transfer_characteristics = 16};
      auto direct = awj::convert_image_pixels(native, request);
      auto reference = awj::convert_image_pixels(*packed, request);
      if (!direct || !reference) return fail(!direct ? direct.error().c_str() : reference.error().c_str());
      for (std::size_t pixel = 0; pixel < 16; ++pixel) for (std::size_t c = 0; c < 3; ++c)
        if (std::abs(static_cast<int>(sample16(*direct, pixel, c)) - static_cast<int>(sample16(*reference, pixel, c))) > 4 ||
            (values[0] == 16 && sample16(*direct, pixel, c) > 1))
          return fail("Native YUV ICC override corrupted limited black or differed from an RGB-first reference.");
    }
  return 0;
}

awj::compat::expected<awj::ImageBuffer, std::string> legacy_geometry(
    const awj::ImageBuffer& source, const awj::ImageGeometryPlan& plan, bool keep_alpha) {
  awj::pixel_detail::Reader reader{source};
  if (awj::is_yuv_format(source.pixel_format)) {
    const auto description = awj::pixel_detail::description(source);
    if (!avifGetYUVColorSpaceInfo(&description, &reader.color)) return awj::compat::unexpected{"Legacy YUV fixture matrix failed"};
  }
  const bool floating = source.sample_representation == awj::SampleRepresentation::ieee_half_float;
  const bool alpha = keep_alpha && (source.alpha_mode != awj::AlphaMode::none || plan.pads());
  const auto sample_bytes = source.bit_depth > 8 ? 2u : 1u;
  const auto maximum = static_cast<std::uint16_t>((1u << source.bit_depth) - 1);
  const auto opaque = floating ? std::uint16_t{0x3c00} : maximum;
  awj::ImageBuffer output{.width = plan.canvas_width, .height = plan.canvas_height, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = alpha ? awj::AlphaMode::straight : awj::AlphaMode::none, .bit_depth = source.bit_depth,
      .sample_representation = source.sample_representation};
  const auto stride = plan.canvas_width * 4 * sample_bytes;
  output.planes.push_back({std::vector<std::byte>(stride * plan.canvas_height), stride});
  if (!alpha) for (std::size_t y = 0; y < plan.canvas_height; ++y) for (std::size_t x = 0; x < plan.canvas_width; ++x) {
    auto* at = output.planes[0].bytes.data() + y * stride + (x * 4 + 3) * sample_bytes;
    if (sample_bytes == 1) *at = std::byte{255}; else std::memcpy(at, &opaque, 2);
  }
  auto filtered = awj::test::legacy_spline36(plan.source_width, plan.source_height, plan.content_width, plan.content_height,
      [&](std::size_t x, std::size_t y) { return reader(x, y); },
      [&](std::size_t x, std::size_t y, std::array<double, 4> rgba) -> awj::compat::expected<void, std::string> {
        for (std::size_t c = 0; c < 4; ++c) {
          const auto value = c == 3 ? (alpha ? rgba[3] : 1.0) : rgba[c] * (keep_alpha ? 1.0 : rgba[3]);
          if (!std::isfinite(value) || (floating && std::abs(value) > 65504)) return awj::compat::unexpected{"Legacy non-finite output"};
          // 与旧实现共用最终量化器，仅替换重采样路径；独立二维多项式参考不共用其数学实现。
          const auto encoded = floating ? awj::geometry_binary16(static_cast<float>(value)) :
              static_cast<std::uint16_t>(std::floor(std::clamp(value, 0.0, 1.0) * maximum + 0.5));
          auto* at = output.planes[0].bytes.data() + (y + plan.offset_y) * stride + ((x + plan.offset_x) * 4 + c) * sample_bytes;
          if (sample_bytes == 1) *at = std::byte{static_cast<unsigned char>(encoded)};
          else std::memcpy(at, &encoded, 2);
        }
        return {};
      });
  if (!filtered) return awj::compat::unexpected{filtered.error()};
  return output;
}

awj::compat::expected<awj::ImageBuffer, std::string> resize_fixture(awj::PixelFormat format, int depth,
    awj::AlphaMode alpha, std::size_t width, std::size_t height, int range = 1) {
  const bool rgb = format == awj::PixelFormat::rgb;
  const auto channels = rgb ? 3u : 4u;
  const auto sample_bytes = depth > 8 ? 2u : 1u;
  const auto maximum = (1u << depth) - 1;
  awj::ImageBuffer source{.width = width, .height = height,
      .pixel_format = rgb ? awj::PixelFormat::rgb : awj::PixelFormat::rgba,
      .alpha_mode = rgb ? awj::AlphaMode::none : alpha, .bit_depth = depth};
  const auto stride = width * channels * sample_bytes + 8;
  source.planes.push_back({std::vector<std::byte>(stride * height, std::byte{0xa5}), stride});
  source.metadata.push_back({awj::MetadataKind::xmp, std::vector<std::byte>(17, std::byte{0x41})});
  for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
    const auto a = alpha == awj::AlphaMode::none ? maximum : ((x * 13 + y * 37) % 257) * maximum / 256;
    for (std::size_t c = 0; c < channels; ++c) {
      auto value = c == 3 ? a : ((x * 197 + y * 71 + c * 47 + 17) % 1024) * maximum / 1023;
      if (c < 3 && alpha == awj::AlphaMode::premultiplied) value = value * a / maximum;
      const auto code = static_cast<std::uint16_t>(value);
      auto* at = source.planes[0].bytes.data() + y * stride + (x * channels + c) * sample_bytes;
      if (sample_bytes == 1) *at = std::byte{static_cast<unsigned char>(code)}; else std::memcpy(at, &code, 2);
    }
  }
  if (!awj::is_yuv_format(format)) return source;
  return awj::convert_image_pixels(source, {.format = format, .bit_depth = depth, .matrix_coefficients = 6, .color_range = range});
}

int compare_resize(const awj::ImageBuffer& source, std::size_t width, std::size_t height, bool padding, bool keep_alpha) {
  const auto logical = awj::image_logical_dimensions(source);
  const awj::ImageGeometryPlan plan{.source_width = logical[0], .source_height = logical[1],
      .content_width = width, .content_height = height, .canvas_width = width + (padding ? 3 : 0),
      .canvas_height = height + (padding ? 2 : 0), .offset_x = padding ? 1u : 0u, .offset_y = padding ? 1u : 0u};
  auto reference = legacy_geometry(source, plan, keep_alpha);
  if (!reference) return fail(reference.error().c_str());
  for (const int budget : {1, 2, 8}) {
    const awj::ResourcePlan resources{.encoder_threads_per_file = budget, .global_thread_budget = budget};
    awj::GeometryTransformDiagnostics diagnostics;
    auto result = awj::apply_image_geometry(source, plan, keep_alpha, {}, resources, &diagnostics);
    if (!result || !same_planes(*result, *reference) || result->alpha_mode != reference->alpha_mode ||
        result->sample_representation != source.sample_representation || result->bit_depth != source.bit_depth || result->geometry)
      return fail(result ? "Cached/parallel native resampling differs from the old serial per-pixel path." : result.error().c_str());
    const auto wanted = awj::spline36_detail::worker_limit(logical[0], logical[1], width, height, budget);
    if (diagnostics.workers != wanted || diagnostics.scratch_bytes > awj::spline36_detail::scratch_bytes(logical[0], logical[1], width, height, budget))
      return fail("Native resize worker/scratch diagnostics do not match the budget.");
    if (result->metadata.size() != source.metadata.size() ||
        (!source.metadata.empty() && result->metadata[0].bytes != source.metadata[0].bytes)) return fail("Geometry lost metadata.");
  }
  return 0;
}

int check_cached_native_resizes() {
  const std::array layouts{awj::PixelFormat::rgb, awj::PixelFormat::rgba, awj::PixelFormat::yuv444,
      awj::PixelFormat::yuv422, awj::PixelFormat::yuv420, awj::PixelFormat::gray};
  unsigned geometry_case{};
  for (const auto layout : layouts) for (const int depth : {8, 10, 12, 16}) {
    if (!awj::is_yuv_format(layout) && depth != 8 && depth != 16) continue;
    for (const auto alpha : {awj::AlphaMode::none, awj::AlphaMode::straight}) {
      if (layout == awj::PixelFormat::rgb && alpha != awj::AlphaMode::none) continue;
      auto source = resize_fixture(layout, depth, alpha, 17, 13, static_cast<int>(geometry_case % 2));
      if (!source) return fail(source.error().c_str());
      if (awj::is_yuv_format(layout)) source->source_info->chroma_location = 1 + static_cast<int>(geometry_case % 6);
      const auto rotation = geometry_case % 4;
      const auto mirror = static_cast<int>((geometry_case / 4) % 3) - 1;
      ++geometry_case;
      source->geometry = awj::ImageGeometry{.stored_width = 17, .stored_height = 13,
          .crop_x = 1, .crop_y = 2, .crop_width = 14, .crop_height = 9, .rotation = rotation, .mirror_axis = mirror};
      source->width = rotation % 2 ? 9 : 14; source->height = rotation % 2 ? 14 : 9;
      for (const auto size : {std::array<std::size_t, 2>{7, 5}, {4, 3}, {23, 19}, {1, 9}, {11, 1}, {1, 1}})
        for (const bool keep : {false, true})
          if (const auto result = compare_resize(*source, size[0], size[1], true, keep)) return result;
    }
    if (depth == 8 || depth == 16) {
      auto large = resize_fixture(layout, depth, layout == awj::PixelFormat::rgb ? awj::AlphaMode::none : awj::AlphaMode::straight, 512, 256);
      if (!large) return fail(large.error().c_str());
      if (const auto result = compare_resize(*large, 257, 129, false, true)) return result;
    }
  }
  for (const int depth : {8, 16}) {
    auto premultiplied = resize_fixture(awj::PixelFormat::rgba, depth, awj::AlphaMode::premultiplied, 512, 256);
    if (!premultiplied) return fail(premultiplied.error().c_str());
    for (const auto size : {std::array<std::size_t, 2>{256, 128}, {128, 64}, {713, 371}})
      if (const auto result = compare_resize(*premultiplied, size[0], size[1], false, true)) return result;
  }
  for (const auto alpha : {awj::AlphaMode::straight, awj::AlphaMode::premultiplied}) {
    auto half = scrgb();
    half.width = 512; half.height = 256; half.alpha_mode = alpha;
    half.planes[0].stride = half.width * 8;
    half.planes[0].bytes.resize(half.planes[0].stride * half.height);
    for (std::size_t y = 0; y < half.height; ++y) for (std::size_t x = 0; x < half.width; ++x) {
      const float a = (x + y) % 3 == 0 ? 0.0F : (x + y) % 3 == 1 ? 0.5F : 1.0F;
      for (std::size_t c = 0; c < 4; ++c) {
        auto value = c == 3 ? a : (static_cast<int>((x * 3 + y * 7 + c * 5) % 33) - 8) * 0.125F;
        if (c < 3 && alpha == awj::AlphaMode::premultiplied) value *= a;
        const auto code = dyadic_half(value);
        std::memcpy(half.planes[0].bytes.data() + y * half.planes[0].stride + (x * 4 + c) * 2, &code, 2);
      }
    }
    if (const auto result = compare_resize(half, 257, 129, true, true)) return result;
    const std::uint16_t nan = 0x7e00, zero = 0;
    std::memcpy(half.planes[0].bytes.data(), &nan, 2);
    std::memcpy(half.planes[0].bytes.data() + 6, &zero, 2);
    awj::GeometryTransformDiagnostics diagnostics{8, 1};
    const awj::ResourcePlan resources{.encoder_threads_per_file = 8, .global_thread_budget = 8};
    if (awj::resize_native_pixels(half, 257, 129, {}, resources, &diagnostics) || diagnostics.workers)
      return fail("Parallel FP16 resampling hid a non-finite transparent component or reported workers on failure.");
  }
  for (unsigned rotation = 0; rotation < 4; ++rotation) for (int mirror = -1; mirror <= 1; ++mirror)
    for (const bool crop : {false, true}) {
      auto view = premultiplied_scrgb();
      view.geometry = awj::ImageGeometry{.stored_width = 3, .stored_height = 2,
          .crop_x = crop ? 1u : 0u, .crop_width = crop ? 2u : 3u, .crop_height = 2,
          .rotation = rotation, .mirror_axis = mirror};
      view.width = rotation % 2 ? 2 : view.geometry->crop_width;
      view.height = rotation % 2 ? view.geometry->crop_width : 2;
      if (const auto result = compare_resize(view, 5, 7, true, true)) return result;
    }
  return 0;
}

awj::ImageBuffer threaded_pixel_fixture(int depth, awj::AlphaMode alpha) {
  constexpr std::size_t width = 259, height = 257;
  const auto maximum = (1u << depth) - 1;
  const auto sample_bytes = depth > 8 ? 2u : 1u;
  awj::ImageBuffer image{.width = width, .height = height, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = alpha, .bit_depth = depth,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = depth,
          .color_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 0, .color_range = 1},
      // Force the owned Reader path, including mirrored odd 420/422 edges.
      .geometry = awj::ImageGeometry{.stored_width = width, .stored_height = height,
          .crop_width = width, .crop_height = height, .mirror_axis = 1}};
  awj::ImagePlane plane{.stride = width * 4 * sample_bytes + 16};
  plane.bytes.resize(plane.stride * height, std::byte{0xa5});
  for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
    const auto a = alpha == awj::AlphaMode::none ? maximum : static_cast<unsigned>((x * 97 + y * 13) & maximum);
    for (std::size_t c = 0; c < 4; ++c) {
      auto value = c == 3 ? a : static_cast<unsigned>((x * 31 + y * 67 + c * 41) & maximum);
      if (c < 3 && alpha == awj::AlphaMode::premultiplied)
        value = static_cast<unsigned>(static_cast<std::uint64_t>(value) * a / maximum);
      auto* dst = plane.bytes.data() + y * plane.stride + (x * 4 + c) * sample_bytes;
      const auto encoded = static_cast<std::uint16_t>(value);
      if (sample_bytes == 1) *dst = std::byte{static_cast<unsigned char>(encoded)};
      else std::memcpy(dst, &encoded, 2);
    }
  }
  image.planes.push_back(std::move(plane));
  return image;
}

int check_threaded_pixel_conversion() {
  const awj::ResourcePlan serial{.file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1};
  const awj::ResourcePlan parallel{.file_parallelism = 2, .encoder_threads_per_file = 7, .global_thread_budget = 8};
  constexpr std::array depths{8, 10, 12, 16};
  constexpr std::array formats{awj::PixelFormat::yuv420, awj::PixelFormat::yuv422, awj::PixelFormat::yuv444};
  awj::compat::stop_source canceled; canceled.request_stop();
  for (std::size_t d = 0; d < depths.size(); ++d) {
    const auto depth = depths[d];
    const auto target_depth = depths[(d + 1) % depths.size()];
    for (const auto alpha : {awj::AlphaMode::none, awj::AlphaMode::straight, awj::AlphaMode::premultiplied}) {
      const auto source = threaded_pixel_fixture(depth, alpha);
      for (const auto format : formats) {
        const awj::PixelConversionRequest request{.format = format, .bit_depth = depth,
            .matrix_coefficients = 6, .color_range = 1};
        auto expected = awj::convert_image_pixels(source, request, {}, serial);
        auto converted = awj::convert_image_pixels(source, request, {}, parallel);
        if (!expected || !converted || !same_planes(*expected, *converted) ||
            expected->alpha_mode != converted->alpha_mode || converted->geometry)
          return fail("Threaded RGB/YUV conversion changed alpha, geometry or odd chroma edges.");
        auto view = awj::yuv_plane_view(*expected);
        if (!view) return fail("Cannot build the native YUV copy fixture.");
        auto serial_copy = awj::copy_yuv_pixels(*view, {}, serial);
        auto threaded_copy = awj::copy_yuv_pixels(*view, {}, parallel);
        if (!serial_copy || !threaded_copy || !same_planes(*expected, *serial_copy) ||
            !same_planes(*serial_copy, *threaded_copy) ||
            awj::copy_yuv_pixels(*view, canceled.get_token(), parallel))
          return fail("Threaded native YUV/alpha copy changed planes or ignored cancellation.");
        // Native YUV fast path, and the explicit chroma-location Reader path.
        for (const int location : {1, 2}) {
          converted->source_info->chroma_location = location;
          for (const bool preserve : {false, true}) {
            const awj::PixelConversionRequest rgb{.format = awj::PixelFormat::rgba,
                .bit_depth = target_depth, .preserve_alpha = preserve};
            auto a = awj::convert_image_pixels(*converted, rgb, {}, serial);
            auto b = awj::convert_image_pixels(*converted, rgb, {}, parallel);
            if (!a || !b || !same_planes(*a, *b) || a->alpha_mode != b->alpha_mode)
              return fail("Threaded YUV -> RGB changed 8/10/12/16-bit, alpha or chroma siting.");
          }
        }
        for (const auto target_format : formats) for (const int range : {0, 1}) {
          const awj::PixelConversionRequest yuv{.format = target_format, .bit_depth = target_depth,
              .matrix_coefficients = 6, .color_range = range};
          auto a = awj::convert_image_pixels(*converted, yuv, {}, serial);
          auto b = awj::convert_image_pixels(*converted, yuv, {}, parallel);
          if (!a || !b || !same_planes(*a, *b))
            return fail("Threaded YUV resampling/range/depth conversion changed planar samples.");
        }
        auto tight = parallel;
        tight.memory_limit_bytes = awj::pixel_detail::resident_image_bytes(source) + awj::pixel_detail::resident_image_bytes(*expected);
        tight.memory_reservation_bytes = tight.memory_limit_bytes;
        auto bounded = awj::convert_image_pixels(source, request, {}, tight);
        if (!bounded || !same_planes(*expected, *bounded)) return fail("Pixel control-memory fallback changed output.");
        if (awj::convert_image_pixels(source, request, canceled.get_token(), parallel))
          return fail("Threaded pixel conversion ignored cancellation.");
      }
    }
  }
  return 0;
}

} // namespace

int main() {
  if (const auto result = check_threaded_pixel_conversion()) return result;
  if (const auto result = check_cached_native_resizes()) return result;
  if (const auto result = check_requested_color_conversion()) return result;
  if (const auto result = check_hlg_and_tone_color_boundaries()) return result;
  if (const auto result = check_icc_native_yuv_conversion()) return result;
  if (const auto result = check_hdr_recursive_budget()) return result;
  if (const auto result = check_hdr_geometry()) return result;
  const auto source = rgba();
  const std::array formats{awj::OutputFormat::avif, awj::OutputFormat::png, awj::OutputFormat::webp,
      awj::OutputFormat::jxl, awj::OutputFormat::jpgli};
  awj::HeifImageDecoder heif;
  for (const char* name : {"rainbow-451x461.heic", "clap_cropped.heic", "with-alpha-512x512.heic"}) {
    const auto path = std::filesystem::path{AWJ_HEIF_TEST_DATA} / name;
    auto native = heif.decode(path, awj::DecodeOptions{.native_pixels = true});
    auto previous = heif.decode(path);
    if (!native || !previous) return fail(!native ? native.error().c_str() : previous.error().c_str());
    if (!awj::is_yuv_format(native->image.pixel_format) || native->image.width != previous->image.width ||
        native->image.height != previous->image.height || native->image.alpha_mode != previous->image.alpha_mode)
      return fail("Native HEIF changed YUV layout, transformed dimensions or alpha.");
    for (auto output : formats) {
      auto writer = encoder(output); auto verifier = decoder(output);
      if (!writer || !verifier) continue;
      awj::NativeEncodeSettings settings{.output_format = output, .quality = 70, .speed = 10, .bit_depth = 8,
          .source_has_alpha_channel = native->image.alpha_mode != awj::AlphaMode::none,
          .encoder_supports_alpha = output != awj::OutputFormat::jpgli,
          .applied_alpha = native->image.alpha_mode != awj::AlphaMode::none && output != awj::OutputFormat::jpgli ? "kept" : "discarded"};
      auto file = writer->encode(native->image, settings);
      if (!file) return fail(file.error().c_str());
      auto decoded = verifier->decode_memory(file->encoded.bytes, "native HEIF conversion");
      if (!decoded || decoded->image.width != native->image.width || decoded->image.height != native->image.height ||
          (settings.applied_alpha == "kept" && decoded->image.alpha_mode == awj::AlphaMode::none))
        return fail("HEIF conversion lost displayed dimensions or alpha.");
    }
  }
  for (auto input : formats) {
    auto writer = encoder(input); auto reader = decoder(input);
    if (!writer || !reader) continue;
    const awj::NativeEncodeSettings settings{.output_format = input, .quality = 70, .speed = 10,
        .bit_depth = 8, .source_has_alpha_channel = true, .encoder_supports_alpha = input != awj::OutputFormat::jpgli,
        .applied_alpha = input == awj::OutputFormat::jpgli ? "discarded" : "kept"};
    auto file = writer->encode(source, settings);
    if (!file) return fail(file.error().c_str());
    auto native = reader->decode_memory(file->encoded.bytes, "native pixel fixture", awj::DecodeOptions{.native_pixels = true});
    if (!native) return fail(native.error().c_str());
    if ((input == awj::OutputFormat::avif || input == awj::OutputFormat::webp || input == awj::OutputFormat::jpgli) &&
        !awj::is_yuv_format(native->image.pixel_format)) return fail("Decoder discarded native YUV planes.");
    for (auto output : formats) {
      auto destination = encoder(output); auto verifier = decoder(output);
      if (!destination || !verifier) continue;
      auto request = settings; request.output_format = output;
      request.encoder_supports_alpha = output != awj::OutputFormat::jpgli;
      request.applied_alpha = request.encoder_supports_alpha && native->image.alpha_mode != awj::AlphaMode::none ? "kept" : "discarded";
      auto converted = destination->encode(native->image, request);
      if (!converted) return fail(converted.error().c_str());
      auto decoded = verifier->decode_memory(converted->encoded.bytes, "cross-format conversion");
      if (!decoded) return fail(decoded.error().c_str());
      if (decoded->image.width != 5 || decoded->image.height != 3)
        return fail("Cross-format conversion changed dimensions.");
      if (request.applied_alpha == "kept" && decoded->image.alpha_mode == awj::AlphaMode::none)
        return fail("Cross-format conversion dropped alpha.");
    }
  }
  for (unsigned rotation = 0; rotation < 4; ++rotation) for (int mirror = -1; mirror <= 1; ++mirror) {
    auto transformed = source;
    if (!awj::gain_map_detail::transform_rgba(transformed, rotation, mirror)) return fail("Reference geometry failed.");
    auto view = source;
    view.geometry = awj::ImageGeometry{.stored_width = 5, .stored_height = 3,
        .crop_width = 5, .crop_height = 3, .rotation = rotation, .mirror_axis = mirror};
    view.width = rotation % 2 ? 3 : 5; view.height = rotation % 2 ? 5 : 3;
    auto fused = awj::convert_image_pixels(view, awj::PixelConversionRequest{});
    if (!fused || !same_planes(*fused, transformed)) return fail("Fused rotation/mirror changed pixels.");
  }
  auto view = source;
  view.geometry = awj::ImageGeometry{.stored_width = 5, .stored_height = 3,
      .crop_x = 1, .crop_y = 1, .crop_width = 3, .crop_height = 2};
  view.width = 2; view.height = 1;
  if (awj::convert_image_pixels(view, awj::PixelConversionRequest{}))
    return fail("旧的隐式最近邻缩放表示未被拒绝。");
  view.width = 3; view.height = 2;
  auto cropped = awj::convert_image_pixels(view, awj::PixelConversionRequest{});
  if (!cropped || cropped->planes[0].bytes[0] != source.planes[0].bytes[24] ||
      cropped->planes[0].bytes[4] != source.planes[0].bytes[28]) return fail("纯裁切没有保持源像素。");
  auto resized_crop = awj::resize_native_pixels(view, 2, 1);
  if (!resized_crop || resized_crop->width != 2 || resized_crop->height != 1 || resized_crop->geometry)
    return fail("显式 Spline36 裁切缩放失败。");
  view.geometry->crop_width = 10;
  if (awj::convert_image_pixels(view, awj::PixelConversionRequest{})) return fail("Invalid geometry was accepted.");
  const std::array layouts{awj::PixelFormat::yuv444, awj::PixelFormat::yuv422,
      awj::PixelFormat::yuv420, awj::PixelFormat::gray};
  for (auto layout : layouts) for (int depth : {8, 10, 12, 16}) for (int range : {0, 1}) {
    const awj::PixelConversionRequest request{.format = layout, .bit_depth = depth,
        .matrix_coefficients = 6, .color_range = range};
    auto yuv = awj::convert_image_pixels(source, request);
    if (!yuv) return fail(yuv.error().c_str());
    if (yuv->width != 5 || yuv->height != 3 || yuv->bit_depth != depth || yuv->alpha_mode != awj::AlphaMode::straight)
      return fail("Native YUV dimensions, depth or alpha changed.");
    auto unchanged = awj::convert_image_pixels(*yuv, request);
    if (!unchanged || !same_planes(*yuv, *unchanged)) return fail("YUV identity conversion changed samples.");
    auto rgb = awj::convert_image_pixels(*yuv, awj::PixelConversionRequest{.bit_depth = 16});
    if (!rgb) return fail(rgb.error().c_str());
    if (rgb->pixel_format != awj::PixelFormat::rgba || rgb->planes.front().bytes.size() != 120)
      return fail("YUV-to-RGB destination is invalid.");
    for (auto target : layouts) {
      auto converted = awj::convert_image_pixels(*yuv, awj::PixelConversionRequest{
          .format = target, .bit_depth = 10, .matrix_coefficients = 1, .color_range = 1});
      if (!converted) return fail(converted.error().c_str());
      if (converted->source_info->matrix_coefficients != 1 || converted->source_info->color_range != 1)
        return fail("Converted YUV signal properties are incorrect.");
    }
    auto smaller = awj::resize_native_pixels(*yuv, 3, 2);
    if (!smaller || !awj::validate_image_pixels(*smaller)) return fail("Native YUV resize is invalid.");
  }
  const auto hdr = scrgb();
  auto old = awj::hdr::materialize_scrgb_as_hdr10(hdr);
  if (!old) return fail(old.error().c_str());
  const awj::PixelConversionRequest pq{.format = awj::PixelFormat::yuv444, .bit_depth = 12, .matrix_coefficients = 9};
  auto fused = awj::convert_image_pixels(hdr, pq);
  auto reference = awj::convert_image_pixels(*old, pq);
  if (!fused || !reference) return fail("Fused HDR conversion failed.");
  if (fused->source_info->color_primaries != 9 || fused->source_info->transfer_characteristics != 16)
    return fail("Fused HDR conversion lost BT.2020/PQ.");
  for (std::size_t c = 0; c < fused->planes.size(); ++c) {
    for (std::size_t i = 0; i < fused->planes[c].bytes.size(); i += 2) {
      std::uint16_t a{}, b{};
      std::memcpy(&a, fused->planes[c].bytes.data() + i, 2);
      std::memcpy(&b, reference->planes[c].bytes.data() + i, 2);
      if (std::abs(static_cast<int>(a) - b) > 1) return fail("Fused HDR conversion exceeds one output codepoint.");
    }
  }
  auto sdr = awj::convert_image_pixels(hdr, awj::PixelConversionRequest{.tone_map_to_sdr = true});
  auto sdr_reference = awj::hdr::tone_map_to_sdr_srgb(hdr);
  if (!sdr || !sdr_reference) return fail("Fused SDR tone mapping failed.");
  for (std::size_t i = 0; i < sdr->planes[0].bytes.size(); ++i)
    if (std::abs(std::to_integer<int>(sdr->planes[0].bytes[i]) - std::to_integer<int>(sdr_reference->planes[0].bytes[i])) > 1)
      return fail("Fused SDR tone mapping differs from the reference.");
  auto truncated = source; truncated.planes[0].bytes.resize(2);
  if (awj::convert_image_pixels(truncated, pq)) return fail("Truncated input was accepted.");
  awj::compat::stop_source stop; stop.request_stop();
  if (awj::convert_image_pixels(source, pq, stop.get_token())) return fail("Cancellation was ignored.");
  auto planes = awj::yuv_plane_view(*fused);
  if (!planes || awj::copy_yuv_pixels(*planes, stop.get_token())) return fail("Native plane copy ignored cancellation.");
  std::puts("Native pixel conversion, odd dimensions, all layouts, alpha, HDR and cancellation passed.");
}
