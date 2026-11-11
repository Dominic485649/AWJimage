#include "../src/compat/stdlib_compat.h"
#include "../src/core/parallel_rows.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

double reference_ssim(const awj::LumaImage& reference, const awj::LumaImage& candidate) {
  double ref_mean{}, mean{};
  const auto count = static_cast<double>(reference.pixels.size());
  for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
    ref_mean += reference.pixels[i]; mean += candidate.pixels[i];
  }
  ref_mean /= count; mean /= count;
  double ref_var{}, var{}, cov{};
  for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
    const double a = reference.pixels[i] - ref_mean, b = candidate.pixels[i] - mean;
    ref_var += a * a; var += b * b; cov += a * b;
  }
  ref_var /= count; var /= count; cov /= count;
  constexpr double c1 = 0.0001, c2 = 0.0009;
  const double denominator = (ref_mean * ref_mean + mean * mean + c1) * (ref_var + var + c2);
  return denominator <= 0 ? 1 : std::clamp((2 * ref_mean * mean + c1) * (2 * cov + c2) / denominator, 0.0, 1.0);
}

awj::LumaImage reference_downsample(const awj::LumaImage& source) {
  awj::LumaImage result{.width = (source.width + 1) / 2, .height = (source.height + 1) / 2};
  result.pixels.resize(result.width * result.height);
  for (std::size_t y = 0; y < result.height; ++y) for (std::size_t x = 0; x < result.width; ++x) {
    double sum{}, count{};
    for (std::size_t dy = 0; dy < 2 && y * 2 + dy < source.height; ++dy)
      for (std::size_t dx = 0; dx < 2 && x * 2 + dx < source.width; ++dx) {
        sum += source.pixels[(y * 2 + dy) * source.width + x * 2 + dx]; ++count;
      }
    result.pixels[y * result.width + x] = static_cast<float>(sum / count);
  }
  return result;
}

double reference_ms_ssim(awj::LumaImage reference, awj::LumaImage candidate) {
  constexpr std::array weights{0.0448, 0.2856, 0.3001, 0.2363, 0.1333};
  double value = 1;
  for (std::size_t level = 0; level < weights.size(); ++level) {
    value *= std::pow(std::clamp(reference_ssim(reference, candidate), 1e-9, 1.0), weights[level]);
    if (level + 1 < weights.size() && reference.width > 1 && reference.height > 1) {
      reference = reference_downsample(reference); candidate = reference_downsample(candidate);
    }
  }
  return std::clamp(value, 0.0, 1.0);
}

