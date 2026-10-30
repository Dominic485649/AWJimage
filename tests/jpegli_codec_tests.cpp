#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

import awj.codec;
import awj.config;
import awj.image;
import awj.pixel_conversion;
import awj.jpegli_codec;
import awj.jpeg_hdr_codec;
import awj.gain_map;
import awj.decoder_registry;
import awj.hdr_tonemap;
import awj.resource_planner;

bool jpeg_hdr_sdr_compatible(const std::byte*, std::size_t, std::size_t);

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

awj::ImageBuffer make_test_image(bool metadata) {
  awj::ImagePlane plane{.stride = 12};
  plane.bytes = {
      std::byte{255}, std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{0},   std::byte{255}, std::byte{255},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
      std::byte{30},  std::byte{90},  std::byte{180}, std::byte{128},
      std::byte{240}, std::byte{200}, std::byte{80},  std::byte{255},
      std::byte{12},  std::byte{34},  std::byte{56},  std::byte{255},
      std::byte{78},  std::byte{90},  std::byte{12},  std::byte{255},
      std::byte{210}, std::byte{120}, std::byte{30},  std::byte{255},
  };
  awj::ImageBuffer image{.width = 3,
                         .height = 3,
                         .pixel_format = awj::PixelFormat::rgba,
                         .alpha_mode = awj::AlphaMode::straight,
                         .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  if (metadata) {
    image.metadata.push_back(awj::MetadataBlock{
        .kind = awj::MetadataKind::icc,
        .bytes = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}});
    image.metadata.push_back(awj::MetadataBlock{
        .kind = awj::MetadataKind::exif,
        .bytes = {std::byte{'I'}, std::byte{'I'}, std::byte{42}, std::byte{0}}});
    image.metadata.push_back(awj::MetadataBlock{
        .kind = awj::MetadataKind::xmp,
        .bytes = {std::byte{'<'}, std::byte{'x'}, std::byte{'/'} }});
  }
  return image;
}

awj::NativeEncodeSettings jpegli_settings(int quality, bool strip_metadata) {
  return awj::NativeEncodeSettings{
      .output_format = awj::OutputFormat::jpgli,
      .quality = quality,
      .speed = awj::default_speed_for(awj::OutputFormat::jpgli),
      .source_has_icc = !strip_metadata,
      .applied_icc = strip_metadata ? "stripped" : "kept",
      .strip_metadata = strip_metadata,
      .resources = awj::ResourcePlan{.file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}};
}

bool has_metadata_kind(const awj::ImageBuffer& image, awj::MetadataKind kind) {
  return std::ranges::any_of(image.metadata, [kind](const awj::MetadataBlock& block) {
    return block.kind == kind && !block.bytes.empty();
  });
}

bool jpeg_sof_matches(std::span<const std::byte> bytes, awj::ChromaMode chroma) {
  const auto value = [&](std::size_t index) { return std::to_integer<unsigned>(bytes[index]); };
  if (bytes.size() < 2 || value(0) != 0xff || value(1) != 0xd8) return false;
  std::size_t offset = 2;
  while (offset + 4 <= bytes.size()) {
    if (value(offset++) != 0xff) return false;
    while (offset < bytes.size() && value(offset) == 0xff) ++offset;
    if (offset >= bytes.size()) return false;
    const auto marker = value(offset++);
    if (marker == 0xda || marker == 0xd9) return false;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) continue;
    if (bytes.size() - offset < 2) return false;
    const auto length = (value(offset) << 8) | value(offset + 1);
    if (length < 2 || length > bytes.size() - offset) return false;
    if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
      const auto sampling = chroma == awj::ChromaMode::yuv420 ? 0x22u
                          : chroma == awj::ChromaMode::yuv422 ? 0x21u : 0x11u;
      return length >= 17 && value(offset + 2) == 8 && value(offset + 7) == 3 &&
          value(offset + 9) == sampling && value(offset + 12) == 0x11 &&
          value(offset + 15) == 0x11;
    }
    offset += length;
  }
  return false;
}

