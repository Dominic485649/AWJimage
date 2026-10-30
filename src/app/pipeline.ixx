module;
#include "../core/work_memory_admission.h"
#include "../core/hdr_cost.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <objbase.h>
#include <windows.h>
#else
#include <sys/sysinfo.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <print>
#include <ranges>
#include <stdexcept>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module awj.pipeline;

export import awj.batch_protocol;

import awj.avif_aom_codec;
import awj.animation_decoder;
import awj.avif_registry;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.large_image_plan;
import awj.resource_planner;
import awj.visual_quality;
import awj.native_backend;

export namespace awj {

namespace pipeline_detail {

struct WorkGroup {
  std::uintmax_t weight{};
  // 组内串行处理，峰值内存取成员估算的最大值，供运行时内存准入使用。
  std::uint64_t estimated_bytes{1};
  std::vector<ImageFile> files{};
};

struct LargeWorkGroup {
  std::uintmax_t weight{};
  std::uint64_t estimated_bytes{1};
  std::vector<BatchLargeImageItem> items{};
};

bool checked_add_uintmax(std::uintmax_t& value,
                         std::uintmax_t addend) noexcept {
  if (value > std::numeric_limits<std::uintmax_t>::max() - addend) {
    return false;
  }
  value += addend;
  return true;
}

std::uintmax_t saturated_add_uintmax(std::uintmax_t value,
                                     std::uintmax_t addend) noexcept {
  return checked_add_uintmax(value, addend)
             ? value
             : std::numeric_limits<std::uintmax_t>::max();
}

struct ClassifiedImageFile {
  ImageFile file{};
  std::uint64_t estimated_bytes{1};
  ImageDimensions dimensions{};
  bool grid{};
};

struct ClassifiedWork {
  std::vector<ClassifiedImageFile> ordinary{};
  std::vector<ClassifiedImageFile> memory_rejected{};
  std::vector<BatchLargeImageItem> large_mode{};
};

#ifdef _WIN32
class WicFallbackComApartment {
 public:
  explicit WicFallbackComApartment(bool enabled) noexcept
      : enabled_{enabled},
        init_{enabled ? CoInitializeEx(nullptr, COINIT_MULTITHREADED |
                                                    COINIT_DISABLE_OLE1DDE)
                      : S_FALSE} {}

  ~WicFallbackComApartment() {
    if (enabled_ && SUCCEEDED(init_)) {
      CoUninitialize();
    }
  }

  WicFallbackComApartment(const WicFallbackComApartment&) = delete;
  WicFallbackComApartment& operator=(const WicFallbackComApartment&) = delete;

  [[nodiscard]] bool usable() const noexcept {
    return !enabled_ || SUCCEEDED(init_) || init_ == RPC_E_CHANGED_MODE;
  }

  [[nodiscard]] HRESULT init() const noexcept { return init_; }

