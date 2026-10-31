#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>
#include <lcms2.h>

#include "codec_color_fixture.h"

import awj.codec;
import awj.config;
import awj.image;
import awj.large_image_plan;
import awj.png_codec;
import awj.visual_metrics;

namespace fs = std::filesystem;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class T> T require(awj::compat::expected<T, std::string> result) {
  if (!result) throw std::runtime_error(result.error());
  return std::move(*result);
}
std::uint16_t sample(const awj::ImageBuffer& image, std::size_t i) {
  const auto* ptr = image.planes[0].bytes.data() + i * (image.bit_depth / 8);
  std::uint16_t value{};
  if (image.bit_depth == 16) std::memcpy(&value, ptr, 2);
  else value = std::to_integer<std::uint8_t>(*ptr);
  return value;
}
void put_sample(awj::ImageBuffer& image, std::size_t i, unsigned input) {
  const auto value = static_cast<std::uint16_t>(input);
  auto* ptr = image.planes[0].bytes.data() + i * (image.bit_depth / 8);
  if (image.bit_depth == 16) std::memcpy(ptr, &value, 2);
  else *ptr = static_cast<std::byte>(value);
}
awj::ImageBuffer make_image(int depth) {
  awj::ImageBuffer image{.width = 256, .height = 256,
      .pixel_format = awj::PixelFormat::rgba, .alpha_mode = awj::AlphaMode::straight,
      .bit_depth = depth};
  image.planes.push_back({.bytes = std::vector<std::byte>(256 * 256 * 4 * (depth / 8)),
                         .stride = static_cast<std::size_t>(256 * 4 * (depth / 8))});
  const auto full = (1u << depth) - 1;
  for (unsigned i = 0; i < 65536; ++i) {
    put_sample(image, i * 4, i & full);
    put_sample(image, i * 4 + 1, (full - i) & full);
    put_sample(image, i * 4 + 2, (i * 31337u) & full);
    put_sample(image, i * 4 + 3, (i * 1777u) & full);
  }
  return image;
}
awj::ImageBuffer roundtrip(const awj::ImageBuffer& image, int quality, const fs::path& path) {
  // These precision/quality regressions deliberately hold storage depth fixed.
  auto encoded = require(awj::PngImageEncoder{}.encode(image,
      {.output_format = awj::OutputFormat::png, .quality = quality, .bit_depth = image.bit_depth}));
  check(encoded.final_quality == quality, "PNG quality was discarded");
  if (quality == 100) check(encoded.lossless, "q100 reported lossy");
  check(std::to_integer<int>(encoded.encoded.bytes[24]) == image.bit_depth, "IHDR storage depth changed");
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded.encoded.bytes.data()), encoded.encoded.bytes.size());
    check(static_cast<bool>(output), "write failed");
  }
  return require(awj::PngImageDecoder{}.decode(path)).image;
}