awj::ImageBuffer make_sampling_image() {
  awj::ImageBuffer image{.width = 16, .height = 16,
      .pixel_format = awj::PixelFormat::rgba, .bit_depth = 8};
  image.planes.push_back({std::vector<std::byte>(16 * 16 * 4), 16 * 4});
  for (std::size_t y = 0; y < 16; ++y) for (std::size_t x = 0; x < 16; ++x) {
    auto* pixel = image.planes[0].bytes.data() + y * image.planes[0].stride + x * 4;
    pixel[0] = std::byte{static_cast<unsigned char>(x * 17)};
    pixel[1] = std::byte{static_cast<unsigned char>(y * 17)};
    pixel[2] = std::byte{static_cast<unsigned char>((x + y) * 8)};
    pixel[3] = std::byte{255};
  }
  return image;
}

int verify_auto_sampling(const awj::JpegliImageEncoder& encoder) {
  static_assert(noexcept(awj::jpeg_auto_chroma(std::declval<const awj::ImageBuffer&>())));
  for (const auto source : {awj::PixelFormat::yuv420, awj::PixelFormat::yuv422,
      awj::PixelFormat::yuv444, awj::PixelFormat::rgb, awj::PixelFormat::rgba,
      awj::PixelFormat::gray, awj::PixelFormat::unknown}) {
    auto image = make_sampling_image();
    // Working RGBA must not override the decoder's original source sampling.
    image.source_info = awj::ImageSourceInfo{.pixel_format = source, .bit_depth = 8};
    const auto expected = source == awj::PixelFormat::yuv420 ? awj::ChromaMode::yuv420
                        : source == awj::PixelFormat::yuv422 ? awj::ChromaMode::yuv422
                        : awj::ChromaMode::yuv444;
    auto settings = jpegli_settings(90, true);
    auto encoded = encoder.encode(image, settings);
    if (awj::jpeg_auto_chroma(image) != expected || !encoded)
      return fail(encoded ? "JPEG auto ignored source_info sampling." : encoded.error());
    if (!jpeg_sof_matches(encoded->encoded.bytes, expected) ||
        encoded->diagnostics.requested_chroma != "auto" ||
        encoded->diagnostics.applied_chroma != awj::chroma_mode_name(expected))
      return fail("JPEG auto direct diagnostics disagree with three-component SOF sampling.");
    if (source != awj::PixelFormat::unknown) {
      auto native = make_sampling_image();
      if (source == awj::PixelFormat::rgb) {
        auto rgb = awj::jpegli_detail::make_rgb_buffer(native.planes[0], native.width, native.height);
        if (!rgb) return fail(rgb.error());
        native.pixel_format = source;
        native.planes[0] = {std::move(*rgb), native.width * 3};
      } else if (source != awj::PixelFormat::rgba) {
        auto converted = awj::convert_image_pixels(native, awj::PixelConversionRequest{
            .format = source, .bit_depth = 8, .preserve_alpha = false});
        if (!converted) return fail(converted.error());
        native = std::move(*converted);
      }
      native.source_info.reset(); // Fallback to native layout when no provenance exists.
      auto direct = encoder.encode(native, jpegli_settings(90, true));
      if (!direct || !jpeg_sof_matches(direct->encoded.bytes, expected))
        return fail(direct ? "JPEG native-layout auto sampling failed." : direct.error());
      auto rejected = jpegli_settings(90, true);
      rejected.chroma_mode = awj::ChromaMode::yuv400;
      if (encoder.encode(native, rejected)) return fail("Explicit JPEG 400 was silently accepted.");
    }
  }
  auto current_420 = awj::convert_image_pixels(make_sampling_image(), awj::PixelConversionRequest{
      .format = awj::PixelFormat::yuv420, .preserve_alpha = false});
  if (!current_420) return fail(current_420.error());
  current_420->source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::unknown, .bit_depth = 8};
  auto unknown_source = encoder.encode(*current_420, jpegli_settings(90, true));
  if (awj::jpeg_auto_chroma(*current_420) != awj::ChromaMode::yuv444 || !unknown_source ||
      !jpeg_sof_matches(unknown_source->encoded.bytes, awj::ChromaMode::yuv444))
    return fail("Unknown source_info sampling was replaced by the converted working YUV420 layout.");
  if (awj::jpeg_auto_chroma(awj::ImageBuffer{}) != awj::ChromaMode::yuv444 ||
      awj::avif_auto_chroma(awj::PixelFormat::gray) != awj::ChromaMode::yuv400)
    return fail("Unknown JPEG auto or AVIF gray-auto behavior changed.");
  auto xyb = jpegli_settings(90, true);
  xyb.jpegli_xyb = true;
  auto plain = encoder.encode(make_sampling_image(), xyb);
  auto with_source = make_sampling_image();
  with_source.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::yuv420, .bit_depth = 8};
  auto known_yuv = encoder.encode(with_source, xyb);
  if (!plain || !known_yuv || plain->encoded.bytes != known_yuv->encoded.bytes)
    return fail("JPEG auto source-follow changed the independent XYB sampling policy.");
  xyb.chroma_mode = awj::ChromaMode::yuv400;
  if (encoder.encode(with_source, xyb)) return fail("XYB accepted explicit JPEG 400.");
  return 0;
}

