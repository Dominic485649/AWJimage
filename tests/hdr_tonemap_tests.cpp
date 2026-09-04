#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

import awj.hdr_tonemap;
import awj.image;

namespace {

int fail(std::string_view message) {
  std::cerr << message << '\n';
  return 1;
}

void append_binary16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(std::byte{static_cast<unsigned char>(value & 0xffu)});
  bytes.push_back(std::byte{static_cast<unsigned char>(value >> 8u)});
}

awj::ImageBuffer make_scrgb_image() {
  awj::ImagePlane plane{.stride = 8};
  // 0.5, -0.25, 2.0, 1.0: scRGB must accept both negative and extended values.
  append_binary16(plane.bytes, 0x3800u);
  append_binary16(plane.bytes, 0xb400u);
  append_binary16(plane.bytes, 0x4000u);
  append_binary16(plane.bytes, 0x3c00u);
  awj::ImageBuffer image{.width = 1,
                          .height = 1,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 16,
                          .sample_representation = awj::SampleRepresentation::ieee_half_float,
                          .source_info = awj::ImageSourceInfo{
                              .pixel_format = awj::PixelFormat::rgba,
                              .bit_depth = 16,
                              .color_primaries = 1,
                              .transfer_characteristics = 8,
                              .matrix_coefficients = 0,
                              .color_range = 1,
                              .has_hdr_metadata = true,
                              .color_metadata_source = "wic-scrgb-half-linear"}};
  image.planes.push_back(std::move(plane));
  return image;
}

}  // namespace

int main() {
  auto source = make_scrgb_image();
  auto has_hdr = awj::hdr::has_explicit_hdr_signal(source);
  if (!has_hdr || !*has_hdr) return fail("scRGB HDR signal was not recognized.");

  auto sdr = awj::hdr::tone_map_to_sdr_srgb(source);
  if (!sdr || sdr->bit_depth != 8 ||
      sdr->sample_representation != awj::SampleRepresentation::unorm ||
      !sdr->source_info ||
      sdr->source_info->transfer_characteristics.value_or(-1) != 13 ||
      sdr->planes.size() != 1 || sdr->planes.front().bytes.size() != 4 ||
      sdr->planes.front().bytes[3] != std::byte{255}) {
    return fail(sdr ? "scRGB tone mapping output metadata or alpha is invalid."
                    : sdr.error());
  }

  auto hdr10 = awj::hdr::materialize_scrgb_as_hdr10(source);
  if (!hdr10 || hdr10->bit_depth != 16 ||
      hdr10->sample_representation != awj::SampleRepresentation::unorm ||
      !hdr10->source_info ||
      hdr10->source_info->color_primaries.value_or(-1) != 9 ||
      hdr10->source_info->transfer_characteristics.value_or(-1) != 16 ||
      hdr10->source_info->matrix_coefficients.value_or(-1) != 9) {
    return fail(hdr10 ? "scRGB HDR materialization metadata is invalid."
                      : hdr10.error());
  }

  auto ambiguous = source;
  ambiguous.source_info->color_metadata_source.clear();
  if (awj::hdr::has_explicit_hdr_signal(ambiguous)) {
    return fail("ambiguous floating point HDR input was accepted.");
  }
  return 0;
}
