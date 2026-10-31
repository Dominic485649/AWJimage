#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <expected>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import awj.codec;
import awj.config;
import awj.core;
import awj.gain_map;
import awj.hdr_tonemap;
import awj.image;
import awj.avif_aom_codec;
import awj.resource_planner;
import awj.jpegli_codec;
import awj.jxl_codec;
import awj.native_backend;
import awj.png_codec;
import awj.webp_codec;

namespace {
namespace fs = std::filesystem;

void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string{message});
}

template <class T>
T take(std::expected<T, std::string> result) {
  if (!result) throw std::runtime_error(result.error());
  return std::move(*result);
}

awj::ImageBuffer image(bool hdr, unsigned red, unsigned green, unsigned blue,
                       int transfer = 16, int primaries = 9, int depth = 0) {
  depth = depth ? depth : hdr ? 16 : 8;
  awj::ImageBuffer output{.width = 16, .height = 8,
      .pixel_format = awj::PixelFormat::rgba, .alpha_mode = awj::AlphaMode::straight,
      .bit_depth = depth, .source_info = awj::ImageSourceInfo{
          .pixel_format = awj::PixelFormat::rgba, .bit_depth = depth,
          .color_primaries = hdr ? primaries : 1,
          .transfer_characteristics = hdr ? transfer : 13,
          .matrix_coefficients = 0, .color_range = 1, .has_hdr_metadata = hdr}};
  const auto sample_bytes = depth == 16 ? 2u : 1u;
  output.planes.push_back({std::vector<std::byte>(output.width * output.height * 4 * sample_bytes),
      output.width * 4 * sample_bytes});
  const std::array values{red, green, blue, depth == 16 ? 65535u : 255u};
  for (std::size_t pixel = 0; pixel < output.width * output.height; ++pixel) {
    for (std::size_t c = 0; c < 4; ++c) {
      if (sample_bytes == 1) output.planes[0].bytes[pixel * 4 + c] = std::byte(values[c]);
      else {
        const auto sample = static_cast<std::uint16_t>(values[c]);
        std::memcpy(output.planes[0].bytes.data() + (pixel * 4 + c) * 2, &sample, 2);
      }
    }
  }
  return output;
}

enum class Kind { sdr, sdr16, hdr, consumed_hdr, pair, reverse_pair, hdr_pair, reverse_hdr_pair, sdr_pair };
constexpr std::array kinds{Kind::sdr, Kind::sdr16, Kind::hdr, Kind::consumed_hdr,
    Kind::pair, Kind::reverse_pair, Kind::hdr_pair, Kind::reverse_hdr_pair, Kind::sdr_pair};

awj::ImageDecodeResult input(Kind kind, int transfer = 16, int primaries = 9) {
  auto high = image(true, 48000, 40000, 30000, transfer, primaries);
  auto low = image(false, 16, 170, 40);
  if (kind == Kind::sdr || kind == Kind::sdr16) {
    auto sdr = kind == Kind::sdr16 ? image(false, 16 * 257, 170 * 257, 40 * 257, 16, 9, 16) : low;
    sdr.source_info->color_primaries = 12;
    return {.image = std::move(sdr)};
  }
  if (kind == Kind::hdr || kind == Kind::consumed_hdr) {
    high.source_info->source_has_gain_map = kind == Kind::consumed_hdr;
    high.source_info->rendition_origin = kind == Kind::consumed_hdr
        ? awj::RenditionOrigin::gain_map_reconstructed : awj::RenditionOrigin::input;
    return {.image = std::move(high)};
  }
  high.source_info->source_has_gain_map = true;
  low.source_info->source_has_gain_map = true;
  if (kind == Kind::sdr_pair)
    return {.image = low, .gain_map_alternative = awj::GainMapAlternative{std::move(low), 0, 0}};
  if (kind == Kind::hdr_pair || kind == Kind::reverse_hdr_pair) {
    low = image(true, 25000, 23000, 20000, transfer, primaries);
    low.source_info->source_has_gain_map = true;
    if (kind == Kind::reverse_hdr_pair)
      return {.image = std::move(low), .gain_map_alternative = awj::GainMapAlternative{std::move(high), 1, 3}};
    return {.image = std::move(high), .gain_map_alternative = awj::GainMapAlternative{std::move(low), 3, 1}};
  }
  if (kind == Kind::reverse_pair)
    return {.image = std::move(low), .gain_map_alternative = awj::GainMapAlternative{std::move(high), 0, 3}};
  return {.image = std::move(high), .gain_map_alternative = awj::GainMapAlternative{std::move(low), 3, 0}};
}