int verify_icc_fragment_limit(awj::JpegliImageEncoder& encoder) {
  constexpr auto maximum = awj::jpegli_detail::jpeg_icc_profile_max_bytes;
  static_assert(maximum == 16707345u);
  if (awj::jpegli_detail::jpeg_icc_size_supported(0) ||
      !awj::jpegli_detail::jpeg_icc_size_supported(maximum - 1) ||
      !awj::jpegli_detail::jpeg_icc_size_supported(maximum) ||
      awj::jpegli_detail::jpeg_icc_size_supported(maximum + 1) ||
      awj::jpegli_detail::jpeg_icc_size_supported(std::numeric_limits<std::size_t>::max()))
    return fail("JPEG ICC 的255分片边界计算错误。");
  auto image = make_test_image(false);
  image.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 8,
      .color_primaries = 12, .transfer_characteristics = 13, .matrix_coefficients = 0, .color_range = 1};
  image.metadata.push_back({awj::MetadataKind::icc, std::vector<std::byte>(maximum + 1)});
  for (const bool essential : {false, true}) {
    image.source_info->source_has_gain_map = essential;
    for (const bool strip : {false, true}) {
      auto encoded = encoder.encode(image, jpegli_settings(90, strip));
      if (encoded || encoded.error().find("255") == std::string::npos) {
        return fail("JPEG 将超过255片的 ICC 交给 C API，或没有明确拒绝。");
      }
    }
  }
  image.source_info->source_has_gain_map = false;
  image.source_info->color_primaries = 1;
  if (!encoder.encode(image, jpegli_settings(90, true)))
    return fail("普通 sRGB JPEG 无需保留已清除的大型可选 ICC。");
  image.source_info->source_has_gain_map = true;
  image.source_info->color_primaries = 12;
  auto yuv = awj::convert_image_pixels(image, {.format = awj::PixelFormat::yuv444,
      .bit_depth = 8, .matrix_coefficients = 6, .color_range = 1, .preserve_alpha = false});
  if (!yuv) return fail(yuv.error());
  auto rejected = encoder.encode(*yuv, jpegli_settings(90, true));
  if (rejected || rejected.error().find("255") == std::string::npos)
    return fail("原生 YUV JPEG 的必要 ICC 路径未检查255分片上限。");
  std::puts("JPEG ICC 255-fragment bounds, essential retain/strip and ordinary stripping passed.");
  return 0;
}

