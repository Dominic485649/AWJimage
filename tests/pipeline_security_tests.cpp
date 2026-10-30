#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <limits>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

import awj.codec;
import awj.config;
import awj.core;
import awj.image;
import awj.large_image_plan;
import awj.native_backend;
import awj.pipeline;
import awj.resource_planner;
import awj.webp_codec;
import awj.png_codec;
import awj.avif_aom_codec;

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

awj::ImageBuffer make_test_image(std::byte red = std::byte{255}) {
  awj::ImagePlane plane{.stride = 8};
  plane.bytes = {
      red,          std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0}, std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0}, std::byte{0},   std::byte{255}, std::byte{255},
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

std::expected<void, std::string> write_webp(const std::filesystem::path& path,
                                            std::byte red = std::byte{255}) {
  awj::WebPImageEncoder encoder;
  auto encoded = encoder.encode(
      make_test_image(red),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                .quality = 100,
                                .speed = 10,
                                .resources = awj::ResourcePlan{
                                    .file_parallelism = 1,
                                    .encoder_threads_per_file = 1,
                                    .global_thread_budget = 1}});
  if (!encoded) {
    return std::unexpected{encoded.error()};
  }
  std::ofstream stream{path, std::ios::binary};
  stream.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
               static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  if (!stream) {
    return std::unexpected{"failed to write test webp."};
  }
  return {};
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream}, {}};
}

}  // namespace

