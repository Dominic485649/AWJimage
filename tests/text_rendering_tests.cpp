#include <slint.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "text_rendering.h"

namespace {

using Snapshot = slint::SharedPixelBuffer<slint::Rgba8Pixel>;

constexpr std::array scales{1.0f, 1.25f, 1.5f, 2.0f};
struct Offset { float x; float y; };
constexpr std::array offsets{
    Offset{0.0f, 0.0f}, Offset{0.25f, 0.25f}, Offset{0.5f, 0.5f},
    Offset{0.75f, 0.75f}, Offset{0.25f, 0.75f}, Offset{0.75f, 0.25f},
    Offset{0.125f, 0.375f}};
struct Cell { const char* name; int x; int y; };
constexpr Cell reference{"direct", 16, 16};
constexpr std::array cells{
    Cell{"rect", 368, 16}, Cell{"rounded", 16, 144},
    Cell{"nested-rounded", 368, 144}};
struct Script { const char* name; int y; };
constexpr std::array scripts{Script{"CJK", 12}, Script{"Latin", 40}};

// 同一 GPU 帧内的相对比较不依赖平台和字体截图基线。
// 允许整体平移一个像素，但不能通过滤波丢失字形对比度。
constexpr double max_normalized_error = 0.04;
constexpr double max_material_mismatch = 0.08;
constexpr int material_channel_error = 12;

struct Comparison {
  double normalized_error = std::numeric_limits<double>::infinity();
  double material_mismatch = 1.0;
  std::size_t reference_ink_pixels = 0;
  std::size_t support_pixels = 0;
  int dx = 0;
  int dy = 0;
  bool opaque = true;

  bool passes() const {
    return reference_ink_pixels >= 50 && opaque &&
           normalized_error <= max_normalized_error &&
           material_mismatch <= max_material_mismatch;
  }
};

int physical(float value, float scale) {
  return static_cast<int>(std::lround(value * scale));
}

const slint::Rgba8Pixel& pixel(const Snapshot& image, int x, int y) {
  if (x < 0 || y < 0 || x >= static_cast<int>(image.width()) ||
      y >= static_cast<int>(image.height())) {
    throw std::runtime_error("comparison crop is outside the GPU snapshot");
  }
  return image.begin()[static_cast<std::size_t>(y) * image.width() + x];
}

Comparison compare(const Snapshot& image, float scale, Offset offset,
                   const Cell& cell, const Script& script) {
  const int ref_x = physical(reference.x + 8.0f + offset.x, scale);
  const int ref_y = physical(reference.y + script.y + offset.y, scale);
  const int test_x = physical(cell.x + 8.0f + offset.x, scale);
  const int test_y = physical(cell.y + script.y + offset.y, scale);
  const int width = physical(304.0f, scale);
  const int height = physical(28.0f, scale);
  Comparison best;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      Comparison candidate;
      candidate.dx = dx;
      candidate.dy = dy;
      double reference_ink = 0.0;
      double absolute_error = 0.0;
      std::size_t mismatches = 0;
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const auto& a = pixel(image, ref_x + x, ref_y + y);
          const auto& b = pixel(image, test_x + x + dx, test_y + y + dy);
          const int ink_a = 765 - a.r - a.g - a.b;
          const int ink_b = 765 - b.r - b.g - b.b;
          const int dr = std::abs(int(a.r) - int(b.r));
          const int dg = std::abs(int(a.g) - int(b.g));
          const int db = std::abs(int(a.b) - int(b.b));
          reference_ink += ink_a;
          absolute_error += dr + dg + db;
          candidate.reference_ink_pixels += ink_a > 36;
          candidate.opaque = candidate.opaque && a.a == 255 && b.a == 255;
          if (ink_a > 36 || ink_b > 36) {
            ++candidate.support_pixels;
            mismatches += std::max({dr, dg, db}) > material_channel_error;
          }
        }
      }
      candidate.normalized_error = reference_ink > 0 ?
          absolute_error / reference_ink : std::numeric_limits<double>::infinity();
      candidate.material_mismatch = candidate.support_pixels ?
          double(mismatches) / candidate.support_pixels : 1.0;
      if (candidate.normalized_error < best.normalized_error) best = candidate;
    }
  }
  return best;
}

bool clip_edges_covered(const Snapshot& image, float scale, Offset offset) {
  const int y = physical(244.0f + offset.y, scale) + physical(4.0f, scale);
  bool covered = true;
  struct Check { int x; bool inside; };
  for (const float base_x : {10.0f, 110.0f}) {
    const float left = (base_x + offset.x) * scale;
    const float right = left + 80.0f * scale;
    const std::array checks{
        Check{static_cast<int>(std::ceil(left)) + 2, true},
        Check{static_cast<int>(std::floor(right)) - 3, true},
        Check{static_cast<int>(std::floor(left)) - 1, false},
        Check{static_cast<int>(std::ceil(right)), false}};
    for (const auto check : checks) {
      const auto& color = pixel(image, check.x, y);
      const int r = check.inside ? 224 : 255;
      const int gb = check.inside ? 32 : 255;
      if (std::abs(int(color.r) - r) <= 2 && std::abs(int(color.g) - gb) <= 2 &&
          std::abs(int(color.b) - gb) <= 2 && color.a == 255) continue;
      covered = false;
      std::fprintf(stderr,
          "FAIL clip-edge scale=%.2f offset=(%.3f,%.3f) parent_cache=%s expected=%s "
          "pixel=(%d,%d) rgba=(%u,%u,%u,%u)\n",
          double(scale), double(offset.x), double(offset.y), base_x == 110.0f ? "yes" : "no",
          check.inside ? "red" : "white", check.x, y,
          unsigned(color.r), unsigned(color.g), unsigned(color.b), unsigned(color.a));
    }
  }
  return covered;
}