bool paired(Kind kind) { return kind == Kind::pair || kind == Kind::reverse_pair; }
bool has_hdr(Kind kind) { return kind != Kind::sdr && kind != Kind::sdr16 && kind != Kind::sdr_pair; }

std::unique_ptr<awj::ImageDecoder> decoder(awj::OutputFormat format) {
  switch (format) {
    case awj::OutputFormat::avif: return awj::make_avif_image_decoder(1);
    case awj::OutputFormat::jxl: return std::make_unique<awj::JXLImageDecoder>();
    case awj::OutputFormat::png: return std::make_unique<awj::PngImageDecoder>();
    case awj::OutputFormat::webp: return std::make_unique<awj::WebPImageDecoder>();
    default: return std::make_unique<awj::JpegliImageDecoder>();
  }
}

awj::AppConfig config(awj::OutputFormat format, awj::JpegHdrMode mode, bool strip) {
  auto cfg = awj::default_app_config();
  cfg.output_format = format; cfg.jpeg_hdr = mode; cfg.strip_metadata = strip;
  cfg.quality = 100; cfg.speed = 10; cfg.chroma_mode = awj::ChromaMode::yuv444;
  cfg.avif_color_representation = awj::AvifColorRepresentation::rgb_identity;
  cfg.image_size_limit.mode = awj::ImageSizeLimitMode::none;
  return cfg;
}

constexpr awj::ResourcePlan resources{.file_parallelism = 1, .encoder_threads_per_file = 1,
    .global_thread_budget = 1, .memory_limit_bytes = 256 * 1024 * 1024};

void check_samples(const awj::ImageBuffer& actual, const awj::ImageBuffer& expected, unsigned tolerance) {
  require(actual.width == expected.width && actual.height == expected.height &&
      actual.pixel_format == awj::PixelFormat::rgba && actual.bit_depth == expected.bit_depth &&
      actual.sample_representation == expected.sample_representation, "所选单层像素布局改变");
  const auto sample_bytes = actual.bit_depth == 16 ? 2u : 1u;
  for (std::size_t y = 0; y < actual.height; ++y) for (std::size_t x = 0; x < actual.width * 4; ++x) {
    unsigned a{}, b{};
    if (sample_bytes == 1) {
      a = std::to_integer<unsigned>(actual.planes[0].read_bytes()[y * actual.planes[0].stride + x]);
      b = std::to_integer<unsigned>(expected.planes[0].read_bytes()[y * expected.planes[0].stride + x]);
    } else {
      std::uint16_t av{}, bv{};
      std::memcpy(&av, actual.planes[0].read_bytes().data() + y * actual.planes[0].stride + x * 2, 2);
      std::memcpy(&bv, expected.planes[0].read_bytes().data() + y * expected.planes[0].stride + x * 2, 2);
      a = av; b = bv;
    }
    require(std::abs(static_cast<int>(a) - static_cast<int>(b)) <= static_cast<int>(tolerance),
        "没有复用实际较高 HDR／原 SDR，或像素被意外重映射");
  }
}