void check_expanded_precision(const fs::path& path, bool palette) {
  std::vector<std::byte> bytes{std::byte{137}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
      std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
  std::array<std::byte, 13> header{};
  header[3] = header[7] = std::byte{1};
  header[8] = std::byte{2}; // 2-bit grayscale or palette index, expanded to RGBA8.
  header[9] = palette ? std::byte{3} : std::byte{0};
  awj::png_detail::append_png_chunk(bytes, "IHDR", header);
  const std::array<std::byte, 3> color_bits{std::byte{5}, std::byte{6}, std::byte{7}};
  const std::array<std::byte, 1> gray_bits{std::byte{2}};
  if (palette) {
    awj::png_detail::append_png_chunk(bytes, "sBIT", color_bits);
    const std::array<std::byte, 3> color{std::byte{99}, std::byte{133}, std::byte{199}};
    awj::png_detail::append_png_chunk(bytes, "PLTE", color);
    const std::array<std::byte, 1> alpha{std::byte{117}};
    awj::png_detail::append_png_chunk(bytes, "tRNS", alpha);
  } else awj::png_detail::append_png_chunk(bytes, "sBIT", gray_bits);
  const std::array<unsigned char, 2> raw{0, static_cast<unsigned char>(palette ? 0 : 0x80)};
  std::array<std::byte, 64> compressed{};
  uLongf count = compressed.size();
  check(compress(reinterpret_cast<Bytef*>(compressed.data()), &count, raw.data(), raw.size()) == Z_OK,
        "fixture deflate failed");
  awj::png_detail::append_png_chunk(bytes, "IDAT", std::span{compressed}.first(count));
  awj::png_detail::append_png_chunk(bytes, "IEND", {});
  {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(static_cast<bool>(out), "fixture write failed");
  }
  const auto dimensions = require(awj::PngImageDecoder{}.probe_dimensions(path));
  check(dimensions.source_bit_depth == 2 && dimensions.source_has_alpha == palette &&
      dimensions.decoded_storage_bytes == 4 && dimensions.source_dynamic_range == awj::ImageDynamicRangeHint::sdr,
      "expanded gray/palette PNG probe storage/alpha/depth incorrect");
  const auto decoded = require(awj::PngImageDecoder{}.decode(path)).image;
  check(decoded.bit_depth == 8 && decoded.significant_bits.has_value(), "expanded sBIT missing");
  const auto bits = *decoded.significant_bits;
  check(bits.red == (palette ? 5 : 2) && bits.green == (palette ? 6 : 2) &&
        bits.blue == (palette ? 7 : 2) && bits.alpha == 8, "expanded sBIT mapping incorrect");
  check(sample(decoded, 3) == (palette ? 117 : 255), "tRNS/filler alpha changed");
}

awj::ImageDimensions probe_bytes(const fs::path& path, const std::vector<std::byte>& bytes) {
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(static_cast<bool>(output), "probe fixture write failed");
  }
  return require(awj::PngImageDecoder{}.probe_dimensions(path));
}
std::vector<std::byte> with_chunk(const std::vector<std::byte>& png, std::string_view tag,
    std::span<const std::byte> payload) {
  std::vector<std::byte> bytes(png.begin(), png.begin() + 33);
  awj::png_detail::append_png_chunk(bytes, tag, payload);
  bytes.insert(bytes.end(), png.begin() + 33, png.end());
  return bytes;
}
std::vector<std::byte> icc_chunk(std::optional<int> transfer) {
  auto* profile = cmsCreate_sRGBProfile();
  check(profile != nullptr, "ICC fixture creation failed");
  struct Guard { cmsHPROFILE profile; ~Guard() { cmsCloseProfile(profile); } } guard{profile};
  if (transfer) {
    const cmsVideoSignalType cicp{9, static_cast<cmsUInt8Number>(*transfer), 0, 1};
    check(cmsWriteTag(profile, cmsSigcicpTag, &cicp) != 0, "ICC CICP fixture tag failed");
  }
  cmsUInt32Number size{};
  check(cmsSaveProfileToMem(profile, nullptr, &size) != 0, "ICC fixture sizing failed");
  std::vector<std::byte> raw(size);
  check(cmsSaveProfileToMem(profile, raw.data(), &size) != 0, "ICC fixture serialization failed");
  std::vector<std::byte> compressed(compressBound(size));
  uLongf count = compressed.size();
  check(compress(reinterpret_cast<Bytef*>(compressed.data()), &count,
      reinterpret_cast<const Bytef*>(raw.data()), size) == Z_OK, "ICC fixture compression failed");
  std::vector<std::byte> payload{std::byte{'p'}, std::byte{'r'}, std::byte{'o'}, std::byte{'b'}, std::byte{'e'}, std::byte{0}, std::byte{0}};
  payload.insert(payload.end(), compressed.begin(), compressed.begin() + count);
  return payload;
}
void check_probe_memory_profile(const fs::path& path) {
  using Range = awj::ImageDynamicRangeHint;
  for (const int depth : {8, 16}) {
    awj::ImageBuffer image{.width = 1, .height = 1, .pixel_format = awj::PixelFormat::rgba,
        .alpha_mode = awj::AlphaMode::straight, .bit_depth = depth};
    image.planes.push_back({std::vector<std::byte>(4 * (depth / 8), std::byte{255}), static_cast<std::size_t>(4 * (depth / 8))});
    const auto base = require(awj::PngImageEncoder{}.encode(image, {.quality = 100, .bit_depth = depth})).encoded.bytes;
    const auto plain = probe_bytes(path, base);
    check(plain.width == 1 && plain.height == 1 && plain.pixel_count == 1 &&
        plain.source_bit_depth == depth && plain.source_has_alpha &&
        plain.decoded_storage_bytes == static_cast<std::uint64_t>(depth / 2), "PNG RGBA probe storage/alpha/depth incorrect");
    check(plain.source_dynamic_range == (depth == 8 ? Range::sdr : Range::unknown) && !plain.opaque_sdr8_yuv420 &&
        !plain.source_has_gain_map, "untagged PNG decode-default or conservative high-depth profile incorrect");
    if (depth == 8) {
      const auto interpreted = require(awj::PngImageDecoder{}.decode(path)).image;
      check(interpreted.source_info && interpreted.source_info->transfer_characteristics == 13 &&
          !interpreted.source_info->has_hdr_metadata, "untagged PNG memory profile disagrees with its actual sRGB decoder branch");
    }
    for (const int transfer : {16, 18, 13, 2, 8}) {
      const std::array<std::byte, 4> cicp{std::byte{9}, static_cast<std::byte>(transfer), std::byte{0}, std::byte{1}};
      const auto expected = transfer == 16 || transfer == 18 ? Range::hdr : transfer == 13 ? Range::sdr : Range::unknown;
      const auto dimensions = probe_bytes(path, with_chunk(base, "cICP", cicp));
      check(dimensions.source_dynamic_range == expected && dimensions.decoded_storage_bytes == plain.decoded_storage_bytes,
          "PNG CICP transfer/decode storage probe incorrect");
      const auto from_icc = probe_bytes(path, with_chunk(base, "iCCP", icc_chunk(transfer)));
      check(from_icc.source_dynamic_range == expected, "PNG ICC CICP dynamic range probe incorrect");
    }
    const auto unknown_icc = probe_bytes(path, with_chunk(base, "iCCP", icc_chunk(std::nullopt)));
    check(unknown_icc.source_dynamic_range == Range::unknown, "PNG unclassified ICC assumed SDR");
    const std::array<std::byte, 1> srgb{std::byte{0}};
    check(probe_bytes(path, with_chunk(base, "sRGB", srgb)).source_dynamic_range == Range::sdr, "PNG explicit sRGB probe missing");
    std::vector<std::byte> gamma;
    awj::png_detail::append_be_u32(gamma, 45455);
    check(probe_bytes(path, with_chunk(base, "gAMA", gamma)).source_dynamic_range == Range::sdr, "PNG known gamma probe missing");
    gamma.clear(); awj::png_detail::append_be_u32(gamma, 100000);
    check(probe_bytes(path, with_chunk(base, "gAMA", gamma)).source_dynamic_range == Range::unknown, "PNG linear gamma assumed SDR");
    const std::array<std::byte, 4> unknown_cicp{std::byte{9}, std::byte{2}, std::byte{0}, std::byte{1}};
    check(probe_bytes(path, with_chunk(with_chunk(base, "sRGB", srgb), "cICP", unknown_cicp)).source_dynamic_range == Range::unknown,
        "PNG unknown CICP fell back to SDR metadata");
    std::vector<std::byte> clli;
    awj::png_detail::append_be_u32(clli, 1000u * 10000u); awj::png_detail::append_be_u32(clli, 400u * 10000u);
    check(probe_bytes(path, with_chunk(base, "cLLI", clli)).source_dynamic_range == Range::hdr, "PNG content-light metadata ignored");

    // Header probing must not decode or allocate the compressed image pixels.
    auto without_pixels = std::vector<std::byte>(base.begin(), base.begin() + 33);
    const std::array<std::byte, 1> invalid_zlib{std::byte{0}};
    awj::png_detail::append_png_chunk(without_pixels, "IDAT", invalid_zlib);
    awj::png_detail::append_png_chunk(without_pixels, "IEND", {});
    check(probe_bytes(path, without_pixels).decoded_storage_bytes == plain.decoded_storage_bytes,
        "PNG metadata probe depends on pixel decoding");

    auto huge_header = std::array<std::byte, 13>{};
    std::copy_n(base.begin() + 16, 13, huge_header.begin());
    for (const std::size_t axis : {0u, 4u}) {
      huge_header[axis] = std::byte{0x7f};
      huge_header[axis + 1] = huge_header[axis + 2] = huge_header[axis + 3] = std::byte{0xff};
    }
    std::vector<std::byte> huge(base.begin(), base.begin() + 8);
    awj::png_detail::append_png_chunk(huge, "IHDR", huge_header);
    awj::png_detail::append_png_chunk(huge, "IDAT", invalid_zlib);
    awj::png_detail::append_png_chunk(huge, "IEND", {});
    {
      std::ofstream output{path, std::ios::binary};
      output.write(reinterpret_cast<const char*>(huge.data()), huge.size());
      check(static_cast<bool>(output), "oversized PNG fixture write failed");
    }
    check(!awj::PngImageDecoder{}.probe_dimensions(path), "PNG probe accepted decoded storage overflow/runtime-limit violation");
    auto bad_crc = base; bad_crc[29] ^= std::byte{1};
    {
      std::ofstream output{path, std::ios::binary};
      output.write(reinterpret_cast<const char*>(bad_crc.data()), bad_crc.size());
      check(static_cast<bool>(output), "bad PNG CRC fixture write failed");
    }
    check(!awj::PngImageDecoder{}.probe_dimensions(path), "PNG probe ignored header CRC");
  }
}

