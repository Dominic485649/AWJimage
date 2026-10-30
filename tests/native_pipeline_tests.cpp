#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <expected>
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
#include <wincodec.h>
#include <wrl/client.h>
#include <avif/avif.h>
#include <jxl/decode.h>

import awj.config;
import awj.codec;
import awj.core;
import awj.encoding_defaults;
import awj.animation_decoder;
import awj.avif_aom_codec;
import awj.image;
import awj.jpegli_codec;
import awj.gain_map;
import awj.jxl_codec;
import awj.jxr_codec;
import awj.native_backend;
import awj.png_codec;
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

// Minimal uncompressed TIFFs keep the decoder's RGB/RGBA/gray/unknown
// provenance distinct, without relying on an encoder that expands all to RGBA.
bool write_source_tiff(const std::filesystem::path& path, awj::PixelFormat format) {
  constexpr std::uint32_t width = 16, height = 16;
  const bool cmyk = format == awj::PixelFormat::unknown;
  const bool alpha = format == awj::PixelFormat::rgba;
  const std::uint16_t channels = format == awj::PixelFormat::gray ? 1 : format == awj::PixelFormat::rgb ? 3 : 4;
  const std::uint16_t count = static_cast<std::uint16_t>(10 + cmyk + alpha);
  const std::uint32_t bits_offset = 8u + 2u + count * 12u + 4u;
  const std::uint32_t data_offset = bits_offset + channels * 2u;
  struct Entry { std::uint16_t tag, type; std::uint32_t count, value; };
  std::vector<Entry> entries{{256, 4, 1, width}, {257, 4, 1, height},
      {258, 3, channels, channels == 1 ? 8u : bits_offset}, {259, 3, 1, 1},
      {262, 3, 1, cmyk ? 5u : channels == 1 ? 1u : 2u},
      {273, 4, 1, data_offset}, {277, 3, 1, channels}, {278, 4, 1, height},
      {279, 4, 1, width * height * channels}, {284, 3, 1, 1}};
  if (cmyk) entries.push_back({332, 3, 1, 1}); // CMYK ink set, not an RGB/alpha source.
  if (alpha) entries.push_back({338, 3, 1, 2});
  std::vector<std::byte> bytes;
  const auto u32 = [&](std::uint32_t value) {
    push_u16(bytes, static_cast<std::uint16_t>(value));
    push_u16(bytes, static_cast<std::uint16_t>(value >> 16));
  };
  push_u16(bytes, 0x4949); push_u16(bytes, 42); u32(8); push_u16(bytes, count);
  for (const auto& entry : entries) {
    push_u16(bytes, entry.tag); push_u16(bytes, entry.type); u32(entry.count); u32(entry.value);
  }
  u32(0);
  for (unsigned c = 0; c < channels; ++c) push_u16(bytes, 8);
  for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
    for (unsigned c = 0; c < channels; ++c) {
      const unsigned value = channels == 1 ? (x + y) * 8 : c == 0 ? x * 17
                           : c == 1 ? y * 17 : c == 2 ? (x + y) * 8 : alpha ? 255 : 0;
      bytes.push_back(std::byte{static_cast<unsigned char>(value)});
    }
  std::ofstream stream{path, std::ios::binary};
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(stream);
}

std::vector<std::string> csv_record(std::string_view text) {
  std::vector<std::string> fields(1);
  bool quoted = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '"') {
      if (quoted && i + 1 < text.size() && text[i + 1] == '"') {
        fields.back().push_back('"'); ++i;
      } else quoted = !quoted;
    } else if (ch == ',' && !quoted) fields.emplace_back();
    else if (ch != '\r' || quoted) fields.back().push_back(ch);
  }
  return fields;
}

bool csv_sampling_matches(std::string_view text, std::span<const awj::EncodeResult> results) {
  std::size_t offset = 0;
  const auto line = [&] {
    const auto end = text.find('\n', offset);
    const auto value = text.substr(offset, end == std::string_view::npos ? text.size() - offset : end - offset);
    offset = end == std::string_view::npos ? text.size() : end + 1;
    return value;
  };
  const auto header = csv_record(line());
  const auto requested = std::ranges::find(header, "requested_chroma");
  const auto applied = std::ranges::find(header, "applied_chroma");
  if (requested == header.end() || applied == header.end()) return false;
  const auto requested_column = static_cast<std::size_t>(requested - header.begin());
  const auto applied_column = static_cast<std::size_t>(applied - header.begin());
  for (const auto& result : results) {
    if (offset >= text.size()) return false;
    const auto fields = csv_record(line());
    if (fields.size() <= std::max(requested_column, applied_column) ||
        fields[requested_column] != result.requested_chroma || fields[applied_column] != result.applied_chroma)
      return false;
  }
  return true;
}

