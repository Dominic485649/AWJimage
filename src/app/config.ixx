module;
#include "../compat/stdlib_compat.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#include <scn/scan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include <utility>

export module awj.config;

export import awj.avif_layers;
import awj.encoding_defaults;
import awj.visual_quality;

export namespace awj {

enum class OutputFormat { png, avif, webp, jxl, jpgli };
enum class BackendMode { native };
enum class OutputPolicy { normal, shell };
enum class CollisionMode { overwrite, skip, suffix_time, suffix_random, suffix_number };
// Stable persisted selection values; never reinterpret them as filtered UI rows.
enum class ChromaMode { auto_keep = 0, yuv444 = 1, yuv422 = 2, yuv420 = 3, yuv400 = 4 };
// AVIF 的颜色表示与 YUV 4:2:0/4:2:2/4:4:4 采样是两件事。
enum class AvifColorRepresentation { yuv, source, rgb_identity };

// Format-level capability only: config must not depend on the codec module.
constexpr bool supports_chroma_selection(OutputFormat format) noexcept {
  return format == OutputFormat::avif || format == OutputFormat::jpgli;
}

constexpr bool supports_chroma_mode(
    OutputFormat format, ChromaMode mode,
    AvifColorRepresentation representation = AvifColorRepresentation::yuv) noexcept {
  if (mode == ChromaMode::auto_keep) return true;
  if (!supports_chroma_selection(format)) return false;
  if (mode == ChromaMode::yuv444) return true;
  if (format == OutputFormat::avif &&
      representation == AvifColorRepresentation::rgb_identity) return false;
  return mode == ChromaMode::yuv422 || mode == ChromaMode::yuv420 ||
         (format == OutputFormat::avif && mode == ChromaMode::yuv400);
}

// UI/config-cache migration only. Explicit CLI and preset requests are validated,
// not normalized, so an unsupported requested mode remains an error.
constexpr int normalize_chroma_selection_index(
    OutputFormat format, int index,
    AvifColorRepresentation representation = AvifColorRepresentation::yuv) noexcept {
  return index >= 0 && index <= 4 &&
                 supports_chroma_mode(format, static_cast<ChromaMode>(index), representation)
             ? index : 0;
}

enum class AvifEncoderMode { automatic, aom = 2 };
enum class AlphaModePolicy { force, automatic, off };
enum class JpegHdrMode { automatic, sdr, hdr };
enum class AvifAnimationTune { automatic, ssim, psnr };

constexpr std::wstring_view avif_animation_tune_name(AvifAnimationTune tune) noexcept {
  return tune == AvifAnimationTune::ssim ? L"ssim" : tune == AvifAnimationTune::psnr ? L"psnr" : L"auto";
}
enum class ImageSizeLimitMode { automatic, none, manual, upscale, downscale, fixed };

constexpr bool manual_image_size_mode(ImageSizeLimitMode mode) noexcept {
  return mode != ImageSizeLimitMode::automatic && mode != ImageSizeLimitMode::none;
}

constexpr int image_size_mode_index(ImageSizeLimitMode mode) noexcept {
  switch (mode) {
    case ImageSizeLimitMode::none: return 1;
    case ImageSizeLimitMode::upscale: return 2;
    case ImageSizeLimitMode::manual:
    case ImageSizeLimitMode::downscale: return 3;
    case ImageSizeLimitMode::fixed: return 4;
    default: return 0;
  }
}

constexpr ImageSizeLimitMode image_size_mode_from_index(int index) noexcept {
  switch (index) {
    case 1: return ImageSizeLimitMode::none;
    case 2: return ImageSizeLimitMode::upscale;
    case 3: return ImageSizeLimitMode::downscale;
    case 4: return ImageSizeLimitMode::fixed;
    default: return ImageSizeLimitMode::automatic;
  }
}

constexpr std::string_view image_size_mode_name(ImageSizeLimitMode mode) noexcept {
  switch (mode) {
    case ImageSizeLimitMode::none: return "none";
    case ImageSizeLimitMode::manual: return "manual";
    case ImageSizeLimitMode::upscale: return "upscale";
    case ImageSizeLimitMode::downscale: return "downscale";
    case ImageSizeLimitMode::fixed: return "fixed";
    default: return "auto";
  }
}

struct ImageSizeLimit {
  ImageSizeLimitMode mode{ImageSizeLimitMode::automatic};
  std::optional<int> max_width{};
  std::optional<int> max_height{};
  std::optional<int> max_long_edge{};
  std::optional<int> max_short_edge{};
  std::optional<double> scale_percent{};
};

struct ImageGeometryPlan {
  std::size_t source_width{}, source_height{};
  std::size_t content_width{}, content_height{};
  std::size_t canvas_width{}, canvas_height{};
  std::size_t offset_x{}, offset_y{};

  bool resamples() const noexcept {
    return content_width != source_width || content_height != source_height;
  }
  bool pads() const noexcept {
    return content_width != canvas_width || content_height != canvas_height;
  }
  bool changes() const noexcept { return resamples() || pads(); }
};

constexpr int automatic_thread_budget(unsigned hardware) noexcept {
  // 自动并发不是“吃满 CPU”，而是给桌面、UI 线程和编码器内部线程预留余量。
  constexpr auto max_auto_jobs =
      static_cast<unsigned>(encoding_defaults::max_automatic_thread_budget);
  if (hardware <= 1) {
    return 1;
  }
  if (hardware >= 12) {
    return static_cast<int>(std::min(hardware - 4, max_auto_jobs));
  }
  if (hardware > 4) {
    return static_cast<int>(hardware - 2);
  }
  return static_cast<int>(hardware - 1);
}

unsigned available_processor_count() noexcept {
  const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
  DWORD_PTR process_mask{}, system_mask{};
  if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask) {
    return std::min(hardware, static_cast<unsigned>(std::popcount(process_mask)));
  }
#else
  cpu_set_t allowed{};
  if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
    const auto count = CPU_COUNT(&allowed);
    if (count > 0) return std::min(hardware, static_cast<unsigned>(count));
  }
#endif
  return hardware;
}

int default_max_jobs() noexcept {
  return automatic_thread_budget(available_processor_count());
}

// shortcut: Windows CPU IDs refer to the current processor group; extend only when multi-group selection is needed.
constexpr unsigned cpu_affinity_capacity() noexcept {
#ifdef _WIN32
  return sizeof(DWORD_PTR) * 8;
#else
  return CPU_SETSIZE;
#endif
}

awj::compat::expected<void, std::string> apply_cpu_affinity(const std::vector<unsigned>& cpus) {
  if (cpus.empty()) return {};
#ifdef _WIN32
  DWORD_PTR allowed{}, system{}, requested{};
  if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) || !allowed)
    return awj::compat::unexpected{"无法读取进程 CPU 亲和性。"};
  for (const auto cpu : cpus) {
    if (cpu >= cpu_affinity_capacity() || !(allowed & (DWORD_PTR{1} << cpu)))
      return awj::compat::unexpected{awj::compat::format("CPU {} 不在进程允许的 CPU 集合内。", cpu)};
    requested |= DWORD_PTR{1} << cpu;
  }
  if (!SetProcessAffinityMask(GetCurrentProcess(), requested))
    return awj::compat::unexpected{awj::compat::format("无法设置 CPU 亲和性，Windows 错误 {}。", GetLastError())};
  if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) || allowed != requested)
    return awj::compat::unexpected{"CPU 亲和性回读与请求不一致。"};
#else
  cpu_set_t allowed{}, requested{};
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    return awj::compat::unexpected{"无法读取进程 CPU 亲和性。"};
  CPU_ZERO(&requested);
  for (const auto cpu : cpus) {
    if (cpu >= cpu_affinity_capacity() || !CPU_ISSET(cpu, &allowed))
      return awj::compat::unexpected{awj::compat::format("CPU {} 不在进程允许的 CPU 集合内。", cpu)};
    CPU_SET(cpu, &requested);
  }
  if (sched_setaffinity(0, sizeof(requested), &requested) != 0)
    return awj::compat::unexpected{"无法设置 CPU 亲和性。"};
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0 || !CPU_EQUAL(&allowed, &requested))
    return awj::compat::unexpected{"CPU 亲和性回读与请求不一致。"};
#endif
  return {};
}

constexpr int default_quality_for(OutputFormat format) noexcept {
  switch (format) {
    case OutputFormat::png:
      return 100;
    case OutputFormat::webp:
      return encoding_defaults::default_webp_quality;
    case OutputFormat::jxl:
      return encoding_defaults::default_jxl_quality;
    case OutputFormat::jpgli:
      return encoding_defaults::default_jpegli_quality;
    case OutputFormat::avif:
    default:
      return encoding_defaults::default_avif_quality;
  }
}

constexpr int default_speed_for(OutputFormat format) noexcept {
  switch (format) {
    case OutputFormat::png:
      return encoding_defaults::default_native_speed;
    case OutputFormat::jxl:
      return encoding_defaults::default_jxl_native_speed;
    case OutputFormat::jpgli:
      return encoding_defaults::default_jpegli_native_speed;
    case OutputFormat::avif:
      return encoding_defaults::default_avif_native_speed;
    case OutputFormat::webp:
      return encoding_defaults::default_webp_native_speed;
    default:
      return encoding_defaults::default_native_speed;
  }
}

constexpr bool default_allow_wic_fallback_for_platform() noexcept {
#ifdef _WIN32
  return encoding_defaults::default_allow_wic_fallback;
#else
  return false;
#endif
}