awj::ImageBuffer make_depth_fixture(int depth) {
  constexpr std::array<unsigned, 12> values8{0, 1, 2, 16, 63, 64, 127, 128, 129, 191, 254, 255};
  constexpr std::array<unsigned, 12> values16{0, 1, 127, 128, 129, 256, 257, 12345, 32767, 32768, 65534, 65535};
  const auto& values = depth == 8 ? values8 : values16;
  awj::ImageBuffer image{.width = values.size(), .height = 2,
      .pixel_format = awj::PixelFormat::rgba, .alpha_mode = awj::AlphaMode::straight,
      .bit_depth = depth, .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba,
          .bit_depth = depth, .color_primaries = 1, .transfer_characteristics = 13,
          .matrix_coefficients = 0, .color_range = 1}};
  const auto stride = values.size() * 4 * static_cast<std::size_t>(depth / 8);
  image.planes.push_back({std::vector<std::byte>(stride * 2), stride});
  const auto maximum = (1u << depth) - 1;
  for (std::size_t i = 0; i < values.size(); ++i) {
    for (std::size_t c = 0; c < 3; ++c) put_sample(image, i * 4 + c, values[(i + 4 * c) % values.size()]);
    put_sample(image, i * 4 + 3, maximum);
    put_sample(image, (i + values.size()) * 4 + 3, values[i]);
  }
  return image;
}