int main() {
  {
    constexpr std::uint64_t mib = 1024ull * 1024;
    constexpr std::uint64_t n = 2048ull * 1536;
    auto dimensions = awj::make_image_dimensions(2048, 1536);
    dimensions.opaque_sdr8_yuv420 = true;
    dimensions.source_bit_depth = 8;
    dimensions.source_dynamic_range = awj::ImageDynamicRangeHint::sdr;
    dimensions.decoded_storage_bytes = n * 3 / 2;
    auto defaults = awj::default_app_config();
    defaults.max_jobs = 1;
    const auto baseline = awj::pipeline_detail::estimated_regular_memory(dimensions, defaults);
    if (baseline.profile != "avif-native-sdr420" ||
        baseline.bytes >= awj::avif_encode_working_set_bytes_for_dimensions(dimensions))
      return fail("Confirmed ordinary SDR420 did not use its own memory profile");
    for (int mode = 0; mode < 10; ++mode) {
      auto cfg = defaults;
      switch (mode) {
        case 0: cfg.quality = 100; break;
        case 1: cfg.visual_quality = 90; break;
        case 2: cfg.alpha_policy = awj::AlphaModePolicy::force; break;
        case 3: cfg.transfer_characteristics = 16; break;
        case 4: cfg.color_primaries = 9; break;
        case 5: cfg.matrix_coefficients = 9; break;
        case 6: cfg.color_range = 1; break;
        case 7: cfg.bit_depth = 12; break;
        case 8: cfg.chroma_mode = awj::ChromaMode::yuv444; break;
        case 9: cfg.avif_color_representation = awj::AvifColorRepresentation::rgb_identity; break;
      }
      const auto guarded = awj::pipeline_detail::estimated_regular_memory(dimensions, cfg);
      if (guarded.profile == "avif-native-sdr420" || guarded.bytes < baseline.bytes)
        return fail("HDR, alpha or overridden output used an ordinary SDR memory profile");
    }
    auto unknown = dimensions;
    unknown.source_bit_depth = 0;
    unknown.source_dynamic_range = awj::ImageDynamicRangeHint::unknown;
    unknown.decoded_storage_bytes = 0;
    const auto fallback = awj::pipeline_detail::estimated_regular_memory(unknown, defaults);
    if (fallback.profile != "avif-general" || fallback.bytes < baseline.bytes)
      return fail("An old native420 flag or missing hints silently treated unknown input as SDR");

    auto rgb = dimensions;
    rgb.opaque_sdr8_yuv420 = false;
    rgb.decoded_storage_bytes = n * 4;
    const auto ordinary = awj::pipeline_detail::estimated_regular_memory(rgb, defaults);
    if (ordinary.profile != "avif-sdr8" || ordinary.bytes >=
        awj::avif_encode_working_set_bytes_for_dimensions(rgb))
      return fail("Known SDR packed input still paid the complete unknown/HDR AVIF cost");
    auto thumbnail_cfg = defaults;
    thumbnail_cfg.image_size_limit.mode = awj::ImageSizeLimitMode::manual;
    thumbnail_cfg.image_size_limit.max_long_edge = 512;
    const auto thumbnail = awj::pipeline_detail::estimated_regular_memory(rgb, thumbnail_cfg);
    if (thumbnail.target_width != 512 || thumbnail.target_height != 384 ||
        thumbnail.source_width != 2048 || thumbnail.source_height != 1536 ||
        thumbnail.source_resident_bytes != ordinary.source_resident_bytes ||
        thumbnail.decode_bytes != ordinary.decode_bytes ||
        thumbnail.encode_bytes >= ordinary.encode_bytes ||
        thumbnail.bytes != std::max({thumbnail.decode_bytes, thumbnail.prepare_bytes,
            thumbnail.encode_bytes, thumbnail.metric_bytes, thumbnail.output_bytes}))
      return fail("Thumbnail planning dropped source residency or used source-sized encoding/summed phases");

    auto gain = dimensions; // An accidental native420 flag must not beat Gain Map semantics.
    gain.source_has_gain_map = true;
    gain.source_bit_depth = 10;
    gain.source_dynamic_range = awj::ImageDynamicRangeHint::hdr;
    gain.decoded_storage_bytes = n * 8;
    gain.gain_map_width = 512;
    gain.gain_map_height = 384;
    const auto dual = awj::pipeline_detail::estimated_regular_memory(gain, defaults);
    auto missing_map_geometry = gain;
    missing_map_geometry.gain_map_width = missing_map_geometry.gain_map_height = 0;
    const auto full_map = awj::pipeline_detail::estimated_regular_memory(missing_map_geometry, defaults);
    if (dual.profile != "avif-dual-gain-map" || dual.source_resident_bytes < n * 24 ||
        dual.prepare_bytes < dual.source_resident_bytes + n * 12 ||
        full_map.bytes < dual.bytes || full_map.decode_bytes <= dual.decode_bytes ||
        full_map.source_resident_bytes != dual.source_resident_bytes)
      return fail("Gain Map planning lost an endpoint, decode-only auxiliary lifetime or conservative geometry");

    auto jpeg = defaults;
    jpeg.output_format = awj::OutputFormat::jpgli;
    const auto sdr_jpeg = awj::pipeline_detail::estimated_regular_memory(rgb, jpeg);
    const auto unknown_jpeg = awj::pipeline_detail::estimated_regular_memory(unknown, jpeg);
    if (sdr_jpeg.profile != "jpegli-sdr" || sdr_jpeg.bytes >= unknown_jpeg.bytes)
      return fail("Ordinary SDR JPEGli still reserved the complete HDR pipeline");
    jpeg.jpeg_hdr = awj::JpegHdrMode::sdr;
    if (awj::pipeline_detail::estimated_regular_memory(gain, jpeg).profile != "jpegli-source-sdr-presentation")
      return fail("Forced SDR Gain Map output did not plan the saved source SDR presentation");

    auto png = defaults;
    png.output_format = awj::OutputFormat::png;
    const auto rgba8 = awj::pipeline_detail::estimated_regular_memory(rgb, png);
    auto high_depth = rgb;
    high_depth.source_bit_depth = 16;
    high_depth.source_has_alpha = true;
    high_depth.decoded_storage_bytes = n * 8;
    const auto rgba16 = awj::pipeline_detail::estimated_regular_memory(high_depth, png);
    auto explicit_png16 = png;
    explicit_png16.bit_depth = 16;
    const auto requested16 = awj::pipeline_detail::estimated_regular_memory(high_depth, explicit_png16);
    if (rgba8.profile != "png-rgba8" || rgba8.bytes >= 128 * mib ||
        rgba16.profile != "png-rgba8" || rgba16.bytes <= rgba8.bytes || rgba16.bytes < 73 * mib ||
        requested16.profile != "png-rgba16" || requested16.encode_bytes <= rgba16.encode_bytes)
      return fail("PNG planning ignored SDR auto8, retained 16-bit source storage or explicit16 output");
    const auto hdr_single = awj::pipeline_detail::estimated_regular_memory(gain, png);
    if (hdr_single.source_resident_bytes >= dual.source_resident_bytes ||
        hdr_single.source_resident_bytes < n * 16)
      return fail("HDR-only PNG retained an unused alternate endpoint or dropped its selected HDR storage");

    auto alpha = rgb;
    alpha.source_has_alpha = true;
    auto webp = defaults;
    webp.output_format = awj::OutputFormat::webp;
    auto jxl = defaults;
    jxl.output_format = awj::OutputFormat::jxl;
    if (awj::pipeline_detail::estimated_regular_memory(alpha, webp).bytes < 115 * mib ||
        awj::pipeline_detail::estimated_regular_memory(rgb, jxl).bytes < 188 * mib ||
        awj::pipeline_detail::estimated_regular_memory(alpha, jxl).bytes < 261 * mib ||
        awj::pipeline_detail::estimated_regular_memory(gain, jxl).profile != "jxl-dual-gain-map")
      return fail("WebP/JXL planning is below the recorded 3MP workload or lost dual presentation");
    auto threaded = jxl;
    threaded.max_jobs = 8;
    if (awj::pipeline_detail::estimated_regular_memory(alpha, threaded).bytes <=
        awj::pipeline_detail::estimated_regular_memory(alpha, jxl).bytes)
      return fail("Codec thread headroom did not participate in memory admission");
    auto metrics = defaults;
    metrics.visual_quality = 90;
    const auto measured_target = awj::pipeline_detail::estimated_regular_memory(rgb, metrics);
    if (!measured_target.metric_bytes || measured_target.encode_bytes <= ordinary.encode_bytes)
      return fail("Cached metric reference/best-candidate storage was omitted from candidate encoding");
    auto metric_thumbnail_cfg = metrics;
    metric_thumbnail_cfg.image_size_limit = thumbnail_cfg.image_size_limit;
    const auto metric_thumbnail = awj::pipeline_detail::estimated_regular_memory(rgb, metric_thumbnail_cfg);
    auto padded_source = rgb;
    padded_source.decoded_storage_bytes = n * 8;
    const auto padded_thumbnail = awj::pipeline_detail::estimated_regular_memory(padded_source, metric_thumbnail_cfg);
    if (metric_thumbnail.metric_bytes >= measured_target.metric_bytes ||
        padded_thumbnail.decode_bytes <= metric_thumbnail.decode_bytes ||
        padded_thumbnail.encode_bytes != metric_thumbnail.encode_bytes)
      return fail("Physical metric resize retained already released source padding or source-sized metric workspaces");
    auto icc_dimensions = rgb;
    icc_dimensions.source_icc_bytes = 660;
    const auto ordinary_icc_hint = awj::pipeline_detail::estimated_regular_memory(icc_dimensions, defaults);
    if (ordinary_icc_hint.decode_bytes != ordinary.decode_bytes ||
        ordinary_icc_hint.prepare_bytes != ordinary.prepare_bytes)
      return fail("非 Gain Map 输入错误地支付了新增 ICC 校验暂存。");
    icc_dimensions.source_has_gain_map = true;
    const auto icc_estimate = awj::pipeline_detail::estimated_regular_memory(icc_dimensions, defaults);
    icc_dimensions.source_icc_bytes = 0;
    const auto no_icc_estimate = awj::pipeline_detail::estimated_regular_memory(icc_dimensions, defaults);
    if (icc_estimate.decode_bytes != no_icc_estimate.decode_bytes + 660 ||
        icc_estimate.prepare_bytes != no_icc_estimate.prepare_bytes + 660 ||
        icc_estimate.encode_bytes != no_icc_estimate.encode_bytes)
      return fail("Gain Map ICC 校验没有按实际单 profile 字节计入对应阶段。");
    icc_dimensions.source_icc_bytes = UINT64_MAX;
    if (awj::pipeline_detail::estimated_regular_memory(icc_dimensions, defaults).bytes != UINT64_MAX)
      return fail("ICC profile 字节计费溢出后错误准入。");
    auto overflow = awj::make_image_dimensions(UINT32_MAX, UINT32_MAX);
    if (awj::pipeline_detail::estimated_regular_memory(overflow, defaults).bytes != UINT64_MAX ||
        awj::pipeline_detail::estimated_regular_memory(rgb, png, UINT64_MAX).bytes != UINT64_MAX ||
        awj::pipeline_detail::estimated_regular_memory({}, defaults).bytes != UINT64_MAX)
      return fail("Image working-set dimensions/input arithmetic overflowed or admitted empty geometry");
  }
  const auto root = std::filesystem::temp_directory_path() / "awjimage-pipeline-security-tests";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return fail("failed to create temp root.");
  }
  {
    const auto input = root / "memory-unlock.webp";
    if (auto written = write_webp(input); !written) return fail(written.error());
    for (const auto configured : {0ull, 64ull * 1024 * 1024}) {
      auto cfg = awj::default_app_config();
      cfg.input_path = input;
      cfg.output_dir = root / std::format("memory-unlock-{}", configured);
      cfg.output_format = awj::OutputFormat::png;
      cfg.quality = 100;
      cfg.max_jobs = 1;
      cfg.memory_limit_bytes = configured;
      cfg.unlock_memory_limit = true;
      cfg.write_summary = true;
      bool traced = false;
      auto summary = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
        if (event.kind == awj::BatchEventKind::item_finished) {
          const auto& result = event.result;
          traced = result.ok && result.memory_budget_bytes == 0 &&
              result.memory_budget_mode == "unlocked" && result.memory_budget_source == "session_unlock" &&
              result.estimated_working_set_bytes > 0 && !result.memory_estimate_profile.empty() &&
              result.memory_source_width == 2 && result.memory_source_height == 2 &&
              result.memory_target_width == 2 && result.memory_target_height == 2;
        }
      });
      const auto csv = read_text(cfg.output_dir / "summary.csv");
      if (!summary || summary->ok_count != 1 || !traced ||
          csv.find("memory_budget_bytes,memory_budget_mode,memory_budget_source,estimated_working_set_bytes") == std::string::npos ||
          csv.find(",0,unlocked,session_unlock,") == std::string::npos)
        return fail("Successful session-unlocked work lost zero budget, mode/source or estimate CSV diagnostics.");
      cfg.output_format = awj::OutputFormat::avif;
      cfg.studio_large_action = L"grid";
      cfg.output_dir = root / std::format("manual-memory-unlock-{}", configured);
      std::stop_source cancel;
      cancel.request_stop();
      bool manual_traced = false;
      auto manual = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
        if (event.kind == awj::BatchEventKind::item_finished) {
          const auto& result = event.result;
          manual_traced = result.canceled && result.memory_budget_bytes == 0 &&
              result.memory_budget_mode == "unlocked" && result.memory_budget_source == "session_unlock" &&
              result.estimated_working_set_bytes > 0 && result.encoder_threads == 0 &&
              result.seconds == 0 && result.decode_seconds == 0 && result.encode_seconds == 0;
        }
      }, cancel.get_token());
      if (!manual || !manual->canceled || !manual_traced ||
          read_text(cfg.output_dir / "summary.csv").find(",0,unlocked,session_unlock,") == std::string::npos)
        return fail("Manual Grid worker reinterpreted unlocked zero as automatic or fabricated unstarted measurements.");
    }
  }
  {
    const auto source = make_test_image();
    const awj::ResourcePlan resources{.file_parallelism = 1,
                                      .encoder_threads_per_file = 1,
                                      .global_thread_budget = 1};
    auto encoder = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
    auto decoder = awj::make_avif_image_decoder(1);
    if (!encoder || !decoder) return fail("AVIF depth fixture codecs are unavailable");
    awj::FileLogger logger{root, false};
    for (const int depth : {8, 10, 12}) {
      const auto context = std::format("{}-bit AVIF to auto PNG", depth);
      auto encoded = encoder->encode(source, awj::NativeEncodeSettings{
          .output_format = awj::OutputFormat::avif,
          .quality = 80,
          .speed = 10,
          .bit_depth = depth,
          .bit_depth_explicit = true,
          .chroma_mode = awj::ChromaMode::yuv420,
          .avif_encoder = awj::AvifEncoderMode::aom,
          .resources = resources});
      if (!encoded) return fail(context + ": " + encoded.error());
      const auto input = root / std::format("backend-png-depth-{}.avif", depth);
      {
        std::ofstream stream{input, std::ios::binary};
        stream.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                     static_cast<std::streamsize>(encoded->encoded.bytes.size()));
        if (!stream) return fail(context + ": could not write AVIF fixture");
      }
      auto fixture = decoder->decode(input, awj::DecodeOptions{.native_pixels = true});
      if (!fixture) return fail(context + ": " + fixture.error());
      if (fixture->image.width != source.width || fixture->image.height != source.height ||
          fixture->image.bit_depth != depth || !fixture->image.source_info ||
          fixture->image.source_info->bit_depth != depth)
        return fail(context + ": AVIF fixture did not retain its explicit depth");

      auto cfg = awj::default_app_config();
      cfg.input_path = input;
      cfg.output_dir = root / std::format("backend-png-depth-{}-out", depth);
      cfg.output_format = awj::OutputFormat::png;
      cfg.quality = 100;
      cfg.max_jobs = 1;
      // 保持 PNG 自动位深；这里强制 16 会掩盖后端位深回归。
      cfg.bit_depth.reset();
      std::filesystem::create_directories(cfg.output_dir, ec);
      if (ec) return fail(context + ": could not create PNG output directory");
      awj::NativeBackend backend{cfg, logger, resources};
      const auto result = backend.encode(awj::ImageFile{
          .index = 0, .path = input,
          .bytes = static_cast<std::uint64_t>(encoded->encoded.bytes.size())});
      if (!result.ok || result.skipped || result.canceled)
        return fail(context + ": backend conversion failed: " + result.message);
      auto restored = awj::PngImageDecoder{}.decode(result.output_path);
      if (!restored) return fail(context + ": " + restored.error());
      const int storage_depth = 8; // 所选呈现为 SDR，PNG 自动位深固定为兼容的 8-bit。
      const auto png_bytes = read_text(result.output_path);
      if (restored->image.width != source.width || restored->image.height != source.height ||
          restored->image.bit_depth != storage_depth || png_bytes.size() < 25 ||
          static_cast<unsigned char>(png_bytes[24]) != storage_depth)
        return fail(context + ": PNG dimensions or IHDR storage depth changed");
    }
  }
  {
    auto cfg = awj::default_app_config();
    const std::vector<awj::ImageFile> unknown{{.path = root / "missing.jpg", .bytes = 7}};
    constexpr auto budget = 512ull * 1024 * 1024;
    for (auto format : {awj::OutputFormat::avif, awj::OutputFormat::png}) {
      cfg.output_format = format;
      auto work = awj::pipeline_detail::classify_work_for_avif(cfg, unknown, budget);
      if (!work || work->ordinary.size() != 1 || work->ordinary.front().estimated_bytes != budget)
        return fail("Unknown dimensions used compressed size instead of exclusive admission");
    }
  }
  {
    auto cfg = awj::default_app_config();
    cfg.output_format = awj::OutputFormat::png;
    const auto bytes = awj::pipeline_detail::estimated_regular_working_set_bytes(
        awj::make_image_dimensions(4096, 4096), cfg);
    const auto plan = awj::plan_resources({.automatic_thread_budget = 8, .file_count = 2,
        .memory_limit_bytes = 1024ull * 1024 * 1024, .estimated_bytes_per_file = bytes});
    if (bytes <= 512ull * 1024 * 1024 || plan.file_parallelism != 1 || plan.encoder_threads_per_file != 8)
      return fail("PNG planning omitted high-depth/output buffers or lost the remaining thread budget");
  }
  {
    auto cfg = awj::default_app_config();
    cfg.output_dir = root / "grid-memory-preflight";
    cfg.memory_limit_bytes = 64ull * 1024 * 1024;
    awj::FileLogger logger{root, false};
    awj::BatchLargeImageItem oversized{
        .file = {.path = root / "nonexistent-huge-input.png"},
        .dimensions = awj::make_image_dimensions(65537, 4096),
        .decision = {.available_grid = true}};
    const auto result = awj::pipeline_detail::encode_large_mode_item(cfg, logger, oversized,
        {.memory_limit_bytes = cfg.memory_limit_bytes}, {});
    if (result.ok || result.message.find("超过内存预算") == std::string::npos ||
        result.memory_budget_bytes != cfg.memory_limit_bytes || result.memory_budget_mode != "manual" ||
        result.memory_budget_source != "configuration" ||
        result.estimated_working_set_bytes <= result.memory_budget_bytes ||
        result.memory_source_width != 65537 || result.memory_source_height != 4096 ||
        result.encoder_threads != 0 || result.seconds != 0 || result.decode_seconds != 0 ||
        result.prepare_seconds != 0 || result.encode_seconds != 0 || result.write_seconds != 0)
      return fail("Grid preflight failed to record its actual budget/estimate or fabricated encoder work");
    if (auto csv = awj::write_csv(cfg.output_dir, std::span<const awj::EncodeResult>{&result, 1}); !csv)
      return fail(csv.error());
    if (read_text(cfg.output_dir / "summary.csv").find(",67108864,manual,configuration,") == std::string::npos)
      return fail("Budget rejection CSV silently emitted zero instead of the actual manual budget");
  }
  {
    auto wide = make_test_image();
    wide.width = 65537;
    wide.height = 64;
    wide.planes[0].stride = wide.width * 4;
    wide.planes[0].bytes.assign(wide.planes[0].stride * wide.height, std::byte{255});
    auto png = awj::PngImageEncoder{}.encode(wide, {.quality = 100});
    if (!png) return fail(png.error());
    const auto input = root / "auto-grid.png";
    {
      std::ofstream stream{input, std::ios::binary};
      stream.write(reinterpret_cast<const char*>(png->encoded.bytes.data()), png->encoded.bytes.size());
      if (!stream) return fail("could not write automatic Grid fixture");
    }
    auto cfg = awj::default_app_config();
    cfg.input_path = input;
    cfg.output_dir = root / "auto-grid-output";
    cfg.quality = 50;
    cfg.speed = 8;
    cfg.max_jobs = 2;
    cfg.memory_limit_bytes = 2ull * 1024 * 1024 * 1024;
    cfg.chroma_mode = awj::ChromaMode::yuv444;
    cfg.write_summary = true;
    auto rejected_cfg = cfg;
    rejected_cfg.output_dir = root / "auto-grid-memory-reject";
    rejected_cfg.memory_limit_bytes = 64ull * 1024 * 1024;
    bool rejected_before_start = false;
    int rejected_started = 0;
    auto rejected_summary = awj::run_batch(rejected_cfg, [&](const awj::BatchProgress& event) {
      if (event.kind == awj::BatchEventKind::item_started) ++rejected_started;
      if (event.kind == awj::BatchEventKind::item_finished) {
        const auto& result = event.result;
        rejected_before_start = !result.ok && result.memory_budget_bytes == rejected_cfg.memory_limit_bytes &&
            result.memory_budget_mode == "manual" && result.memory_budget_source == "configuration" &&
            result.estimated_working_set_bytes > result.memory_budget_bytes &&
            result.memory_estimate_profile.starts_with("avif-grid") &&
            result.memory_source_width == 65537 && result.memory_source_height == 64 &&
            result.encoder_threads == 0 && result.decode_seconds == 0 && result.encode_seconds == 0;
      }
    });
    if (!rejected_summary || rejected_summary->failed_count != 1 || !rejected_before_start || rejected_started ||
        read_text(rejected_cfg.output_dir / "summary.csv").find(",67108864,manual,configuration,") == std::string::npos)
      return fail("Batch admission rejection lost its Grid estimate/budget or reported encoder work before start");
    bool successful_trace = false;
    auto summary = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
      if (event.kind == awj::BatchEventKind::item_finished)
        successful_trace = event.result.ok && event.result.memory_budget_bytes == cfg.memory_limit_bytes &&
            event.result.memory_budget_mode == "manual" && event.result.memory_budget_source == "configuration" &&
            event.result.memory_estimate_profile.starts_with("avif-grid") &&
            event.result.estimated_working_set_bytes > 0;
    });
    if (!summary) return fail(summary.error());
    if (summary->ok_count != 1 || !successful_trace) return fail("automatic Grid encode failed or lost memory diagnostics: " + read_text(cfg.output_dir / "summary.csv"));
    auto decoded = awj::make_avif_image_decoder(1)->decode(cfg.output_dir / "auto-grid.avif");
    if (!summary || summary->ok_count != 1 || !decoded || decoded->image.width != 65537 || decoded->image.height != 64)
      return fail(decoded ? "automatic Grid changed original dimensions" : decoded.error());
    cfg.output_dir = root / "manual-grid-output";
    cfg.image_size_limit.mode = awj::ImageSizeLimitMode::manual;
    cfg.image_size_limit.max_width = 32768;
    summary = awj::run_batch(cfg);
    decoded = awj::make_avif_image_decoder(1)->decode(cfg.output_dir / "auto-grid.avif");
    if (!summary || summary->ok_count != 1 || !decoded || decoded->image.width != 32768 || decoded->image.height != 31)
      return fail(decoded ? "manual size limit left a stale Grid plan" : decoded.error());
  }

  std::vector<awj::ImageFile> manifest_files;
  for (std::size_t index = 0; index < 13; ++index) {
    manifest_files.push_back(awj::ImageFile{
        .index = index,
        .path = root / std::format("input-{}.webp", index),
        .relative_dir = "nested",
        .bytes = 123,
        .date_token = L"20260713",
        .time_token = L"120000",
        .datetime_token = L"20260713-120000",
        .unix_token = L"1783915200",
        .random_token = std::format(L"{:08x}", index),
        .hash_token = L"hash",
        .sha256_token = L"sha256",
        .resolved_output_path = root / std::format("output-{}.avif", index),
        .output_path_resolved = true});
  }
  const auto manifest_path = root / "studio-queue.awjq";
  if (auto written =
          awj::write_studio_queue_manifest(manifest_path, manifest_files);
      !written) {
    return fail(written.error());
  }
  auto manifest = awj::read_studio_queue_manifest(manifest_path);
  if (!manifest || manifest->files.size() != manifest_files.size() ||
      manifest->files.back().resolved_output_path !=
          manifest_files.back().resolved_output_path ||
      manifest->files.back().random_token !=
          manifest_files.back().random_token) {
    return fail(manifest ? "studio queue manifest round-trip changed data."
                         : manifest.error());
  }
  const auto manifest_plan = awj::plan_resources(awj::ResourcePlanRequest{
      .automatic_thread_budget = 8,
      .file_count = static_cast<int>(manifest->files.size()),
      .estimated_bytes_per_file = 1});
  if (manifest_plan.encoder_threads_per_file != 1 ||
      manifest_plan.file_parallelism *
              manifest_plan.encoder_threads_per_file !=
          manifest_plan.global_thread_budget) {
    return fail("13-file Studio worker plan did not keep one encoder thread.");
  }
  {
    std::fstream corrupt{manifest_path,
                         std::ios::binary | std::ios::in | std::ios::out};
    corrupt.seekp(8);
    const std::array<char, 4> invalid_version{};
    corrupt.write(invalid_version.data(), invalid_version.size());
  }
  if (auto invalid = awj::read_studio_queue_manifest(manifest_path); invalid) {
    return fail("studio queue manifest accepted an invalid version.");
  }

  if (auto written =
          awj::write_studio_queue_manifest(manifest_path, manifest_files);
      !written) {
    return fail(written.error());
  }
  {
    std::fstream inflated{manifest_path,
                           std::ios::binary | std::ios::in | std::ios::out};
    inflated.seekp(12);
    constexpr std::uint64_t declared_files = 1'000'000;
    for (int shift = 0; shift < 64; shift += 8) {
      inflated.put(static_cast<char>((declared_files >> shift) & 0xffu));
    }
  }
  std::filesystem::resize_file(manifest_path, 20, ec);
  if (ec) {
    return fail("failed to truncate inflated manifest.");
  }
  if (auto inflated = awj::read_studio_queue_manifest(manifest_path);
      inflated) {
    return fail("studio queue manifest trusted an inflated record count.");
  }

  auto traversal_files = manifest_files;
  traversal_files.front().relative_dir = "../escape";
  if (auto traversal = awj::write_studio_queue_manifest(
          root / "studio-queue-traversal.awjq", traversal_files);
      traversal) {
    return fail("studio queue manifest accepted parent path traversal.");
  }

  auto disambiguator_files = manifest_files;
  disambiguator_files.front().output_path_resolved = false;
  disambiguator_files.front().resolved_output_path.clear();
  disambiguator_files.front().extension_disambiguated = true;
  disambiguator_files.front().source_extension_disambiguator = L"/../escape";
  if (auto disambiguator = awj::write_studio_queue_manifest(
          root / "studio-queue-disambiguator.awjq", disambiguator_files);
      !disambiguator) {
    // The writer may reject malformed records before they reach the worker.
  } else {
    auto disambiguator_cfg = awj::default_app_config();
    disambiguator_cfg.input_path = root;
    disambiguator_cfg.output_dir = root / "allowed-output";
    disambiguator_cfg.studio_queue_manifest =
        root / "studio-queue-disambiguator.awjq";
    disambiguator_cfg.write_log = false;
    disambiguator_cfg.write_summary = false;
    if (auto escaped = awj::run_batch(disambiguator_cfg); escaped) {
      return fail("Studio worker accepted a malicious extension disambiguator.");
    }
  }

  if (auto written =
          awj::write_studio_queue_manifest(manifest_path, manifest_files);
      !written) {
    return fail(written.error());
  }
  auto manifest_cfg = awj::default_app_config();
  manifest_cfg.input_path = root;
  manifest_cfg.output_dir = root / "allowed-output";
  manifest_cfg.studio_queue_manifest = manifest_path;
  manifest_cfg.write_log = false;
  manifest_cfg.write_summary = false;
  auto escaped_output = awj::run_batch(manifest_cfg);
  if (escaped_output ||
      escaped_output.error().find("输出路径越界") == std::string::npos) {
    return fail("Studio worker accepted a resolved output outside output_dir.");
  }

  auto wrong_extension_files = manifest_files;
  wrong_extension_files.front().resolved_output_path =
      manifest_cfg.output_dir / "wrong-extension.txt";
  if (auto written = awj::write_studio_queue_manifest(
          manifest_path, wrong_extension_files);
      !written) {
    return fail(written.error());
  }
  auto wrong_extension = awj::run_batch(manifest_cfg);
  if (wrong_extension ||
      wrong_extension.error().find("扩展名无效") == std::string::npos) {
    return fail("Studio worker accepted an unexpected output extension.");
  }

  auto avif_png_files = manifest_files;
  for (std::size_t index = 0; index < avif_png_files.size(); ++index) {
    avif_png_files[index].resolved_output_path =
        manifest_cfg.output_dir / std::format("output-{}.avif.png", index);
  }
  if (auto written = awj::write_studio_queue_manifest(manifest_path, avif_png_files);
      !written) {
    return fail(written.error());
  }
  auto avif_png_cfg = manifest_cfg;
  avif_png_cfg.append_png_suffix = true;
  if (auto accepted = awj::run_batch(avif_png_cfg);
      !accepted && accepted.error().find("扩展名无效") != std::string::npos) {
    return fail("Studio worker rejected a valid AVIF.png manifest output.");
  }
  if (auto rejected = awj::run_batch(manifest_cfg);
      rejected || rejected.error().find("扩展名无效") == std::string::npos) {
    return fail("Studio worker accepted AVIF.png without the AVIF.png setting.");
  }

  auto input_overwrite_files = manifest_files;
  for (std::size_t index = 0; index < input_overwrite_files.size(); ++index) {
    input_overwrite_files[index].resolved_output_path =
        root / std::format("safe-output-{}.webp", index);
  }
  input_overwrite_files.front().resolved_output_path = manifest_files[1].path;
  if (auto written = awj::write_studio_queue_manifest(
          manifest_path, input_overwrite_files);
      !written) {
    return fail(written.error());
  }
  auto input_overwrite_cfg = manifest_cfg;
  input_overwrite_cfg.output_dir = root;
  input_overwrite_cfg.output_format = awj::OutputFormat::webp;
  auto input_overwrite = awj::run_batch(input_overwrite_cfg);
  if (input_overwrite ||
      input_overwrite.error().find("覆盖输入") == std::string::npos) {
    return fail("Studio worker accepted an output that overwrites another input.");
  }

  auto missing_cfg = awj::default_app_config();
  missing_cfg.input_path = root / "missing.webp";
  auto missing_summary = awj::run_batch(missing_cfg);
  if (missing_summary || missing_summary.error().find("不存在") == std::string::npos) {
    return fail("missing input path was not rejected clearly.");
  }

  const auto input_dir = root / "input";
  const auto output_dir = root / "out";
  std::filesystem::create_directories(input_dir, ec);
  if (ec) {
    return fail("failed to create input dir.");
  }
  if (auto ok = write_webp(input_dir / "same-a.webp", std::byte{255}); !ok) {
    return fail(ok.error());
  }
  if (auto ok = write_webp(input_dir / "same-b.webp", std::byte{128}); !ok) {
    return fail(ok.error());
  }

  const auto drag_input_dir = root / "drag-input";
  const auto drag_nested_dir = drag_input_dir / "nested";
  std::filesystem::create_directories(drag_nested_dir, ec);
  if (ec || !write_webp(drag_input_dir / "b.webp", std::byte{1}) ||
      !write_webp(drag_input_dir / "a.webp", std::byte{2}) ||
      !write_webp(drag_nested_dir / "c.webp", std::byte{3})) {
    return fail("failed to create recursive drag-input fixtures.");
  }
  std::ofstream{drag_input_dir / "ignored.txt", std::ios::binary} << "not an image";
  auto drag_scan_cfg = awj::default_app_config();
  drag_scan_cfg.input_path = drag_input_dir;
  drag_scan_cfg.output_dir = root / "drag-output";
  drag_scan_cfg.output_format = awj::OutputFormat::avif;
  std::vector<awj::ImageFile> drag_scanned;
  if (auto scanned = awj::scan_images(drag_scan_cfg, drag_scanned); !scanned ||
      drag_scanned.size() != 3 ||
      drag_scanned[0].path.filename() != "a.webp" ||
      drag_scanned[1].path.filename() != "b.webp" ||
      drag_scanned[2].path.filename() != "c.webp" ||
      drag_scanned[2].relative_dir != std::filesystem::path{"nested"}) {
    return fail(scanned ? "recursive drag scan lost deterministic order or relative paths."
                        : scanned.error());
  }
  const std::array<std::filesystem::path, 3> drag_targets{
      drag_input_dir, drag_nested_dir, drag_input_dir / "a.webp"};
  std::vector<awj::ImageFile> drag_multi_scanned;
  if (auto scanned = awj::scan_images(drag_scan_cfg, drag_targets,
                                      drag_multi_scanned);
      !scanned || drag_multi_scanned.size() != 3 ||
      drag_multi_scanned[2].relative_dir != std::filesystem::path{"nested"}) {
    return fail(scanned ? "multi-target drag scan did not de-duplicate recursively."
                        : scanned.error());
  }

  const auto invalid_batch_input = root / "invalid-batch-input";
  const auto invalid_batch_output = root / "invalid-batch-output";
  std::filesystem::create_directories(invalid_batch_input, ec);
  if (ec || !write_webp(invalid_batch_input / "valid.webp")) {
    return fail("failed to create valid input for probe failure isolation.");
  }
  std::ofstream{invalid_batch_input / "empty.webp", std::ios::binary};
  auto invalid_batch_cfg = awj::default_app_config();
  invalid_batch_cfg.input_path = invalid_batch_input;
  invalid_batch_cfg.output_dir = invalid_batch_output;
  invalid_batch_cfg.output_format = awj::OutputFormat::avif;
  invalid_batch_cfg.max_jobs = 2;
  invalid_batch_cfg.quality = 90;
  invalid_batch_cfg.write_log = true;
  invalid_batch_cfg.write_summary = true;
  awj::EncodeResult invalid_decode_result;
  bool saw_invalid_decode = false;
  const auto invalid_batch_summary = awj::run_batch(invalid_batch_cfg, [&](const awj::BatchProgress& event) {
    if (event.kind == awj::BatchEventKind::item_finished &&
        event.result.input_path.filename() == "empty.webp") {
      invalid_decode_result = event.result;
      saw_invalid_decode = true;
    }
  });
  if (!invalid_batch_summary || invalid_batch_summary->ok_count != 1 ||
      invalid_batch_summary->failed_count != 1 ||
      !std::filesystem::exists(invalid_batch_output / "valid.avif")) {
    return fail(invalid_batch_summary
                    ? "one invalid input stopped the rest of the AVIF batch."
                    : invalid_batch_summary.error());
  }
  const auto invalid_batch_csv =
      read_text(invalid_batch_output / "summary.csv");
  if (invalid_batch_csv.find("empty.webp") == std::string::npos ||
      invalid_batch_csv.find("failed") == std::string::npos ||
      invalid_batch_csv.find("decode_stage,source_metadata_available,selected_rendition,rendition_origin") == std::string::npos ||
      invalid_batch_csv.find("heif_tile_parallelism,heif_codec_thread_budget,awj_version\n") == std::string::npos) {
    return fail("invalid AVIF batch input was not recorded with its diagnostic columns.");
  }
  if (!saw_invalid_decode || invalid_decode_result.ok || !invalid_decode_result.processed ||
      invalid_decode_result.decode_stage.empty() || invalid_decode_result.source_metadata_available ||
      invalid_decode_result.seconds <= 0 || invalid_decode_result.decode_seconds < 0 ||
      invalid_decode_result.encoder_threads != 0 || invalid_decode_result.output_bytes != 0 ||
      invalid_decode_result.encode_seconds >= 0) {
    return fail("解码失败丢失执行阶段/真实耗时，或冒填尚未开始的编码与源元数据。");
  }
  const auto invalid_batch_log = read_text(invalid_batch_output / "log" / "awj.log");
  const auto failure_position = invalid_batch_log.find("[FAIL]");
  if (failure_position == std::string::npos ||
      invalid_batch_log.find("[FAIL]", failure_position + 1) != std::string::npos ||
      invalid_batch_log.find("empty.webp", failure_position) == std::string::npos ||
      invalid_batch_log.find("decode_stage=" + invalid_decode_result.decode_stage, failure_position) == std::string::npos ||
      invalid_batch_log.find("source_icc=unknown", failure_position) == std::string::npos) {
    return fail("普通解码失败没有恰好写入一条带真实阶段的文件日志，或把未知 ICC 误记为不存在。");
  }

