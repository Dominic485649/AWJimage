#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <brotli/encode.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/gain_map.h>

#include "codec_color_fixture.h"

import awj.codec;
import awj.config;
import awj.encoding_defaults;
import awj.image;
import awj.jxl_codec;
import awj.large_image_plan;
import awj.resource_planner;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

void push_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(std::byte{static_cast<unsigned char>(value & 0xffu)});
  bytes.push_back(std::byte{static_cast<unsigned char>(value >> 8)});
}

void push_be_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (const unsigned shift : {24u, 16u, 8u, 0u})
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

std::uint32_t read_be_u32(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value{};
  for (std::size_t i = 0; i < 4; ++i) value = (value << 8) | std::to_integer<unsigned char>(bytes[offset + i]);
  return value;
}

struct BoxView {
  std::size_t offset{}, header_size{}, payload_size{};
};

std::optional<BoxView> find_box(std::span<const std::byte> bytes, std::array<char, 4> type) {
  if (JxlSignatureCheck(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()) != JXL_SIG_CONTAINER)
    return std::nullopt;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < 8) return std::nullopt;
    const auto size32 = read_be_u32(bytes, offset);
    std::size_t header_size = 8;
    std::uint64_t box_size = size32;
    if (size32 == 1) {
      if (bytes.size() - offset < 16) return std::nullopt;
      box_size = (static_cast<std::uint64_t>(read_be_u32(bytes, offset + 8)) << 32) | read_be_u32(bytes, offset + 12);
      header_size = 16;
    } else if (size32 == 0) box_size = bytes.size() - offset;
    if (box_size < header_size || box_size > bytes.size() - offset) return std::nullopt;
    if (std::equal(type.begin(), type.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4),
        [](char a, std::byte b) { return static_cast<unsigned char>(a) == std::to_integer<unsigned char>(b); }))
      return BoxView{offset, header_size, static_cast<std::size_t>(box_size) - header_size};
    offset += static_cast<std::size_t>(box_size);
  }
  return std::nullopt;
}

std::expected<std::vector<std::byte>, std::string> serialize_bundle(const JxlGainMapBundle& bundle) {
  std::size_t size{}, written{};
  if (JxlGainMapGetBundleSize(&bundle, &size) != JXL_TRUE || size == 0)
    return std::unexpected{"Cannot size test JHGM bundle."};
  std::vector<std::byte> bytes(size);
  if (JxlGainMapWriteBundle(&bundle, reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size(), &written) != JXL_TRUE ||
      written != size) return std::unexpected{"Cannot serialize test JHGM bundle."};
  return bytes;
}

std::expected<std::vector<std::byte>, std::string> replace_gain_map_box(
    std::span<const std::byte> source, std::span<const std::byte> payload,
    std::array<char, 4> type = {'j', 'h', 'g', 'm'}) {
  const auto box = find_box(source, {'j', 'h', 'g', 'm'});
  if (!box || payload.size() > UINT32_MAX - 8) return std::unexpected{"Missing or oversized test JHGM box."};
  std::vector<std::byte> result{source.begin(), source.begin() + static_cast<std::ptrdiff_t>(box->offset)};
  push_be_u32(result, static_cast<std::uint32_t>(payload.size() + 8));
  for (const char c : type) result.push_back(static_cast<std::byte>(c));
  result.insert(result.end(), payload.begin(), payload.end());
  result.insert(result.end(), source.begin() + static_cast<std::ptrdiff_t>(box->offset + box->header_size + box->payload_size), source.end());
  return result;
}

struct RenderingPair {
  awj::ImageBuffer hdr;
  awj::GainMapAlternative alternate;
};

RenderingPair make_rendering_pair() {
  RenderingPair pair;
  pair.hdr = awj::ImageBuffer{.width = 8, .height = 4, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::none, .bit_depth = 16, .source_info = awj::ImageSourceInfo{
      .pixel_format = awj::PixelFormat::rgba, .bit_depth = 16, .color_primaries = 9,
      .transfer_characteristics = 16, .matrix_coefficients = 0, .color_range = 1,
      .has_hdr_metadata = true, .color_metadata_source = "jxl-test-pq"}};
  pair.alternate.image = awj::ImageBuffer{.width = 8, .height = 4, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::none, .bit_depth = 8, .source_info = awj::ImageSourceInfo{
      .pixel_format = awj::PixelFormat::rgba, .bit_depth = 8, .color_primaries = 1,
      .transfer_characteristics = 13, .matrix_coefficients = 0, .color_range = 1,
      .color_metadata_source = "jxl-test-sdr"}};
  pair.alternate.main_hdr_headroom_log2 = 3.0F;
  pair.alternate.alternate_hdr_headroom_log2 = 0.0F;
  awj::ImagePlane hdr{.stride = 8 * 8};
  awj::ImagePlane sdr{.stride = 8 * 4};
  for (std::size_t y = 0; y < pair.hdr.height; ++y) {
    for (std::size_t x = 0; x < pair.hdr.width; ++x) {
      const double linear = 0.04 + 0.65 * static_cast<double>(x + 2 * y) / 13.0;
      const double srgb = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
      const auto sdr_sample = static_cast<unsigned char>(std::lround(srgb * 255));
      const double nits = 203.0 * linear * std::exp2(1.0 + 0.25 * static_cast<double>(x));
      const double power = std::pow(nits / 10000.0, 2610.0 / 16384.0);
      const double pq = std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * power) /
          (1.0 + 2392.0 / 128.0 * power), 2523.0 / 32.0);
      const auto hdr_sample = static_cast<std::uint16_t>(std::lround(pq * 65535));
      for (int c = 0; c < 3; ++c) {
        push_u16(hdr.bytes, hdr_sample);
        sdr.bytes.push_back(static_cast<std::byte>(sdr_sample));
      }
      push_u16(hdr.bytes, 65535);
      sdr.bytes.push_back(std::byte{255});
    }
  }
  pair.hdr.planes.push_back(std::move(hdr));
  pair.alternate.image.planes.push_back(std::move(sdr));
  return pair;
}