void check_rescaled_rgba(const awj::ImageBuffer& source, const awj::ImageBuffer& decoded, int depth) {
  check(decoded.width == source.width && decoded.height == source.height && decoded.bit_depth == depth &&
      decoded.alpha_mode == awj::AlphaMode::straight, "PNG precision conversion changed dimensions/alpha/storage");
  const auto source_maximum = (std::uint64_t{1} << source.bit_depth) - 1;
  const auto target_maximum = (std::uint64_t{1} << depth) - 1;
  for (std::size_t i = 0; i < source.width * source.height * 4; ++i) {
    // Independent integer reference, not the production pixel converter.
    const auto expected = (sample(source, i) * target_maximum + source_maximum / 2) / source_maximum;
    check(sample(decoded, i) == expected, "PNG decoded sample does not match independent UNORM rescaling");
  }
}

void check_bit_depth_policy() {
  awj::PngImageEncoder encoder;
  const std::array<std::optional<int>, 3> requested{std::nullopt, 8, 16};
  for (const int input_depth : {8, 16}) {
    auto image = make_depth_fixture(input_depth);
    const auto original = image.planes[0].bytes;
    for (const auto depth : requested) {
      auto encoded = require(encoder.encode(image, {.output_format = awj::OutputFormat::png,
          .quality = 100, .bit_depth = depth}));
      const int expected_depth = depth.value_or(8);
      check(encoded.encoded.bytes.size() >= 33 && std::to_integer<int>(encoded.encoded.bytes[24]) == expected_depth &&
          encoded.encoded.bytes[25] == std::byte{6}, "PNG IHDR ignored auto/explicit bit depth or alpha");
      check(encoded.diagnostics.applied_bit_depth == expected_depth, "PNG diagnostics disagree with IHDR");
      auto decoded = require(awj::PngImageDecoder{}.decode_memory(encoded.encoded.bytes, "depth-policy")).image;
      check_rescaled_rgba(image, decoded, expected_depth);
      check(image.planes[0].bytes == original, "PNG depth conversion modified caller storage");
      if (expected_depth < input_depth) check(!encoded.lossless, "PNG precision reduction claimed source losslessness");
    }
    for (const int invalid : {0, 1, 10, 12, 32}) {
      check(!encoder.encode(image, {.quality = 100, .bit_depth = invalid}), "PNG accepted an unsupported requested depth");
    }
    awj::compat::stop_source cancel;
    cancel.request_stop();
    for (const auto depth : requested)
      check(!encoder.encode(image, {.quality = 100, .bit_depth = depth}, cancel.get_token()), "PNG policy conversion ignored cancellation");
  }
  auto precise_alpha = make_depth_fixture(8);
  precise_alpha.significant_bits = awj::SignificantBits{8, 8, 8, 2};
  for (std::size_t i = 0; i < precise_alpha.width * precise_alpha.height; ++i)
    put_sample(precise_alpha, i * 4 + 3, static_cast<unsigned>(i % 4) * 85u);
  const auto promoted = require(encoder.encode(precise_alpha, {.quality = 100, .bit_depth = 16}));
  const auto decoded = require(awj::PngImageDecoder{}.decode_memory(promoted.encoded.bytes, "promoted-alpha")).image;
  check_rescaled_rgba(precise_alpha, decoded, 16);
  check(decoded.significant_bits && decoded.significant_bits->red == 8 && decoded.significant_bits->alpha == 2,
      "PNG promotion falsely claimed new RGB/alpha precision");
}