void write_ppm(const std::filesystem::path& path, const Snapshot& image) {
  std::ofstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot create PPM snapshot");
  stream << "P6\n" << image.width() << ' ' << image.height() << "\n255\n";
  for (const auto& p : image) {
    const std::array<char, 3> rgb{static_cast<char>(p.r), static_cast<char>(p.g),
                                  static_cast<char>(p.b)};
    stream.write(rgb.data(), rgb.size());
  }
  if (!stream) throw std::runtime_error("cannot write PPM snapshot");
}

void set_environment(const char* name, const char* value) {
#ifdef _WIN32
  if (_putenv_s(name, value) != 0)
#else
  if (setenv(name, value, 1) != 0)
#endif
    throw std::runtime_error("cannot set the renderer test environment");
}

void select_renderer(float scale) {
  set_environment("SLINT_BACKEND", "winit-femtovg");
  // Winit 创建后端时读取缩放覆盖，因此各比例使用独立进程。
  // set_const_scale_factor 会在首次调用后锁定属性，不能用于动态切换。
  const std::string value = std::to_string(scale);
  set_environment("SLINT_SCALE_FACTOR", value.c_str());
}

struct Options {
  float scale = 1.5f;
  std::optional<std::filesystem::path> output_directory;
#ifdef _WIN32
  std::string family = "Microsoft YaHei";
#else
  std::string family = "DejaVu Sans";
#endif
};

Options parse_options(int argc, char** argv) {
  Options options;
#ifdef _WIN32
  // 直接读取宽字符环境变量，避免 C 运行库按系统 ANSI 代码页转换字体名。
  const DWORD capacity = GetEnvironmentVariableW(L"AWJ_TEXT_TEST_FONT", nullptr, 0);
  if (capacity) {
    std::vector<wchar_t> family(capacity);
    const DWORD length = GetEnvironmentVariableW(L"AWJ_TEXT_TEST_FONT", family.data(), capacity);
    if (!length || length >= capacity) throw std::runtime_error("cannot read AWJ_TEXT_TEST_FONT");
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, family.data(),
        static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (!bytes) throw std::runtime_error("AWJ_TEXT_TEST_FONT is not valid Unicode");
    options.family.resize(bytes);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, family.data(),
        static_cast<int>(length), options.family.data(), bytes, nullptr, nullptr))
      throw std::runtime_error("cannot encode AWJ_TEXT_TEST_FONT as UTF-8");
  }
#else
  if (const char* family = std::getenv("AWJ_TEXT_TEST_FONT")) options.family = family;
#endif
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--output-dir" && i + 1 < argc) {
      options.output_directory = std::filesystem::path{argv[++i]};
    } else if (arg == "--font-family" && i + 1 < argc) {
      options.family = argv[++i];
    } else if (arg == "--scale" && i + 1 < argc) {
      const std::string value = argv[++i];
      std::size_t consumed = 0;
      options.scale = std::stof(value, &consumed);
      if (consumed != value.size()) throw std::runtime_error("invalid scale factor");
    } else {
      throw std::runtime_error("usage: awj_text_rendering_tests "
          "[--scale 1|1.25|1.5|2] [--output-dir NEW_DIRECTORY] [--font-family FAMILY]");
    }
  }
  if (std::find(scales.begin(), scales.end(), options.scale) == scales.end())
    throw std::runtime_error("scale must be 1, 1.25, 1.5, or 2");
  if (options.family.empty()) throw std::runtime_error("font family must not be empty");
  if (options.output_directory &&
      !std::filesystem::create_directory(*options.output_directory)) {
    throw std::runtime_error("PPM output directory must be new (parent must exist)");
  }
  return options;
}

