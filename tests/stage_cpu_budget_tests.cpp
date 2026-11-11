#include "../src/compat/stdlib_compat.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

import awj.animation;
import awj.animation_decoder;
import awj.webp_codec;
import awj.codec;
import awj.config;
import awj.core;
import awj.image;
import awj.native_backend;
import awj.resource_planner;

namespace {
constexpr std::uint64_t mib = 1024ull * 1024;

int fail(const std::string& message) {
  std::fprintf(stderr, "%s\n", message.c_str());
  return 1;
}

awj::ImageBuffer image(std::size_t width = 1024, std::size_t height = 512) {
  awj::ImageBuffer result{.width = width, .height = height,
      .pixel_format = awj::PixelFormat::rgba, .alpha_mode = awj::AlphaMode::straight,
      .bit_depth = 8};
  awj::ImagePlane plane{.stride = result.width * 4};
  plane.bytes.assign(plane.stride * result.height, std::byte{90});
  for (std::size_t i = 3; i < plane.bytes.size(); i += 4) plane.bytes[i] = std::byte{255};
  result.planes.push_back(std::move(plane));
  result.source_info = awj::ImageSourceInfo{};
  result.source_info->pixel_format = awj::PixelFormat::rgba;
  result.source_info->bit_depth = 8;
  result.source_info->color_primaries = 1;
  result.source_info->transfer_characteristics = 13;
  result.source_info->matrix_coefficients = 0;
  result.source_info->color_range = 1;
  return result;
}

class BudgetFrames final : public awj::AnimationReader {
 public:
  BudgetFrames() : pixels_{image(64, 64)} {
    info_.width = 64;
    info_.height = 64;
    info_.durations.assign(2, awj::FrameDuration{1, 10});
    info_.decoder_id = "stage-budget-fixture";
  }
  const awj::AnimationInfo& info() const noexcept override { return info_; }
  const awj::ImageBuffer& frame() const noexcept override { return pixels_; }
  std::uint64_t memory_usage_bytes() const noexcept override { return pixels_.planes[0].bytes.capacity(); }
  awj::compat::expected<bool, std::string> next(awj::compat::stop_token stop = {}) override {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (index_ == info_.durations.size()) return false;
    for (std::size_t i = 0; i < pixels_.planes[0].bytes.size(); i += 4)
      pixels_.planes[0].bytes[i] = std::byte{static_cast<unsigned char>(index_ ? 190 : 30)};
    ++index_;
    return true;
  }
 private:
  awj::AnimationInfo info_;
  awj::ImageBuffer pixels_;
  std::size_t index_{};
};

int animation_budget_checks(awj::FileLogger& logger) {
  const auto root = std::filesystem::path{AWJ_TEST_RESULTS_ROOT} / "stage-animation-budget";
  std::filesystem::create_directories(root);
  const auto input = root / "input.webp";
  BudgetFrames fixture;
  if (!fixture.next().value_or(false)) return fail("无法建立阶段预算动画输入。");
  awj::NativeEncodeSettings fixture_settings{};
  fixture_settings.quality = 100;
  fixture_settings.speed = 10;
  fixture_settings.resources = {.file_parallelism = 1, .encoder_threads_per_file = 1,
      .global_thread_budget = 1, .memory_limit_bytes = 256 * mib};
  auto bytes = awj::encode_webp_animation(fixture, fixture_settings);
  if (!bytes) return fail(bytes.error());
  {
    std::ofstream output{input, std::ios::binary | std::ios::trunc};
    const auto& data = bytes->encoded.bytes;
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!output) return fail("无法写入阶段预算动画输入。");
  }
  const awj::ImageFile source{.path = input, .bytes = std::filesystem::file_size(input)};
  std::vector<std::byte> reference;
  for (const bool refuse_growth : {false, true}) {
    auto cfg = awj::default_app_config();
    cfg.input_path = input;
    cfg.output_dir = root / (refuse_growth ? "fallback" : "enlarged");
    cfg.output_format = awj::OutputFormat::webp;
    cfg.quality = 100;
    cfg.speed = 10;
    cfg.max_jobs = 8;
    cfg.memory_limit_bytes = 256 * mib;
    const awj::ResourcePlan plan{.file_parallelism = 1, .encoder_threads_per_file = 8,
        .global_thread_budget = 8, .memory_limit_bytes = cfg.memory_limit_bytes,
        .memory_reservation_bytes = mib};
    int last_grant{};
    int minimum_grant = 8;
    int denied{};
    int growth_requests{};
    std::size_t frames{};
    bool invalid_grant{};
    auto resize = [&](std::uint64_t required) {
      if (required > cfg.memory_limit_bytes) return false;
      // 只拒绝 reader 的可选线程工作区；后续编码精化仍有足够空间。
      if (required > mib && required < 16 * mib) {
        ++growth_requests;
        if (refuse_growth) { ++denied; return false; }
      }
      return true;
    };
    auto cpu = [&](int demand) {
      last_grant = std::clamp(demand, 0, 8);
      if (last_grant) minimum_grant = std::min(minimum_grant, last_grant);
      return last_grant;
    };
    cfg.frame_progress = [&](std::size_t, std::size_t, std::size_t) {
      ++frames;
      if (refuse_growth ? last_grant != 1 : last_grant != 8) invalid_grant = true;
    };
    const auto result = awj::NativeBackend{cfg, logger, plan, resize, cpu}.encode(source);
    if (!result.ok || result.canceled || invalid_grant || frames != 2 || !growth_requests ||
        (refuse_growth && (!denied || minimum_grant != 1))) {
      std::fprintf(stderr, "refuse_growth=%d growth_requests=%d denied=%d last_grant=%d minimum_grant=%d frames=%zu invalid_grant=%d canceled=%d\n",
          refuse_growth, growth_requests, denied, last_grant, minimum_grant, frames, invalid_grant, result.canceled);
      return fail(result.ok ? "动画重开/降线程后的授权未保持到同步序列结束。" : result.message);
    }
    auto reopened = awj::open_animation(result.output_path, 0);
    if (!reopened || !*reopened || (*reopened)->info().durations.size() != 2 || !(*reopened)->next().value_or(false))
      return fail(reopened ? "阶段预约调整丢失了动画序列。" : reopened.error());
    const auto& pixels = (*reopened)->frame().planes[0].bytes;
    if (reference.empty()) reference = pixels;
    else if (pixels != reference) return fail("动画预约回退改变了首帧像素。");
  }
  auto cfg = awj::default_app_config();
  cfg.input_path = input;
  cfg.output_dir = root / "canceled-write";
  cfg.output_format = awj::OutputFormat::webp;
  cfg.quality = 100; cfg.speed = 10; cfg.max_jobs = 8;
  cfg.memory_limit_bytes = 256 * mib;
  const awj::ResourcePlan plan{.file_parallelism = 1, .encoder_threads_per_file = 8,
      .global_thread_budget = 8, .memory_limit_bytes = cfg.memory_limit_bytes,
      .memory_reservation_bytes = cfg.memory_limit_bytes};
  std::size_t frames{};
  cfg.frame_progress = [&](std::size_t, std::size_t, std::size_t) { ++frames; };
  const auto canceled = awj::NativeBackend{cfg, logger, plan, {},
      [](int demand) { return demand == 1 ? 0 : std::min(demand, 8); }}.encode(source);
  if (canceled.ok || !canceled.canceled || frames != 2 || canceled.write_seconds != -1.0 ||
      std::filesystem::exists(canceled.output_path))
    return fail("写出前 CPU 授权返回 0 后仍提交了动画输出或报告成功。");
  return 0;
}
}

