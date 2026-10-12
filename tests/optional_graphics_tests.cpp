#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

import awj.image;
import awj.visual_metrics_gpu;

awj::ImageBuffer image(std::size_t width, std::size_t height) {
  awj::ImagePlane plane{.stride = width * 4};
  plane.bytes.assign(width * height * 4, std::byte{255});
  awj::ImageBuffer result{.width = width, .height = height,
      .pixel_format = awj::PixelFormat::rgba, .alpha_mode = awj::AlphaMode::straight,
      .bit_depth = 8};
  result.planes.push_back(std::move(plane));
  return result;
}

int main(int argc, char** argv) {
  const bool expect_unavailable = argc == 2 && std::string_view{argv[1]} == "--expect-unavailable";
  const auto small = image(16, 16);
  if (awj::AcceleratedVisualMetricSession::create(small)) return 1;
  auto luma = awj::make_luma_image_accelerated(small);
  if (!luma || luma->pixels.size() != 256) return 2;
  std::puts("Small image CPU fallback passed.");

  const auto large = image(1024, 1024);
  auto first = awj::AcceleratedVisualMetricSession::create(large);
  if (expect_unavailable && first) return 3;
  const bool available = first.has_value();
  const std::string error = available ? "" : first.error();
  if (!available) std::printf("GPU unavailable: %s\n", error.c_str());
  std::atomic<bool> failed{};
  {
    std::vector<std::jthread> threads;
    for (int thread = 0; thread < 4; ++thread) threads.emplace_back([&] {
      for (int attempt = 0; attempt < 2; ++attempt) {
        auto session = awj::AcceleratedVisualMetricSession::create(large);
        if (session.has_value() != available) { failed = true; return; }
        if (!session) {
          if (session.error() != error) failed = true;
          continue;
        }
        auto metrics = session->calculate_candidate_metrics(large);
        if (!metrics || !std::isfinite(metrics->raw_gmsd) || !std::isfinite(metrics->raw_ms_ssim) ||
            std::abs(metrics->raw_gmsd) > 1e-6 ||
            std::abs(metrics->raw_ms_ssim - 1.0) > 1e-6) failed = true;
      }
    });
  }
  if (failed) return 4;
  std::puts("Repeated and concurrent GPU capability checks passed.");
  return 0;
}