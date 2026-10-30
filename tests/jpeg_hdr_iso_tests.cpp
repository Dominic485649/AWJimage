#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ultrahdr_api.h>
#include <ultrahdr/ultrahdrcommon.h>
#include <ultrahdr/gainmapmath.h>
#include "../src/core/hdr_cost.h"

import awj.codec;
import awj.config;
import awj.hdr_tonemap;
import awj.image;
import awj.jpeg_codec;
import awj.jpeg_hdr_codec;

namespace {

int fail(std::string_view text) { std::cerr << text << '\n'; return 1; }

float half_reference(std::uint16_t bits) {
  const auto exponent = (bits >> 10) & 31u, mantissa = bits & 1023u;
  const auto sign = bits & 0x8000u ? -1.0F : 1.0F;
  return exponent == 0 ? sign * std::ldexp(static_cast<float>(mantissa), -24)
      : exponent == 31 ? (mantissa ? std::numeric_limits<float>::quiet_NaN() : sign * std::numeric_limits<float>::infinity())
      : sign * std::ldexp(1.0F + mantissa / 1024.0F, static_cast<int>(exponent) - 15);
}

awj::ImageBuffer fixture(float peak_nits, std::array<float, 3> rgb = {1, 1, 1},
                         std::size_t width = 17, std::size_t height = 13) {
  awj::ImageBuffer image{.width = width, .height = height, .pixel_format = awj::PixelFormat::rgba,
      .bit_depth = 16, .sample_representation = awj::SampleRepresentation::ieee_half_float,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
          .color_primaries = 9, .transfer_characteristics = 8, .matrix_coefficients = 0,
          .color_range = 1, .has_hdr_metadata = true, .color_metadata_source = "test-bt2020-linear",
          .linear_reference_white_nits = peak_nits > 0 ? peak_nits : 80.0F}};
  awj::ImagePlane plane{.stride = width * 8 + 16};
  plane.bytes.resize(plane.stride * height, std::byte{0xa5});
  for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
    for (std::size_t c = 0; c < 4; ++c) {
      const auto half = ultrahdr::floatToHalf(c == 3 ? 1.0F : peak_nits > 0 ? rgb[c] : 0.0F);
      std::memcpy(plane.bytes.data() + y * plane.stride + (x * 4 + c) * 2, &half, 2);
    }
  }
  image.planes.push_back(std::move(plane));
  return image;
}

using Decoder = std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>;

std::expected<Decoder, std::string> probe(std::span<const std::byte> bytes) {
  Decoder decoder(uhdr_create_decoder(), uhdr_release_decoder);
  if (!decoder) return std::unexpected{"Cannot create ISO JPEG decoder."};
  uhdr_compressed_image_t data{const_cast<std::byte*>(bytes.data()), bytes.size(), bytes.size(),
      UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
  auto status = uhdr_dec_set_image(decoder.get(), &data);
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_dec_probe(decoder.get());
  if (status.error_code != UHDR_CODEC_OK) return std::unexpected{std::string("Cannot probe ISO JPEG: ") + status.detail};
  return decoder;
}

std::expected<std::vector<float>, std::string> reconstruct(std::span<const std::byte> bytes, float display_boost) {
  // probe 会锁定输出 setter；使用新上下文，在 decode 内部 probe 之前完成配置。
  Decoder decoder(uhdr_create_decoder(), uhdr_release_decoder);
  if (!decoder) return std::unexpected{"Cannot create ISO JPEG decoder."};
  uhdr_compressed_image_t data{const_cast<std::byte*>(bytes.data()), bytes.size(), bytes.size(),
      UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
  auto status = uhdr_dec_set_image(decoder.get(), &data);
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat);
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR);
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_dec_set_out_max_display_boost(decoder.get(), display_boost);
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_decode(decoder.get());
  if (status.error_code != UHDR_CODEC_OK) return std::unexpected{std::string("Cannot reconstruct ISO JPEG: ") + status.detail};
  const auto* image = uhdr_get_decoded_image(decoder.get());
  if (!image || !image->planes[0] || image->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || image->stride[0] < image->w)
    return std::unexpected{"Invalid reconstructed HDR layout."};
  std::vector<float> rgb(static_cast<std::size_t>(image->w) * image->h * 3);
  for (unsigned y = 0; y < image->h; ++y) {
    const auto* row = static_cast<const std::uint16_t*>(image->planes[0]) + static_cast<std::size_t>(y) * image->stride[0] * 4;
    for (unsigned x = 0; x < image->w; ++x) for (unsigned c = 0; c < 3; ++c) {
      const auto value = half_reference(row[x * 4 + c]);
      if (!std::isfinite(value)) return std::unexpected{"Non-finite HDR reconstruction."};
      rgb[(static_cast<std::size_t>(y) * image->w + x) * 3 + c] = value;
    }
  }
  return rgb;
}