// 命令行只负责生成这个配置对象；后面的流水线不会再回头解析 argv。
struct AppConfig {
  std::filesystem::path input_path{
      std::wstring{encoding_defaults::default_input_path}};
  std::filesystem::path output_dir{};
  std::wstring output_template{encoding_defaults::default_output_template};
  BackendMode backend{BackendMode::native};
  OutputFormat output_format{OutputFormat::avif};
  OutputPolicy output_policy{OutputPolicy::normal};
  CollisionMode collision_mode{CollisionMode::suffix_number};
  ChromaMode chroma_mode{ChromaMode::auto_keep};
  AvifColorRepresentation avif_color_representation{
      AvifColorRepresentation::yuv};
  AvifEncoderMode avif_encoder{AvifEncoderMode::automatic};
  // AVIF 文件内容不变，只把完整输出后缀写成 .avif.png。
  bool append_png_suffix{};
  AlphaModePolicy alpha_policy{AlphaModePolicy::automatic};
  bool experimental_clamped_grid_padding{
      encoding_defaults::default_experimental_clamped_grid_padding};
  int quality{default_quality_for(output_format)};
  std::optional<int> visual_quality{};
  std::optional<int> bit_depth{};
  std::optional<int> speed{};
  int jpegli_progressive_level{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  JpegHdrMode jpeg_hdr{JpegHdrMode::automatic};
  AvifAnimationTune avif_animation_tune{AvifAnimationTune::automatic};
  int avif_animation_keyframe{};
  AvifLayerConfig avif_layers{};
  bool jxl_jpeg_lossless{true};
  int max_jobs{default_max_jobs()};
  // CLI session only; apply before creating pipeline workers.
  std::vector<unsigned> cpu_affinity{};
  std::uint64_t memory_limit_bytes{
       encoding_defaults::default_memory_limit_bytes};
  // Session only: overrides automatic/manual admission without erasing the value.
  bool unlock_memory_limit{false};
  int encode_timeout_minutes{
      encoding_defaults::default_encode_timeout_minutes};
  bool allow_wic_fallback{default_allow_wic_fallback_for_platform()};
  std::optional<int> color_primaries{};
  std::optional<int> transfer_characteristics{};
  std::optional<int> matrix_coefficients{};
  std::optional<int> color_range{};
  // 默认在视觉质量搜索未达标时交付最接近候选；未启用 visual-quality 时该值
  // 不参与编码，保持 UI、CLI 与 worker 的默认一致。
  bool visual_quality_fallback{
      encoding_defaults::default_visual_quality_fallback};
  bool strip_metadata{false};
  // Compatibility alias: either true enables both awj.log and summery.csv.
  bool write_summary{false};
  bool write_log{false};
  // Application file queues only; never used by SDK memory encoding.
  bool delete_source{false};
  // 只在 Windows 实际执行；Linux CLI 不公开也不解析这些开关。
  bool preserve_creation_time{false};
  bool preserve_modification_time{false};
  bool preserve_access_time{false};
  bool shell_close_on_finish{true};
  ImageSizeLimit image_size_limit{};
  std::wstring studio_cancel_event_name{};
  std::filesystem::path studio_queue_manifest{};
  // ponytail: empty = auto chain; kept for worker/CLI compatibility, not a Studio page action.
  std::wstring studio_large_action{};
  // Legacy CLI input-only opt-out. Studio derives it from unlock_memory_limit.
  bool unlock_max_input_file_bytes{false};
  // Internal operation/worker snapshots; never persisted in JSONC or presets.
  std::optional<encoding_defaults::MemoryPolicy> runtime_memory_policy{};
  std::optional<std::uint64_t> studio_input_limit_bytes{};
  // Runtime callback only; never serialized into presets or update manifests.
  std::function<void(std::size_t, std::size_t, std::size_t)> frame_progress{};
};

awj::compat::expected<encoding_defaults::MemoryPolicy, std::string>
operation_memory_policy(const AppConfig& cfg) {
  const bool input_unlocked = cfg.unlock_memory_limit || cfg.unlock_max_input_file_bytes;
  if (cfg.runtime_memory_policy && cfg.runtime_memory_policy->valid() &&
      cfg.runtime_memory_policy->unrestricted_memory == cfg.unlock_memory_limit &&
      cfg.runtime_memory_policy->unrestricted_input == input_unlocked)
    return *cfg.runtime_memory_policy;
  if (!input_unlocked && cfg.studio_input_limit_bytes) {
    if (!*cfg.studio_input_limit_bytes)
      return awj::compat::unexpected{"Studio 输入保护快照不能为零。"};
    return encoding_defaults::MemoryPolicy{*cfg.studio_input_limit_bytes, false, false};
  }
  const auto policy = encoding_defaults::system_memory_policy(
      cfg.unlock_memory_limit, cfg.unlock_max_input_file_bytes);
  if (!policy) return awj::compat::unexpected{std::string{encoding_defaults::memory_policy_unavailable_message}};
  return *policy;
}

awj::compat::expected<void, std::string> freeze_operation_memory_policy(AppConfig& cfg) {
  auto policy = operation_memory_policy(cfg);
  if (!policy) return awj::compat::unexpected{policy.error()};
  cfg.runtime_memory_policy = *policy;
  return {};
}

awj::compat::expected<void, std::string> validate_image_size_limit(const ImageSizeLimit& limit) {
  if (!manual_image_size_mode(limit.mode)) return {};
  for (const auto value : {limit.max_width, limit.max_height, limit.max_long_edge, limit.max_short_edge}) {
    if (value && (*value < 1 || *value > 1000000))
      return awj::compat::unexpected{"尺寸必须为 1～1000000 的整数。"};
  }
  if (limit.scale_percent && (!std::isfinite(*limit.scale_percent) || *limit.scale_percent <= 0))
    return awj::compat::unexpected{"scale-percent 必须为正有限数，可以超过 100%。"};
  if (limit.mode == ImageSizeLimitMode::fixed) {
    const bool has_dimensions = limit.max_width || limit.max_height || limit.max_long_edge || limit.max_short_edge;
    if (limit.scale_percent && has_dimensions) return awj::compat::unexpected{"固定模式的尺寸与缩放比例不能同时填写。"};
    if (!has_dimensions && !limit.scale_percent)
      return awj::compat::unexpected{"固定模式至少需要填写一个尺寸或缩放比例。"};
    if (limit.max_long_edge && limit.max_short_edge && *limit.max_long_edge < *limit.max_short_edge)
      return awj::compat::unexpected{"固定长边不能小于固定短边。"};
    if (limit.max_width && limit.max_height &&
        ((limit.max_long_edge && std::max(*limit.max_width, *limit.max_height) != *limit.max_long_edge) ||
         (limit.max_short_edge && std::min(*limit.max_width, *limit.max_height) != *limit.max_short_edge)))
      return awj::compat::unexpected{"固定宽高与固定长边／短边互相矛盾。"};
  }
  return {};
}

awj::compat::expected<ImageGeometryPlan, std::string> plan_image_geometry(
    std::size_t width, std::size_t height, const AppConfig& cfg) {
  if (!width || !height || width > UINT32_MAX || height > UINT32_MAX)
    return awj::compat::unexpected{"源图片显示尺寸无效。"};
  const auto& limit = cfg.image_size_limit;
  if (auto valid = validate_image_size_limit(limit); !valid) return awj::compat::unexpected{valid.error()};
  ImageGeometryPlan plan{.source_width = width, .source_height = height,
      .content_width = width, .content_height = height, .canvas_width = width, .canvas_height = height};
  const auto dimension = [](long double value, int rounding) -> awj::compat::expected<std::size_t, std::string> {
    if (!std::isfinite(value) || value > UINT32_MAX)
      return awj::compat::unexpected{"目标尺寸超过格式与整数范围。"};
    const auto nearest = std::round(value);
    if (std::abs(value - nearest) <= std::numeric_limits<long double>::epsilon() * std::max(1.0L, value) * 8)
      value = nearest;
    value = rounding > 0 ? std::ceil(value) : rounding < 0 ? std::floor(value) : std::round(value);
    return static_cast<std::size_t>(std::max(1.0L, value));
  };
  if (limit.mode == ImageSizeLimitMode::fixed && !limit.scale_percent) {
    double best_coverage = -1;
    bool best_source_direction = false;
    for (const bool wide : {true, false}) {
      std::optional<std::size_t> cw, ch;
      bool consistent = true;
      const auto bind = [&](std::optional<std::size_t>& axis, std::optional<int> value) {
        if (!value) return;
        if (axis && *axis != static_cast<std::size_t>(*value)) consistent = false;
        axis = static_cast<std::size_t>(*value);
      };
      bind(cw, limit.max_width); bind(ch, limit.max_height);
      bind(wide ? cw : ch, limit.max_long_edge);
      bind(wide ? ch : cw, limit.max_short_edge);
      if (!consistent) continue;
      if (!cw) {
        auto value = dimension(static_cast<long double>(*ch) * width / height, 0);
        if (!value) continue;
        cw = wide ? std::max(*ch, *value) : std::min(*ch, *value);
      }
      if (!ch) {
        auto value = dimension(static_cast<long double>(*cw) * height / width, 0);
        if (!value) continue;
        ch = wide ? std::min(*cw, *value) : std::max(*cw, *value);
      }
      if ((wide && *cw < *ch) || (!wide && *ch < *cw)) continue;
      const auto scale = std::min(static_cast<long double>(*cw) / width, static_cast<long double>(*ch) / height);
      auto iw = dimension(width * scale, 0), ih = dimension(height * scale, 0);
      if (!iw || !ih) continue;
      *iw = std::min(*iw, *cw); *ih = std::min(*ih, *ch);
      const auto coverage = static_cast<double>(*iw) * static_cast<double>(*ih) /
          (static_cast<double>(*cw) * static_cast<double>(*ch));
      const bool source_direction = wide == (width >= height);
      if (coverage < best_coverage || (coverage == best_coverage && !source_direction && best_source_direction)) continue;
      best_coverage = coverage; best_source_direction = source_direction;
      plan.content_width = *iw; plan.content_height = *ih;
      plan.canvas_width = *cw; plan.canvas_height = *ch;
    }
    if (best_coverage < 0) return awj::compat::unexpected{"固定宽、高、长边、短边无法同时满足，请检查冲突尺寸。"};
    plan.offset_x = (plan.canvas_width - plan.content_width) / 2;
    plan.offset_y = (plan.canvas_height - plan.content_height) / 2;
    return plan;
  }
  std::optional<long double> scale;
  const bool maximum = limit.mode == ImageSizeLimitMode::upscale;
  const auto candidate = [&](long double value) {
    scale = !scale ? value : maximum ? std::max(*scale, value) : std::min(*scale, value);
  };
  if (limit.mode == ImageSizeLimitMode::automatic) {
    candidate(1);
    if (cfg.output_format == OutputFormat::webp) candidate(16383.0L / std::max(width, height));
    if (cfg.output_format == OutputFormat::jpgli) candidate(65535.0L / std::max(width, height));
  } else if (limit.mode != ImageSizeLimitMode::none) {
    if (limit.mode == ImageSizeLimitMode::manual) candidate(1);
    if (limit.scale_percent) candidate(static_cast<long double>(*limit.scale_percent) / 100);
    if (limit.max_width) candidate(static_cast<long double>(*limit.max_width) / width);
    if (limit.max_height) candidate(static_cast<long double>(*limit.max_height) / height);
    if (limit.max_long_edge) candidate(static_cast<long double>(*limit.max_long_edge) / std::max(width, height));
    if (limit.max_short_edge) candidate(static_cast<long double>(*limit.max_short_edge) / std::min(width, height));
  }
  const int rounding = limit.mode == ImageSizeLimitMode::fixed ? 0 : maximum ? 1 : -1;
  auto w = dimension(width * scale.value_or(1), rounding);
  auto h = dimension(height * scale.value_or(1), rounding);
  if (!w || !h) return awj::compat::unexpected{!w ? w.error() : h.error()};
  plan.content_width = plan.canvas_width = *w;
  plan.content_height = plan.canvas_height = *h;
  return plan;
}

std::optional<std::pair<std::size_t, std::size_t>> limited_dimensions(
    std::size_t width, std::size_t height, const AppConfig& cfg) {
  const auto plan = plan_image_geometry(width, height, cfg);
  if (!plan || !plan->changes()) return std::nullopt;
  return std::pair{plan->canvas_width, plan->canvas_height};
}

AppConfig default_app_config() { return AppConfig{}; }

#ifndef _WIN32
// Keep invalid POSIX argument bytes distinct from real Unicode text. Surrogate
// escapes are internal only; native paths are restored before filesystem use.
std::wstring argument_text_from_native(std::string_view value) {
  try {
    const auto unicode = std::filesystem::path{value}.u32string();
    if (awj::compat::ranges::none_of(unicode, [](char32_t ch) {
          return ch >= 0xd800 && ch <= 0xdfff;
        })) return {unicode.begin(), unicode.end()};
  } catch (const std::runtime_error&) {
  }
  std::wstring text;
  text.reserve(value.size());
  for (const unsigned char byte : value)
    text.push_back(byte < 0x80 ? byte : 0xdc00 + byte);
  return text;
}
#endif

std::filesystem::path path_from_argument_text(std::wstring_view value) {
#ifdef _WIN32
  return std::filesystem::path{value};
#else
  std::string native;
  std::u32string unicode;
  const auto flush = [&] {
    native += std::filesystem::path{unicode}.native();
    unicode.clear();
  };
  for (const auto ch : value) {
    if (ch >= 0xdc80 && ch <= 0xdcff) {
      flush();
      native.push_back(static_cast<char>(ch - 0xdc00));
    } else {
      unicode.push_back(static_cast<char32_t>(ch));
    }
  }
  flush();
  return std::filesystem::path{native};
#endif
}

awj::compat::expected<std::filesystem::path, std::string> normalize_path_argument(
    std::wstring_view value, std::string_view label) {
  const auto is_space = [](wchar_t ch) {
    return std::iswspace(static_cast<wint_t>(ch)) != 0;
  };
  while (!value.empty() && is_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_space(value.back())) {
    value.remove_suffix(1);
  }
  if (value.empty()) {
    return awj::compat::unexpected{awj::compat::format("{}不能为空。", label)};
  }
  const bool starts_quote = value.front() == L'\"';
  const bool ends_quote = value.back() == L'\"';
  if (starts_quote != ends_quote) {
    return awj::compat::unexpected{awj::compat::format(
        "{}的双引号必须成对并包住整个路径，例如 \"D:\\\\example.jxr\"。",
        label)};
  }
  if (starts_quote) {
    value.remove_prefix(1);
    value.remove_suffix(1);
    if (value.empty()) {
      return awj::compat::unexpected{awj::compat::format("{}不能为空。", label)};
    }
  }
  if (value.find(L'\"') != std::wstring_view::npos) {
    return awj::compat::unexpected{
        awj::compat::format("{}中的双引号只能成对包住整个路径。", label)};
  }
  return path_from_argument_text(value);
}

void apply_format_defaults(AppConfig& cfg, OutputFormat format) noexcept {
  cfg.output_format = format;
  cfg.quality = default_quality_for(format);
}

struct ParseResult {
  bool should_exit{false};
  int exit_code{0};
  AppConfig config{};
  // 用户预设在 CLI 前端加载：先用第一遍解析确定输出格式，再把对应格式的
  // 预设作为第二遍解析的基线，使显式参数与书写顺序无关。
  std::optional<std::wstring> preset_name{};
  std::optional<std::filesystem::path> preset_file{};
  std::vector<std::filesystem::path> shell_inputs{};
  std::vector<std::string> warnings{};
  // 分层解析保留 auto 线程意图，较高层修改 affinity 后仍按最终 CPU 集合计算。
  bool automatic_threads{};
  bool input_was_set{};
};

struct ArgumentParsePolicy {
  bool finalize{true};
  bool conversion_only{};
  bool override_inherited{};
  std::optional<bool> automatic_threads{};
};

namespace config_detail {

constexpr std::size_t max_output_template_length = 512;

std::wstring lower_copy(std::wstring_view text) {
  std::wstring out{text};
  awj::compat::ranges::transform(out, out.begin(),
                         [](wchar_t ch) { return std::towlower(ch); });
  return out;
}

std::string narrow_ascii(std::wstring_view text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t ch : text) {
    out.push_back(ch <= 0x7f ? static_cast<char>(ch) : '?');
  }
  return out;
}

std::string narrow_ascii_for_diagnostics(std::wstring_view text) {
  try {
    return narrow_ascii(text);
  } catch (const std::bad_alloc&) {
    return "?";
  } catch (const std::length_error&) {
    return "?";
  }
}

std::filesystem::path::string_type shell_input_key(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  auto key = (ec ? path : absolute).lexically_normal().native();
#ifdef _WIN32
  awj::compat::ranges::transform(key, key.begin(),
                         [](wchar_t ch) { return std::towlower(ch); });
#endif
  return key;
}

template <class Number>
std::optional<Number> scan_number(std::wstring_view text) {
  const auto narrow = narrow_ascii(lower_copy(text));
  const std::string_view source{narrow};
  if (!source.empty() &&
      std::isspace(static_cast<unsigned char>(source.front())) != 0) {
    return std::nullopt;
  }
  auto scanned = [&] {
    if constexpr (std::is_integral_v<Number>) {
      return scn::scan_int<Number>(source);
    } else {
      return scn::scan_value<Number>(source);
    }
  }();
  if (!scanned || scanned->begin() != scanned->end()) {
    return std::nullopt;
  }
  return scanned->value();
}

awj::compat::expected<int, std::string> parse_quality(std::wstring_view text) {
  // 同时接受 90、q90 和 0.9；内部统一归一化到 native 编码质量 1..100。
  auto quality_text = lower_copy(text);
  if (!quality_text.empty() && quality_text.front() == L'q') {
    quality_text.erase(quality_text.begin());
  }
  const auto value = scan_number<double>(quality_text);
  if (!value) {
    return awj::compat::unexpected{"质量参数必须是数字，例如 90 或 q90。"};
  }

  if (!std::isfinite(*value)) {
    return awj::compat::unexpected{"质量参数必须是有限数字。"};
  }

  const double normalized =
      (*value > 0.0 && *value <= 1.0) ? (*value * 100.0) : *value;
  if (normalized < 0.5 || normalized >= 100.5) {
    return awj::compat::unexpected{"质量范围必须在 1 到 100 之间。"};
  }
  const int quality = static_cast<int>(std::lround(normalized));
  return quality;
}

awj::compat::expected<int, std::string> parse_int_range(std::wstring_view text,
                                                int min_value, int max_value,
                                                std::string_view name) {
  const auto value = scan_number<int>(text);
  if (!value) {
    return awj::compat::unexpected{awj::compat::format("{} 必须是整数。", name)};
  }
  if (*value < min_value || *value > max_value) {
    return awj::compat::unexpected{awj::compat::format("{} 范围必须在 {} 到 {} 之间。", name,
                                       min_value, max_value)};
  }
  return *value;
}

awj::compat::expected<int, std::string> parse_auto_jobs(std::wstring_view text) {
  // CLI 和 UI 都允许“自动/auto/jthread”，避免用户必须手算预留线程数。
  const auto lower = lower_copy(text);
  if (lower == L"auto" || lower == L"jthread" || lower == L"自动") {
    return default_max_jobs();
  }
  return parse_int_range(text, 1, 128, "并发数量");
}

awj::compat::expected<std::vector<unsigned>, std::string> parse_cpu_affinity(std::wstring_view text) {
  std::vector<unsigned> cpus;
  const auto parse_id = [](std::wstring_view value) -> awj::compat::expected<int, std::string> {
    if (value.empty() || !awj::compat::ranges::all_of(value, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
      return awj::compat::unexpected{"cpu-affinity 使用从 0 开始的 CPU 编号，例如 0-3,6。"};
    return parse_int_range(value, 0, static_cast<int>(cpu_affinity_capacity() - 1), "CPU 编号");
  };
  do {
    const auto comma = text.find(L',');
    const auto item = text.substr(0, comma);
    const auto dash = item.find(L'-');
    const auto first = parse_id(item.substr(0, dash));
    const auto last = dash == std::wstring_view::npos ? first : parse_id(item.substr(dash + 1));
    if (!first) return awj::compat::unexpected{first.error()};
    if (!last) return awj::compat::unexpected{last.error()};
    if (*last < *first) return awj::compat::unexpected{"cpu-affinity 范围必须从小到大。"};
    for (auto cpu = *first; cpu <= *last; ++cpu) {
      if (awj::compat::ranges::find(cpus, static_cast<unsigned>(cpu)) != cpus.end())
        return awj::compat::unexpected{"cpu-affinity 不允许重复 CPU 编号。"};
      cpus.push_back(static_cast<unsigned>(cpu));
    }
    if (comma == std::wstring_view::npos) break;
    text.remove_prefix(comma + 1);
  } while (true);
  return cpus;
}

awj::compat::expected<std::uint64_t, std::string> parse_memory_limit(
    std::wstring_view text) {
  const auto lower = lower_copy(text);
  if (lower == L"auto" || lower == L"自动") {
    return 0ull;
  }
  std::wstring number_part{lower};
  std::uint64_t multiplier = 1;
  const auto consume_suffix = [&](std::wstring_view suffix,
                                  std::uint64_t value) {
    if (number_part.ends_with(suffix)) {
      number_part.resize(number_part.size() - suffix.size());
      multiplier = value;
      return true;
    }
    return false;
  };
  consume_suffix(L"gib", 1024ull * 1024ull * 1024ull) ||
      consume_suffix(L"gb", 1000ull * 1000ull * 1000ull) ||
      consume_suffix(L"mib", 1024ull * 1024ull) ||
      consume_suffix(L"mb", 1000ull * 1000ull) ||
      consume_suffix(L"kib", 1024ull) || consume_suffix(L"kb", 1000ull) ||
      consume_suffix(L"b", 1ull);
  const auto value = scan_number<double>(number_part);
  if (!value || !std::isfinite(*value) || *value < 0.0) {
    return awj::compat::unexpected{
        "memory-limit 必须是 auto 或非负数字，可带 KiB/MiB/GiB 后缀。"};
  }
  const double scaled = *value * static_cast<double>(multiplier);
  constexpr double uint64_limit =
      static_cast<double>(std::numeric_limits<std::uint64_t>::max());
  if (!std::isfinite(scaled) || scaled >= uint64_limit) {
    return awj::compat::unexpected{"memory-limit 超过运行时可表示范围。"};
  }
  if (scaled > 0.0 && scaled < 1.0) {
    return awj::compat::unexpected{
        "memory-limit 不能小于 1 字节；使用 auto 可由程序自动估算。"};
  }
  return static_cast<std::uint64_t>(scaled);
}

awj::compat::expected<double, std::string> parse_double_range(std::wstring_view text,
                                                      double min_value,
                                                      double max_value,
                                                      std::string_view name) {
  const auto value = scan_number<double>(text);
  if (!value) {
    return awj::compat::unexpected{awj::compat::format("{} 必须是数字。", name)};
  }
  if (!std::isfinite(*value)) {
    return awj::compat::unexpected{awj::compat::format("{} 必须是有限数字。", name)};
  }
  if (*value < min_value || *value > max_value) {
    return awj::compat::unexpected{awj::compat::format("{} 范围必须在 {:.3f} 到 {:.3f} 之间。",
                                       name, min_value, max_value)};
  }
  return *value;
}

awj::compat::expected<void, std::string> validate_native_option(std::wstring_view value,
                                                        std::string_view name) {
  constexpr std::size_t max_option_length = 512;
  if (value.empty()) {
    return awj::compat::unexpected{awj::compat::format("{} 不能为空。", name)};
  }
  if (value.size() > max_option_length) {
    return awj::compat::unexpected{awj::compat::format("{} 长度不能超过 512 个字符。", name)};
  }
  const auto pos = value.find(L'=');
  const auto key =
      pos == std::wstring_view::npos ? value : value.substr(0, pos);
  if (key.empty()) {
    return awj::compat::unexpected{awj::compat::format("{} 的 key 不能为空。", name)};
  }
  for (const wchar_t ch : value) {
    if (ch < 0x20 || ch == 0x7f) {
      return awj::compat::unexpected{awj::compat::format("{} 不能包含控制字符。", name)};
    }
  }
  return {};
}

awj::compat::expected<void, std::string> validate_native_option(
    std::wstring_view value) {
  return validate_native_option(value, "--option");
}

awj::compat::expected<void, std::string> validate_native_key_value_option(
    std::wstring_view value, std::string_view name) {
  if (auto valid = validate_native_option(value, name); !valid) {
    return awj::compat::unexpected{valid.error()};
  }
  if (value.find(L'=') == std::wstring_view::npos) {
    return awj::compat::unexpected{awj::compat::format("{} 必须为 key=value。", name)};
  }
  return {};
}

std::optional<OutputFormat> parse_output_format(std::wstring_view value) {
  auto lower = lower_copy(value);
  if (!lower.empty() && lower.front() == L'.') {
    lower.erase(lower.begin());
  }
  if (lower == L"png") {
    return OutputFormat::png;
  }
  if (lower == L"avif") {
    return OutputFormat::avif;
  }
  if (lower == L"webp") {
    return OutputFormat::webp;
  }
  if (lower == L"jxl") {
    return OutputFormat::jxl;
  }
  if (lower == L"jpg" || lower == L"jpeg" || lower == L"jpgli" || lower == L"jpegli") {
    return OutputFormat::jpgli;
  }
  return std::nullopt;
}

std::optional<CollisionMode> parse_collision(std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"overwrite" || lower == L"replace" || lower == L"覆盖") {
    return CollisionMode::overwrite;
  }
  if (lower == L"skip" || lower == L"skip-existing" || lower == L"跳过") {
    return CollisionMode::skip;
  }
  if (lower == L"time" || lower == L"suffix-time" || lower == L"时间") {
    return CollisionMode::suffix_time;
  }
  if (lower == L"random" || lower == L"suffix-random" || lower == L"随机") {
    return CollisionMode::suffix_random;
  }
  if (lower == L"number" || lower == L"suffix-number" ||
      lower == L"numeric" || lower == L"copy" || lower == L"编号") {
    return CollisionMode::suffix_number;
  }
  return std::nullopt;
}

std::optional<ChromaMode> parse_chroma(std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"auto" || lower == L"keep" || lower == L"保持" ||
      lower == L"自动") {
    return ChromaMode::auto_keep;
  }
  if (lower == L"444" || lower == L"4:4:4") {
    return ChromaMode::yuv444;
  }
  if (lower == L"422" || lower == L"4:2:2") {
    return ChromaMode::yuv422;
  }
  if (lower == L"400" || lower == L"4:0:0") return ChromaMode::yuv400;
  if (lower == L"420" || lower == L"4:2:0") {
    return ChromaMode::yuv420;
  }
  return std::nullopt;
}

std::string chroma_name(ChromaMode mode) {
  switch (mode) {
    case ChromaMode::yuv444:
      return "444";
    case ChromaMode::yuv422:
      return "422";
    case ChromaMode::yuv420:
      return "420";
    case ChromaMode::yuv400:
      return "400";
    case ChromaMode::auto_keep:
    default:
      return "auto";
  }
}

std::optional<AvifColorRepresentation> parse_avif_color_representation(
    std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"yuv" || lower == L"ycbcr" || lower == L"默认") {
    return AvifColorRepresentation::yuv;
  }
  if (lower == L"source" || lower == L"follow-source" || lower == L"跟随源") {
    return AvifColorRepresentation::source;
  }
  if (lower == L"rgb" || lower == L"rgba" || lower == L"gbr" ||
      lower == L"identity") {
    return AvifColorRepresentation::rgb_identity;
  }
  return std::nullopt;
}

