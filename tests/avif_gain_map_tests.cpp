#include <avif/avif.h>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import awj.avif_aom_codec;
import awj.codec;
import awj.config;
import awj.image;
import awj.large_image_plan;
import awj.resource_planner;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class T> T take(std::expected<T, std::string> result) {
  if (!result) throw std::runtime_error(result.error());
  return std::move(*result);
}
awj::ImageBuffer high_image() {
  awj::ImageBuffer image{.width = 128, .height = 64, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 16,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
          .color_primaries = 9, .transfer_characteristics = 16, .matrix_coefficients = 0,
          .color_range = 1, .has_hdr_metadata = true}};
  image.planes = {{std::vector<std::byte>(128 * 64 * 8), 128 * 8}};
  for (std::size_t y = 0; y < 64; ++y) for (std::size_t x = 0; x < 128; ++x)
    for (std::size_t c = 0; c < 4; ++c) {
      const std::uint16_t value = c == 3 ? static_cast<std::uint16_t>((y % 8) * 8192) : 50000;
      std::memcpy(image.planes[0].bytes.data() + (y * 128 * 4 + x * 4 + c) * 2, &value, 2);
    }
  return image;
}
awj::GainMapAlternative low_image() {
  awj::GainMapAlternative alternative{.image = {.width = 128, .height = 64,
      .pixel_format = awj::PixelFormat::rgba, .alpha_mode = awj::AlphaMode::straight,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 8,
          .color_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 0, .color_range = 1}},
      .main_hdr_headroom_log2 = 3, .alternate_hdr_headroom_log2 = 0};
  alternative.image.planes = {{std::vector<std::byte>(128 * 64 * 4), 128 * 4}};
  for (std::size_t y = 0; y < 64; ++y) for (std::size_t x = 0; x < 128; ++x) {
    auto* p = alternative.image.planes[0].bytes.data() + (y * 128 + x) * 4;
    p[0] = std::byte{32}; p[1] = std::byte(x < 64 ? 180 : 40);
    p[2] = std::byte(x < 64 ? 48 : 180); p[3] = std::byte((y % 8) * 32);
  }
  return alternative;
}
void check_low(const awj::ImageBuffer& low, const awj::ImageBuffer& reference) {
  require(low.width == 128 && low.height == 64 && low.bit_depth == 8 &&
      low.source_info && !low.source_info->has_hdr_metadata &&
      low.source_info->color_primaries == 1 && low.source_info->transfer_characteristics == 13,
      "AVIF did not preserve the SDR rendition interpretation");
  // 远离增益图缩放边界，分别核对两个不同 cell，不能把同一张辅助图重复铺满。
  for (const std::size_t x : {16u, 48u, 80u, 112u}) for (const std::size_t y : {17u, 25u, 41u, 49u}) {
    const auto* a = low.planes[0].bytes.data() + y * low.planes[0].stride + x * 4;
    const auto* b = reference.planes[0].bytes.data() + y * reference.planes[0].stride + x * 4;
    for (int c = 0; c < 3; ++c)
      require(std::abs(std::to_integer<int>(a[c]) - std::to_integer<int>(b[c])) <= 18,
          "AVIF regenerated gain map did not reconstruct its original SDR endpoint");
  }
}
void run_case(bool grid, bool strip, bool alpha) {
  auto high = high_image();
  auto alternative = low_image();
  const auto high_before = high.planes[0].bytes, low_before = alternative.image.planes[0].bytes;
  awj::NativeEncodeSettings settings{.output_format = awj::OutputFormat::avif,
      .quality = 100, .speed = 10, .bit_depth = 12, .chroma_mode = awj::ChromaMode::yuv444,
      .avif_color_representation = awj::AvifColorRepresentation::rgb_identity};
  settings.applied_color_primaries = 9; settings.applied_transfer_characteristics = 16;
  settings.applied_matrix_coefficients = 0; settings.applied_color_range = 1;
  settings.alpha_policy = alpha ? awj::AlphaModePolicy::automatic : awj::AlphaModePolicy::off;
  settings.applied_alpha = alpha ? "kept" : "dropped";
  settings.source_has_alpha_channel = true;
  settings.encoder_supports_alpha = true;
  settings.strip_metadata = strip;
  settings.resources = {.file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1};
  settings.gain_map_alternative = &alternative;
  if (grid) settings.avif_grid_plan = awj::GridPlan{.cols = 2, .rows = 1,
      .tile_width = 64, .tile_height = 64, .padded_width = 128, .padded_height = 64};
  auto encoder = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
  require(bool(encoder), "Missing AVIF encoder");
  auto encoded = take(encoder->encode(high, settings));
  require(!encoded.lossless && encoded.diagnostics.applied_hdr_metadata == "iso21496-1-gain-map-hdr-base",
      "AVIF incorrectly claimed whole-pair losslessness or omitted paired diagnostics");
  std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> parsed(avifDecoderCreate(), &avifDecoderDestroy);
  require(bool(parsed), "Missing libavif parser");
  parsed->imageContentToDecode = AVIF_IMAGE_CONTENT_COLOR_AND_ALPHA | AVIF_IMAGE_CONTENT_GAIN_MAP;
  require(avifDecoderSetIOMemory(parsed.get(), reinterpret_cast<const std::uint8_t*>(encoded.encoded.bytes.data()),
      encoded.encoded.bytes.size()) == AVIF_RESULT_OK && avifDecoderParse(parsed.get()) == AVIF_RESULT_OK,
      "Paired AVIF container parse failed");
  const auto& image = *parsed->image;
  require(image.gainMap && image.colorPrimaries == 9 && image.transferCharacteristics == 16,
      "AVIF container did not retain HDR base color or a functional map");
  if (image.gainMap->baseHdrHeadroom.n != 3 * image.gainMap->baseHdrHeadroom.d ||
      image.gainMap->alternateHdrHeadroom.n != 0 || image.gainMap->altPlaneCount != 3 ||
      (parsed->alphaPresent != AVIF_FALSE) != alpha) {
    std::fprintf(stderr, "grid=%d strip=%d alpha=%d B=%u/%u A=%u/%u planes=%u\n", grid, strip, alpha,
        image.gainMap->baseHdrHeadroom.n, image.gainMap->baseHdrHeadroom.d,
        image.gainMap->alternateHdrHeadroom.n, image.gainMap->alternateHdrHeadroom.d, image.gainMap->altPlaneCount);
    throw std::runtime_error("AVIF container changed reverse headroom or alpha semantics");
  }
  auto decoder = awj::make_avif_image_decoder(1);
  auto both = take(decoder->decode_memory(encoded.encoded.bytes, "paired AVIF",
      {.copy_metadata_payloads = false, .gain_map_mode = awj::GainMapDecodeMode::preserve_both}));
  auto hdr = take(decoder->decode_memory(encoded.encoded.bytes, "HDR-only AVIF",
      {.gain_map_mode = awj::GainMapDecodeMode::hdr_only}));
  auto sdr = take(decoder->decode_memory(encoded.encoded.bytes, "SDR-only AVIF",
      {.gain_map_mode = awj::GainMapDecodeMode::sdr_only}));
  require(both.gain_map_alternative && !hdr.gain_map_alternative && !sdr.gain_map_alternative &&
      both.image.planes[0].bytes == hdr.image.planes[0].bytes &&
      both.gain_map_alternative->image.planes[0].bytes == sdr.image.planes[0].bytes,
      "AVIF mode or optional metadata stripping lost a functional endpoint");
  require(both.image.source_info && both.image.source_info->color_primaries == 9 &&
      both.image.source_info->transfer_characteristics == 16, "AVIF rewrote an existing HDR base");
  check_low(sdr.image, alternative.image);
  require(high.planes[0].bytes == high_before && alternative.image.planes[0].bytes == low_before,
      "AVIF modified borrowed endpoint buffers");
  alternative.image.width = 127;
  require(!encoder->encode(high, settings), "AVIF silently discarded an invalid alternate endpoint");
}
}
int main() {
  try {
    for (const bool grid : {false, true}) for (const bool strip : {false, true})
      for (const bool alpha : {false, true}) run_case(grid, strip, alpha);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