int check_reconstruction(std::span<const std::byte> bytes, const uhdr_gainmap_metadata_t& metadata) {
  auto full = reconstruct(bytes, std::numeric_limits<float>::max());
  auto at_capacity = reconstruct(bytes, metadata.hdr_capacity_max);
  auto beyond_capacity = reconstruct(bytes, metadata.hdr_capacity_max * 1.125F);
  if (!full || !at_capacity || !beyond_capacity) return fail(!full ? full.error() : !at_capacity ? at_capacity.error() : beyond_capacity.error());
  if (full->size() != at_capacity->size() || full->size() != beyond_capacity->size()) return fail("Display models changed reconstruction dimensions.");
  for (std::size_t i = 0; i < full->size(); ++i) {
    const auto tolerance = 1.0e-4F * std::max(1.0e-6F, std::abs((*full)[i]));
    if (std::abs((*full)[i] - (*at_capacity)[i]) > tolerance ||
        std::abs((*full)[i] - (*beyond_capacity)[i]) > tolerance) return fail("Display at/above written capacity differs from full HDR by more than relative 1e-4.");
  }
  // Also check the ISO display-headroom weight independently of the decoder's
  // lookup table. Lossy JPEG-vs-original error is deliberately not conflated
  // with the full-vs-limited display-model comparison above.
  auto model_metadata = metadata;
  ultrahdr::uhdr_gainmap_metadata_ext_t extended(model_metadata, ultrahdr::kJpegrVersion);
  const auto weight = std::clamp((std::log2(metadata.hdr_capacity_max) - std::log2(metadata.hdr_capacity_min)) /
      (std::log2(metadata.hdr_capacity_max) - std::log2(metadata.hdr_capacity_min)), 0.0F, 1.0F);
  if (!std::isfinite(weight) || weight != 1.0F) return fail("Written headroom has a zero/non-finite display denominator.");
  for (const float gain : {0.0F, 0.25F, 0.5F, 1.0F}) {
    const ultrahdr::Color base{{{0.02F, 0.18F, 0.7F}}}, map{{{gain, gain, gain}}};
    const auto full_model = ultrahdr::applyGain(base, map, &extended);
    const auto limited_model = ultrahdr::applyGain(base, map, &extended, weight);
    const std::array full_rgb{full_model.r, full_model.g, full_model.b};
    const std::array limited_rgb{limited_model.r, limited_model.g, limited_model.b};
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(full_rgb[c]) || !std::isfinite(limited_rgb[c]) ||
          std::abs(full_rgb[c] - limited_rgb[c]) > 1.0e-4F * std::max(1.0e-6F, std::abs(full_rgb[c])))
        return fail("Analytic full/limited gain reconstruction differs at capacity.");
    }
  }
  return 0;
}