void check_ordinary_color_contract() {
  awj::PngImageEncoder encoder;
  awj::PngImageDecoder decoder;
  for (const int input_depth : {8, 16}) {
    for (const int transfer : {16, 18}) {
      auto hdr = make_depth_fixture(input_depth);
      hdr.source_info->color_primaries = transfer == 16 ? 9 : 12;
      hdr.source_info->transfer_characteristics = transfer;
      hdr.source_info->has_hdr_metadata = true;
      hdr.source_info->content_light = awj::HdrContentLightMetadata{1000, 400};
      hdr.metadata.push_back({.kind = awj::MetadataKind::exif, .bytes = {std::byte{'E'}}});
      hdr.metadata.push_back({.kind = awj::MetadataKind::xmp, .bytes = {std::byte{'X'}}});
      for (const bool strip : {false, true}) {
        const auto encoded = require(encoder.encode(hdr, {.quality = 100, .strip_metadata = strip}));
        check(encoded.encoded.bytes.size() >= 33 && encoded.encoded.bytes[24] == std::byte{16}, "PNG HDR auto did not write 16-bit IHDR");
        const auto color = require(awj::png_detail::inspect_color_chunks(encoded.encoded.bytes, "ordinary-hdr"));
        check(color.has_cicp && color.color_primaries == hdr.source_info->color_primaries &&
            color.transfer_characteristics == transfer && color.matrix_coefficients == 0 && color.color_range == 1,
            "PNG ordinary HDR strip lost or relabeled necessary CICP");
        check(color.content_light.has_value() == !strip, "PNG ordinary HDR strip retained optional cLLI");
        const auto decoded = require(decoder.decode_memory(encoded.encoded.bytes, "ordinary-hdr")).image;
        check_rescaled_rgba(hdr, decoded, 16);
        check(decoded.source_info && decoded.source_info->has_hdr_metadata && !decoded.source_info->source_has_gain_map,
            "PNG ordinary HDR acquired a map or lost HDR interpretation");
        if (strip) check(decoded.metadata.empty(), "PNG HDR strip retained optional Exif/XMP");
      }
    }
    auto p3 = make_depth_fixture(input_depth);
    p3.source_info->color_primaries = 12;
    const auto profile = require(awj::test::display_p3_srgb_icc());
    p3.metadata.push_back({.kind = awj::MetadataKind::icc, .bytes = profile});
    p3.metadata.push_back({.kind = awj::MetadataKind::exif, .bytes = {std::byte{'E'}}});
    p3.metadata.push_back({.kind = awj::MetadataKind::xmp, .bytes = {std::byte{'X'}}});
    for (const bool strip : {false, true}) {
      const auto encoded = require(encoder.encode(p3, {.quality = 100, .applied_icc = "kept", .strip_metadata = strip}));
      check(encoded.encoded.bytes.size() >= 33 && encoded.encoded.bytes[24] == std::byte{8}, "PNG P3 SDR auto ignored compatibility depth");
      check(!require(awj::png_detail::inspect_color_chunks(encoded.encoded.bytes, "p3-icc")).has_cicp,
          "PNG emitted CICP alongside an authoritative retained ICC");
      const auto decoded = require(decoder.decode_memory(encoded.encoded.bytes, "p3-icc")).image;
      check_rescaled_rgba(p3, decoded, 8);
      check(decoded.metadata.size() == 1 && decoded.metadata[0].kind == awj::MetadataKind::icc &&
          decoded.metadata[0].bytes == profile, "PNG strip deleted or changed necessary ordinary P3 SDR ICC");
      check(decoded.source_info && !decoded.source_info->source_has_gain_map, "PNG ICC-only SDR gained a synthetic map");
    }
  }
}