int main() {
  auto cfg = awj::default_app_config();
  // 无损 WebP 可记录实际获准的编码预算；PNG 编码器始终单线程。
  cfg.output_format = awj::OutputFormat::webp;
  cfg.quality = 100;
  cfg.speed = 10;
  cfg.max_jobs = 8;
  cfg.memory_limit_bytes = 256 * mib;
  // 旧 manual 模式会把比例限制在 100%；本用例显式请求 upscale，确保确有放大。
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::upscale, .scale_percent = 200};
  const auto geometry = awj::plan_image_geometry(1024, 512, cfg);
  if (!geometry || !geometry->resamples() || geometry->canvas_width != 2048 || geometry->canvas_height != 1024)
    return fail(geometry ? "阶段预算 fixture 未建立真实放大计划。" : geometry.error());
  awj::FileLogger logger{std::filesystem::path{AWJ_TEST_RESULTS_ROOT} / "stage-cpu-budget", false};
  const awj::ResourcePlan resources{.file_parallelism = 1, .encoder_threads_per_file = 8,
      .global_thread_budget = 8, .memory_limit_bytes = cfg.memory_limit_bytes,
      .memory_reservation_bytes = cfg.memory_limit_bytes};
  int requests = 0;
  std::uint64_t reserved = cfg.memory_limit_bytes;
  auto resize_memory = [&](std::uint64_t bytes) {
    if (bytes > cfg.memory_limit_bytes) return false;
    reserved = bytes;
    return true;
  };
  const auto grant = [&](int demand) {
    if (demand <= 0) return 0;
    ++requests;
    return std::min(demand, requests == 1 ? 2 : 4);
  };
  awj::NativeBackend backend{cfg, logger, resources, resize_memory, grant};
  auto encoded = backend.encode_memory(awj::ImageDecodeResult{.image = image()});
  if (!encoded) return fail(encoded.error());
  // encoder_threads 表示每文件获准的配置预算，不是实测的 libwebp 工作线程数。
  if (requests < 2 || encoded->encoded.bytes.empty() || reserved > cfg.memory_limit_bytes ||
      encoded->diagnostics.timing.resize_workers != 2 || encoded->diagnostics.timing.resize_seconds <= 0 ||
      encoded->diagnostics.encoder_threads != 4) {
    std::fprintf(stderr, "requests=%d bytes=%zu reserved=%llu resize_workers=%d resize_seconds=%.9f encoder_budget=%d\n",
        requests, encoded->encoded.bytes.size(), static_cast<unsigned long long>(reserved),
        encoded->diagnostics.timing.resize_workers, encoded->diagnostics.timing.resize_seconds,
        encoded->diagnostics.encoder_threads);
    return fail("阶段 CPU 授权未进入实际缩放，或编码精化忽略了后续授权。");
  }
  auto restored = awj::WebPImageDecoder{}.decode_memory(encoded->encoded.bytes, "stage-budget.webp");
  if (!restored || restored->image.width != 2048 || restored->image.height != 1024)
    return fail(restored ? "动态阶段授权改变了输出尺寸。" : restored.error());
  const auto reference = restored->image.planes.front().bytes;
  requests = 0;
  auto repeated = backend.encode_memory(awj::ImageDecodeResult{.image = image()});
  if (!repeated || requests < 2 || repeated->diagnostics.timing.resize_workers != 2 ||
      repeated->diagnostics.encoder_threads != 4)
    return fail(repeated ? "重复调用继承了上次操作的阶段快照。" : repeated.error());
  auto second = awj::WebPImageDecoder{}.decode_memory(repeated->encoded.bytes, "stage-budget-repeat.webp");
  if (!second || second->image.planes.front().bytes != reference)
    return fail(second ? "阶段授权改变了实际输出像素。" : second.error());

  requests = 0;
  awj::NativeBackend canceled{cfg, logger, resources, resize_memory, [&](int) { ++requests; return 0; }};
  if (canceled.encode_memory(awj::ImageDecodeResult{.image = image()}) || requests != 1)
    return fail("未取得 CPU 授权时仍进入图像处理。");
  return animation_budget_checks(logger);
}