int check_image(const awj::ImageBuffer& image, awj::NativeEncodeSettings settings) {
  auto encoded = awj::encode_hdr_jpeg(image, settings);
  if (!encoded) return fail(encoded.error());
  if (encoded->encoded.codec_name != "jpegli-iso21496-1" || encoded->diagnostics.integration_mode != "jpegli-iso21496-1" ||
      encoded->diagnostics.applied_hdr_metadata != "iso21496-1-gain-map" ||
      encoded->diagnostics.applied_color_primaries != 1 || encoded->diagnostics.applied_transfer_characteristics != 13 ||
      encoded->diagnostics.applied_bit_depth != 8) return fail("ISO JPEG external name/base color contract differs.");
  auto decoder = probe(encoded->encoded.bytes);
  if (!decoder) return fail(decoder.error());
  const auto* metadata = uhdr_dec_get_gainmap_metadata(decoder->get());
  if (!metadata || !awj::jpeg_hdr_detail::valid_gain_metadata(*metadata) ||
      metadata->hdr_capacity_min != 1.0F || metadata->hdr_capacity_max <= 1.0F) return fail("Serialized ISO JPEG lacks finite, strictly positive headroom.");
  // Fixtures are BT.2020 already, so source->canonical is an exact identity.
  float actual_peak{};
  for (std::size_t y = 0; y < image.height; ++y) for (std::size_t x = 0; x < image.width; ++x) for (std::size_t c = 0; c < 3; ++c) {
    std::uint16_t half{}; std::memcpy(&half, image.planes[0].bytes.data() + y * image.planes[0].stride + (x * 4 + c) * 2, 2);
    const auto nits = std::max(half_reference(half) * *image.source_info->linear_reference_white_nits, 0.0F);
    actual_peak = std::max({actual_peak, nits, half_reference(ultrahdr::floatToHalf(nits / 203.0F)) * 203.0F});
  }
  const auto expected_target = std::max(actual_peak, 203.0F * std::exp2(1.0F / 1024.0F));
  if (std::abs(metadata->hdr_capacity_max - expected_target / 203.0F) > 1.0e-5F * (expected_target / 203.0F)) {
    return fail("Capacity follows metadata/Y/theoretical 10000 nits instead of the actual canonical RGB peak.");
  }
  if (const auto result = check_reconstruction(encoded->encoded.bytes, *metadata)) return result;
  return 0;
}

