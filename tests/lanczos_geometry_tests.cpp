#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

import awj.config;
import awj.image;
import awj.pixel_conversion;
import awj.resource_planner;

namespace {
int fail(const char* message) { std::fprintf(stderr, "%s\n", message); return 1; }

awj::ImageBuffer fixture() {
  awj::ImageBuffer image{.width = 7, .height = 5, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 8};
  awj::ImagePlane plane{.stride = 28};
  for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < 7; ++x) {
    plane.bytes.push_back(std::byte{static_cast<unsigned char>((x * 53 + y * 17) % 256)});
    plane.bytes.push_back(std::byte{static_cast<unsigned char>((x * 11 + y * 79) % 256)});
    plane.bytes.push_back(std::byte{static_cast<unsigned char>((x * 31 + y * 3) % 256)});
    plane.bytes.push_back(std::byte{static_cast<unsigned char>((x * 19 + y * 43) % 256)});
  }
  image.planes.push_back(std::move(plane));
  return image;
}

double sinc(double value) {
  return std::abs(value) < 1e-12 ? 1 : std::sin(std::numbers::pi * value) / (std::numbers::pi * value);
}

// 独立二维标量参考，不复用生产内核、系数表或横纵分离计算。
std::array<int, 4> reference(const awj::ImageBuffer& image, std::size_t width, std::size_t height,
    std::size_t x, std::size_t y) {
  const auto sx = std::max(1.0, static_cast<double>(image.width) / width);
  const auto sy = std::max(1.0, static_cast<double>(image.height) / height);
  const auto cx = (x + 0.5) * image.width / width - 0.5;
  const auto cy = (y + 0.5) * image.height / height - 0.5;
  std::array<double, 4> sum{};
  double weights{};
  for (std::size_t py = 0; py < image.height; ++py) for (std::size_t px = 0; px < image.width; ++px) {
    const auto dx = (px - cx) / sx, dy = (py - cy) / sy;
    if (std::abs(dx) >= 3 || std::abs(dy) >= 3) continue;
    const auto w = sinc(dx) * sinc(dx / 3) * sinc(dy) * sinc(dy / 3);
    const auto* pixel = image.planes[0].bytes.data() + py * image.planes[0].stride + px * 4;
    const auto alpha = std::to_integer<unsigned>(pixel[3]) / 255.0;
    for (std::size_t c = 0; c < 3; ++c) sum[c] += std::to_integer<unsigned>(pixel[c]) / 255.0 * alpha * w;
    sum[3] += alpha * w; weights += w;
  }
  for (auto& value : sum) value /= weights;
  const auto alpha = sum[3];
  for (std::size_t c = 0; c < 3; ++c) sum[c] = alpha > 1e-12 ? sum[c] / alpha : 0;
  std::array<int, 4> result{};
  for (std::size_t c = 0; c < 4; ++c) result[c] = static_cast<int>(std::floor(std::clamp(sum[c], 0.0, 1.0) * 255 + 0.5));
  return result;
}
}  // namespace

