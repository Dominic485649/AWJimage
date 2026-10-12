module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

namespace visual_metrics_detail {

std::expected<std::vector<float>, std::string> make_float_buffer(std::size_t pixel_count,
                                                                 std::string_view context,
                                                                 std::vector<float> buffer = {}) {
  if (pixel_count == 0) {
    return std::unexpected{std::format("{} 输入为空。", context)};
  }
  if (pixel_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
    return std::unexpected{std::format("{} luma buffer 尺寸超过运行时限制。", context)};
  }
  const auto byte_count = pixel_count * sizeof(float);
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{std::format("{} luma buffer 超过当前运行时上限。", context)};
  }
  try {
    buffer.resize(pixel_count);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"luma buffer 内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"luma buffer 尺寸超过运行时限制。"};
  }
  return buffer;
}

}  // namespace visual_metrics_detail

std::expected<LumaImage, std::string> make_luma_image(
    const ImageBuffer& image, LumaImage recycled = {}) {
  if (image.width == 0 || image.height == 0 || image.planes.empty()) {
    return std::unexpected{"输入图像为空，无法计算视觉指标。"};
  }
  if (image.bit_depth != 8 && image.bit_depth != 10 &&
      image.bit_depth != 12 && image.bit_depth != 16) {
    return std::unexpected{"当前视觉指标仅支持 8、10、12 或 16-bit 图像。"};
  }

  if (image.width > std::numeric_limits<std::size_t>::max() / image.height) {
    return std::unexpected{"输入图像尺寸过大，无法计算视觉指标。"};
  }

  const auto pixel_count = image.width * image.height;
  auto pixels = visual_metrics_detail::make_float_buffer(pixel_count, "视觉指标",
                                                        std::move(recycled.pixels));
  if (!pixels) {
    return std::unexpected{pixels.error()};
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
      return std::unexpected{"RGB/RGBA 图像宽度过大，无法计算视觉指标。"};
    }
    const std::size_t min_stride = image.width * channels * bytes_per_sample;
    if (plane.stride < min_stride ||
        plane.stride > std::numeric_limits<std::size_t>::max() / image.height ||
        plane.bytes.size() < plane.stride * image.height) {
      return std::unexpected{"RGB/RGBA 图像 buffer 尺寸无效，无法计算视觉指标。"};
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(plane.bytes.data());
    for (std::size_t y = 0; y < image.height; ++y) {
      const auto* row = bytes + y * plane.stride;
      for (std::size_t x = 0; x < image.width; ++x) {
        const auto* pixel = row + x * channels * bytes_per_sample;
        const double luma_value =
            0.2126 * normalized_sample(pixel, 0) +
            0.7152 * normalized_sample(pixel, 1) +
            0.0722 * normalized_sample(pixel, 2);
        luma.pixels[y * image.width + x] = static_cast<float>(luma_value);
      }
    }
    return luma;
  }

  if (image.pixel_format == PixelFormat::gray) {
    if (image.width > std::numeric_limits<std::size_t>::max() / bytes_per_sample ||
        plane.stride < image.width * bytes_per_sample ||
        plane.stride > std::numeric_limits<std::size_t>::max() / image.height ||
        plane.bytes.size() < plane.stride * image.height) {
      return std::unexpected{"灰度图像 buffer 尺寸无效，无法计算视觉指标。"};
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(plane.bytes.data());
    for (std::size_t y = 0; y < image.height; ++y) {
      const auto* row = bytes + y * plane.stride;
      for (std::size_t x = 0; x < image.width; ++x) {
        luma.pixels[y * image.width + x] =
            static_cast<float>(normalized_sample(row + x * bytes_per_sample, 0));
      }
    }
    return luma;
  }

  return std::unexpected{"当前视觉指标仅支持 RGB、RGBA 或灰度图像。"};
}

namespace visual_metrics_detail {

std::expected<void, std::string> validate_same_shape(const LumaImage& reference,
                                                     const LumaImage& candidate) {
  if (reference.empty() || candidate.empty()) {
    return std::unexpected{"视觉指标输入为空。"};
  }
  if (reference.width != candidate.width || reference.height != candidate.height ||
      reference.pixels.size() != candidate.pixels.size()) {
    return std::unexpected{"视觉指标输入尺寸不一致。"};
  }
  if (reference.width > std::numeric_limits<std::size_t>::max() / reference.height ||
      reference.width * reference.height != reference.pixels.size()) {
    return std::unexpected{"视觉指标输入尺寸无效。"};
  }
  return {};
}

std::expected<LumaImage, std::string> downsample_2x(const LumaImage& source,
                                                  LumaImage recycled = {}) {
  LumaImage result{.width = (source.width + 1) / 2,
                   .height = (source.height + 1) / 2};
  if (result.width == source.width && result.height == source.height) {
    return std::unexpected{"视觉指标降采样尺寸无变化。"};
  }
  if (result.width > std::numeric_limits<std::size_t>::max() / result.height) {
    return std::unexpected{"视觉指标降采样尺寸过大。"};
  }
  auto pixels = make_float_buffer(result.width * result.height, "视觉指标降采样",
                                  std::move(recycled.pixels));
  if (!pixels) {
    return std::unexpected{pixels.error()};
  }
  result.pixels = std::move(*pixels);
  for (std::size_t y = 0; y < result.height; ++y) {
    for (std::size_t x = 0; x < result.width; ++x) {
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
  }
  return result;
}

double ssim_global(const LumaImage& reference, const LumaImage& candidate) noexcept {
  const auto count = static_cast<double>(reference.pixels.size());
  double mean_ref = 0.0;
  double mean_candidate = 0.0;
  for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
    mean_ref += reference.pixels[i];
    mean_candidate += candidate.pixels[i];
  }
  mean_ref /= count;
  mean_candidate /= count;

  double variance_ref = 0.0;
  double variance_candidate = 0.0;
  double covariance = 0.0;
  for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
    const double ref_delta = reference.pixels[i] - mean_ref;
    const double candidate_delta = candidate.pixels[i] - mean_candidate;
    variance_ref += ref_delta * ref_delta;
    variance_candidate += candidate_delta * candidate_delta;
    covariance += ref_delta * candidate_delta;
  }
  variance_ref /= count;
  variance_candidate /= count;
  covariance /= count;

  constexpr double k1 = 0.01;
  constexpr double k2 = 0.03;
  constexpr double c1 = k1 * k1;
  constexpr double c2 = k2 * k2;
  const double numerator = (2.0 * mean_ref * mean_candidate + c1) *
                           (2.0 * covariance + c2);
  const double denominator = (mean_ref * mean_ref + mean_candidate * mean_candidate + c1) *
                             (variance_ref + variance_candidate + c2);
  if (denominator <= 0.0) {
    return 1.0;
  }
  return std::clamp(numerator / denominator, 0.0, 1.0);
}

}  // namespace visual_metrics_detail