awj::NativeEncodeSettings rendering_settings() {
  awj::NativeEncodeSettings settings{.output_format = awj::OutputFormat::jxl,
      .quality = 100, .speed = 10, .speed_explicit = true};
  settings.applied_alpha = "dropped";
  settings.resources = awj::ResourcePlan{.file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1};
  return settings;
}

struct IntegerJxl {
  JxlBasicInfo info{};
  std::optional<JxlColorEncoding> color;
  std::vector<std::byte> icc;
  std::vector<std::uint16_t> rgba;
};

// This decoder is independent of AWJimage's decoder/normalizer. Request the
// actual codestream's integer range so a matching encoder/decoder scaling bug
// cannot make a 10/12-bit stream appear correct just by round-tripping it.
std::expected<IntegerJxl, std::string> decode_integer_jxl(std::span<const std::byte> bytes) {
  std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder{JxlDecoderCreate(nullptr), &JxlDecoderDestroy};
  if (!decoder || JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
      JxlDecoderSetInput(decoder.get(), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()) != JXL_DEC_SUCCESS)
    return std::unexpected{"Cannot initialize independent integer JXL decoder."};
  JxlDecoderCloseInput(decoder.get());
  IntegerJxl result;
  const JxlPixelFormat format{.num_channels = 4, .data_type = JXL_TYPE_UINT16, .endianness = JXL_NATIVE_ENDIAN, .align = 0};
  bool received_full_image = false;
  for (;;) {
    const auto status = JxlDecoderProcessInput(decoder.get());
    if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT)
      return std::unexpected{"Independent integer JXL decode failed or was truncated."};
    if (status == JXL_DEC_BASIC_INFO) {
      if (JxlDecoderGetBasicInfo(decoder.get(), &result.info) != JXL_DEC_SUCCESS ||
          !result.info.xsize || !result.info.ysize || result.info.bits_per_sample < 8 || result.info.bits_per_sample > 16 ||
          result.info.exponent_bits_per_sample || result.info.alpha_exponent_bits)
        return std::unexpected{"Independent JXL integer basic info is invalid."};
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      JxlColorEncoding color{};
      if (JxlDecoderGetColorAsEncodedProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &color) == JXL_DEC_SUCCESS) {
        result.color = color;
      } else {
        std::size_t size{};
        if (JxlDecoderGetICCProfileSize(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &size) != JXL_DEC_SUCCESS ||
            !size || size > 1024 * 1024)
          return std::unexpected{"Cannot size independent original JXL ICC."};
        result.icc.resize(size);
        if (JxlDecoderGetColorAsICCProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
            reinterpret_cast<std::uint8_t*>(result.icc.data()), result.icc.size()) != JXL_DEC_SUCCESS)
          return std::unexpected{"Cannot read independent original JXL ICC."};
      }
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      std::size_t size{};
      if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &size) != JXL_DEC_SUCCESS || !size ||
          size > 1024 * 1024 || size % sizeof(std::uint16_t))
        return std::unexpected{"Independent JXL integer output size is invalid."};
      result.rgba.resize(size / sizeof(std::uint16_t));
      if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, result.rgba.data(), size) != JXL_DEC_SUCCESS)
        return std::unexpected{"Cannot set independent JXL integer output."};
      const JxlBitDepth depth{.type = JXL_BIT_DEPTH_FROM_CODESTREAM};
      if (JxlDecoderSetImageOutBitDepth(decoder.get(), &depth) != JXL_DEC_SUCCESS)
        return std::unexpected{"Cannot request independent codestream-range JXL output."};
    } else if (status == JXL_DEC_FULL_IMAGE) {
      received_full_image = true;
    } else if (status == JXL_DEC_SUCCESS) {
      if (!received_full_image || result.rgba.size() != static_cast<std::uint64_t>(result.info.xsize) * result.info.ysize * 4)
        return std::unexpected{"Independent JXL decoder did not produce a complete image."};
      return result;
    }
  }
}

awj::ImageBuffer make_depth_fixture(int depth) {
  constexpr std::array<unsigned, 12> values8{0, 1, 2, 16, 63, 64, 127, 128, 129, 191, 254, 255};
  constexpr std::array<unsigned, 12> values16{0, 1, 127, 128, 129, 256, 257, 12345, 32767, 32768, 65534, 65535};
  const auto& values = depth == 8 ? values8 : values16;
  awj::ImageBuffer image{.width = values.size(), .height = 2,
      .pixel_format = awj::PixelFormat::rgba, .alpha_mode = awj::AlphaMode::straight,
      .bit_depth = depth, .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba,
          .bit_depth = depth, .color_primaries = 12, .transfer_characteristics = 13,
          .matrix_coefficients = 0, .color_range = 1}};
  awj::ImagePlane plane{.stride = values.size() * 4 * static_cast<std::size_t>(depth / 8)};
  const auto append = [&](unsigned value) {
    if (depth == 8) plane.bytes.push_back(static_cast<std::byte>(value));
    else push_u16(plane.bytes, static_cast<std::uint16_t>(value));
  };
  for (std::size_t i = 0; i < values.size(); ++i) {
    for (std::size_t c = 0; c < 3; ++c) append(values[(i + 4 * c) % values.size()]);
    append((1u << depth) - 1);
  }
  // Isolate varying alpha from hidden RGB so the precision assertion does not
  // accidentally depend on an unrelated transparent-color optimization.
  for (const auto value : values) { append(0); append(0); append(0); append(value); }
  image.planes.push_back(std::move(plane));
  return image;
}