std::string avif_color_representation_name(AvifColorRepresentation value) {
  switch (value) {
    case AvifColorRepresentation::source:
      return "source";
    case AvifColorRepresentation::rgb_identity:
      return "rgb";
    case AvifColorRepresentation::yuv:
    default:
      return "yuv";
  }
}

std::optional<AvifEncoderMode> parse_avif_encoder(std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"auto" || lower == L"automatic" || lower == L"自动") {
    return AvifEncoderMode::automatic;
  }

  if (lower == L"aom" || lower == L"libaom") {
    return AvifEncoderMode::aom;
  }

  return std::nullopt;
}

std::optional<AlphaModePolicy> parse_alpha_policy(std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"force" || lower == L"on" || lower == L"keep" ||
      lower == L"forced" || lower == L"强制开启" || lower == L"开启") {
    return AlphaModePolicy::force;
  }
  if (lower == L"auto" || lower == L"automatic" || lower == L"自动") {
    return AlphaModePolicy::automatic;
  }
  if (lower == L"off" || lower == L"disable" || lower == L"disabled" ||
      lower == L"strip" || lower == L"关闭" || lower == L"删除") {
    return AlphaModePolicy::off;
  }
  return std::nullopt;
}

std::string alpha_policy_name(AlphaModePolicy policy) {
  switch (policy) {
    case AlphaModePolicy::force:
      return "force";
    case AlphaModePolicy::off:
      return "off";
    case AlphaModePolicy::automatic:
    default:
      return "auto";
  }
}

