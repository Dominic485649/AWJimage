#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <vector>

#include <avif/avif.h>

import awj.image;
import awj.pixel_conversion;
import awj.hdr_tonemap;
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

} // namespace

int main() {
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
  auto cropped = awj::convert_image_pixels(view, awj::PixelConversionRequest{});
  if (!cropped || cropped->planes[0].bytes[0] != source.planes[0].bytes[24] ||
      cropped->planes[0].bytes[4] != source.planes[0].bytes[28]) return fail("Fused crop/resize used incorrect samples.");
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
  std::stop_source stop; stop.request_stop();
  if (awj::convert_image_pixels(source, pq, stop.get_token())) return fail("Cancellation was ignored.");
  auto planes = awj::yuv_plane_view(*fused);
  if (!planes || awj::copy_yuv_pixels(*planes, stop.get_token())) return fail("Native plane copy ignored cancellation.");
  std::puts("Native pixel conversion, odd dimensions, all layouts, alpha, HDR and cancellation passed.");
}