void run_case(Kind kind, awj::OutputFormat format, awj::JpegHdrMode mode, bool strip,
              int transfer, int primaries, awj::FileLogger& logger) {
  auto decoded = input(kind, transfer, primaries);
  auto main_bytes = std::move(decoded.image.planes[0].bytes);
  decoded.image.planes[0].borrowed = main_bytes;
  std::vector<std::byte> alternate_bytes;
  if (decoded.gain_map_alternative) {
    alternate_bytes = std::move(decoded.gain_map_alternative->image.planes[0].bytes);
    decoded.gain_map_alternative->image.planes[0].borrowed = alternate_bytes;
  }
  const auto main_before = main_bytes, alternate_before = alternate_bytes;
  auto cfg = config(format, mode, strip);
  awj::NativeBackend backend{cfg, logger, resources};
  const auto encoded = take(backend.encode_memory(decoded));
  require(main_bytes == main_before && alternate_bytes == alternate_before, "编码改写了借用来源像素");
  const bool jpeg_hdr = format == awj::OutputFormat::jpgli && mode != awj::JpegHdrMode::sdr && has_hdr(kind);
  const bool expected_map = jpeg_hdr || ((format == awj::OutputFormat::avif ||
      format == awj::OutputFormat::jxl) && paired(kind));
  require(encoded.diagnostics.rendition.gain_map_generated == expected_map, "输出 Gain Map 资格与真实端点不符");
  auto reader = decoder(format);
  auto restored = take(reader->decode_memory(encoded.encoded.bytes, "rendition-policy",
      {.copy_metadata_payloads = false, .gain_map_mode = awj::GainMapDecodeMode::preserve_both}));
  require(restored.gain_map_alternative.has_value() == expected_map, "实际容器中的双呈现与诊断不符");
  require(restored.image.source_info.has_value(), "回读缺少颜色来源");
  require(restored.image.source_info->source_has_gain_map == expected_map, "实际增益图来源标记不符");
  const bool expected_hdr = has_hdr(kind) && (jpeg_hdr ||
      (format != awj::OutputFormat::webp && format != awj::OutputFormat::jpgli));
  require(take(awj::hdr::has_explicit_hdr_signal(restored.image)) == expected_hdr, "单层 SDR/HDR 解释与所选呈现不符");
  const bool generated_sdr = has_hdr(kind) && !paired(kind) &&
      (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli);
  require(encoded.diagnostics.rendition.tone_mapped == generated_sdr, "自动滚降诊断不符");
  if (paired(kind) && (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli))
    require(encoded.diagnostics.rendition.reused_sdr, "没有优先复用已有 SDR");
  if (expected_map) {
    const auto& low = restored.gain_map_alternative->image;
    require(!take(awj::hdr::has_explicit_hdr_signal(low)), "Gain Map 的备用端不是 SDR");
    require(restored.gain_map_alternative->main_hdr_headroom_log2 > 0 &&
        restored.gain_map_alternative->alternate_hdr_headroom_log2 == 0, "HDR/SDR headroom 方向改变");
    if (paired(kind)) check_samples(low, image(false, 16, 170, 40), 16);
  }
  if (expected_hdr && format != awj::OutputFormat::jpgli)
    check_samples(restored.image, image(true, 48000, 40000, 30000, transfer, primaries),
        format == awj::OutputFormat::avif ? 96 : 0);
  if (paired(kind) && !expected_map && !expected_hdr)
    check_samples(restored.image, image(false, 16, 170, 40), format == awj::OutputFormat::webp ? 0 : 4);
}

void invalid_pairs(awj::FileLogger& logger) {
  auto cfg = config(awj::OutputFormat::jxl, awj::JpegHdrMode::automatic, false);
  cfg.image_size_limit.mode = awj::ImageSizeLimitMode::manual;
  cfg.image_size_limit.max_width = 8;
  awj::NativeBackend backend{cfg, logger, resources};
  for (unsigned failure = 0; failure < 7; ++failure) {
    auto decoded = input(Kind::hdr_pair);
    auto& other = *decoded.gain_map_alternative;
    switch (failure) {
      case 0: other.main_hdr_headroom_log2 = std::numeric_limits<float>::quiet_NaN(); break;
      case 1: other.alternate_hdr_headroom_log2 = std::numeric_limits<float>::infinity(); break;
      case 2: other.alternate_hdr_headroom_log2 = -1; break;
      case 3: --other.image.width; break;
      case 4: other.image.planes[0].bytes.clear(); break;
      case 5: other.alternate_hdr_headroom_log2 = 0; break;
      default: other.image.metadata.push_back({awj::MetadataKind::icc, {std::byte{1}}}); break;
    }
    require(!backend.encode_memory(std::move(decoded)), "丢弃备用端／缩放掩盖了无效配对");
  }
  std::stop_source stopped; stopped.request_stop();
  require(!backend.encode_memory(input(Kind::pair), stopped.get_token()), "呈现选择忽略取消");
}