bool samples_match_depth(const awj::ImageBuffer& source, const IntegerJxl& decoded, int depth, bool keep_alpha) {
  if (decoded.info.xsize != source.width || decoded.info.ysize != source.height ||
      decoded.info.bits_per_sample != static_cast<unsigned>(depth) ||
      decoded.info.alpha_bits != static_cast<unsigned>(keep_alpha ? depth : 0) ||
      decoded.rgba.size() != source.width * source.height * 4) return false;
  const auto source_maximum = (std::uint64_t{1} << source.bit_depth) - 1;
  const auto target_maximum = (std::uint64_t{1} << depth) - 1;
  for (std::size_t i = 0; i < source.width * source.height; ++i) {
    for (std::size_t c = 0; c < (keep_alpha ? 4u : 3u); ++c) {
      const auto index = i * 4 + c;
      unsigned value{};
      if (source.bit_depth == 8) value = std::to_integer<unsigned char>(source.planes[0].bytes[index]);
      else {
        std::uint16_t sample{};
        std::memcpy(&sample, source.planes[0].bytes.data() + index * 2, sizeof(sample));
        value = sample;
      }
      // Exact independent round-to-nearest UNORM reference, including endpoints.
      const auto expected = (value * target_maximum + source_maximum / 2) / source_maximum;
      if (decoded.rgba[index] != expected) return false;
    }
  }
  return true;
}

bool is_p3_srgb(const JxlColorEncoding& color) {
  const auto close = [](double a, double b) { return std::abs(a - b) <= 3e-4; };
  const bool p3 = color.primaries == JXL_PRIMARIES_P3 || (color.primaries == JXL_PRIMARIES_CUSTOM &&
      close(color.primaries_red_xy[0], 0.680) && close(color.primaries_red_xy[1], 0.320) &&
      close(color.primaries_green_xy[0], 0.265) && close(color.primaries_green_xy[1], 0.690) &&
      close(color.primaries_blue_xy[0], 0.150) && close(color.primaries_blue_xy[1], 0.060));
  const bool d65 = color.white_point == JXL_WHITE_POINT_D65 || (color.white_point == JXL_WHITE_POINT_CUSTOM &&
      close(color.white_point_xy[0], 0.3127) && close(color.white_point_xy[1], 0.3290));
  return color.color_space == JXL_COLOR_SPACE_RGB && p3 && d65 && color.transfer_function == JXL_TRANSFER_FUNCTION_SRGB;
}