std::optional<ImageSizeLimitMode> parse_image_size_limit_mode(std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"auto" || lower == L"automatic" || lower == L"common" || lower == L"自动") {
    return ImageSizeLimitMode::automatic;
  }
  if (lower == L"none" || lower == L"unlimited" || lower == L"off" || lower == L"无限制") {
    return ImageSizeLimitMode::none;
  }
  if (lower == L"manual" || lower == L"custom" || lower == L"手动") return ImageSizeLimitMode::manual;
  if (lower == L"upscale") return ImageSizeLimitMode::upscale;
  if (lower == L"downscale") return ImageSizeLimitMode::downscale;
  if (lower == L"fixed") return ImageSizeLimitMode::fixed;
  return std::nullopt;
}

std::string avif_encoder_name(AvifEncoderMode mode) {
  switch (mode) {
    case AvifEncoderMode::aom:
      return "aom";
    case AvifEncoderMode::automatic:
    default:
      return "auto";
  }
}

bool avif_bit_depth_supported(int bit_depth) noexcept {
  return bit_depth == 8 || bit_depth == 10 || bit_depth == 12;
}

bool jpegli_progressive_level_supported(int level) noexcept {
  return level >= 0 && level <= 2;
}

}  // namespace config_detail

std::string chroma_mode_name(ChromaMode mode) {
  return config_detail::chroma_name(mode);
}

std::string avif_encoder_mode_name(AvifEncoderMode mode) {
  return config_detail::avif_encoder_name(mode);
}

std::string avif_color_representation_name(
    AvifColorRepresentation value) {
  return config_detail::avif_color_representation_name(value);
}

std::string alpha_mode_policy_name(AlphaModePolicy policy) {
  return config_detail::alpha_policy_name(policy);
}

awj::compat::expected<void, std::string> validate_native_option(
    std::wstring_view value) {
  return config_detail::validate_native_option(value);
}

awj::compat::expected<int, std::string> parse_quality(std::wstring_view text) {
  return config_detail::parse_quality(text);
}

awj::compat::expected<int, std::string> parse_auto_jobs(std::wstring_view text) {
  return config_detail::parse_auto_jobs(text);
}

awj::compat::expected<std::uint64_t, std::string> parse_memory_limit(
    std::wstring_view text) {
  return config_detail::parse_memory_limit(text);
}

awj::compat::expected<double, std::string> parse_scale_percent(std::wstring_view text) {
  return config_detail::parse_double_range(text, std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::max(), "scale-percent");
}

awj::compat::expected<void, std::string> validate_config(const AppConfig& cfg) {
  if (cfg.avif_animation_keyframe < 0 || static_cast<int>(cfg.avif_animation_tune) < 0 || static_cast<int>(cfg.avif_animation_tune) > 2)
    return awj::compat::unexpected{"动画参数无效。"};
  if (auto valid = validate_image_size_limit(cfg.image_size_limit); !valid) return valid;
  if (auto valid = validate_avif_layers(cfg.avif_layers, cfg.output_format == OutputFormat::avif,
                                       cfg.quality, cfg.visual_quality); !valid) return valid;
  // 路径存在性在 scan_images 中校验；这里专注于格式自身不能违反的编码约束。
  if (cfg.output_template.size() > config_detail::max_output_template_length) {
    return awj::compat::unexpected{
        awj::compat::format("输出命名模板长度不能超过 {} 个字符。",
                    config_detail::max_output_template_length)};
  }
  if (cfg.append_png_suffix && cfg.output_format != OutputFormat::avif) {
    return awj::compat::unexpected{
        "--append-png-suffix 仅可与 AVIF 输出一起使用。"};
  }
  switch (cfg.output_format) {
    case OutputFormat::png:
      if (cfg.visual_quality) {
        return awj::compat::unexpected{"PNG 不支持视觉质量搜索；请使用 --quality。"};
      }
      if (cfg.speed) {
        return awj::compat::unexpected{"PNG 不支持 --speed；请移除该参数。"};
      }
      if (cfg.bit_depth && *cfg.bit_depth != 8 && *cfg.bit_depth != 16) {
        return awj::compat::unexpected{"PNG 输出仅支持 8-bit 或 16-bit RGBA；请把位深设为 8/16，或留空。"};
      }
      if (!supports_chroma_mode(cfg.output_format, cfg.chroma_mode)) {
        return awj::compat::unexpected{"PNG 不支持手动选择 444/422/420；请将 chroma 设为 auto。"};
      }
      break;
    case OutputFormat::avif:
      if (cfg.avif_encoder != AvifEncoderMode::automatic && cfg.avif_encoder != AvifEncoderMode::aom) {
        return awj::compat::unexpected{"AVIF 编码器已移除或无效；请使用 auto/aom。"};
      }
      if (cfg.bit_depth &&
          !config_detail::avif_bit_depth_supported(*cfg.bit_depth)) {
        return awj::compat::unexpected{
            "当前 native AVIF 输出仅支持 8、10、12-bit "
            "位深；留空表示自动选择。"};
      }
      if (cfg.avif_color_representation ==
          AvifColorRepresentation::rgb_identity) {
        if (!supports_chroma_mode(cfg.output_format, cfg.chroma_mode,
                                  cfg.avif_color_representation)) {
          return awj::compat::unexpected{
              "RGB(A)/GBR(A) Identity AVIF 必须使用 4:4:4；请使用 --chroma auto/444。"};
        }

        if (cfg.matrix_coefficients && *cfg.matrix_coefficients != 0) {
          return awj::compat::unexpected{
              "RGB(A)/GBR(A) Identity 与非 Identity matrix-coefficients 冲突。"};
        }
      } else if (cfg.avif_color_representation ==
                     AvifColorRepresentation::yuv &&
                 cfg.matrix_coefficients && *cfg.matrix_coefficients == 0) {
        return awj::compat::unexpected{
            "YUV 颜色表示不能使用 Identity matrix-coefficients；请选择 RGB(A)/GBR(A) 或移除该参数。"};
      }

      break;
    case OutputFormat::webp:
      if (cfg.bit_depth && *cfg.bit_depth != 8) {
        return awj::compat::unexpected{
            "WebP bitstream 只支持 8-bit；请把位深设为 8，或留空保持原片。"};
      }
      if (!supports_chroma_mode(cfg.output_format, cfg.chroma_mode)) {
        return awj::compat::unexpected{
            "WebP 不支持手动选择 444/422/420；有损 WebP 为 8-bit 4:2:0，"
            "无损 WebP 为 8-bit ARGB。"};
      }
      break;
    case OutputFormat::jxl:
      if (cfg.bit_depth && *cfg.bit_depth != 8 && *cfg.bit_depth != 10 && *cfg.bit_depth != 12 && *cfg.bit_depth != 16)
        return awj::compat::unexpected{"JXL 输出仅支持 8/10/12/16-bit；请使用支持的位深或留空按源选择 10/12/16-bit。"};
      if (!supports_chroma_mode(cfg.output_format, cfg.chroma_mode)) {
        return awj::compat::unexpected{
            "JXL 不支持手动选择 444/422/420；请将 chroma 设为 auto。"};
      }
      break;
    case OutputFormat::jpgli:
      if (!supports_chroma_mode(cfg.output_format, cfg.chroma_mode))
        return awj::compat::unexpected{"400 强制灰度仅支持 AVIF；JPG 支持 auto/444/422/420。"};
      if (cfg.speed) {
        return awj::compat::unexpected{"JPG 不支持 --speed；请移除该参数。"};
      }
      if (cfg.bit_depth && *cfg.bit_depth != 8) {
        return awj::compat::unexpected{
            "JPG 底图使用 8-bit；HDR 通过 Gain Map 保存。请把输出位深设为 8，或留空。"};
      }
      if (!config_detail::jpegli_progressive_level_supported(
              cfg.jpegli_progressive_level)) {
        return awj::compat::unexpected{
            "JPG progressive level 只支持 0、1、2；0 表示顺序 JPEG。"};
      }
      if (cfg.jpegli_progressive_level > 0 && !cfg.jpegli_optimize_huffman) {
        return awj::compat::unexpected{
            "JPG 渐进 JPEG 需要优化哈夫曼表；若要关闭优化，请将渐进设为 0。"};
      }
      if (cfg.alpha_policy == AlphaModePolicy::force) {
        return awj::compat::unexpected{
            "JPG 不支持 alpha 输出；请使用 --alpha auto/off，或选择支持透明通道的格式。"};
      }
      break;
  }
  if (!visual_quality_weights_are_valid()) {
    return awj::compat::unexpected{
        "视觉质量权重配置无效：GMSD_WEIGHT + MSSSIM_WEIGHT 必须等于 1。"};
  }
  if (cfg.visual_quality &&
      (*cfg.visual_quality < 1 || *cfg.visual_quality > 100)) {
    return awj::compat::unexpected{"visual-quality 范围必须在 1 到 100 之间。"};
  }
  // visual_quality 未设置时 fallback 没有工作可做，因此允许默认值保留；一旦
  // 启用搜索，native backend 才读取该开关。
  if (!cfg.unlock_memory_limit && cfg.memory_limit_bytes > 0 &&
      cfg.memory_limit_bytes < 64ull * 1024ull * 1024ull) {
    return awj::compat::unexpected{
        "memory-limit 不能低于 64MiB；使用 auto 可由程序自动估算。"};
  }
  return {};
}

awj::compat::expected<void, std::string> validate_execution_config(const AppConfig& cfg) {
  if (cfg.avif_encoder != AvifEncoderMode::automatic && cfg.avif_encoder != AvifEncoderMode::aom) {
    return awj::compat::unexpected{"AVIF 编码器已移除；请将 avif_encoder 修正为 auto/aom。"};
  }
  return {};
}

awj::compat::expected<void, std::string> finalize_config_defaults(AppConfig& cfg,
                                                          bool quality_was_set,
                                                          bool preset_was_set) {
  cfg.write_log = cfg.write_summary = cfg.write_log || cfg.write_summary;
#ifndef _WIN32
  if (cfg.output_policy == OutputPolicy::shell) {
    cfg.visual_quality.reset();
    cfg.max_jobs = default_max_jobs();
    // Preserve the configured admission value, including while a session unlock
    // takes precedence; shell workers use the same effective-memory policy.
  }
#endif
  if (!quality_was_set && !preset_was_set) {
    cfg.quality = default_quality_for(cfg.output_format);
  }
  if (cfg.output_template.empty()) {
    cfg.output_template = encoding_defaults::default_output_template;
  }
#ifndef _WIN32
  cfg.allow_wic_fallback = false;
#endif
  return validate_config(cfg);
}