#ifdef _WIN32
  const auto manifest_input_dir = root / "manifest-input";
  const auto manifest_output_dir = root / "manifest-output";
#else
  const auto manifest_input_dir = root / (std::string{"中文 input\n"} + char(0xff));
  const auto manifest_output_dir = root / (std::string{"中文 output\n"} + char(0xfe));
#endif
  std::filesystem::create_directories(manifest_input_dir, ec);
  if (ec) {
    return fail("failed to create manifest input dir.");
  }
  std::vector<awj::ImageFile> runnable_manifest_files;
  for (std::size_t index = 0; index < 13; ++index) {
    auto input_name = std::format("manifest-input-{}.webp", index);
#ifndef _WIN32
    // Same displayed replacement character, distinct POSIX file names.
    if (index < 2) input_name = std::string{"image-"} + char(0xff - index) + ".webp";
#endif
    const auto input = manifest_input_dir / input_name;
    if (auto ok = write_webp(input, std::byte{static_cast<unsigned char>(index)});
        !ok) {
      return fail(ok.error());
    }
    runnable_manifest_files.push_back(awj::ImageFile{
        .index = index,
        .path = input,
        .bytes = std::filesystem::file_size(input, ec),
        .resolved_output_path =
            manifest_output_dir / std::format("manifest-output-{}.webp", index),
        .output_path_resolved = true});
  }
  const auto runnable_manifest_path = root / "studio-queue-runnable.awjq";
  if (auto written = awj::write_studio_queue_manifest(
          runnable_manifest_path, runnable_manifest_files);
      !written) {
    return fail(written.error());
  }
  const auto native_roundtrip = awj::read_studio_queue_manifest(runnable_manifest_path);
  if (!native_roundtrip || native_roundtrip->files.size() != runnable_manifest_files.size())
    return fail("native manifest roundtrip failed");
  for (std::size_t i = 0; i < runnable_manifest_files.size(); ++i) {
    if (native_roundtrip->files[i].path != runnable_manifest_files[i].path ||
        native_roundtrip->files[i].resolved_output_path != runnable_manifest_files[i].resolved_output_path)
      return fail("native manifest path bytes changed");
  }
  auto runnable_manifest_cfg = awj::default_app_config();
  runnable_manifest_cfg.input_path = manifest_input_dir;
  runnable_manifest_cfg.output_dir = manifest_output_dir;
  runnable_manifest_cfg.output_format = awj::OutputFormat::webp;
  runnable_manifest_cfg.studio_queue_manifest = runnable_manifest_path;
  runnable_manifest_cfg.max_jobs = 8;
  runnable_manifest_cfg.quality = 100;
  runnable_manifest_cfg.speed = 10;
  runnable_manifest_cfg.write_log = false;
  runnable_manifest_cfg.write_summary = false;
  std::vector<int> manifest_encoder_threads(13, -1);
  std::atomic<int> manifest_started_count{0};
  auto runnable_manifest_summary = awj::run_batch(
      runnable_manifest_cfg, [&](const awj::BatchProgress& event) {
        if (event.kind == awj::BatchEventKind::item_started) {
          manifest_started_count.fetch_add(1, std::memory_order_relaxed);
        } else if (event.kind == awj::BatchEventKind::item_finished &&
                   event.result.index < manifest_encoder_threads.size()) {
          manifest_encoder_threads[event.result.index] =
              event.result.encoder_threads;
        }
      });
  if (!runnable_manifest_summary || runnable_manifest_summary->ok_count != 13 ||
      manifest_started_count.load(std::memory_order_relaxed) != 13 ||
      manifest_encoder_threads.size() != 13 ||
      std::ranges::any_of(manifest_encoder_threads,
                          [](int threads) { return threads != 1; })) {
    return fail(runnable_manifest_summary
                    ? "Studio manifest batch did not keep one encoder thread per image."
                    : runnable_manifest_summary.error());
  }

  auto runnable_scan_cfg = runnable_manifest_cfg;
  runnable_scan_cfg.studio_queue_manifest.clear();
  runnable_scan_cfg.output_dir = root / "scan-output";
  std::vector<int> scan_encoder_threads(13, -1);
  auto runnable_scan_summary = awj::run_batch(
      runnable_scan_cfg, [&](const awj::BatchProgress& event) {
        if (event.kind == awj::BatchEventKind::item_finished &&
            event.result.index < scan_encoder_threads.size()) {
          scan_encoder_threads[event.result.index] =
              event.result.encoder_threads;
        }
      });
  if (!runnable_scan_summary || runnable_scan_summary->ok_count != 13 ||
      scan_encoder_threads.size() != 13 ||
      std::ranges::any_of(scan_encoder_threads,
                          [](int threads) { return threads != 1; })) {
    return fail(runnable_scan_summary
                    ? "13-file scanned batch did not keep one encoder thread per image."
                    : runnable_scan_summary.error());
  }

  awj::AppConfig cfg = awj::default_app_config();
  cfg.input_path = input_dir;
  cfg.output_dir = output_dir;
  cfg.output_format = awj::OutputFormat::webp;
  cfg.output_template = L"same";
  cfg.collision_mode = awj::CollisionMode::suffix_random;
  cfg.max_jobs = 2;
  cfg.quality = 100;
  cfg.write_summary = true;
  auto summary = awj::run_batch(cfg);
  if (!summary || summary->ok_count != 2 || summary->failed_count != 0) {
    return fail(summary ? "suffix collision batch did not succeed." : summary.error());
  }

  std::vector<std::filesystem::path> outputs;
  for (const auto& entry : std::filesystem::directory_iterator{output_dir}) {
    if (entry.path().extension() == ".webp") {
      outputs.push_back(entry.path());
    }
  }
  if (outputs.size() != 2) {
    return fail("suffix collision did not create two distinct outputs.");
  }

  const auto numbered_dir = root / "numbered";
  std::filesystem::create_directories(numbered_dir, ec);
  if (ec) {
    return fail("failed to create numbered dir.");
  }
  const auto numbered_input = numbered_dir / "name.webp";
  if (auto ok = write_webp(numbered_input, std::byte{32}); !ok) {
    return fail(ok.error());
  }
  awj::AppConfig number_cfg = awj::default_app_config();
  number_cfg.input_path = numbered_input;
  number_cfg.output_dir = numbered_dir;
  number_cfg.output_format = awj::OutputFormat::webp;
  number_cfg.collision_mode = awj::CollisionMode::suffix_number;
  number_cfg.max_jobs = 1;
  number_cfg.quality = 100;
  auto number_summary = awj::run_batch(number_cfg);
  if (!number_summary || number_summary->ok_count != 1 ||
      !std::filesystem::exists(numbered_dir / "name(1).webp")) {
    return fail(number_summary ? "numbered collision did not create name(1)."
                               : number_summary.error());
  }
  number_cfg.input_path = numbered_dir / "name(1).webp";
  number_summary = awj::run_batch(number_cfg);
  if (!number_summary || number_summary->ok_count != 1 ||
      !std::filesystem::exists(numbered_dir / "name(2).webp") ||
      std::filesystem::exists(numbered_dir / "name(1)(1).webp")) {
    return fail(number_summary ? "numbered collision did not advance existing suffix."
                               : number_summary.error());
  }