awj::ImageBuffer solid_rgba8(unsigned r, unsigned g, unsigned b) {
  awj::ImageBuffer image{.width = 16, .height = 16, .pixel_format = awj::PixelFormat::rgba,
      .bit_depth = 8, .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba,
          .bit_depth = 8, .color_primaries = 1, .transfer_characteristics = 13,
          .matrix_coefficients = 0, .color_range = 1}};
  image.planes.push_back({std::vector<std::byte>(16 * 16 * 4), 16 * 4});
  for (std::size_t i = 0; i < 16 * 16; ++i) {
    auto* pixel = image.planes[0].bytes.data() + i * 4;
    pixel[0] = std::byte(r); pixel[1] = std::byte(g); pixel[2] = std::byte(b); pixel[3] = std::byte{255};
  }
  return image;
}
void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::ofstream output{path, std::ios::binary};
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!output) fail("Cannot write native pair/poster test fixture.");
}
bool same_sdr_pixels(const awj::ImageBuffer& a, const awj::ImageBuffer& b, unsigned tolerance = 0) {
  if (a.width != b.width || a.height != b.height || a.bit_depth != 8 || b.bit_depth != 8 ||
      a.pixel_format != awj::PixelFormat::rgba || b.pixel_format != awj::PixelFormat::rgba) return false;
  for (std::size_t y = 0; y < a.height; ++y) for (std::size_t x = 0; x < a.width; ++x)
    for (std::size_t c = 0; c < 3; ++c) {
      const auto pa = std::to_integer<int>(a.planes[0].bytes[y * a.planes[0].stride + x * 4 + c]);
      const auto pb = std::to_integer<int>(b.planes[0].bytes[y * b.planes[0].stride + x * 4 + c]);
      if (static_cast<unsigned>(std::abs(pa - pb)) > tolerance) return false;
    }
  return true;
}
int explicit_fp16_color(const std::filesystem::path& root, const std::filesystem::path& output, awj::FileLogger& logger) {
  struct ComScope {
    HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)};
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
  } com;
  if (FAILED(com.hr)) return fail("Cannot initialize COM for the synthetic FP16 fixture.");
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> writer;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  const auto input = root / "synthetic-scrgb-half.jxr";
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(input.c_str(), GENERIC_WRITE)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &writer)) ||
      FAILED(writer->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(writer->CreateNewFrame(&frame, &options)) || FAILED(frame->Initialize(options.Get())) ||
      FAILED(frame->SetSize(2, 2))) return fail("Cannot prepare the synthetic FP16 JPEG XR fixture.");
  WICPixelFormatGUID format = GUID_WICPixelFormat64bppRGBAHalf;
  std::uint16_t pixels[]{0, 0, 0, 0x3c00, 0x3c00, 0x3800, 0x3400, 0x3c00,
      0x4000, 0x4000, 0x4000, 0x3c00, 0x4400, 0x3c00, 0x3800, 0x3c00};
  if (FAILED(frame->SetPixelFormat(&format)) || !IsEqualGUID(format, GUID_WICPixelFormat64bppRGBAHalf) ||
      FAILED(frame->WritePixels(2, 16, static_cast<UINT>(sizeof(pixels)), reinterpret_cast<BYTE*>(pixels))) ||
      FAILED(frame->Commit()) || FAILED(writer->Commit())) return fail("Cannot write synthetic FP16 samples.");
  frame.Reset(); writer.Reset(); stream.Reset();
  auto original = awj::JxrImageDecoder{}.decode(input);
  if (!original || original->image.sample_representation != awj::SampleRepresentation::ieee_half_float ||
      !original->image.source_info || original->image.source_info->color_primaries != 1)
    return fail(original ? "FP16 fixture was not decoded with scRGB semantics." : original.error());
  for (int primaries : {1, 12}) {
    auto cfg = awj::default_app_config();
    cfg.output_format = awj::OutputFormat::png; cfg.quality = 100; cfg.bit_depth = 16;
    cfg.color_primaries = primaries; cfg.strip_metadata = true;
    cfg.output_dir = output / std::format("explicit-fp16-{}", primaries);
    awj::NativeBackend backend{cfg, logger, {.encoder_threads_per_file = 2, .global_thread_budget = 2}};
    auto result = backend.encode(awj::ImageFile{.path = input, .bytes = std::filesystem::file_size(input)});
    if (!result.ok || result.applied_color_primaries != primaries || result.applied_transfer_characteristics != 16)
      return fail(result.ok ? "Source-matching FP16 primaries override was replaced by default BT.2020." : result.message);
    auto restored = awj::PngImageDecoder{}.decode(result.output_path);
    if (!restored || restored->image.bit_depth != 16 || !restored->image.source_info ||
        restored->image.source_info->color_primaries != primaries || restored->image.source_info->transfer_characteristics != 16)
      return fail(restored ? "FP16 override output lost its actual CICP." : restored.error());
    // 独立 binary16、BT.709→Display P3 及 ST2084 参考，不调用生产像素转换。
    constexpr double to_p3[3][3]{{.8224619687, .1775380313, 0},
        {.03319419885, .9668058011, 0}, {.0170826307, .07239744066, .9105199286}};
    for (std::size_t y = 0; y < original->image.height; ++y) for (std::size_t x = 0; x < original->image.width; ++x) {
      double nits[3]{};
      for (std::size_t c = 0; c < 3; ++c) {
        std::uint16_t bits{};
        std::memcpy(&bits, original->image.planes[0].bytes.data() + y * original->image.planes[0].stride + (x * 4 + c) * 2, 2);
        const int exponent = (bits >> 10) & 31;
        const double sign = bits & 0x8000 ? -1 : 1;
        nits[c] = sign * (exponent ? std::ldexp(1.0 + (bits & 1023) / 1024.0, exponent - 15)
            : std::ldexp(static_cast<double>(bits & 1023), -24)) *
            original->image.source_info->linear_reference_white_nits.value_or(80.0F);
      }
      for (std::size_t c = 0; c < 3; ++c) {
        const double value = std::max(0.0, primaries == 1 ? nits[c]
            : to_p3[c][0] * nits[0] + to_p3[c][1] * nits[1] + to_p3[c][2] * nits[2]);
        const double p = std::pow(value / 10000.0, 2610.0 / 16384.0);
        const double expected = std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) /
            (1.0 + 2392.0 / 128.0 * p), 2523.0 / 32.0);
        std::uint16_t actual{};
        std::memcpy(&actual, restored->image.planes[0].bytes.data() + y * restored->image.planes[0].stride + (x * 4 + c) * 2, 2);
        if (std::abs(actual / 65535.0 - expected) > 4.0 / 65535.0)
          return fail("FP16 CICP override differs from independent primaries/PQ math.");
      }
    }
  }
  auto unsupported = awj::default_app_config();
  unsupported.output_format = awj::OutputFormat::png; unsupported.transfer_characteristics = 8;
  unsupported.output_dir = output / "explicit-fp16-unsupported-linear";
  awj::NativeBackend backend{unsupported, logger, {.encoder_threads_per_file = 1, .global_thread_budget = 1}};
  if (backend.encode(awj::ImageFile{.path = input, .bytes = std::filesystem::file_size(input)}).ok)
    return fail("Explicit extended linear HDR output silently became PQ.");
  return 0;
}