void check_gain_map_color_contract(const fs::path& path) {
  for (const int depth : {8, 16}) {
    awj::ImageBuffer image{.width = 4, .height = 2,
        .pixel_format = awj::PixelFormat::rgba, .alpha_mode = awj::AlphaMode::straight,
        .bit_depth = depth};
    const auto sample_bytes = static_cast<std::size_t>(depth / 8);
    image.planes.push_back({std::vector<std::byte>(8 * 4 * sample_bytes), 4 * 4 * sample_bytes});
    for (unsigned i = 0; i < 8 * 4; ++i) put_sample(image, i, (i * 7919u + 7u) & ((1u << depth) - 1));
    image.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba,
        .bit_depth = depth, .color_primaries = 12, .transfer_characteristics = 13,
        .matrix_coefficients = 0, .color_range = 1,
        .content_light = awj::HdrContentLightMetadata{1000, 400},
        .color_metadata_source = "source-icc-verified-cicp"};
    const auto original = image.planes[0].bytes;
    for (const bool gain_map : {false, true}) {
      image.source_info->source_has_gain_map = gain_map;
      for (const bool strip : {false, true}) {
        const auto encoded = require(awj::PngImageEncoder{}.encode(image,
            {.output_format = awj::OutputFormat::png, .quality = 100, .bit_depth = depth, .strip_metadata = strip}));
        const auto color = require(awj::png_detail::inspect_color_chunks(encoded.encoded.bytes, "gain-map-color"));
        check(color.has_cicp, "PNG strip deleted the pixel interpretation of ordinary/Gain Map P3 samples");
        if (color.has_cicp) {
          check(color.color_primaries == 12 && color.transfer_characteristics == 13 &&
              color.matrix_coefficients == 0 && color.color_range == 1,
              "PNG changed the normalized P3/sRGB sample interpretation");
        }
        check(color.content_light.has_value() == !strip, "PNG strip kept optional cLLI");
        {
          std::ofstream output{path, std::ios::binary};
          output.write(reinterpret_cast<const char*>(encoded.encoded.bytes.data()), encoded.encoded.bytes.size());
          check(static_cast<bool>(output), "functional-color fixture write failed");
        }
        const auto decoded = require(awj::PngImageDecoder{}.decode(path)).image;
        check(image.planes[0].bytes == original && decoded.planes[0].bytes == original &&
            decoded.width == image.width && decoded.height == image.height && decoded.bit_depth == depth &&
            decoded.alpha_mode == image.alpha_mode, "PNG functional-color preservation changed RGBA samples");
        if (color.has_cicp) {
          check(decoded.source_info && decoded.source_info->color_primaries == 12 &&
              decoded.source_info->transfer_characteristics == 13, "PNG decoder lost functional CICP");
        }
      }
    }
  }
}