 private:
  bool enabled_{};
  HRESULT init_{S_FALSE};
};
#else
class WicFallbackComApartment {
 public:
  explicit WicFallbackComApartment(bool) noexcept {}
  [[nodiscard]] bool usable() const noexcept { return true; }
  [[nodiscard]] int init() const noexcept { return 0; }
};
#endif

int count_to_int_saturated(std::size_t value) noexcept {
  return value > static_cast<std::size_t>(std::numeric_limits<int>::max())
             ? std::numeric_limits<int>::max()
             : static_cast<int>(value);
}

std::string large_image_actions_text(const LargeImageDecision& decision) {
  std::string actions;
  if (decision.available_grid) {
    actions = "grid";
  }

  if (actions.empty()) {
    actions = "无可用大图编码器";
  }
  return actions;
}

std::string large_image_report_message(const BatchLargeImageItem& item) {
  return std::format("超大图自动处理：{}x{}，原因 {}；可用处理方式：{}；{}",
                     item.dimensions.width, item.dimensions.height,
                     large_image_reason_name(item.decision.reason),
                     large_image_actions_text(item.decision),
                     item.decision.reason_text);
}

std::string large_image_action_text(const BatchLargeImageItem& item) {
  const auto actions = large_image_actions_text(item.decision);
  return std::format(
      "[LARGE] {:04} {} ({}x{}, {}) -> 自动大图：{}；{}", item.file.index + 1,
      path_to_utf8(item.file.path.filename()), item.dimensions.width,
      item.dimensions.height, large_image_reason_name(item.decision.reason),
      actions, item.decision.reason_text);
}

std::string large_image_log_text(const BatchLargeImageItem& item) {
  const auto actions = large_image_actions_text(item.decision);
  return std::format("[LARGE] {:04} ({}x{}, {}) -> 自动大图：{}；{}",
                     item.file.index + 1, item.dimensions.width,
                     item.dimensions.height,
                     large_image_reason_name(item.decision.reason), actions,
                     item.decision.reason_text);
}

bool avif_capability_available_for_large_mode(
    const AvifEncoderCapability& capability) noexcept {
  return capability.enabled;
}

struct ImageMemoryEstimate {
  std::uint64_t bytes{1};
  std::uint64_t source_resident_bytes{};
  std::uint64_t decode_bytes{};
  std::uint64_t prepare_bytes{};
  std::uint64_t encode_bytes{};
  std::uint64_t metric_bytes{};
  std::uint64_t output_bytes{};
  std::uint32_t source_width{};
  std::uint32_t source_height{};
  std::uint32_t target_width{};
  std::uint32_t target_height{};
  bool source_has_gain_map{};
  std::string_view profile{"dimensions-unknown"};
};

ImageDimensions target_dimensions_for_config(ImageDimensions source,
                                              const AppConfig& cfg) noexcept {
  if (const auto target = limited_dimensions(source.width, source.height, cfg)) {
    source.width = static_cast<std::uint32_t>(target->first);
    source.height = static_cast<std::uint32_t>(target->second);
  }
  source.pixel_count = static_cast<std::uint64_t>(source.width) * source.height;
  return source;
}

// Reservations describe simultaneous live storage, not an allocator/RSS cap.
// Workspace headroom is deliberately branch-specific: the 2026-10-27 single-
// thread samples expose both SDR/HDR over-reservation and WebP/JXL undercounts;
// they do not establish universal compression ratios or multi-thread hard bounds.
ImageMemoryEstimate estimated_regular_memory(
    ImageDimensions dimensions, const AppConfig& cfg,
    std::uint64_t compressed_bytes = 0, bool grid = false) noexcept {
  using hdr_cost::add;
  using hdr_cost::multiply;
  constexpr std::uint64_t mib = 1024ull * 1024;
  const auto target = target_dimensions_for_config(dimensions, cfg);
  ImageMemoryEstimate estimate{};
  estimate.source_width = dimensions.width;
  estimate.source_height = dimensions.height;
  estimate.target_width = target.width;
  estimate.target_height = target.height;
  estimate.source_has_gain_map = dimensions.source_has_gain_map;
  if (!dimensions.width || !dimensions.height) {
    estimate.bytes = std::numeric_limits<std::uint64_t>::max();
    estimate.profile = "dimensions-invalid";
    return estimate;
  }

  const auto display_pixels = multiply(dimensions.width, dimensions.height);
  // 裁切后的显示尺寸可能远小于完整解码画布；像素存储画像仍包含完整 coded canvas。
  const auto storage_pixel_bytes = dimensions.source_bit_depth > 8 ? 8u : 4u;
  const auto stored_pixels = dimensions.decoded_storage_bytes / storage_pixel_bytes;
  const auto n = std::max(display_pixels, stored_pixels);
  const auto t = multiply(target.width, target.height);
  const auto padded = hdr_cost::padded_pixels(target.width, target.height, 64);
  const auto workers = static_cast<std::uint64_t>(std::max(1, cfg.max_jobs));
  // 覆盖各阶段有界 worker 控制及 scratch；Gain Map compute 每 worker 80,000 B，apply 16,384 B。
  const auto worker_scratch = multiply(workers, 256 * 1024);
  const bool known_depth = dimensions.source_bit_depth > 0;
  const bool sdr = dimensions.source_dynamic_range == ImageDynamicRangeHint::sdr &&
                   !dimensions.source_has_gain_map;
  const bool sdr8 = sdr && known_depth && dimensions.source_bit_depth <= 8;
  const bool possible_alpha = dimensions.source_has_alpha || !known_depth ||
                             dimensions.source_dynamic_range == ImageDynamicRangeHint::unknown;
  const bool visual_quality = cfg.visual_quality.has_value() && !grid;
  const bool native_sdr420 = sdr8 && !possible_alpha && dimensions.opaque_sdr8_yuv420;
  const auto native420 = add(n, multiply(multiply((static_cast<std::uint64_t>(dimensions.width) + 1) / 2,
      (static_cast<std::uint64_t>(dimensions.height) + 1) / 2), 2));
  const auto fallback_source = sdr && known_depth
      ? (native_sdr420 ? native420 : multiply(n, dimensions.source_bit_depth > 8 ? 8 : 4))
      : multiply(n, 16); // Unknown is not evidence of SDR or integer storage.
  auto source = dimensions.decoded_storage_bytes
      ? std::max(dimensions.decoded_storage_bytes,
                 multiply(n, dimensions.source_bit_depth > 8 ? 2 : 1))
      : fallback_source;
  // Metric decode requests packed pixels rather than native YUV storage.
  if (visual_quality)
    source = std::max(source, multiply(n, sdr && known_depth
        ? (dimensions.source_bit_depth > 8 ? 8 : 4) : 16));
  const bool dual = dimensions.source_has_gain_map &&
      (cfg.output_format == OutputFormat::avif || cfg.output_format == OutputFormat::jxl ||
       (cfg.output_format == OutputFormat::jpgli && cfg.jpeg_hdr != JpegHdrMode::sdr));
  auto decode_workspace = multiply(n, sdr8 ? 8 : 16);
  std::uint64_t decode_auxiliary{};
  if (dimensions.source_has_gain_map) {
    const auto gain_pixels = dimensions.gain_map_width && dimensions.gain_map_height
        ? multiply(dimensions.gain_map_width, dimensions.gain_map_height) : n;
    // 只有双端输出在解码后保留 alternate。aux 像素及单呈现的另一端只计解码峰值。
    const bool sdr_only = cfg.output_format == OutputFormat::webp ||
        (cfg.output_format == OutputFormat::jpgli && cfg.jpeg_hdr == JpegHdrMode::sdr);
    source = multiply(n, sdr_only ? 8 : 16);
    if (dual) source = add(source, multiply(n, 8));
    decode_auxiliary = add(dual ? 0 : multiply(n, 8), multiply(gain_pixels, 8));
    decode_workspace = add(decode_workspace, multiply(n, 12));
  }
  estimate.source_resident_bytes = source;
  // Containers can retain the complete compressed file and metadata/input
  // copies, including motion bytes not yet excluded by structured reads.
  const auto common = add(source, multiply(compressed_bytes, 2));
  estimate.decode_bytes = add(add(add(common, decode_auxiliary), decode_workspace), add(16 * mib, worker_scratch));
  const bool hdr_jpeg = cfg.output_format == OutputFormat::jpgli &&
      cfg.jpeg_hdr != JpegHdrMode::sdr &&
      (!sdr || cfg.jpeg_hdr == JpegHdrMode::hdr ||
       cfg.transfer_characteristics == 16 || cfg.transfer_characteristics == 18);
  const int target_depth = cfg.output_format == OutputFormat::webp ||
                                   (cfg.output_format == OutputFormat::jpgli && !hdr_jpeg)
      ? 8 : cfg.bit_depth.value_or(cfg.output_format == OutputFormat::avif
          ? (dimensions.source_has_gain_map || dimensions.source_bit_depth > 10 ? 12 : 10)
          : cfg.output_format == OutputFormat::jxl ? 16 : cfg.output_format == OutputFormat::png ? (sdr ? 8 : 16) : 8);
  const auto normalized = multiply(t, hdr_jpeg ? 8 : (target_depth > 8 ? 8 : 4));
  // Map regeneration keeps a target SDR endpoint and full-size auxiliary map;
  // neither alternate bit depth is available in the probe, so allow 16-bit
  // RGBA storage for each. libavif's three full-resolution float channels are
  // generation scratch, not scratch to add to the subsequent codec phase.
  const auto dual_resident = dual ? multiply(t, 16) : 0;
  const auto dual_compute = dual ? multiply(t, 3 * sizeof(float)) : 0;
  const auto tone_scratch = !sdr && !dimensions.source_has_gain_map
      ? hdr_cost::tone_scratch_bytes(workers) : 0;
  // A non-metric geometry view retains its original planes. Metric resize of a
  // plain image materializes packed pixels and replaces those planes; reserve
  // old+new during resize, not the released source again throughout encoding.
  // Gain Map endpoint ownership is conservatively retained until both finish.
  const bool physical_resize = visual_quality && !dimensions.source_has_gain_map &&
      (target.width != dimensions.width || target.height != dimensions.height);
  const auto resized_source = physical_resize ? multiply(t, sdr && known_depth
      ? (dimensions.source_bit_depth > 8 ? 8 : 4) : 16) : source;
  const auto encoder_common = add(resized_source, multiply(compressed_bytes, 2));
  const auto prepare_live = add(add(encoder_common, normalized), add(dual_resident, tone_scratch));
  estimate.prepare_bytes = add(add(prepare_live, dual_compute), add(8 * mib, worker_scratch));
  if (physical_resize)
    estimate.prepare_bytes = std::max(estimate.prepare_bytes,
        add(add(common, resized_source), add(8 * mib, worker_scratch)));
  source = resized_source;

  auto output_bound = add(multiply(t, target_depth > 8 ? 8 : 4), 64 * 1024);
  auto encoder_live = add(add(encoder_common, normalized), dual_resident);
  std::uint64_t fixed = 16 * mib;
  std::uint64_t codec_workspace{};
  switch (cfg.output_format) {
    case OutputFormat::avif: {
      const bool ordinary_sdr = sdr8 && cfg.quality < 100 && !visual_quality &&
          cfg.alpha_policy != AlphaModePolicy::force && !cfg.color_primaries &&
          !cfg.transfer_characteristics && !cfg.matrix_coefficients && !cfg.color_range &&
          cfg.avif_color_representation != AvifColorRepresentation::rgb_identity &&
          cfg.bit_depth.value_or(10) <= 10 &&
          (cfg.chroma_mode == ChromaMode::auto_keep || cfg.chroma_mode == ChromaMode::yuv420);
      const bool fast = ordinary_sdr && native_sdr420;
      estimate.profile = grid
          ? (dual ? "avif-grid-dual-gain-map" : fast ? "avif-grid-native-sdr420"
                  : ordinary_sdr ? "avif-grid-sdr8" : "avif-grid-general")
          : (dual ? "avif-dual-gain-map" : fast ? "avif-native-sdr420"
                  : ordinary_sdr ? "avif-sdr8" : "avif-general");
      // Native opaque 420 avoids general RGBA/HDR/alpha normalization. Other
      // confirmed SDR8 inputs still need conversion, but not HDR-sized scratch.
      codec_workspace = multiply(padded, fast ? 56 : ordinary_sdr ? 80 : 112);
      if (fast) encoder_live = add(common, multiply(t, target_depth > 8 ? 3 : 2));
      encoder_live = add(encoder_live, multiply(padded, fast ? 2 : (target_depth > 8 ? 8 : 4)));
      output_bound = add(multiply(padded, fast ? 4 : ordinary_sdr ? 6 : 8), 64 * 1024);
      fixed = add(32 * mib, multiply(workers - 1, mib));
      break;
    }
    case OutputFormat::png: {
      estimate.profile = target_depth > 8 ? "png-rgba16" : "png-rgba8";
      const auto raw = multiply(t, target_depth > 8 ? 8 : 4);
      // High-entropy deflate can approach raw scanline size. Account for
      // filters/chunks and old+new vector capacity, not the easy-image ratio.
      output_bound = add(add(raw, raw / 16), add(target.height, 64 * 1024));
      const auto row = multiply(target.width, target_depth > 8 ? 8 : 4);
      codec_workspace = add(2 * mib, multiply(row, 7));
      fixed = 8 * mib;
      break;
    }
    case OutputFormat::webp: {
      const bool lossless = cfg.visual_quality.value_or(cfg.quality) >= 100;
      estimate.profile = lossless ? "webp-lossless" : possible_alpha ? "webp-alpha" : "webp-sdr";
      const auto picture = multiply(t, lossless || possible_alpha ? 4 : 2);
      codec_workspace = add(picture, multiply(t, lossless ? 40 : possible_alpha ? 28 : 12));
      break;
    }
    case OutputFormat::jxl: {
      const bool lossless = cfg.visual_quality.value_or(cfg.quality) >= 100;
      estimate.profile = dual ? "jxl-dual-gain-map" : target_depth > 8 ? "jxl-high-depth"
                          : possible_alpha ? "jxl-sdr-alpha" : "jxl-sdr-rgb";
      const std::uint64_t scratch_per_pixel = target_depth > 8 ? (possible_alpha ? 112 : 80)
          : possible_alpha ? (lossless ? 88 : 72) : (lossless ? 64 : 48);
      // Opaque input can retain an additional RGB packing buffer alongside
      // source/normalized RGBA and libjxl's copied input and encoder workspace.
      const auto rgb = possible_alpha ? 0 : multiply(t, target_depth > 8 ? 6 : 3);
      codec_workspace = add(rgb, multiply(t, scratch_per_pixel));
      fixed = add(32 * mib, multiply(workers - 1, 2 * mib));
      break;
    }
    case OutputFormat::jpgli: {
      estimate.profile = hdr_jpeg ? (dual ? "jpegli-hdr-gain-map" : "jpegli-hdr-or-unknown")
          : sdr ? "jpegli-sdr" : dimensions.source_has_gain_map ? "jpegli-source-sdr-presentation"
          : "jpegli-tonemapped-or-unknown-sdr";
      estimate.encode_bytes = hdr_jpeg
          ? hdr_cost::jpeg_working_set_bytes(target.width, target.height, compressed_bytes, workers, source)
          : hdr_cost::jpeg_sdr_working_set_bytes(target.width, target.height, compressed_bytes,
                                                source, native_sdr420);
      output_bound = add(multiply(hdr_cost::padded_pixels(target.width, target.height, 16), 6), 64 * 1024);
      fixed = hdr_jpeg ? 32 * mib : 8 * mib;
      break;
    }
  }
  if (dual) output_bound = add(output_bound, multiply(t, 4));
  if (grid) encoder_live = add(encoder_live, multiply(padded, 8));
  const auto encode_phase = add(add(encoder_live, codec_workspace), multiply(output_bound, 3));
  estimate.encode_bytes = std::max(estimate.encode_bytes, add(encode_phase, add(fixed, worker_scratch)));
  if (dual && cfg.output_format != OutputFormat::jpgli) {
    // A retained main stream and map codec storage can coexist, but the main
    // and auxiliary codec workspaces are sequential, not additive.
    const auto auxiliary = add(add(encoder_live, multiply(t, 96)), multiply(output_bound, 3));
    estimate.encode_bytes = std::max(estimate.encode_bytes, add(auxiliary, add(fixed, worker_scratch)));
  }
  estimate.output_bytes = add(add(encoder_live, multiply(output_bound, 3)), add(fixed, worker_scratch));
  if (visual_quality) {
    const auto metric = visual_quality_working_set_bytes_for_dimensions(target);
    // The cached reference and best encoded candidate survive candidate encode;
    // candidate decode and the full metric pyramids peak in a different phase.
    estimate.encode_bytes = add(estimate.encode_bytes, add(metric / 2, output_bound));
    const auto candidate = multiply(t, target_depth > 8 ? 8 : 4);
    estimate.metric_bytes = add(add(encoder_live, add(metric, candidate)),
                                add(multiply(output_bound, 3), add(16 * mib, worker_scratch)));
  }
  // LCMS 校验只复制当前一个受上限约束的 ICC；主图和另一端顺序验证。
  // 按 probe 的实际 profile 峰值计费，无 ICC 不加固定大块，标量表由既有 scratch 覆盖。
  const auto icc_validation = dimensions.source_has_gain_map ? dimensions.source_icc_bytes : 0;
  estimate.decode_bytes = add(estimate.decode_bytes, icc_validation);
  estimate.prepare_bytes = add(estimate.prepare_bytes, icc_validation);
  estimate.bytes = std::max({estimate.decode_bytes, estimate.prepare_bytes,
      estimate.encode_bytes, estimate.metric_bytes, estimate.output_bytes});
  return estimate;
}

std::uint64_t estimated_regular_working_set_bytes(
    ImageDimensions dimensions, const AppConfig& cfg,
    std::uint64_t compressed_bytes = 0) noexcept {
  return estimated_regular_memory(dimensions, cfg, compressed_bytes).bytes;
}

void clear_unstarted_execution_metrics(EncodeResult& result) noexcept {
  result.encoder_threads = 0;
  result.gain_map_render_workers = result.gain_map_compute_workers = 0;
  result.heif_tile_parallelism = result.heif_codec_thread_budget = 0;
  result.base_decode_seconds = result.auxiliary_decode_seconds = result.gain_map_render_seconds =
      result.gain_map_compute_seconds = result.color_convert_seconds = 0.0;
  result.seconds = result.decode_seconds = result.prepare_seconds = result.encode_seconds =
      result.avif_rgb_to_yuv_seconds = result.avif_add_image_seconds = result.avif_finish_seconds =
      result.avif_output_copy_seconds = result.write_seconds = result.visual_quality_search_seconds =
      result.visual_quality_candidate_encode_seconds = result.visual_quality_candidate_decode_seconds =
      result.visual_quality_candidate_io_seconds = result.visual_quality_luma_seconds =
      result.gmsd_seconds = result.ms_ssim_seconds = result.visual_quality_metric_seconds = 0.0;
}

void record_memory_diagnostics(EncodeResult& result, const AppConfig& cfg,
                                MemoryStatus status, std::uint64_t effective_limit,
                                const ImageMemoryEstimate& estimate) {
  result.memory_budget_bytes = effective_limit;
  result.memory_budget_mode = memory_limit_mode(cfg);
  result.memory_budget_source = memory_limit_source(cfg, status);
  result.estimated_working_set_bytes = estimate.bytes;
  result.memory_estimate_profile = estimate.profile;
  result.memory_source_width = estimate.source_width;
  result.memory_source_height = estimate.source_height;
  result.memory_target_width = estimate.target_width;
  result.memory_target_height = estimate.target_height;
  result.memory_source_has_gain_map = estimate.source_has_gain_map;
}

// Backend results replace the pending result. Move the original admission
// snapshot (including its original system-memory source), never a new sample.
void inherit_memory_diagnostics(EncodeResult& result, EncodeResult& pending) noexcept {
  result.memory_budget_bytes = pending.memory_budget_bytes;
  result.memory_budget_mode = std::move(pending.memory_budget_mode);
  result.memory_budget_source = std::move(pending.memory_budget_source);
  result.estimated_working_set_bytes = pending.estimated_working_set_bytes;
  result.memory_estimate_profile = std::move(pending.memory_estimate_profile);
  result.memory_source_width = pending.memory_source_width;
  result.memory_source_height = pending.memory_source_height;
  result.memory_target_width = pending.memory_target_width;
  result.memory_target_height = pending.memory_target_height;
  result.memory_source_has_gain_map = pending.memory_source_has_gain_map;
}

std::expected<ClassifiedWork, std::string> classify_work_for_avif(
    const AppConfig& cfg, const std::vector<ImageFile>& files,
    std::uint64_t memory_limit) {
  try {
    WicFallbackComApartment com{cfg.allow_wic_fallback};
    const bool allow_wic_fallback = cfg.allow_wic_fallback && com.usable();

    ClassifiedWork classified{};
    if (cfg.output_format != OutputFormat::avif) {
      classified.ordinary.reserve(files.size());
      DecoderRegistryOptions decoder_options{.allow_wic_fallback =
                                                 allow_wic_fallback};
      for (const auto& image : files) {
        // Unknown decoded dimensions must not admit several compressed-small
        // images together. Reserve the whole budget until decode completes.
        ClassifiedImageFile candidate{
            .file = image,
            .estimated_bytes = std::max<std::uint64_t>(memory_limit,
                std::max<std::uintmax_t>(1, image.bytes))};
        if (auto dimensions =
                probe_image_dimensions_for_path(image.path, decoder_options)) {
          candidate.dimensions = *dimensions;
          candidate.estimated_bytes =
              estimated_regular_working_set_bytes(*dimensions, cfg, image.bytes);
        }
        classified.ordinary.push_back(std::move(candidate));
      }
      return classified;
    }

    const auto capabilities = avif_encoder_capabilities_for_current_build();
    const bool grid_available =
        std::ranges::any_of(capabilities, [](const auto& capability) {
          return capability.mode == AvifEncoderMode::aom &&
                 avif_capability_available_for_large_mode(capability) &&
                 capability.supports_avif_grid;
        });
    DecoderRegistryOptions decoder_options{.allow_wic_fallback =
                                               allow_wic_fallback};
    for (const auto& image : files) {
      auto dimensions =
          probe_image_dimensions_for_path(image.path, decoder_options);
      if (!dimensions) {
        classified.ordinary.push_back(ClassifiedImageFile{
            .file = image,
            .estimated_bytes = std::max<std::uint64_t>(memory_limit,
                std::max<std::uintmax_t>(1, image.bytes))});
        continue;
      }
      const auto target = target_dimensions_for_config(*dimensions, cfg);
      auto decision = classify_large_image(target, grid_available);
      if (decision.klass == LargeImageClass::large_mode_required && grid_available) {
        auto animated = input_uses_animation_reader(image.path);
        if (!animated) return std::unexpected{animated.error()};
        if (*animated) decision = classify_large_image(target, false);
      }
      switch (decision.klass) {
        case LargeImageClass::ordinary:
          classified.ordinary.push_back(ClassifiedImageFile{
              .file = image,
              .estimated_bytes =
                  estimated_regular_working_set_bytes(*dimensions, cfg, image.bytes),
              .dimensions = *dimensions});
          break;
        case LargeImageClass::large_mode_required:
          classified.large_mode.push_back(
              BatchLargeImageItem{.file = image,
                                  .dimensions = *dimensions,
                                  .decision = std::move(decision)});
          break;
      }
    }
    return classified;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"批处理分类列表内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"批处理分类列表数量超过运行时限制。"};
  }
}

// 并发规划用的「每图占用」估计。取中位数而不是最大值：一个队列里只要有一张
// 大图，用最大值会把 memory_limit / per_file 压到 1，整批退化成串行（实测并发
// 恒为 1）。中位数代表大多数文件的实际占用，让常规并发能起来；少数确实吃内存的
// 大图由运行时内存准入（reserve_work_memory）单独限制，不会同时挤进内存。
std::uint64_t typical_decoded_bytes_per_file(
    const std::vector<ClassifiedImageFile>& files) noexcept {
  if (files.empty()) {
    return 1;
  }
  std::vector<std::uint64_t> sorted;
  try {
    sorted.reserve(files.size());
    for (const auto& image : files) {
      sorted.push_back(image.estimated_bytes);
    }
    const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(
                                             sorted.size() / 2);
    std::nth_element(sorted.begin(), middle, sorted.end());
    return std::max<std::uint64_t>(1, *middle);
  } catch (...) {
    std::uint64_t largest = 1;
    for (const auto& file : files) largest = std::max(largest, file.estimated_bytes);
    return largest;
  }
}

std::uint64_t estimated_large_working_set_bytes(
    const BatchLargeImageItem& item, const AppConfig& cfg) noexcept;

std::expected<void, std::string> reject_over_memory_budget(
    ClassifiedWork& classified, std::uint64_t memory_limit, const AppConfig& cfg) {
  if (memory_limit == 0) {
    return {};
  }

  auto reject_from = [&](std::vector<ClassifiedImageFile>& source)
      -> std::expected<void, std::string> {
    try {
      if (classified.memory_rejected.size() >
          std::numeric_limits<std::size_t>::max() - source.size()) {
        return std::unexpected{"内存预算预检列表数量超过运行时限制。"};
      }
      classified.memory_rejected.reserve(classified.memory_rejected.size() +
                                         source.size());
      std::vector<ClassifiedImageFile> retained;
      retained.reserve(source.size());
      for (auto& image : source) {
        if (image.estimated_bytes > memory_limit) {
          classified.memory_rejected.push_back(std::move(image));
        } else {
          retained.push_back(std::move(image));
        }
      }
      source = std::move(retained);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"内存预算预检列表内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"内存预算预检列表数量超过运行时限制。"};
    }
    return {};
  };

  if (auto result = reject_from(classified.ordinary); !result) {
    return result;
  }
  try {
    std::vector<BatchLargeImageItem> retained;
    retained.reserve(classified.large_mode.size());
    for (auto& image : classified.large_mode) {
      const auto estimate = estimated_large_working_set_bytes(image, cfg);
      if (estimate > memory_limit)
        classified.memory_rejected.push_back({.file = std::move(image.file),
                                               .estimated_bytes = estimate,
                                               .dimensions = image.dimensions,
                                               .grid = true});
      else retained.push_back(std::move(image));
    }
    classified.large_mode = std::move(retained);
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"大图内存预算预检列表内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"大图内存预算预检列表数量超过运行时限制。"};
  }
}

std::expected<std::vector<WorkGroup>, std::string> build_work_groups(
    const AppConfig& cfg, const std::vector<ClassifiedImageFile>& classified) {
  std::vector<WorkGroup> groups;
  std::unordered_map<PathText, std::size_t> index_by_output;

  try {
    // 同一输出路径的文件必须串行处理，避免并发覆盖；不同输出路径按总大小分组调度。
    groups.reserve(classified.size());
    index_by_output.reserve(classified.size());
    for (const auto& entry : classified) {
      const auto& image = entry.file;
      const auto output = output_path_for(cfg, image);
      auto key = normalized_lower_path_key(output);
      const auto [it, inserted] = index_by_output.emplace(key, groups.size());
      if (inserted) {
        groups.push_back(WorkGroup{});
      }

      auto& group = groups[it->second];
      group.weight = saturated_add_uintmax(group.weight, image.bytes);
      // 组内串行，峰值取组内最大估算；供运行时内存准入使用。
      group.estimated_bytes =
          std::max(group.estimated_bytes, entry.estimated_bytes);
      group.files.push_back(image);
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected{"批处理工作组内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"批处理工作组数量超过运行时限制。"};
  }

  // 不同输出路径之间按总大小调度；覆盖/跳过模式下同一路径保留扫描顺序。
  std::ranges::sort(groups, [](const WorkGroup& left, const WorkGroup& right) {
    return left.weight > right.weight;
  });
  return groups;
}

std::uint64_t estimated_large_working_set_bytes(
    const BatchLargeImageItem& item, const AppConfig& cfg) noexcept {
  return estimated_regular_memory(item.dimensions, cfg, item.file.bytes, true).bytes;
}

std::uint64_t largest_large_mode_working_set(
    const std::vector<BatchLargeImageItem>& items, const AppConfig& cfg) noexcept {
  std::uint64_t estimate = 1;
  for (const auto& item : items) {
    estimate = std::max(estimate, estimated_large_working_set_bytes(item, cfg));
  }
  return estimate;
}

std::expected<std::vector<LargeWorkGroup>, std::string> build_large_work_groups(
    const AppConfig& cfg, const std::vector<BatchLargeImageItem>& items) {
  std::vector<LargeWorkGroup> groups;
  std::unordered_map<PathText, std::size_t> index_by_output;

  try {
    groups.reserve(items.size());
    index_by_output.reserve(items.size());
    for (const auto& item : items) {
      const auto output = output_path_for(cfg, item.file);
      auto key = normalized_lower_path_key(output);
      const auto [it, inserted] = index_by_output.emplace(key, groups.size());
      if (inserted) {
        groups.push_back(LargeWorkGroup{});
      }

      auto& group = groups[it->second];
      group.weight = saturated_add_uintmax(group.weight, item.file.bytes);
      group.estimated_bytes =
          std::max(group.estimated_bytes, estimated_large_working_set_bytes(item, cfg));
      group.items.push_back(item);
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected{"大图工作组内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"大图工作组数量超过运行时限制。"};
  }

  std::ranges::sort(groups, [](const LargeWorkGroup& left,
                               const LargeWorkGroup& right) {
    return left.weight > right.weight;
  });
  return groups;
}

std::string format_result_line(const EncodeResult& result) {
  const auto warning_suffix = [&] {
    return result.ok && !result.skipped && !result.message.empty() &&
                   result.message != "OK"
               ? std::format(" [WARN] {}", result.message)
               : std::string{};
  };
  if (result.ok) {
    if (result.skipped) {
      return std::format("[SKIP] {:04} {} -> 已存在", result.index + 1,
                         path_to_utf8(result.input_path.filename()));
    }

    const double ratio = result.original_bytes == 0
                             ? 0.0
                             : static_cast<double>(result.output_bytes) /
                                   static_cast<double>(result.original_bytes);
    if (result.requested_visual_quality) {
      if (result.lossless) {
        return std::format(
            "[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s, VQ {}→lossless, q{}, "
            "{} 次){}",
            result.index + 1, path_to_utf8(result.input_path.filename()),
            path_to_utf8(result.output_path.filename()),
            format_size(result.output_bytes), ratio * 100.0, result.seconds,
            *result.requested_visual_quality, result.final_encoder_quality,
            result.search_attempt_count, warning_suffix());
      }
      // 注意：Studio 会在 src/ui/main.cpp 里对子进程 stdout 嗅探 "未达标" 来给
      // 队列行打警告标记。这条文本属于跨进程约定，必须保持中文；worker 输出与
      // 日志不跟随界面语言。改动前先看那边的说明。
      const char* target_state =
          result.visual_quality_target_met ? "" : ", 未达标兜底";
      return std::format(
          "[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s, VQ {}→{:.2f}{}, q{}, "
          "{} 次){}",
          result.index + 1, path_to_utf8(result.input_path.filename()),
          path_to_utf8(result.output_path.filename()),
          format_size(result.output_bytes), ratio * 100.0, result.seconds,
          *result.requested_visual_quality, result.visual_score, target_state,
          result.final_encoder_quality, result.search_attempt_count,
          warning_suffix());
    }
    return std::format(
        "[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s){}", result.index + 1,
        path_to_utf8(result.input_path.filename()),
        path_to_utf8(result.output_path.filename()),
        format_size(result.output_bytes), ratio * 100.0, result.seconds,
        warning_suffix());
  }

  if (result.canceled) {
    return std::format("[CANCEL] {:04} {} -> {}", result.index + 1,
                       path_to_utf8(result.input_path.filename()),
                       result.message);
  }

  return std::format("[FAIL] {:04} {} -> {}", result.index + 1,
                     path_to_utf8(result.input_path.filename()),
                     result.message);
}

}  // namespace pipeline_detail

void emit_progress(const ProgressCallback& progress,
                   const BatchProgress& event);

namespace pipeline_detail {

struct WorkExecutionResult {
  int worker_failures{};
};

template <class Function>
void best_effort(Function&& fn) noexcept {
  try {
    fn();
  } catch (...) {
  }
}

void log_item_failure_noexcept(FileLogger& logger, const EncodeResult& result) noexcept {
  if (result.ok || !result.processed || result.canceled) return;
  best_effort([&] {
    if (!logger.enabled()) return;
    if (result.decode_stage.empty()) {
      logger.warn(format_result_line(result));
      return;
    }
    logger.warn(std::format(
        "{} [decoder={}, decode_stage={}, source_metadata_available={}, source_icc={}]",
        format_result_line(result), result.decoder_id.empty() ? "unknown" : result.decoder_id,
        result.decode_stage, result.source_metadata_available,
        result.source_metadata_available ? (result.source_has_icc ? "true" : "false") : "unknown"));
  });
}

void emit_item_started_noexcept(
    const AppConfig& cfg, const ImageFile& image, std::size_t completed,
    std::size_t total, const ProgressCallback& progress) noexcept {
  best_effort([&] {
    emit_progress(
        progress,
        BatchProgress{
            .kind = BatchEventKind::item_started,
            .completed = completed,
            .total = total,
            .result = EncodeResult{.index = image.index,
                                   .input_path = image.path,
                                   .output_path = output_path_for(cfg, image),
                                   .original_bytes = image.bytes,
                                   .message = "正在转码"}});
  });
}

void report_worker_warning_noexcept(
    FileLogger& logger, const ProgressCallback& progress, std::size_t completed,
    std::size_t total, std::string_view log_message,
    std::string_view progress_message) noexcept {
  try {
    logger.error(log_message);
  } catch (...) {
  }
  try {
    emit_progress(progress,
                  BatchProgress{.kind = BatchEventKind::warning,
                                .completed = completed,
                                .total = total,
                                .text = std::string{progress_message}});
  } catch (...) {
  }
}

void report_worker_exception_noexcept(FileLogger& logger,
                                      const ProgressCallback& progress,
                                      std::size_t completed,
                                      std::size_t total) noexcept {
  report_worker_warning_noexcept(logger, progress, completed, total,
                                 "worker failed",
                                 "[WARN] 工作线程异常，已停止该线程。");
}


struct MemoryAdmissionGuard {
  WorkMemoryAdmission* admission{};
  std::uint64_t amount{};

  MemoryAdmissionGuard() = default;
  MemoryAdmissionGuard(WorkMemoryAdmission& owner, std::uint64_t reserved) noexcept
      : admission(&owner), amount(reserved) {}
  ~MemoryAdmissionGuard() {
    if (admission != nullptr) {
      admission->release(amount);
    }
  }
  MemoryAdmissionGuard(const MemoryAdmissionGuard&) = delete;
  MemoryAdmissionGuard& operator=(const MemoryAdmissionGuard&) = delete;
};

WorkExecutionResult encode_work_groups(
    const AppConfig& cfg, FileLogger& logger, const ResourcePlan& resource_plan,
    const std::vector<WorkGroup>& work, std::size_t progress_total,
    std::atomic<std::size_t>& completed, std::vector<EncodeResult>& results,
    const ProgressCallback& progress, std::stop_token stop_token) {
  WorkExecutionResult execution{};
  if (work.empty()) {
    return execution;
  }

  const int jobs =
      std::max(1, std::min({resource_plan.file_parallelism,
                            resource_plan.memory_file_parallelism,
                            count_to_int_saturated(work.size())}));
  std::atomic<int> worker_failures{0};
  WorkMemoryAdmission admission{resource_plan.memory_limit_bytes, work};

  std::vector<std::jthread> workers;
  try {
    workers.reserve(static_cast<std::size_t>(jobs));
  } catch (const std::bad_alloc&) {
    worker_failures.fetch_add(1);
    try {
      const auto text =
          std::format("[WARN] 工作线程列表内存不足，需要 {} 个线程槽。", jobs);
      report_worker_warning_noexcept(logger, progress, completed.load(),
                                     progress_total, text, text);
    } catch (...) {
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "worker list allocation failed", "[WARN] 工作线程列表内存不足。");
    }
    execution.worker_failures = worker_failures.load();
    return execution;
  } catch (const std::length_error&) {
    worker_failures.fetch_add(1);
    constexpr std::string_view text = "[WARN] 工作线程列表数量超过运行时限制。";
    report_worker_warning_noexcept(logger, progress, completed.load(),
                                   progress_total, text, text);
    execution.worker_failures = worker_failures.load();
    return execution;
  }
  for (const int worker_index : std::views::iota(0, jobs)) {
    try {
      workers.emplace_back([&] {
        WicFallbackComApartment com{cfg.allow_wic_fallback};
        try {
          std::optional<AppConfig> worker_cfg;
          if (cfg.allow_wic_fallback && !com.usable()) {
            worker_cfg.emplace(cfg);
            worker_cfg->allow_wic_fallback = false;
            try {
              const auto text = std::format(
                  "[WARN] 工作线程 WIC fallback COM 初始化失败，已禁用该线程的 "
                  "WIC fallback: 0x{:08X}",
                  static_cast<unsigned int>(com.init()));
              report_worker_warning_noexcept(logger, progress, completed.load(),
                                             progress_total, text, text);
            } catch (...) {
              report_worker_warning_noexcept(
                  logger, progress, completed.load(), progress_total,
                  "worker WIC fallback COM initialization failed",
                  "[WARN] 工作线程 WIC fallback COM 初始化失败，已禁用该线程的 "
                  "WIC fallback。");
            }
          }
          const auto& effective_cfg = worker_cfg ? *worker_cfg : cfg;
          NativeBackend native_backend{effective_cfg, logger, resource_plan};
          while (true) {
            if (stop_token.stop_requested()) {
              break;
            }

            const auto ticket = admission.acquire(stop_token);
            if (!ticket) break;
            const auto& group = work[ticket->index];
            MemoryAdmissionGuard reservation{admission, ticket->bytes};
            if (stop_token.stop_requested()) {
              break;
            }
            if (group.files.size() > 1 &&
                cfg.collision_mode == CollisionMode::overwrite) {
              best_effort([&] {
                emit_progress(
                    progress,
                    BatchProgress{.kind = BatchEventKind::warning,
                                  .completed = completed.load(),
                                  .total = progress_total,
                                  .text = std::format(
                                      "[WARN] 输出重名: {} 个输入将依次覆盖 {}",
                                      group.files.size(),
                                      display_path_for_user(output_path_for(
                                          cfg, group.files.back())))});
              });
            }
            for (const auto& image : group.files) {
              if (stop_token.stop_requested()) {
                break;
              }
              emit_item_started_noexcept(cfg, image, completed.load(),
                                         progress_total, progress);
              auto result = native_backend.encode(image, stop_token);
              const auto result_index = result.index;
              inherit_memory_diagnostics(result, results[result_index]);
              results[result_index] = std::move(result);
              log_item_failure_noexcept(logger, results[result_index]);
              const auto done = completed.fetch_add(1) + 1;
              try {
                BatchProgress event{
                    .kind = BatchEventKind::item_finished,
                    .completed = done,
                    .total = progress_total,
                    .result = results[result_index],
                    .text = format_result_line(results[result_index])};
                emit_progress(progress, event);
              } catch (...) {
                report_worker_warning_noexcept(
                    logger, progress, done, progress_total,
                    "item progress reporting failed",
                    "[WARN] 单项进度报告生成失败，结果已记录。");
              }
            }
          }
        } catch (const std::exception&) {
          worker_failures.fetch_add(1);
          report_worker_exception_noexcept(logger, progress, completed.load(),
                                           progress_total);
        } catch (...) {
          worker_failures.fetch_add(1);
          report_worker_warning_noexcept(
              logger, progress, completed.load(), progress_total,
              "worker failed: unknown exception",
              "[WARN] 工作线程异常，已停止该线程: 未知异常");
        }
      });
    } catch (const std::bad_alloc&) {
      worker_failures.fetch_add(1);
      try {
        const auto text = std::format(
            "[WARN] 创建工作线程失败，线程状态内存不足，已继续等待已启动线程: "
            "{}/{}",
            worker_index, jobs);
        report_worker_warning_noexcept(logger, progress, completed.load(),
                                       progress_total, text, text);
      } catch (...) {
        report_worker_warning_noexcept(
            logger, progress, completed.load(), progress_total,
            "worker thread creation allocation failed",
            "[WARN] "
            "创建工作线程失败，线程状态内存不足，已继续等待已启动线程。");
      }
      break;
    } catch (const std::system_error&) {
      worker_failures.fetch_add(1);
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "worker thread creation failed",
          "[WARN] 创建工作线程失败，已继续等待已启动线程。");
      break;
    } catch (const std::exception&) {
      worker_failures.fetch_add(1);
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "worker thread creation failed",
          "[WARN] 创建工作线程失败，已继续等待已启动线程。");
      break;
    }
  }
  workers.clear();
  execution.worker_failures = worker_failures.load();
  return execution;
}

EncodeResult encode_large_mode_item(const AppConfig& cfg, FileLogger& logger,
                                    const BatchLargeImageItem& item,
                                    ResourcePlan resource_plan,
                                    std::stop_token stop_token) {
  if (cfg.unlock_memory_limit) resource_plan.memory_limit_bytes = 0;
  const auto estimate = estimated_regular_memory(item.dimensions, cfg, item.file.bytes, true);
  EncodeResult failed{
      .index = item.file.index,
      .input_path = item.file.path,
      .output_path = output_path_for(cfg, item.file),
      .original_bytes = item.file.bytes,
      .quality = cfg.quality,
      .requested_visual_quality = cfg.visual_quality,
      .final_encoder_quality = cfg.quality,
      .speed = cfg.speed.value_or(default_speed_for(cfg.output_format)),
      .processed = true};
  clear_unstarted_execution_metrics(failed);
  record_memory_diagnostics(failed, cfg, {}, resource_plan.memory_limit_bytes, estimate);
  if (stop_token.stop_requested()) {
    failed.canceled = true;
    failed.message = "任务已取消。";
    return failed;
  }

  const auto required_memory = estimate.bytes;
  if (resource_plan.memory_limit_bytes > 0 && required_memory > resource_plan.memory_limit_bytes) {
    failed.message = std::format("AOM Grid 预估工作集 {} 超过内存预算 {}。",
        format_size(required_memory), format_size(resource_plan.memory_limit_bytes));
    return failed;
  }
  AppConfig large_cfg = cfg;
  large_cfg.output_format = OutputFormat::avif;
  large_cfg.input_path = item.file.path;
  large_cfg.visual_quality.reset();

  try {
    if (!cfg.studio_large_action.empty() && cfg.studio_large_action != L"grid") {
      failed.message = "大图编码仅支持 AOM Grid。";
      return failed;
    }
    if (!item.decision.available_grid) {
      failed.message = "当前构建没有可用的 AOM Grid 编码器。";
      return failed;
    }
    const auto target = target_dimensions_for_config(item.dimensions, cfg);
    auto planned = plan_grid(GridPlanRequest{
        .width = target.width,
        .height = target.height,
        .mode = GridMode::auto_grid,
        .clamped_padding_enabled = large_cfg.experimental_clamped_grid_padding});
    if (!planned) {
      failed.message = std::format("Grid 规划失败：{}", planned.error());
      return failed;
    }
    large_cfg.avif_encoder = AvifEncoderMode::aom;
    NativeBackend backend{large_cfg, logger, resource_plan};
    auto result = backend.encode_avif_grid(item.file, *planned, stop_token);
    inherit_memory_diagnostics(result, failed);
    return result;
  } catch (const std::exception&) {
    failed.message = "自动大图工作线程异常。";
    return failed;
  } catch (...) {
    failed.message = "自动大图工作线程异常：未知异常。";
    return failed;
  }

  failed.message = "没有可用的大图自动处理方式。";
  return failed;
}

WorkExecutionResult encode_large_work_groups(
    const AppConfig& cfg, FileLogger& logger, const ResourcePlan& resource_plan,
    const std::vector<LargeWorkGroup>& work, std::size_t progress_total,
    std::atomic<std::size_t>& completed, std::vector<EncodeResult>& results,
    const ProgressCallback& progress, std::stop_token stop_token) {
  WorkExecutionResult execution{};
  if (work.empty()) {
    return execution;
  }

  const int jobs =
      std::max(1, std::min({resource_plan.file_parallelism,
                            resource_plan.memory_file_parallelism,
                            count_to_int_saturated(work.size())}));
  std::atomic<int> worker_failures{0};
  WorkMemoryAdmission admission{resource_plan.memory_limit_bytes, work};

  std::vector<std::jthread> workers;
  try {
    workers.reserve(static_cast<std::size_t>(jobs));
  } catch (const std::bad_alloc&) {
    worker_failures.fetch_add(1);
    try {
      const auto text =
          std::format("[WARN] 大图工作线程列表内存不足，需要 {} 个线程槽。", jobs);
      report_worker_warning_noexcept(logger, progress, completed.load(),
                                     progress_total, text, text);
    } catch (...) {
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "large worker list allocation failed",
          "[WARN] 大图工作线程列表内存不足。");
    }
    execution.worker_failures = worker_failures.load();
    return execution;
  } catch (const std::length_error&) {
    worker_failures.fetch_add(1);
    constexpr std::string_view text = "[WARN] 大图工作线程列表数量超过运行时限制。";
    report_worker_warning_noexcept(logger, progress, completed.load(),
                                   progress_total, text, text);
    execution.worker_failures = worker_failures.load();
    return execution;
  }

  for (const int worker_index : std::views::iota(0, jobs)) {
    try {
      workers.emplace_back([&] {
        WicFallbackComApartment com{cfg.allow_wic_fallback};
        try {
          std::optional<AppConfig> worker_cfg;
          if (cfg.allow_wic_fallback && !com.usable()) {
            worker_cfg.emplace(cfg);
            worker_cfg->allow_wic_fallback = false;
            try {
              const auto text = std::format(
                  "[WARN] 大图工作线程 WIC fallback COM 初始化失败，已禁用该线程的 "
                  "WIC fallback: 0x{:08X}",
                  static_cast<unsigned int>(com.init()));
              report_worker_warning_noexcept(logger, progress, completed.load(),
                                             progress_total, text, text);
            } catch (...) {
              report_worker_warning_noexcept(
                  logger, progress, completed.load(), progress_total,
                  "large worker WIC fallback COM initialization failed",
                  "[WARN] 大图工作线程 WIC fallback COM 初始化失败，已禁用该线程的 "
                  "WIC fallback。");
            }
          }
          const auto& effective_cfg = worker_cfg ? *worker_cfg : cfg;
          while (true) {
            if (stop_token.stop_requested()) {
              break;
            }

            const auto ticket = admission.acquire(stop_token);
            if (!ticket) break;
            const auto& group = work[ticket->index];
            MemoryAdmissionGuard reservation{admission, ticket->bytes};
            if (stop_token.stop_requested()) {
              break;
            }
            if (group.items.size() > 1 &&
                cfg.collision_mode == CollisionMode::overwrite) {
              best_effort([&] {
                emit_progress(
                    progress,
                    BatchProgress{.kind = BatchEventKind::warning,
                                  .completed = completed.load(),
                                  .total = progress_total,
                                  .text = std::format(
                                      "[WARN] 大图输出重名: {} 个输入将依次覆盖 {}",
                                      group.items.size(),
                                      display_path_for_user(output_path_for(
                                          cfg, group.items.back().file)))});
              });
            }

            for (const auto& item : group.items) {
              if (stop_token.stop_requested()) {
                break;
              }
              emit_item_started_noexcept(cfg, item.file, completed.load(),
                                         progress_total, progress);
              best_effort([&] {
                logger.info(large_image_log_text(item));
                emit_progress(
                    progress,
                    BatchProgress{.kind = BatchEventKind::message,
                                  .completed = completed.load(),
                                  .total = progress_total,
                                  .large_image = item,
                                  .text = large_image_action_text(item)});
              });
              auto result = encode_large_mode_item(
                  effective_cfg, logger, item, resource_plan, stop_token);
              const auto result_index = result.index;
              inherit_memory_diagnostics(result, results[result_index]);
              results[result_index] = std::move(result);
              log_item_failure_noexcept(logger, results[result_index]);
              const auto done = completed.fetch_add(1) + 1;
              try {
                BatchProgress event{
                    .kind = BatchEventKind::item_finished,
                    .completed = done,
                    .total = progress_total,
                    .result = results[result_index],
                    .text = format_result_line(results[result_index])};
                emit_progress(progress, event);
              } catch (...) {
                report_worker_warning_noexcept(
                    logger, progress, done, progress_total,
                    "large item progress reporting failed",
                    "[WARN] 大图单项进度报告生成失败，结果已记录。");
              }
            }
          }
        } catch (const std::exception&) {
          worker_failures.fetch_add(1);
          report_worker_warning_noexcept(
              logger, progress, completed.load(), progress_total,
              "large worker failed",
              "[WARN] 大图工作线程异常，已停止该线程。");
        } catch (...) {
          worker_failures.fetch_add(1);
          report_worker_warning_noexcept(
              logger, progress, completed.load(), progress_total,
              "large worker failed: unknown exception",
              "[WARN] 大图工作线程异常，已停止该线程: 未知异常");
        }
      });
    } catch (const std::bad_alloc&) {
      worker_failures.fetch_add(1);
      try {
        const auto text = std::format(
            "[WARN] 创建大图工作线程失败，线程状态内存不足，已继续等待已启动线程: "
            "{}/{}",
            worker_index, jobs);
        report_worker_warning_noexcept(logger, progress, completed.load(),
                                       progress_total, text, text);
      } catch (...) {
        report_worker_warning_noexcept(
            logger, progress, completed.load(), progress_total,
            "large worker thread creation allocation failed",
            "[WARN] 创建大图工作线程失败，线程状态内存不足，已继续等待已启动线程。");
      }
      break;
    } catch (const std::system_error&) {
      worker_failures.fetch_add(1);
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "large worker thread creation failed",
          "[WARN] 创建大图工作线程失败，已继续等待已启动线程。");
      break;
    } catch (const std::exception&) {
      worker_failures.fetch_add(1);
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "large worker thread creation failed",
          "[WARN] 创建大图工作线程失败，已继续等待已启动线程。");
      break;
    }
  }

