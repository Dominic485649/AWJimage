#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lcms2.h>
#include <avif/avif.h>

import awj.animation;
import awj.avif_aom_codec;
import awj.codec;
import awj.config;
import awj.gain_map;
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

// IQ 使用 deltaq=6；auto-intra-tools-off 的 deltaq=3 预分析不应被误当成
// IQ 的新优化。用真实编解码结果保护这条依赖行为，并覆盖带 alpha 的输入。
awj::compat::expected<std::vector<std::uint8_t>, std::string> encode_iq_intra_options(
    const avifImage& image, int automatic_tools_off, bool explicit_tools) {
  std::unique_ptr<avifEncoder, decltype(&avifEncoderDestroy)> encoder{
      avifEncoderCreate(), avifEncoderDestroy};
  if (!encoder) return awj::compat::unexpected{"IQ 对照无法创建编码器。"};
  encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
  encoder->quality = 70;
  encoder->qualityAlpha = 70;
  encoder->speed = 5;
  encoder->maxThreads = 1;
  const auto set = [&](const char* key, const char* value) {
    return avifEncoderSetCodecSpecificOption(encoder.get(), key, value) == AVIF_RESULT_OK;
  };
  if (!set("color:tune", "iq") ||
      (automatic_tools_off >= 0 &&
       !set("color:auto-intra-tools-off", automatic_tools_off ? "1" : "0"))) {
    return awj::compat::unexpected{"IQ 对照参数设置失败。"};
  }
  if (explicit_tools) {
    for (const auto* key : {"color:enable-smooth-intra", "color:enable-paeth-intra",
                            "color:enable-cfl-intra", "color:enable-diagonal-intra"}) {
      if (!set(key, "1")) return awj::compat::unexpected{"IQ 帧内工具参数设置失败。"};
    }
  }
  struct EncodedData {
    avifRWData bytes = AVIF_DATA_EMPTY;
    ~EncodedData() { avifRWDataFree(&bytes); }
  } output;
  const auto result = avifEncoderWrite(encoder.get(), &image, &output.bytes);
  if (result != AVIF_RESULT_OK) {
    return awj::compat::unexpected{std::string{"IQ 对照编码失败："} + avifResultToString(result)};
  }
  if (!output.bytes.data || output.bytes.size == 0)
    return awj::compat::unexpected{"IQ 对照没有输出码流。"};
  std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder{
      avifDecoderCreate(), avifDecoderDestroy};
  if (!decoder ||
      avifDecoderSetIOMemory(decoder.get(), output.bytes.data, output.bytes.size) != AVIF_RESULT_OK ||
      avifDecoderParse(decoder.get()) != AVIF_RESULT_OK ||
      avifDecoderNextImage(decoder.get()) != AVIF_RESULT_OK ||
      decoder->image->width != image.width || decoder->image->height != image.height ||
      decoder->image->depth != image.depth || decoder->image->yuvFormat != image.yuvFormat ||
      (decoder->image->alphaPlane != nullptr) != (image.alphaPlane != nullptr)) {
    return awj::compat::unexpected{"IQ 对照解码或位深、采样、透明通道不符。"};
  }
  return std::vector<std::uint8_t>{output.bytes.data, output.bytes.data + output.bytes.size};
}