// Optional reproducible measurement path; uses the actual encoder and project metrics.
int benchmark(const fs::path& inputs, const fs::path& outputs) {
  check(fs::is_directory(inputs) && !fs::exists(outputs), "benchmark needs inputs and a fresh output directory");
  fs::create_directories(outputs);
  std::ofstream csv{outputs / "metrics.csv"};
  csv << "sample,depth,quality,rgb_bits,bytes,encode_ms,psnr_rgb_code_values,gmsd,ms_ssim\n";
  for (const auto& file : fs::directory_iterator(inputs)) {
    if (file.path().extension() != ".png") continue;
    const auto image = require(awj::PngImageDecoder{}.decode(file.path())).image;
    const auto reference = require(awj::make_luma_image(image));
    for (int quality : {1, 25, 50, 75, 90, 99, 100}) {
      const auto started = std::chrono::steady_clock::now();
      const auto encoded = require(awj::PngImageEncoder{}.encode(image, {.quality = quality, .bit_depth = image.bit_depth}));
      const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      const auto path = outputs / (file.path().stem().string() + "-q" + std::to_string(quality) + ".png");
      {
        std::ofstream out{path, std::ios::binary};
        out.write(reinterpret_cast<const char*>(encoded.encoded.bytes.data()), encoded.encoded.bytes.size());
        check(static_cast<bool>(out), "benchmark output failed");
      }
      const auto decoded = require(awj::PngImageDecoder{}.decode(path)).image;
      const auto metrics = require(awj::calculate_visual_metrics_cpu(reference, require(awj::make_luma_image(decoded))));
      double squared = 0;
      for (std::size_t i = 0; i < image.width * image.height; ++i) {
        check(sample(image, i * 4 + 3) == sample(decoded, i * 4 + 3), "benchmark alpha changed");
        for (unsigned c = 0; c < 3; ++c) {
          const double difference = double(sample(image, i * 4 + c)) - sample(decoded, i * 4 + c);
          squared += difference * difference;
        }
      }
      const double mse = squared / (3.0 * image.width * image.height);
      const double full = (1u << image.bit_depth) - 1;
      const double psnr = mse == 0 ? INFINITY : 10 * std::log10(full * full / mse);
      csv << file.path().stem().string() << ',' << image.bit_depth << ',' << quality << ','
          << (decoded.significant_bits ? decoded.significant_bits->red : decoded.bit_depth) << ','
          << encoded.encoded.bytes.size() << ',' << ms << ',' << psnr << ','
          << metrics.raw_gmsd << ',' << metrics.raw_ms_ssim << '\n';
      csv.flush();
    }
  }
  check(static_cast<bool>(csv), "benchmark CSV failed");
  return 0;
}

