module;
#include "../compat/stdlib_compat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "parallel_rows.h"

export module awj.visual_metrics;

import awj.encoding_defaults;
import awj.image;
import awj.visual_quality;

export namespace awj {

struct LumaImage {
  std::size_t width{};
  std::size_t height{};
  std::vector<float> pixels{};

  [[nodiscard]] bool empty() const noexcept {
    return width == 0 || height == 0 || pixels.empty();
  }
};

struct VisualMetricResult {
  double raw_gmsd{};
  double raw_ms_ssim{};
  VisualScoreBreakdown score{};
};

// Internal C++ execution context, not part of the public C ABI. The caller
// lends only control/reduction bytes from its already-admitted fixed allowance;
// luma/pyramid buffers retain their existing ownership and reservation model.
// Defaults preserve all existing call sites and select the serial path.
struct VisualMetricExecution {
  int thread_budget{1};
  awj::compat::stop_token stop{};
  std::uint64_t scratch_limit_bytes{};
};

namespace visual_metrics_detail {

parallel_rows_detail::RunOptions parallel_options(const VisualMetricExecution& execution) noexcept {
  return {.worker_budget = execution.thread_budget, .scratch_limit_bytes = execution.scratch_limit_bytes};
}

awj::compat::expected<std::vector<float>, std::string> make_float_buffer(std::size_t pixel_count,
                                                                 std::string_view context,
                                                                 std::vector<float> buffer = {}) {
  if (pixel_count == 0) {
    return awj::compat::unexpected{awj::compat::format("{} 输入为空。", context)};
  }
  if (pixel_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
    return awj::compat::unexpected{awj::compat::format("{} luma buffer 尺寸超过运行时限制。", context)};
  }
  const auto byte_count = pixel_count * sizeof(float);
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return awj::compat::unexpected{awj::compat::format("{} luma buffer 超过当前运行时上限。", context)};
  }
  try {
    buffer.resize(pixel_count);
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"luma buffer 内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"luma buffer 尺寸超过运行时限制。"};
  }
  return buffer;
}

}  // namespace visual_metrics_detail