void write_bytes(const fs::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream{path, std::ios::binary};
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(stream), "无法写测试夹具");
}

void file_cases(const fs::path& root, awj::FileLogger& logger) {
  for (const auto kind : {Kind::sdr, Kind::hdr, Kind::pair, Kind::reverse_pair, Kind::hdr_pair}) {
    auto decoded = input(kind);
    awj::NativeEncodeSettings settings{.output_format = awj::OutputFormat::jxl,
        .quality = 100, .speed = 10, .bit_depth = 16, .resources = resources};
    settings.gain_map_alternative = decoded.gain_map_alternative ? &*decoded.gain_map_alternative : nullptr;
    const auto encoded = take(awj::JXLImageEncoder{}.encode(decoded.image, settings));
    const auto path = root / std::format("input-{}.jxl", static_cast<int>(kind));
    write_bytes(path, encoded.encoded.bytes);
    for (const auto format : {awj::OutputFormat::jxl, awj::OutputFormat::avif, awj::OutputFormat::jpgli,
                              awj::OutputFormat::webp, awj::OutputFormat::png}) {
      auto cfg = config(format, awj::JpegHdrMode::automatic, false);
      cfg.output_dir = root / std::format("output-{}-{}", static_cast<int>(kind), static_cast<int>(format));
      awj::NativeBackend backend{cfg, logger, resources};
      const auto result = backend.encode({.path = path, .bytes = encoded.encoded.bytes.size()});
      require(result.ok, result.message);
      const bool expected_map = (paired(kind) && (format == awj::OutputFormat::jxl || format == awj::OutputFormat::avif)) ||
          (has_hdr(kind) && format == awj::OutputFormat::jpgli);
      require(result.rendition.gain_map_generated == expected_map, "文件入口绕过呈现策略");
      auto reader = decoder(format);
      const auto actual = take(reader->decode(result.output_path,
          {.gain_map_mode = awj::GainMapDecodeMode::preserve_both}));
      require(actual.gain_map_alternative.has_value() == expected_map, "文件输出实际 Gain Map 不符");
    }
  }
}
}

int main(int argc, char** argv) {
  try {
    require(argc == 2, "用法：awj_rendition_policy_tests <项目内证据目录>");
    const auto root = fs::path{argv[1]} / std::format("run-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root);
    awj::FileLogger logger{root, false};
    unsigned cases{};
    for (const bool strip : {false, true}) for (const auto kind : kinds)
      for (const auto format : {awj::OutputFormat::jxl, awj::OutputFormat::avif, awj::OutputFormat::png,
                                awj::OutputFormat::jpgli, awj::OutputFormat::webp}) {
        for (const auto mode : {awj::JpegHdrMode::automatic, awj::JpegHdrMode::sdr, awj::JpegHdrMode::hdr}) {
          if (format != awj::OutputFormat::jpgli && mode != awj::JpegHdrMode::automatic) continue;
          run_case(kind, format, mode, strip, 16, 9, logger); ++cases;
        }
      }
    for (const auto kind : {Kind::hdr, Kind::pair, Kind::hdr_pair})
      for (const auto format : {awj::OutputFormat::jxl, awj::OutputFormat::avif, awj::OutputFormat::png,
                                awj::OutputFormat::jpgli, awj::OutputFormat::webp}) {
        run_case(kind, format, awj::JpegHdrMode::automatic, false, 18, 12, logger); ++cases;
      }
    invalid_pairs(logger);
    file_cases(root, logger);
    std::printf("%u 个内存策略组合、8 个失败／取消边界、25 个实际文件闭环通过。夹具与输出：%s\n",
        cases, root.string().c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what()); return 1;
  }
}
