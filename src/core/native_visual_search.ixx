module;
#include "../compat/stdlib_compat.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "parallel_rows.h"

export module awj.native_visual_search;

import awj.codec;
import awj.config;
import awj.core;
import awj.encoding_defaults;
import awj.image;
import awj.resource_planner;
import awj.visual_metrics;
import awj.visual_quality;

export namespace awj {

namespace native_visual_search_detail {

using Clock = std::chrono::steady_clock;

double elapsed_seconds(Clock::time_point started) {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

EncodeTimingDiagnostics make_visual_quality_timing() {
  EncodeTimingDiagnostics timing{};
  timing.encode_seconds = 0.0;
  timing.visual_quality_search_seconds = 0.0;
  timing.visual_quality_candidate_encode_seconds = 0.0;
  timing.visual_quality_candidate_decode_seconds = 0.0;
  timing.visual_quality_candidate_io_seconds = 0.0;
  timing.visual_quality_luma_seconds = 0.0;
  timing.gmsd_seconds = 0.0;
  timing.ms_ssim_seconds = 0.0;
  timing.visual_quality_metric_seconds = 0.0;
  return timing;
}

void accumulate_visual_quality_timing(EncodeTimingDiagnostics& target,
                                      const EncodeTimingDiagnostics& source) {
  target.visual_quality_candidate_encode_seconds +=
      source.visual_quality_candidate_encode_seconds;
  target.encode_seconds += source.encode_seconds;
  target.visual_quality_candidate_decode_seconds +=
      source.visual_quality_candidate_decode_seconds;
  target.visual_quality_candidate_io_seconds +=
      source.visual_quality_candidate_io_seconds;
  target.visual_quality_luma_seconds += source.visual_quality_luma_seconds;
  target.gmsd_seconds += source.gmsd_seconds;
  target.ms_ssim_seconds += source.ms_ssim_seconds;
  target.visual_quality_metric_seconds += source.visual_quality_metric_seconds;
  target.visual_quality_candidate_count +=
      source.visual_quality_candidate_count;
  target.visual_quality_decode_memory_fallback_count +=
      source.visual_quality_decode_memory_fallback_count;
}

int midpoint_quality(int low, int high) noexcept {
  return low + (high - low) / 2;
}

VisualMetricExecution metric_execution(const ResourcePlan& resources, awj::compat::stop_token stop) noexcept {
  const int workers = per_file_thread_budget(resources);
  // image_memory_plan already includes regular_memory_fixed_bytes(workers) in
  // the metric phase. Lend a bounded slice of that existing control allowance,
  // not extra luma/pyramid storage or a new reservation outside the ticket.
  auto control = static_cast<std::uint64_t>(std::min(workers, parallel_rows_detail::max_worker_budget)) *
      parallel_rows_detail::worker_control_bytes + parallel_rows_detail::reduction_control_bytes;
  if (!encoding_defaults::memory_limits_unlocked()) {
    control = std::min(control, resource_memory_budget(resources));
    if (resources.memory_reservation_bytes) control = std::min(control, resources.memory_reservation_bytes);
  }
  return {.thread_budget = workers, .stop = stop, .scratch_limit_bytes = control};
}

awj::compat::expected<std::size_t, std::string> checked_visual_quality_stride(
    std::size_t width, std::size_t channels, std::string_view context) {
  if (channels == 0 || width == 0 ||
      width > std::numeric_limits<std::size_t>::max() / channels) {
    return awj::compat::unexpected{awj::compat::format("{} 输入宽度无效。", context)};
  }
  return width * channels;
}

awj::compat::expected<std::size_t, std::string> checked_visual_quality_image_bytes(
    std::size_t stride, std::size_t height, std::string_view context) {
  if (stride == 0 || height == 0 ||
      height > std::numeric_limits<std::size_t>::max() / stride) {
    return awj::compat::unexpected{awj::compat::format("{} 输入尺寸过大。", context)};
  }
  const auto byte_count = stride * height;
  if (static_cast<std::uint64_t>(byte_count) >
      encoding_defaults::effective_max_input_file_bytes()) {
    return awj::compat::unexpected{
        awj::compat::format("{} 图像 buffer 超过当前运行时上限。", context)};
  }
  return byte_count;
}

awj::compat::expected<std::vector<std::byte>, std::string>
make_visual_quality_byte_buffer(std::size_t byte_count,
                                std::string_view context) {
  if (static_cast<std::uint64_t>(byte_count) >
      encoding_defaults::effective_max_input_file_bytes()) {
    return awj::compat::unexpected{
        awj::compat::format("{} 输出缓冲区超过当前运行时上限。", context)};
  }
  std::vector<std::byte> buffer;
  try {
    buffer.resize(byte_count);
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{awj::compat::format("{} 输出缓冲区内存不足。", context)};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{
        awj::compat::format("{} 输出缓冲区尺寸超过运行时限制。", context)};
  }
  return buffer;
}

bool output_format_uses_rgb8_cache(OutputFormat format) noexcept {
  return format == OutputFormat::jxl || format == OutputFormat::jpgli;
}

std::string_view rgb8_cache_context(OutputFormat format) noexcept {
  return format == OutputFormat::jpgli ? "JPG visual_quality RGB"
                                       : "JXL visual_quality RGB";
}

awj::compat::expected<std::vector<std::byte>, std::string> make_rgb8_input_cache(
    const ImageBuffer& image, const NativeEncodeSettings& settings, awj::compat::stop_token stop = {}) {
  if (stop.stop_requested()) return awj::compat::unexpected{"visual_quality 搜索已取消。"};
  if (!output_format_uses_rgb8_cache(settings.output_format) ||
      settings.applied_alpha == "kept") {
    return std::vector<std::byte>{};
  }
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
      image.planes.empty()) {
    return std::vector<std::byte>{};
  }
  const auto context = rgb8_cache_context(settings.output_format);
  if (image.width == 0 ||
      image.width > std::numeric_limits<std::size_t>::max() / 3) {
    return awj::compat::unexpected{awj::compat::format("{} 输入宽度无效。", context)};
  }
  const auto rgb_stride =
      checked_visual_quality_stride(image.width, 3, context);
  if (!rgb_stride) {
    return awj::compat::unexpected{rgb_stride.error()};
  }
  const auto rgb_size = checked_visual_quality_image_bytes(
      *rgb_stride, image.height, context);
  if (!rgb_size) {
    return awj::compat::unexpected{rgb_size.error()};
  }
  const auto rgba_stride =
      checked_visual_quality_stride(image.width, 4, context);
  if (!rgba_stride) {
    return awj::compat::unexpected{rgba_stride.error()};
  }
  const auto& plane = image.planes.front();
  const auto rgba_size = checked_visual_quality_image_bytes(
      plane.stride, image.height, context);
  if (!rgba_size) {
    return awj::compat::unexpected{rgba_size.error()};
  }
  if (plane.stride < *rgba_stride || plane.read_bytes().size() < *rgba_size) {
    return awj::compat::unexpected{
        awj::compat::format("{} 输入 RGBA buffer 尺寸无效。", context)};
  }

  auto rgb = make_visual_quality_byte_buffer(*rgb_size, context);
  if (!rgb) {
    return awj::compat::unexpected{rgb.error()};
  }
  const auto* rgba_data =
      reinterpret_cast<const std::uint8_t*>(plane.read_bytes().data());
  auto* rgb_data = reinterpret_cast<std::uint8_t*>(rgb->data());
  const auto execution = metric_execution(settings.resources, stop);
  const parallel_rows_detail::RunOptions options{.worker_budget = execution.thread_budget,
      .scratch_limit_bytes = execution.scratch_limit_bytes};
  auto converted = parallel_rows_detail::for_each(image.height, image.width, options, stop,
      [&](std::size_t y) -> awj::compat::expected<void, std::string> {
        const auto* rgba_row = rgba_data + y * plane.stride;
        auto* rgb_row = rgb_data + y * *rgb_stride;
        for (std::size_t x = 0; x < image.width; ++x) {
          if (x % 1024 == 0 && stop.stop_requested()) return awj::compat::unexpected{"visual_quality 搜索已取消。"};
          rgb_row[x * 3] = rgba_row[x * 4];
          rgb_row[x * 3 + 1] = rgba_row[x * 4 + 1];
          rgb_row[x * 3 + 2] = rgba_row[x * 4 + 2];
        }
        return {};
      });
  if (!converted) return awj::compat::unexpected{converted.error()};
  return rgb;
}

awj::compat::expected<std::span<const std::byte>, std::string> make_rgb8_input_view(
    const ImageBuffer& image, const NativeEncodeSettings& settings,
    std::vector<std::byte>& cache, awj::compat::stop_token stop = {}) {
  if (stop.stop_requested()) return awj::compat::unexpected{"visual_quality 搜索已取消。"};
  if (!output_format_uses_rgb8_cache(settings.output_format) ||
      settings.applied_alpha == "kept") {
    return std::span<const std::byte>{};
  }
  if (cache.empty()) {
    auto prepared = make_rgb8_input_cache(image, settings, stop);
    if (!prepared) {
      return awj::compat::unexpected{prepared.error()};
    }
    cache = std::move(*prepared);
  }
  return std::span<const std::byte>{cache};
}

void append_visual_quality_probe(VisualQualitySearchTrace& trace,
                                 const VisualQualityCandidate& candidate,
                                 int requested) {
  try {
    trace.probes.push_back(
        VisualQualitySearchProbe{.quality = candidate.quality,
                                 .bytes = candidate.bytes,
                                 .visual_score = candidate.visual_score,
                                 .target_met = visual_quality_candidate_meets_target(candidate, requested),
                                 .decision = visual_quality_candidate_meets_target(candidate, requested)
                                                 ? "达标，继续向更低质量收缩"
                                                 : "未达标，继续向更高质量扩展"});
  } catch (...) {
    // Trace 是诊断信息，不应影响编码结果。
  }
}

std::string format_visual_quality_trace(const VisualQualitySearchTrace& trace) {
  std::string text;
  try {
    text = awj::compat::format("requested={} range=q{}..q{} predicted=q{}",
                       trace.requested_visual_quality,
                       trace.q_min,
                       trace.q_max,
                       trace.predicted_quality);
    if (!trace.probes.empty()) {
      text += " probes=[";
      for (std::size_t index = 0; index < trace.probes.size(); ++index) {
        const auto& probe = trace.probes[index];
        if (index != 0) {
          text += "; ";
        }
        text += awj::compat::format("q{}:{} vq={:.2f} bytes={} ({})",
                            probe.quality,
                            probe.target_met ? "pass" : "fail",
                            probe.visual_score,
                            probe.bytes,
                            probe.decision);
      }
      text += "]";
    }
    if (!trace.selection_reason.empty()) {
      text += awj::compat::format(" selected={}", trace.selection_reason);
    }
    if (trace.fallback_used) {
      text += " fallback=true";
    }
  } catch (...) {
    return {};
  }
  return text;
}

}  // namespace native_visual_search_detail

struct NativeVisualQualitySearchResult {
  NativeEncodeResult encode_result{};
  VisualQualityCandidate candidate{};
  bool target_met{};
};

struct EvaluatedVisualQualityCandidate {
  VisualQualityCandidate candidate{};
  NativeEncodeResult encode_result{};
  EncodeTimingDiagnostics timing{};
};

awj::compat::expected<EvaluatedVisualQualityCandidate, std::string>
evaluate_visual_quality_candidate(
    std::optional<LumaImage>& reference_luma,
    CpuVisualMetricWorkspace& cpu_workspace,
    const ImageBuffer& reference_image, const ImageEncoder& encoder,
    const ImageDecoder& decoder, NativeEncodeSettings settings, int quality,
    const fs::path& candidate_path,
    awj::compat::stop_token stop_token = {}) {
  const auto execution = native_visual_search_detail::metric_execution(settings.resources, stop_token);
  auto timing = native_visual_search_detail::make_visual_quality_timing();
  timing.visual_quality_candidate_count = 1;
  settings.quality = quality;
  settings.visual_quality.reset();
  if (stop_token.stop_requested()) {
    return awj::compat::unexpected{"visual_quality 搜索已取消。"};
  }

  auto started = native_visual_search_detail::Clock::now();
  auto encoded = encoder.encode(reference_image, settings, stop_token);
  timing.visual_quality_candidate_encode_seconds +=
      native_visual_search_detail::elapsed_seconds(started);
  timing.encode_seconds += timing.visual_quality_candidate_encode_seconds;
  if (!encoded) {
    return awj::compat::unexpected{encoded.error()};
  }
  if (stop_token.stop_requested()) {
    return awj::compat::unexpected{"visual_quality 搜索已取消。"};
  }

  started = native_visual_search_detail::Clock::now();
  const auto candidate_name = path_to_utf8(candidate_path.filename());
  auto decoded = decoder.decode_memory(
      std::span<const std::byte>{encoded->encoded.bytes}, candidate_name,
      DecodeOptions{.copy_metadata_payloads = false, .stop_token = stop_token});
  timing.visual_quality_candidate_decode_seconds +=
      native_visual_search_detail::elapsed_seconds(started);
  if (!decoded) {
    return awj::compat::unexpected{decoded.error()};
  }
  if (stop_token.stop_requested()) {
    return awj::compat::unexpected{"visual_quality 搜索已取消。"};
  }

  started = native_visual_search_detail::Clock::now();
  auto candidate_luma =
      make_luma_image(decoded->image, std::move(cpu_workspace.candidate_luma), execution);
  decoded->image = ImageBuffer{};
  timing.visual_quality_luma_seconds +=
      native_visual_search_detail::elapsed_seconds(started);
  if (!candidate_luma) {
    return awj::compat::unexpected{candidate_luma.error()};
  }
  if (stop_token.stop_requested()) {
    return awj::compat::unexpected{"visual_quality 搜索已取消。"};
  }

  auto ensure_reference_luma =
      [&]() -> awj::compat::expected<const LumaImage*, std::string> {
    if (!reference_luma) {
      if (stop_token.stop_requested()) {
        return awj::compat::unexpected{"visual_quality 搜索已取消。"};
      }
      const auto luma_started = native_visual_search_detail::Clock::now();
      auto luma = make_luma_image(reference_image, {}, execution);
      timing.visual_quality_luma_seconds +=
          native_visual_search_detail::elapsed_seconds(luma_started);
      if (!luma) {
        return awj::compat::unexpected{luma.error()};
      }
      if (stop_token.stop_requested()) {
        return awj::compat::unexpected{"visual_quality 搜索已取消。"};
      }
      reference_luma = std::move(*luma);
    }
    return &*reference_luma;
  };

  auto record_gmsd_metric =
      [&](awj::compat::expected<double, std::string> result,
          native_visual_search_detail::Clock::time_point metric_started) {
        const auto metric_seconds =
            native_visual_search_detail::elapsed_seconds(metric_started);
        timing.gmsd_seconds += metric_seconds;
        timing.visual_quality_metric_seconds += metric_seconds;
        return result;
      };

  auto record_ms_ssim_metric =
      [&](awj::compat::expected<double, std::string> result,
          native_visual_search_detail::Clock::time_point metric_started) {
        const auto metric_seconds =
            native_visual_search_detail::elapsed_seconds(metric_started);
        timing.ms_ssim_seconds += metric_seconds;
        timing.visual_quality_metric_seconds += metric_seconds;
        return result;
      };

  auto gmsd = [&]() -> awj::compat::expected<double, std::string> {
    auto current_reference_luma = ensure_reference_luma();
    if (!current_reference_luma) {
      return awj::compat::unexpected{current_reference_luma.error()};
    }
    started = native_visual_search_detail::Clock::now();
    return record_gmsd_metric(
        compute_gmsd(**current_reference_luma, *candidate_luma, execution), started);
  }();
  if (!gmsd) {
    return awj::compat::unexpected{gmsd.error()};
  }
  if (stop_token.stop_requested()) {
    return awj::compat::unexpected{"visual_quality 搜索已取消。"};
  }

  auto current_reference_luma = ensure_reference_luma();
  if (!current_reference_luma) {
    return awj::compat::unexpected{current_reference_luma.error()};
  }
  auto ms_ssim = [&]() -> awj::compat::expected<double, std::string> {
    started = native_visual_search_detail::Clock::now();
    return record_ms_ssim_metric(
        compute_ms_ssim_cached(**current_reference_luma, *candidate_luma, cpu_workspace, execution), started);
  }();
  if (!ms_ssim) {
    return awj::compat::unexpected{ms_ssim.error()};
  }
  if (stop_token.stop_requested()) {
    return awj::compat::unexpected{"visual_quality 搜索已取消。"};
  }

  started = native_visual_search_detail::Clock::now();
  const auto metrics = make_visual_metric_result(*gmsd, *ms_ssim);
  cpu_workspace.candidate_luma = std::move(*candidate_luma);
  timing.visual_quality_metric_seconds +=
      native_visual_search_detail::elapsed_seconds(started);
  const auto encoded_bytes = encoded->encoded.bytes.size();
  auto encode_result = std::move(*encoded);
  encode_result.diagnostics.timing = timing;
  return EvaluatedVisualQualityCandidate{
      .candidate =
          VisualQualityCandidate{
              .quality = encode_result.final_quality,
              .bytes = encoded_bytes,
              .visual_score = metrics.score.visual_score,
              .raw_gmsd = metrics.raw_gmsd,
              .raw_ms_ssim = metrics.raw_ms_ssim,
              .gmsd_quality_score = metrics.score.gmsd_quality_score,
              .msssim_quality_score = metrics.score.msssim_quality_score},
      .encode_result = std::move(encode_result),
      .timing = timing};
}

awj::compat::expected<NativeVisualQualitySearchResult, std::string>
encode_with_native_visual_quality_search(const ImageBuffer& reference_image,
                                         const ImageEncoder& encoder,
                                         const ImageDecoder& decoder,
                                         NativeEncodeSettings settings,
                                         const fs::path& candidate_path,
                                         awj::compat::stop_token stop_token = {}) {
  if (!settings.visual_quality) {
    auto encoded = encoder.encode(reference_image, settings, stop_token);
    if (!encoded) {
      return awj::compat::unexpected{encoded.error()};
    }
    return NativeVisualQualitySearchResult{.encode_result = std::move(*encoded),
                                           .target_met = true};
  }

  const int requested = std::clamp(*settings.visual_quality, 1, 100);
  const auto range = get_quality_search_range(requested);
  // 自动渐进的首层固定 q10；搜索末层时也遵守上游的最低目标质量。
  const int minimum_quality = settings.output_format == OutputFormat::avif && settings.avif_layers.automatic
      ? 10 : ENCODER_QUALITY_MIN;
  const int search_q_min =
      std::clamp(range.q_min, minimum_quality, ENCODER_QUALITY_MAX - 1);
  const int search_q_max =
      std::clamp(range.q_max, search_q_min, ENCODER_QUALITY_MAX - 1);
  VisualQualitySearchTrace search_trace{.requested_visual_quality = requested,
                                        .q_min = search_q_min,
                                        .q_max = search_q_max,
                                        .predicted_quality = search_q_min};
  const auto search_started = native_visual_search_detail::Clock::now();
  auto timing = native_visual_search_detail::make_visual_quality_timing();
  if (range.lossless) {
    settings.quality = 100;
    const auto encode_started = native_visual_search_detail::Clock::now();
    auto encoded = encoder.encode(reference_image, settings, stop_token);
    timing.visual_quality_candidate_encode_seconds +=
        native_visual_search_detail::elapsed_seconds(encode_started);
    timing.encode_seconds += timing.visual_quality_candidate_encode_seconds;
    timing.visual_quality_candidate_count = 1;
    timing.visual_quality_search_seconds =
        native_visual_search_detail::elapsed_seconds(search_started);
    if (!encoded) {
      return awj::compat::unexpected{encoded.error()};
    }
    const auto encoded_bytes = encoded->encoded.bytes.size();
    encoded->final_quality = 100;
    encoded->search_attempt_count = 1;
    encoded->diagnostics.timing = timing;
    encoded->diagnostics.visual_quality_search_trace =
        "requested=100 range=lossless selected=q100 lossless-bypass";
    return NativeVisualQualitySearchResult{
        .encode_result = std::move(*encoded),
        .candidate = VisualQualityCandidate{.quality = 100,
                                            .bytes = encoded_bytes,
                                            .visual_score = 99.0,
                                            .raw_gmsd = 0.0,
                                            .raw_ms_ssim = 1.0,
                                            .gmsd_quality_score = 99.0,
                                            .msssim_quality_score = 99.0},
        .target_met = true};
  }

  std::optional<LumaImage> reference_luma;
  CpuVisualMetricWorkspace cpu_workspace;

  std::optional<EvaluatedVisualQualityCandidate> closest_candidate_result;
  std::optional<EvaluatedVisualQualityCandidate> smallest_passing_result;
  std::vector<VisualQualityCandidate> candidates;
  std::vector<int> evaluated_qualities;
  const auto reserve_candidate_count = static_cast<std::size_t>(
      std::max(4, search_q_max - search_q_min + 1));
  try {
    candidates.reserve(reserve_candidate_count);
    evaluated_qualities.reserve(reserve_candidate_count);
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"visual_quality 候选记录内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"visual_quality 候选记录数量超过运行时限制。"};
  }

  auto candidate_matches = [](const VisualQualityCandidate& left,
                              const VisualQualityCandidate& right) noexcept {
    return left.quality == right.quality && left.bytes == right.bytes &&
           left.visual_score == right.visual_score;
  };

  auto retain_candidate_result =
      [&](EvaluatedVisualQualityCandidate&& evaluated) {
        const auto& candidate = evaluated.candidate;
        if (visual_quality_candidate_meets_target(candidate, requested)) {
          if (!smallest_passing_result ||
              candidate.bytes < smallest_passing_result->candidate.bytes ||
              (candidate.bytes == smallest_passing_result->candidate.bytes &&
               candidate.quality <
                   smallest_passing_result->candidate.quality)) {
            smallest_passing_result = std::move(evaluated);
          }
          return;
        }
        if (!closest_candidate_result ||
            candidate.visual_score >
                closest_candidate_result->candidate.visual_score ||
            (candidate.visual_score ==
                 closest_candidate_result->candidate.visual_score &&
             candidate.bytes < closest_candidate_result->candidate.bytes)) {
          closest_candidate_result = std::move(evaluated);
        }
      };

  auto find_evaluated_candidate =
      [&](int quality) -> std::optional<VisualQualityCandidate> {
    quality = std::clamp(quality, search_q_min, search_q_max);
    for (std::size_t index = 0; index < evaluated_qualities.size(); ++index) {
      if (evaluated_qualities[index] == quality) {
        return candidates[index];
      }
    }
    return std::nullopt;
  };

  std::vector<std::byte> rgb8_input_cache;

  auto evaluate_quality =
      [&](int quality) -> awj::compat::expected<VisualQualityCandidate, std::string> {
    quality = std::clamp(quality, search_q_min, search_q_max);
    if (const auto existing = find_evaluated_candidate(quality)) {
      return *existing;
    }
    if (stop_token.stop_requested()) {
      return awj::compat::unexpected{"visual_quality 搜索已取消。"};
    }
    auto candidate_settings = settings;
    auto rgb8_input = native_visual_search_detail::make_rgb8_input_view(
        reference_image, candidate_settings, rgb8_input_cache, stop_token);
    if (!rgb8_input) {
      return awj::compat::unexpected{rgb8_input.error()};
    }
    if (candidate_settings.output_format == OutputFormat::jxl) {
      candidate_settings.jxl_rgb8_input = *rgb8_input;
    } else if (candidate_settings.output_format == OutputFormat::jpgli) {
      candidate_settings.jpegli_rgb8_input = *rgb8_input;
    }
    auto candidate = evaluate_visual_quality_candidate(
        reference_luma, cpu_workspace, reference_image, encoder, decoder,
        candidate_settings, quality, candidate_path, stop_token);
    if (!candidate) {
      return awj::compat::unexpected{candidate.error()};
    }
    auto candidate_summary = candidate->candidate;
    try {
      candidates.push_back(candidate_summary);
      evaluated_qualities.push_back(quality);
    } catch (const std::bad_alloc&) {
      if (candidates.size() > evaluated_qualities.size()) {
        candidates.pop_back();
      }
      return awj::compat::unexpected{"visual_quality 候选记录内存不足。"};
    } catch (const std::length_error&) {
      if (candidates.size() > evaluated_qualities.size()) {
        candidates.pop_back();
      }
      return awj::compat::unexpected{"visual_quality 候选记录数量超过运行时限制。"};
    }
    native_visual_search_detail::accumulate_visual_quality_timing(
        timing, candidate->timing);
    retain_candidate_result(std::move(*candidate));
    native_visual_search_detail::append_visual_quality_probe(
        search_trace, candidate_summary, requested);
    return candidate_summary;
  };

  CandidateSelection selected{};
  bool target_met = false;

  // 先探测曲线窗口中心作为预测点，再按达标方向收缩搜索窗口。
  // lossy q 限定为 1..99；q100 保留给 lossless 特例。
  int low = search_q_min - 1;
  int high = search_q_max;
  const int predicted_quality = std::clamp(
      native_visual_search_detail::midpoint_quality(low, high),
      search_q_min,
      search_q_max);
  search_trace.predicted_quality = predicted_quality;
  auto predicted_candidate = evaluate_quality(predicted_quality);
  if (!predicted_candidate) {
    return awj::compat::unexpected{predicted_candidate.error()};
  }
  if (visual_quality_candidate_meets_target(*predicted_candidate, requested)) {
    high = predicted_quality;
  } else {
    low = predicted_quality;
  }
  while (low + 1 < high) {
    const int midpoint =
        native_visual_search_detail::midpoint_quality(low, high);
    auto midpoint_candidate = evaluate_quality(midpoint);
    if (!midpoint_candidate) {
      return awj::compat::unexpected{midpoint_candidate.error()};
    }
    if (visual_quality_candidate_meets_target(*midpoint_candidate, requested)) {
      high = midpoint;
    } else {
      low = midpoint;
    }
  }

  auto final_candidate = evaluate_quality(high);
  if (!final_candidate) {
    return awj::compat::unexpected{final_candidate.error()};
  }

  if (visual_quality_candidate_meets_target(*final_candidate, requested)) {
    selected =
        select_smallest_passing_visual_quality_candidate(candidates, requested);
    target_met = selected.found;
    if (target_met) {
      search_trace.selection_reason = awj::compat::format(
          "smallest-passing q{} vq={:.2f} bytes={}",
          selected.candidate.quality,
          selected.candidate.visual_score,
          selected.candidate.bytes);
    }
  } else {
    if (!settings.visual_quality_fallback) {
      const auto closest = select_closest_visual_quality_candidate(candidates);
      if (closest.found) {
        return awj::compat::unexpected{awj::compat::format(
            "未找到达到 visual_quality={} 的候选，最接近候选为 q{}、VQ "
            "{:.2f}；请降低 visual_quality 或使用 visual_quality=100 无损。",
            requested, closest.candidate.quality, closest.candidate.visual_score)};
      }
      return awj::compat::unexpected{
          awj::compat::format("未找到达到 visual_quality={} 的候选，请降低 "
                      "visual_quality 或使用 visual_quality=100 无损。",
                      requested)};
    }
    selected = select_closest_visual_quality_candidate(candidates);
    if (selected.found) {
      search_trace.fallback_used = true;
      search_trace.selection_reason = awj::compat::format(
          "closest-fallback q{} vq={:.2f} bytes={}",
          selected.candidate.quality,
          selected.candidate.visual_score,
          selected.candidate.bytes);
    }
  }

  if (!selected.found) {
    return awj::compat::unexpected{"visual_quality 搜索没有产生候选。"};
  }

  EvaluatedVisualQualityCandidate* selected_result = nullptr;
  if (smallest_passing_result &&
      candidate_matches(smallest_passing_result->candidate,
                        selected.candidate)) {
    selected_result = &*smallest_passing_result;
  } else if (closest_candidate_result &&
             candidate_matches(closest_candidate_result->candidate,
                               selected.candidate)) {
    selected_result = &*closest_candidate_result;
  }
  if (selected_result == nullptr) {
    return awj::compat::unexpected{"visual_quality 搜索无法匹配已选候选。"};
  }

  auto encoded = std::move(selected_result->encode_result);
  timing.visual_quality_search_seconds =
      native_visual_search_detail::elapsed_seconds(search_started);
  encoded.final_quality = selected.candidate.quality;
  encoded.lossless = false;
  encoded.search_attempt_count = static_cast<int>(candidates.size());
  encoded.raw_gmsd = selected.candidate.raw_gmsd;
  encoded.raw_ms_ssim = selected.candidate.raw_ms_ssim;
  encoded.visual_score = VisualScoreBreakdown{
      .visual_score = selected.candidate.visual_score,
      .gmsd_quality_score = selected.candidate.gmsd_quality_score,
      .msssim_quality_score = selected.candidate.msssim_quality_score};
  encoded.diagnostics.timing = timing;
  encoded.diagnostics.visual_quality_search_trace =
      native_visual_search_detail::format_visual_quality_trace(search_trace);

  return NativeVisualQualitySearchResult{.encode_result = std::move(encoded),
                                         .candidate = selected.candidate,
                                         .target_met = target_met};
}

}  // namespace awj