awj::compat::expected<LumaImage, std::string> make_luma_image(
    const ImageBuffer& image, LumaImage recycled = {}, VisualMetricExecution execution = {}) {
  if (execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
  if (image.width == 0 || image.height == 0 || image.planes.empty()) {
    return awj::compat::unexpected{"输入图像为空，无法计算视觉指标。"};
  }
  if (image.bit_depth != 8 && image.bit_depth != 10 &&
      image.bit_depth != 12 && image.bit_depth != 16) {
    return awj::compat::unexpected{"当前视觉指标仅支持 8、10、12 或 16-bit 图像。"};
  }

  if (image.width > std::numeric_limits<std::size_t>::max() / image.height) {
    return awj::compat::unexpected{"输入图像尺寸过大，无法计算视觉指标。"};
  }

  const auto pixel_count = image.width * image.height;
  auto pixels = visual_metrics_detail::make_float_buffer(pixel_count, "视觉指标",
                                                        std::move(recycled.pixels));
  if (!pixels) {
    return awj::compat::unexpected{pixels.error()};
  }

  const auto& plane = image.planes.front();
  LumaImage luma{.width = image.width, .height = image.height,
                 .pixels = std::move(*pixels)};
  const std::size_t bytes_per_sample = image.bit_depth > 8 ? 2 : 1;
  const double max_sample = image.bit_depth == 16
                                ? 65535.0
                                : static_cast<double>((1u << image.bit_depth) - 1u);
  const auto normalized_sample = [bytes_per_sample, max_sample](
                                     const unsigned char* pixel,
                                     std::size_t channel) noexcept {
    const auto* sample = pixel + channel * bytes_per_sample;
    if (bytes_per_sample == 1) {
      return static_cast<double>(sample[0]) / max_sample;
    }
    const auto value = static_cast<unsigned int>(sample[0]) |
                       (static_cast<unsigned int>(sample[1]) << 8u);
    return static_cast<double>(value) / max_sample;
  };

  if (image.pixel_format == PixelFormat::rgba || image.pixel_format == PixelFormat::rgb) {
    const std::size_t channels = image.pixel_format == PixelFormat::rgba ? 4 : 3;
    if (image.width > std::numeric_limits<std::size_t>::max() / channels /
                          bytes_per_sample) {
      return awj::compat::unexpected{"RGB/RGBA 图像宽度过大，无法计算视觉指标。"};
    }
    const std::size_t min_stride = image.width * channels * bytes_per_sample;
    if (!plane.contains_rows(image.height, min_stride)) {
      return awj::compat::unexpected{"RGB/RGBA 图像 buffer 尺寸无效，无法计算视觉指标。"};
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(plane.read_bytes().data());
    auto converted = parallel_rows_detail::for_each(image.height, image.width,
        visual_metrics_detail::parallel_options(execution), execution.stop,
        [&](std::size_t y) -> awj::compat::expected<void, std::string> {
          const auto* row = bytes + y * plane.stride;
          for (std::size_t x = 0; x < image.width; ++x) {
            if (x % 1024 == 0 && execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
            const auto* pixel = row + x * channels * bytes_per_sample;
            const double luma_value =
                0.2126 * normalized_sample(pixel, 0) +
                0.7152 * normalized_sample(pixel, 1) +
                0.0722 * normalized_sample(pixel, 2);
            luma.pixels[y * image.width + x] = static_cast<float>(luma_value);
          }
          return {};
        });
    if (!converted) return awj::compat::unexpected{converted.error()};
    return luma;
  }

  if (image.pixel_format == PixelFormat::gray) {
    if (image.width > std::numeric_limits<std::size_t>::max() / bytes_per_sample ||
        !plane.contains_rows(image.height, image.width * bytes_per_sample)) {
      return awj::compat::unexpected{"灰度图像 buffer 尺寸无效，无法计算视觉指标。"};
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(plane.read_bytes().data());
    auto converted = parallel_rows_detail::for_each(image.height, image.width,
        visual_metrics_detail::parallel_options(execution), execution.stop,
        [&](std::size_t y) -> awj::compat::expected<void, std::string> {
          const auto* row = bytes + y * plane.stride;
          for (std::size_t x = 0; x < image.width; ++x) {
            if (x % 1024 == 0 && execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
            luma.pixels[y * image.width + x] =
                static_cast<float>(normalized_sample(row + x * bytes_per_sample, 0));
          }
          return {};
        });
    if (!converted) return awj::compat::unexpected{converted.error()};
    return luma;
  }

  return awj::compat::unexpected{"当前视觉指标仅支持 RGB、RGBA 或灰度图像。"};
}

namespace visual_metrics_detail {

awj::compat::expected<void, std::string> validate_same_shape(const LumaImage& reference,
                                                     const LumaImage& candidate) {
  if (reference.empty() || candidate.empty()) {
    return awj::compat::unexpected{"视觉指标输入为空。"};
  }
  if (reference.width != candidate.width || reference.height != candidate.height ||
      reference.pixels.size() != candidate.pixels.size()) {
    return awj::compat::unexpected{"视觉指标输入尺寸不一致。"};
  }
  if (reference.width > std::numeric_limits<std::size_t>::max() / reference.height ||
      reference.width * reference.height != reference.pixels.size()) {
    return awj::compat::unexpected{"视觉指标输入尺寸无效。"};
  }
  return {};
}

awj::compat::expected<LumaImage, std::string> downsample_2x(const LumaImage& source,
    LumaImage recycled = {}, VisualMetricExecution execution = {}) {
  if (execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
  if (auto valid = validate_same_shape(source, source); !valid) return awj::compat::unexpected{valid.error()};
  LumaImage result{.width = source.width / 2 + source.width % 2,
                   .height = source.height / 2 + source.height % 2};
  if (result.width == source.width && result.height == source.height) {
    return awj::compat::unexpected{"视觉指标降采样尺寸无变化。"};
  }
  if (result.width > std::numeric_limits<std::size_t>::max() / result.height) {
    return awj::compat::unexpected{"视觉指标降采样尺寸过大。"};
  }
  auto pixels = make_float_buffer(result.width * result.height, "视觉指标降采样",
                                  std::move(recycled.pixels));
  if (!pixels) {
    return awj::compat::unexpected{pixels.error()};
  }
  result.pixels = std::move(*pixels);
  auto sampled = parallel_rows_detail::for_each(result.height, result.width,
      parallel_options(execution), execution.stop,
      [&](std::size_t y) -> awj::compat::expected<void, std::string> {
        for (std::size_t x = 0; x < result.width; ++x) {
          if (x % 1024 == 0 && execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
          const std::size_t sx = x * 2;
          const std::size_t sy = y * 2;
          double sum = 0.0;
          double count = 0.0;
          for (std::size_t oy = 0; oy < 2 && sy + oy < source.height; ++oy) {
            for (std::size_t ox = 0; ox < 2 && sx + ox < source.width; ++ox) {
              sum += source.pixels[(sy + oy) * source.width + sx + ox];
              count += 1.0;
            }
          }
          result.pixels[y * result.width + x] = static_cast<float>(sum / count);
        }
        return {};
      });
  if (!sampled) return awj::compat::unexpected{sampled.error()};
  return result;
}

struct Statistics { double mean{}, variance{}; };
struct GlobalStatistics { Statistics reference{}, candidate{}; double covariance{}; };

// The cached and uncached paths share exactly the same fixed-block arithmetic.
// Cached reference state is immutable while workers inspect the current level.
awj::compat::expected<GlobalStatistics, std::string> global_statistics(
    const LumaImage& reference, const LumaImage& candidate,
    VisualMetricExecution execution, const Statistics* cached = nullptr) {
  if (auto valid = validate_same_shape(reference, candidate); !valid) return awj::compat::unexpected{valid.error()};
  struct Sums { double reference{}, candidate{}; };
  const auto size = reference.pixels.size();
  const auto count = static_cast<double>(size);
  const auto options = parallel_options(execution);
  const bool same = &reference == &candidate;
  auto sums = parallel_rows_detail::reduce<Sums>(size, options, execution.stop,
      [&](std::size_t first, std::size_t end, Sums& state) -> awj::compat::expected<void, std::string> {
        for (auto i = first; i < end; ++i) {
          if (i % 1024 == 0 && execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
          const auto pixel = candidate.pixels[i];
          if (!std::isfinite(pixel)) return awj::compat::unexpected{"视觉指标包含非有限 luma。"};
          state.candidate += pixel;
          if (!cached && !same) state.reference += reference.pixels[i];
        }
        return {};
      }, [](Sums& total, const Sums& block) noexcept {
        total.reference += block.reference;
        total.candidate += block.candidate;
      });
  if (!sums) return awj::compat::unexpected{sums.error()};
  GlobalStatistics stats;
  stats.candidate.mean = sums->candidate / count;
  stats.reference.mean = cached ? cached->mean : same ? stats.candidate.mean : sums->reference / count;
  struct Moments { double reference{}, candidate{}, covariance{}; };
  auto moments = parallel_rows_detail::reduce<Moments>(size, options, execution.stop,
      [&](std::size_t first, std::size_t end, Moments& state) -> awj::compat::expected<void, std::string> {
        for (auto i = first; i < end; ++i) {
          if (i % 1024 == 0 && execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
          const double delta = candidate.pixels[i] - stats.candidate.mean;
          state.candidate += delta * delta;
          if (!same) {
            const double ref_delta = reference.pixels[i] - stats.reference.mean;
            if (!std::isfinite(ref_delta)) return awj::compat::unexpected{"视觉指标包含非有限 luma。"};
            if (!cached) state.reference += ref_delta * ref_delta;
            state.covariance += ref_delta * delta;
          }
        }
        return {};
      }, [](Moments& total, const Moments& block) noexcept {
        total.reference += block.reference;
        total.candidate += block.candidate;
        total.covariance += block.covariance;
      });
  if (!moments) return awj::compat::unexpected{moments.error()};
  stats.candidate.variance = moments->candidate / count;
  stats.reference.variance = cached ? cached->variance : same ? stats.candidate.variance : moments->reference / count;
  stats.covariance = same ? stats.candidate.variance : moments->covariance / count;
  return stats;
}

double ssim_from_statistics(const GlobalStatistics& stats) noexcept {
  constexpr double c1 = 0.01 * 0.01, c2 = 0.03 * 0.03;
  const auto& ref = stats.reference;
  const auto& candidate = stats.candidate;
  const double numerator = (2.0 * ref.mean * candidate.mean + c1) *
                           (2.0 * stats.covariance + c2);
  const double denominator = (ref.mean * ref.mean + candidate.mean * candidate.mean + c1) *
                             (ref.variance + candidate.variance + c2);
  return denominator <= 0.0 ? 1.0 : std::clamp(numerator / denominator, 0.0, 1.0);
}

awj::compat::expected<double, std::string> ssim_global(const LumaImage& reference,
    const LumaImage& candidate, VisualMetricExecution execution = {}) {
  auto stats = global_statistics(reference, candidate, execution);
  if (!stats) return awj::compat::unexpected{stats.error()};
  return ssim_from_statistics(*stats);
}

}  // namespace visual_metrics_detail

awj::compat::expected<double, std::string> compute_gmsd(
    const LumaImage& reference, const LumaImage& candidate,
    VisualMetricExecution execution = {}) {
  if (execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
  if (auto valid = visual_metrics_detail::validate_same_shape(reference, candidate); !valid) {
    return awj::compat::unexpected{valid.error()};
  }

  struct Moments { std::size_t count{}; double mean{}, m2{}; };
  auto moments = parallel_rows_detail::reduce<Moments>(reference.pixels.size(),
      visual_metrics_detail::parallel_options(execution), execution.stop,
      [&](std::size_t first, std::size_t end, Moments& state) -> awj::compat::expected<void, std::string> {
        constexpr double c = 0.0026;
        for (auto i = first; i < end;) {
          const auto y = i / reference.width;
          const auto row = y * reference.width;
          const auto top = (y == 0 ? y : y - 1) * reference.width;
          const auto bottom = std::min(y + 1, reference.height - 1) * reference.width;
          const auto end_x = std::min(reference.width, end - row);
          // Fixed partitions may start/end inside a row. Keep the original
          // neighbor coordinates without adding a division for every pixel.
          for (auto x = i - row; x < end_x; ++x, ++i) {
            if (i % 1024 == 0 && execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
            const auto left = row + (x == 0 ? x : x - 1);
            const auto right = row + std::min(x + 1, reference.width - 1);
            const auto gradient = [&](const LumaImage& image) {
              const double dx = double(image.pixels[right]) - double(image.pixels[left]);
              const double dy = double(image.pixels[bottom + x]) - double(image.pixels[top + x]);
              return std::sqrt(dx * dx + dy * dy);
            };
            const double ref_grad = gradient(reference);
            const double candidate_grad = gradient(candidate);
            const double similarity = (2.0 * ref_grad * candidate_grad + c) /
                                      (ref_grad * ref_grad + candidate_grad * candidate_grad + c);
            if (!std::isfinite(similarity)) return awj::compat::unexpected{"GMSD 包含非有限 luma。"};
            const double delta = similarity - state.mean;
            state.mean += delta / static_cast<double>(++state.count);
            state.m2 += delta * (similarity - state.mean);
          }
        }
        return {};
      }, [](Moments& total, const Moments& block) noexcept {
        if (!total.count) { total = block; return; }
        const auto count = total.count + block.count;
        const double delta = block.mean - total.mean;
        const double weight = static_cast<double>(block.count) / static_cast<double>(count);
        total.m2 += block.m2 + delta * delta * static_cast<double>(total.count) * weight;
        total.mean += delta * weight;
        total.count = count;
      });
  if (!moments) return awj::compat::unexpected{moments.error()};
  return std::sqrt(std::max(0.0, moments->m2) / static_cast<double>(moments->count));
}

awj::compat::expected<double, std::string> compute_ms_ssim(
    const LumaImage& reference, const LumaImage& candidate,
    VisualMetricExecution execution = {}) {
  if (execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
  if (auto valid = visual_metrics_detail::validate_same_shape(reference, candidate); !valid) {
    return awj::compat::unexpected{valid.error()};
  }

  constexpr std::array<double, 5> weights{0.0448, 0.2856, 0.3001, 0.2363, 0.1333};
  const LumaImage* ref_level = &reference;
  const LumaImage* candidate_level = &candidate;
  LumaImage ref_owned;
  LumaImage candidate_owned;
  double value = 1.0;
  for (std::size_t level = 0; level < weights.size(); ++level) {
    auto ssim = visual_metrics_detail::ssim_global(*ref_level, *candidate_level, execution);
    if (!ssim) return awj::compat::unexpected{ssim.error()};
    value *= std::pow(std::clamp(*ssim, 1e-9, 1.0), weights[level]);
    if (level + 1 < weights.size() && ref_level->width > 1 && ref_level->height > 1) {
      auto next_ref = visual_metrics_detail::downsample_2x(*ref_level, {}, execution);
      if (!next_ref) {
        return awj::compat::unexpected{next_ref.error()};
      }
      auto next_candidate = visual_metrics_detail::downsample_2x(*candidate_level, {}, execution);
      if (!next_candidate) {
        return awj::compat::unexpected{next_candidate.error()};
      }
      ref_owned = std::move(*next_ref);
      candidate_owned = std::move(*next_candidate);
      ref_level = &ref_owned;
      candidate_level = &candidate_owned;
    }
  }
  return std::clamp(value, 0.0, 1.0);
}

// One workspace per visual search. Reference statistics/pyramids are immutable;
// candidate storage is reused and never shared between worker threads.
struct CpuVisualMetricWorkspace {
  using Statistics = visual_metrics_detail::Statistics;
  std::array<LumaImage, 4> reference_levels{}, candidate_levels{};
  std::array<Statistics, 5> reference_statistics{};
  LumaImage candidate_luma{};
  bool reference_ready{};
};

awj::compat::expected<double, std::string> compute_ms_ssim_cached(
    const LumaImage& reference, const LumaImage& candidate,
    CpuVisualMetricWorkspace& workspace, VisualMetricExecution execution = {}) {
  if (execution.stop.stop_requested()) return awj::compat::unexpected{"视觉指标已取消。"};
  if (auto valid = visual_metrics_detail::validate_same_shape(reference, candidate); !valid)
    return awj::compat::unexpected{valid.error()};
  constexpr std::array<double, 5> weights{0.0448, 0.2856, 0.3001, 0.2363, 0.1333};
  if (!workspace.reference_ready) {
    const LumaImage* image = &reference;
    for (std::size_t level = 0; level < weights.size(); ++level) {
      auto stats = visual_metrics_detail::global_statistics(*image, *image, execution);
      if (!stats) return awj::compat::unexpected{stats.error()};
      workspace.reference_statistics[level] = stats->reference;
      if (level + 1 < weights.size() && image->width > 1 && image->height > 1) {
        auto next = visual_metrics_detail::downsample_2x(*image, {}, execution);
        if (!next) return awj::compat::unexpected{next.error()};
        workspace.reference_levels[level] = std::move(*next);
        image = &workspace.reference_levels[level];
      }
    }
    workspace.reference_ready = true;
  }
  const LumaImage* ref_level = &reference;
  const LumaImage* candidate_level = &candidate;
  double value = 1.0;
  for (std::size_t level = 0; level < weights.size(); ++level) {
    auto stats = visual_metrics_detail::global_statistics(*ref_level, *candidate_level,
        execution, &workspace.reference_statistics[level]);
    if (!stats) return awj::compat::unexpected{stats.error()};
    const double ssim = visual_metrics_detail::ssim_from_statistics(*stats);
    value *= std::pow(std::clamp(ssim, 1e-9, 1.0), weights[level]);
    if (level + 1 < weights.size() && ref_level->width > 1 && ref_level->height > 1) {
      auto next = visual_metrics_detail::downsample_2x(
          *candidate_level, std::move(workspace.candidate_levels[level]), execution);
      if (!next) return awj::compat::unexpected{next.error()};
      workspace.candidate_levels[level] = std::move(*next);
      ref_level = &workspace.reference_levels[level];
      candidate_level = &workspace.candidate_levels[level];
    }
  }
  return std::clamp(value, 0.0, 1.0);
}

VisualMetricResult make_visual_metric_result(double raw_gmsd, double raw_ms_ssim) {
  return VisualMetricResult{.raw_gmsd = raw_gmsd,
                            .raw_ms_ssim = raw_ms_ssim,
                            .score = calculate_visual_score(raw_gmsd, raw_ms_ssim)};
}

awj::compat::expected<VisualMetricResult, std::string> calculate_visual_metrics_cpu(
    const LumaImage& reference, const LumaImage& candidate,
    VisualMetricExecution execution = {}) {
  auto gmsd = compute_gmsd(reference, candidate, execution);
  if (!gmsd) {
    return awj::compat::unexpected{gmsd.error()};
  }
  auto ms_ssim = compute_ms_ssim(reference, candidate, execution);
  if (!ms_ssim) {
    return awj::compat::unexpected{ms_ssim.error()};
  }
  return make_visual_metric_result(*gmsd, *ms_ssim);
}

awj::compat::expected<VisualScoreBreakdown, std::string> calculate_visual_score(
    const LumaImage& reference, const LumaImage& candidate,
    VisualMetricExecution execution = {}) {
  auto metrics = calculate_visual_metrics_cpu(reference, candidate, execution);
  if (!metrics) {
    return awj::compat::unexpected{metrics.error()};
  }
  return metrics->score;
}

}  // namespace awj
