#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>
#include <jxl/decode.h>

import awj.config;
import awj.core;
import awj.encoding_defaults;
import awj.avif_aom_codec;
import awj.image;
import awj.jpegli_codec;
import awj.jxl_codec;
import awj.native_backend;
import awj.resource_planner;
import awj.webp_codec;

namespace {

[[noreturn]] void terminate_test_process(int exit_code) noexcept {
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(exit_code);
}

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  terminate_test_process(1);
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream},
                     std::istreambuf_iterator<char>{}};
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

awj::ImageBuffer make_alpha_test_image() {
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

void push_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(std::byte{static_cast<unsigned char>(value & 0xffu)});
  bytes.push_back(std::byte{static_cast<unsigned char>(value >> 8)});
}

awj::ImageBuffer make_hdr_test_image() {
  awj::ImagePlane plane{.stride = 16};
  for (const std::uint16_t value : {std::uint16_t{0}, std::uint16_t{32768},
                                    std::uint16_t{65535}, std::uint16_t{65535},
                                    std::uint16_t{65535}, std::uint16_t{0},
                                    std::uint16_t{32768}, std::uint16_t{65535}}) {
    push_u16(plane.bytes, value);
  }
  awj::ImageBuffer image{.width = 2,
                          .height = 1,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 16,
                          .source_info = awj::ImageSourceInfo{
                              .pixel_format = awj::PixelFormat::rgba,
                              .bit_depth = 16,
                              .color_primaries = 9,
                              .transfer_characteristics = 16,
                              .matrix_coefficients = 0,
                              .color_range = 1,
                              .has_hdr_metadata = true,
                              .color_metadata_source = "test-bt2020-pq"}};
  image.planes.push_back(std::move(plane));
  return image;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "awjimage-native-pipeline-test";
  const auto input = root / "input.webp";
  const auto output = root / "out";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return fail("failed to create temp dir.");
  }

  awj::WebPImageEncoder encoder;
  auto encoded = encoder.encode(
      make_test_image(),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                 .quality = 100,
                                 .speed = 10,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!encoded) {
    return fail(encoded.error());
  }
  {
    std::ofstream stream{input, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  }

  awj::AppConfig cfg;
  cfg.input_path = input;
  cfg.output_dir = output;
  cfg.output_format = awj::OutputFormat::webp;
  cfg.quality = 100;
  cfg.max_jobs = 1;

  awj::FileLogger logger{output, false};
  awj::NativeBackend backend{cfg, logger, awj::ResourcePlan{
                                               .file_parallelism = 1,
                                               .encoder_threads_per_file = 1,
                                               .global_thread_budget = 1}};
  const auto input_bytes = std::filesystem::file_size(input, ec);
  if (ec) {
    return fail("failed to stat input file.");
  }
  auto result = backend.encode(awj::ImageFile{.index = 0,
                                               .path = input,
                                               .bytes = input_bytes});
  if (!result.ok || result.skipped || result.canceled) {
    return fail(result.message.empty() ? "native backend encode failed." : result.message);
  }
  const auto output_file = output / "input.webp";
  if (!std::filesystem::exists(output_file)) {
    return fail("native backend did not create output file.");
  }
  if (result.output_path != output_file || result.output_bytes == 0) {
    return fail("native backend result metadata invalid.");
  }

  const auto hdr_input = root / "input-hdr.jxl";
  awj::JXLImageEncoder jxl_encoder;
  auto hdr_encoded = jxl_encoder.encode(
      make_hdr_test_image(),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                 .quality = 100,
                                 .speed = 10,
                                 .speed_explicit = true,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!hdr_encoded) {
    return fail(hdr_encoded.error());
  }
  {
    std::ofstream stream{hdr_input, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(hdr_encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(hdr_encoded->encoded.bytes.size()));
  }

  auto hdr_cfg = cfg;
  hdr_cfg.input_path = hdr_input;
  hdr_cfg.output_format = awj::OutputFormat::webp;
  hdr_cfg.quality = 100;
  hdr_cfg.bit_depth = {};
  hdr_cfg.visual_quality = {};
  awj::NativeBackend hdr_backend{hdr_cfg, logger, awj::ResourcePlan{
                                                   .file_parallelism = 1,
                                                   .encoder_threads_per_file = 1,
                                                   .global_thread_budget = 1}};
  const auto hdr_input_bytes = std::filesystem::file_size(hdr_input, ec);
  if (ec) {
    return fail("failed to stat HDR input file.");
  }
  auto hdr_result = hdr_backend.encode(awj::ImageFile{.index = 0,
                                                      .path = hdr_input,
                                                      .bytes = hdr_input_bytes});
  if (!hdr_result.ok || hdr_result.skipped || hdr_result.canceled) {
    return fail(hdr_result.message.empty() ? "native backend HDR fallback encode failed."
                                           : hdr_result.message);
  }
  if (hdr_result.source_bit_depth.value_or(0) != 16 ||
      hdr_result.applied_bit_depth.value_or(0) != 8 ||
      !hdr_result.source_has_hdr_metadata ||
      hdr_result.applied_hdr_metadata != "sdr-tone-map" ||
      hdr_result.fallback_reason != "HDR -> SDR tone-map") {
    return fail("native backend HDR tone-map diagnostics invalid.");
  }

  auto hdr_avif_cfg = awj::default_app_config();
  hdr_avif_cfg.input_path = hdr_input;
  hdr_avif_cfg.output_dir = output / "hdr-avif";
  hdr_avif_cfg.output_format = awj::OutputFormat::avif;
  hdr_avif_cfg.quality = 75;
  hdr_avif_cfg.max_jobs = 1;
  awj::NativeBackend hdr_avif_backend{hdr_avif_cfg, logger,
                                      awj::ResourcePlan{.file_parallelism = 1,
                                                        .encoder_threads_per_file = 1,
                                                        .global_thread_budget = 1}};
  auto hdr_avif_result = hdr_avif_backend.encode(
      awj::ImageFile{.index = 0, .path = hdr_input, .bytes = hdr_input_bytes});
  if (!hdr_avif_result.ok ||
      hdr_avif_result.applied_color_representation != "yuv" ||
      hdr_avif_result.source_bit_depth.value_or(0) != 16 ||
      hdr_avif_result.requested_bit_depth.value_or(0) != 16 ||
      hdr_avif_result.applied_bit_depth.value_or(0) != 12 ||
      hdr_avif_result.bit_depth_reason.find("12-bit") == std::string::npos) {
    return fail(hdr_avif_result.message.empty()
                    ? "HDR AVIF bit-depth selection diagnostics are inconsistent."
                    : hdr_avif_result.message);
  }
  auto hdr_avif_decoder = awj::make_avif_image_decoder(1);
  auto hdr_avif_decoded = hdr_avif_decoder->decode(hdr_avif_result.output_path);
  if (!hdr_avif_decoded || !hdr_avif_decoded->image.source_info ||
      hdr_avif_decoded->image.source_info->bit_depth != 12 ||
      hdr_avif_decoded->image.source_info->color_primaries.value_or(-1) != 9 ||
      hdr_avif_decoded->image.source_info->transfer_characteristics.value_or(-1) != 16 ||
      hdr_avif_decoded->image.source_info->matrix_coefficients.value_or(-1) != 9) {
    return fail(hdr_avif_decoded
                    ? "HDR AVIF did not retain BT.2020/PQ CICP with a YUV matrix."
                    : hdr_avif_decoded.error());
  }

  cfg.output_format = awj::OutputFormat::jxl;
  awj::NativeBackend jxl_backend{cfg, logger, awj::ResourcePlan{
                                                  .file_parallelism = 1,
                                                  .encoder_threads_per_file = 1,
                                                  .global_thread_budget = 1}};
  auto jxl_result = jxl_backend.encode(awj::ImageFile{.index = 0,
                                                      .path = input,
                                                      .bytes = input_bytes});
  if (!jxl_result.ok || jxl_result.skipped || jxl_result.canceled) {
    return fail(jxl_result.message.empty() ? "native backend JXL encode failed."
                                           : jxl_result.message);
  }
  const auto jxl_output_file = output / "input.jxl";
  if (!std::filesystem::exists(jxl_output_file)) {
    return fail("native backend did not create JXL output file.");
  }
  if (jxl_result.output_path != jxl_output_file || jxl_result.output_bytes == 0 ||
      jxl_result.command.find("jxl") == std::string::npos) {
    return fail("native backend JXL result metadata invalid.");
  }

#if AWJ_HAS_JPEGLI
  const auto jpeg_input = root / "photo.jpg";
  awj::JpegliImageEncoder source_jpeg_encoder;
  auto source_jpeg = source_jpeg_encoder.encode(
      make_test_image(),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jpgli,
                                 .quality = awj::encoding_defaults::default_jpegli_quality,
                                 .speed = awj::default_speed_for(awj::OutputFormat::jpgli),
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!source_jpeg) {
    return fail(source_jpeg.error());
  }
  {
    std::ofstream stream{jpeg_input, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(source_jpeg->encoded.bytes.data()),
                 static_cast<std::streamsize>(source_jpeg->encoded.bytes.size()));
  }
  const auto jpeg_bytes = static_cast<std::uint64_t>(
      std::filesystem::file_size(jpeg_input, ec));
  cfg.output_format = awj::OutputFormat::jxl;
  cfg.quality = awj::encoding_defaults::default_jxl_quality;
  cfg.visual_quality = {};
  cfg.strip_metadata = false;
  awj::NativeBackend jxl_jpeg_backend{cfg, logger, awj::ResourcePlan{
                                                       .file_parallelism = 1,
                                                       .encoder_threads_per_file = 1,
                                                       .global_thread_budget = 1}};
  auto jxl_jpeg_result = jxl_jpeg_backend.encode(
      awj::ImageFile{.index = 0, .path = jpeg_input, .bytes = jpeg_bytes});
  if (!jxl_jpeg_result.ok ||
      jxl_jpeg_result.integration_mode != "jxl-jpeg-bitstream-transcode" ||
      !jxl_jpeg_result.lossless ||
      jxl_jpeg_result.final_encoder_quality != 100) {
    return fail("native backend JPEG->JXL did not use bitstream transcode.");
  }

  {
    const auto compressed = read_text(jxl_jpeg_result.output_path);
    std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder{
        JxlDecoderCreate(nullptr), JxlDecoderDestroy};
    if (!decoder || JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_JPEG_RECONSTRUCTION | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(decoder.get(), reinterpret_cast<const std::uint8_t*>(compressed.data()), compressed.size()) != JXL_DEC_SUCCESS) {
      return fail("could not initialize JPEG reconstruction verification");
    }
    JxlDecoderCloseInput(decoder.get());
    std::vector<std::uint8_t> reconstructed(static_cast<std::size_t>(jpeg_bytes) + 1024);
    bool recovered = false;
    for (;;) {
      const auto status = JxlDecoderProcessInput(decoder.get());
      if (status == JXL_DEC_JPEG_RECONSTRUCTION) {
        if (JxlDecoderSetJPEGBuffer(decoder.get(), reconstructed.data(), reconstructed.size()) != JXL_DEC_SUCCESS)
          return fail("could not provide JPEG reconstruction buffer");
      } else if (status == JXL_DEC_FULL_IMAGE) {
        const auto size = reconstructed.size() - JxlDecoderReleaseJPEGBuffer(decoder.get());
        recovered = size == source_jpeg->encoded.bytes.size() &&
            std::equal(reconstructed.begin(), reconstructed.begin() + size,
                reinterpret_cast<const std::uint8_t*>(source_jpeg->encoded.bytes.data()));
      } else if (status == JXL_DEC_SUCCESS) {
        break;
      } else {
        return fail("JPEG reconstruction did not complete");
      }
    }
    if (!recovered) return fail("JPEG -> JXL -> JPEG changed the original bytes");
  }
  for (const bool resize : {false, true}) {
    auto branch_cfg = cfg;
    branch_cfg.output_dir = output / (resize ? "jxl-scaled" : "jxl-quality");
    branch_cfg.jxl_jpeg_lossless = resize;
    if (resize) branch_cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::manual, .scale_percent = 50};
    awj::NativeBackend branch{branch_cfg, logger, awj::ResourcePlan{
        .file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1}};
    const auto result = branch.encode(awj::ImageFile{.index = 0, .path = jpeg_input, .bytes = jpeg_bytes});
    if (!result.ok || result.integration_mode == "jxl-jpeg-bitstream-transcode") {
      return fail("JXL ignored the disabled lossless switch or bypassed percentage resize");
    }
    if (resize) {
      auto decoded = awj::JXLImageDecoder{}.decode(result.output_path);
      const auto expected = awj::limited_dimensions(make_test_image().width, make_test_image().height, branch_cfg);
      if (!decoded || !expected || decoded->image.width != expected->first || decoded->image.height != expected->second)
        return fail("JXL percentage resize produced incorrect dimensions");
    }
  }

  cfg.strip_metadata = true;
  awj::NativeBackend jxl_strip_backend{cfg, logger, awj::ResourcePlan{
                                                        .file_parallelism = 1,
                                                        .encoder_threads_per_file = 1,
                                                        .global_thread_budget = 1}};
  auto jxl_strip_result = jxl_strip_backend.encode(
      awj::ImageFile{.index = 0, .path = jpeg_input, .bytes = jpeg_bytes});
  if (!jxl_strip_result.ok ||
      jxl_strip_result.integration_mode == "jxl-jpeg-bitstream-transcode" ||
      jxl_strip_result.lossless ||
      jxl_strip_result.final_encoder_quality != awj::encoding_defaults::default_jxl_quality) {
    return fail("native backend JPEG->JXL strip fallback did not use default JXL lossy encode.");
  }
  cfg.strip_metadata = false;

  cfg.output_format = awj::OutputFormat::jpgli;
  cfg.quality = awj::encoding_defaults::default_jpegli_quality;
  cfg.bit_depth = 8;
  cfg.visual_quality = {};
  awj::NativeBackend jpegli_backend{cfg, logger, awj::ResourcePlan{
                                                     .file_parallelism = 1,
                                                     .encoder_threads_per_file = 1,
                                                     .global_thread_budget = 1}};
  auto jpegli_result = jpegli_backend.encode(awj::ImageFile{.index = 0,
                                                            .path = input,
                                                            .bytes = input_bytes});
  if (!jpegli_result.ok || jpegli_result.skipped || jpegli_result.canceled) {
    return fail(jpegli_result.message.empty() ? "native backend JPGLI encode failed."
                                              : jpegli_result.message);
  }
  const auto jpegli_output_file = output / "input.jpg";
  if (!std::filesystem::exists(jpegli_output_file)) {
    return fail("native backend did not create JPGLI .jpg output file.");
  }
  if (jpegli_result.output_path != jpegli_output_file ||
      jpegli_result.output_bytes == 0 ||
      jpegli_result.output_format != "JPGLI" ||
      jpegli_result.encoder_id != "jpegli" ||
      jpegli_result.command.find("JPGLI") == std::string::npos) {
    return fail("native backend JPGLI result metadata invalid.");
  }
  const auto jpegli_summary_dir = output / "jpegli-summary";
  if (auto csv_ok = awj::write_csv(
          jpegli_summary_dir,
          std::span<const awj::EncodeResult>{&jpegli_result, 1});
      !csv_ok) {
    return fail(csv_ok.error());
  }
  const auto jpegli_summary_csv = read_text(jpegli_summary_dir / "summary.csv");
  if (jpegli_summary_csv.find("format,encoder_id") == std::string::npos ||
      jpegli_summary_csv.find("JPGLI,jpegli") == std::string::npos) {
    return fail("native backend JPGLI summary diagnostics invalid.");
  }
#endif

  cfg.output_format = awj::OutputFormat::avif;
  cfg.avif_encoder = awj::AvifEncoderMode::automatic;
  cfg.chroma_mode = awj::ChromaMode::yuv444;
  cfg.bit_depth = 8;
  cfg.visual_quality = {};
  awj::NativeBackend avif_backend{cfg, logger, awj::ResourcePlan{
                                                    .file_parallelism = 1,
                                                    .encoder_threads_per_file = 1,
                                                    .global_thread_budget = 1}};
  auto avif_result = avif_backend.encode(awj::ImageFile{.index = 0,
                                                         .path = input,
                                                         .bytes = input_bytes});
  if (!avif_result.ok || avif_result.skipped || avif_result.canceled) {
    return fail(avif_result.message.empty() ? "native backend AVIF encode failed."
                                            : avif_result.message);
  }
  const auto avif_output_file = output / "input.avif";
  if (!std::filesystem::exists(avif_output_file)) {
    return fail("native backend did not create AVIF output file.");
  }
  if (avif_result.output_path != avif_output_file || avif_result.output_bytes == 0 ||
      avif_result.command.find("aom") == std::string::npos ||
      avif_result.requested_encoder_id != "auto" || avif_result.encoder_id != "aom") {
    return fail("native backend AVIF auto result metadata invalid.");
  }
  auto avif_decoder = awj::make_avif_image_decoder(1);
  auto default_yuv_decoded = avif_decoder->decode(avif_output_file);
  if (!default_yuv_decoded || !default_yuv_decoded->image.source_info ||
      avif_result.requested_color_representation != "yuv" ||
      avif_result.applied_color_representation != "yuv" ||
      default_yuv_decoded->image.source_info->matrix_coefficients.value_or(-1) == 0 ||
      default_yuv_decoded->image.source_info->matrix_coefficients.value_or(-1) == 2) {
    return fail(default_yuv_decoded
                    ? "default AVIF path unexpectedly used Identity/unknown color representation."
                    : default_yuv_decoded.error());
  }

  auto suffix_cfg = cfg;
  suffix_cfg.output_dir = output / "avif-png";
  suffix_cfg.append_png_suffix = true;
  awj::NativeBackend suffix_backend{suffix_cfg, logger,
                                    awj::ResourcePlan{.file_parallelism = 1,
                                                      .encoder_threads_per_file = 1,
                                                      .global_thread_budget = 1}};
  auto suffix_result = suffix_backend.encode(
      awj::ImageFile{.index = 0, .path = input, .bytes = input_bytes});
  const auto suffix_output = suffix_cfg.output_dir / "input.avif.png";
  auto suffix_decoded = avif_decoder->decode(suffix_output);
  if (!suffix_result.ok || suffix_result.output_path != suffix_output ||
      !std::filesystem::exists(suffix_output) || !suffix_decoded) {
    return fail(suffix_decoded
                    ? "AVIF.png did not create the expected decodable AVIF file."
                    : suffix_decoded.error());
  }

  auto numbered_suffix_cfg = suffix_cfg;
  numbered_suffix_cfg.collision_mode = awj::CollisionMode::suffix_number;
  awj::NativeBackend numbered_suffix_backend{
      numbered_suffix_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto numbered_suffix_result = numbered_suffix_backend.encode(
      awj::ImageFile{.index = 0, .path = input, .bytes = input_bytes});
  if (!numbered_suffix_result.ok ||
      numbered_suffix_result.output_path.filename() != "input(1).avif.png") {
    return fail("AVIF.png collision numbering split its complete extension.");
  }

  auto source_representation_cfg = cfg;
  source_representation_cfg.output_dir = output / "source-representation";
  source_representation_cfg.chroma_mode = awj::ChromaMode::auto_keep;
  source_representation_cfg.bit_depth = {};
  source_representation_cfg.quality = 70;
  source_representation_cfg.avif_color_representation =
      awj::AvifColorRepresentation::source;
  awj::NativeBackend source_representation_backend{
      source_representation_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto source_representation_result = source_representation_backend.encode(
      awj::ImageFile{.index = 0, .path = input, .bytes = input_bytes});
  auto source_representation_decoded =
      avif_decoder->decode(source_representation_result.output_path);
  if (!source_representation_result.ok ||
      source_representation_result.requested_color_representation != "source" ||
      source_representation_result.applied_color_representation != "rgb" ||
      source_representation_result.applied_chroma != "444" ||
      !source_representation_decoded ||
      !source_representation_decoded->image.source_info ||
      source_representation_decoded->image.source_info->matrix_coefficients
              .value_or(-1) != 0) {
    return fail(source_representation_decoded
                    ? "follow-source did not select RGB/GBR Identity for an RGB source."
                    : source_representation_decoded.error());
  }
  auto passthrough_seed_cfg = cfg;
  passthrough_seed_cfg.output_dir = output / "passthrough-seed";
  passthrough_seed_cfg.quality = 100;
  passthrough_seed_cfg.chroma_mode = awj::ChromaMode::auto_keep;
  passthrough_seed_cfg.bit_depth = {};
  awj::NativeBackend passthrough_seed_backend{
      passthrough_seed_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto passthrough_seed_result = passthrough_seed_backend.encode(
      awj::ImageFile{.index = 0, .path = input, .bytes = input_bytes});
  if (!passthrough_seed_result.ok || !passthrough_seed_result.lossless) {
    return fail("could not build a lossless YUV AVIF passthrough fixture.");
  }
  auto passthrough_cfg = passthrough_seed_cfg;
  passthrough_cfg.input_path = passthrough_seed_result.output_path;
  passthrough_cfg.output_dir = output / "passthrough-copy";
  awj::NativeBackend passthrough_backend{
      passthrough_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto passthrough_result = passthrough_backend.encode(awj::ImageFile{
      .index = 0,
      .path = passthrough_seed_result.output_path,
      .bytes = passthrough_seed_result.output_bytes});
  if (!passthrough_result.ok ||
      passthrough_result.integration_mode != "avif-lossless-passthrough" ||
      passthrough_result.applied_color_representation != "yuv") {
    return fail(passthrough_result.message.empty()
                    ? "eligible YUV AVIF did not keep lossless bitstream passthrough."
                    : passthrough_result.message);
  }

  const auto alpha_source = make_alpha_test_image();
  {
    auto scaled_cfg = passthrough_cfg;
    scaled_cfg.output_dir = output / "passthrough-scaled";
    scaled_cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::manual, .scale_percent = 50};
    awj::NativeBackend scaled{scaled_cfg, logger, awj::ResourcePlan{
        .file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1}};
    const auto result = scaled.encode(awj::ImageFile{.index = 0,
        .path = passthrough_seed_result.output_path, .bytes = passthrough_seed_result.output_bytes});
    if (!result.ok || result.integration_mode == "avif-lossless-passthrough")
      return fail("AVIF passthrough bypassed percentage resize");
    auto decoded = avif_decoder->decode(result.output_path);
    const auto expected = awj::limited_dimensions(make_test_image().width, make_test_image().height, scaled_cfg);
    if (!decoded || !expected || decoded->image.width != expected->first || decoded->image.height != expected->second)
      return fail("AVIF percentage resize produced incorrect dimensions");
  }
  const auto alpha_input = root / "alpha-input.webp";
  auto alpha_webp = encoder.encode(
      alpha_source,
      awj::NativeEncodeSettings{
          .output_format = awj::OutputFormat::webp,
          .quality = 100,
          .speed = 10,
          .source_has_alpha_channel = true,
          .encoder_supports_alpha = true,
          .applied_alpha = "kept",
          .resources = awj::ResourcePlan{.file_parallelism = 1,
                                         .encoder_threads_per_file = 1,
                                         .global_thread_budget = 1}});
  if (!alpha_webp) {
    return fail(alpha_webp.error());
  }
  {
    std::ofstream stream{alpha_input, std::ios::binary};
    stream.write(
        reinterpret_cast<const char*>(alpha_webp->encoded.bytes.data()),
        static_cast<std::streamsize>(alpha_webp->encoded.bytes.size()));
  }
  awj::WebPImageDecoder webp_decoder;
  auto alpha_reference = webp_decoder.decode(alpha_input);
  if (!alpha_reference ||
      alpha_reference->image.alpha_mode == awj::AlphaMode::none) {
    return fail(alpha_reference ? "WebP alpha fixture lost its alpha channel."
                                : alpha_reference.error());
  }

  auto alpha_cfg = cfg;
  alpha_cfg.input_path = alpha_input;
  alpha_cfg.output_dir = output / "alpha-auto";
  alpha_cfg.quality = 70;
  alpha_cfg.visual_quality = {};
  alpha_cfg.speed = 5;
  alpha_cfg.chroma_mode = awj::ChromaMode::auto_keep;
  alpha_cfg.bit_depth = {};
  alpha_cfg.alpha_policy = awj::AlphaModePolicy::automatic;
  awj::NativeBackend alpha_backend{
      alpha_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto alpha_result = alpha_backend.encode(
      awj::ImageFile{.index = 0,
                     .path = alpha_input,
                     .bytes = std::filesystem::file_size(alpha_input)});
  const auto alpha_output = alpha_cfg.output_dir / "alpha-input.avif";
  if (!alpha_result.ok || alpha_result.lossless ||
      alpha_result.final_encoder_quality != 70 ||
      alpha_result.encoder_id != "aom" || alpha_result.applied_alpha != "kept" ||
      alpha_result.applied_chroma != "444" ||
      alpha_result.applied_bit_depth.value_or(0) != 10 ||
      alpha_result.speed != 5 ||
      alpha_result.avif_rgb_to_yuv_seconds < 0.0 ||
      alpha_result.avif_add_image_seconds < 0.0 ||
      alpha_result.avif_finish_seconds < 0.0 ||
      alpha_result.avif_output_copy_seconds < 0.0 ||
      !std::filesystem::exists(alpha_output)) {
    return fail("transparent AVIF auto path did not preserve alpha at the requested lossy quality.");
  }
  auto alpha_avif_decoder = awj::make_avif_image_decoder(1);
  auto alpha_decoded = alpha_avif_decoder->decode(alpha_output);
  if (!alpha_decoded || alpha_decoded->image.alpha_mode == awj::AlphaMode::none) {
    return fail(alpha_decoded ? "transparent AVIF did not retain alpha."
                              : alpha_decoded.error());
  }

  auto alpha_explicit_8_cfg = alpha_cfg;
  alpha_explicit_8_cfg.output_dir = output / "alpha-explicit-8";
  alpha_explicit_8_cfg.bit_depth = 8;
  awj::NativeBackend alpha_explicit_8_backend{
      alpha_explicit_8_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto alpha_explicit_8_result = alpha_explicit_8_backend.encode(
      awj::ImageFile{.index = 0,
                     .path = alpha_input,
                     .bytes = std::filesystem::file_size(alpha_input)});
  if (!alpha_explicit_8_result.ok || alpha_explicit_8_result.lossless ||
      alpha_explicit_8_result.final_encoder_quality != 70 ||
      alpha_explicit_8_result.applied_chroma != "444" ||
      alpha_explicit_8_result.applied_bit_depth.value_or(0) != 8) {
    return fail("transparent AVIF explicit 8-bit lossy request was not preserved.");
  }

  const auto alpha_summary_dir = output / "alpha-summary";
  if (auto csv_ok = awj::write_csv(
          alpha_summary_dir,
          std::span<const awj::EncodeResult>{&alpha_result, 1});
      !csv_ok) {
    return fail(csv_ok.error());
  }
  const auto alpha_summary_csv =
      read_text(alpha_summary_dir / "summary.csv");
  if (alpha_summary_csv.find("avif_rgb_to_yuv_seconds,avif_add_image_seconds,avif_finish_seconds,avif_output_copy_seconds") ==
      std::string::npos) {
    return fail("AVIF substage timing columns are missing from summary.csv.");
  }

  auto alpha_off_cfg = alpha_cfg;
  alpha_off_cfg.output_dir = output / "alpha-off";
  alpha_off_cfg.alpha_policy = awj::AlphaModePolicy::off;
  awj::NativeBackend alpha_off_backend{
      alpha_off_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto alpha_off_result = alpha_off_backend.encode(
      awj::ImageFile{.index = 0,
                     .path = alpha_input,
                     .bytes = std::filesystem::file_size(alpha_input)});
  if (!alpha_off_result.ok || alpha_off_result.lossless ||
      alpha_off_result.final_encoder_quality != 70 ||
      alpha_off_result.applied_alpha != "stripped" ||
      alpha_off_result.speed != 5) {
    return fail("alpha=off no longer follows the requested lossy quality and speed.");
  }

  auto alpha_visual_cfg = alpha_cfg;
  alpha_visual_cfg.output_dir = output / "alpha-visual";
  alpha_visual_cfg.visual_quality = 80;
  alpha_visual_cfg.visual_quality_fallback = true;
  awj::NativeBackend alpha_visual_backend{
      alpha_visual_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto alpha_visual_result = alpha_visual_backend.encode(
      awj::ImageFile{.index = 0,
                     .path = alpha_input,
                     .bytes = std::filesystem::file_size(alpha_input)});
  if (!alpha_visual_result.ok || alpha_visual_result.final_encoder_quality < 1 ||
      alpha_visual_result.final_encoder_quality > 100 ||
      alpha_visual_result.search_attempt_count < 1 ||
      alpha_visual_result.applied_alpha != "kept" ||
      alpha_visual_result.applied_chroma != "444" ||
      alpha_visual_result.speed != 5) {
    return fail(std::format(
        "transparent AVIF visual-quality diagnostics invalid: ok={} q={} attempts={} alpha={} chroma={} speed={} message={}",
        alpha_visual_result.ok ? "true" : "false",
        alpha_visual_result.final_encoder_quality,
        alpha_visual_result.search_attempt_count,
        alpha_visual_result.applied_alpha,
        alpha_visual_result.applied_chroma,
        alpha_visual_result.speed,
        alpha_visual_result.message));
  }

  auto avif_source_encoder = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
  const auto preserves_native_color_range = [&](int color_range) {
    auto source_settings = awj::NativeEncodeSettings{
        .output_format = awj::OutputFormat::avif,
        .quality = 70,
        .speed = 6,
        .bit_depth = 8,
        .chroma_mode = awj::ChromaMode::yuv420,
        .avif_encoder = awj::AvifEncoderMode::aom,
        .applied_color_range = color_range,
        .resources = awj::ResourcePlan{.file_parallelism = 1,
                                       .encoder_threads_per_file = 1,
                                       .global_thread_budget = 1}};
    auto range_source = avif_source_encoder->encode(make_test_image(), source_settings);
    if (!range_source) {
      return false;
    }
    const auto range_name = std::format("range-{}", color_range);
    const auto range_input = root / std::format("{}.avif", range_name);
    {
      std::ofstream stream{range_input, std::ios::binary};
      stream.write(reinterpret_cast<const char*>(range_source->encoded.bytes.data()),
                   static_cast<std::streamsize>(range_source->encoded.bytes.size()));
    }
    auto range_cfg = alpha_cfg;
    range_cfg.input_path = range_input;
    range_cfg.output_dir = output / range_name;
    range_cfg.quality = 70;
    range_cfg.visual_quality = {};
    range_cfg.chroma_mode = awj::ChromaMode::auto_keep;
    range_cfg.bit_depth = {};
    awj::NativeBackend range_backend{
        range_cfg, logger,
        awj::ResourcePlan{.file_parallelism = 1,
                          .encoder_threads_per_file = 1,
                          .global_thread_budget = 1}};
    auto range_result = range_backend.encode(
        awj::ImageFile{.index = 0,
                       .path = range_input,
                       .bytes = std::filesystem::file_size(range_input)});
    auto range_decoded = avif_decoder->decode(
        range_cfg.output_dir / std::format("{}.avif", range_name));
    return range_result.ok && range_result.source_color_range.value_or(-1) == color_range &&
           range_result.applied_color_range.value_or(-1) == color_range &&
           range_result.applied_chroma == "420" && range_decoded &&
           range_decoded->image.source_info &&
           range_decoded->image.source_info->pixel_format == awj::PixelFormat::yuv420 &&
           range_decoded->image.source_info->color_range.value_or(-1) == color_range;
  };
  if (!preserves_native_color_range(0) || !preserves_native_color_range(1)) {
    return fail("native AVIF did not preserve limited/full source color range.");
  }

  cfg.visual_quality = 100;
  cfg.chroma_mode = awj::ChromaMode::yuv420;
  cfg.bit_depth = 8;
  awj::NativeBackend avif_visual_backend{cfg, logger, awj::ResourcePlan{
                                                          .file_parallelism = 1,
                                                          .encoder_threads_per_file = 1,
                                                          .global_thread_budget = 1}};
  auto avif_visual_result = avif_visual_backend.encode(awj::ImageFile{.index = 0,
                                                                      .path = input,
                                                                      .bytes = input_bytes});
  if (!avif_visual_result.ok || avif_visual_result.skipped || avif_visual_result.canceled) {
    return fail(avif_visual_result.message.empty() ? "native backend AVIF visual_quality encode failed."
                                                  : avif_visual_result.message);
  }
  if (avif_visual_result.search_attempt_count != 1 ||
      !avif_visual_result.lossless ||
      avif_visual_result.final_encoder_quality != 100 ||
      avif_visual_result.command.find("aom") == std::string::npos ||
      avif_visual_result.requested_encoder_id != "aom") {
    return fail(std::format(
        "native backend AVIF visual_quality did not use shared lossless auto search path: attempts={} lossless={} final_q={} command={} requested_encoder={}",
        avif_visual_result.search_attempt_count,
        avif_visual_result.lossless ? "true" : "false",
        avif_visual_result.final_encoder_quality,
        avif_visual_result.command,
        avif_visual_result.requested_encoder_id));
  }

  // 下一次启动会清理该目录；这里直接退出以避开第三方静态析构阶段的访问违规。
  terminate_test_process(0);
}