awj::GainMapAlternative independent_sdr(std::size_t width, std::size_t height) {
  awj::ImageBuffer image{.width = width, .height = height, .pixel_format = awj::PixelFormat::rgba,
      .bit_depth = 8, .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba,
          .bit_depth = 8, .color_primaries = 1, .transfer_characteristics = 13,
          .matrix_coefficients = 0, .color_range = 1}};
  image.planes.push_back({std::vector<std::byte>(width * height * 4), width * 4});
  for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
    const auto rgb = x < width / 2 ? std::array{24u, 180u, 40u} : std::array{35u, 45u, 210u};
    auto* pixel = image.planes[0].bytes.data() + y * image.planes[0].stride + x * 4;
    for (std::size_t c = 0; c < 3; ++c) pixel[c] = std::byte(rgb[c]);
    pixel[3] = std::byte{255};
  }
  return {.image = std::move(image), .main_hdr_headroom_log2 = std::log2(1000.0F / 203.0F),
      .alternate_hdr_headroom_log2 = 0};
}
float rgb_distance(const awj::ImageBuffer& a, const awj::ImageBuffer& b) {
  if (a.width != b.width || a.height != b.height || a.bit_depth != 8 || b.bit_depth != 8 ||
      a.pixel_format != awj::PixelFormat::rgba || b.pixel_format != awj::PixelFormat::rgba)
    return std::numeric_limits<float>::infinity();
  float total{}; std::size_t count{};
  for (std::size_t y = 2; y + 2 < a.height; ++y) for (std::size_t x = 2; x + 2 < a.width; ++x) {
    if (x + 2 >= a.width / 2 && x < a.width / 2 + 2) continue;
    const auto* pa = a.planes[0].bytes.data() + y * a.planes[0].stride + x * 4;
    const auto* pb = b.planes[0].bytes.data() + y * b.planes[0].stride + x * 4;
    for (std::size_t c = 0; c < 3; ++c) {
      total += std::abs(std::to_integer<int>(pa[c]) - std::to_integer<int>(pb[c])); ++count;
    }
  }
  return count ? total / static_cast<float>(count) : std::numeric_limits<float>::infinity();
}
int check_existing_sdr_pair(awj::NativeEncodeSettings settings) {
  auto source = fixture(1000, {1, 1, 1}, 32, 24);
  auto alternative = independent_sdr(source.width, source.height);
  const auto source_bytes = source.planes[0].bytes, sdr_bytes = alternative.image.planes[0].bytes;
  auto prepared = awj::hdr::prepare_tone_map(source);
  if (!prepared) return fail(prepared.error());
  auto old_tone = awj::hdr::tone_map_to_sdr_srgb(source, *prepared, 8, {}, settings.resources);
  if (!old_tone) return fail(old_tone.error());
  if (rgb_distance(alternative.image, *old_tone) < 50)
    return fail("Existing-SDR fixture is too similar to a newly tone-mapped HDR base.");
  settings.quality = 100;
  settings.chroma_mode = awj::ChromaMode::yuv444;
  settings.gain_map_alternative = &alternative;
  awj::JpegImageDecoder decoder;
  for (const bool strip : {false, true}) {
    settings.strip_metadata = strip;
    auto encoded = awj::encode_hdr_jpeg(source, settings);
    if (!encoded) return fail(encoded.error());
    auto parsed = probe(encoded->encoded.bytes);
    if (!parsed || !uhdr_dec_get_gainmap_metadata(parsed->get()))
      return fail("Optional metadata stripping removed the necessary ISO JPEG pair.");
    auto low = decoder.decode_memory(encoded->encoded.bytes, "existing SDR JPEG base",
        {.copy_metadata_payloads = false, .gain_map_mode = awj::GainMapDecodeMode::sdr_only});
    auto high = decoder.decode_memory(encoded->encoded.bytes, "existing SDR JPEG HDR",
        {.copy_metadata_payloads = false, .gain_map_mode = awj::GainMapDecodeMode::hdr_only});
    auto both = decoder.decode_memory(encoded->encoded.bytes, "existing SDR JPEG pair",
        {.copy_metadata_payloads = false, .gain_map_mode = awj::GainMapDecodeMode::preserve_both});
    if (!low || !high || !both) return fail(!low ? low.error() : !high ? high.error() : both.error());
    if (low->gain_map_alternative || high->gain_map_alternative || !both->gain_map_alternative)
      return fail("JPEG rendition selection did not honor hdr_only/sdr_only/preserve_both.");
    if (!low->image.source_info || low->image.source_info->has_hdr_metadata ||
        low->image.source_info->color_primaries != 1 || low->image.source_info->transfer_characteristics != 13 ||
        !high->image.source_info || !high->image.source_info->has_hdr_metadata ||
        (high->image.source_info->color_primaries != 1 && high->image.source_info->color_primaries != 9 &&
         high->image.source_info->color_primaries != 12) || high->image.source_info->transfer_characteristics != 8 ||
        high->image.sample_representation != awj::SampleRepresentation::ieee_half_float)
      return fail("JPEG SDR-base/HDR-map CICP direction changed.");
    const auto& retained = *both->gain_map_alternative;
    if (retained.main_hdr_headroom_log2 <= retained.alternate_hdr_headroom_log2 ||
        retained.alternate_hdr_headroom_log2 != 0 ||
        retained.image.planes[0].bytes != low->image.planes[0].bytes ||
        both->image.planes[0].bytes != high->image.planes[0].bytes)
      return fail("JPEG preserve_both lost an endpoint or reversed ISO headroom.");
    if (rgb_distance(low->image, alternative.image) > 6 || rgb_distance(low->image, *old_tone) < 40)
      return fail("JPEG encoder replaced the source's green/blue SDR with a new gray tone map.");
    if (source.planes[0].bytes != source_bytes || alternative.image.planes[0].bytes != sdr_bytes)
      return fail("Borrowed HDR/SDR pair was modified by JPEG encoding.");
  }
  auto mismatched = alternative;
  --mismatched.image.width;
  settings.gain_map_alternative = &mismatched;
  if (awj::encode_hdr_jpeg(source, settings)) return fail("Mismatched source SDR was silently replaced by a new tone map.");
  return 0;
}

}  // namespace