  workers.clear();
  execution.worker_failures = worker_failures.load();
  return execution;
}

void set_result_message_noexcept(EncodeResult& result,
                                 std::string_view message) noexcept {
  try {
    result.message = message;
  } catch (...) {
  }
}

void mark_unstarted_results(std::vector<EncodeResult>& results, bool canceled,
                            bool worker_failed) noexcept {
  for (auto& result : results) {
    if (result.large_image_queued || result.processed || result.ok ||
        result.canceled) {
      continue;
    }
    if (canceled) {
      result.canceled = true;
      set_result_message_noexcept(result, "任务已取消。");
    } else if (worker_failed) {
      result.processed = true;
      set_result_message_noexcept(result, "任务未启动：工作线程提前停止。");
    }
  }
}

}  // namespace pipeline_detail

void print_line(std::string_view text) {
  std::println("{}", text);
  std::fflush(stdout);
}

void emit_progress(const ProgressCallback& progress,
                   const BatchProgress& event) {
  try {
    if (progress) {
      progress(event);
    }
  } catch (...) {
    // 进度回调不应该影响编码任务；UI 层异常会被吞掉，批处理继续写 CSV。
  }
}

MemoryStatus current_memory_status() noexcept {
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) {
    return {};
  }
  return MemoryStatus{.total_bytes = status.ullTotalPhys,
                      .available_bytes = status.ullAvailPhys};