int existing_sdr_targets(const std::filesystem::path& root, const std::filesystem::path& output, awj::FileLogger& logger) {
  auto main = solid_rgba8(0, 0, 0);
  main.bit_depth = 16;
  main.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
      .color_primaries = 9, .transfer_characteristics = 16, .matrix_coefficients = 9,
      .color_range = 1, .has_hdr_metadata = true};
  main.planes = {{std::vector<std::byte>(16 * 16 * 8), 16 * 8}};
  for (std::size_t i = 0; i < 16 * 16; ++i) for (std::size_t c = 0; c < 4; ++c) {
    const std::uint16_t value = c == 3 ? 65535 : 50000;
    std::memcpy(main.planes[0].bytes.data() + (i * 4 + c) * 2, &value, 2);
  }
  awj::GainMapAlternative source_sdr{.image = solid_rgba8(16, 170, 40),
      .main_hdr_headroom_log2 = 2, .alternate_hdr_headroom_log2 = 0};
  auto encoder = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
  if (!encoder) return fail("No AVIF fixture encoder.");
  awj::NativeEncodeSettings settings{.output_format = awj::OutputFormat::avif, .quality = 100,
      .speed = 10, .bit_depth = 12, .chroma_mode = awj::ChromaMode::yuv444};
  settings.applied_color_primaries = 9; settings.applied_transfer_characteristics = 16;
  settings.applied_matrix_coefficients = 9; settings.applied_color_range = 1;
  settings.gain_map_alternative = &source_sdr;
  settings.resources = {.file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1};
  auto pair = encoder->encode(main, settings);
  if (!pair) return fail(pair.error());
  const auto input = root / "existing-sdr-pair.avif"; write_bytes(input, pair->encoded.bytes);
  auto decoder = awj::make_avif_image_decoder(1);
  auto low = decoder->decode_memory(pair->encoded.bytes, "native existing SDR fixture",
      {.gain_map_mode = awj::GainMapDecodeMode::sdr_only});
  auto both = decoder->decode_memory(pair->encoded.bytes, "native paired fixture",
      {.gain_map_mode = awj::GainMapDecodeMode::preserve_both});
  if (!low || !both) return fail(!low ? low.error() : both.error());
  if (!both->gain_map_alternative || !low->image.source_info || low->image.source_info->has_hdr_metadata ||
      !same_sdr_pixels(low->image, source_sdr.image, 12))
    return fail("AVIF routing fixture lost its independently green SDR endpoint.");
  for (const auto format : {awj::OutputFormat::webp}) for (const bool strip : {false, true}) {
    auto cfg = awj::default_app_config();
    cfg.input_path = input; cfg.output_format = format; cfg.quality = 100; cfg.bit_depth = 8;
    cfg.strip_metadata = strip; cfg.max_jobs = 1;
    cfg.output_dir = output / std::format("existing-sdr-{}-{}", static_cast<int>(format), strip);
    awj::NativeBackend backend{cfg, logger, awj::ResourcePlan{
        .file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1}};
    auto converted = backend.encode(awj::ImageFile{.index = 0, .path = input, .bytes = pair->encoded.bytes.size()});
    if (!converted.ok) return fail(converted.message);
    if (converted.applied_hdr_metadata == "sdr-tone-map" || converted.fallback_reason.find("tone-map") != std::string::npos)
      return fail("WebP/PNG re-tone-mapped a gain-map input that already has SDR.");
    auto restored = awj::WebPImageDecoder{}.decode(converted.output_path);
    if (!restored || !same_sdr_pixels(restored->image, low->image, 1))
      return fail(restored ? "WebP selected HDR or a new tone map instead of the existing SDR." : restored.error());
  }
  for (const bool strip : {false, true}) for (int depth : {0, 8, 16}) {
    auto cfg = awj::default_app_config();
    cfg.output_format = awj::OutputFormat::png; cfg.quality = 100; cfg.strip_metadata = strip;
    if (depth) cfg.bit_depth = depth;
    cfg.output_dir = output / std::format("paired-hdr-png-{}-{}", strip, depth);
    awj::NativeBackend backend{cfg, logger, {.encoder_threads_per_file = 2, .global_thread_budget = 2}};
    auto result = backend.encode(awj::ImageFile{.path = input, .bytes = pair->encoded.bytes.size()});
    if (!result.ok || result.rendition.selected != "hdr" || result.rendition.reused_sdr ||
        result.rendition.tone_mapped || result.rendition.gain_map_generated || result.applied_bit_depth != (depth ? depth : 16))
      return fail(result.ok ? "PNG did not select the highest supported HDR rendition." : result.message);
    auto restored = awj::PngImageDecoder{}.decode(result.output_path);
    if (!restored || restored->image.bit_depth != (depth ? depth : 16) ||
        restored->image.width != both->image.width || restored->image.height != both->image.height ||
        restored->image.pixel_format != awj::PixelFormat::rgba ||
        !restored->image.source_info || restored->image.source_info->color_primaries != 9 ||
        restored->image.source_info->transfer_characteristics != 16)
      return fail(restored ? "PNG lost essential HDR color interpretation under strip." : restored.error());
    const int output_depth = depth ? depth : 16;
    for (std::size_t y = 0; y < both->image.height; ++y) for (std::size_t x = 0; x < both->image.width * 4; ++x) {
      std::uint16_t source_value{};
      std::memcpy(&source_value, both->image.planes[0].bytes.data() + y * both->image.planes[0].stride + x * 2, 2);
      unsigned value{};
      if (output_depth == 8) value = std::to_integer<unsigned>(restored->image.planes[0].bytes[y * restored->image.planes[0].stride + x]);
      else { std::uint16_t sample{}; std::memcpy(&sample, restored->image.planes[0].bytes.data() + y * restored->image.planes[0].stride + x * 2, 2); value = sample; }
      const unsigned expected = output_depth == 8 ? (static_cast<unsigned>(source_value) * 255u + 32767u) / 65535u : source_value;
      if (value != expected) return fail("PNG re-tone-mapped the selected HDR or only changed its bit-depth label.");
    }
  }
  return 0;
}
int hdr_color_policy(const std::filesystem::path& root, const std::filesystem::path& output, awj::FileLogger& logger) {
  for (int primaries : {9, 12}) for (int transfer : {16, 18}) for (bool paired : {false, true}) {
    auto source = make_hdr_test_image();
    source.source_info->color_primaries = primaries;
    source.source_info->transfer_characteristics = transfer;
    awj::NativeEncodeSettings settings{.quality = 100, .speed = 10, .bit_depth = paired ? 12 : 16,
        .chroma_mode = awj::ChromaMode::yuv444};
    settings.applied_color_primaries = primaries;
    settings.applied_transfer_characteristics = transfer;
    settings.applied_matrix_coefficients = 9;
    settings.applied_color_range = 1;
    auto sdr = solid_rgba8(16, 170, 40);
    // Match logical dimensions without deriving a new SDR appearance from HDR.
    sdr.width = source.width; sdr.height = source.height;
    sdr.planes = {{std::vector<std::byte>(source.width * source.height * 4), source.width * 4}};
    for (std::size_t i = 0; i < source.width * source.height; ++i) {
      sdr.planes[0].bytes[i * 4] = std::byte{16}; sdr.planes[0].bytes[i * 4 + 1] = std::byte{170};
      sdr.planes[0].bytes[i * 4 + 2] = std::byte{40}; sdr.planes[0].bytes[i * 4 + 3] = std::byte{255};
    }
    awj::GainMapAlternative alternate{.image = std::move(sdr), .main_hdr_headroom_log2 = 2, .alternate_hdr_headroom_log2 = 0};
    std::expected<awj::NativeEncodeResult, std::string> encoded = std::unexpected{"fixture"};
    if (paired) {
      settings.output_format = awj::OutputFormat::avif;
      settings.gain_map_alternative = &alternate;
      encoded = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom)->encode(source, settings);
    } else {
      settings.output_format = awj::OutputFormat::jxl;
      encoded = awj::JXLImageEncoder{}.encode(source, settings);
    }
    if (!encoded) return fail(encoded.error());
    const auto input = root / std::format("color-policy-{}-{}-{}.{}", primaries, transfer, paired, paired ? "avif" : "jxl");
    write_bytes(input, encoded->encoded.bytes);
    std::unique_ptr<awj::ImageDecoder> input_decoder;
    if (paired) input_decoder = awj::make_avif_image_decoder(1);
    else input_decoder = std::make_unique<awj::JXLImageDecoder>();
    auto reference = input_decoder->decode(input, {.gain_map_mode = awj::GainMapDecodeMode::hdr_only});
    if (!reference || reference->image.bit_depth != 16) return fail(reference ? "HDR reference lost16-bit storage." : reference.error());
    for (auto format : {awj::OutputFormat::avif, awj::OutputFormat::png}) for (bool strip : {false, true}) {
      auto cfg = awj::default_app_config();
      cfg.output_format = format; cfg.quality = 100; cfg.strip_metadata = strip;
      if (format == awj::OutputFormat::avif) { cfg.speed = 10; cfg.bit_depth = 12; cfg.chroma_mode = awj::ChromaMode::yuv444; }
      cfg.output_dir = output / std::format("color-policy-{}-{}-{}-{}-{}", primaries, transfer, paired, static_cast<int>(format), strip);
      awj::NativeBackend backend{cfg, logger, {.encoder_threads_per_file = 2, .global_thread_budget = 2}};
      auto result = backend.encode(awj::ImageFile{.path = input, .bytes = encoded->encoded.bytes.size()});
      const bool map_expected = paired && format == awj::OutputFormat::avif;
      if (!result.ok || result.applied_color_primaries != primaries || result.applied_transfer_characteristics != transfer ||
          result.rendition.selected != "hdr" || result.rendition.tone_mapped || result.rendition.gain_map_generated != map_expected)
        return fail(result.ok ? "HDR base was relabeled, tone-mapped or given an invented SDR endpoint." : result.message);
      std::unique_ptr<awj::ImageDecoder> reader;
      if (format == awj::OutputFormat::png) reader = std::make_unique<awj::PngImageDecoder>();
      else reader = awj::make_avif_image_decoder(1);
      auto restored = reader->decode(result.output_path, {.gain_map_mode = awj::GainMapDecodeMode::hdr_only});
      if (!restored || !restored->image.source_info || restored->image.bit_depth != 16 ||
          restored->image.source_info->color_primaries != primaries || restored->image.source_info->transfer_characteristics != transfer ||
          restored->image.source_info->source_has_gain_map != map_expected)
        return fail(restored ? "Actual HDR color/map signaling differs from the selected rendition." : restored.error());
      for (std::size_t y = 0; y < reference->image.height; ++y) for (std::size_t x = 0; x < reference->image.width * 4; ++x) {
        std::uint16_t a{}, b{};
        std::memcpy(&a, reference->image.planes[0].bytes.data() + y * reference->image.planes[0].stride + x * 2, 2);
        std::memcpy(&b, restored->image.planes[0].bytes.data() + y * restored->image.planes[0].stride + x * 2, 2);
        if (std::abs(static_cast<int>(a) - static_cast<int>(b)) > (format == awj::OutputFormat::png ? 0 : 64))
          return fail("Retained HDR base pixels changed color despite unchanged color labels.");
      }
    }
  }
  return 0;
}

