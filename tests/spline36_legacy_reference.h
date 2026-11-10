#pragma once
#include "../src/core/spline36_resample.h"

namespace awj::test {

// 冻结缓存优化前（fe3e788）的实现，用于逐像素精确对照。
// 故意保留旧的 12 行 FIFO 和重复源读取；geometry_tests 另有独立展开多项式的二维参考。
template <class Read, class Write>
awj::compat::expected<void, std::string> legacy_spline36(std::size_t width, std::size_t height,
    std::size_t target_width, std::size_t target_height, Read read, Write write) {
  auto horizontal = spline36_detail::make_axis(width, target_width, {});
  if (!horizontal) return awj::compat::unexpected{horizontal.error()};
  auto vertical = spline36_detail::make_axis(height, target_height, {});
  if (!vertical) return awj::compat::unexpected{vertical.error()};
  using Pixel = std::array<double, 4>;
  struct Row { std::size_t id{SIZE_MAX}; std::vector<Pixel> pixels; };
  std::vector<Row> cache(std::min<std::size_t>(12, height));
  for (auto& row : cache) row.pixels.resize(target_width);
  std::vector<Pixel> accumulated(target_width);
  std::size_t replacement{};
  for (std::size_t y = 0; y < target_height; ++y) {
    awj::compat::ranges::fill(accumulated, Pixel{});
    const auto& ys = (*vertical)[y];
    for (std::size_t v = 0; v < ys.weights.size(); ++v) {
      const auto sy = ys.first + v;
      auto found = awj::compat::ranges::find(cache, sy, &Row::id);
      if (found == cache.end()) {
        auto& row = cache[replacement++ % cache.size()];
        row.id = sy;
        for (std::size_t x = 0; x < target_width; ++x) {
          Pixel sum{};
          const auto& xs = (*horizontal)[x];
          for (std::size_t h = 0; h < xs.weights.size(); ++h) {
            const auto rgba = read(xs.first + h, sy);
            for (const auto value : rgba) if (!std::isfinite(value))
              return awj::compat::unexpected{"Legacy Spline36 non-finite input"};
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

} // namespace awj::test