int encoding_policy_regressions() {
  awj::JXLImageEncoder encoder;
  if (encoder.capabilities().bit_depths != std::vector<int>{8, 10, 12, 16})
    return fail("JXL depth capabilities do not match the implemented integer codestreams.");
  const std::array<std::optional<int>, 5> requested{std::nullopt, 8, 10, 12, 16};
  for (const int input_depth : {8, 16}) {
    auto image = make_depth_fixture(input_depth);
    const auto original = image.planes[0].bytes;
    for (const bool keep_alpha : {false, true}) {
      for (const auto depth : requested) {
        auto settings = rendering_settings();
        settings.bit_depth = depth;
        settings.applied_alpha = keep_alpha ? "kept" : "dropped";
        std::vector<std::byte> cached_rgb;
        if (input_depth == 8 && !keep_alpha && !depth) {
          for (std::size_t i = 0; i < image.width * image.height; ++i)
            cached_rgb.insert(cached_rgb.end(), original.begin() + static_cast<std::ptrdiff_t>(i * 4),
                original.begin() + static_cast<std::ptrdiff_t>(i * 4 + 3));
          settings.jxl_rgb8_input = cached_rgb;
        }
        const auto encoded = encoder.encode(image, settings);
        if (!encoded) return fail(encoded.error());
        const auto decoded = decode_integer_jxl(encoded->encoded.bytes);
        if (!decoded) return fail(decoded.error());
        const int expected_depth = depth.value_or(16);
        if (!samples_match_depth(image, *decoded, expected_depth, keep_alpha) ||
            encoded->diagnostics.applied_bit_depth != expected_depth)
          return fail("JXL actual BasicInfo/alpha/independent integer samples do not match auto/explicit depth.");
        if (!decoded->color || !is_p3_srgb(*decoded->color) || find_box(encoded->encoded.bytes, {'j', 'h', 'g', 'm'}))
          return fail("JXL integer precision conversion relabeled P3 SDR or invented a Gain Map.");
        if (image.planes[0].bytes != original) return fail("JXL precision conversion modified caller pixels.");
        if (expected_depth < input_depth && encoded->lossless) return fail("JXL precision reduction claimed source losslessness.");
      }
    }
    for (const int invalid : {0, 9, 14, 32}) {
      auto settings = rendering_settings();
      settings.bit_depth = invalid;
      if (encoder.encode(image, settings)) return fail("JXL accepted unsupported output precision.");
    }
    std::stop_source cancel;
    cancel.request_stop();
    for (const auto depth : requested) {
      auto settings = rendering_settings();
      settings.bit_depth = depth;
      if (encoder.encode(image, settings, cancel.get_token())) return fail("JXL precision path ignored cancellation.");
    }
    for (const int transfer : {16, 18}) {
      auto hdr = image;
      hdr.source_info->color_primaries = transfer == 16 ? 9 : 12;
      hdr.source_info->transfer_characteristics = transfer;
      hdr.source_info->has_hdr_metadata = true;
      for (const bool strip : {false, true}) {
        auto settings = rendering_settings();
        settings.applied_alpha = "kept";
        settings.strip_metadata = strip;
        auto encoded = encoder.encode(hdr, settings);
        if (!encoded) return fail(encoded.error());
        auto decoded = decode_integer_jxl(encoded->encoded.bytes);
        if (!decoded) return fail(decoded.error());
        if (!samples_match_depth(hdr, *decoded, 16, true) || !decoded->color ||
            decoded->color->transfer_function != static_cast<JxlTransferFunction>(transfer) ||
            decoded->color->primaries != (transfer == 16 ? JXL_PRIMARIES_2100 : JXL_PRIMARIES_P3) ||
            find_box(encoded->encoded.bytes, {'j', 'h', 'g', 'm'}))
          return fail("JXL ordinary HDR strip lost original PQ/HLG/P3 semantics, rescaled pixels, or invented a map.");
      }
    }
  }
  auto p3 = make_depth_fixture(16);
  const auto profile = awj::test::display_p3_srgb_icc();
  if (!profile) return fail(profile.error());
  p3.metadata.push_back({.kind = awj::MetadataKind::icc, .bytes = *profile});
  // Only the real profile explains these samples. Falling back to untagged
  // sRGB under strip must fail even though no Gain Map is present.
  p3.source_info->color_primaries.reset();
  p3.source_info->transfer_characteristics.reset();
  p3.source_info->color_metadata_source = "icc-only-display-p3";
  p3.metadata.push_back({.kind = awj::MetadataKind::exif, .bytes = {std::byte{'E'}}});
  p3.metadata.push_back({.kind = awj::MetadataKind::xmp, .bytes = {std::byte{'X'}}});
  for (const int depth : {10, 12}) {
    for (const bool strip : {false, true}) {
      auto settings = rendering_settings();
      settings.bit_depth = depth;
      settings.applied_alpha = "kept";
      settings.applied_icc = "kept";
      settings.strip_metadata = strip;
      const auto encoded = encoder.encode(p3, settings);
      if (!encoded) return fail(encoded.error());
      const auto decoded = decode_integer_jxl(encoded->encoded.bytes);
      if (!decoded) return fail(decoded.error());
      if (!samples_match_depth(p3, *decoded, depth, true) ||
          (decoded->color ? !is_p3_srgb(*decoded->color) : decoded->icc != *profile))
        return fail("JXL kept ICC/structured P3 under strip changed interpretation or 10/12-bit sample scale.");
      if (strip && (find_box(encoded->encoded.bytes, {'E', 'x', 'i', 'f'}) || find_box(encoded->encoded.bytes, {'x', 'm', 'l', ' '})))
        return fail("JXL ICC preservation under strip also retained optional Exif/XMP.");
    }
  }
  const std::array<std::byte, 1> not_a_jpeg{std::byte{0}};
  const auto incompatible_transcode = encoder.encode_jpeg_bitstream(not_a_jpeg, rendering_settings());
  if (incompatible_transcode || incompatible_transcode.error().find("8-bit") == std::string::npos)
    return fail("Direct JPEG transcode bypassed the JXL auto-16 output plan.");
  return 0;
}

bool close_rgb(const awj::ImageBuffer& a, const awj::ImageBuffer& b, double tolerance) {
  if (a.width != b.width || a.height != b.height || a.planes.empty() || b.planes.empty() ||
      a.pixel_format != awj::PixelFormat::rgba || b.pixel_format != awj::PixelFormat::rgba || a.geometry || b.geometry ||
      a.sample_representation != awj::SampleRepresentation::unorm || b.sample_representation != awj::SampleRepresentation::unorm)
    return false;
  const auto sample = [](const awj::ImageBuffer& image, std::size_t x, std::size_t y, std::size_t c) {
    const auto* row = image.planes[0].bytes.data() + y * image.planes[0].stride;
    if (image.bit_depth == 8) return std::to_integer<unsigned char>(row[x * 4 + c]) / 255.0;
    std::uint16_t value{};
    std::memcpy(&value, row + (x * 4 + c) * 2, 2);
    return value / 65535.0;
  };
  for (std::size_t y = 0; y < a.height; ++y)
    for (std::size_t x = 0; x < a.width; ++x)
      for (std::size_t c = 0; c < 3; ++c)
        if (std::abs(sample(a, x, y, c) - sample(b, x, y, c)) > tolerance) return false;
  return true;
}

bool gain_map_failure(const std::expected<awj::ImageDecodeResult, std::string>& result) {
  return !result && result.error().starts_with("Gain Map: ISO 21496-1");
}

std::expected<std::vector<std::byte>, std::string> compressed_gain_map_payload(std::span<const std::byte> payload) {
  const auto bound = BrotliEncoderMaxCompressedSize(payload.size());
  if (bound == 0) return std::unexpected{"Cannot size compressed JHGM fixture."};
  std::vector<std::byte> bytes(bound + 4);
  std::memcpy(bytes.data(), "jhgm", 4);
  auto size = bound;
  if (BrotliEncoderCompress(5, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, payload.size(),
      reinterpret_cast<const std::uint8_t*>(payload.data()), &size,
      reinterpret_cast<std::uint8_t*>(bytes.data() + 4)) != BROTLI_TRUE)
    return std::unexpected{"Cannot compress JHGM fixture."};
  bytes.resize(size + 4);
  return bytes;
}