int verify_gain_map_sdr_icc(awj::JpegliImageEncoder& encoder) {
  auto image = make_test_image(false);
  image.alpha_mode = awj::AlphaMode::none;
  image.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 8,
      .color_primaries = 12, .transfer_characteristics = 13, .matrix_coefficients = 0, .color_range = 1,
      .color_metadata_source = "source-icc-verified-cicp", .source_has_gain_map = true};
  image.metadata.push_back({awj::MetadataKind::exif, {std::byte{'I'}, std::byte{'I'}, std::byte{42}, std::byte{0}}});
  image.metadata.push_back({awj::MetadataKind::xmp, {std::byte{'<'}, std::byte{'x'}, std::byte{'/'}, std::byte{'>'}}});
  const auto original = image.planes.front().bytes;
  std::vector<std::byte> generated_profile;
  for (const bool gain_map : {false, true}) for (const bool native_yuv : {false, true}) {
    auto input = image;
    input.source_info->source_has_gain_map = gain_map;
    if (native_yuv) {
      auto converted = awj::convert_image_pixels(input, {.format = awj::PixelFormat::yuv444,
          .bit_depth = 8, .matrix_coefficients = 6, .color_range = 1, .preserve_alpha = false});
      if (!converted) return fail(converted.error());
      input = std::move(*converted);
    }
    for (const bool strip : {false, true}) {
      auto settings = jpegli_settings(100, strip);
      settings.source_has_icc = false;
      settings.applied_icc = "none";
      settings.chroma_mode = awj::ChromaMode::yuv444;
      auto encoded = encoder.encode(input, settings);
      if (!encoded) return fail(encoded.error());
      auto decoded = awj::JpegliImageDecoder{}.decode_memory(encoded->encoded.bytes, "gain-map-sdr-icc");
      if (!decoded) return fail(decoded.error());
      const auto& metadata = decoded->image.metadata;
      const auto icc = std::ranges::find_if(metadata, [](const auto& block) { return block.kind == awj::MetadataKind::icc; });
      if (icc == metadata.end() || icc->bytes.size() < 132 ||
          std::ranges::count_if(metadata, [](const auto& block) { return block.kind == awj::MetadataKind::icc; }) != 1 ||
          encoded->diagnostics.applied_icc != "generated-essential-sdr-cicp" ||
          encoded->diagnostics.applied_color_primaries != 12 || encoded->diagnostics.applied_transfer_characteristics != 13)
        return fail("已有 P3 SDR 的普通 JPEG 丢失必要 ICC 或实际色彩诊断。");
      if (has_metadata_kind(decoded->image, awj::MetadataKind::exif) != !strip ||
          has_metadata_kind(decoded->image, awj::MetadataKind::xmp) != !strip || image.planes.front().bytes != original)
        return fail("JPEG 必要 ICC 的保留意外改变可选元数据清除或调用方像素。");
      generated_profile = icc->bytes;
    }
  }
  auto borrowed = image;
  borrowed.metadata.push_back({awj::MetadataKind::icc, generated_profile});
  auto settings = jpegli_settings(100, true);
  auto encoded = encoder.encode(borrowed, settings);
  if (!encoded) return fail(encoded.error());
  auto decoded = awj::JpegliImageDecoder{}.decode_memory(encoded->encoded.bytes, "borrowed-essential-sdr-icc");
  if (!decoded) return fail(decoded.error());
  const auto icc = std::ranges::find_if(decoded->image.metadata, [](const auto& block) { return block.kind == awj::MetadataKind::icc; });
  if (icc == decoded->image.metadata.end() || icc->bytes != generated_profile || encoded->diagnostics.applied_icc != "essential-sdr-rendition")
    return fail("JPEG 未借用并原样保留已有 SDR 的必要 ICC。");
  settings.jpegli_xyb = true;
  if (encoder.encode(image, settings)) return fail("非 sRGB 的 Gain Map SDR 被错误套用 XYB 内置 ICC。");
  std::stop_source cancelled;
  cancelled.request_stop();
  settings.jpegli_xyb = false;
  if (encoder.encode(image, settings, cancelled.get_token())) return fail("必要 SDR ICC 编码忽略取消。");
  std::puts("JPEGli Gain Map SDR essential ICC, RGBA/YUV, strip and cancellation passed.");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif

  awj::JpegliImageEncoder encoder;
  if (const int result = verify_icc_fragment_limit(encoder)) return result;
  if (const int result = verify_gain_map_sdr_icc(encoder)) return result;
  if (argc == 2 && std::string_view{argv[1]} == "--gain-map-color-only") return 0;
  if (const int result = verify_auto_sampling(encoder)) return result;
  auto encoded = encoder.encode(make_test_image(true), jpegli_settings(42, false));
  if (!encoded) {
    return fail(encoded.error());
  }
  if (encoded->encoded.bytes.empty() || encoded->encoded.codec_name != "jpegli") {
    return fail("JPG encode result invalid.");
  }
  for (const auto chroma : {awj::ChromaMode::auto_keep, awj::ChromaMode::yuv420, awj::ChromaMode::yuv422, awj::ChromaMode::yuv444}) {
    auto settings = jpegli_settings(90, true);
    settings.chroma_mode = chroma;
    auto jpeg = encoder.encode(make_test_image(false), settings);
    if (!jpeg) return fail(jpeg.error());
    awj::JpegliImageDecoder decoder;
    auto decoded = decoder.decode_memory(jpeg->encoded.bytes, "JPG sampling");
    const auto expected = chroma == awj::ChromaMode::auto_keep || chroma == awj::ChromaMode::yuv444 ? awj::PixelFormat::yuv444 : chroma == awj::ChromaMode::yuv422 ? awj::PixelFormat::yuv422 : awj::PixelFormat::yuv420;
    if (!decoded || !decoded->image.source_info || decoded->image.source_info->bit_depth != 8 ||
        decoded->image.source_info->pixel_format != expected || jpeg->diagnostics.applied_matrix_coefficients != 6)
      return fail("JPG SOF sampling, precision or YCbCr diagnostics are invalid.");
    auto image = make_test_image(false);
    auto rgb = awj::jpegli_detail::make_rgb_buffer(image.planes.front(), image.width, image.height);
    if (!rgb) return fail(rgb.error());
    settings.jpegli_rgb8_input = *rgb;
    auto cached = encoder.encode(image, settings);
    if (!cached || cached->encoded.bytes != jpeg->encoded.bytes) return fail("JPG scanline conversion differs from cached RGB input.");
  }
  for (const auto name : {L"jpg", L"jpeg", L"jpgli", L"jpegli"})
    if (awj::config_detail::parse_output_format(name) != awj::OutputFormat::jpgli) return fail("JPG CLI alias is incompatible.");
  if (encoded->final_quality != 42 ||
      encoded->diagnostics.encoder_id != "jpegli" ||
      !encoded->diagnostics.speed_mapping.codec_key.empty() ||
      encoded->diagnostics.speed_mapping.codec_value != 0) {
    return fail("JPG quality or diagnostics invalid.");
  }

  awj::JpegliImageDecoder decoder;
  auto memory_decoded = decoder.decode_memory(
      std::span<const std::byte>{encoded->encoded.bytes}, "jpglI-memory-test");
  if (!memory_decoded) {
    return fail(memory_decoded.error());
  }
  if (memory_decoded->decoder_id != "jpegli" ||
      memory_decoded->image.width != 3 ||
      memory_decoded->image.height != 3 ||
      memory_decoded->image.pixel_format != awj::PixelFormat::rgba ||
      memory_decoded->image.planes.empty()) {
    return fail("JPG decode_memory result invalid.");
  }
  if (!has_metadata_kind(memory_decoded->image, awj::MetadataKind::icc) ||
      !has_metadata_kind(memory_decoded->image, awj::MetadataKind::exif) ||
      !has_metadata_kind(memory_decoded->image, awj::MetadataKind::xmp)) {
    return fail("JPG metadata was not preserved.");
  }

  const auto path = std::filesystem::temp_directory_path() /
                    "awjimage-jpegli-codec-test.jpg";
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  }

  auto dimensions = decoder.probe_dimensions(path);
  if (!dimensions || dimensions->width != 3 || dimensions->height != 3 ||
      dimensions->opaque_sdr8_yuv420) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return fail(dimensions ? "JPG probe dimensions invalid." : dimensions.error());
  }

  auto file_decoded = decoder.decode(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!file_decoded || file_decoded->decoder_id != "jpegli") {
    return fail(file_decoded ? "JPG file decode id invalid." : file_decoded.error());
  }

  auto stripped = encoder.encode(make_test_image(true), jpegli_settings(90, true));
  if (!stripped) {
    return fail(stripped.error());
  }
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(stripped->encoded.bytes.data()),
                 static_cast<std::streamsize>(stripped->encoded.bytes.size()));
  }
  const auto sdr_dimensions = decoder.probe_dimensions(path);
  std::filesystem::remove(path, ec);
  if (!sdr_dimensions || sdr_dimensions->opaque_sdr8_yuv420)
    return fail("RGB-source auto JPG was incorrectly classified as SDR420 by memory preflight.");
  auto stripped_decoded = decoder.decode_memory(
      std::span<const std::byte>{stripped->encoded.bytes}, "jpglI-strip-test");
  if (!stripped_decoded) {
    return fail(stripped_decoded.error());
  }
  if (!stripped_decoded->image.metadata.empty()) {
    return fail("JPG strip metadata did not remove APP/ICC metadata.");
  }

  auto advanced_settings = jpegli_settings(80, true);
  advanced_settings.requested_chroma_mode = awj::ChromaMode::yuv420;
  advanced_settings.chroma_mode = awj::ChromaMode::yuv420;
  advanced_settings.requested_bit_depth = 8;
  advanced_settings.bit_depth = 8;
  advanced_settings.jpegli_progressive_level = 0;
  advanced_settings.jpegli_optimize_huffman = false;
  auto advanced = encoder.encode(make_test_image(false), advanced_settings);
  if (!advanced) {
    return fail(advanced.error());
  }
  { std::ofstream output{path, std::ios::binary}; output.write(
      reinterpret_cast<const char*>(advanced->encoded.bytes.data()),
      static_cast<std::streamsize>(advanced->encoded.bytes.size())); }
  const auto explicit_420_dimensions = decoder.probe_dimensions(path);
  std::filesystem::remove(path, ec);
  if (!explicit_420_dimensions || !explicit_420_dimensions->opaque_sdr8_yuv420 ||
      !jpeg_sof_matches(advanced->encoded.bytes, awj::ChromaMode::yuv420)) {
    if (!explicit_420_dimensions) std::fprintf(stderr, "JPEGli 探测失败：%s\n", explicit_420_dimensions.error().c_str());
    else std::fprintf(stderr, "JPEGli 探测诊断：depth=%d range=%d opaque420=%d storage=%llu sof420=%d\n",
        explicit_420_dimensions->source_bit_depth, static_cast<int>(explicit_420_dimensions->source_dynamic_range),
        int(explicit_420_dimensions->opaque_sdr8_yuv420), static_cast<unsigned long long>(explicit_420_dimensions->decoded_storage_bytes),
        int(jpeg_sof_matches(advanced->encoded.bytes, awj::ChromaMode::yuv420)));
    return fail("Explicit plain SDR420 JPG was not recognized by memory preflight/SOF.");
  }
  auto advanced_decoded = decoder.decode_memory(
      std::span<const std::byte>{advanced->encoded.bytes}, "jpglI-advanced-test");
  if (!advanced_decoded || advanced_decoded->image.width != 3 ||
      advanced_decoded->image.height != 3) {
    return fail(advanced_decoded ? "JPG advanced decode dimensions invalid."
                                 : advanced_decoded.error());
  }

  auto invalid_settings = jpegli_settings(80, true);
  invalid_settings.jpegli_progressive_level = 2;
  invalid_settings.jpegli_optimize_huffman = false;
  auto invalid = encoder.encode(make_test_image(false), invalid_settings);
  if (invalid ||
      invalid.error().find("渐进 JPEG 需要优化哈夫曼表") == std::string::npos) {
    return fail("JPG invalid progressive/fixed Huffman combination was not rejected.");
  }

  for (const auto size : {std::size_t{1}, std::size_t{3}, std::size_t{65}}) {
    awj::ImageBuffer hdr{.width = size, .height = size,
        .pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
        .sample_representation = awj::SampleRepresentation::ieee_half_float,
        .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
            .color_primaries = 9, .transfer_characteristics = 8, .matrix_coefficients = 0,
            .color_range = 1, .has_hdr_metadata = true, .color_metadata_source = "linear-test",
            .linear_reference_white_nits = 203.0F}};
    hdr.planes.push_back({std::vector<std::byte>(size * size * 8), size * 8});
    for (std::size_t i = 0; i < size * size; ++i) {
      const std::uint16_t samples[]{0x3e00, 0x4000, 0x4200, 0x3c00};
      std::memcpy(hdr.planes.front().bytes.data() + i * 8, samples, 8);
    }
    auto opts = jpegli_settings(100, true);
    opts.resources.memory_limit_bytes = 128 * 1024 * 1024;
    auto enhanced = awj::encode_hdr_jpeg(hdr, opts);
    if (!enhanced) return fail(enhanced.error());
    if (enhanced->encoded.codec_name != "jpegli-iso21496-1" ||
        enhanced->diagnostics.integration_mode != "jpegli-iso21496-1" ||
        enhanced->diagnostics.applied_hdr_metadata != "iso21496-1-gain-map" ||
        enhanced->diagnostics.requested_chroma != "auto" ||
        enhanced->diagnostics.applied_chroma != "444" ||
        !jpeg_sof_matches(enhanced->encoded.bytes, awj::ChromaMode::yuv444))
      return fail("ISO HDR JPEG diagnostics or RGB-source auto SOF sampling are invalid.");
    if (!jpeg_hdr_sdr_compatible(enhanced->encoded.bytes.data(), enhanced->encoded.bytes.size(), size))
      return fail("HDR JPEG SDR compatibility failed.");
    const auto fixture = std::filesystem::temp_directory_path() / "awj-hdr-jpeg-roundtrip.jpg";
    { std::ofstream file(fixture, std::ios::binary); file.write(reinterpret_cast<const char*>(enhanced->encoded.bytes.data()), enhanced->encoded.bytes.size()); }
    auto reconstructed = awj::decode_jpeg_gain_map(fixture, {}, 1);
    auto dimensions = awj::probe_image_dimensions_for_path(fixture, {.allow_wic_fallback = false});
    auto direct_memory = decoder.decode_memory(enhanced->encoded.bytes, "direct ISO HDR JPEG");
    auto direct_file = decoder.decode(fixture);
    auto direct_dimensions = decoder.probe_dimensions(fixture);
    std::filesystem::remove(fixture, ec);
    if (!direct_memory || !direct_file || !direct_dimensions ||
        direct_memory->decoder_id != "iso21496-1-jpeg-gain-map-enhanced" ||
        direct_file->decoder_id != direct_memory->decoder_id ||
        !direct_memory->image.source_info || !direct_memory->image.source_info->source_has_gain_map ||
        direct_dimensions->width != size || direct_dimensions->height != size)
      return fail("JPEGli direct file/memory/probe silently decoded ISO HDR as its SDR base.");
    if (!reconstructed || !*reconstructed || (**reconstructed).image.width != size ||
        !(**reconstructed).image.source_info->source_has_gain_map)
      return fail(reconstructed ? "HDR JPEG did not reconstruct a gain map." : reconstructed.error());
    if (!dimensions || dimensions->width != (**reconstructed).image.width ||
        dimensions->height != (**reconstructed).image.height)
      return fail(dimensions ? "Gain Map dimension probe disagrees with decode." : dimensions.error());
    if (size == 3) {
      for (const auto source : {awj::PixelFormat::yuv420, awj::PixelFormat::yuv422,
          awj::PixelFormat::yuv444, awj::PixelFormat::rgb, awj::PixelFormat::rgba,
          awj::PixelFormat::gray, awj::PixelFormat::unknown}) {
        auto original_sampling = hdr;
        original_sampling.source_info->pixel_format = source;
        auto sample = awj::encode_hdr_jpeg(original_sampling, opts);
        const auto expected = awj::ChromaMode::yuv444;
        if (!sample || !jpeg_sof_matches(sample->encoded.bytes, expected) ||
            sample->diagnostics.requested_chroma != "auto" || !sample->diagnostics.rendition.tone_mapped ||
            sample->diagnostics.applied_chroma != awj::chroma_mode_name(expected))
          return fail(sample ? "Generated SDR JPEG base followed HDR-container sampling instead of its RGB rendition." : sample.error());
        auto existing_sdr = make_sampling_image();
        existing_sdr.width = hdr.width; existing_sdr.height = hdr.height;
        existing_sdr.planes[0].stride = hdr.width * 4;
        existing_sdr.planes[0].bytes.assign(hdr.width * hdr.height * 4, std::byte{0});
        for (std::size_t i = 0; i < hdr.width * hdr.height; ++i) {
          existing_sdr.planes[0].bytes[i * 4 + 1] = std::byte{200};
          existing_sdr.planes[0].bytes[i * 4 + 3] = std::byte{255};
        }
        existing_sdr.source_info = awj::ImageSourceInfo{.pixel_format = source, .bit_depth = 8,
            .color_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 6, .color_range = 1,
            .source_has_gain_map = true, .rendition_origin = awj::RenditionOrigin::gain_map_reconstructed};
        awj::GainMapAlternative existing{.image = std::move(existing_sdr),
            .main_hdr_headroom_log2 = 2, .alternate_hdr_headroom_log2 = 0};
        const auto original_sdr = existing.image.planes[0].bytes;
        auto paired_options = opts;
        paired_options.gain_map_alternative = &existing;
        auto paired = awj::encode_hdr_jpeg(hdr, paired_options);
        const auto sdr_sampling = awj::jpeg_auto_chroma(existing.image);
        if (!paired || !jpeg_sof_matches(paired->encoded.bytes, sdr_sampling) ||
            paired->diagnostics.applied_chroma != awj::chroma_mode_name(sdr_sampling) ||
            !paired->diagnostics.rendition.reused_sdr || paired->diagnostics.rendition.tone_mapped ||
            existing.image.planes[0].bytes != original_sdr)
          return fail(paired ? "Existing SDR JPEG base lost its own sampling or was tone-mapped again." : paired.error());
      }
      auto invalid_chroma = opts;
      invalid_chroma.chroma_mode = awj::ChromaMode::yuv400;
      if (awj::encode_hdr_jpeg(hdr, invalid_chroma)) return fail("HDR JPEG accepted explicit base 400.");
      auto portrait = hdr;
      portrait.height = 5;
      portrait.planes[0].bytes.resize(portrait.planes[0].stride * portrait.height);
      for (std::size_t y = 3; y < 5; ++y)
        std::memcpy(portrait.planes[0].bytes.data() + y * portrait.planes[0].stride,
                    portrait.planes[0].bytes.data(), portrait.planes[0].stride);
      const unsigned char tiff[]{0x49,0x49,0x2a,0,8,0,0,0,1,0,
          0x12,1,3,0,1,0,0,0,6,0,0,0,0,0,0,0};
      awj::MetadataBlock exif{.kind = awj::MetadataKind::exif};
      for (auto value : tiff) exif.bytes.push_back(std::byte{value});
      portrait.metadata.push_back(std::move(exif));
      auto orientation_settings = opts;
      orientation_settings.strip_metadata = false;
      auto rotated = awj::encode_hdr_jpeg(portrait, orientation_settings);
      if (!rotated) return fail(rotated.error());
      { std::ofstream file(fixture, std::ios::binary); file.write(
          reinterpret_cast<const char*>(rotated->encoded.bytes.data()), rotated->encoded.bytes.size()); }
      auto probed = awj::probe_image_dimensions_for_path(fixture, {.allow_wic_fallback = false});
      auto decoded = awj::decode_jpeg_gain_map(fixture);
      std::filesystem::remove(fixture, ec);
      if (!probed || !decoded || !*decoded || probed->width != 5 || probed->height != 3 ||
          (**decoded).image.width != probed->width || (**decoded).image.height != probed->height)
        return fail("Gain Map probe lost portrait Exif rotation");
    }
    const auto& result = (**reconstructed).image;
    double relative_error = 0;
    const float reference[]{1.5F, 2.F, 3.F};
    for (std::size_t y = 0; y < size; ++y) {
      const auto* row = result.planes[0].bytes.data() + y * result.planes[0].stride;
      for (std::size_t x = 0; x < size; ++x) for (int c = 0; c < 3; ++c) {
        const auto value = awj::hdr::hdr_detail::read_sample(result, row, x * 4 + c);
        if (!std::isfinite(value) || value < 0) return fail("HDR JPEG produced invalid linear samples.");
        relative_error += std::abs(value - reference[c]) / reference[c];
      }
    }
    relative_error /= size * size * 3;
    std::printf("HDR JPEG %zux%zu mean_relative_error=%.6f\n", size, size, relative_error);
    if (relative_error > 0.2) return fail("HDR JPEG flat-field reconstruction color error exceeded 20 percent.");
    for (const auto transfer : {16, 18}) {
      auto nonlinear = hdr;
      nonlinear.sample_representation = awj::SampleRepresentation::unorm;
      nonlinear.source_info->transfer_characteristics = transfer;
      nonlinear.source_info->linear_reference_white_nits.reset();
      if (!awj::encode_hdr_jpeg(nonlinear, opts)) return fail("PQ/HLG HDR JPEG input was rejected.");
    }
    opts.visual_quality = 100;
    if (awj::encode_hdr_jpeg(hdr, opts)) return fail("HDR JPEG visual-quality search was accepted.");
    opts.visual_quality.reset(); opts.resources.memory_limit_bytes = 1;
    if (awj::encode_hdr_jpeg(hdr, opts)) return fail("HDR JPEG ignored its memory limit.");
    opts.resources.memory_limit_bytes = 128 * 1024 * 1024;
    std::stop_source cancellation; cancellation.request_stop();
    if (awj::encode_hdr_jpeg(hdr, opts, cancellation.get_token())) return fail("HDR JPEG ignored cancellation.");
  }
  terminate_test_process(0);
}
