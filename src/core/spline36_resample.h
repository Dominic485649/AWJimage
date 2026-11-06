#include "../compat/stdlib_compat.h"
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <stop_token>
#include <string>
#include <vector>

namespace awj::spline36_detail {

struct Sample {
  std::size_t first{};
  std::vector<double> weights;
};

inline std::uint64_t axis_bytes(std::size_t source, std::size_t target) noexcept {
  const auto support = std::min(static_cast<double>(source),
      std::ceil(6 * std::max(1.0, static_cast<double>(source) / target)) + 2);
  const long double bytes = static_cast<long double>(target) * (sizeof(Sample) + support * sizeof(double));
  return bytes >= UINT64_MAX ? UINT64_MAX : static_cast<std::uint64_t>(bytes);
}

inline std::uint64_t scratch_bytes(std::size_t width, std::size_t height,
    std::size_t target_width, std::size_t target_height) noexcept {
  if (!width || !height || !target_width || !target_height) return UINT64_MAX;
  const long double bytes = static_cast<long double>(axis_bytes(width, target_width)) +
      axis_bytes(height, target_height) + static_cast<long double>(target_width) * 4 * sizeof(double) *
      (std::min<std::size_t>(12, height) + 1);
  return bytes >= UINT64_MAX ? UINT64_MAX : static_cast<std::uint64_t>(bytes);
}

inline double kernel(double x) noexcept {
  x = std::abs(x);
  // 半径 3 的分段三次 Spline36；缩小时由系数生成器扩大支撑。
  if (x < 1) return ((13.0 / 11 * x - 453.0 / 209) * x - 3.0 / 209) * x + 1;
  if (x < 2) {
    x -= 1;
    return ((-6.0 / 11 * x + 270.0 / 209) * x - 156.0 / 209) * x;
  }
  if (x < 3) {
    x -= 2;
    return ((1.0 / 11 * x - 45.0 / 209) * x + 26.0 / 209) * x;
  }
  return 0;
}

inline awj::compat::expected<std::vector<Sample>, std::string> make_axis(
    std::size_t source, std::size_t target, awj::compat::stop_token stop) {
  std::vector<Sample> axis;
  axis.reserve(target);
  const auto step = static_cast<double>(source) / static_cast<double>(target);
  const auto support = std::max(1.0, step);
  for (std::size_t x = 0; x < target; ++x) {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    const auto center = (static_cast<double>(x) + 0.5) * step - 0.5;
    const auto first = static_cast<std::size_t>(std::max(0.0, std::ceil(center - 3 * support)));
    const auto last = static_cast<std::size_t>(std::min(static_cast<double>(source - 1), std::floor(center + 3 * support)));
    Sample sample{.first = first};
    sample.weights.reserve(last - first + 1);
    double sum{};
    for (auto p = first; p <= last; ++p) {
      if ((p - first) % 4096 == 0 && stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
      const auto value = kernel((static_cast<double>(p) - center) / support);
      sample.weights.push_back(value); sum += value;
    }
    if (!std::isfinite(sum) || std::abs(sum) < 1e-12)
      return awj::compat::unexpected{"Spline36 滤波系数无效。"};
    for (auto& value : sample.weights) value /= sum;
    axis.push_back(std::move(sample));
  }
  return axis;
}

template <class Read, class Write>
awj::compat::expected<void, std::string> filter(std::size_t width, std::size_t height,
    std::size_t target_width, std::size_t target_height, Read read, Write write,
    awj::compat::stop_token stop) {
  auto horizontal = make_axis(width, target_width, stop);
  if (!horizontal) return awj::compat::unexpected{horizontal.error()};
  auto vertical = make_axis(height, target_height, stop);
  if (!vertical) return awj::compat::unexpected{vertical.error()};
  using Pixel = std::array<double, 4>;
  struct Row { std::size_t id{SIZE_MAX}; std::vector<Pixel> pixels; };
  std::vector<Row> cache(std::min<std::size_t>(12, height));
  for (auto& row : cache) row.pixels.resize(target_width);
  std::vector<Pixel> accumulated(target_width);
  std::size_t replacement{};
  for (std::size_t y = 0; y < target_height; ++y) {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    awj::compat::ranges::fill(accumulated, Pixel{});
    const auto& ys = (*vertical)[y];
    for (std::size_t v = 0; v < ys.weights.size(); ++v) {
      const auto sy = ys.first + v;
      auto found = awj::compat::ranges::find(cache, sy, &Row::id);
      if (found == cache.end()) {
        auto& row = cache[replacement++ % cache.size()];
        row.id = sy;
        for (std::size_t x = 0; x < target_width; ++x) {
          if (x % 256 == 0 && stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
          Pixel sum{};
          const auto& xs = (*horizontal)[x];
          for (std::size_t h = 0; h < xs.weights.size(); ++h) {
            if (h % 4096 == 0 && stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
            const auto rgba = read(xs.first + h, sy);
            for (const auto value : rgba) if (!std::isfinite(value))
              return awj::compat::unexpected{"Spline36 输入含非有限像素。"};
            const auto alpha = std::clamp(static_cast<double>(rgba[3]), 0.0, 1.0);
            for (std::size_t c = 0; c < 3; ++c) sum[c] += rgba[c] * alpha * xs.weights[h];
            sum[3] += alpha * xs.weights[h];
          }
          row.pixels[x] = sum;
        }
        found = cache.begin() + static_cast<std::ptrdiff_t>((replacement - 1) % cache.size());
      }
      for (std::size_t x = 0; x < target_width; ++x)
        for (std::size_t c = 0; c < 4; ++c) accumulated[x][c] += found->pixels[x][c] * ys.weights[v];
    }
    for (std::size_t x = 0; x < target_width; ++x) {
      auto rgba = accumulated[x];
      const auto alpha = rgba[3];
      for (std::size_t c = 0; c < 3; ++c) rgba[c] = alpha > 1e-12 ? rgba[c] / alpha : 0;
      rgba[3] = std::clamp(alpha, 0.0, 1.0);
      if (auto result = write(x, y, rgba); !result) return result;
    }
  }
  return {};
}

}  // namespace awj::spline36_detail
