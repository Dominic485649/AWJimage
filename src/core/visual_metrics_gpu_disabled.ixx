module;
#include "../compat/expected_compat.h"
#include <string>
export module awj.visual_metrics_gpu;
import awj.image;
import awj.visual_metrics;

export namespace awj {
struct AcceleratedVisualMetricTiming {
  double luma_seconds{}, gmsd_seconds{}, ms_ssim_seconds{};
};
class AcceleratedVisualMetricSession {
 public:
  AcceleratedVisualMetricSession() = default;
  AcceleratedVisualMetricSession(const AcceleratedVisualMetricSession&) = delete;
  AcceleratedVisualMetricSession& operator=(const AcceleratedVisualMetricSession&) = delete;
  AcceleratedVisualMetricSession(AcceleratedVisualMetricSession&&) = default;
  AcceleratedVisualMetricSession& operator=(AcceleratedVisualMetricSession&&) = default;
  static awj::compat::expected<AcceleratedVisualMetricSession, std::string> create(const ImageBuffer&) {
    return awj::compat::unexpected{"此构建未启用 GPU 视觉质量后端。"};
  }
  awj::compat::expected<VisualMetricResult, std::string> calculate_candidate_metrics(
      const ImageBuffer&, AcceleratedVisualMetricTiming* = nullptr) {
    return awj::compat::unexpected{"此构建未启用 GPU 视觉质量后端。"};
  }
};
awj::compat::expected<LumaImage, std::string> make_luma_image_accelerated(const ImageBuffer& image) {
  return make_luma_image(image);
}
awj::compat::expected<double, std::string> compute_gmsd_accelerated(const LumaImage& a, const LumaImage& b) {
  return compute_gmsd(a, b);
}
awj::compat::expected<double, std::string> compute_ms_ssim_accelerated(const LumaImage& a, const LumaImage& b) {
  return compute_ms_ssim(a, b);
}
awj::compat::expected<VisualMetricResult, std::string> calculate_visual_metrics_accelerated(const LumaImage& a, const LumaImage& b) {
  return calculate_visual_metrics_cpu(a, b);
}
}