std::string help_text() {
  std::string help = R"(AWJimage C++23
=======================

默认后端：内置 native（libavif/AOM/WebP/JXL/JPG）
默认质量：PNG q100（无损），AVIF q@AVIF_QUALITY@，WebP q@WEBP_QUALITY@，JXL q@JXL_QUALITY@，JPG q@JPEGLI_QUALITY@
默认动画：GIF/APNG 输出 AVIF 时自动保留动画；零时长 GIF 使用 10ms，APNG 使用 1ms
质量范围：q1..q100；JXL 仅在8-bit输出计划与JPEG码流相符时允许可逆转封装，自动按源选10/12/16-bit并走像素编码；其他 WebP/JXL q100 为编码器无损；AVIF q100 仅对未请求改写色彩、alpha、位深或元数据的目标 YUV AVIF 输入原始流直通，其他输入使用 AOM 无损量化并按 auto 色度规则重编码；默认颜色表示始终为非 Identity 的 YUV

用法:
  AWJ [选项]

常用选项:
  -i, --input <路径>          输入文件或目录，默认 @INPUT_PATH@
  -o, --output <目录>         输出目录；默认与输入同目录
  -f, --format <png|avif|webp|jxl|jpg> 输出格式，默认 avif；PNG 默认 SDR 8-bit/HDR 16-bit，显式8/16优先；q100无损编码目标像素、q1–99量化RGB，JPG 生成 JPEG 兼容 bitstream，扩展名为 .jpg
  -q, --quality <1-100>       编码质量，PNG 默认 100；q1–99 降低 RGB 有效精度（16-bit 最低 10 位），alpha 保持原精度；PNG 不支持视觉质量搜索，AVIF 默认 @AVIF_QUALITY@，WebP 默认 @WEBP_QUALITY@，JXL 默认 @JXL_QUALITY@，JPG 默认 @JPEGLI_QUALITY@；JXL 仅在8-bit输出计划与JPEG码流相符时允许可逆转封装，自动按源选10/12/16-bit并走像素编码；其他 WebP/JXL 100 为编码器无损；JPG 100 表示最高质量 JPEG 兼容编码，不声明无损；AVIF 100 仅对未请求改写色彩、alpha、位深或元数据的目标 YUV AVIF 输入原始流直通，其他输入走 AOM 无损量化：默认 YUV 保留 420/422/444 采样但始终使用非 Identity matrix；--avif-color-representation source 仅在 RGB/RGBA 或 Identity 源时使用 RGB/GBR Identity；--avif-color-representation rgb 强制 Identity + 444。也接受 q90 或 0.9
  --visual-quality <1-100>   视觉质量目标；存在时覆盖 --quality，100：JXL 仅在显式8-bit且参数兼容时允许 JPEG 原始码流级可逆转封装，自动位深仍像素重编码；其他 WebP/JXL 按编码器无损语义处理，JPG 按最高质量 JPEG 兼容编码处理，AVIF 仅对未请求改写色彩、alpha、位深或元数据的目标 YUV AVIF 输入直通，其他输入走 AOM 无损量化；默认 YUV 不会因无损自动切换 Identity。1..99 自动搜索最小体积达标候选
                            1..99 会为每张图片重复编码、解码并使用 CPU 计算 luma、GMSD 与 MS-SSIM 指标；大图会明显增加耗时与内存，不需要自动质量搜索时请不要设置
  --visual-quality-fallback  visual_quality 搜索未达标时输出最接近目标的候选（默认）
  --no-visual-quality-fallback visual_quality 搜索未达标时失败
@WINDOWS_TIMESTAMP_OPTIONS@
  -d, --bit-depth <位深>      AVIF 8/10/12，自动按所选呈现取10/12；JXL 8/10/12/16，自动按所选呈现取10/12/16；PNG 8/16，SDR自动8、HDR自动16；WebP/JPG固定8
  --chroma <auto|444|422|420|400> AVIF/JPG 色度采样；AVIF 400 强制灰度并保留 alpha；JPG 支持 auto/444/422/420；AVIF auto 参考 avifenc：保留 YUV 源的 420/422/444，灰度使用 400，RGB/RGBA 与未知源使用 444；JPG auto 同样保留源 420/422/444，RGB/RGBA、灰度与未知源使用三分量 444；也可用 --444 / --422 / --420
  --avif-color-representation <yuv|source|rgb> AVIF 颜色表示；默认 yuv，source 跟随源的 YUV 或 RGB Identity，rgb 强制 RGB(A)/GBR(A) Identity 与 4:4:4（自动选择 AOM）
  --append-png-suffix        AVIF 输出额外添加 .png 后缀，实际字节仍为 AVIF
  --jpeg-hdr <auto|sdr|hdr> JPEG HDR 策略；默认 auto：auto/hdr 对 HDR 复用原 SDR，缺少时自动高光滚降生成 8-bit SDR base + ISO Gain Map；SDR 输入始终单层；sdr 仅输出原 SDR 或自动滚降的 SDR；HDR Gain Map 不支持 visual-quality
  --avif-animation-tune <auto|ssim|psnr> 动画优化；默认 auto 使用编码器默认值；静态图保留 IQ
  --avif-animation-keyframe <N> 动画最大关键帧间隔；0 自动（默认），正整数指定最大帧间隔
  --avif-progressive / --no-avif-progressive 自动两层渐进；首层半尺寸 q10，末层按请求质量；默认关闭
  --avif-layers <2..4>       手动渐进总层数，与自动模式互斥；各层序号从 1 开始
  --avif-layer-size <序号:宽x高> 设置该层像素尺寸，最后一层必须保持最终尺寸
  --avif-layer-scale <序号:分子/分母> 按最终尺寸缩放，与同层 size 互斥
  --avif-layer-quality <序号:1..100> 设置各层颜色质量，未填沿用普通质量
  --avif-layer-alpha-quality <序号:1..100> 设置各层 alpha 质量，未填沿用普通质量
                            预览层至少指定尺寸、比例或质量；不支持 Gain Map、动画或 Grid 组合
                            示例：--avif-progressive
                            三层：--avif-layers 3 --avif-layer-scale 1:1/4 --avif-layer-quality 1:10 --avif-layer-scale 2:1/2 --avif-layer-quality 2:35
  --jpeg-progressive-level <0|1|2> JPG 渐进级别；0 为顺序 JPEG，默认 2
  --jpeg-optimize-huffman / --no-jpeg-optimize-huffman JPG 优化哈夫曼表；渐进级别大于 0 时必须开启
  --jpeg-xyb               JPG 启用 XYB 模式（实验）
  --avif-encoder <auto|aom>   AVIF 使用 AOM；单图上限 65536 边/2^30 像素，超限自动使用 AOM Grid
  --alpha <force|auto|off>   透明通道策略：force 强制保留源 alpha，auto 保留非不透明 alpha，off 总是删除 alpha；AVIF 颜色与 alpha 都跟随请求质量，auto chroma 按源图选择
  --color-primaries <n>      AVIF CICP color primaries；未指定时保留源值（含 BT.2020 等 HDR 源），源与用户都没有时才回退 BT.709；AOM/libavif 可应用
  --transfer-characteristics <n> AVIF CICP transfer characteristics；未指定时保留源值（含 PQ/HLG 等 HDR 传输函数），源与用户都没有时才回退 sRGB；AOM/libavif 可应用
  --matrix-coefficients <n>  AVIF CICP matrix coefficients；未指定时保留源的有效非 Identity 值；YUV 中 Identity/未知值会对 BT.2020 源回退 BT.2020 NCL，其他源回退 BT.709；Identity 仅可与 --avif-color-representation source/rgb 配合；AOM/libavif 可应用
  --color-range <0|1>        AVIF color range；显式指定时覆盖源值，未指定时保留源 PC/full 或 TV/limited，未知源使用 full；AOM/libavif 可应用
  --experimental-clamped-grid-padding 允许 AVIF grid 规划在无可整除方案时使用较小的右列和底行 cell，保持原始尺寸不变，默认开启；奇数输出尺寸或奇数 cell 尺寸与 420/422 色度仍不兼容，需要 --chroma 444
  --no-experimental-clamped-grid-padding 禁用 AVIF grid 较小边缘 cell；不可整除分割会报错
  -p, --preset <名称>         从程序同目录 preset/ 按 JSONC 内 name 加载用户预设；未指定使用当前内置默认
  --preset-file <路径>        加载并自动兼容 JSONC 预设；高优先级 CLI > 显式参数 > formats > 低优先级 CLI
  --list-presets              列出程序同目录 preset/ 中可用的用户预设及简介
  -t, --threads <auto|数量>   总线程预算；auto 按允许的逻辑 CPU 数预留桌面余量，显式数量不扣除预留（1-128）
  --cpu-affinity <编号列表>  限定逻辑 CPU，例如 0-3,6；编号从 0 开始，必须在进程允许集合内；Windows 使用当前处理器组
  --memory-limit <auto|大小>  工作集准入预算（非进程内存硬上限）；auto 为总内存 80% 与可用内存 50% 的较小值，可用 4GiB/4096MiB
  --unlock-memory-limit / --no-unlock-memory-limit 会话内解除/恢复工作集预算；解除优先于手动和 auto，不清除手动值，不写入配置
  --unlock-max-input-file-bytes / --unlock-20gib-limit 会话内解除物理总内存 80% 的输入/缓冲区保护，不改变工作集预算（兼容入口，默认关闭，不写入配置）
  --no-unlock-max-input-file-bytes / --no-unlock-20gib-limit 保持物理总内存 80% 的输入/缓冲区保护；--unlock-memory-limit 优先解除全部内存预算
  --image-size-limit <auto|none|upscale|downscale|fixed> 自动／无限制／取最大倍率／取最小倍率／固定尺寸或比例；兼容 manual
  --width/--height/--long-edge/--short-edge <像素> 可组合填写；固定模式等比居中并补透明或黑边，其他手动模式按倍率取最大／最小
  -m, --template <模板>       输出命名，最多 512 个字符，默认 @OUTPUT_TEMPLATE@
  --speed <0-10>             统一速度参数；支持 AVIF/WebP/JXL；JPG/PNG 不支持
  --allow-wic-fallback       Windows: 允许 native 解码器失败后使用 WIC 兜底；Linux: 接受但忽略
  --no-wic-fallback          禁用 WIC 解码兜底
  --close-on-finish / --no-close-on-finish 右键转换窗口完成后是否自动关闭
  --option <key=value>       预留 native 高级选项，可重复；当前版本只记录与校验
  --collision <策略>          overwrite / skip / number / time / random，默认 number
  --timeout-encode <分钟>     单张图片编码超时，默认 @ENCODE_TIMEOUT@
  --strip                    去除 EXIF/ICC 等元数据，通常更小且更隐私
  --keep-metadata            保留源元数据，取消 --strip
  --log / --summary          同时写入 log/awj.log 与 log/summery.csv；--summary 为兼容别名
  --no-log / --no-summary    关闭两种日志；与启用选项按最后一次生效
  --delete-source            每个输出成功落盘后移入系统回收站；失败、取消、跳过或无法安全回收时保留源
  --no-delete-source         保留源文件（默认）；不提供永久删除降级
  --skip-existing            已有输出时跳过
  --overwrite                已有输出时覆盖（显式选择）
  --suffix-number            输出名按 name(1)、name(2) 避免重名
  --suffix-time              输出名追加时间后缀
  --suffix-random            输出名追加随机后缀
  --scale-percent <正数>      缩放至百分比，支持小数和超过 100%；固定模式中与四项尺寸互斥
  --jxl-jpeg-lossless         允许在显式8-bit且其他参数兼容时可逆转封装 JPEG；自动位深仍走像素编码
  --no-jxl-jpeg-lossless      禁用 JPEG 到 JXL 的可逆转封装优化
  --version                  显示编译版本
  --help                     显示帮助

模板变量:
  {index}  图片序号，从 1 开始
  {name}   原文件名，不含扩展名
  {ext}    原扩展名，不含点
  {date}   扫描日期，例如 20260514
  {time}   扫描时间，例如 193005
  {datetime} 日期时间，例如 20260514-193005
  {unix}   Unix 时间戳
  {rand}   每张图一个随机 8 位十六进制
  {hash}   文件内容 FNV-1a 64 位哈希
  {hash8}  文件内容哈希前 8 位
  {sha256} 文件内容 SHA-256
  {sha2568} 文件内容 SHA-256 前 8 位
  {params} 编码参数，例如 q95t5 或 q95t5_444_10

示例:
  AWJ -i "D:\图片" -o Avifoutput -q q90
  AWJ -i input --format webp --template "{name}-{date}"
  AWJ -i input.png -o output.jxl --format jxl -q 90
  AWJ -i input.png --format jpg -q 95
  AWJ -i input --visual-quality 92 --summary
  AWJ -i input --chroma 444 --bit-depth 10
)";
  const auto replace_all = [](std::string& text, std::string_view from,
                              std::string_view to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
      text.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replace_all(help, "@AVIF_QUALITY@",
              awj::compat::format("{}", default_quality_for(OutputFormat::avif)));
  replace_all(help, "@WEBP_QUALITY@",
              awj::compat::format("{}", default_quality_for(OutputFormat::webp)));
  replace_all(help, "@JXL_QUALITY@",
              awj::compat::format("{}", default_quality_for(OutputFormat::jxl)));
  replace_all(help, "@JPEGLI_QUALITY@",
              awj::compat::format("{}", default_quality_for(OutputFormat::jpgli)));
  replace_all(help, "@INPUT_PATH@", encoding_defaults::default_input_path_text);
  replace_all(help, "@WEBP_BIT_DEPTH@",
              awj::compat::format("{}", encoding_defaults::default_webp_bit_depth));
  replace_all(help, "@OUTPUT_TEMPLATE@",
              encoding_defaults::default_output_template_text);
#ifdef _WIN32
  replace_all(help, "@WINDOWS_TIMESTAMP_OPTIONS@",
              "  --preserve-creation-time    输出成功后保留源文件创建时间\n"
              "  --preserve-modification-time 输出成功后保留源文件修改时间\n"
              "  --preserve-access-time      输出成功后保留源文件访问时间\n"
              "  --stdin-wgc-rgba16f <宽>x<高> 从 stdin 读取单帧 DXGI R16G16B16A16_FLOAT 线性 scRGB；必须配合 -o，不能与 -i 同用\n");
#else
  replace_all(help, "@WINDOWS_TIMESTAMP_OPTIONS@", "");
#endif
  replace_all(
      help, "@ENCODE_TIMEOUT@",
      awj::compat::format("{}", encoding_defaults::default_encode_timeout_minutes));
  return help;
}

