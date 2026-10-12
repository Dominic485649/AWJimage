#include <algorithm>
#include <cmath>
#include <utility>
#include <cstddef>
#include <cstdio>
#include <string_view>

import awj.image;
import awj.visual_metrics;
import awj.visual_quality;

namespace {

bool nearly_equal(double left, double right, double epsilon = 1e-9) {
  return std::abs(left - right) <= epsilon;
}

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

awj::ImageBuffer make_test_image(std::byte changed_blue) {
  awj::ImagePlane plane{.stride = 8};
  plane.bytes = {
      std::byte{255}, std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{0},   changed_blue,   std::byte{255},
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

awj::ImageBuffer make_high_bit_depth_test_image(int bit_depth) {
  const auto maximum = static_cast<unsigned int>((1u << bit_depth) - 1u);
  awj::ImagePlane plane{.stride = 16};
  const auto append_sample = [&plane](unsigned int sample) {
    plane.bytes.push_back(std::byte{static_cast<unsigned char>(sample & 0xffu)});
    plane.bytes.push_back(std::byte{static_cast<unsigned char>(sample >> 8u)});
  };
  for (unsigned int channel = 0; channel < 4; ++channel) {
    append_sample(maximum);
  }
  append_sample(0);
  append_sample(0);
  append_sample(0);
  append_sample(maximum);
  awj::ImageBuffer image{.width = 2,
                          .height = 1,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = bit_depth};
  image.planes.push_back(std::move(plane));
  return image;
}

double pixel_at_clamped(const awj::LumaImage& image, std::size_t x, std::size_t y) noexcept {
  x = std::min(x, image.width - 1);
  y = std::min(y, image.height - 1);
  return image.pixels[y * image.width + x];
}

double gradient_magnitude(const awj::LumaImage& image, std::size_t x, std::size_t y) noexcept {
  const auto left = x == 0 ? x : x - 1;
  const auto right = std::min(x + 1, image.width - 1);
  const auto top = y == 0 ? y : y - 1;
  const auto bottom = std::min(y + 1, image.height - 1);
  const double dx = pixel_at_clamped(image, right, y) - pixel_at_clamped(image, left, y);
  const double dy = pixel_at_clamped(image, x, bottom) - pixel_at_clamped(image, x, top);
  return std::sqrt(dx * dx + dy * dy);
}

double reference_gmsd(const awj::LumaImage& reference, const awj::LumaImage& candidate) {
  std::size_t count = 0;
  double mean = 0.0, m2 = 0.0;
  for (std::size_t y = 0; y < reference.height; ++y) {
    for (std::size_t x = 0; x < reference.width; ++x) {
      const double ref = gradient_magnitude(reference, x, y);
      const double test = gradient_magnitude(candidate, x, y);
      const double similarity = (2.0 * ref * test + 0.0026) / (ref * ref + test * test + 0.0026);
      const double delta = similarity - mean;
      mean += delta / static_cast<double>(++count);
      m2 += delta * (similarity - mean);
    }
  }
  return std::sqrt(m2 / static_cast<double>(count));
}

}  // namespace

int main() {
  auto reference = awj::make_luma_image(make_test_image(std::byte{255}));
  if (!reference) {
    return fail(reference.error());
  }
  auto identical = awj::make_luma_image(make_test_image(std::byte{255}));
  if (!identical) {
    return fail(identical.error());
  }
  auto changed = awj::make_luma_image(make_test_image(std::byte{0}));
  if (!changed) {
    return fail(changed.error());
  }

  auto identical_gmsd = awj::compute_gmsd(*reference, *identical);
  if (!identical_gmsd || !nearly_equal(*identical_gmsd, 0.0)) {
    return fail("相同图像 GMSD 应为 0。");
  }
  auto identical_ms_ssim = awj::compute_ms_ssim(*reference, *identical);
  if (!identical_ms_ssim || !nearly_equal(*identical_ms_ssim, 1.0)) {
    return fail("相同图像 MS-SSIM 应为 1。");
  }

  auto changed_gmsd = awj::compute_gmsd(*reference, *changed);
  if (!changed_gmsd || *changed_gmsd < 0.0) {
    return fail("变化图像 GMSD 无效。");
  }
  auto changed_ms_ssim = awj::compute_ms_ssim(*reference, *changed);
  if (!changed_ms_ssim || *changed_ms_ssim >= 1.0) {
    return fail("变化图像 MS-SSIM 应低于 1。");
  }

  auto score = awj::calculate_visual_score(*reference, *changed);
  if (!score || score->visual_score < 1.0 || score->visual_score > 99.0) {
    return fail("视觉指标 score 范围无效。");
  }

  for (const int bit_depth : {10, 16}) {
    auto high_bit_depth = awj::make_luma_image(make_high_bit_depth_test_image(bit_depth));
    if (!high_bit_depth || high_bit_depth->pixels.size() != 2 ||
        !nearly_equal(high_bit_depth->pixels[0], 1.0) ||
        !nearly_equal(high_bit_depth->pixels[1], 0.0)) {
      return fail(high_bit_depth ? "高位深图像 luma 归一化无效。"
                                 : high_bit_depth.error());
    }
  }

  for (const auto shape : {std::pair{1u, 1u}, {1u, 9u}, {9u, 1u}, {9u, 11u}, {64u, 32u}}) {
    awj::LumaImage ref{.width = shape.first, .height = shape.second};
    ref.pixels.resize(ref.width * ref.height);
    for (std::size_t i = 0; i < ref.pixels.size(); ++i)
      ref.pixels[i] = float((i * 37 + 11) % 257) / 256.0F;
    awj::CpuVisualMetricWorkspace workspace;
    for (int pass = 0; pass < 3; ++pass) {
      auto test = ref;
      for (std::size_t i = 0; i < test.pixels.size(); ++i)
        test.pixels[i] = std::clamp(test.pixels[i] + float(pass) * 0.01F, 0.0F, 1.0F);
      auto gmsd = awj::compute_gmsd(ref, test);
      if (!gmsd || !nearly_equal(*gmsd, reference_gmsd(ref, test), 1e-8))
        return fail("CPU GMSD changed at a boundary.");
      auto old = awj::compute_ms_ssim(ref, test);
      auto cached = awj::compute_ms_ssim_cached(ref, test, workspace);
      if (!old || !cached || !nearly_equal(*old, *cached, 1e-8))
        return fail("Cached CPU MS-SSIM changed the metric.");
    }
  }
  return 0;
}