int gain_map_regressions(const std::filesystem::path& test_root) {
  auto pair = make_rendering_pair();
  pair.hdr.metadata.push_back(awj::MetadataBlock{.kind = awj::MetadataKind::xmp,
      .bytes = {std::byte{'<'}, std::byte{'x'}, std::byte{'/'}, std::byte{'>'}}});
  auto settings = rendering_settings();
  settings.gain_map_alternative = &pair.alternate;
  awj::JXLImageEncoder encoder;
  awj::JXLImageDecoder decoder;
  auto encoded = encoder.encode(pair.hdr, settings);
  if (!encoded) return fail(encoded.error());
  std::stop_source cancelled_decode;
  cancelled_decode.request_stop();
  const auto cancelled = decoder.decode_memory(encoded->encoded.bytes, "cancelled-jhgm",
      {.copy_metadata_payloads = false, .gain_map_mode = awj::GainMapDecodeMode::preserve_both,
       .stop_token = cancelled_decode.get_token()});
  if (cancelled || cancelled.error().find("取消") == std::string::npos)
    return fail("JHGM decode did not forward/honor its caller's stop token.");
  if (encoded->lossless) return fail("Regenerated/quantized JHGM pairs must not claim sample-wise losslessness.");
  if (!encoded->diagnostics.rendition.gain_map_generated || encoded->diagnostics.rendition.gain_map_width != 8 ||
      encoded->diagnostics.rendition.gain_map_height != 4 || encoded->diagnostics.rendition.gain_map_bit_depth != 8 ||
      encoded->diagnostics.timing.gain_map_compute_workers != 1)
    return fail("JHGM actual full-resolution 8-bit map or bounded compute diagnostics are missing.");
  const auto box = find_box(encoded->encoded.bytes, {'j', 'h', 'g', 'm'});
  if (!box) return fail("HDR base plus SDR alternate did not emit the standard jhgm box.");
  const auto payload = std::span<const std::byte>{encoded->encoded.bytes}.subspan(box->offset + box->header_size, box->payload_size);
  JxlGainMapBundle bundle{};
  std::size_t read{};
  if (JxlGainMapReadBundle(&bundle, reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size(), &read) != JXL_TRUE ||
      read != payload.size() || !bundle.gain_map_metadata_size || !bundle.gain_map_size ||
      bundle.has_color_encoding != JXL_TRUE || bundle.color_encoding.transfer_function != JXL_TRANSFER_FUNCTION_SRGB ||
      JxlSignatureCheck(bundle.gain_map, bundle.gain_map_size) != JXL_SIG_CODESTREAM)
    return fail("JHGM standard bundle/alternate color/naked auxiliary codestream invalid.");
  if (!(bundle.gain_map_metadata[4] & 4u)) return fail("HDR-base JHGM must encode backwardDirection.");
  const auto auxiliary = decode_integer_jxl({reinterpret_cast<const std::byte*>(bundle.gain_map), bundle.gain_map_size});
  if (!auxiliary || auxiliary->info.bits_per_sample != 8 || auxiliary->info.alpha_bits != 0 ||
      auxiliary->info.xsize != 8 || auxiliary->info.ysize != 4)
    return fail(auxiliary ? "Primary auto-16 policy changed the full-resolution 8-bit Gain Map auxiliary." : auxiliary.error());
  auto base_only = replace_gain_map_box(encoded->encoded.bytes, {}, {'f', 'r', 'e', 'e'});
  if (!base_only) return fail(base_only.error());
  auto raw_base = decoder.decode_memory(*base_only, "jhgm-main-without-map");
  if (!raw_base || !raw_base->image.source_info || raw_base->image.source_info->transfer_characteristics != 16 ||
      !close_rgb(raw_base->image, pair.hdr, 0.0) || raw_base->gain_map_alternative)
    return fail(raw_base ? "JHGM wrote an SDR/changed base instead of the original HDR pixels." : raw_base.error());

  auto hdr = decoder.decode_memory(encoded->encoded.bytes, "hdr-base-default");
  if (!hdr) return fail(hdr.error());
  if (hdr->gain_map_alternative || !hdr->image.source_info || !hdr->image.source_info->source_has_gain_map ||
      hdr->image.source_info->transfer_characteristics != 16 || hdr->image.source_info->color_primaries != 9 ||
      hdr->image.bit_depth != 16 || !close_rgb(hdr->image, pair.hdr, 0.0))
    return fail("JHGM default HDR rendering changed/dropped the original HDR base.");
  const awj::DecodeOptions sdr_options{.copy_metadata_payloads = false, .gain_map_mode = awj::GainMapDecodeMode::sdr_only};
  const awj::DecodeOptions both_options{.copy_metadata_payloads = false, .gain_map_mode = awj::GainMapDecodeMode::preserve_both};
  auto sdr = decoder.decode_memory(encoded->encoded.bytes, "hdr-base-sdr", sdr_options);
  if (!sdr) return fail(sdr.error());
  if (sdr->gain_map_alternative || !sdr->image.source_info || sdr->image.source_info->has_hdr_metadata ||
      sdr->image.source_info->transfer_characteristics != 13 || sdr->image.bit_depth != 8 ||
      !close_rgb(sdr->image, pair.alternate.image, 8.0 / 255.0))
    return fail("JHGM sdr_only did not reconstruct the authored SDR alternate.");
  awj::DecodeDiagnostics render_diagnostics;
  auto measured_options = both_options;
  measured_options.diagnostics = &render_diagnostics;
  auto both = awj::JXLImageDecoder{2}.decode_memory(encoded->encoded.bytes, "hdr-base-both", measured_options);
  if (!both) return fail(both.error());
  if (render_diagnostics.timing.gain_map_render_seconds < 0 ||
      render_diagnostics.timing.gain_map_render_workers < 1 || render_diagnostics.timing.gain_map_render_workers > 2)
    return fail("JHGM rendering did not forward its actual bounded worker/time diagnostics.");
  if (!both->gain_map_alternative || !close_rgb(both->image, pair.hdr, 0.0) ||
      !close_rgb(both->gain_map_alternative->image, pair.alternate.image, 8.0 / 255.0) ||
      both->gain_map_alternative->main_hdr_headroom_log2 <= both->gain_map_alternative->alternate_hdr_headroom_log2 ||
      both->gain_map_alternative->alternate_hdr_headroom_log2 != 0.0F)
    return fail("JHGM preserve_both did not own both correctly ordered renditions/headrooms.");

  const auto path = test_root / "gain-map.jxl";
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()), static_cast<std::streamsize>(encoded->encoded.bytes.size()));
    if (!output) return fail("Cannot write JHGM probe fixture.");
  }
  auto dimensions = decoder.probe_dimensions(path);
  if (!dimensions) return fail(dimensions.error());
  if (dimensions->width != 8 || dimensions->height != 4 || dimensions->source_bit_depth != 16 ||
      dimensions->source_has_alpha || dimensions->source_dynamic_range != awj::ImageDynamicRangeHint::hdr ||
      !dimensions->source_has_gain_map || dimensions->gain_map_width != 8 || dimensions->gain_map_height != 4 ||
      dimensions->decoded_storage_bytes < 8 * 4 * 16)
    return fail("JHGM header-only dimensions/storage profile invalid.");
  auto file_both = decoder.decode(path, both_options);
  if (!file_both || !file_both->gain_map_alternative)
    return fail(file_both ? "JHGM file DecodeOptions lost the alternate." : file_both.error());

  auto stripped_settings = settings;
  stripped_settings.strip_metadata = true;
  auto stripped = encoder.encode(pair.hdr, stripped_settings);
  if (!stripped) return fail(stripped.error());
  if (!find_box(stripped->encoded.bytes, {'j', 'h', 'g', 'm'}) ||
      find_box(stripped->encoded.bytes, {'x', 'm', 'l', ' '}) || find_box(stripped->encoded.bytes, {'E', 'x', 'i', 'f'}))
    return fail("strip removed functional JHGM or retained optional metadata boxes.");
  auto stripped_both = decoder.decode_memory(stripped->encoded.bytes, "stripped-jhgm", both_options);
  if (!stripped_both) return fail(stripped_both.error());
  if (!stripped_both->gain_map_alternative || !stripped_both->image.metadata.empty() ||
      !stripped_both->gain_map_alternative->image.metadata.empty() ||
      !close_rgb(stripped_both->image, pair.hdr, 0.0) ||
      !close_rgb(stripped_both->gain_map_alternative->image, pair.alternate.image, 8.0 / 255.0))
    return fail("Stripped JHGM no longer preserves both presentations.");

  auto reencode_settings = rendering_settings();
  reencode_settings.gain_map_alternative = &both->gain_map_alternative.value();
  auto reencoded = encoder.encode(both->image, reencode_settings);
  if (!reencoded) return fail(reencoded.error());
  auto round_trip = decoder.decode_memory(reencoded->encoded.bytes, "reencoded-jhgm", both_options);
  if (!round_trip || !round_trip->gain_map_alternative ||
      !close_rgb(round_trip->image, both->image, 0.0) ||
      !close_rgb(round_trip->gain_map_alternative->image, both->gain_map_alternative->image, 8.0 / 255.0))
    return fail(round_trip ? "Reencoding a decoded JHGM pair dropped/changed a presentation." : round_trip.error());

  // The HDR container's authored headroom is not the maximum pixel value.
  // An entirely dark HDR rendition must still retain its separate SDR endpoint.
  auto dark_pair = make_rendering_pair();
  for (std::size_t y = 0; y < dark_pair.hdr.height; ++y)
    for (std::size_t x = 0; x < dark_pair.hdr.width; ++x)
      std::fill_n(dark_pair.hdr.planes[0].bytes.data() + y * dark_pair.hdr.planes[0].stride + x * 8, 6, std::byte{0});
  auto dark_settings = rendering_settings();
  dark_settings.gain_map_alternative = &dark_pair.alternate;
  auto dark_encoded = encoder.encode(dark_pair.hdr, dark_settings);
  if (!dark_encoded) return fail(dark_encoded.error());
  auto dark_both = decoder.decode_memory(dark_encoded->encoded.bytes, "dark-hdr-authored-headroom", both_options);
  if (!dark_both || !dark_both->gain_map_alternative || !close_rgb(dark_both->image, dark_pair.hdr, 0.0) ||
      !close_rgb(dark_both->gain_map_alternative->image, dark_pair.alternate.image, 8.0 / 255.0) ||
      dark_both->gain_map_alternative->main_hdr_headroom_log2 != dark_pair.alternate.main_hdr_headroom_log2)
    return fail(dark_both ? "Dark HDR pair lost authored headroom or used the wrong gain direction." : dark_both.error());

  auto brotli = compressed_gain_map_payload(payload);
  if (!brotli) return fail(brotli.error());
  auto compressed = replace_gain_map_box(encoded->encoded.bytes, *brotli, {'b', 'r', 'o', 'b'});
  if (!compressed) return fail(compressed.error());
  auto compressed_both = decoder.decode_memory(*compressed, "brob-jhgm", both_options);
  if (!compressed_both || !compressed_both->gain_map_alternative ||
      !close_rgb(compressed_both->gain_map_alternative->image, both->gain_map_alternative->image, 0.0))
    return fail(compressed_both ? "Compressed JHGM was not recognized without metadata copying." : compressed_both.error());

  const auto reject_bundle = [&](const JxlGainMapBundle& invalid) -> bool {
    auto body = serialize_bundle(invalid);
    if (!body) return false;
    auto container = replace_gain_map_box(encoded->encoded.bytes, *body);
    return container && gain_map_failure(decoder.decode_memory(*container, "invalid-jhgm", both_options));
  };
  std::vector<std::uint8_t> bad_metadata{bundle.gain_map_metadata, bundle.gain_map_metadata + bundle.gain_map_metadata_size};
  auto invalid = bundle;
  invalid.gain_map_metadata = bad_metadata.data();
  const std::size_t denominator = bad_metadata[4] & 8u ? 5 : 9;
  std::fill_n(bad_metadata.begin() + static_cast<std::ptrdiff_t>(denominator), 4, std::uint8_t{0});
  if (!reject_bundle(invalid)) return fail("Zero-denominator JHGM metadata did not fail terminally.");
  std::copy_n(bundle.gain_map_metadata, bad_metadata.size(), bad_metadata.begin());
  bad_metadata[4] ^= 4u;
  if (!reject_bundle(invalid)) return fail("Inconsistent JHGM backwardDirection did not fail terminally.");
  std::copy_n(bundle.gain_map_metadata, bad_metadata.size(), bad_metadata.begin());
  bad_metadata[4] |= 1u;
  if (!reject_bundle(invalid)) return fail("Reserved JHGM metadata flags did not fail terminally.");
  std::copy_n(bundle.gain_map_metadata, bad_metadata.size(), bad_metadata.begin());
  bad_metadata[3] = 1;
  if (!reject_bundle(invalid)) return fail("Unsupported JHGM metadata version did not fail terminally.");
  bad_metadata.assign(bundle.gain_map_metadata, bundle.gain_map_metadata + bundle.gain_map_metadata_size);
  bad_metadata.push_back(0);
  invalid.gain_map_metadata = bad_metadata.data();
  invalid.gain_map_metadata_size = static_cast<std::uint16_t>(bad_metadata.size());
  if (!reject_bundle(invalid)) return fail("Trailing JHGM ISO metadata was silently ignored.");

  invalid = bundle;
  invalid.gain_map = reinterpret_cast<const std::uint8_t*>(encoded->encoded.bytes.data());
  invalid.gain_map_size = static_cast<std::uint32_t>(encoded->encoded.bytes.size());
  if (!reject_bundle(invalid)) return fail("Nested JHGM/container auxiliary was accepted.");
  std::vector<std::byte> auxiliary_with_trailer(bundle.gain_map_size);
  std::memcpy(auxiliary_with_trailer.data(), bundle.gain_map, bundle.gain_map_size);
  push_be_u32(auxiliary_with_trailer, static_cast<std::uint32_t>(payload.size() + 8));
  for (const char c : std::array{'j', 'h', 'g', 'm'}) auxiliary_with_trailer.push_back(static_cast<std::byte>(c));
  auxiliary_with_trailer.insert(auxiliary_with_trailer.end(), payload.begin(), payload.end());
  invalid = bundle;
  invalid.gain_map = reinterpret_cast<const std::uint8_t*>(auxiliary_with_trailer.data());
  invalid.gain_map_size = static_cast<std::uint32_t>(auxiliary_with_trailer.size());
  if (!reject_bundle(invalid)) return fail("Naked auxiliary with trailing/nested JHGM data was accepted.");
  invalid = bundle;
  invalid.gain_map_size = 2;
  if (!reject_bundle(invalid)) return fail("Truncated JHGM auxiliary was accepted.");
  auto duplicate = encoded->encoded.bytes;
  push_be_u32(duplicate, static_cast<std::uint32_t>(payload.size() + 8));
  for (const char c : std::array{'j', 'h', 'g', 'm'}) duplicate.push_back(static_cast<std::byte>(c));
  duplicate.insert(duplicate.end(), payload.begin(), payload.end());
  if (!gain_map_failure(decoder.decode_memory(duplicate, "duplicate-jhgm", both_options)))
    return fail("Duplicate trailing JHGM was silently ignored.");
  auto truncated = encoded->encoded.bytes;
  truncated.pop_back();
  if (!gain_map_failure(decoder.decode_memory(truncated, "truncated-jhgm", both_options)))
    return fail("Truncated JHGM container was silently ignored.");
  return 0;
}