int main() {
  const auto source = fixture();
  for (const auto size : {std::array<std::size_t, 2>{3, 2}, {11, 8}, {1, 1}, {9, 1}}) {
    auto converted = awj::resize_native_pixels(source, size[0], size[1]);
    if (!converted) return fail(converted.error().c_str());
    for (std::size_t y = 0; y < size[1]; ++y) for (std::size_t x = 0; x < size[0]; ++x) {
      const auto expected = reference(source, size[0], size[1], x, y);
      for (std::size_t c = 0; c < 4; ++c) {
        const auto actual = std::to_integer<int>(converted->planes[0].bytes[y * converted->planes[0].stride + x * 4 + c]);
        if (std::abs(actual - expected[c]) > 1) return fail("Lanczos 与独立二维参考不一致。");
      }
    }
  }
  auto unchanged = awj::resize_native_pixels(source, 7, 5);
  if (!unchanged || unchanged->planes[0].bytes != source.planes[0].bytes)
    return fail("无缩放路径没有精确复制像素。");
  auto constant = source;
  for (std::size_t p = 0; p < constant.planes[0].bytes.size(); p += 4) {
    constant.planes[0].bytes[p] = std::byte{127}; constant.planes[0].bytes[p + 1] = std::byte{63};
    constant.planes[0].bytes[p + 2] = std::byte{201}; constant.planes[0].bytes[p + 3] = std::byte{255};
  }
  auto filtered = awj::resize_native_pixels(constant, 2, 1);
  if (!filtered) return fail(filtered.error().c_str());
  for (std::size_t p = 0; p < filtered->planes[0].bytes.size(); ++p)
    if (filtered->planes[0].bytes[p] != constant.planes[0].bytes[p % 4]) return fail("常量图被 Lanczos 改色。");
  auto invisible = source;
  for (std::size_t p = 0; p < invisible.planes[0].bytes.size(); p += 4) invisible.planes[0].bytes[p + 3] = std::byte{0};
  auto empty = awj::resize_native_pixels(invisible, 3, 2);
  if (!empty || awj::compat::ranges::any_of(empty->planes[0].bytes, [](auto b) { return b != std::byte{0}; }))
    return fail("全透明隐藏 RGB 污染了重采样结果。");
  awj::AppConfig cfg;
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::fixed, .max_width = 10, .max_height = 10};
  auto plan = awj::plan_image_geometry(constant.width, constant.height, cfg);
  if (!plan || !plan->pads()) return fail("补边测试没有生成画布。");
  auto transparent = awj::apply_image_geometry(constant, *plan, true);
  auto black = awj::apply_image_geometry(constant, *plan, false);
  if (!transparent || !black || transparent->alpha_mode != awj::AlphaMode::straight || black->alpha_mode != awj::AlphaMode::none)
    return fail("补边 alpha 策略错误。");
  if (transparent->planes[0].bytes[3] != std::byte{0} || black->planes[0].bytes[0] != std::byte{0} ||
      black->planes[0].bytes[3] != std::byte{255}) return fail("画布没有正确补透明／黑边。");
  const awj::ResourcePlan budget{.memory_limit_bytes = 1};
  if (awj::apply_image_geometry(constant, *plan, true, {}, budget)) return fail("忽略了 Lanczos 内存准入。");
  awj::compat::stop_source stop; stop.request_stop();
  if (awj::apply_image_geometry(constant, *plan, true, stop.get_token())) return fail("忽略了 Lanczos 取消。");
  awj::ImageBuffer half{.width = 2, .height = 1, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 16,
      .sample_representation = awj::SampleRepresentation::ieee_half_float};
  awj::ImagePlane hp{.stride = 16};
  const std::array<std::uint16_t, 8> samples{0x4000, 0x3c00, 0, 0x3c00, 0x4000, 0x3c00, 0, 0x3c00};
  hp.bytes.resize(sizeof(samples)); std::memcpy(hp.bytes.data(), samples.data(), sizeof(samples));
  half.planes.push_back(std::move(hp));
  auto hdr = awj::resize_native_pixels(half, 5, 3);
  if (!hdr || hdr->sample_representation != awj::SampleRepresentation::ieee_half_float)
    return fail("Lanczos 丢失了 FP16 表示。");
  for (std::size_t p = 0; p < hdr->planes[0].bytes.size(); p += 8) {
    std::uint16_t red{}; std::memcpy(&red, hdr->planes[0].bytes.data() + p, 2);
    if (red != 0x4000) return fail("HDR 超过 1 的线性值被截断。");
  }
  half.sample_representation = awj::SampleRepresentation::unorm;
  const std::array<std::uint16_t, 8> integer_samples{12345, 32768, 65535, 65535, 12345, 32768, 65535, 65535};
  std::memcpy(half.planes[0].bytes.data(), integer_samples.data(), sizeof(integer_samples));
  auto high_depth = awj::resize_native_pixels(half, 5, 3);
  if (!high_depth) return fail(high_depth.error().c_str());
  for (std::size_t p = 0; p < high_depth->planes[0].bytes.size(); p += 2) {
    std::uint16_t value{}; std::memcpy(&value, high_depth->planes[0].bytes.data() + p, 2);
    if (value != integer_samples[(p / 2) % 4]) return fail("16-bit Lanczos 被提前量化为 8-bit。");
  }
  return 0;
}