void print_help() {
  const auto help = help_text();
  awj::compat::println("{}", help);
}

awj::compat::expected<ParseResult, std::string> parse_arguments_impl(
    const std::vector<std::wstring>& args, AppConfig cfg,
    bool preset_base_active, ArgumentParsePolicy policy = {}) {
  // 每层独立检查重复参数，完整配置只在最后一层校验。
  bool preset_was_set = preset_base_active;
  bool quality_was_set = false;
  bool automatic_threads = policy.automatic_threads.value_or(!preset_base_active);
  bool input_was_set = false;
  bool affinity_was_set = false;
  bool layer_count_was_set = false;
  bool progressive_was_set = false;
  AvifLayerConfig explicit_layers{};
  std::optional<int> explicit_threads;
  std::optional<std::wstring> preset_name;
  std::optional<std::filesystem::path> preset_file;
  std::vector<std::filesystem::path> shell_inputs;
  std::vector<std::wstring> positional_args;
  std::vector<std::string> warnings;
  std::array<std::optional<double>, 5> explicit_size_fields;
  std::optional<ImageSizeLimitMode> explicit_size_mode;
  const auto accept_size_field = [&](std::size_t slot, double value) {
    if (explicit_size_fields[slot] && *explicit_size_fields[slot] != value) return false;
    if (policy.override_inherited) {
      if (slot == 0 && std::none_of(explicit_size_fields.begin() + 1,
                                   explicit_size_fields.end(),
                                   [](const auto& field) { return field.has_value(); })) {
        cfg.image_size_limit.max_width.reset();
        cfg.image_size_limit.max_height.reset();
        cfg.image_size_limit.max_long_edge.reset();
        cfg.image_size_limit.max_short_edge.reset();
      } else if (slot != 0 && !explicit_size_fields[0]) {
        cfg.image_size_limit.scale_percent.reset();
      }
    }
    explicit_size_fields[slot] = value;
    return true;
  };

  auto require_value = [&](std::size_t& index, std::wstring_view option)
      -> awj::compat::expected<std::wstring, std::string> {
    if (index + 1 >= args.size()) {
      return awj::compat::unexpected{awj::compat::format(
          "{} 需要一个参数。请在该选项后补充具体值，例如路径、数字或枚举值。",
          config_detail::narrow_ascii_for_diagnostics(option))};
    }
    ++index;
    return args[index];
  };

  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto lower = config_detail::lower_copy(args[i]);
    if (policy.conversion_only &&
        (lower == L"-h" || lower == L"--help" || lower == L"--version" ||
         lower == L"-p" || lower == L"--preset" || lower == L"--preset-file" ||
         lower == L"--shell-convert" || lower == L"--output-policy" ||
         lower.starts_with(L"--studio-"))) {
      return awj::compat::unexpected{awj::compat::format(
          "预设 CLI 只能包含转换参数，不能使用控制选项 {}。",
          config_detail::narrow_ascii_for_diagnostics(args[i]))};
    }

    if (lower == L"-h" || lower == L"--help") {
      print_help();
      return ParseResult{.should_exit = true, .exit_code = 0, .config = cfg};
    }

    if (lower == L"--version") {
      awj::compat::println("AWJimage {}", AWJ_BUILD_VERSION);
      return ParseResult{.should_exit = true, .exit_code = 0, .config = cfg};
    }

    if (lower == L"--avif-progressive" || lower == L"--no-avif-progressive") {
      if (policy.override_inherited && !progressive_was_set &&
          !layer_count_was_set &&
          std::none_of(explicit_layers.layers.begin(), explicit_layers.layers.end(),
                       [](const auto& layer) { return layer.specified(); })) {
        cfg.avif_layers = {};
      }
      progressive_was_set = true;
      cfg.avif_layers.automatic = lower == L"--avif-progressive";
      continue;
    }
    if (lower == L"--avif-layers") {
      const auto value = require_value(i, args[i]);
      if (!value) return awj::compat::unexpected{value.error()};
      const auto count = config_detail::parse_int_range(*value, 2, max_avif_layers, "avif-layers");
      if (!count) return awj::compat::unexpected{count.error()};
      if (layer_count_was_set || (!policy.override_inherited && cfg.avif_layers.count))
        return awj::compat::unexpected{"渐进总层数不能重复指定。"};
      if (policy.override_inherited && !progressive_was_set)
        cfg.avif_layers.automatic = false;
      layer_count_was_set = true;
      cfg.avif_layers.count = *count;
      if (policy.override_inherited) {
        for (int slot = *count; slot < max_avif_layers; ++slot) {
          if (!explicit_layers.layers[slot].specified()) cfg.avif_layers.layers[slot] = {};
        }
      }
      continue;
    }
    if (lower == L"--avif-layer-size" || lower == L"--avif-layer-scale" ||
        lower == L"--avif-layer-quality" || lower == L"--avif-layer-alpha-quality") {
      const auto value = require_value(i, args[i]);
      if (!value) return awj::compat::unexpected{value.error()};
      if (policy.override_inherited) {
        if (auto set = set_avif_layer_option(explicit_layers, lower, *value); !set)
          return awj::compat::unexpected{set.error()};
        for (int slot = 0; slot < max_avif_layers; ++slot) {
          const auto& source = explicit_layers.layers[slot];
          auto& target = cfg.avif_layers.layers[slot];
          if (source.size || source.scale) {
            target.size = source.size;
            target.scale = source.scale;
          }
          if (source.quality) target.quality = source.quality;
          if (source.alpha_quality) target.alpha_quality = source.alpha_quality;
        }
      } else if (auto set = set_avif_layer_option(cfg.avif_layers, lower, *value); !set) {
        return awj::compat::unexpected{set.error()};
      }
      continue;
    }

    if (lower == L"--jxl-jpeg-lossless" || lower == L"--no-jxl-jpeg-lossless") {
      cfg.jxl_jpeg_lossless = lower == L"--jxl-jpeg-lossless";
      continue;
    }

    if (lower == L"--scale-percent") {
      const auto value = require_value(i, args[i]);
      if (!value) return awj::compat::unexpected{value.error()};
      const auto parsed = parse_scale_percent(*value);
      if (!parsed) return awj::compat::unexpected{parsed.error()};
      if (!accept_size_field(0, *parsed)) return awj::compat::unexpected{"重复的缩放比例互相矛盾。"};
      cfg.image_size_limit.scale_percent = *parsed;
      continue;
    }

    if (lower == L"-i" || lower == L"--input") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto path = normalize_path_argument(*value, "输入路径");
      if (!path) {
        return awj::compat::unexpected{path.error()};
      }
      cfg.input_path = *path;
      input_was_set = true;
      shell_inputs.push_back(*path);
      continue;
    }

    if (lower == L"-o" || lower == L"--output") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto path = normalize_path_argument(*value, "输出目录");
      if (!path) {
        return awj::compat::unexpected{path.error()};
      }
      cfg.output_dir = *path;
      continue;
    }

    if (lower == L"-f" || lower == L"--format" || lower == L"--output-format") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto format = config_detail::parse_output_format(*value);
      if (!format) {
        return awj::compat::unexpected{
            awj::compat::format("输出格式不支持: {}。可选值：png、avif、webp、jxl、jpg。",
                        config_detail::narrow_ascii_for_diagnostics(*value))};
      }
      cfg.output_format = *format;
      continue;
    }

    if (lower == L"--shell-convert") {
      cfg.output_policy = OutputPolicy::shell;
      continue;
    }

    if (lower == L"--output-policy") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto output_policy = config_detail::lower_copy(*value);
      if (output_policy == L"shell") {
        cfg.output_policy = OutputPolicy::shell;
      } else if (output_policy == L"normal") {
        cfg.output_policy = OutputPolicy::normal;
      } else {
        return awj::compat::unexpected{"output-policy 只支持 normal 或 shell。"};
      }
      continue;
    }

    if (lower == L"-m" || lower == L"--template" ||
        lower == L"--output-template") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      cfg.output_template = *value;
      continue;
    }

    if (lower == L"-p" || lower == L"--preset") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      if (value->empty()) {
        return awj::compat::unexpected{"--preset 名称不能为空。"};
      }
      preset_name = *value;
      continue;
    }

    if (lower == L"--preset-file") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto path = normalize_path_argument(*value, "预设文件路径");
      if (!path) {
        return awj::compat::unexpected{path.error()};
      }
      preset_file = *path;
      continue;
    }

    if (lower == L"-q" || lower == L"--quality") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto quality = config_detail::parse_quality(*value);
      if (!quality) {
        return awj::compat::unexpected{quality.error()};
      }
      cfg.quality = *quality;
      quality_was_set = true;
      continue;
    }

    if (lower == L"--visual-quality" || lower == L"--visual_quality") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto visual_quality =
          config_detail::parse_int_range(*value, 1, 100, "visual-quality");
      if (!visual_quality) {
        return awj::compat::unexpected{visual_quality.error()};
      }
      cfg.visual_quality = *visual_quality;
      continue;
    }

    if (lower == L"--studio-cancel-event") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      cfg.studio_cancel_event_name = *value;
      continue;
    }

    if (lower == L"--studio-queue-manifest") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      cfg.studio_queue_manifest = path_from_argument_text(*value);
      continue;
    }

    if (lower == L"--studio-input-limit-bytes") {
      const auto text = require_value(i, args[i]);
      if (!text) return awj::compat::unexpected{text.error()};
      const auto value = config_detail::scan_number<std::uint64_t>(*text);
      if (!value || !*value || cfg.studio_input_limit_bytes)
        return awj::compat::unexpected{"studio-input-limit-bytes 必须是非零 uint64 字节值且只能指定一次。"};
      cfg.studio_input_limit_bytes = *value;
      continue;
    }

    if (lower == L"--studio-large-action") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto lower_value = config_detail::lower_copy(*value);
      if (lower_value != L"grid") {
        return awj::compat::unexpected{"studio-large-action 只支持 grid；旧编码路径已移除。"};
      }
      cfg.studio_large_action = lower_value;
      continue;
    }


    if (lower == L"--large-image-priority") {
      return awj::compat::unexpected{"large-image-priority 已移除；超出单图上限时自动使用 AOM Grid。"};
    }

    if (lower == L"--unlock-memory-limit" || lower == L"--no-unlock-memory-limit") {
      cfg.unlock_memory_limit = lower == L"--unlock-memory-limit";
      continue;
    }

    if (lower == L"--unlock-max-input-file-bytes" ||
        lower == L"--unlock-20gib-limit") {
      cfg.unlock_max_input_file_bytes = true;
      continue;
    }

    if (lower == L"--no-unlock-max-input-file-bytes" ||
        lower == L"--no-unlock-20gib-limit") {
      cfg.unlock_max_input_file_bytes = false;
      continue;
    }

    if (lower == L"--visual-quality-fallback" ||
        lower == L"--visual_quality_fallback") {
      cfg.visual_quality_fallback = true;
      continue;
    }

    if (lower == L"--no-visual-quality-fallback" ||
        lower == L"--no_visual_quality_fallback") {
      cfg.visual_quality_fallback = false;
      continue;
    }

#ifdef _WIN32
    if (lower == L"--preserve-creation-time") {
      cfg.preserve_creation_time = true;
      continue;
    }
    if (lower == L"--preserve-modification-time") {
      cfg.preserve_modification_time = true;
      continue;
    }
    if (lower == L"--preserve-access-time") {
      cfg.preserve_access_time = true;
      continue;
    }