struct ScopedTestDirectory {
  std::filesystem::path path;
  ~ScopedTestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

}  // namespace

int main(int argc, char** argv) {
  // CTest runs in this project's build tree. Keep all fixtures in one newly
  // created task-owned directory rather than scattering them in a shared /tmp.
  const auto test_root = std::filesystem::current_path() /
      ("jxl-codec-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code directory_error;
  if (!std::filesystem::create_directory(test_root, directory_error)) return fail("Cannot create project-local JXL test directory.");
  ScopedTestDirectory test_directory{test_root};
  if (const int result = encoding_policy_regressions(); result != 0) return result;
  if (argc == 2 && std::string_view{argv[1]} == "--encoding-policy-only") {
    std::puts("JXL auto/explicit BasicInfo depth, independent integer RGB/alpha, HDR/ICC strip and cancellation passed.");
    return 0;
  }
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

  awj::JXLImageEncoder encoder;
  auto encoded = encoder.encode(
      image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                 .quality = 100,
                                 .speed = 10,
                                 .speed_explicit = true,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!encoded) {
    return fail(encoded.error());
  }
  if (!encoded->lossless || encoded->encoded.bytes.empty()) {
    return fail("JXL lossless encode result invalid.");
  }
  if (encoded->diagnostics.speed_mapping.codec_value != 1) {
    return fail("JXL speed mapping invalid.");
  }

  auto default_speed = encoder.encode(
      image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                 .quality = 90,
                                 .speed = awj::default_speed_for(awj::OutputFormat::jxl),
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!default_speed || default_speed->diagnostics.speed_mapping.codec_value != awj::encoding_defaults::default_jxl_effort) {
    return fail(default_speed ? "JXL default effort diagnostics invalid." : default_speed.error());
  }

  const auto path = test_directory.path /
                    "awjimage-jxl-codec-test.jxl";
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  }

  awj::JXLImageDecoder decoder;
  auto plain_dimensions = decoder.probe_dimensions(path);
  if (!plain_dimensions || plain_dimensions->width != 2 || plain_dimensions->height != 2 ||
      plain_dimensions->source_bit_depth != 16 || plain_dimensions->source_has_alpha ||
      plain_dimensions->source_has_gain_map || plain_dimensions->source_dynamic_range != awj::ImageDynamicRangeHint::sdr ||
      plain_dimensions->decoded_storage_bytes != 32)
    return fail(plain_dimensions ? "Plain JXL dimensions/profile regressed." : plain_dimensions.error());
  auto decoded = decoder.decode(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!decoded) {
    return fail(decoded.error());
  }
  if (decoded->image.width != 2 || decoded->image.height != 2 || decoded->image.bit_depth != 16 ||
      decoded->image.pixel_format != awj::PixelFormat::rgba ||
      decoded->image.planes.empty()) {
    return fail("JXL decode result invalid.");
  }

  awj::ImagePlane hdr_plane{.stride = 16};
  for (const std::uint16_t value : {std::uint16_t{0}, std::uint16_t{32768},
                                    std::uint16_t{65535}, std::uint16_t{65535},
                                    std::uint16_t{65535}, std::uint16_t{0},
                                    std::uint16_t{32768}, std::uint16_t{65535}}) {
    push_u16(hdr_plane.bytes, value);
  }
  awj::ImageBuffer hdr_image{.width = 2,
                             .height = 1,
                             .pixel_format = awj::PixelFormat::rgba,
                             .alpha_mode = awj::AlphaMode::straight,
                             .bit_depth = 16};
  hdr_image.planes.push_back(std::move(hdr_plane));
  auto hdr_encoded = encoder.encode(
      hdr_image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                 .quality = 100,
                                 .speed = 10,
                                 .speed_explicit = true,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!hdr_encoded || hdr_encoded->encoded.bytes.empty()) {
    return fail(hdr_encoded ? "JXL 16-bit encode result invalid." : hdr_encoded.error());
  }

  const auto hdr_path = test_directory.path /
                        "awjimage-jxl-codec-test-16.jxl";
  {
    std::ofstream output{hdr_path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(hdr_encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(hdr_encoded->encoded.bytes.size()));
  }
  auto generic16_dimensions = decoder.probe_dimensions(hdr_path);
  if (!generic16_dimensions || generic16_dimensions->source_bit_depth != 16 ||
      generic16_dimensions->source_dynamic_range != awj::ImageDynamicRangeHint::sdr || generic16_dimensions->source_has_gain_map)
    return fail(generic16_dimensions ? "16-bit JXL probe guessed HDR from precision alone." : generic16_dimensions.error());
  auto hdr_decoded = decoder.decode(hdr_path);
  std::filesystem::remove(hdr_path, ec);
  if (!hdr_decoded || hdr_decoded->image.bit_depth != 16 ||
      !hdr_decoded->image.source_info ||
      hdr_decoded->image.source_info->bit_depth != 16 ||
      hdr_decoded->image.source_info->has_hdr_metadata) {
    return fail(hdr_decoded ? "JXL generic 16-bit decode color semantics invalid."
                            : hdr_decoded.error());
  }

  auto pq_image = hdr_image;
  pq_image.source_info = awj::ImageSourceInfo{
      .pixel_format = awj::PixelFormat::rgba,
      .bit_depth = 16,
      .color_primaries = 9,
      .transfer_characteristics = 16,
      .matrix_coefficients = 0,
      .color_range = 1,
      .has_hdr_metadata = true,
      .color_metadata_source = "test-bt2020-pq"};
  auto pq_encoded = encoder.encode(
      pq_image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                 .quality = 100,
                                 .speed = 10,
                                 .speed_explicit = true,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!pq_encoded || pq_encoded->encoded.bytes.empty()) {
    return fail(pq_encoded ? "JXL PQ encode result invalid." : pq_encoded.error());
  }
  const auto pq_path = test_directory.path /
                       "awjimage-jxl-codec-test-pq.jxl";
  {
    std::ofstream output{pq_path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(pq_encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(pq_encoded->encoded.bytes.size()));
  }
  auto pq_dimensions = decoder.probe_dimensions(pq_path);
  if (!pq_dimensions || pq_dimensions->source_dynamic_range != awj::ImageDynamicRangeHint::hdr ||
      pq_dimensions->source_has_gain_map || pq_dimensions->source_bit_depth != 16)
    return fail(pq_dimensions ? "PQ JXL header probe lost explicit HDR semantics." : pq_dimensions.error());
  auto pq_decoded = decoder.decode(pq_path);
  std::filesystem::remove(pq_path, ec);
  if (!pq_decoded || !pq_decoded->image.source_info ||
      pq_decoded->image.source_info->color_primaries != 9 ||
      pq_decoded->image.source_info->transfer_characteristics != 16 ||
      !pq_decoded->image.source_info->has_hdr_metadata) {
    return fail(pq_decoded ? "JXL PQ HDR semantics were not preserved."
                           : pq_decoded.error());
  }

  return gain_map_regressions(test_directory.path);
}