#ifdef _WIN32
  const auto short_dir = root / "short-path";
  std::filesystem::create_directories(short_dir, ec);
  if (ec) {
    return fail("failed to create short-path dir.");
  }
  const auto long_webp = short_dir / "20B07A very long filename that must stay complete.webp";
  if (auto ok = write_webp(long_webp, std::byte{16}); !ok) {
    return fail(ok.error());
  }
  const DWORD needed = GetShortPathNameW(long_webp.c_str(), nullptr, 0);
  if (needed > 0) {
    std::wstring short_buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    const DWORD written = GetShortPathNameW(
        long_webp.c_str(), short_buffer.data(), static_cast<DWORD>(short_buffer.size()));
    if (written > 0 && written < short_buffer.size()) {
      short_buffer.resize(written);
      const std::filesystem::path short_webp{short_buffer};
      if (short_webp.native() != long_webp.native()) {
        const auto short_out = root / "short-out";
        awj::AppConfig short_cfg = awj::default_app_config();
        short_cfg.input_path = short_webp;
        short_cfg.output_dir = short_out;
        short_cfg.output_format = awj::OutputFormat::webp;
        short_cfg.collision_mode = awj::CollisionMode::suffix_number;
        short_cfg.max_jobs = 1;
        short_cfg.quality = 100;
        auto short_summary = awj::run_batch(short_cfg);
        if (!short_summary || short_summary->ok_count != 1 ||
            !std::filesystem::exists(
                short_out / "20B07A very long filename that must stay complete.webp")) {
          return fail(short_summary ? "short WEBP path did not keep long output name."
                                    : short_summary.error());
        }
      }
    }
  }