#endif

    if (lower == L"-d" || lower == L"--depth" || lower == L"--bit-depth" ||
        lower == L"--bitdepth") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto bit_depth =
          config_detail::parse_int_range(*value, 1, 16, "bit-depth");
      if (!bit_depth) {
        return awj::compat::unexpected{bit_depth.error()};
      }
      cfg.bit_depth = std::optional<int>{*bit_depth};
      continue;
    }

    if (lower == L"--chroma" || lower == L"--sampling" ||
        lower == L"--subsampling") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto chroma = config_detail::parse_chroma(*value);
      if (!chroma) {
        return awj::compat::unexpected{
            awj::compat::format("chroma 不支持: {}。可选值：auto、444、422、420、400。",
                        config_detail::narrow_ascii_for_diagnostics(*value))};
      }
      cfg.chroma_mode = *chroma;
      continue;
    }

    if (lower == L"--avif-color-representation" ||
        lower == L"--avif_color_representation") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto representation =
          config_detail::parse_avif_color_representation(*value);
      if (!representation) {
        return awj::compat::unexpected{awj::compat::format(
            "avif-color-representation 不支持: {}。可选值：yuv、source、rgb。",
            config_detail::narrow_ascii_for_diagnostics(*value))};
      }
      cfg.avif_color_representation = *representation;
      continue;
    }

    if (lower == L"--append-png-suffix") {
      cfg.append_png_suffix = true;
      continue;
    }
    if (lower == L"--jpeg-hdr" || lower == L"--avif-animation-tune") {
      const auto value = require_value(i, args[i]);
      if (!value) return awj::compat::unexpected{value.error()};
      const auto mode = config_detail::lower_copy(*value);
      if (lower == L"--jpeg-hdr") {
        if (mode == L"auto") cfg.jpeg_hdr = JpegHdrMode::automatic;
        else if (mode == L"sdr") cfg.jpeg_hdr = JpegHdrMode::sdr;
        else if (mode == L"hdr") cfg.jpeg_hdr = JpegHdrMode::hdr;
        else return awj::compat::unexpected{"jpeg-hdr 只支持 auto、sdr、hdr。"};
      } else {
        if (mode == L"auto") cfg.avif_animation_tune = AvifAnimationTune::automatic;
        else if (mode == L"ssim") cfg.avif_animation_tune = AvifAnimationTune::ssim;
        else if (mode == L"psnr") cfg.avif_animation_tune = AvifAnimationTune::psnr;
        else return awj::compat::unexpected{"avif-animation-tune 只支持 auto、ssim、psnr。"};
      }
      continue;
    }
    if (lower == L"--avif-animation-keyframe") {
      const auto value = require_value(i, args[i]);
      if (!value) return awj::compat::unexpected{value.error()};
      const auto count = config_detail::parse_int_range(*value, 0, std::numeric_limits<int>::max(), "avif-animation-keyframe");
      if (!count) return awj::compat::unexpected{count.error()};
      if (*count < 0) return awj::compat::unexpected{"avif-animation-keyframe 必须大于等于 0。"};
      cfg.avif_animation_keyframe = *count;
      continue;
    }
    if (lower == L"--preserve-animation") {
      // Accepted for compatibility with early 1.2.0 development commands.
      // AVIF/WebP 输出会自动保留受支持的动画。
      continue;
    }
    if (lower == L"--no-append-png-suffix") {
      cfg.append_png_suffix = false;
      continue;
    }

    if (lower == L"--jpeg-progressive-level" || lower == L"--jpegli-progressive-level" ||
        lower == L"--jpgli-progressive-level" ||
        lower == L"--jpegli-progressive_level" ||
        lower == L"--jpgli-progressive_level") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto level =
          config_detail::parse_int_range(*value, 0, 2, "jpeg-progressive-level");
      if (!level) {
        return awj::compat::unexpected{level.error()};
      }
      cfg.jpegli_progressive_level = *level;
      if (cfg.jpegli_progressive_level > 0) {
        cfg.jpegli_optimize_huffman = true;
      }
      continue;
    }

    if (lower == L"--jpeg-progressive" || lower == L"--jpegli-progressive" ||
        lower == L"--jpgli-progressive") {
      cfg.jpegli_progressive_level = 2;
      cfg.jpegli_optimize_huffman = true;
      continue;
    }

    if (lower == L"--no-jpeg-progressive" || lower == L"--no-jpegli-progressive" ||
        lower == L"--no-jpgli-progressive") {
      cfg.jpegli_progressive_level = 0;
      continue;
    }

    if (lower == L"--jpeg-optimize-huffman" || lower == L"--jpegli-optimize-huffman" ||
        lower == L"--jpgli-optimize-huffman" ||
        lower == L"--jpegli-optimize-huffman-table" ||
        lower == L"--jpgli-optimize-huffman-table") {
      cfg.jpegli_optimize_huffman = true;
      continue;
    }

    if (lower == L"--no-jpeg-optimize-huffman" || lower == L"--no-jpegli-optimize-huffman" ||
        lower == L"--no-jpgli-optimize-huffman" ||
        lower == L"--jpegli-fixed-huffman" ||
        lower == L"--jpgli-fixed-huffman" ||
        lower == L"--jpegli-fixed-code" ||
        lower == L"--jpgli-fixed-code") {
      cfg.jpegli_optimize_huffman = false;
      continue;
    }

    if (lower == L"--jpeg-xyb" || lower == L"--jpegli-xyb" || lower == L"--jpgli-xyb" ||
        lower == L"--jpegli-xyz" || lower == L"--jpgli-xyz") {
      cfg.jpegli_xyb = true;
      continue;
    }

    if (lower == L"--no-jpeg-xyb" || lower == L"--no-jpegli-xyb" || lower == L"--no-jpgli-xyb" ||
        lower == L"--no-jpegli-xyz" || lower == L"--no-jpgli-xyz") {
      cfg.jpegli_xyb = false;
      continue;
    }

    if (lower == L"--avif-encoder" || lower == L"--avif_encoder") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto encoder = config_detail::parse_avif_encoder(*value);
      if (!encoder) {
        return awj::compat::unexpected{
            awj::compat::format("AVIF encoder 不支持: "
                        "{}。仅支持 auto/aom；旧 SVT/Zen 编码器已移除。",
                        config_detail::narrow_ascii_for_diagnostics(*value))};
      }
      cfg.avif_encoder = *encoder;
      continue;
    }

    if (lower == L"--alpha" || lower == L"--alpha-mode" ||
        lower == L"--alpha_mode") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto alpha_policy = config_detail::parse_alpha_policy(*value);
      if (!alpha_policy) {
        return awj::compat::unexpected{
            awj::compat::format("alpha 不支持: {}。可选值：force、auto、off。",
                        config_detail::narrow_ascii_for_diagnostics(*value))};
      }
      cfg.alpha_policy = *alpha_policy;
      continue;
    }

    if (lower == L"--experimental-encoders" || lower == L"--enable-experimental-encoders" ||
        lower == L"--no-experimental-encoders") {
      return awj::compat::unexpected{"experimental-encoders 已移除；AVIF 仅支持 AOM。"};
    }

    if (lower == L"--experimental-clamped-grid-padding") {
      cfg.experimental_clamped_grid_padding = true;
      continue;
    }

    if (lower == L"--no-experimental-clamped-grid-padding") {
      cfg.experimental_clamped_grid_padding = false;
      continue;
    }

    if (lower == L"--444" || lower == L"--yuv444") {
      cfg.chroma_mode = ChromaMode::yuv444;
      continue;
    }

    if (lower == L"--422" || lower == L"--yuv422") {
      cfg.chroma_mode = ChromaMode::yuv422;
      continue;
    }

    if (lower == L"--420" || lower == L"--yuv420") {
      cfg.chroma_mode = ChromaMode::yuv420;
      continue;
    }

    if (lower == L"-t" || lower == L"-j" || lower == L"--threads" ||
        lower == L"--jobs" || lower == L"--max-jobs") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto jobs = config_detail::parse_auto_jobs(*value);
      if (!jobs) {
        return awj::compat::unexpected{jobs.error()};
      }
      cfg.max_jobs = *jobs;
      const auto mode = config_detail::lower_copy(*value);
      automatic_threads = mode == L"auto" || mode == L"jthread" || mode == L"自动";
      explicit_threads = automatic_threads ? std::nullopt : std::optional{*jobs};
      continue;
    }

    if (lower == L"--cpu-affinity") {
      if (affinity_was_set || (!policy.override_inherited && !cfg.cpu_affinity.empty()))
        return awj::compat::unexpected{"cpu-affinity 只能指定一次。"};
      const auto value = require_value(i, args[i]);
      if (!value) return awj::compat::unexpected{value.error()};
      auto cpus = config_detail::parse_cpu_affinity(*value);
      if (!cpus) return awj::compat::unexpected{cpus.error()};
      affinity_was_set = true;
      cfg.cpu_affinity = std::move(*cpus);
      continue;
    }

    if (lower == L"--memory-limit" || lower == L"--memory") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto memory_limit = config_detail::parse_memory_limit(*value);
      if (!memory_limit) {
        return awj::compat::unexpected{memory_limit.error()};
      }
      cfg.memory_limit_bytes = *memory_limit;
      continue;
    }

    if (lower == L"--image-size-limit") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto mode = config_detail::parse_image_size_limit_mode(*value);
      if (!mode) {
        return awj::compat::unexpected{"image-size-limit 支持 auto、none、upscale、downscale、fixed（兼容 manual）。"};
      }
      if (explicit_size_mode && *explicit_size_mode != *mode) return awj::compat::unexpected{"重复的尺寸模式互相矛盾。"};
      explicit_size_mode = *mode;
      cfg.image_size_limit.mode = *mode;
      continue;
    }

    if (lower == L"--width" || lower == L"--max-width" || lower == L"--image-max-width") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(*value, 1, 1000000, "max-width");
      if (!parsed) {
        return awj::compat::unexpected{parsed.error()};
      }
      if (!accept_size_field(1, *parsed)) return awj::compat::unexpected{"重复的宽度参数互相矛盾。"};
      cfg.image_size_limit.max_width = *parsed;
      continue;
    }

    if (lower == L"--height" || lower == L"--max-height" || lower == L"--image-max-height") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(*value, 1, 1000000, "max-height");
      if (!parsed) {
        return awj::compat::unexpected{parsed.error()};
      }
      if (!accept_size_field(2, *parsed)) return awj::compat::unexpected{"重复的高度参数互相矛盾。"};
      cfg.image_size_limit.max_height = *parsed;
      continue;
    }

    if (lower == L"--long-edge" || lower == L"--max-long-edge" || lower == L"--image-max-long-edge") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(*value, 1, 1000000, "max-long-edge");
      if (!parsed) {
        return awj::compat::unexpected{parsed.error()};
      }
      if (!accept_size_field(3, *parsed)) return awj::compat::unexpected{"重复的长边参数互相矛盾。"};
      cfg.image_size_limit.max_long_edge = *parsed;
      continue;
    }

    if (lower == L"--short-edge" || lower == L"--max-short-edge" || lower == L"--image-max-short-edge") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(*value, 1, 1000000, "max-short-edge");
      if (!parsed) {
        return awj::compat::unexpected{parsed.error()};
      }
      if (!accept_size_field(4, *parsed)) return awj::compat::unexpected{"重复的短边参数互相矛盾。"};
      cfg.image_size_limit.max_short_edge = *parsed;
      continue;
    }

    if (lower == L"--speed") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto speed = config_detail::parse_int_range(*value, 0, 10, "speed");
      if (!speed) {
        return awj::compat::unexpected{speed.error()};
      }
      cfg.speed = *speed;
      continue;
    }

    if (lower == L"--allow-wic-fallback") {
#ifdef _WIN32
      cfg.allow_wic_fallback = true;
#else
      cfg.allow_wic_fallback = false;
      warnings.emplace_back("Linux 不支持 WIC fallback；--allow-wic-fallback 已忽略。");
#endif
      continue;
    }

    if (lower == L"--no-wic-fallback") {
      cfg.allow_wic_fallback = false;
      continue;
    }

    if (lower == L"--close-on-finish") {
      cfg.shell_close_on_finish = true;
      continue;
    }

    if (lower == L"--no-close-on-finish") {
      cfg.shell_close_on_finish = false;
      continue;
    }

    if (lower.starts_with(L"--svtav1hdr-") || lower.starts_with(L"--svt-av1-hdr-") ||
        lower.starts_with(L"--zenrav") || lower == L"--mastering-display" ||
        lower == L"--content-light") {
      return awj::compat::unexpected{awj::compat::format("参数 {} 所属编码器已移除；请移除此参数并使用 auto/aom。",
          config_detail::narrow_ascii_for_diagnostics(args[i]))};
    }

    if (lower == L"--color-primaries") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto parsed =
          config_detail::parse_int_range(*value, 0, 255, "color-primaries");
      if (!parsed) {
        return awj::compat::unexpected{parsed.error()};
      }
      cfg.color_primaries = *parsed;
      continue;
    }

    if (lower == L"--transfer-characteristics") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(
          *value, 0, 255, "transfer-characteristics");
      if (!parsed) {
        return awj::compat::unexpected{parsed.error()};
      }
      cfg.transfer_characteristics = *parsed;
      continue;
    }

    if (lower == L"--matrix-coefficients") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto parsed =
          config_detail::parse_int_range(*value, 0, 255, "matrix-coefficients");
      if (!parsed) {
        return awj::compat::unexpected{parsed.error()};
      }
      cfg.matrix_coefficients = *parsed;
      continue;
    }

    if (lower == L"--color-range") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto parsed =
          config_detail::parse_int_range(*value, 0, 1, "color-range");
      if (!parsed) {
        return awj::compat::unexpected{parsed.error()};
      }
      cfg.color_range = *parsed;
      continue;
    }

    if (lower == L"--timeout-encode") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto timeout =
          config_detail::parse_int_range(*value, 1, 24 * 60, "timeout-encode");
      if (!timeout) {
        return awj::compat::unexpected{timeout.error()};
      }
      cfg.encode_timeout_minutes = *timeout;
      continue;
    }

    if (lower == L"--option") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      if (auto valid = config_detail::validate_native_option(*value); !valid) {
        return awj::compat::unexpected{valid.error()};
      }
      continue;
    }

    if (lower == L"--define") {
      return awj::compat::unexpected{
          "--define 是旧外部后端选项，当前版本仅支持 native 编码。"};
    }

    if (lower == L"--collision") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return awj::compat::unexpected{value.error()};
      }
      const auto collision = config_detail::parse_collision(*value);
      if (!collision) {
        return awj::compat::unexpected{awj::compat::format(
            "冲突策略不支持: {}。可选值：overwrite、skip、number、time、random。",
            config_detail::narrow_ascii_for_diagnostics(*value))};
      }
      cfg.collision_mode = *collision;
      continue;
    }

    if (lower == L"--strip") {
      cfg.strip_metadata = true;
      continue;
    }

    if (lower == L"--keep-metadata") {
      cfg.strip_metadata = false;
      continue;
    }

    if (lower == L"--overwrite") {
      cfg.collision_mode = CollisionMode::overwrite;
      continue;
    }

    if (lower == L"--skip-existing") {
      cfg.collision_mode = CollisionMode::skip;
      continue;
    }

    if (lower == L"--suffix-number") {
      cfg.collision_mode = CollisionMode::suffix_number;
      continue;
    }

    if (lower == L"--suffix-time") {
      cfg.collision_mode = CollisionMode::suffix_time;
      continue;
    }

    if (lower == L"--suffix-random") {
      cfg.collision_mode = CollisionMode::suffix_random;
      continue;
    }

    if (lower == L"--summary" || lower == L"--log" ||
        lower == L"--no-summary" || lower == L"--no-log") {
      // The last spelling wins, including across the legacy aliases.
      cfg.write_log = cfg.write_summary = lower == L"--summary" || lower == L"--log";
      continue;
    }

    if (lower == L"--delete-source" || lower == L"--no-delete-source") {
      cfg.delete_source = lower == L"--delete-source";
      continue;
    }

    if (!policy.conversion_only && !args[i].empty() && args[i].front() != L'-') {
      positional_args.push_back(args[i]);
      input_was_set = true;
      continue;
    }

    return awj::compat::unexpected{awj::compat::format(
        "未知参数: {}", config_detail::narrow_ascii_for_diagnostics(args[i]))};
  }

  if (cfg.output_policy == OutputPolicy::shell) {
    for (const auto& arg : positional_args) {
      shell_inputs.push_back(path_from_argument_text(arg));
    }
    if (shell_inputs.empty()) {
      shell_inputs.push_back(cfg.input_path);
    }
  } else if (!positional_args.empty()) {
    return awj::compat::unexpected{awj::compat::format(
        "未知参数: {}",
        config_detail::narrow_ascii_for_diagnostics(positional_args.front()))};
  }

  if (policy.finalize) {
    if (auto valid = finalize_config_defaults(cfg, quality_was_set, preset_was_set);
        !valid) return awj::compat::unexpected{valid.error()};
  }

  std::vector<std::filesystem::path> unique_shell_inputs;
  if (automatic_threads) {
    cfg.max_jobs = automatic_thread_budget(cfg.cpu_affinity.empty()
        ? available_processor_count() : static_cast<unsigned>(cfg.cpu_affinity.size()));
  } else if (explicit_threads) cfg.max_jobs = *explicit_threads;
  if (cfg.output_policy == OutputPolicy::shell) {
    std::vector<std::filesystem::path::string_type> seen_keys;
    unique_shell_inputs.reserve(shell_inputs.size());
    seen_keys.reserve(shell_inputs.size());
    for (const auto& input : shell_inputs) {
      const auto key = config_detail::shell_input_key(input);
      if (awj::compat::ranges::find(seen_keys, key) != seen_keys.end()) {
        continue;
      }
      seen_keys.push_back(key);
      unique_shell_inputs.push_back(input);
    }
  }

  return ParseResult{.should_exit = false,
                     .exit_code = 0,
                     .config = cfg,
                     .preset_name = std::move(preset_name),
                     .preset_file = std::move(preset_file),
                     .shell_inputs = std::move(unique_shell_inputs),
                     .warnings = std::move(warnings),
                     .automatic_threads = automatic_threads,
                     .input_was_set = input_was_set};
}