struct FailingLauncher {
  int allowed{};
  bool allocation_failure{};
  mutable int started{};
  template <class Work>
  awj::compat::scoped_worker launch(Work&& work) const {
    if (started == allowed) {
      if (allocation_failure) throw std::bad_alloc{};
      throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    auto thread = awj::compat::scoped_worker(std::forward<Work>(work));
    ++started;
    return thread;
  }
};

int check_row_lifecycle() {
  using namespace awj::parallel_rows_detail;
  const RunOptions options{.worker_budget = 4, .scratch_limit_bytes = 128 * 1024, .minimum_work_per_worker = 1};
  if (worker_count(1000, 1000, {.worker_budget = 128, .scratch_limit_bytes = UINT64_MAX}) != 61 ||
      worker_count(1000, 1000, {.worker_budget = 128, .scratch_limit_bytes = 0}) != 1 ||
      worker_count(2, 1, options) != 2 || worker_count(1000, 1000,
          {.worker_budget = 128, .scratch_limit_bytes = UINT64_MAX, .minimum_work_per_worker = 1}) != 128)
    return fail("Row worker selection ignored workload, memory or N.");
  const awj::memory_policy::Policy policy{123'456, true, true};
  const awj::memory_policy::Scope scope{policy};
  for (const int allowed : {0, 1, 2, 3}) for (const bool allocation_failure : {false, true}) {
    std::array<std::atomic<unsigned>, 513> visits{};
    std::atomic<unsigned> active{}, peak{};
    std::atomic<bool> wrong_policy{};
    FailingLauncher launcher{.allowed = allowed, .allocation_failure = allocation_failure};
    auto result = for_each(visits.size(), 1000, options, {},
        [&](std::size_t row) -> awj::compat::expected<void, std::string> {
          const auto current = active.fetch_add(1) + 1;
          auto old = peak.load(); while (old < current && !peak.compare_exchange_weak(old, current)) {}
          if (awj::memory_policy::current() != policy) wrong_policy.store(true);
          visits[row].fetch_add(1);
          active.fetch_sub(1);
          return {};
        }, launcher);
    if (!result || active.load() || peak.load() > 4 || wrong_policy.load() || launcher.started != allowed)
      return fail("Partial row worker startup failed to join or propagate memory policy.");
    for (const auto& count : visits) if (count.load() != 1) return fail("Row startup fallback omitted or duplicated a row.");
  }
  FailingLauncher serial{0};
  auto low_memory = options; low_memory.scratch_limit_bytes = 0;
  unsigned rows{};
  if (!for_each(513, 1000, low_memory, {}, [&](std::size_t) -> awj::compat::expected<void, std::string> { ++rows; return {}; }, serial) ||
      rows != 513 || serial.started) return fail("Zero scratch did not fall back to complete serial rows.");

  awj::compat::stop_source during;
  std::atomic<unsigned> entered{}, completed{}, active{};
  auto canceled = for_each(513, 1000, options, during.get_token(),
      [&](std::size_t) -> awj::compat::expected<void, std::string> {
        active.fetch_add(1); entered.fetch_add(1);
        during.request_stop(); completed.fetch_add(1); active.fetch_sub(1);
        return {};
      });
  if (canceled || !entered.load() || entered.load() != completed.load() || active.load())
    return fail("Canceled row API returned before all callbacks joined.");
  auto failed = for_each(513, 1000, options, {},
      [&](std::size_t row) -> awj::compat::expected<void, std::string> {
        if (row == 0) return awj::compat::unexpected{"injected row failure"};
        return {};
      });
  if (failed || failed.error() != "injected row failure") return fail("Worker failure message was lost.");
  auto thrown = for_each(513, 1000, options, {},
      [&](std::size_t row) -> awj::compat::expected<void, std::string> {
        if (row == 0) throw std::runtime_error("injected callback exception");
        return {};
      });
  if (thrown) return fail("A worker exception escaped the joined failure boundary.");
  struct Sum { double value{}; std::size_t count{}; };
  constexpr auto pixels = (reduction_max_blocks + 1) * reduction_block_pixels + 13;
  const auto calculate = [](std::size_t first, std::size_t end, Sum& state) -> awj::compat::expected<void, std::string> {
    for (auto i = first; i < end; ++i) { state.value += i % 7 ? 1e-6 : 1e6; ++state.count; }
    return {};
  };
  const auto merge = [](Sum& total, const Sum& block) noexcept { total.value += block.value; total.count += block.count; };
  auto sum_serial = reduce<Sum>(pixels, low_memory, {}, calculate, merge);
  auto sum_threaded = reduce<Sum>(pixels, options, {}, calculate, merge);
  if (!sum_serial || !sum_threaded || sum_serial->count != pixels || sum_threaded->count != pixels ||
      sum_serial->value != sum_threaded->value)
    return fail("Fixed reductions changed order across a large or partial logical partition.");
  return 0;
}

awj::ImageBuffer parallel_luma_fixture(int depth, awj::PixelFormat format) {
  constexpr std::size_t width = 257, height = 513;
  const auto channels = format == awj::PixelFormat::gray ? 1u : format == awj::PixelFormat::rgb ? 3u : 4u;
  const auto bytes = depth > 8 ? 2u : 1u;
  const auto maximum = (1u << depth) - 1;
  awj::ImageBuffer image{.width = width, .height = height, .pixel_format = format,
      .alpha_mode = format == awj::PixelFormat::rgba ? awj::AlphaMode::straight : awj::AlphaMode::none, .bit_depth = depth};
  awj::ImagePlane plane{.stride = width * channels * bytes + 16};
  plane.bytes.resize(plane.stride * height, std::byte{0xa5});
  for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x)
    for (std::size_t c = 0; c < channels; ++c) {
      const auto value = static_cast<std::uint16_t>((x * 157 + y * 269 + c * 43) & maximum);
      auto* destination = plane.bytes.data() + y * plane.stride + (x * channels + c) * bytes;
      if (bytes == 1) *destination = std::byte{static_cast<unsigned char>(value)};
      else std::memcpy(destination, &value, 2);
    }
  image.planes.push_back(std::move(plane));
  return image;
}

int check_parallel_metrics() {
  const awj::VisualMetricExecution parallel{.thread_budget = 4, .scratch_limit_bytes = 128 * 1024};
  auto low_memory = parallel; low_memory.scratch_limit_bytes = 0;
  for (const int depth : {8, 10, 12, 16}) for (const auto format :
      {awj::PixelFormat::gray, awj::PixelFormat::rgb, awj::PixelFormat::rgba}) {
    const auto image = parallel_luma_fixture(depth, format);
    auto serial = awj::make_luma_image(image);
    auto threaded = awj::make_luma_image(image, {}, parallel);
    if (!serial || !threaded || serial->pixels != threaded->pixels)
      return fail("Threaded luma changed padded 8/10/12/16-bit samples or alpha handling.");
  }
  for (const auto shape : {std::pair{1u, 65537u}, {65537u, 1u}, {257u, 513u}}) {
    awj::LumaImage ref{.width = shape.first, .height = shape.second};
    ref.pixels.resize(ref.width * ref.height);
    for (std::size_t i = 0; i < ref.pixels.size(); ++i) ref.pixels[i] = static_cast<float>((i * 271 + i / 17) % 4096) / 4095.0F;
    awj::CpuVisualMetricWorkspace serial_workspace, threaded_workspace;
    for (const float delta : {0.0F, 0.001F, 0.02F}) {
      auto test = ref;
      for (std::size_t i = 0; i < test.pixels.size(); ++i)
        test.pixels[i] = std::clamp(test.pixels[i] + (i % 2 ? delta : -delta), 0.0F, 1.0F);
      auto serial = awj::calculate_visual_metrics_cpu(ref, test);
      auto threaded = awj::calculate_visual_metrics_cpu(ref, test, parallel);
      auto bounded = awj::calculate_visual_metrics_cpu(ref, test, low_memory);
      auto cached = awj::compute_ms_ssim_cached(ref, test, threaded_workspace, parallel);
      auto serial_cached = awj::compute_ms_ssim_cached(ref, test, serial_workspace);
      if (!serial || !threaded || !bounded || !cached || !serial_cached ||
          serial->raw_gmsd != threaded->raw_gmsd || serial->raw_gmsd != bounded->raw_gmsd ||
          serial->raw_ms_ssim != threaded->raw_ms_ssim || serial->raw_ms_ssim != bounded->raw_ms_ssim ||
          !nearly_equal(*cached, serial->raw_ms_ssim, 1e-12) || !nearly_equal(*serial_cached, *cached, 1e-12) ||
          serial->score.visual_score != threaded->score.visual_score ||
          !nearly_equal(serial->raw_gmsd, reference_gmsd(ref, test), 1e-12) ||
          !nearly_equal(serial->raw_ms_ssim, reference_ms_ssim(ref, test), 1e-12))
        return fail("Fixed-block metrics changed with N, cache, memory fallback or legacy scalar oracle.");
    }
    awj::compat::stop_source stop; stop.request_stop();
    auto canceled = parallel; canceled.stop = stop.get_token();
    awj::CpuVisualMetricWorkspace empty_workspace;
    if (awj::compute_gmsd(ref, ref, canceled) || awj::compute_ms_ssim(ref, ref, canceled) ||
        awj::compute_ms_ssim_cached(ref, ref, empty_workspace, canceled) || empty_workspace.reference_ready ||
        awj::calculate_visual_metrics_cpu(ref, ref, canceled) || awj::calculate_visual_score(ref, ref, canceled) ||
        awj::visual_metrics_detail::downsample_2x(ref, {}, canceled) ||
        awj::make_luma_image(parallel_luma_fixture(16, awj::PixelFormat::rgba), {}, canceled))
      return fail("Visual metric/luma/pyramid cancellation did not propagate.");
  }
  return 0;
}

}  // namespace

int main() {
  if (const int result = check_row_lifecycle()) return result;
  if (const int result = check_parallel_metrics()) return result;
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

  for (const int bit_depth : {10, 12, 16}) {
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