std::expected<double, std::string> compute_gmsd(
    const LumaImage& reference,
    const LumaImage& candidate) {
  if (auto valid = visual_metrics_detail::validate_same_shape(reference, candidate); !valid) {
    return std::unexpected{valid.error()};
  }

  constexpr double c = 0.0026;
  std::size_t count = 0;
  double mean = 0.0;
  double m2 = 0.0;
  for (std::size_t y = 0; y < reference.height; ++y) {
    const auto top = (y == 0 ? y : y - 1) * reference.width;
    const auto bottom = std::min(y + 1, reference.height - 1) * reference.width;
    const auto row = y * reference.width;
    for (std::size_t x = 0; x < reference.width; ++x) {
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
      ++count;
      const double delta = similarity - mean;
      mean += delta / static_cast<double>(count);
      const double delta_after = similarity - mean;
      m2 += delta * delta_after;
    }
  }
  if (count == 0) {
    return std::unexpected{"视觉指标输入为空。"};
  }
  return std::sqrt(m2 / static_cast<double>(count));
}

std::expected<double, std::string> compute_ms_ssim(
    const LumaImage& reference,
    const LumaImage& candidate) {
  if (auto valid = visual_metrics_detail::validate_same_shape(reference, candidate); !valid) {
    return std::unexpected{valid.error()};
  }

  constexpr std::array<double, 5> weights{0.0448, 0.2856, 0.3001, 0.2363, 0.1333};
  const LumaImage* ref_level = &reference;
  const LumaImage* candidate_level = &candidate;
  LumaImage ref_owned;
  LumaImage candidate_owned;
  double value = 1.0;
  for (std::size_t level = 0; level < weights.size(); ++level) {
    const double ssim = visual_metrics_detail::ssim_global(*ref_level, *candidate_level);
    value *= std::pow(std::clamp(ssim, 1e-9, 1.0), weights[level]);
    if (level + 1 < weights.size() && ref_level->width > 1 && ref_level->height > 1) {
      auto next_ref = visual_metrics_detail::downsample_2x(*ref_level);
      if (!next_ref) {
        return std::unexpected{next_ref.error()};
      }
      auto next_candidate = visual_metrics_detail::downsample_2x(*candidate_level);
      if (!next_candidate) {
        return std::unexpected{next_candidate.error()};
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
  struct Statistics { double mean{}, variance{}; };
  std::array<LumaImage, 4> reference_levels{}, candidate_levels{};
  std::array<Statistics, 5> reference_statistics{};
  LumaImage candidate_luma{};
  bool reference_ready{};
};

std::expected<double, std::string> compute_ms_ssim_cached(
    const LumaImage& reference, const LumaImage& candidate,
    CpuVisualMetricWorkspace& workspace) {
  if (auto valid = visual_metrics_detail::validate_same_shape(reference, candidate); !valid)
    return std::unexpected{valid.error()};
  constexpr std::array<double, 5> weights{0.0448, 0.2856, 0.3001, 0.2363, 0.1333};
  if (!workspace.reference_ready) {
    const LumaImage* image = &reference;
    for (std::size_t level = 0; level < weights.size(); ++level) {
      auto& stats = workspace.reference_statistics[level];
      const double count = static_cast<double>(image->pixels.size());
      stats = {};
      for (float pixel : image->pixels) stats.mean += pixel;
      stats.mean /= count;
      for (float pixel : image->pixels) {
        const double delta = pixel - stats.mean;
        stats.variance += delta * delta;
      }
      stats.variance /= count;
      if (level + 1 < weights.size() && image->width > 1 && image->height > 1) {
        auto next = visual_metrics_detail::downsample_2x(*image);
        if (!next) return std::unexpected{next.error()};
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
    const auto stats = workspace.reference_statistics[level];
    const double count = static_cast<double>(candidate_level->pixels.size());
    double mean = 0.0;
    for (float pixel : candidate_level->pixels) mean += pixel;
    mean /= count;
    double variance = 0.0, covariance = 0.0;
    for (std::size_t i = 0; i < candidate_level->pixels.size(); ++i) {
      const double delta = candidate_level->pixels[i] - mean;
      variance += delta * delta;
      covariance += (ref_level->pixels[i] - stats.mean) * delta;
    }
    variance /= count;
    covariance /= count;
    constexpr double c1 = 0.01 * 0.01, c2 = 0.03 * 0.03;
    const double numerator = (2.0 * stats.mean * mean + c1) * (2.0 * covariance + c2);
    const double denominator = (stats.mean * stats.mean + mean * mean + c1) *
                                (stats.variance + variance + c2);
    const double ssim = denominator <= 0.0 ? 1.0 : std::clamp(numerator / denominator, 0.0, 1.0);
    value *= std::pow(std::clamp(ssim, 1e-9, 1.0), weights[level]);
    if (level + 1 < weights.size() && ref_level->width > 1 && ref_level->height > 1) {
      auto next = visual_metrics_detail::downsample_2x(
          *candidate_level, std::move(workspace.candidate_levels[level]));
      if (!next) return std::unexpected{next.error()};
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

std::expected<VisualMetricResult, std::string> calculate_visual_metrics_cpu(
    const LumaImage& reference,
    const LumaImage& candidate) {
  auto gmsd = compute_gmsd(reference, candidate);
  if (!gmsd) {
    return std::unexpected{gmsd.error()};
  }
  auto ms_ssim = compute_ms_ssim(reference, candidate);
  if (!ms_ssim) {
    return std::unexpected{ms_ssim.error()};
  }
  return make_visual_metric_result(*gmsd, *ms_ssim);
}

std::expected<VisualScoreBreakdown, std::string> calculate_visual_score(
    const LumaImage& reference,
    const LumaImage& candidate) {
  auto metrics = calculate_visual_metrics_cpu(reference, candidate);
  if (!metrics) {
    return std::unexpected{metrics.error()};
  }
  return metrics->score;
}

}  // namespace awj