int run(const Options& options) {
  select_renderer(options.scale);
  bool saw_opengl = false;
  auto app = TextRenderingFixture::create();
  app->set_family(slint::SharedString{options.family});
  if (app->window().set_rendering_notifier(
          [&saw_opengl](slint::RenderingState, slint::GraphicsAPI api) {
            saw_opengl = saw_opengl || api == slint::GraphicsAPI::NativeOpenGL;
          })) {
    throw std::runtime_error("real GPU rendering notifier unavailable; no headless/software substitute is allowed");
  }

  std::size_t case_index = 0;
  std::size_t comparisons = 0;
  std::size_t failures = 0;
  std::array<std::size_t, 3> failed_by_mode{};
  std::array<double, 3> worst_error{};
  bool configured = false;
  int resize_retries = 0;
  std::optional<std::string> error;
  slint::Timer timer;
  timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds{35}, [&] {
    try {
      if (case_index == offsets.size()) {
        app->hide();
        slint::quit_event_loop();
        return;
      }
      const float scale = options.scale;
      const auto offset = offsets[case_index % offsets.size()];
      const slint::PhysicalSize expected{{static_cast<std::uint32_t>(physical(704, scale)),
                                          static_cast<std::uint32_t>(physical(256, scale))}};
      if (!configured) {
        app->set_offset_x(offset.x);
        app->set_offset_y(offset.y);
        app->set_inner_offset_x(offset.y);
        app->set_inner_offset_y(offset.x);
        app->window().set_size(expected);
        configured = true;
        resize_retries = 0;
        return;
      }
      const auto image = app->window().take_snapshot();
      if (!image) throw std::runtime_error("native FemtoVG window snapshot is unavailable");
      if (image->width() != expected.width || image->height() != expected.height) {
        if (++resize_retries < 20) return;
        throw std::runtime_error("native window did not settle at the requested physical snapshot size");
      }
      if (std::fabs(app->window().scale_factor() - scale) > 0.001f)
        throw std::runtime_error("native window ignored the requested test scale factor");
      if (!saw_opengl) throw std::runtime_error("snapshot did not exercise the OpenGL renderer");
#ifdef _WIN32
      if (!app->window().win32_hwnd()) throw std::runtime_error("snapshot did not use a real Win32 window");
#endif
      if (options.output_directory) {
        char name[96];
        std::snprintf(name, sizeof(name), "scale-%.2f-offset-%zu.ppm",
                      double(scale), case_index % offsets.size());
        write_ppm(*options.output_directory / name, *image);
      }
      for (std::size_t mode = 0; mode < cells.size(); ++mode) {
        for (const auto script : scripts) {
          const auto result = compare(*image, scale, offset, cells[mode], script);
          ++comparisons;
          worst_error[mode] = std::max(worst_error[mode], result.normalized_error);
          if (!result.passes()) {
            ++failures;
            ++failed_by_mode[mode];
            std::fprintf(stderr,
                "FAIL text-pixels scale=%.2f offset=(%.3f,%.3f) clip=%s script=%s "
                "ink_error=%.5f material_mismatch=%.3f shift=(%d,%d) "
                "reference_ink=%zu support=%zu opaque=%s\n",
                double(scale), double(offset.x), double(offset.y), cells[mode].name,
                script.name, result.normalized_error, result.material_mismatch,
                result.dx, result.dy, result.reference_ink_pixels, result.support_pixels,
                result.opaque ? "yes" : "no");
          }
        }
      }
      ++comparisons;
      if (!clip_edges_covered(*image, scale, offset)) ++failures;
      ++case_index;
      configured = false;
    } catch (const std::exception& exception) {
      error = exception.what();
      app->hide();
      slint::quit_event_loop();
    }
  });
  slint::Timer timeout;
  timeout.start(slint::TimerMode::SingleShot, std::chrono::seconds{45}, [&] {
    error = "real renderer test timed out";
    app->hide();
    slint::quit_event_loop();
  });
  app->show();
  std::fprintf(stderr,
      "text-pixels requested_backend=winit-femtovg family=\"%s\" font_size=12px "
      "weight=400 scale=%.2f offsets=%zu "
      "max_ink_error=%.3f max_material_mismatch=%.3f translation=+/-1px\n",
      options.family.c_str(), double(options.scale), offsets.size(),
      max_normalized_error, max_material_mismatch);
  slint::run_event_loop();
  timer.stop();
  timeout.stop();
  if (error) throw std::runtime_error(*error);
  if (case_index != offsets.size())
    throw std::runtime_error("native renderer test exited before all cases ran");
#ifdef _WIN32
  const bool mactype_loader = GetModuleHandleW(L"MacType64.dll") || GetModuleHandleW(L"MacType.dll");
  const bool mactype_core = GetModuleHandleW(L"MacType64.Core.dll") || GetModuleHandleW(L"MacType.Core.dll");
  std::fprintf(stderr, "PROCESS MacType_loader=%s MacType_core=%s\n",
               mactype_loader ? "present" : "absent", mactype_core ? "present" : "absent");
#endif
  for (std::size_t mode = 0; mode < cells.size(); ++mode) {
    std::fprintf(stderr, "SUMMARY clip=%s failures=%zu/%zu worst_ink_error=%.5f\n",
                 cells[mode].name, failed_by_mode[mode],
                 offsets.size() * scripts.size(), worst_error[mode]);
  }
  std::fprintf(stderr,
      "RESULT text-pixels %s snapshots=%zu comparisons=%zu failures=%zu "
      "api=NativeOpenGL same_window=yes\n",
      failures ? "FAIL" : "PASS", case_index, comparisons, failures);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(parse_options(argc, argv));
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "ERROR text-pixels: %s\n", exception.what());
    return 2;
  }
}