int main(int argc, char** argv) try {
  if (argc == 4 && std::string_view{argv[1]} == "--benchmark") return benchmark(argv[2], argv[3]);
  // CTest's working directory is the project's build tree; do not scatter
  // task-owned fixtures in the user's global temporary directory.
  const auto directory = fs::current_path() /
      ("awj-png-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  check(fs::create_directory(directory), "test directory not isolated");
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{directory};
  const auto path = directory / "roundtrip.png";
  check_bit_depth_policy();
  check_ordinary_color_contract();
  check_gain_map_color_contract(path);
  if (argc == 2 && (std::string_view{argv[1]} == "--gain-map-color-only" ||
                   std::string_view{argv[1]} == "--encoding-policy-only")) {
    std::puts("PNG auto/explicit IHDR depth, independent RGBA scaling, ordinary HDR/P3 ICC strip and Gain Map color passed.");
    return 0;
  }
  check_expanded_precision(path, false);
  check_expanded_precision(path, true);
  check_probe_memory_profile(path);
  awj::PngImageEncoder encoder;
  check(encoder.capabilities().min_quality == 1 && encoder.capabilities().max_quality == 100,
        "PNG quality capability incorrect");
  for (int depth : {8, 16}) {
    auto image = make_image(depth);
    const auto original = image.planes[0].bytes;
    for (int quality : {1, 25, 50, 75, 99, 100}) {
      const auto decoded = roundtrip(image, quality, path);
      check(image.planes[0].bytes == original, "encoder changed caller pixels");
      if (quality == 100) {
        check(decoded.planes[0].bytes == original, "q100 pixels differ");
        check(!decoded.significant_bits, "q100 invented precision metadata");
        continue;
      }
      check(decoded.significant_bits.has_value(), "quantized PNG lacks sBIT");
      const auto bits = decoded.significant_bits->red;
      check(bits >= (depth == 16 ? 10 : 4) && bits < depth &&
            decoded.significant_bits->green == bits && decoded.significant_bits->blue == bits &&
            decoded.significant_bits->alpha == depth, "invalid channel precision");
      const unsigned full = (1u << depth) - 1;
      const unsigned levels = (1u << bits) - 1;
      for (unsigned i = 0; i < 65536; ++i) {
        check(sample(decoded, i * 4 + 3) == sample(image, i * 4 + 3), "alpha changed");
        for (unsigned c = 0; c < 3; ++c) {
          const auto a = sample(image, i * 4 + c), b = sample(decoded, i * 4 + c);
          check(std::abs(int(a) - int(b)) <= int((full + levels) / (2 * levels) + 1), "rounding error exceeds bound");
          if (a == 0 || a == full) check(a == b, "endpoint changed");
          check((b >> (depth - bits)) == (std::uint64_t(a) * levels + full / 2) / full,
                "sBIT does not describe recoverable high bits");
        }
      }
      auto reread = roundtrip(decoded, 100, path);
      check(reread.planes[0].bytes == decoded.planes[0].bytes &&
            reread.significant_bits->red == bits, "q100 lost sBIT or changed quantized pixels");
    }
    auto reduced = roundtrip(image, 1, path);
    reduced.significant_bits->green = depth;
    reduced.significant_bits->alpha = depth - 1;
    auto reread = roundtrip(reduced, 100, path);
    check(reread.significant_bits->red == reduced.significant_bits->red &&
          reread.significant_bits->green == depth && reread.significant_bits->alpha == depth - 1,
          "per-channel sBIT was collapsed");
    image.significant_bits = awj::SignificantBits{0, depth, depth, depth};
    check(!encoder.encode(image, {.quality = 100}), "invalid precision accepted");
    image.significant_bits.reset();
    image.sample_representation = awj::SampleRepresentation::ieee_half_float;
    check(!encoder.encode(image, {.quality = 100}), "half float treated as unorm");
    image.sample_representation = awj::SampleRepresentation::unorm;
    check(!encoder.encode(image, {.quality = 0}) && !encoder.encode(image, {.quality = 101}), "invalid quality accepted");
    awj::compat::stop_source cancel;
    cancel.request_stop();
    check(!encoder.encode(image, {.quality = 50}, cancel.get_token()), "cancellation ignored");
  }
  std::puts("PNG output policy, actual IHDR depth, HDR/ICC strip, precision, alpha, sBIT, endpoints and cancellation passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