int main() {
  awj::NativeEncodeSettings settings;
  settings.output_format = awj::OutputFormat::jpgli;
  settings.quality = 90;
  settings.chroma_mode = awj::ChromaMode::auto_keep;
  settings.resources = {.file_parallelism = 1, .encoder_threads_per_file = 2, .global_thread_budget = 2,
      .memory_limit_bytes = 0}; // Unlimited is not a zero-byte allowance.
  awj::jpeg_hdr_detail::EngineThreadLimit test_threads(1);
  if (const auto result = check_existing_sdr_pair(settings)) return result;
  for (const float peak : {0.0F, 80.0F, 100.0F, 203.0F, 215.0F, 1000.0F, 10000.0F}) {
    if (const auto result = check_image(fixture(peak), settings)) return result;
  }
  // Very saturated channels have low CIE Y but must retain full RGB capacity.
  for (const auto color : {std::array{1.0F, 0.0F, 0.0F}, std::array{0.0F, 1.0F, 0.0F}, std::array{0.0F, 0.0F, 1.0F}}) {
    auto source = fixture(1000, color);
    source.source_info->content_light = awj::HdrContentLightMetadata{100, 80};
    if (const auto result = check_image(source, settings)) return result;
  }
  for (const auto cll : {awj::HdrContentLightMetadata{0, 100}, awj::HdrContentLightMetadata{20, 100}, awj::HdrContentLightMetadata{65535, 500}}) {
    auto source = fixture(215); source.source_info->content_light = cll;
    if (const auto result = check_image(source, settings)) return result;
  }
  auto gray = fixture(203); gray.source_info->pixel_format = awj::PixelFormat::gray;
  auto gray_encoded = awj::encode_hdr_jpeg(gray, settings);
  if (!gray_encoded || gray_encoded->diagnostics.applied_chroma != "444") return fail("HDR gray auto was not frozen to base 444 before RGBA relabeling.");
  auto source_422 = fixture(215); source_422.source_info->pixel_format = awj::PixelFormat::yuv422;
  auto encoded_422 = awj::encode_hdr_jpeg(source_422, settings);
  if (!encoded_422 || encoded_422->diagnostics.applied_chroma != "444" || !encoded_422->diagnostics.rendition.tone_mapped)
    return fail("Generated SDR JPEG base did not use its RGB/444 rendition instead of HDR-source 422.");

  auto over = awj::encode_hdr_jpeg(fixture(10001), settings);
  if (over || over.error().find("10000") == std::string::npos) return fail("Out-of-engine-range HDR JPEG did not explicitly reject 10000-nit overflow.");
  for (const float invalid : {-1.0F, 10001.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    if (awj::jpeg_hdr_detail::capacity_target_peak(invalid)) return fail("Invalid canonical capacity peak was accepted.");
  }
  auto bad = fixture(203); const std::uint16_t nan = 0x7e00u; std::memcpy(bad.planes[0].bytes.data(), &nan, 2);
  if (awj::encode_hdr_jpeg(bad, settings)) return fail("Non-finite canonical input was accepted.");
  std::stop_source stopped; stopped.request_stop();
  if (awj::encode_hdr_jpeg(fixture(203), settings, stopped.get_token())) return fail("HDR JPEG ignored cancellation.");

  // A dynamic admission ticket can reserve most of the global budget for one
  // image, even when file_parallelism is high. The old limit/parallelism gate
  // rejected this case despite the shared whole-plan reservation fitting.
  auto admitted = fixture(215, {1, 1, 1}, 128, 129);
  auto admitted_settings = settings;
  admitted_settings.resources = {.file_parallelism = 128, .encoder_threads_per_file = 1, .global_thread_budget = 128,
      .memory_limit_bytes = awj::hdr_cost::jpeg_working_set_bytes(admitted.width, admitted.height, 0, 1,
          admitted.planes[0].bytes.size())};
  if (const auto result = check_image(admitted, admitted_settings)) return result;
  auto tight = settings; tight.resources.memory_limit_bytes = 1;
  if (awj::encode_hdr_jpeg(fixture(203), tight)) return fail("Impossible HDR JPEG working-set limit was ignored.");

  uhdr_gainmap_metadata_t invalid{};
  std::fill_n(invalid.min_content_boost, 3, 1.0F); std::fill_n(invalid.max_content_boost, 3, 2.0F);
  std::fill_n(invalid.gamma, 3, 1.0F); invalid.hdr_capacity_min = 1.0F; invalid.hdr_capacity_max = 2.0F;
  if (!awj::jpeg_hdr_detail::valid_gain_metadata(invalid)) return fail("Valid metadata control rejected.");
  invalid.hdr_capacity_max = invalid.hdr_capacity_min;
  if (awj::jpeg_hdr_detail::valid_gain_metadata(invalid)) return fail("Degenerate capacity denominator was accepted.");
  invalid.hdr_capacity_max = std::numeric_limits<float>::infinity();
  if (awj::jpeg_hdr_detail::valid_gain_metadata(invalid)) return fail("Infinite capacity was accepted.");
  invalid.hdr_capacity_max = 2.0F; invalid.gamma[1] = 0;
  if (awj::jpeg_hdr_detail::valid_gain_metadata(invalid)) return fail("Zero metadata gamma denominator was accepted.");
  return 0;
}
