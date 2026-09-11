#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lcms2.h>

import awj.avif_aom_codec;
import awj.codec;
import awj.config;
import awj.image;
import awj.hdr_tonemap;
import awj.large_image_plan;
import awj.resource_planner;

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

awj::ImageBuffer make_test_image() {
  awj::ImagePlane plane{.stride = 8};
  plane.bytes = {
      std::byte{255}, std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{0},   std::byte{255}, std::byte{255},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
  };
  awj::ImageBuffer image{.width = 2,
                          .height = 2,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

void append_binary16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(std::byte{static_cast<unsigned char>(value & 0xffu)});
  bytes.push_back(std::byte{static_cast<unsigned char>(value >> 8u)});
}

awj::ImageBuffer make_scrgb_image() {
  awj::ImagePlane plane{.stride = 8};
  append_binary16(plane.bytes, 0x3800u);  // 0.5
  append_binary16(plane.bytes, 0xb400u);  // -0.25
  append_binary16(plane.bytes, 0x4000u);  // 2.0
  append_binary16(plane.bytes, 0x3c00u);  // 1.0
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

awj::ImageBuffer make_grid_test_image(std::uint32_t width = 128,
                                      std::uint32_t height = 128) {
  awj::ImagePlane plane{.stride = width * 4};
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      plane.bytes.push_back(std::byte{static_cast<unsigned char>((x * 2) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>((y * 2) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>((x + y) & 0xffu)});
      plane.bytes.push_back(std::byte{255});
    }
  }
  awj::ImageBuffer image{.width = width,
                          .height = height,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::none,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

awj::ImageBuffer make_alpha_quality_test_image() {
  constexpr std::uint32_t width = 32;
  constexpr std::uint32_t height = 32;
  awj::ImagePlane plane{.stride = width * 4};
  plane.bytes.reserve(static_cast<std::size_t>(plane.stride) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      plane.bytes.push_back(std::byte{static_cast<unsigned char>(
          (x * 17u + y * 31u + 5u) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>(
          (x * 97u + y * 11u + x * y * 3u) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>(
          (x * 7u + y * 89u + x * y * 13u) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>(
          1u + ((x * 73u + y * 151u + x * y * 19u) % 254u))});
    }
  }
  awj::ImageBuffer image{.width = width,
                          .height = height,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

bool rgba_alpha_differs(const awj::ImageBuffer& source,
                        const awj::ImageBuffer& decoded) {
  if (source.pixel_format != awj::PixelFormat::rgba ||
      decoded.pixel_format != awj::PixelFormat::rgba ||
      source.bit_depth != 8 || decoded.bit_depth != 8 ||
      source.width != decoded.width || source.height != decoded.height ||
      source.planes.empty() || decoded.planes.empty()) {
    return false;
  }
  const auto& source_plane = source.planes.front();
  const auto& decoded_plane = decoded.planes.front();
  constexpr std::size_t channels = 4;
  if (source_plane.stride < source.width * channels ||
      decoded_plane.stride < decoded.width * channels ||
      source_plane.bytes.size() < source_plane.stride * source.height ||
      decoded_plane.bytes.size() < decoded_plane.stride * decoded.height) {
    return false;
  }
  for (std::size_t y = 0; y < source.height; ++y) {
    for (std::size_t x = 0; x < source.width; ++x) {
      const auto source_offset = y * source_plane.stride + x * channels + 3;
      const auto decoded_offset = y * decoded_plane.stride + x * channels + 3;
      if (source_plane.bytes[source_offset] != decoded_plane.bytes[decoded_offset]) {
        return true;
      }
    }
  }
  return false;
}

std::vector<std::byte> make_srgb_icc_profile() {
  cmsHPROFILE profile = cmsCreate_sRGBProfile();
  if (profile == nullptr) {
    return {};
  }
  cmsUInt32Number size = 0;
  if (!cmsSaveProfileToMem(profile, nullptr, &size) || size == 0) {
    cmsCloseProfile(profile);
    return {};
  }
  std::vector<std::byte> bytes(size);
  const bool saved = cmsSaveProfileToMem(profile, bytes.data(), &size) != 0;
  cmsCloseProfile(profile);
  if (!saved) {
    return {};
  }
  bytes.resize(size);
  return bytes;
}

awj::ImageBuffer make_metadata_test_image(std::vector<std::byte> icc) {
  auto image = make_test_image();
  image.metadata = {
      awj::MetadataBlock{.kind = awj::MetadataKind::icc,
                         .bytes = std::move(icc)},
      awj::MetadataBlock{.kind = awj::MetadataKind::exif,
                         .bytes = {std::byte{0}, std::byte{0},
                                   std::byte{0}, std::byte{0},
                                   std::byte{'I'}, std::byte{'I'},
                                   std::byte{42}, std::byte{0},
                                   std::byte{8}, std::byte{0},
                                   std::byte{0}, std::byte{0},
                                   std::byte{0}, std::byte{0},
                                   std::byte{0}, std::byte{0},
                                   std::byte{0}, std::byte{0}}},
      awj::MetadataBlock{.kind = awj::MetadataKind::xmp,
                         .bytes = {std::byte{'<'}, std::byte{'x'},
                                   std::byte{'/'}, std::byte{'>'}}},
  };
  return image;
}

bool has_metadata(const awj::ImageBuffer& image, awj::MetadataKind kind) {
  for (const auto& block : image.metadata) {
    if (block.kind == kind && !block.bytes.empty()) {
      return true;
    }
  }
  return false;
}

awj::NativeEncodeSettings settings(int bit_depth,
                                    awj::ChromaMode chroma,
                                    awj::AvifEncoderMode encoder) {
  return awj::NativeEncodeSettings{.output_format = awj::OutputFormat::avif,
                                    .quality = 80,
                                    .speed = 6,
                                    .speed_explicit = true,
                                    .bit_depth = bit_depth,
                                    .chroma_mode = chroma,
                                    .avif_encoder = encoder,
                                    .resources = awj::ResourcePlan{
                                        .file_parallelism = 1,
                                        .encoder_threads_per_file = 1,
                                        .global_thread_budget = 1}};
}

int verify_preferred_decoder(const awj::ImageDecodeResult& decoded) {
  const bool dav1d_available = awj::avif_dav1d_decoder_available();
  const std::string_view expected_id = dav1d_available ? "libavif-dav1d" : "libavif-aom";
  if (decoded.decoder_id != expected_id ||
      decoded.used_fallback != !dav1d_available) {
    return fail("AVIF decoder did not prefer dav1d with AOM fallback.");
  }
  return 0;
}

int decode_bytes_to_temp(const awj::EncodedImage& encoded,
                         const std::filesystem::path& temp,
                         std::uint32_t expected_width,
                         std::uint32_t expected_height,
                         int expected_rgba_bit_depth,
                         int expected_source_bit_depth) {
  {
    std::ofstream output{temp, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded.bytes.size()));
  }
  auto decoder = awj::make_avif_image_decoder(1);
  auto decoded = decoder->decode(temp);
  std::error_code ec;
  std::filesystem::remove(temp, ec);
  if (!decoded || decoded->image.width != expected_width ||
      decoded->image.height != expected_height ||
      decoded->image.pixel_format != awj::PixelFormat::rgba ||
      decoded->image.bit_depth != expected_rgba_bit_depth ||
      !decoded->image.source_info ||
      decoded->image.source_info->bit_depth != expected_source_bit_depth) {
    if (decoded) {
      return fail(std::format("AVIF decode result invalid: got {}x{} {}-bit source {}-bit.",
                              decoded->image.width, decoded->image.height,
                              decoded->image.bit_depth,
                              decoded->image.source_info
                                  ? decoded->image.source_info->bit_depth
                                  : 0));
    }
    return fail(decoded.error());
  }
  if (const int rc = verify_preferred_decoder(*decoded); rc != 0) {
    return rc;
  }
  return 0;
}

int verify_avif_source_info(const awj::EncodedImage& encoded,
                            awj::PixelFormat expected_chroma,
                            int expected_color_range) {
  auto decoder = awj::make_avif_image_decoder(1);
  auto decoded = decoder->decode_memory(encoded.bytes, "AVIF metadata regression");
  if (!decoded || !decoded->image.source_info ||
      decoded->image.source_info->pixel_format != expected_chroma ||
      decoded->image.source_info->color_range.value_or(-1) != expected_color_range) {
    return fail(decoded ? "AVIF chroma or color range metadata was not preserved."
                        : decoded.error());
  }
  if (const int rc = verify_preferred_decoder(*decoded); rc != 0) {
    return rc;
  }
  return 0;
}

int verify_avif_matrix(const awj::EncodedImage& encoded, int expected_matrix) {
  auto decoder = awj::make_avif_image_decoder(1);
  auto decoded = decoder->decode_memory(encoded.bytes, "AVIF color representation regression");
  if (!decoded || !decoded->image.source_info ||
      decoded->image.source_info->matrix_coefficients.value_or(-1) !=
          expected_matrix) {
    return fail(decoded ? "AVIF matrix coefficients did not match the requested color representation."
                        : decoded.error());
  }
  if (const int rc = verify_preferred_decoder(*decoded); rc != 0) {
    return rc;
  }
  return 0;
}

}  // namespace

int main() {
  auto aom = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);

  auto scrgb_materialized = awj::hdr::materialize_scrgb_as_hdr10(make_scrgb_image());
  if (!scrgb_materialized || scrgb_materialized->bit_depth != 16 ||
      scrgb_materialized->sample_representation != awj::SampleRepresentation::unorm) {
    return fail(scrgb_materialized
                    ? "scRGB materialization no longer uses a 16-bit UNORM input container."
                    : scrgb_materialized.error());
  }
  auto scrgb_12_settings = settings(12, awj::ChromaMode::yuv444,
                                    awj::AvifEncoderMode::aom);
  scrgb_12_settings.source_bit_depth = 16;
  scrgb_12_settings.requested_bit_depth = 16;
  scrgb_12_settings.bit_depth_reason =
      "源图 16-bit 超过 aom 支持上限，限制为 12-bit 输出";
  scrgb_12_settings.applied_color_primaries = 9;
  scrgb_12_settings.applied_transfer_characteristics = 16;
  scrgb_12_settings.applied_matrix_coefficients = 9;
  scrgb_12_settings.applied_color_range = 1;
  auto scrgb_12_encoded = aom->encode(*scrgb_materialized, scrgb_12_settings);
  if (!scrgb_12_encoded ||
      scrgb_12_encoded->diagnostics.applied_bit_depth != 12) {
    return fail(scrgb_12_encoded
                    ? "16-bit scRGB container was not encoded as 12-bit AVIF."
                    : scrgb_12_encoded.error());
  }
  auto scrgb_decoder = awj::make_avif_image_decoder(1);
  auto scrgb_12_decoded = scrgb_decoder->decode_memory(
      scrgb_12_encoded->encoded.bytes, "scRGB 16-bit container -> AVIF 12-bit regression");
  if (!scrgb_12_decoded || !scrgb_12_decoded->image.source_info ||
      scrgb_12_decoded->image.source_info->bit_depth != 12) {
    return fail(scrgb_12_decoded
                    ? "encoded scRGB regression fixture is not a real 12-bit AVIF."
                    : scrgb_12_decoded.error());
  }

  auto encoded = aom->encode(make_test_image(), settings(8, awj::ChromaMode::yuv444,
                                                         awj::AvifEncoderMode::aom));
  if (!encoded) {
    return fail(encoded.error());
  }
  if (encoded->encoded.bytes.empty() || encoded->encoded.codec_name != "libavif-aom") {
    return fail("AVIF AOM encoder did not produce bytes.");
  }
  if (encoded->diagnostics.encoder_id != "aom" ||
      encoded->diagnostics.applied_chroma != "444" ||
      encoded->diagnostics.speed_mapping.codec_key != "aom:cpu-used" ||
      encoded->diagnostics.speed_mapping.codec_value != 6) {
    return fail("AVIF AOM diagnostics invalid.");
  }

  auto default_speed_settings = settings(8, awj::ChromaMode::yuv420,
                                         awj::AvifEncoderMode::aom);
  default_speed_settings.speed_explicit = false;
  auto default_speed = aom->encode(make_test_image(), default_speed_settings);
  if (!default_speed || default_speed->diagnostics.speed_mapping.codec_key != "aom:cpu-used" ||
      default_speed->diagnostics.speed_mapping.codec_value != 5) {
    return fail(default_speed ? "AOM default speed diagnostics invalid."
                              : default_speed.error());
  }

  auto auto_8_image = make_test_image();
  auto auto_8_settings = default_speed_settings;
  auto_8_settings.bit_depth.reset();
  auto_8_settings.bit_depth_explicit = false;
  auto auto_8 = aom->encode(auto_8_image, auto_8_settings);
  if (!auto_8 || auto_8->diagnostics.applied_bit_depth != 10) {
    return fail(auto_8 ? "AOM auto 8-bit input did not use 10-bit output."
                       : auto_8.error());
  }

  auto auto_10_image = make_test_image();
  auto_10_image.source_info = awj::ImageSourceInfo{.bit_depth = 10};
  auto auto_10 = aom->encode(auto_10_image, auto_8_settings);
  if (!auto_10 || auto_10->diagnostics.applied_bit_depth != 10) {
    return fail(auto_10 ? "AOM auto 10-bit input did not retain 10-bit output."
                        : auto_10.error());
  }

  auto auto_12_image = make_test_image();
  auto_12_image.source_info = awj::ImageSourceInfo{.bit_depth = 12};
  auto auto_12 = aom->encode(auto_12_image, auto_8_settings);
  if (!auto_12 || auto_12->diagnostics.applied_bit_depth != 12) {
    return fail(auto_12 ? "AOM auto 12-bit input did not retain 12-bit output."
                        : auto_12.error());
  }

  auto lossless_auto_chroma_settings = auto_8_settings;
  lossless_auto_chroma_settings.quality = 100;
  lossless_auto_chroma_settings.chroma_mode = awj::ChromaMode::auto_keep;
  lossless_auto_chroma_settings.applied_color_range = 1;
  const auto verify_auto_chroma = [&](awj::PixelFormat source_format,
                                      awj::PixelFormat expected_format,
                                      std::string_view expected_chroma) {
    auto image = make_test_image();
    image.source_info = awj::ImageSourceInfo{
        .pixel_format = source_format, .bit_depth = 8};
    auto encoded_auto = aom->encode(image, lossless_auto_chroma_settings);
    if (!encoded_auto || !encoded_auto->lossless ||
        encoded_auto->diagnostics.applied_chroma != expected_chroma) {
      return fail(encoded_auto ? "AOM auto chroma did not follow source metadata."
                               : encoded_auto.error());
    }
    return verify_avif_source_info(encoded_auto->encoded, expected_format, 1);
  };
  if (const int rc = verify_auto_chroma(awj::PixelFormat::yuv444,
                                        awj::PixelFormat::yuv444, "444");
      rc != 0) {
    return rc;
  }
  if (const int rc = verify_auto_chroma(awj::PixelFormat::yuv422,
                                        awj::PixelFormat::yuv422, "422");
      rc != 0) {
    return rc;
  }
  if (const int rc = verify_auto_chroma(awj::PixelFormat::rgb,
                                        awj::PixelFormat::yuv444, "444");
      rc != 0) {
    return rc;
  }
  if (const int rc = verify_auto_chroma(awj::PixelFormat::rgba,
                                        awj::PixelFormat::yuv444, "444");
      rc != 0) {
    return rc;
  }

  auto default_yuv_q100 = lossless_auto_chroma_settings;
  default_yuv_q100.avif_color_representation =
      awj::AvifColorRepresentation::yuv;
  auto default_yuv_q100_encoded =
      aom->encode(make_test_image(), default_yuv_q100);
  if (!default_yuv_q100_encoded || !default_yuv_q100_encoded->lossless ||
      default_yuv_q100_encoded->diagnostics.applied_color_representation !=
          "yuv") {
    return fail(default_yuv_q100_encoded
                    ? "default q100 AVIF did not retain the YUV representation."
                    : default_yuv_q100_encoded.error());
  }
  if (const int rc = verify_avif_matrix(default_yuv_q100_encoded->encoded, 1);
      rc != 0) {
    return rc;
  }

  auto rgb_identity = default_yuv_q100;
  rgb_identity.quality = 80;
  rgb_identity.chroma_mode = awj::ChromaMode::yuv444;
  rgb_identity.avif_color_representation =
      awj::AvifColorRepresentation::rgb_identity;
  auto rgb_identity_encoded = aom->encode(make_test_image(), rgb_identity);
  if (!rgb_identity_encoded ||
      rgb_identity_encoded->diagnostics.applied_color_representation !=
          "rgb" ||
      rgb_identity_encoded->diagnostics.applied_chroma != "444") {
    return fail(rgb_identity_encoded
                    ? "RGB Identity AVIF did not force the expected representation."
                    : rgb_identity_encoded.error());
  }
  if (const int rc = verify_avif_matrix(rgb_identity_encoded->encoded, 0);
      rc != 0) {
    return rc;
  }

  auto explicit_422_settings = auto_8_settings;
  explicit_422_settings.chroma_mode = awj::ChromaMode::yuv422;
  explicit_422_settings.applied_color_range = 1;
  auto explicit_422 = aom->encode(make_test_image(), explicit_422_settings);
  if (!explicit_422 || explicit_422->diagnostics.applied_chroma != "422") {
    return fail(explicit_422 ? "AOM explicit 422 request was not preserved."
                             : explicit_422.error());
  }
  if (const int rc = verify_avif_source_info(explicit_422->encoded,
                                             awj::PixelFormat::yuv422, 1);
      rc != 0) {
    return rc;
  }

  const auto verify_color_range = [&](int color_range) {
    auto range_settings = auto_8_settings;
    range_settings.chroma_mode = awj::ChromaMode::auto_keep;
    range_settings.applied_color_range = color_range;
    auto range_encoded = aom->encode(make_test_image(), range_settings);
    if (!range_encoded || range_encoded->diagnostics.applied_chroma != "420") {
      return fail(range_encoded ? "AOM default chroma was not 420."
                                : range_encoded.error());
    }
    return verify_avif_source_info(range_encoded->encoded,
                                   awj::PixelFormat::yuv420, color_range);
  };
  if (const int rc = verify_color_range(0); rc != 0) {
    return rc;
  }
  if (const int rc = verify_color_range(1); rc != 0) {
    return rc;
  }

  auto metadata_settings = auto_8_settings;
  metadata_settings.source_has_icc = true;
  metadata_settings.applied_icc = "kept";
  auto icc = make_srgb_icc_profile();
  if (icc.empty()) {
    return fail("Could not create the ICC profile fixture.");
  }
  auto metadata_encoded =
      aom->encode(make_metadata_test_image(std::move(icc)), metadata_settings);
  if (!metadata_encoded) {
    return fail(metadata_encoded.error());
  }
  auto metadata_decoder = awj::make_avif_image_decoder(1);
  auto metadata_decoded = metadata_decoder->decode_memory(
      metadata_encoded->encoded.bytes, "AVIF metadata regression",
      awj::DecodeOptions{.copy_metadata_payloads = true});
  if (!metadata_decoded) {
    return fail(metadata_decoded.error());
  }
  if (!has_metadata(metadata_decoded->image, awj::MetadataKind::icc)) {
    return fail("AVIF ICC metadata was not preserved by default.");
  }
  if (!has_metadata(metadata_decoded->image, awj::MetadataKind::exif)) {
    return fail("AVIF EXIF metadata was not preserved by default.");
  }
  if (!has_metadata(metadata_decoded->image, awj::MetadataKind::xmp)) {
    return fail("AVIF XMP metadata was not preserved by default.");
  }

  auto ten_bit = aom->encode(make_test_image(), settings(10, awj::ChromaMode::yuv420,
                                                         awj::AvifEncoderMode::aom));
  if (!ten_bit || ten_bit->encoded.bytes.empty() ||
      ten_bit->diagnostics.applied_bit_depth != 10) {
    return fail(ten_bit ? "AOM 10-bit diagnostics invalid." : ten_bit.error());
  }
  auto twelve_bit = aom->encode(make_test_image(), settings(12, awj::ChromaMode::yuv420,
                                                            awj::AvifEncoderMode::aom));
  if (!twelve_bit || twelve_bit->encoded.bytes.empty() ||
      twelve_bit->diagnostics.applied_bit_depth != 12) {
    return fail(twelve_bit ? "AOM 12-bit diagnostics invalid." : twelve_bit.error());
  }

  auto grid_settings = settings(8, awj::ChromaMode::yuv420,
                                awj::AvifEncoderMode::aom);
  grid_settings.resources = awj::ResourcePlan{.file_parallelism = 2,
                                              .encoder_threads_per_file = 4,
                                              .global_thread_budget = 4};
  grid_settings.alpha_policy = awj::AlphaModePolicy::off;
  grid_settings.avif_grid_plan = awj::GridPlan{.cols = 2,
                                               .rows = 2,
                                               .tile_width = 64,
                                               .tile_height = 64,
                                               .padded_width = 128,
                                               .padded_height = 128,
                                               .uses_padding = false,
                                               .clamped_to_original_size = false};
  auto grid_encoded = aom->encode(make_grid_test_image(), grid_settings);
  if (!grid_encoded || grid_encoded->encoded.bytes.empty() ||
      grid_encoded->diagnostics.integration_mode != "libavif-grid" ||
      grid_encoded->diagnostics.encoder_threads != 4) {
    return fail(grid_encoded ? "AOM grid diagnostics invalid." : grid_encoded.error());
  }

  auto alpha_settings = settings(8, awj::ChromaMode::auto_keep,
                                 awj::AvifEncoderMode::aom);
  alpha_settings.quality = 1;
  alpha_settings.resources.encoder_threads_per_file = 28;
  alpha_settings.resources.global_thread_budget = 28;
  alpha_settings.source_has_alpha_channel = true;
  alpha_settings.encoder_supports_alpha = true;
  alpha_settings.applied_alpha = "kept";
  const auto alpha_image = make_alpha_quality_test_image();
  auto alpha_encoded = aom->encode(alpha_image, alpha_settings);
  if (!alpha_encoded || alpha_encoded->encoded.bytes.empty() ||
      alpha_encoded->lossless || alpha_encoded->final_quality != 1 ||
      alpha_encoded->diagnostics.applied_chroma != "420" ||
      alpha_encoded->diagnostics.speed_mapping.user_speed != 6 ||
      alpha_encoded->diagnostics.encoder_threads != 28) {
    return fail(alpha_encoded ? "AOM alpha encode produced no bytes." : alpha_encoded.error());
  }
  auto alpha_decoder = awj::make_avif_image_decoder(1);
  auto alpha_decoded = alpha_decoder->decode_memory(alpha_encoded->encoded.bytes,
                                                     "AOM alpha regression");
  if (!alpha_decoded || alpha_decoded->image.alpha_mode == awj::AlphaMode::none ||
      !rgba_alpha_differs(alpha_image, alpha_decoded->image)) {
    return fail(alpha_decoded ? "AOM alpha did not follow the requested lossy quality."
                              : alpha_decoded.error());
  }

  auto auto_alpha_settings = alpha_settings;
  auto_alpha_settings.bit_depth.reset();
  auto_alpha_settings.bit_depth_explicit = false;
  auto auto_alpha_encoded = aom->encode(alpha_image, auto_alpha_settings);
  if (!auto_alpha_encoded || auto_alpha_encoded->lossless ||
      auto_alpha_encoded->final_quality != 1 ||
      auto_alpha_encoded->diagnostics.applied_chroma != "420" ||
      auto_alpha_encoded->diagnostics.applied_bit_depth != 10 ||
      auto_alpha_encoded->diagnostics.timing.avif_rgb_to_yuv_seconds < 0.0 ||
      auto_alpha_encoded->diagnostics.timing.avif_add_image_seconds < 0.0 ||
      auto_alpha_encoded->diagnostics.timing.avif_finish_seconds < 0.0 ||
      auto_alpha_encoded->diagnostics.timing.avif_output_copy_seconds < 0.0) {
    return fail(auto_alpha_encoded
                    ? "AOM automatic alpha path did not retain lossy 10-bit diagnostics."
                    : auto_alpha_encoded.error());
  }
  alpha_decoded = alpha_decoder->decode_memory(auto_alpha_encoded->encoded.bytes,
                                                "AOM automatic alpha regression");
  if (!alpha_decoded || alpha_decoded->image.alpha_mode == awj::AlphaMode::none) {
    return fail(alpha_decoded ? "AOM automatic alpha channel was not retained."
                              : alpha_decoded.error());
  }

  auto edge_grid_settings = settings(8, awj::ChromaMode::auto_keep,
                                     awj::AvifEncoderMode::aom);
  edge_grid_settings.alpha_policy = awj::AlphaModePolicy::off;
  edge_grid_settings.avif_grid_plan = awj::GridPlan{
      .cols = 2, .rows = 2, .tile_width = 65, .tile_height = 64,
      .padded_width = 130, .padded_height = 128, .uses_padding = true,
      .clamped_to_original_size = true};
  auto edge_grid_encoded = aom->encode(make_grid_test_image(129, 127),
                                       edge_grid_settings);
  if (edge_grid_encoded ||
      edge_grid_encoded.error().find("420") == std::string::npos) {
    return fail("AOM edge grid auto mode did not reject incompatible 420 chroma.");
  }

  const auto temp_dir = std::filesystem::temp_directory_path();
  if (const int rc = decode_bytes_to_temp(encoded->encoded,
                                          temp_dir / "avif-aom-codec-test.avif",
                                          2, 2, 8, 8);
      rc != 0) {
    return rc;
  }
  if (const int rc = decode_bytes_to_temp(ten_bit->encoded,
                                          temp_dir / "avif-aom-codec-test-10.avif",
                                          2, 2, 16, 10);
      rc != 0) {
    return rc;
  }
  if (const int rc = decode_bytes_to_temp(twelve_bit->encoded,
                                          temp_dir / "avif-aom-codec-test-12.avif",
                                          2, 2, 16, 12);
      rc != 0) {
    return rc;
  }
  if (const int rc = decode_bytes_to_temp(grid_encoded->encoded,
                                          temp_dir / "avif-aom-codec-test-grid.avif",
                                          128, 128, 8, 8);
      rc != 0) {
    return rc;
  }
  return 0;
}