int verify_iq_intra_tool_defaults() {
  if (!awj::NativeEncodeSettings{}.avif_tune_iq)
    return fail("静态 AVIF 的 IQ 默认值意外变化。");
  struct Case { int depth; avifPixelFormat format; bool alpha; };
  for (const auto sample : {Case{8, AVIF_PIXEL_FORMAT_YUV420, false},
                            Case{10, AVIF_PIXEL_FORMAT_YUV444, true},
                            Case{12, AVIF_PIXEL_FORMAT_YUV444, false},
                            Case{8, AVIF_PIXEL_FORMAT_YUV400, true}}) {
    std::unique_ptr<avifImage, decltype(&avifImageDestroy)> image{
        avifImageCreate(32, 32, sample.depth, sample.format), avifImageDestroy};
    if (!image) return fail("IQ 对照无法创建图像。");
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
    image->yuvRange = AVIF_RANGE_FULL;
    std::vector<std::uint8_t> pixels(32 * 32 * 4);
    for (unsigned y = 0; y < 32; ++y) {
      for (unsigned x = 0; x < 32; ++x) {
        const auto offset = (y * 32 + x) * 4;
        pixels[offset] = static_cast<std::uint8_t>(x * 17 + y * 31);
        pixels[offset + 1] = static_cast<std::uint8_t>(x * 97 + y * 11 + x * y * 3);
        pixels[offset + 2] = static_cast<std::uint8_t>(x * 7 + y * 89 + x * y * 13);
        pixels[offset + 3] = sample.alpha ? static_cast<std::uint8_t>(1 + (x * 73 + y * 151) % 254) : 255;
      }
    }
    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, image.get());
    rgb.depth = 8;
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.ignoreAlpha = sample.alpha ? AVIF_FALSE : AVIF_TRUE;
    rgb.rowBytes = 32 * 4;
    rgb.pixels = pixels.data();
    if (avifImageRGBToYUV(image.get(), &rgb) != AVIF_RESULT_OK)
      return fail("IQ 对照 RGB 转换失败。");
    const auto baseline = encode_iq_intra_options(*image, -1, false);
    const auto automatic_off = encode_iq_intra_options(*image, 0, false);
    const auto automatic_on = encode_iq_intra_options(*image, 1, false);
    const auto explicit_tools = encode_iq_intra_options(*image, 0, true);
    for (const auto* encoded : {&baseline, &automatic_off, &automatic_on, &explicit_tools}) {
      if (!*encoded) return fail(encoded->error());
    }
    if (*baseline != *automatic_off || *baseline != *automatic_on || *baseline != *explicit_tools)
      return fail("IQ 默认、auto=0/1 与显式启用四项工具的码流不再一致，须重新研究依赖行为。");
  }
  return 0;
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
      return fail(awj::compat::format("AVIF decode result invalid: got {}x{} {}-bit source {}-bit.",
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

int verify_thread_budget_policy() {
  for (const int threads : {1, 2, 8}) {
    if (awj::avif_aom_detail::codec_thread_count(threads) != threads)
      return fail("AVIF changed the resolved auto/explicit thread budget.");
    const awj::ResourcePlan single{.file_parallelism = 1,
        .encoder_threads_per_file = threads, .global_thread_budget = threads};
    for (const std::size_t cells : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{9}}) {
      const int expected = cells < 1 ? 1 : cells < static_cast<std::size_t>(threads) ? static_cast<int>(cells) : threads;
      if (awj::avif_aom_detail::grid_tile_prepare_thread_count(cells, single) != expected)
        return fail("Grid preparation did not use the file's normalized CPU share.");
    }
    // An inconsistent direct caller must not let preparation bypass the
    // concurrent-file split merely because encoder_threads_per_file is larger.
    const awj::ResourcePlan shared{.file_parallelism = 2,
        .encoder_threads_per_file = threads, .global_thread_budget = threads};
    const int share = threads / 2 < 1 ? 1 : threads / 2;
    if (awj::avif_aom_detail::grid_tile_prepare_thread_count(9, shared) != share)
      return fail("Grid preparation oversubscribed concurrent files.");
  }
  if (awj::avif_aom_detail::codec_thread_count(0) != 1 ||
      awj::avif_aom_detail::codec_thread_count(-2) != 1 ||
      awj::avif_aom_detail::codec_thread_count(129) != 128 ||
      awj::avif_aom_detail::grid_tile_prepare_thread_count(4,
          awj::ResourcePlan{.file_parallelism = 2, .encoder_threads_per_file = 4, .global_thread_budget = 4}) != 2 ||
      awj::avif_aom_detail::grid_tile_prepare_thread_count(std::numeric_limits<std::size_t>::max(),
          awj::ResourcePlan{.encoder_threads_per_file = 256, .global_thread_budget = 256}) != 128)
    return fail("AVIF lower/upper thread limits or Grid preparation normalization changed.");
  return 0;
}

awj::compat::expected<std::vector<std::byte>, std::string> make_budget_sequence(
    std::uint32_t width, std::uint32_t height, int depth, avifPixelFormat format, bool alpha) {
  std::unique_ptr<avifEncoder, decltype(&avifEncoderDestroy)> encoder{avifEncoderCreate(), avifEncoderDestroy};
  if (!encoder) return awj::compat::unexpected{"AVIF budget fixture encoder allocation failed."};
  encoder->codecChoice = AVIF_CODEC_CHOICE_AOM; encoder->maxThreads = 1;
  encoder->speed = 10; encoder->quality = 80; encoder->qualityAlpha = 100;
  encoder->timescale = 1000; encoder->repetitionCount = 2;
  constexpr std::array<std::uint64_t, 3> durations{17, 43, 29};
  const auto maximum = static_cast<std::uint16_t>((1u << depth) - 1);
  for (std::size_t index = 0; index < durations.size(); ++index) {
    std::unique_ptr<avifImage, decltype(&avifImageDestroy)> image{
        avifImageCreate(width, height, static_cast<std::uint32_t>(depth), format), avifImageDestroy};
    if (!image || avifImageAllocatePlanes(image.get(), alpha ? AVIF_PLANES_ALL : AVIF_PLANES_YUV) != AVIF_RESULT_OK)
      return awj::compat::unexpected{"AVIF budget fixture plane allocation failed."};
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
    image->yuvRange = AVIF_RANGE_FULL;
    constexpr std::array<std::uint8_t, 9> xmp{'<','b','u','d','g','e','t','/','>'};
    if (avifImageSetMetadataXMP(image.get(), xmp.data(), xmp.size()) != AVIF_RESULT_OK)
      return awj::compat::unexpected{"AVIF budget fixture XMP allocation failed."};
    for (int c = 0; c < (alpha ? 4 : 3); ++c) {
      auto* plane = avifImagePlane(image.get(), c);
      const auto stride = avifImagePlaneRowBytes(image.get(), c);
      const auto plane_width = avifImagePlaneWidth(image.get(), c), plane_height = avifImagePlaneHeight(image.get(), c);
      if (!plane || !plane_width || !plane_height) return awj::compat::unexpected{"AVIF budget fixture plane layout invalid."};
      for (std::uint32_t y = 0; y < plane_height; ++y) for (std::uint32_t x = 0; x < plane_width; ++x) {
        const auto value8 = c == 0 ? 32u + static_cast<unsigned>(index) * 64u + (x * 3 + y * 5) % 24u
            : c < 3 ? 120u + (x + y + static_cast<unsigned>(c)) % 16u
            : index == 0 ? 255u : index == 1 ? ((x / 8 + y / 4) % 3u) * 127u
            : 64u + ((x + y) % 2u) * 128u;
        const auto value = static_cast<std::uint16_t>((value8 * maximum + 127u) / 255u);
        if (depth == 8) plane[y * stride + x] = static_cast<std::uint8_t>(value);
        else std::memcpy(plane + y * stride + x * 2, &value, sizeof(value));
      }
    }
    const auto result = avifEncoderAddImage(encoder.get(), image.get(), durations[index], AVIF_ADD_IMAGE_FLAG_NONE);
    if (result != AVIF_RESULT_OK)
      return awj::compat::unexpected{std::string{"AVIF budget sequence encoding failed: "} + avifResultToString(result)};
  }
  struct Data { avifRWData value = AVIF_DATA_EMPTY; ~Data() { avifRWDataFree(&value); } } output;
  const auto result = avifEncoderFinish(encoder.get(), &output.value);
  if (result != AVIF_RESULT_OK)
    return awj::compat::unexpected{std::string{"AVIF budget sequence mux failed: "} + avifResultToString(result)};
  const auto* begin = reinterpret_cast<const std::byte*>(output.value.data);
  return std::vector<std::byte>{begin, begin + output.value.size};
}

bool same_budget_image(const awj::ImageBuffer& a, const awj::ImageBuffer& b) {
  if (a.width != b.width || a.height != b.height || a.pixel_format != b.pixel_format ||
      a.alpha_mode != b.alpha_mode || a.bit_depth != b.bit_depth || a.planes.size() != b.planes.size() ||
      a.metadata.size() != b.metadata.size() || !a.source_info || !b.source_info) return false;
  if (a.source_info->pixel_format != b.source_info->pixel_format || a.source_info->bit_depth != b.source_info->bit_depth ||
      a.source_info->color_primaries != b.source_info->color_primaries ||
      a.source_info->transfer_characteristics != b.source_info->transfer_characteristics ||
      a.source_info->matrix_coefficients != b.source_info->matrix_coefficients ||
      a.source_info->color_range != b.source_info->color_range ||
      a.source_info->has_hdr_metadata != b.source_info->has_hdr_metadata) return false;
  for (std::size_t c = 0; c < a.planes.size(); ++c)
    if (a.planes[c].stride != b.planes[c].stride || a.planes[c].bytes != b.planes[c].bytes) return false;
  for (std::size_t c = 0; c < a.metadata.size(); ++c)
    if (a.metadata[c].kind != b.metadata[c].kind || a.metadata[c].bytes != b.metadata[c].bytes) return false;
  return true;
}

int verify_large_sequence_budgets(const std::filesystem::path& test_root) {
  struct Directory {
    std::filesystem::path path; bool owned{};
    ~Directory() { if (owned) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
  } scratch{test_root / ("avif-budget-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
  std::filesystem::create_directories(test_root);
  scratch.owned = std::filesystem::create_directory(scratch.path);
  if (!scratch.owned) return fail("AVIF budget fixture directory already exists.");
  const auto path = scratch.path / "sequence.avifs";
  const auto write_fixture = [&](const std::vector<std::byte>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bool(file);
  };
  awj::compat::stop_source cancelled; cancelled.request_stop();
  constexpr std::array durations{17u, 43u, 29u};
  const std::vector<std::byte> expected_xmp{std::byte{'<'}, std::byte{'b'}, std::byte{'u'}, std::byte{'d'},
      std::byte{'g'}, std::byte{'e'}, std::byte{'t'}, std::byte{'/'}, std::byte{'>'}};
  for (const auto format : {AVIF_PIXEL_FORMAT_YUV420, AVIF_PIXEL_FORMAT_YUV422, AVIF_PIXEL_FORMAT_YUV444})
    for (const int depth : {8, 10, 12}) {
      auto bytes = make_budget_sequence(257, 129, depth, format, true);
      if (!bytes) return fail(bytes.error());
      if (!write_fixture(*bytes)) return fail("AVIF budget fixture write failed.");
      const auto pixel_format = format == AVIF_PIXEL_FORMAT_YUV420 ? awj::PixelFormat::yuv420
          : format == AVIF_PIXEL_FORMAT_YUV422 ? awj::PixelFormat::yuv422 : awj::PixelFormat::yuv444;
      std::array<std::optional<awj::ImageBuffer>, 3> baseline;
      std::optional<awj::ImageBuffer> native_baseline;
      for (const int threads : {1, 2, 8}) {
        // No automatic re-resolution: these are exactly the resolved per-file
        // shares. Pixel identity is evidence of correctness, not worker count.
        auto opened = awj::open_avif_animation(path, 128ull * 1024 * 1024, {}, threads);
        if (!opened || !*opened) return fail(opened ? "AVIF budget reader missing." : opened.error());
        auto& reader = **opened;
        if (reader.info().width != 257 || reader.info().height != 129 || reader.info().durations.size() != 3 ||
            reader.info().repetitions != 2 || reader.memory_usage_bytes() < awj::avif_aom_detail::sequence_decode_fixed_bytes(threads, true))
          return fail("AVIF sequence dimensions/loop or fixed pool reservation changed.");
        if (reader.next(cancelled.get_token()) || awj::open_avif_animation(path, 0, cancelled.get_token(), threads))
          return fail("AVIF budget decoder ignored cancellation.");
        for (std::size_t index = 0; index < durations.size(); ++index) {
          const auto duration = reader.info().durations[index];
          if (static_cast<std::uint64_t>(duration.numerator) * 1000 !=
              static_cast<std::uint64_t>(durations[index]) * duration.denominator)
            return fail("AVIF CPU share changed sequence timing.");
          if (index == 1) {
            const auto previous = reader.frame().planes[0].bytes;
            if (reader.next(cancelled.get_token()) || reader.frame().planes[0].bytes != previous)
              return fail("Cancelled AVIF next advanced or discarded the previous frame.");
          }
          const auto next = reader.next();
          if (!next || !*next) return fail(next ? "AVIF budget sequence ended early." : next.error());
          const auto& image = reader.frame();
          if (image.width != 257 || image.height != 129 || image.pixel_format != awj::PixelFormat::rgba ||
              image.bit_depth != (depth == 8 ? 8 : 16) || image.alpha_mode != awj::AlphaMode::straight ||
              image.planes.size() != 1 || image.planes[0].stride != 257u * 4u * (depth == 8 ? 1u : 2u) ||
              image.planes[0].bytes.size() != image.planes[0].stride * 129 ||
              !image.source_info || image.source_info->pixel_format != pixel_format || image.source_info->bit_depth != depth ||
              image.source_info->color_primaries != 1 || image.source_info->transfer_characteristics != 13 ||
              image.source_info->matrix_coefficients != 1 || image.source_info->color_range != 1 ||
              image.metadata.size() != 1 || image.metadata[0].kind != awj::MetadataKind::xmp ||
              image.metadata[0].bytes != expected_xmp)
            return fail("Odd AVIF sequence lost chroma/depth/alpha/CICP/XMP under a CPU share.");
          const auto alpha_offset = static_cast<std::size_t>(image.bit_depth == 8 ? 3 : 6);
          std::uint16_t alpha{};
          if (image.bit_depth == 8) alpha = std::to_integer<std::uint8_t>(image.planes[0].bytes[alpha_offset]);
          else std::memcpy(&alpha, image.planes[0].bytes.data() + alpha_offset, 2);
          if ((index == 0 && alpha != (depth == 8 ? 255 : 65535)) || (index == 1 && alpha != 0) ||
              (index == 2 && (alpha == 0 || alpha == (depth == 8 ? 255 : 65535))))
            return fail("AVIF dependent frames lost ordered opaque/transparent/partial alpha.");
          if (!baseline[index]) baseline[index] = image;
          else if (!same_budget_image(*baseline[index], image))
            return fail("AVIF 420/422/444 8/10/12-bit sequence samples or metadata depend on CPU budget.");
        }
        const auto end = reader.next(); if (!end || *end) return fail("AVIF budget sequence termination changed.");
        opened->reset();
        auto partial = awj::open_avif_animation(path, 0, {}, threads);
        if (!partial || !*partial || !(*partial)->next().value_or(false)) return fail("AVIF partial lifecycle failed.");
        partial->reset();
        auto decoder = awj::make_avif_image_decoder(threads);
        auto native = decoder->decode_memory(*bytes, "odd AVIF native copy", {.copy_metadata_payloads = true, .native_pixels = true});
        if (!native || native->image.pixel_format != pixel_format || native->image.bit_depth != depth ||
            native->image.planes.size() != 4) return fail(native ? "AVIF native YUV/alpha copy layout changed." : native.error());
        for (std::size_t c = 0; c < 4; ++c) {
          const auto width = (c == 1 || c == 2) && format != AVIF_PIXEL_FORMAT_YUV444 ? 129u : 257u;
          const auto height = (c == 1 || c == 2) && format == AVIF_PIXEL_FORMAT_YUV420 ? 65u : 129u;
          const auto stride = width * (depth > 8 ? 2u : 1u);
          if (native->image.planes[c].stride != stride || native->image.planes[c].bytes.size() != stride * height)
            return fail("Odd AVIF native copy rounded chroma planes incorrectly.");
        }
        if (!native_baseline) native_baseline = native->image;
        else if (!same_budget_image(*native_baseline, native->image)) return fail("AVIF native YUV copy changed across CPU shares.");
        if (decoder->decode_memory(*bytes, "cancelled native copy", {.native_pixels = true, .stop_token = cancelled.get_token()}))
          return fail("AVIF native-copy cancellation was not propagated.");
      }
      if (baseline[0]->planes[0].bytes == baseline[1]->planes[0].bytes ||
          baseline[1]->planes[0].bytes == baseline[2]->planes[0].bytes)
        return fail("AVIF budget fixture did not contain distinct dependent frames.");
    }

  auto small = make_budget_sequence(64, 64, 8, AVIF_PIXEL_FORMAT_YUV420, false);
  if (!small) return fail(small.error());
  if (!write_fixture(*small)) return fail("Small AVIF budget fixture write failed.");
  const auto fixed_one = awj::avif_aom_detail::sequence_decode_fixed_bytes(1, false);
  const auto fixed_many = awj::avif_aom_detail::sequence_decode_fixed_bytes(128, false);
  if (fixed_many <= fixed_one || awj::avif_aom_detail::sequence_decode_fixed_bytes(128, true) <= fixed_many ||
      awj::avif_aom_detail::sequence_decode_fixed_bytes(0, false) != fixed_one)
    return fail("AVIF thread/pool fixed working-set model did not normalize monotonically.");
  for (const auto limit : {std::uint64_t{327680}, fixed_many}) {
    std::uint64_t required{};
    const auto rejected = awj::open_avif_animation(path, limit, {}, 128, &required);
    if (rejected || rejected.error().find("内存预算") == std::string::npos || required <= limit || required == UINT64_MAX)
      return fail("Small AVIF authorized 128 decoder slots without their fixed working set or lost the required reservation.");
    auto retried = awj::open_avif_animation(path, required, {}, 128);
    if (!retried || !*retried || !(*retried)->next().value_or(false))
      return fail(retried ? "AVIF reader did not reopen after exact working-set admission." : retried.error());
  }
  auto one = awj::open_avif_animation(path, 0, {}, 1);
  auto many = awj::open_avif_animation(path, 512ull * 1024 * 1024, {}, 128);
  auto unlimited = awj::open_avif_animation(path, 0, {}, 128);
  if (!one || !*one || !many || !*many || !unlimited || !*unlimited ||
      (*many)->memory_usage_bytes() - (*one)->memory_usage_bytes() != fixed_many - fixed_one ||
      (*unlimited)->memory_usage_bytes() != (*many)->memory_usage_bytes())
    return fail("AVIF entry admission and retained reporting do not share the exact fixed thread cost or zero semantics.");
  if (!(*one)->next().value_or(false) || !(*many)->next().value_or(false) ||
      !same_budget_image((*one)->frame(), (*many)->frame()))
    return fail("Sufficient-memory 128-slot AVIF decode did not match the serial result.");
  if (awj::open_avif_animation(path, 0, cancelled.get_token(), 128) ||
      (*many)->next(cancelled.get_token())) return fail("AVIF high-share/unlimited cancellation failed.");
  one->reset(); many->reset(); unlimited->reset();
  return 0;
}

int verify_animation_sequence() {
  const std::array<awj::FrameDuration, 3> long_delays{{{65535, 1}, {1, 65521}, {1, 65519}}};
  const auto scale = awj::animation_timescale(long_delays);
  if (scale < 65521 || scale * 65535 > std::numeric_limits<std::uint32_t>::max())
    return fail("Long APNG frame durations would overflow AVIF sample_delta.");

  struct Frames final : awj::AnimationReader {
    awj::AnimationInfo details{.width = 2, .height = 2,
        .durations = {{1, 10}, {3, 20}, {1, 50}, {7, 100}}};
    std::array<awj::ImageBuffer, 4> images{make_test_image(), make_test_image(), make_test_image(), make_test_image()};
    std::size_t index{};
    Frames() {
      images[0].alpha_mode = awj::AlphaMode::none;
      images[1].planes[0].bytes[3] = std::byte{0};
      images[2].planes[0].bytes[3] = std::byte{0};
    }
    const awj::AnimationInfo& info() const noexcept override { return details; }
    const awj::ImageBuffer& frame() const noexcept override { return images[index]; }
    std::uint64_t memory_usage_bytes() const noexcept override { return 256; }
    awj::compat::expected<bool, std::string> next(awj::compat::stop_token = {}) override { return ++index < details.durations.size(); }
  };
  auto sequence_settings = settings(10, awj::ChromaMode::yuv444, awj::AvifEncoderMode::aom);
  sequence_settings.quality = 100;
  sequence_settings.speed = 10;
  sequence_settings.resources.memory_limit_bytes = 64 * 1024 * 1024;
  for (int repetitions : {-1, 0, 2}) {
    Frames frames;
    frames.details.repetitions = repetitions;
    std::size_t progress = 0;
    sequence_settings.frame_progress = [&](std::size_t completed, std::size_t total) {
      if (completed == progress + 1 && total == 4) progress = completed;
    };
    auto encoded = awj::encode_avif_animation(frames, sequence_settings);
    if (!encoded) return fail(encoded.error());
    std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder{avifDecoderCreate(), avifDecoderDestroy};
    if (!decoder || avifDecoderSetSource(decoder.get(), AVIF_DECODER_SOURCE_TRACKS) != AVIF_RESULT_OK ||
        avifDecoderSetIOMemory(decoder.get(), reinterpret_cast<const std::uint8_t*>(encoded->encoded.bytes.data()),
            encoded->encoded.bytes.size()) != AVIF_RESULT_OK || avifDecoderParse(decoder.get()) != AVIF_RESULT_OK ||
        decoder->imageCount != 4 || decoder->repetitionCount != repetitions ||
        decoder->timescale != 100 || decoder->durationInTimescales != 34 || progress != 4)
      return fail("Animation frame count, VFR duration, loop or progress was not preserved.");
    for (int index = 0; index < 4; ++index) {
      if (avifDecoderNextImage(decoder.get()) != AVIF_RESULT_OK || decoder->image->depth != 10 ||
          !decoder->image->alphaPlane || ((index == 1 || index == 2) && decoder->image->alphaPlane[0] != 0))
        return fail("10-bit sequence lost alpha after its opaque first frame.");
    }
    const auto path = std::filesystem::temp_directory_path() / "awj-sequence-reader.avifs";
    { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()), encoded->encoded.bytes.size()); }
    auto probe = awj::avif_has_sequence(path);
    if (!probe || !*probe) return fail("AVIF sequence probe did not find the animation.");
    std::array<std::vector<std::byte>, 4> baseline{};
    for (const int threads : {1, 2, 8}) {
      auto reader = awj::open_avif_animation(path, 64 * 1024 * 1024, {}, threads);
      if (!reader || !*reader || (*reader)->info().durations.size() != 4 || (*reader)->info().repetitions != repetitions)
        return fail(reader ? "AVIF sequence reader lost timing or repetitions." : reader.error());
      awj::compat::stop_source stopped; stopped.request_stop();
      if ((*reader)->next(stopped.get_token()) ||
          awj::open_avif_animation(path, 64 * 1024 * 1024, stopped.get_token(), threads))
        return fail("AVIF thread budget disabled animation cancellation.");
      for (int index = 0; index < 4; ++index) {
        const auto duration = (*reader)->info().durations[index];
        const auto expected = frames.details.durations[index];
        if (static_cast<std::uint64_t>(duration.numerator) * expected.denominator !=
            static_cast<std::uint64_t>(expected.numerator) * duration.denominator)
          return fail("AVIF thread budget changed VFR durations.");
        auto next = (*reader)->next();
        if (!next || !*next || (*reader)->frame().bit_depth != 16 || !(*reader)->frame().source_info ||
            (*reader)->frame().source_info->bit_depth != 10 || (*reader)->frame().planes.empty())
          return fail(next ? "AVIF sequence precision was lost." : next.error());
        const auto& pixels = (*reader)->frame().planes.front().bytes;
        if (baseline[index].empty()) baseline[index] = pixels;
        else if (pixels != baseline[index]) return fail("AVIF thread budget changed ordered animation pixels/alpha.");
      }
      const auto end = (*reader)->next();
      if (!end || *end) return fail("AVIF thread budget changed end-of-sequence semantics.");
    }
    std::filesystem::remove(path);
  }
  for (auto tune : {awj::AvifAnimationTune::automatic, awj::AvifAnimationTune::ssim, awj::AvifAnimationTune::psnr}) {
    Frames frames; sequence_settings.quality = 70; sequence_settings.speed = 5;
    sequence_settings.resources.encoder_threads_per_file = 1;
    sequence_settings.avif_animation_tune = tune; sequence_settings.avif_animation_keyframe = 2;
    auto lossy = awj::encode_avif_animation(frames, sequence_settings);
    if (!lossy || lossy->lossless) return fail(lossy ? "Lossy sequence unexpectedly lossless." : lossy.error());
  }
  {
    Frames frames;
    sequence_settings.bit_depth = 12;
    sequence_settings.applied_color_primaries = 9;
    sequence_settings.applied_transfer_characteristics = 16;
    sequence_settings.applied_matrix_coefficients = 9;
    for (auto& image : frames.images) image.source_info = awj::ImageSourceInfo{
        .pixel_format = awj::PixelFormat::rgba, .bit_depth = 8, .color_primaries = 9,
        .transfer_characteristics = 16, .matrix_coefficients = 9, .has_hdr_metadata = true};
    frames.details.durations = {{1,65521}, {1,65519}, {1,65513}, {1,65507}};
    auto encoded = awj::encode_avif_animation(frames, sequence_settings);
    if (!encoded) return fail(encoded.error());
    std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder{avifDecoderCreate(), avifDecoderDestroy};
    avifDecoderSetSource(decoder.get(), AVIF_DECODER_SOURCE_TRACKS);
    avifDecoderSetIOMemory(decoder.get(), reinterpret_cast<const std::uint8_t*>(encoded->encoded.bytes.data()), encoded->encoded.bytes.size());
    long double expected = 0;
    for (const auto duration : frames.details.durations) expected += static_cast<long double>(duration.numerator) / duration.denominator;
    if (avifDecoderParse(decoder.get()) != AVIF_RESULT_OK || decoder->image->depth != 12 ||
        decoder->image->colorPrimaries != 9 || decoder->image->transferCharacteristics != 16 ||
        std::abs(expected * decoder->timescale - decoder->durationInTimescales) > 1)
      return fail("12-bit HDR sequence or VFR cumulative rounding failed.");
    Frames single; single.details.durations.resize(1);
    auto still = awj::encode_avif_animation(single, sequence_settings);
    if (!still) return fail(still.error());
    Frames tiny; tiny.details.durations = {{1,4000000000u},{1,4000000000u},{1,4000000000u},{1,4000000000u}};
    if (awj::encode_avif_animation(tiny, sequence_settings)) return fail("Unrepresentable animation timing accepted.");
    awj::compat::stop_source stop; stop.request_stop(); Frames canceled;
    if (awj::encode_avif_animation(canceled, sequence_settings, stop.get_token())) return fail("Sequence cancellation ignored.");
    sequence_settings.resources.memory_limit_bytes = 1; Frames limited;
    if (awj::encode_avif_animation(limited, sequence_settings)) return fail("Sequence memory budget ignored.");
  }
  return 0;
}

}  // namespace

int verify_gain_map_geometry_and_samples() {
  const std::array<std::array<unsigned, 6>, 8> expected{{
      {1,2,3,4,5,6}, {2,1,4,3,6,5}, {6,5,4,3,2,1}, {5,6,3,4,1,2},
      {1,3,5,2,4,6}, {5,3,1,6,4,2}, {6,4,2,5,3,1}, {2,4,6,1,3,5}}};
  for (unsigned orientation = 1; orientation <= 8; ++orientation) {
    auto image = make_test_image(); image.height = 3;
    image.planes.front().bytes.resize(24);
    for (unsigned i = 0; i < 6; ++i) image.planes.front().bytes[i * 4] = std::byte(i + 1);
    const std::array<unsigned, 26> tiff{0x49,0x49,0x2a,0,8,0,0,0,1,0,
        0x12,1,3,0,1,0,0,0,orientation,0,0,0,0,0,0,0};
    awj::MetadataBlock exif{.kind = awj::MetadataKind::exif};
    for (auto value : tiff) exif.bytes.push_back(std::byte(value));
    image.metadata.push_back(std::move(exif));
    auto result = awj::gain_map_detail::apply_exif_orientation(image);
    if (!result) return fail(result.error());
    if (image.width != (orientation >= 5 ? 3u : 2u) || image.height != (orientation >= 5 ? 2u : 3u))
      return fail("Gain Map Exif orientation dimensions incorrect.");
    for (unsigned i = 0; i < 6; ++i)
      if (std::to_integer<unsigned>(image.planes.front().bytes[i * 4]) != expected[orientation - 1][i])
        return fail("Gain Map Exif orientation pixels incorrect.");
    const auto once = image.planes.front().bytes;
    if (!awj::gain_map_detail::apply_exif_orientation(image) || once != image.planes.front().bytes)
      return fail("Gain Map Exif orientation applied twice.");
    if (awj::gain_map_detail::crop_rgba(image, image.width, 0, 1, 1))
      return fail("Gain Map invalid crop accepted.");
  }
  auto base = make_test_image();
  for (unsigned i = 0; i < 4; ++i) {
    for (unsigned c = 0; c < 3; ++c) base.planes.front().bytes[i * 4 + c] = std::byte{128};
    base.planes.front().bytes[i * 4 + 3] = std::byte{64};
  }
  base.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 8,
      .color_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 1, .color_range = 1};
  auto map = base; map.width = map.height = 1; map.planes.front().stride = 4; map.planes.front().bytes.resize(4);
  std::unique_ptr<avifGainMap, decltype(&avifGainMapDestroy)> metadata(avifGainMapCreate(), avifGainMapDestroy);
  if (!metadata) return fail("Gain Map metadata allocation failed.");
  metadata->alternateHdrHeadroom = {2,1};
  for (int c = 0; c < 3; ++c) {
    metadata->gainMapMax[c] = {2,1}; metadata->gainMapGamma[c] = {2,1};
    metadata->baseOffset[c] = {1,100}; metadata->alternateOffset[c] = {2,100};
  }
  auto enhanced = awj::compose_iso_gain_map(base, map, *metadata);
  if (!enhanced) return fail(enhanced.error());
  auto transform = awj::hdr::hdr_detail::make_source_transform(*enhanced);
  if (!transform) return fail(transform.error());
  auto nits = awj::hdr::hdr_detail::linear_nits(*enhanced, *transform, enhanced->planes.front().bytes.data(), 0);
  const float linear = std::pow((128.0f / 255 + 0.055f) / 1.055f, 2.4f);
  const float reference = ((linear + 0.01f) * std::exp2(2 * std::sqrt(128.0f / 255)) - 0.02f) * 203;
  if (!nits || std::abs((*nits)[0] - reference) > reference * 0.02f || enhanced->width != 2 ||
      !enhanced->source_info->source_has_gain_map || metadata->image != nullptr ||
      std::abs(awj::hdr::hdr_detail::read_sample(*enhanced, enhanced->planes.front().bytes.data(), 3) - 64.0f / 255) > 0.001f)
    return fail("ISO Gain Map gamma/offset/size/alpha reconstruction incorrect.");
  metadata->gainMapGamma[0].n = 0;
  if (awj::compose_iso_gain_map(base, map, *metadata)) return fail("Zero Gain Map gamma accepted.");
  metadata->gainMapGamma[0].n = 2; metadata->baseHdrHeadroom.d = 0;
  if (awj::compose_iso_gain_map(base, map, *metadata)) return fail("Zero Gain Map denominator accepted.");
  metadata->baseHdrHeadroom.d = 1;
  base.metadata.push_back({awj::MetadataKind::icc, {std::byte{0}}});
  if (awj::compose_iso_gain_map(base, map, *metadata)) return fail("Unsupported ISO ICC silently accepted.");
  // Legacy protocol rejection and namespace-only false-positive boundaries are
  // covered by iso_gain_map_tests; there is no Apple reconstruction path.
  return 0;
}

