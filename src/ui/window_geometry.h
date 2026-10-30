#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace awj::studio {

// 配置保存逻辑像素；先限幅再取整，避免极小比例或极大物理尺寸溢出。
inline int logical_window_extent(std::uint32_t physical, double reported_scale,
                                 int fallback, int minimum, int maximum) noexcept {
  const double scale = std::isfinite(reported_scale) && reported_scale > 0
                           ? reported_scale : 1.0;
  const double logical = physical == 0 ? static_cast<double>(fallback)
                                      : static_cast<double>(physical) / scale;
  return static_cast<int>(std::lround(std::clamp(
      logical, static_cast<double>(minimum), static_cast<double>(maximum))));
}

}  // namespace awj::studio