std::vector<std::byte> poster_apng(bool independent_poster) {
  awj::PngImageEncoder encoder;
  const auto png = [&](unsigned r, unsigned g, unsigned b) {
    auto encoded = encoder.encode(solid_rgba8(r, g, b), {.output_format = awj::OutputFormat::png, .quality = 100});
    if (!encoded) fail(encoded.error());
    return std::move(encoded->encoded.bytes);
  };
  const auto red = png(255, 0, 0), green = png(0, 255, 0), blue = png(0, 0, 255);
  std::vector<std::byte> result(red.begin(), red.begin() + 33), control;
  awj::png_detail::append_be_u32(control, 2); awj::png_detail::append_be_u32(control, 3);
  awj::png_detail::append_png_chunk(result, "acTL", control);
  const auto data = [&](const auto& bytes, bool frame_data, std::uint32_t& sequence) {
    for (std::size_t pos = 8; pos + 12 <= bytes.size();) {
      const auto size = awj::png_detail::read_be_u32(bytes, pos);
      if (std::memcmp(bytes.data() + pos + 4, "IDAT", 4) == 0) {
        std::vector<std::byte> payload;
        if (frame_data) awj::png_detail::append_be_u32(payload, sequence++);
        payload.insert(payload.end(), bytes.begin() + pos + 8, bytes.begin() + pos + 8 + size);
        awj::png_detail::append_png_chunk(result, frame_data ? "fdAT" : "IDAT", payload);
      }
      pos += size + 12;
    }
  };
  std::uint32_t sequence{};
  if (independent_poster) data(blue, false, sequence);
  for (unsigned index = 0; index < 2; ++index) {
    control.clear(); awj::png_detail::append_be_u32(control, sequence++);
    for (const auto n : {16u, 16u, 0u, 0u}) awj::png_detail::append_be_u32(control, n);
    control.insert(control.end(), {std::byte{0}, std::byte(index + 1), std::byte{0}, std::byte{10}, std::byte{0}, std::byte{0}});
    awj::png_detail::append_png_chunk(result, "fcTL", control);
    data(index ? green : red, independent_poster || index != 0, sequence);
  }
  awj::png_detail::append_png_chunk(result, "IEND", {});
  return result;
}
int poster_pipeline(const std::filesystem::path& root, const std::filesystem::path& output, awj::FileLogger& logger) {
  for (const bool poster : {true, false}) {
    const auto input = root / (poster ? "blue-poster.png" : "pure-apng.png");
    const auto bytes = poster_apng(poster); write_bytes(input, bytes);
    auto cfg = awj::default_app_config(); cfg.input_path = input; cfg.output_format = awj::OutputFormat::avif;
    cfg.output_dir = output / (poster ? "static-poster" : "pure-apng"); cfg.quality = 100; cfg.max_jobs = 1;
    awj::NativeBackend backend{cfg, logger, awj::ResourcePlan{
        .file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1}};
    const auto converted = backend.encode(awj::ImageFile{.index = 0, .path = input, .bytes = bytes.size()});
    if (!converted.ok) return fail(converted.message);
    auto animated = awj::open_animation(converted.output_path, 0);
    if (!animated) return fail(animated.error());
    if (poster) {
      if (*animated || converted.integration_mode == "libavif-sequence") return fail("Independent blue PNG poster became AVIF animation.");
      auto decoder = awj::make_avif_image_decoder(1);
      auto decoded = decoder->decode(converted.output_path);
      if (!decoded) return fail(decoded.error());
      const auto sample = [&](std::size_t c) {
        if (decoded->image.bit_depth == 8) return std::to_integer<unsigned>(decoded->image.planes[0].bytes[c]);
        std::uint16_t v{}; std::memcpy(&v, decoded->image.planes[0].bytes.data() + c * 2, 2); return unsigned(v);
      };
      const auto full = decoded->image.bit_depth == 8 ? 255u : 65535u;
      if (sample(2) < full * 9 / 10 || sample(0) > full / 10 || sample(1) > full / 10)
        return fail("Red animation frame replaced independent blue poster in the native pipeline.");
    } else {
      if (!*animated || converted.integration_mode != "libavif-sequence" || (*animated)->info().durations.size() != 2 ||
          (*animated)->info().repetitions != 2) return fail("Pure APNG timing/loop was flattened by static-first routing.");
      for (unsigned index = 0; index < 2; ++index) {
        const auto delay = (*animated)->info().durations[index];
        if (!delay.denominator || std::uint64_t(delay.numerator) * 10 != std::uint64_t(delay.denominator) * (index + 1))
          return fail("Pure APNG frame duration changed in AVIF output.");
        const auto next = (*animated)->next();
        if (!next || !*next) return fail("Pure APNG output lost an animation frame.");
        const auto& frame = (*animated)->frame();
        const auto channel = [&](std::size_t c) {
          if (frame.bit_depth == 8) return std::to_integer<unsigned>(frame.planes[0].bytes[c]);
          std::uint16_t value{}; std::memcpy(&value, frame.planes[0].bytes.data() + c * 2, 2); return unsigned(value);
        };
        const auto full = frame.bit_depth == 8 ? 255u : 65535u;
        if (frame.width != 16 || frame.height != 16 || channel(index) < full * 9 / 10 ||
            channel(1 - index) > full / 10 || channel(2) > full / 10)
          return fail("Pure APNG red/green frame order or canvas changed in AVIF output.");
      }
      const auto ended = (*animated)->next();
      if (!ended || *ended) return fail("Pure APNG output frame count/termination changed.");
    }
  }
  return 0;
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
  if (const auto checked = explicit_fp16_color(root, output, logger)) return checked;
  if (const auto checked = existing_sdr_targets(root, output, logger)) return checked;
  if (const auto checked = hdr_color_policy(root, output, logger)) return checked;
  if (const auto checked = poster_pipeline(root, output, logger)) return checked;
  {
    // Two one-pixel GIF frames: automatic animation must survive the normal preset path.
    constexpr unsigned char header[]{'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0,
        0, 0, 0, 255, 255, 255};
    constexpr unsigned char frame[]{0x21, 0xf9, 4, 0, 10, 0, 0, 0,
        0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0};
    const auto gif = root / "automatic-animation.gif";
    {
      std::ofstream stream{gif, std::ios::binary};
      stream.write(reinterpret_cast<const char*>(header), sizeof(header));
      for (int i = 0; i < 2; ++i) stream.write(reinterpret_cast<const char*>(frame), sizeof(frame));
      stream.put('\x3b');
    }
    awj::AppConfig automatic;
    automatic.input_path = gif;
    automatic.output_dir = output;
    awj::NativeBackend animated{automatic, logger, awj::ResourcePlan{
        .file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1,
        .memory_limit_bytes = 64 * 1024 * 1024}};
    auto converted = animated.encode(awj::ImageFile{.index = 0, .path = gif,
        .bytes = std::filesystem::file_size(gif)});
    if (!converted.ok || converted.integration_mode != "libavif-sequence" ||
        converted.applied_bit_depth != 10 || converted.applied_chroma != "444" ||
        converted.final_encoder_quality != awj::encoding_defaults::default_avif_quality)
      return fail(converted.ok ? "Default preset did not automatically preserve GIF animation." : converted.message);
    const auto bytes = read_text(converted.output_path);
    std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder{avifDecoderCreate(), avifDecoderDestroy};
    if (!decoder || avifDecoderSetSource(decoder.get(), AVIF_DECODER_SOURCE_TRACKS) != AVIF_RESULT_OK ||
        avifDecoderSetIOMemory(decoder.get(), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()) != AVIF_RESULT_OK ||
        avifDecoderParse(decoder.get()) != AVIF_RESULT_OK || decoder->imageCount != 2)
      return fail("Automatic animation conversion produced a single-frame AVIF.");
  }
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

  for (const auto mode : {awj::JpegHdrMode::automatic, awj::JpegHdrMode::sdr, awj::JpegHdrMode::hdr}) {
    auto jpeg_cfg = awj::default_app_config();
    jpeg_cfg.output_format = awj::OutputFormat::jpgli;
    jpeg_cfg.jpeg_hdr = mode;
    jpeg_cfg.output_dir = output / std::format("hdr-jpeg-{}", static_cast<int>(mode));
    jpeg_cfg.quality = 85;
    jpeg_cfg.max_jobs = 1;
    awj::NativeBackend jpeg_backend{jpeg_cfg, logger, awj::ResourcePlan{
        .file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1,
        .memory_limit_bytes = 128 * 1024 * 1024}};
    const auto result = jpeg_backend.encode(awj::ImageFile{.index = 0, .path = hdr_input, .bytes = hdr_input_bytes});
    if (!result.ok) return fail(result.message);
    const bool enhanced = mode != awj::JpegHdrMode::sdr;
    auto restored = awj::decode_jpeg_gain_map(result.output_path);
    if (!restored) return fail(restored.error());
    if (restored->has_value() != enhanced) return fail("HDR JPEG output map presence failed.");
    if (result.integration_mode != (enhanced ? "jpegli-iso21496-1" : "jpegli-native") ||
        result.requested_chroma != "auto" || result.applied_chroma != "444" ||
        (enhanced && result.applied_hdr_metadata != "iso21496-1-gain-map"))
      return fail("HDR JPEG auto/sdr/hdr routing or source-follow sampling failed.");
    if (enhanced) {
      auto branch_cfg = awj::default_app_config();
      branch_cfg.output_format = awj::OutputFormat::jxl;
      branch_cfg.output_dir = output / std::format("jxl-iso-gain-map-{}", static_cast<int>(mode));
      branch_cfg.quality = 100;
      branch_cfg.jxl_jpeg_lossless = true;
      awj::NativeBackend branch{branch_cfg, logger, awj::ResourcePlan{
          .file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1,
          .memory_limit_bytes = 128 * 1024 * 1024}};
      const auto converted = branch.encode(awj::ImageFile{.index = 0, .path = result.output_path, .bytes = result.output_bytes});
      if (!converted.ok || converted.integration_mode == "jxl-jpeg-bitstream-transcode" || !converted.source_has_hdr_metadata)
        return fail("ISO gain-map JPEG bypassed HDR pixel decoding via JXL bitstream transcode.");
    }
  }

  {
    auto rejected_cfg = awj::default_app_config();
    rejected_cfg.output_format = awj::OutputFormat::jpgli;
    rejected_cfg.output_dir = output / "hdr-jpeg-rejected";
    rejected_cfg.visual_quality = 100;
    const awj::ResourcePlan budget{.file_parallelism = 1, .encoder_threads_per_file = 1,
        .global_thread_budget = 1, .memory_limit_bytes = 128 * 1024 * 1024};
    awj::NativeBackend visual{rejected_cfg, logger, budget};
    if (visual.encode(awj::ImageFile{.index = 0, .path = hdr_input, .bytes = hdr_input_bytes}).ok ||
        std::filesystem::exists(rejected_cfg.output_dir / "input-hdr.jpg"))
      return fail("HDR JPEG visual search rejection left a completed output.");
    rejected_cfg.visual_quality.reset(); rejected_cfg.jpeg_hdr = awj::JpegHdrMode::hdr;
    awj::NativeBackend forced{rejected_cfg, logger, budget};
    if (forced.encode(awj::ImageFile{.index = 0, .path = input, .bytes = input_bytes}).ok)
      return fail("Forced HDR JPEG accepted SDR input.");
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
      hdr_avif_result.requested_bit_depth.value_or(0) != 12 ||
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
  {
    auto automatic = cfg;
    automatic.bit_depth.reset();
    automatic.output_dir = output / "jxl-jpeg-auto16";
    awj::NativeBackend backend{automatic, logger, {.encoder_threads_per_file = 1, .global_thread_budget = 1}};
    auto result = backend.encode(awj::ImageFile{.path = jpeg_input, .bytes = jpeg_bytes});
    if (!result.ok || result.applied_bit_depth != 16 || result.integration_mode == "jxl-jpeg-bitstream-transcode")
      return fail(result.ok ? "JPEG transcode bypassed the automatic JXL16 plan." : result.message);
  }
  cfg.bit_depth = 8;
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
  // 没有 MPF 的旧 HDR／坏 ISO 也不能经 JPEG 位流快速路径绕过终止屏障。
  for (const bool invalid_iso : {false, true}) {
    std::string packet = invalid_iso ? "urn:iso:std:iso:ts:21496:-1" : "http://ns.adobe.com/xap/1.0/";
    packet.push_back('\0');
    packet += invalid_iso ? std::string(8, '\0') :
        "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
        "<rdf:Description xmlns:hdrgm='http://ns.adobe.com/hdr-gain-map/1.0/' hdrgm:GainMapMax='2'/></rdf:RDF></x:xmpmeta>";
    const auto malformed_input = root / (invalid_iso ? "invalid-iso.jpg" : "legacy-hdr.jpg");
    {
      std::ofstream stream{malformed_input, std::ios::binary};
      const auto marker_length = static_cast<unsigned>(packet.size() + 2);
      const char marker[]{char(0xff), char(invalid_iso ? 0xe2 : 0xe1), char(marker_length >> 8), char(marker_length & 0xff)};
      stream.write(reinterpret_cast<const char*>(source_jpeg->encoded.bytes.data()), 2);
      stream.write(marker, sizeof(marker));
      stream.write(packet.data(), static_cast<std::streamsize>(packet.size()));
      stream.write(reinterpret_cast<const char*>(source_jpeg->encoded.bytes.data() + 2),
          static_cast<std::streamsize>(source_jpeg->encoded.bytes.size() - 2));
      if (!stream) return fail("Cannot write HDR bitstream barrier fixture.");
    }
    auto branch_cfg = cfg;
    branch_cfg.output_dir = output / (invalid_iso ? "jxl-invalid-iso" : "jxl-legacy-hdr");
    awj::NativeBackend branch{branch_cfg, logger, awj::ResourcePlan{
        .file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1}};
    const auto result = branch.encode(awj::ImageFile{.index = 0, .path = malformed_input,
        .bytes = std::filesystem::file_size(malformed_input)});
    if (result.ok || result.message.find("Gain Map:") == std::string::npos ||
        result.message.find("ISO 21496-1") == std::string::npos)
      return fail("Legacy or invalid ISO JPEG escaped terminal rejection through JXL transcode.");
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
    return fail(jpegli_result.message.empty() ? "native backend JPG encode failed."
                                              : jpegli_result.message);
  }
  const auto jpegli_output_file = output / "input.jpg";
  if (!std::filesystem::exists(jpegli_output_file)) {
    return fail("native backend did not create JPG .jpg output file.");
  }
  if (jpegli_result.output_path != jpegli_output_file ||
      jpegli_result.output_bytes == 0 ||
      jpegli_result.output_format != "JPG" ||
      jpegli_result.encoder_id != "jpegli" ||
      jpegli_result.command.find("JPG") == std::string::npos) {
    return fail("native backend JPG result metadata invalid.");
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
      jpegli_summary_csv.find("JPG,jpegli") == std::string::npos) {
    return fail("native backend JPG summary diagnostics invalid.");
  }
  std::vector<awj::EncodeResult> sampling_results;
  const auto exercise_auto = [&](const std::filesystem::path& source,
                                 awj::PixelFormat source_format,
                                 awj::ChromaMode expected) {
    for (const bool search : {false, true}) {
      auto options = awj::default_app_config();
      options.input_path = source;
      options.output_format = awj::OutputFormat::jpgli;
      options.output_dir = output / (search ? "jpeg-auto-search" : "jpeg-auto") / source.stem();
      options.quality = 85;
      if (search) options.visual_quality = 30;
      options.visual_quality_gpu = false;
      options.visual_quality_fallback = true;
      options.strip_metadata = true;
      options.allow_wic_fallback = false;
      options.max_jobs = 1;
      awj::NativeBackend test{options, logger, awj::ResourcePlan{
          .file_parallelism = 1, .encoder_threads_per_file = 1,
          .global_thread_budget = 1, .memory_limit_bytes = 64 * 1024 * 1024}};
      auto result = test.encode(awj::ImageFile{.index = sampling_results.size(), .path = source,
          .bytes = std::filesystem::file_size(source)});
      if (!result.ok) return fail(result.message);
      const auto source_chroma = source_format == awj::PixelFormat::unknown ? "unknown"
                               : source_format == awj::PixelFormat::gray ? "400"
                               : awj::chroma_mode_name(expected);
      const auto expected_format = expected == awj::ChromaMode::yuv420 ? awj::PixelFormat::yuv420
                                 : expected == awj::ChromaMode::yuv422 ? awj::PixelFormat::yuv422
                                 : awj::PixelFormat::yuv444;
      awj::JpegliImageDecoder decoder;
      const auto decoded = decoder.decode(result.output_path);
      if (!decoded || !decoded->image.source_info ||
          decoded->image.source_info->pixel_format != expected_format ||
          decoded->image.source_info->bit_depth != 8 || result.user_chroma != "auto" ||
          result.source_chroma != source_chroma || result.requested_chroma != "auto" ||
          result.applied_chroma != awj::chroma_mode_name(expected))
        return fail(decoded ? std::format(
            "JPEG sampling mismatch: source={} search={} expected={} decoded_format={} depth={} user={} source_chroma={} expected_source={} requested={} applied={}",
            source.filename().string(), search, awj::chroma_mode_name(expected),
            decoded->image.source_info ? static_cast<int>(decoded->image.source_info->pixel_format) : -1,
            decoded->image.source_info ? decoded->image.source_info->bit_depth : -1,
            result.user_chroma, result.source_chroma, source_chroma, result.requested_chroma, result.applied_chroma) : decoded.error());
      sampling_results.push_back(std::move(result));
    }
    return 0;
  };
  for (const auto mode : {awj::ChromaMode::yuv420, awj::ChromaMode::yuv422, awj::ChromaMode::yuv444}) {
    const auto seed = root / std::format("source-yuv-{}.jpg", awj::chroma_mode_name(mode));
    auto bytes = source_jpeg_encoder.encode(make_alpha_test_image(), awj::NativeEncodeSettings{
        .output_format = awj::OutputFormat::jpgli, .quality = 95, .chroma_mode = mode,
        .strip_metadata = true});
    if (!bytes) return fail(bytes.error());
    { std::ofstream file{seed, std::ios::binary}; file.write(
        reinterpret_cast<const char*>(bytes->encoded.bytes.data()),
        static_cast<std::streamsize>(bytes->encoded.bytes.size())); }
    const auto source_format = mode == awj::ChromaMode::yuv420 ? awj::PixelFormat::yuv420
                             : mode == awj::ChromaMode::yuv422 ? awj::PixelFormat::yuv422
                             : awj::PixelFormat::yuv444;
    if (const int result = exercise_auto(seed, source_format, mode)) return result;
  }
  for (const auto source_format : {awj::PixelFormat::rgb, awj::PixelFormat::rgba,
                                  awj::PixelFormat::gray, awj::PixelFormat::unknown}) {
    const auto seed = root / std::format("source-{}.tif", static_cast<int>(source_format));
    if (!write_source_tiff(seed, source_format)) return fail("failed to create TIFF source provenance fixture.");
    if (const int result = exercise_auto(seed, source_format, awj::ChromaMode::yuv444)) return result;
  }
  const auto sampling_csv_dir = output / "jpeg-auto-summary";
  if (auto written = awj::write_csv(sampling_csv_dir, sampling_results); !written) return fail(written.error());
  if (!csv_sampling_matches(read_text(sampling_csv_dir / "summary.csv"), sampling_results))
    return fail("JPEG requested/applied sampling CSV columns disagree with encoded SOFs.");
  auto invalid_jpeg = awj::default_app_config();
  invalid_jpeg.output_format = awj::OutputFormat::jpgli;
  invalid_jpeg.chroma_mode = awj::ChromaMode::yuv400;
  invalid_jpeg.output_dir = output / "jpeg-reject-400";
  awj::NativeBackend rejected_400{invalid_jpeg, logger, awj::ResourcePlan{
      .file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1}};
  if (rejected_400.encode(awj::ImageFile{.index = 0, .path = input, .bytes = input_bytes}).ok)
    return fail("Native JPEG backend silently accepted explicit 400.");
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