#else
  struct sysinfo status {};
  if (sysinfo(&status) != 0) {
    return {};
  }
  const auto unit = static_cast<std::uint64_t>(status.mem_unit == 0 ? 1 : status.mem_unit);
  return MemoryStatus{.total_bytes = static_cast<std::uint64_t>(status.totalram) * unit,
                      .available_bytes = static_cast<std::uint64_t>(status.freeram + status.bufferram) * unit};
#endif
}

void apply_runtime_input_limit_policy(const AppConfig& cfg) noexcept {
  encoding_defaults::unlock_max_input_file_bytes.store(
      cfg.unlock_max_input_file_bytes, std::memory_order_relaxed);
}

std::expected<BatchSummary, std::string> run_batch(
    AppConfig cfg, ProgressCallback progress = {},
    std::stop_token stop_token = {},
    std::span<const std::filesystem::path> input_paths = {}) {
  try {
    apply_runtime_input_limit_policy(cfg);
#ifndef _WIN32
    cfg.allow_wic_fallback = false;
#endif
    if (!cfg.studio_queue_manifest.empty() &&
        !cfg.studio_large_action.empty()) {
      return std::unexpected{
          "Studio 队列 manifest 不能与手动大图 worker 同时使用。"};
    }
    if (!cfg.studio_large_action.empty()) {
      if (auto valid = validate_config(cfg); !valid) {
        return std::unexpected{valid.error()};
      }
      if (cfg.output_format != OutputFormat::avif) {
        return std::unexpected{"Studio 大图 worker 仅支持 AVIF 输出。"};
      }
      auto dimensions = probe_image_dimensions_for_path(
          cfg.input_path,
          DecoderRegistryOptions{.allow_wic_fallback = cfg.allow_wic_fallback});
      if (!dimensions) {
        return std::unexpected{dimensions.error()};
      }
      std::error_code file_ec;
      const auto bytes = std::filesystem::file_size(cfg.input_path, file_ec);
      if (file_ec) {
        return std::unexpected{std::format("读取文件大小失败: {}；系统错误：{}。",
                                           display_path_for_user(cfg.input_path),
                                           file_ec.message())};
      }
      const bool grid_action = cfg.studio_large_action == L"grid";
      if (grid_action) {
        auto animated = input_uses_animation_reader(cfg.input_path);
        if (!animated) return std::unexpected{animated.error()};
        if (*animated) return std::unexpected{"保留动画时不能使用静态 Grid。"};
      }
      auto decision = classify_large_image(*dimensions, grid_action);
      decision.klass = LargeImageClass::large_mode_required;
      if (decision.reason == LargeImageReason::none) {
        decision.reason_text = "Studio 手动大图 worker。";
      }
      auto item = BatchLargeImageItem{
          .file = ImageFile{.index = 0, .path = cfg.input_path, .bytes = bytes},
          .dimensions = *dimensions,
          .decision = std::move(decision)};
      const auto output_dir = output_dir_for(cfg);
      FileLogger logger{output_dir, cfg.write_log};
      const auto memory_status = current_memory_status();
      const auto configured_memory_limit = effective_memory_limit(cfg, memory_status);
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::message,
                                    .completed = 0,
                                    .total = 1,
                                    .large_image = item,
                                    .text = pipeline_detail::large_image_action_text(item)});
      });
      const auto estimate = pipeline_detail::estimated_regular_memory(
          item.dimensions, cfg, item.file.bytes, true);
      const auto large_resource_plan = plan_large_mode_resources(
          plan_resources(ResourcePlanRequest{
              .automatic_thread_budget = cfg.max_jobs,
              .file_count = 1,
              .memory_limit_bytes = configured_memory_limit,
              .estimated_bytes_per_file = estimate.bytes}),
          1, estimate.bytes);
      pipeline_detail::emit_item_started_noexcept(
          cfg, item.file, 0, 1, progress);
      auto result = pipeline_detail::encode_large_mode_item(
          cfg, logger, item, large_resource_plan, stop_token);
      pipeline_detail::record_memory_diagnostics(
          result, cfg, memory_status, configured_memory_limit, estimate);
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::item_finished,
                                    .completed = 1,
                                    .total = 1,
                                    .result = result,
                                    .text = pipeline_detail::format_result_line(result)});
      });
      const bool canceled = stop_token.stop_requested() || result.canceled;
      const bool ok = result.ok;
      const BatchSummary summary{.ok_count = ok ? 1 : 0,
                                 .failed_count = !ok && !canceled ? 1 : 0,
                                 .canceled_count = canceled ? 1 : 0,
                                 .large_image_queued_count = 0,
                                 .original_total = result.original_bytes,
                                 .output_total = ok ? result.output_bytes : 0,
                                 .canceled = canceled,
                                 .exit_code = canceled ? 130 : (ok ? 0 : 2)};
      if (cfg.write_summary) {
        if (auto csv = write_csv(output_dir, std::span<const EncodeResult>{&result, 1}); !csv) {
          return std::unexpected{csv.error()};
        }
      }
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::summary,
                                    .completed = 1,
                                    .total = 1,
                                    .summary = summary,
                                    .text = std::format("{}：成功 {}，失败 {}，取消 {}。",
                                                        canceled ? "已取消" : "完成",
                                                        summary.ok_count,
                                                        summary.failed_count,
                                                        summary.canceled_count)});
      });
      return summary;
    }
    if (auto valid = validate_config(cfg); !valid) {
      return std::unexpected{valid.error()};
    }

    const auto output_dir = output_dir_for(cfg);

    std::vector<ImageFile> files;
    if (!cfg.studio_queue_manifest.empty()) {
      auto manifest = read_studio_queue_manifest(cfg.studio_queue_manifest);
      if (!manifest) {
        return std::unexpected{manifest.error()};
      }
      if (auto valid = pipeline_detail::validate_manifest_paths_for_config(
              cfg, manifest->files);
          !valid) {
        return std::unexpected{valid.error()};
      }
      files = std::move(manifest->files);
    } else {
      if (auto scanned = input_paths.empty() ? scan_images(cfg, files)
                                             : scan_images(cfg, input_paths, files);
          !scanned) {
        return std::unexpected{scanned.error()};
      }
    }

    FileLogger logger{output_dir, cfg.write_log};
    logger.info("native backend enabled");
    if (cfg.write_log && !logger.enabled()) {
      pipeline_detail::best_effort([&] {
        const auto text =
            std::format("[WARN] 日志写入失败: {}", logger.last_error());
        emit_progress(progress, BatchProgress{.kind = BatchEventKind::warning,
                                              .text = text});
      });
    }
    if (files.empty()) {
      pipeline_detail::best_effort([&] {
        emit_progress(
            progress,
            BatchProgress{
                .kind = BatchEventKind::message,
                .text = "未找到图片。支持: "
                        "jpg, jpeg, jpe, jfif, png, webp, bmp, dib, rle, "
                        "tif, tiff, gif, jxl, avif, awsraw, dng, cr2, cr3, "
                        "nef, arw, rw2, orf, raf, pef, srw, x3f, 3fr, erf, "
                        "kdc, mrw, raw, heic, heif, jxr, wdp, hdp。"
                        "请确认输入目录中有支持格式的文件。"});
      });
      return BatchSummary{.exit_code = 0};
    }

    const auto disambiguated_count =
        std::ranges::count_if(files, &ImageFile::extension_disambiguated);
    if (disambiguated_count > 0) {
      pipeline_detail::best_effort([&] {
        const auto text = std::format(
            "[WARN] 同名不同扩展: {} 个输入会自动保留源扩展名，"
            "例如 1.jpg.avif / 1.bmp.avif",
            disambiguated_count);
        logger.warn(text);
        emit_progress(progress, BatchProgress{.kind = BatchEventKind::warning,
                                              .total = files.size(),
                                              .text = text});
      });
    }

    const auto memory_status = current_memory_status();
    const auto configured_memory_limit = effective_memory_limit(cfg, memory_status);
    auto classified = pipeline_detail::classify_work_for_avif(cfg, files, configured_memory_limit);
    if (!classified) {
      return std::unexpected{classified.error()};
    }
    if (auto memory_filter = pipeline_detail::reject_over_memory_budget(
            *classified, configured_memory_limit, cfg);
        !memory_filter) {
      return std::unexpected{memory_filter.error()};
    }
    auto ordinary_work =
        pipeline_detail::build_work_groups(cfg, classified->ordinary);
    if (!ordinary_work) {
      return std::unexpected{ordinary_work.error()};
    }
    auto large_work =
        pipeline_detail::build_large_work_groups(cfg, classified->large_mode);
    if (!large_work) {
      return std::unexpected{large_work.error()};
    }
    // 并发度按「典型」单图占用规划（中位数），而不是队列最大值：最大值会让
    // 一张大图把整批压成串行。真正吃内存的组由 encode_work_groups 的运行时
    // 内存准入单独限制，不会同时进入。
    const auto ordinary_estimated_bytes_per_file =
        pipeline_detail::typical_decoded_bytes_per_file(classified->ordinary);
    const auto resource_plan = plan_resources(ResourcePlanRequest{
        .automatic_thread_budget = cfg.max_jobs,
        .file_count = pipeline_detail::count_to_int_saturated(
            std::max<std::size_t>(1, ordinary_work->size())),
        .memory_limit_bytes = configured_memory_limit,
        .estimated_bytes_per_file = ordinary_estimated_bytes_per_file});
    const auto large_largest_working_set =
        pipeline_detail::largest_large_mode_working_set(classified->large_mode, cfg);
    const auto large_base_resource_plan = plan_resources(ResourcePlanRequest{
        .automatic_thread_budget = cfg.max_jobs,
        .file_count = pipeline_detail::count_to_int_saturated(
            std::max<std::size_t>(1, large_work->size())),
        .memory_limit_bytes = configured_memory_limit,
        .estimated_bytes_per_file = large_largest_working_set});
    const auto large_resource_plan = plan_large_mode_resources(
        large_base_resource_plan,
        pipeline_detail::count_to_int_saturated(std::max<std::size_t>(
            1, large_work->size())),
        large_largest_working_set);
    const int ordinary_jobs =
        ordinary_work->empty()
            ? 0
            : std::max(1, std::min({resource_plan.file_parallelism,
                                    resource_plan.memory_file_parallelism,
                                    pipeline_detail::count_to_int_saturated(
                                        ordinary_work->size())}));
    const int large_jobs =
        large_work->empty()
            ? 0
            : std::max(1, std::min({large_resource_plan.file_parallelism,
                                    large_resource_plan.memory_file_parallelism,
                                    pipeline_detail::count_to_int_saturated(
                                        large_work->size())}));
    pipeline_detail::best_effort([&] {
      emit_progress(
          progress,
          BatchProgress{
              .kind = BatchEventKind::message,
              .total = files.size(),
              .text = std::format(
                  "共 {} 个文件；普通 {}，内存超限 {}，Grid "
                  "{}。普通并发 {}，Grid 并发 {}，编码器线程/文件 "
                  "{}/{}，内存限制 {}。",
                  files.size(), classified->ordinary.size(),
                  classified->memory_rejected.size(),
                  classified->large_mode.size(), ordinary_jobs,
                  large_jobs,
                  resource_plan.encoder_threads_per_file,
                  large_resource_plan.encoder_threads_per_file,
                  resource_plan.memory_limit_bytes == 0
                      ? std::string{"未限制"}
                      : format_size(resource_plan.memory_limit_bytes))});
    });

    std::vector<EncodeResult> results;
    try {
      results.resize(files.size());
    } catch (const std::bad_alloc&) {
      return std::unexpected{"批处理结果列表内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"批处理结果列表数量超过运行时限制。"};
    }
    for (const auto& image : files) {
      results[image.index] =
          EncodeResult{.index = image.index,
                       .input_path = image.path,
                       .output_path = output_path_for(cfg, image),
                       .original_bytes = image.bytes,
                       .quality = cfg.quality,
                       .requested_visual_quality = cfg.visual_quality,
                       .gmsd_weight = GMSD_WEIGHT,
                       .msssim_weight = MSSSIM_WEIGHT,
                       .final_encoder_quality = cfg.quality,
                       .speed = cfg.speed.value_or(-1),
                       .quality_overridden_by_visual_quality =
                           cfg.visual_quality.has_value(),
                       .message = "未处理。"};
      pipeline_detail::clear_unstarted_execution_metrics(results[image.index]);
    }
    const auto record_classified = [&](const pipeline_detail::ClassifiedImageFile& image) {
      auto estimate = image.dimensions.width && image.dimensions.height
          ? pipeline_detail::estimated_regular_memory(image.dimensions, cfg, image.file.bytes, image.grid)
          : pipeline_detail::ImageMemoryEstimate{};
      estimate.bytes = image.estimated_bytes;
      pipeline_detail::record_memory_diagnostics(
          results[image.file.index], cfg, memory_status, configured_memory_limit, estimate);
    };
    for (const auto& image : classified->ordinary) record_classified(image);
    for (const auto& image : classified->memory_rejected) record_classified(image);

    for (const auto& item : classified->large_mode) {
      auto& result = results[item.file.index];
      pipeline_detail::record_memory_diagnostics(result, cfg, memory_status, configured_memory_limit,
          pipeline_detail::estimated_regular_memory(item.dimensions, cfg, item.file.bytes, true));
      result.processed = false;
      result.message = pipeline_detail::large_image_report_message(item);
    }

    for (const auto& image : classified->memory_rejected) {
      auto& result = results[image.file.index];
      result.processed = true;
      result.message =
          std::format("估算工作集 {} 超过内存限制 {}，未启动编码。",
                      format_size(image.estimated_bytes),
                      format_size(configured_memory_limit));
    }

    std::atomic<std::size_t> completed{0};
    int worker_failures = 0;

    if (!stop_token.stop_requested()) {
      for (const auto& image : classified->memory_rejected) {
        const auto done = completed.fetch_add(1) + 1;
        pipeline_detail::best_effort([&] {
          const auto& result = results[image.file.index];
          logger.warn(
              std::format("[FAIL] {:04} {}", result.index + 1, result.message));
          emit_progress(
              progress,
              BatchProgress{
                  .kind = BatchEventKind::item_finished,
                  .completed = done,
                  .total = files.size(),
                  .result = result,
                  .text = pipeline_detail::format_result_line(result)});
        });
      }
    }

    const auto ordinary_execution = pipeline_detail::encode_work_groups(
        cfg, logger, resource_plan, *ordinary_work, files.size(), completed,
        results, progress, stop_token);
    worker_failures += ordinary_execution.worker_failures;

    if (!stop_token.stop_requested() && !classified->large_mode.empty()) {
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::message,
                                    .completed = completed.load(),
                                    .total = files.size(),
                                    .text = std::format(
                                        "开始自动处理超大图：{} 个文件，并发 "
                                        "{}，编码器线程/文件 {}，使用 AOM Grid。",
                                        classified->large_mode.size(),
                                        large_jobs,
                                        large_resource_plan
                                            .encoder_threads_per_file)});
      });
      const auto large_execution = pipeline_detail::encode_large_work_groups(
          cfg, logger, large_resource_plan, *large_work, files.size(),
          completed, results, progress, stop_token);
      worker_failures += large_execution.worker_failures;
    }

    const bool canceled = stop_token.stop_requested();
    pipeline_detail::mark_unstarted_results(results, canceled,
                                            worker_failures > 0);

    std::uintmax_t original_total = 0;
    std::unordered_map<PathText, std::uintmax_t> final_output_sizes;
    bool original_total_incomplete = false;
    bool output_total_incomplete = false;
    std::size_t ok_count = 0;
    std::size_t failed_count = 0;
    std::size_t canceled_count = 0;
    for (const auto& result : results) {
      if (result.ok) {
        ++ok_count;
        if (!original_total_incomplete &&
            !pipeline_detail::checked_add_uintmax(original_total,
                                                  result.original_bytes)) {
          original_total = 0;
          original_total_incomplete = true;
        }
        if (!output_total_incomplete) {
          try {
            final_output_sizes[normalized_lower_path_key(result.output_path)] =
                result.output_bytes;
          } catch (const std::bad_alloc&) {
            output_total_incomplete = true;
          } catch (const std::length_error&) {
            output_total_incomplete = true;
          }
        }
      } else if (result.canceled || (!result.processed && canceled)) {
        ++canceled_count;
      } else {
        ++failed_count;
        if (result.processed && !original_total_incomplete &&
            !pipeline_detail::checked_add_uintmax(original_total,
                                                  result.original_bytes)) {
          original_total = 0;
          original_total_incomplete = true;
        }
      }
    }
    std::uintmax_t output_total = 0;
    if (!output_total_incomplete) {
      for (const auto& [_, bytes] : final_output_sizes) {
        if (!pipeline_detail::checked_add_uintmax(output_total, bytes)) {
          output_total = 0;
          output_total_incomplete = true;
          break;
        }
      }
    }

    bool summary_failed = original_total_incomplete || output_total_incomplete;
    bool csv_write_failed = false;
    if (original_total_incomplete || output_total_incomplete) {
      try {
        logger.warn("体积汇总不可用，压缩率未统计。");
      } catch (...) {
      }
    }
    if (cfg.write_summary) {
      if (auto csv = write_csv(output_dir, results); !csv) {
        summary_failed = true;
        csv_write_failed = true;
        try {
          logger.warn(csv.error());
        } catch (...) {
        }
      }
    }
    const double total_ratio = original_total_incomplete ||
                                       output_total_incomplete ||
                                       original_total == 0
                                   ? 0.0
                                   : static_cast<double>(output_total) /
                                         static_cast<double>(original_total);

    const int worker_failure_count = worker_failures;
    const bool has_failures =
        failed_count > 0 || worker_failure_count > 0 || summary_failed;

    BatchSummary summary{
        .ok_count = pipeline_detail::count_to_int_saturated(ok_count),
        .failed_count = pipeline_detail::count_to_int_saturated(failed_count),
        .canceled_count =
            pipeline_detail::count_to_int_saturated(canceled_count),
        .large_image_queued_count = 0,
        .original_total = original_total,
        .output_total = output_total,
        .canceled = canceled,
        .exit_code = canceled ? 130 : (has_failures ? 2 : 0)};
    try {
      std::string summary_report;
      if (cfg.write_summary) {
        summary_report = "，报告 summary.csv";
        if (csv_write_failed) {
          summary_report += "，报告写入失败";
        }
      }
      const auto volume_incomplete =
          original_total_incomplete || output_total_incomplete;
      const auto volume_text =
          volume_incomplete
              ? std::format("体积 {} -> 未统计", format_size(original_total))
              : std::format("体积 {} -> {} ({:.1f}%)",
                            format_size(original_total),
                            format_size(output_total), total_ratio * 100.0);
      emit_progress(
          progress,
          BatchProgress{
              .kind = BatchEventKind::summary,
              .completed = completed.load(),
              .total = files.size(),
              .summary = summary,
              .text = std::format(
                  "{}：成功 {}，失败 {}，取消 {}，大图模式 {}{}；{}{}",
                  canceled ? "已取消" : "完成", ok_count, failed_count,
                  canceled_count, classified->large_mode.size(),
                  classified->large_mode.empty() ? std::string{}
                                                 : "（已自动处理）",
                  volume_text, summary_report)});
    } catch (...) {
    }
    return summary;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"批处理运行内存不足，程序已安全退出。"};
  } catch (const std::length_error&) {
    return std::unexpected{"批处理运行数据超过运行时限制，程序已安全退出。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"批处理运行文件系统访问失败，程序已安全退出。"};
  } catch (const std::exception&) {
    return std::unexpected{"批处理运行时异常，程序已安全退出。"};
  } catch (...) {
    return std::unexpected{"未知异常，程序已安全退出。"};
  }
}