#endif

  const auto csv = read_text(output_dir / "summary.csv");
  if (csv.find("same-a.webp") == std::string::npos ||
      csv.find(root.string()) != std::string::npos) {
    return fail("summary should contain file names but not full temp root paths.");
  }

  const auto formula_input = input_dir / "=cmd.webp";
  if (auto ok = write_webp(formula_input, std::byte{64}); !ok) {
    return fail(ok.error());
  }
  awj::ImageFile image{.index = 0,
                       .path = formula_input,
                       .bytes = std::filesystem::file_size(formula_input, ec)};
  awj::EncodeResult formula_result{.index = 0,
                                   .input_path = image.path,
                                   .output_path = output_dir / "=cmd.webp",
                                   .original_bytes = image.bytes,
                                   .output_bytes = image.bytes,
                                   .processed = true,
                                   .ok = true,
                                   .message = "=FORMULA"};
  if (auto csv_ok = awj::write_csv(output_dir / "csv-formula", std::span<const awj::EncodeResult>{&formula_result, 1}); !csv_ok) {
    return fail(csv_ok.error());
  }
  const auto formula_csv = read_text(output_dir / "csv-formula" / "summary.csv");
  if (formula_csv.find("'=cmd.webp") == std::string::npos ||
      formula_csv.find("'=FORMULA") == std::string::npos) {
    return fail("summary CSV did not escape formula-like cells.");
  }

  std::filesystem::remove_all(root, ec);
  return 0;
}