awj::compat::expected<ParseResult, std::string> parse_arguments(
    const std::vector<std::wstring>& args) {
  return parse_arguments_impl(args, default_app_config(), false);
}

awj::compat::expected<ParseResult, std::string> parse_arguments_with_preset_base(
    const std::vector<std::wstring>& args, AppConfig base) {
  return parse_arguments_impl(args, std::move(base), true);
}

// 传输已经解析的最终配置，不再次加载预设；供 worker 与契约测试共用。
std::vector<std::wstring> conversion_arguments_from_config(const AppConfig& cfg) {
  std::vector<std::wstring> args;
  args.reserve(120);
  const auto push = [&](std::wstring key, std::wstring value) {
    args.push_back(std::move(key));
    args.push_back(std::move(value));
  };
  const auto ascii = [](std::string_view text) { return std::wstring{text.begin(), text.end()}; };
  const auto path_text = [](const std::filesystem::path& path) {
#ifdef _WIN32
    return path.native();
#else
    return argument_text_from_native(path.native());
#endif
  };
  const auto optional = [&](std::wstring key, const auto& value) {
    if (value) push(std::move(key), awj::compat::format(L"{}", *value));
  };
  push(L"--input", path_text(cfg.input_path));
  if (!cfg.output_dir.empty()) push(L"--output", path_text(cfg.output_dir));
  const auto format = cfg.output_format == OutputFormat::png ? L"png"
      : cfg.output_format == OutputFormat::webp ? L"webp"
      : cfg.output_format == OutputFormat::jxl ? L"jxl"
      : cfg.output_format == OutputFormat::jpgli ? L"jpg" : L"avif";
  push(L"--format", format);
  push(L"--template", cfg.output_template);
  push(L"--threads", std::to_wstring(cfg.max_jobs));
  if (!cfg.cpu_affinity.empty()) {
    std::wstring cpus;
    for (const auto cpu : cfg.cpu_affinity) {
      if (!cpus.empty()) cpus += L',';
      cpus += std::to_wstring(cpu);
    }
    push(L"--cpu-affinity", std::move(cpus));
  }
  push(L"--memory-limit", cfg.memory_limit_bytes == 0 ? L"auto"
      : awj::compat::format(L"{}b", cfg.memory_limit_bytes));
  args.push_back(cfg.unlock_memory_limit ? L"--unlock-memory-limit" : L"--no-unlock-memory-limit");
  args.push_back(cfg.unlock_max_input_file_bytes ? L"--unlock-max-input-file-bytes"
                                                : L"--no-unlock-max-input-file-bytes");
  push(L"--image-size-limit", ascii(image_size_mode_name(cfg.image_size_limit.mode)));
  if (manual_image_size_mode(cfg.image_size_limit.mode)) {
    optional(L"--max-width", cfg.image_size_limit.max_width);
    optional(L"--max-height", cfg.image_size_limit.max_height);
    optional(L"--max-long-edge", cfg.image_size_limit.max_long_edge);
    optional(L"--max-short-edge", cfg.image_size_limit.max_short_edge);
    optional(L"--scale-percent", cfg.image_size_limit.scale_percent);
  }
  push(L"--timeout-encode", std::to_wstring(cfg.encode_timeout_minutes));
  const auto collision = cfg.collision_mode == CollisionMode::skip ? L"skip"
      : cfg.collision_mode == CollisionMode::suffix_time ? L"time"
      : cfg.collision_mode == CollisionMode::suffix_random ? L"random"
      : cfg.collision_mode == CollisionMode::overwrite ? L"overwrite" : L"number";
  push(L"--collision", collision);
  push(L"--quality", std::to_wstring(cfg.quality));
  optional(L"--visual-quality", cfg.visual_quality);
  args.push_back(cfg.visual_quality_fallback ? L"--visual-quality-fallback"
                                            : L"--no-visual-quality-fallback");
  optional(L"--bit-depth", cfg.bit_depth);
  optional(L"--speed", cfg.speed);
  args.push_back(cfg.allow_wic_fallback ? L"--allow-wic-fallback" : L"--no-wic-fallback");
  args.push_back(cfg.experimental_clamped_grid_padding ? L"--experimental-clamped-grid-padding"
                                                      : L"--no-experimental-clamped-grid-padding");
  args.push_back(cfg.strip_metadata ? L"--strip" : L"--keep-metadata");
  args.push_back(cfg.write_log || cfg.write_summary ? L"--log" : L"--no-log");
  args.push_back(cfg.delete_source ? L"--delete-source" : L"--no-delete-source");
#ifdef _WIN32
  if (cfg.preserve_creation_time) args.push_back(L"--preserve-creation-time");
  if (cfg.preserve_modification_time) args.push_back(L"--preserve-modification-time");
  if (cfg.preserve_access_time) args.push_back(L"--preserve-access-time");
#endif
  args.push_back(cfg.append_png_suffix ? L"--append-png-suffix" : L"--no-append-png-suffix");
  if (cfg.output_policy == OutputPolicy::shell) args.push_back(L"--shell-convert");
  args.push_back(cfg.shell_close_on_finish ? L"--close-on-finish" : L"--no-close-on-finish");
  if (cfg.output_format == OutputFormat::avif) {
    push(L"--avif-encoder", ascii(avif_encoder_mode_name(cfg.avif_encoder)));
    push(L"--avif-color-representation", ascii(avif_color_representation_name(cfg.avif_color_representation)));
    push(L"--chroma", ascii(chroma_mode_name(cfg.chroma_mode)));
    push(L"--alpha", ascii(alpha_mode_policy_name(cfg.alpha_policy)));
    push(L"--avif-animation-tune", std::wstring{avif_animation_tune_name(cfg.avif_animation_tune)});
    push(L"--avif-animation-keyframe", std::to_wstring(cfg.avif_animation_keyframe));
    if (cfg.avif_layers.automatic) args.push_back(L"--avif-progressive");
    optional(L"--avif-layers", cfg.avif_layers.count);
    for (int slot = 0; slot < max_avif_layers; ++slot) {
      const auto& layer = cfg.avif_layers.layers[slot];
      if (layer.size) push(L"--avif-layer-size", awj::compat::format(L"{}:{}x{}",
          slot + 1, layer.size->first, layer.size->second));
      if (layer.scale) push(L"--avif-layer-scale", awj::compat::format(L"{}:{}/{}",
          slot + 1, layer.scale->first, layer.scale->second));
      if (layer.quality) push(L"--avif-layer-quality", awj::compat::format(L"{}:{}", slot + 1, *layer.quality));
      if (layer.alpha_quality) push(L"--avif-layer-alpha-quality", awj::compat::format(L"{}:{}", slot + 1, *layer.alpha_quality));
    }
  }
  if (cfg.output_format == OutputFormat::jxl)
    args.push_back(cfg.jxl_jpeg_lossless ? L"--jxl-jpeg-lossless" : L"--no-jxl-jpeg-lossless");
  if (cfg.output_format == OutputFormat::jpgli) {
    push(L"--chroma", ascii(chroma_mode_name(cfg.chroma_mode)));
    push(L"--jpeg-progressive-level", std::to_wstring(cfg.jpegli_progressive_level));
    args.push_back(cfg.jpegli_optimize_huffman ? L"--jpeg-optimize-huffman" : L"--no-jpeg-optimize-huffman");
    args.push_back(cfg.jpegli_xyb ? L"--jpeg-xyb" : L"--no-jpeg-xyb");
    push(L"--jpeg-hdr", cfg.jpeg_hdr == JpegHdrMode::sdr ? L"sdr"
        : cfg.jpeg_hdr == JpegHdrMode::hdr ? L"hdr" : L"auto");
  }
  optional(L"--color-primaries", cfg.color_primaries);
  optional(L"--transfer-characteristics", cfg.transfer_characteristics);
  optional(L"--matrix-coefficients", cfg.matrix_coefficients);
  optional(L"--color-range", cfg.color_range);
  return args;
}

}  // namespace awj