// 顶层流水线返回进程退出码；单张图片错误会落到 CSV，不会让程序闪退。
int run_pipeline(const AppConfig& cfg,
                 std::span<const std::filesystem::path> input_paths,
                 std::stop_token stop_token);

int run_pipeline(const AppConfig& cfg, std::stop_token stop_token = {}) {
  return run_pipeline(cfg, std::span<const std::filesystem::path>{}, stop_token);
}

int run_pipeline(const AppConfig& cfg,
                 std::span<const std::filesystem::path> input_paths,
                 std::stop_token stop_token) {
  std::mutex print_mutex;
  auto runtime_cfg = cfg;
  runtime_cfg.frame_progress = [&](std::size_t item, std::size_t completed, std::size_t total) {
    // Bound worker event volume for long animations while retaining first/last progress.
    if (completed != 1 && completed != total && completed % 16 != 0) return;
    std::scoped_lock lock{print_mutex};
    if ((!cfg.studio_queue_manifest.empty() || !cfg.studio_cancel_event_name.empty())) {
      print_line(std::format("@AWJ-STUDIO/1 FRAME {} {} {}", item, completed, total));
    } else {
      print_line(std::format("动画 {}: 帧 {}/{}", item + 1, completed, total));
    }
    if (cfg.frame_progress) cfg.frame_progress(item, completed, total);
  };
  const auto summary = run_batch(
      runtime_cfg,
      [&](const BatchProgress& event) {
        std::scoped_lock lock{print_mutex};
        if ((!cfg.studio_queue_manifest.empty() || !cfg.studio_cancel_event_name.empty()) &&
            (event.kind == BatchEventKind::item_started ||
             event.kind == BatchEventKind::item_finished)) {
          const char status =
              event.kind == BatchEventKind::item_started
                  ? 'R'
                  : (event.result.ok
                         ? (event.result.skipped ? 'S' : 'D')
                         : (event.result.canceled ? 'C' : 'F'));
          print_line(std::format("@AWJ-STUDIO/1 ITEM {} {} {} {}",
                                 event.result.index, status, event.completed,
                                 event.total));
          if (event.kind == BatchEventKind::item_finished) {
            const auto microseconds = [](double seconds) -> std::int64_t {
              return seconds < 0.0
                         ? -1
                         : static_cast<std::int64_t>(seconds * 1'000'000.0 +
                                                     0.5);
            };
            print_line(std::format(
                "@AWJ-STUDIO/1 DETAIL {} {} {} {} {} {} {}",
                event.result.index,
                event.result.encoder_id.empty() ? "-" : event.result.encoder_id,
                event.result.encoder_threads,
                microseconds(event.result.decode_seconds),
                microseconds(event.result.prepare_seconds),
                microseconds(event.result.encode_seconds),
                microseconds(event.result.write_seconds)));
          }
        }
        if (event.kind == BatchEventKind::summary) {
          print_line("");
        }
        if (cfg.studio_queue_manifest.empty() && cfg.studio_cancel_event_name.empty() &&
            event.kind == BatchEventKind::item_finished && event.result.ok &&
            !event.result.skipped && !event.result.message.empty() &&
            event.result.message != "OK") {
          std::println(stderr, "[WARN] {}", event.result.message);
          std::fflush(stderr);
        }
        if (!event.text.empty()) {
          print_line(event.text);
        }
      },
      stop_token,
      input_paths);
  if (!summary) {
    print_line(std::format("[FAIL] {}", summary.error()));
    return 1;
  }
  return summary->exit_code;
}

}  // namespace awj