int main(int argc, char** argv) {
  if (const int rc = verify_thread_budget_policy(); rc != 0) return rc;
  const bool budget_only = argc >= 2 && std::string_view{argv[1]} == "--thread-budget-only";
  const auto test_root = budget_only && argc >= 3 ? std::filesystem::path{argv[2]}
      : std::filesystem::current_path() / "results" / "codec-thread-budget";
  if (const int rc = verify_large_sequence_budgets(test_root); rc != 0) return rc;
  if (budget_only) return 0;
  if (const int rc = verify_iq_intra_tool_defaults(); rc != 0) return rc;
  if (const int rc = verify_gain_map_geometry_and_samples(); rc != 0) return rc;
  if (const int rc = verify_animation_sequence(); rc != 0) return rc;
  auto aom = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
  {
    auto colored = make_test_image(); colored.planes.front().bytes[3] = std::byte{64};
    auto mono_settings = settings(10, awj::ChromaMode::yuv400, awj::AvifEncoderMode::aom);
    mono_settings.quality = 100;
    mono_settings.source_has_alpha_channel = true;
    mono_settings.encoder_supports_alpha = true;
    mono_settings.applied_alpha = "kept";
    auto mono = aom->encode(colored, mono_settings);
    if (!mono || mono->diagnostics.applied_chroma != "400")
      return fail(mono ? "Forced AVIF monochrome diagnostics incorrect." : mono.error());
    auto decoder = awj::make_avif_image_decoder(1);
    auto decoded = decoder->decode_memory(mono->encoded.bytes, "forced monochrome");
    if (!decoded || !decoded->image.source_info || decoded->image.source_info->pixel_format != awj::PixelFormat::gray)
      return fail(decoded ? "Forced AVIF monochrome retained color planes." : decoded.error());
    const auto& image = decoded->image;
    const auto* row = image.planes.front().bytes.data();
    for (unsigned p = 0; p < 4; ++p) {
      const auto r = awj::hdr::hdr_detail::read_sample(image, row, p * 4);
      for (unsigned c = 1; c < 3; ++c)
        if (std::abs(r - awj::hdr::hdr_detail::read_sample(image, row, p * 4 + c)) > 0.001f)
          return fail("Forced AVIF monochrome decoded colored pixels.");
    }
    if (std::abs(awj::hdr::hdr_detail::read_sample(image, row, 3) - 64.0f / 255) > 0.01f)
      return fail("Forced AVIF monochrome lost alpha.");
  }

  auto scrgb_materialized = awj::hdr::materialize_scrgb_as_hdr10(make_scrgb_image());
  if (!scrgb_materialized || scrgb_materialized->bit_depth != 16 ||
      scrgb_materialized->sample_representation != awj::SampleRepresentation::unorm) {
    return fail(scrgb_materialized
                    ? "scRGB materialization no longer uses a 16-bit UNORM input container."
                    : scrgb_materialized.error());
  }
  auto gain_map_linear = make_scrgb_image();
  gain_map_linear.source_info->color_metadata_source = "jpeg-gain-map-linear";
  gain_map_linear.source_info->linear_reference_white_nits = 203;
  gain_map_linear.source_info->source_has_gain_map = true;
  auto gain_map_hdr = awj::hdr::materialize_scrgb_as_hdr10(gain_map_linear);
  if (!gain_map_hdr || gain_map_hdr->planes[0].bytes[1] <= scrgb_materialized->planes[0].bytes[1])
    return fail(gain_map_hdr ? "203-nit Gain Map reference white was treated as 80-nit scRGB."
                            : gain_map_hdr.error());
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
  if (const int rc = verify_auto_chroma(awj::PixelFormat::gray,
                                      awj::PixelFormat::gray, "400"); rc != 0) {
    return rc;
  }
  if (const int rc = verify_auto_chroma(awj::PixelFormat::unknown,
                                      awj::PixelFormat::yuv444, "444"); rc != 0) {
    return rc;
  }
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
    if (!range_encoded || range_encoded->diagnostics.applied_chroma != "444") {
      return fail(range_encoded ? "AOM RGB auto chroma was not 444."
                                : range_encoded.error());
    }
    return verify_avif_source_info(range_encoded->encoded,
                                   awj::PixelFormat::yuv444, color_range);
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
      grid_encoded->diagnostics.encoder_threads != 2) {
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
      alpha_encoded->diagnostics.applied_chroma != "444" ||
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
      auto_alpha_encoded->diagnostics.applied_chroma != "444" ||
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

  auto edge_grid_settings = settings(8, awj::ChromaMode::yuv420,
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
    return fail("AOM edge grid did not reject explicitly requested incompatible 420 chroma.");
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
