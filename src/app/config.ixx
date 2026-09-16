module;

#include <scn/scan.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <new>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include <utility>

export module awj.config;

import awj.encoding_defaults;
import awj.visual_quality;

export namespace awj {

enum class OutputFormat { png, avif, webp, jxl, jpgli };
enum class BackendMode { native };
enum class OutputPolicy { normal, shell };
enum class CollisionMode { overwrite, skip, suffix_time, suffix_random, suffix_number };
enum class ChromaMode { auto_keep, yuv444, yuv422, yuv420 };
// AVIF 的颜色表示与 YUV 4:2:0/4:2:2/4:4:4 采样是两件事。
enum class AvifColorRepresentation { yuv, source, rgb_identity };
enum class AvifEncoderMode { automatic, aom = 2 };
enum class AlphaModePolicy { force, automatic, off };
enum class ImageSizeLimitMode { automatic, none, manual };

struct ImageSizeLimit {
  ImageSizeLimitMode mode{ImageSizeLimitMode::automatic};
  std::optional<int> max_width{};
  std::optional<int> max_height{};
  std::optional<int> max_long_edge{};
  std::optional<int> max_short_edge{};
  std::optional<int> scale_percent{};
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

int default_max_jobs() noexcept {
  return automatic_thread_budget(std::thread::hardware_concurrency());
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
  CollisionMode collision_mode{CollisionMode::overwrite};
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
  bool jxl_jpeg_lossless{true};
  int max_jobs{default_max_jobs()};
  std::uint64_t memory_limit_bytes{
       encoding_defaults::default_memory_limit_bytes};
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
  bool visual_quality_gpu{true};
  bool strip_metadata{false};
  bool write_summary{false};
  bool write_log{false};
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
  // session-only unlock of 20 GiB input/runtime caps; never persist.
  bool unlock_max_input_file_bytes{false};
};

std::optional<std::pair<std::size_t, std::size_t>> limited_dimensions(
    std::size_t width, std::size_t height, const AppConfig& cfg) {
  if (width == 0 || height == 0 ||
      cfg.image_size_limit.mode == ImageSizeLimitMode::none) {
    return std::nullopt;
  }
  double scale = 1.0;
  const auto apply_edge = [&](std::optional<int> limit, std::size_t value) {
    if (limit && value > static_cast<std::size_t>(*limit)) {
      scale = std::min(scale, static_cast<double>(*limit) / static_cast<double>(value));
    }
  };
  if (cfg.image_size_limit.mode == ImageSizeLimitMode::automatic) {
    switch (cfg.output_format) {
      case OutputFormat::avif:
        // AOM/Grid preserves dimensions; only explicit manual limits resize AVIF.
        break;
      case OutputFormat::webp:
        apply_edge(16383, std::max(width, height));
        break;
      case OutputFormat::jpgli:
        apply_edge(65535, std::max(width, height));
        break;
      case OutputFormat::png:
      case OutputFormat::jxl:
      default:
        break;
    }
  } else {
    scale = cfg.image_size_limit.scale_percent.value_or(100) / 100.0;
    apply_edge(cfg.image_size_limit.max_width, width);
    apply_edge(cfg.image_size_limit.max_height, height);
    apply_edge(cfg.image_size_limit.max_long_edge, std::max(width, height));
    apply_edge(cfg.image_size_limit.max_short_edge, std::min(width, height));
  }
  if (scale >= 1.0) {
    return std::nullopt;
  }
  const auto target_width = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(width * scale)));
  const auto target_height = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(height * scale)));
  if (target_width == width && target_height == height) {
    return std::nullopt;
  }
  return std::pair{target_width, target_height};
}

AppConfig default_app_config() { return AppConfig{}; }

std::expected<std::filesystem::path, std::string> normalize_path_argument(
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
    return std::unexpected{std::format("{}不能为空。", label)};
  }
  const bool starts_quote = value.front() == L'\"';
  const bool ends_quote = value.back() == L'\"';
  if (starts_quote != ends_quote) {
    return std::unexpected{std::format(
        "{}的双引号必须成对并包住整个路径，例如 \"D:\\\\example.jxr\"。",
        label)};
  }
  if (starts_quote) {
    value.remove_prefix(1);
    value.remove_suffix(1);
    if (value.empty()) {
      return std::unexpected{std::format("{}不能为空。", label)};
    }
  }
  if (value.find(L'\"') != std::wstring_view::npos) {
    return std::unexpected{
        std::format("{}中的双引号只能成对包住整个路径。", label)};
  }
  return std::filesystem::path{value};
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
};

namespace config_detail {

constexpr std::size_t max_output_template_length = 512;

std::wstring lower_copy(std::wstring_view text) {
  std::wstring out{text};
  std::ranges::transform(out, out.begin(),
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

std::wstring shell_input_key(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  auto key = (ec ? path : absolute).lexically_normal().wstring();
#ifdef _WIN32
  std::ranges::transform(key, key.begin(),
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

std::expected<int, std::string> parse_quality(std::wstring_view text) {
  // 同时接受 90、q90 和 0.9；内部统一归一化到 native 编码质量 1..100。
  auto quality_text = lower_copy(text);
  if (!quality_text.empty() && quality_text.front() == L'q') {
    quality_text.erase(quality_text.begin());
  }
  const auto value = scan_number<double>(quality_text);
  if (!value) {
    return std::unexpected{"质量参数必须是数字，例如 90 或 q90。"};
  }

  if (!std::isfinite(*value)) {
    return std::unexpected{"质量参数必须是有限数字。"};
  }

  const double normalized =
      (*value > 0.0 && *value <= 1.0) ? (*value * 100.0) : *value;
  if (normalized < 0.5 || normalized >= 100.5) {
    return std::unexpected{"质量范围必须在 1 到 100 之间。"};
  }
  const int quality = static_cast<int>(std::lround(normalized));
  return quality;
}

std::expected<int, std::string> parse_int_range(std::wstring_view text,
                                                int min_value, int max_value,
                                                std::string_view name) {
  const auto value = scan_number<int>(text);
  if (!value) {
    return std::unexpected{std::format("{} 必须是整数。", name)};
  }
  if (*value < min_value || *value > max_value) {
    return std::unexpected{std::format("{} 范围必须在 {} 到 {} 之间。", name,
                                       min_value, max_value)};
  }
  return *value;
}

std::expected<int, std::string> parse_auto_jobs(std::wstring_view text) {
  // CLI 和 UI 都允许“自动/auto/jthread”，避免用户必须手算预留线程数。
  const auto lower = lower_copy(text);
  if (lower == L"auto" || lower == L"jthread" || lower == L"自动") {
    return default_max_jobs();
  }
  return parse_int_range(text, 1, 128, "并发数量");
}

std::expected<std::uint64_t, std::string> parse_memory_limit(
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
    return std::unexpected{
        "memory-limit 必须是 auto 或非负数字，可带 KiB/MiB/GiB 后缀。"};
  }
  const double scaled = *value * static_cast<double>(multiplier);
  constexpr double uint64_limit =
      static_cast<double>(std::numeric_limits<std::uint64_t>::max());
  if (!std::isfinite(scaled) || scaled >= uint64_limit) {
    return std::unexpected{"memory-limit 超过运行时可表示范围。"};
  }
  if (scaled > 0.0 && scaled < 1.0) {
    return std::unexpected{
        "memory-limit 不能小于 1 字节；使用 auto 可由程序自动估算。"};
  }
  return static_cast<std::uint64_t>(scaled);
}

std::expected<double, std::string> parse_double_range(std::wstring_view text,
                                                      double min_value,
                                                      double max_value,
                                                      std::string_view name) {
  const auto value = scan_number<double>(text);
  if (!value) {
    return std::unexpected{std::format("{} 必须是数字。", name)};
  }
  if (!std::isfinite(*value)) {
    return std::unexpected{std::format("{} 必须是有限数字。", name)};
  }
  if (*value < min_value || *value > max_value) {
    return std::unexpected{std::format("{} 范围必须在 {:.3f} 到 {:.3f} 之间。",
                                       name, min_value, max_value)};
  }
  return *value;
}

std::expected<void, std::string> validate_native_option(std::wstring_view value,
                                                        std::string_view name) {
  constexpr std::size_t max_option_length = 512;
  if (value.empty()) {
    return std::unexpected{std::format("{} 不能为空。", name)};
  }
  if (value.size() > max_option_length) {
    return std::unexpected{std::format("{} 长度不能超过 512 个字符。", name)};
  }
  const auto pos = value.find(L'=');
  const auto key =
      pos == std::wstring_view::npos ? value : value.substr(0, pos);
  if (key.empty()) {
    return std::unexpected{std::format("{} 的 key 不能为空。", name)};
  }
  for (const wchar_t ch : value) {
    if (ch < 0x20 || ch == 0x7f) {
      return std::unexpected{std::format("{} 不能包含控制字符。", name)};
    }
  }
  return {};
}

std::expected<void, std::string> validate_native_option(
    std::wstring_view value) {
  return validate_native_option(value, "--option");
}

std::expected<void, std::string> validate_native_key_value_option(
    std::wstring_view value, std::string_view name) {
  if (auto valid = validate_native_option(value, name); !valid) {
    return std::unexpected{valid.error()};
  }
  if (value.find(L'=') == std::wstring_view::npos) {
    return std::unexpected{std::format("{} 必须为 key=value。", name)};
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
  if (lower == L"jpgli" || lower == L"jpegli") {
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
  if (lower == L"manual" || lower == L"custom" || lower == L"手动") {
    return ImageSizeLimitMode::manual;
  }
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

std::expected<void, std::string> validate_native_option(
    std::wstring_view value) {
  return config_detail::validate_native_option(value);
}

std::expected<int, std::string> parse_quality(std::wstring_view text) {
  return config_detail::parse_quality(text);
}

std::expected<int, std::string> parse_auto_jobs(std::wstring_view text) {
  return config_detail::parse_auto_jobs(text);
}

std::expected<std::uint64_t, std::string> parse_memory_limit(
    std::wstring_view text) {
  return config_detail::parse_memory_limit(text);
}

std::expected<void, std::string> validate_config(const AppConfig& cfg) {
  if (cfg.image_size_limit.scale_percent &&
      (*cfg.image_size_limit.scale_percent < 1 || *cfg.image_size_limit.scale_percent > 100)) {
    return std::unexpected{"scale-percent 必须为 1～100 的整数。"};
  }
  // 路径存在性在 scan_images 中校验；这里专注于格式自身不能违反的编码约束。
  if (cfg.output_template.size() > config_detail::max_output_template_length) {
    return std::unexpected{
        std::format("输出命名模板长度不能超过 {} 个字符。",
                    config_detail::max_output_template_length)};
  }
  if (cfg.append_png_suffix && cfg.output_format != OutputFormat::avif) {
    return std::unexpected{
        "--append-png-suffix 仅可与 AVIF 输出一起使用。"};
  }
  switch (cfg.output_format) {
    case OutputFormat::png:
      if (cfg.visual_quality) {
        return std::unexpected{"PNG 不支持视觉质量搜索；请使用 --quality。"};
      }
      if (cfg.speed) {
        return std::unexpected{"PNG 不支持 --speed；请移除该参数。"};
      }
      if (cfg.bit_depth && *cfg.bit_depth != 8 && *cfg.bit_depth != 16) {
        return std::unexpected{"PNG 输出仅支持 8-bit 或 16-bit RGBA；请把位深设为 8/16，或留空。"};
      }
      if (cfg.chroma_mode != ChromaMode::auto_keep) {
        return std::unexpected{"PNG 不支持手动选择 444/422/420；请将 chroma 设为 auto。"};
      }
      break;
    case OutputFormat::avif:
      if (cfg.avif_encoder != AvifEncoderMode::automatic && cfg.avif_encoder != AvifEncoderMode::aom) {
        return std::unexpected{"AVIF 编码器已移除或无效；请使用 auto/aom。"};
      }
      if (cfg.bit_depth &&
          !config_detail::avif_bit_depth_supported(*cfg.bit_depth)) {
        return std::unexpected{
            "当前 native AVIF 输出仅支持 8、10、12-bit "
            "位深；留空表示自动选择。"};
      }
      if (cfg.avif_color_representation ==
          AvifColorRepresentation::rgb_identity) {
        if (cfg.chroma_mode != ChromaMode::auto_keep &&
            cfg.chroma_mode != ChromaMode::yuv444) {
          return std::unexpected{
              "RGB(A)/GBR(A) Identity AVIF 必须使用 4:4:4；请使用 --chroma auto/444。"};
        }

        if (cfg.matrix_coefficients && *cfg.matrix_coefficients != 0) {
          return std::unexpected{
              "RGB(A)/GBR(A) Identity 与非 Identity matrix-coefficients 冲突。"};
        }
      } else if (cfg.avif_color_representation ==
                     AvifColorRepresentation::yuv &&
                 cfg.matrix_coefficients && *cfg.matrix_coefficients == 0) {
        return std::unexpected{
            "YUV 颜色表示不能使用 Identity matrix-coefficients；请选择 RGB(A)/GBR(A) 或移除该参数。"};
      }

      break;
    case OutputFormat::webp:
      if (cfg.bit_depth && *cfg.bit_depth != 8) {
        return std::unexpected{
            "WebP bitstream 只支持 8-bit；请把位深设为 8，或留空保持原片。"};
      }
      if (cfg.chroma_mode != ChromaMode::auto_keep) {
        return std::unexpected{
            "WebP 不支持手动选择 444/422/420；有损 WebP 为 8-bit 4:2:0，"
            "无损 WebP 为 8-bit ARGB。"};
      }
      break;
    case OutputFormat::jxl:
      if (cfg.chroma_mode != ChromaMode::auto_keep) {
        return std::unexpected{
            "JXL 不支持手动选择 444/422/420；请将 chroma 设为 auto。"};
      }
      break;
    case OutputFormat::jpgli:
      if (cfg.speed) {
        return std::unexpected{"JPGLI 不支持 --speed；请移除该参数。"};
      }
      if (cfg.bit_depth && *cfg.bit_depth != 8) {
        return std::unexpected{
            "JPGLI 输出 JPEG 兼容 bitstream，当前仅支持 8-bit 输入写入；请把位深设为 8，或留空。"};
      }
      if (!config_detail::jpegli_progressive_level_supported(
              cfg.jpegli_progressive_level)) {
        return std::unexpected{
            "JPGLI progressive level 只支持 0、1、2；0 表示顺序 JPEG。"};
      }
      if (cfg.jpegli_progressive_level > 0 && !cfg.jpegli_optimize_huffman) {
        return std::unexpected{
            "JPGLI 渐进 JPEG 需要优化哈夫曼表；若要关闭优化，请将渐进设为 0。"};
      }
      if (cfg.alpha_policy == AlphaModePolicy::force) {
        return std::unexpected{
            "JPGLI 不支持 alpha 输出；请使用 --alpha auto/off，或选择支持透明通道的格式。"};
      }
      break;
  }
  if (!visual_quality_weights_are_valid()) {
    return std::unexpected{
        "视觉质量权重配置无效：GMSD_WEIGHT + MSSSIM_WEIGHT 必须等于 1。"};
  }
  if (cfg.visual_quality &&
      (*cfg.visual_quality < 1 || *cfg.visual_quality > 100)) {
    return std::unexpected{"visual-quality 范围必须在 1 到 100 之间。"};
  }
  // visual_quality 未设置时 fallback 没有工作可做，因此允许默认值保留；一旦
  // 启用搜索，native backend 才读取该开关。
  if (cfg.memory_limit_bytes > 0 &&
      cfg.memory_limit_bytes < 64ull * 1024ull * 1024ull) {
    return std::unexpected{
        "memory-limit 不能低于 64MiB；使用 auto 可由程序自动估算。"};
  }
  return {};
}

std::expected<void, std::string> validate_execution_config(const AppConfig& cfg) {
  if (cfg.avif_encoder != AvifEncoderMode::automatic && cfg.avif_encoder != AvifEncoderMode::aom) {
    return std::unexpected{"AVIF 编码器已移除；请将 avif_encoder 修正为 auto/aom。"};
  }
  return {};
}

std::expected<void, std::string> finalize_config_defaults(AppConfig& cfg,
                                                          bool quality_was_set,
                                                          bool preset_was_set) {
  if (cfg.output_policy == OutputPolicy::shell) {
    cfg.visual_quality.reset();
    cfg.max_jobs = default_max_jobs();
    cfg.memory_limit_bytes = 0;
  }
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

默认后端：内置 native（libavif/AOM/WebP/JXL/JPGLI）
默认质量：PNG q100（无损），AVIF q@AVIF_QUALITY@，WebP q@WEBP_QUALITY@，JXL q@JXL_QUALITY@，JPGLI q@JPEGLI_QUALITY@
质量范围：q1..q100；JXL 对 JPEG 输入优先使用原始码流级无损转封装，冲突时回退普通 JXL 编码；其他 WebP/JXL q100 为编码器无损；AVIF q100 仅对未请求改写色彩、alpha、位深或元数据的目标 YUV AVIF 输入原始流直通，其他输入使用 AOM 无损量化并按 auto 色度规则重编码；默认颜色表示始终为非 Identity 的 YUV

用法:
  AWJ [选项]

常用选项:
  -i, --input <路径>          输入文件或目录，默认 @INPUT_PATH@
  -o, --output <目录>         输出目录；默认与输入同目录
  -f, --format <png|avif|webp|jxl|jpgli|jpegli> 输出格式，默认 avif；PNG 保持 8/16-bit 存储位深，q100 无损、q1–99 量化 RGB，JPGLI 生成 JPEG 兼容 bitstream，扩展名为 .jpg
  -q, --quality <1-100>       编码质量，PNG 默认 100；q1–99 降低 RGB 有效精度（16-bit 最低 10 位），alpha 保持原精度；PNG 不支持视觉质量搜索，AVIF 默认 @AVIF_QUALITY@，WebP 默认 @WEBP_QUALITY@，JXL 默认 @JXL_QUALITY@，JPGLI 默认 @JPEGLI_QUALITY@；JXL 对 JPEG 输入优先使用原始码流级无损转封装，冲突时回退普通 JXL 编码；其他 WebP/JXL 100 为编码器无损；JPGLI 100 表示最高质量 JPEG 兼容编码，不声明无损；AVIF 100 仅对未请求改写色彩、alpha、位深或元数据的目标 YUV AVIF 输入原始流直通，其他输入走 AOM 无损量化：默认 YUV 保留 420/422/444 采样但始终使用非 Identity matrix；--avif-color-representation source 仅在 RGB/RGBA 或 Identity 源时使用 RGB/GBR Identity；--avif-color-representation rgb 强制 Identity + 444。也接受 q90 或 0.9
  --visual-quality <1-100>   视觉质量目标；存在时覆盖 --quality，100：JXL 对 JPEG 输入优先原始码流级无损转封装，其他 WebP/JXL 按编码器无损语义处理，JPGLI 按最高质量 JPEG 兼容编码处理，AVIF 仅对未请求改写色彩、alpha、位深或元数据的目标 YUV AVIF 输入直通，其他输入走 AOM 无损量化；默认 YUV 不会因无损自动切换 Identity。1..99 自动搜索最小体积达标候选
                            1..99 会为每张图片重复编码、解码并计算指标；大图会明显增加耗时与内存，不需要自动质量搜索时请不要设置
  --visual-quality-gpu       启用平台 GPU 加速 visual_quality 的 luma/GMSD/MS-SSIM 指标（Windows D3D11 / Linux Vulkan，默认）；codec 编码/解码仍走 native CPU 库，失败或小图会回退 CPU
  --no-visual-quality-gpu    禁用 visual_quality GPU 指标路径，固定使用 CPU metric 路径
  --visual-quality-fallback  visual_quality 搜索未达标时输出最接近目标的候选（默认）
  --no-visual-quality-fallback visual_quality 搜索未达标时失败
@WINDOWS_TIMESTAMP_OPTIONS@
  -d, --bit-depth <位深>      AVIF 支持 8/10/12；JXL 不填保持原片；WebP 固定 @WEBP_BIT_DEPTH@；JPGLI 输出 JPEG 兼容 8-bit precision
  --chroma <auto|444|422|420> AVIF/JPGLI 色度采样；AVIF auto 保留 YUV 源的 420/422/444，RGB/RGBA 转为 444，灰度或未知为 420；JPGLI auto 使用 Jpegli 默认采样；也可用 --444 / --422 / --420
  --avif-color-representation <yuv|source|rgb> AVIF 颜色表示；默认 yuv，source 跟随源的 YUV 或 RGB Identity，rgb 强制 RGB(A)/GBR(A) Identity 与 4:4:4（自动选择 AOM）
  --append-png-suffix        AVIF 输出额外添加 .png 后缀，实际字节仍为 AVIF
  --jpegli-progressive-level <0|1|2> JPGLI 渐进级别；0 为顺序 JPEG，默认 2
  --jpegli-optimize-huffman / --no-jpegli-optimize-huffman JPGLI 优化哈夫曼表；渐进级别大于 0 时必须开启
  --jpegli-xyb               JPGLI 启用 Jpegli XYB 模式（实验）
  --avif-encoder <auto|aom>   AVIF 使用 AOM；单图上限 65536 边/2^30 像素，超限自动使用 AOM Grid
  --alpha <force|auto|off>   透明通道策略：force 强制保留源 alpha，auto 保留非不透明 alpha，off 总是删除 alpha；AVIF 颜色与 alpha 都跟随请求质量，auto chroma 按源图选择
  --color-primaries <n>      AVIF CICP color primaries；未指定时保留源值（含 BT.2020 等 HDR 源），源与用户都没有时才回退 BT.709；AOM/libavif 可应用
  --transfer-characteristics <n> AVIF CICP transfer characteristics；未指定时保留源值（含 PQ/HLG 等 HDR 传输函数），源与用户都没有时才回退 sRGB；AOM/libavif 可应用
  --matrix-coefficients <n>  AVIF CICP matrix coefficients；未指定时保留源的有效非 Identity 值；YUV 中 Identity/未知值会对 BT.2020 源回退 BT.2020 NCL，其他源回退 BT.709；Identity 仅可与 --avif-color-representation source/rgb 配合；AOM/libavif 可应用
  --color-range <0|1>        AVIF color range；显式指定时覆盖源值，未指定时保留源 PC/full 或 TV/limited，未知源使用 full；AOM/libavif 可应用
  --experimental-clamped-grid-padding 允许 AVIF grid 规划在无可整除方案时使用较小的右列和底行 cell，保持原始尺寸不变，默认开启；奇数输出尺寸或奇数 cell 尺寸与 420/422 色度仍不兼容，需要 --chroma 444
  --no-experimental-clamped-grid-padding 禁用 AVIF grid 较小边缘 cell；不可整除分割会报错
  -p, --preset <名称>         从程序同目录 preset/ 按 JSONC 内 name 加载用户预设；未指定使用当前内置默认
  --preset-file <路径>        加载指定 JSONC 用户预设；显式 CLI 参数始终覆盖预设
  --list-presets              列出程序同目录 preset/ 中可用的用户预设及简介
  -t, --threads <auto|数量>   总线程预算；auto/jthread/自动 按 CPU 线程数预留桌面余量，预算精确拆分为编码器线程与文件并发
  --memory-limit <auto|大小>  内存限制；auto 为总内存 80% 与可用内存 50% 的较小值，可用 4GiB/4096MiB
  --unlock-max-input-file-bytes / --unlock-20gib-limit 会话内解除默认 20 GiB 输入/运行时上限（默认关闭，不写入配置）
  --no-unlock-max-input-file-bytes / --no-unlock-20gib-limit 保持默认 20 GiB 上限
  --image-size-limit <auto|none|manual> 图片边长限制；manual 可配合 --max-width/--max-height/--max-long-edge/--max-short-edge
  -m, --template <模板>       输出命名，最多 512 个字符，默认 @OUTPUT_TEMPLATE@
  --speed <0-10>             统一速度参数；支持 AVIF/WebP/JXL；JPGLI/PNG 不支持
  --allow-wic-fallback       Windows: 允许 native 解码器失败后使用 WIC 兜底；Linux: 接受但忽略
  --no-wic-fallback          禁用 WIC 解码兜底
  --close-on-finish / --no-close-on-finish 右键转换窗口完成后是否自动关闭
  --option <key=value>       预留 native 高级选项，可重复；当前版本只记录与校验
  --collision <策略>          overwrite / skip / number / time / random，默认 overwrite
  --timeout-encode <分钟>     单张图片编码超时，默认 @ENCODE_TIMEOUT@
  --strip                    去除 EXIF/ICC 等元数据，通常更小且更隐私
  --keep-metadata            保留源元数据，取消 --strip
  --summary                  写入 CSV 日志 summary.csv
  --no-summary               不写入 CSV 日志 summary.csv
  --log                      生成 log\awj.log
  --no-log                   不生成日志文件
  --skip-existing            已有输出时跳过
  --overwrite                已有输出时覆盖，默认行为
  --suffix-number            输出名按 name(1)、name(2) 避免重名
  --suffix-time              输出名追加时间后缀
  --suffix-random            输出名追加随机后缀
  --scale-percent <1-100>     手动尺寸缩小至百分比，留空等同 100%
  --jxl-jpeg-lossless         JPEG 转 JXL 默认无损转封装
  --no-jxl-jpeg-lossless      JPEG 转 JXL 使用普通质量编码
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
  AWJ -i input.png --format jpgli -q 95
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
              std::format("{}", default_quality_for(OutputFormat::avif)));
  replace_all(help, "@WEBP_QUALITY@",
              std::format("{}", default_quality_for(OutputFormat::webp)));
  replace_all(help, "@JXL_QUALITY@",
              std::format("{}", default_quality_for(OutputFormat::jxl)));
  replace_all(help, "@JPEGLI_QUALITY@",
              std::format("{}", default_quality_for(OutputFormat::jpgli)));
  replace_all(help, "@INPUT_PATH@", encoding_defaults::default_input_path_text);
  replace_all(help, "@WEBP_BIT_DEPTH@",
              std::format("{}", encoding_defaults::default_webp_bit_depth));
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
      std::format("{}", encoding_defaults::default_encode_timeout_minutes));
  return help;
}

void print_help() {
  const auto help = help_text();
  std::println("{}", help);
}

std::expected<ParseResult, std::string> parse_arguments_impl(
    const std::vector<std::wstring>& args, AppConfig cfg,
    bool preset_base_active) {
  // CLI 参数直接落到 AppConfig，确保命令行和 Slint UI 走同一套核心逻辑。
  bool preset_was_set = preset_base_active;
  bool quality_was_set = false;
  std::optional<std::wstring> preset_name;
  std::optional<std::filesystem::path> preset_file;
  std::vector<std::filesystem::path> shell_inputs;
  std::vector<std::wstring> positional_args;
  std::vector<std::string> warnings;

  auto require_value = [&](std::size_t& index, std::wstring_view option)
      -> std::expected<std::wstring, std::string> {
    if (index + 1 >= args.size()) {
      return std::unexpected{std::format(
          "{} 需要一个参数。请在该选项后补充具体值，例如路径、数字或枚举值。",
          config_detail::narrow_ascii_for_diagnostics(option))};
    }
    ++index;
    return args[index];
  };

  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto lower = config_detail::lower_copy(args[i]);

    if (lower == L"-h" || lower == L"--help") {
      print_help();
      return ParseResult{.should_exit = true, .exit_code = 0, .config = cfg};
    }

    if (lower == L"--version") {
      std::println("AWJimage {}", AWJ_BUILD_VERSION);
      return ParseResult{.should_exit = true, .exit_code = 0, .config = cfg};
    }

    if (lower == L"--jxl-jpeg-lossless" || lower == L"--no-jxl-jpeg-lossless") {
      cfg.jxl_jpeg_lossless = lower == L"--jxl-jpeg-lossless";
      continue;
    }

    if (lower == L"--scale-percent") {
      const auto value = require_value(i, args[i]);
      if (!value) return std::unexpected{value.error()};
      const auto parsed = config_detail::parse_int_range(*value, 1, 100, "scale-percent");
      if (!parsed) return std::unexpected{parsed.error()};
      cfg.image_size_limit.scale_percent = *parsed;
      continue;
    }

    if (lower == L"-i" || lower == L"--input") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto path = normalize_path_argument(*value, "输入路径");
      if (!path) {
        return std::unexpected{path.error()};
      }
      cfg.input_path = *path;
      shell_inputs.push_back(*path);
      continue;
    }

    if (lower == L"-o" || lower == L"--output") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto path = normalize_path_argument(*value, "输出目录");
      if (!path) {
        return std::unexpected{path.error()};
      }
      cfg.output_dir = *path;
      continue;
    }

    if (lower == L"-f" || lower == L"--format" || lower == L"--output-format") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto format = config_detail::parse_output_format(*value);
      if (!format) {
        return std::unexpected{
            std::format("输出格式不支持: {}。可选值：png、avif、webp、jxl、jpgli、jpegli。",
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
        return std::unexpected{value.error()};
      }
      const auto policy = config_detail::lower_copy(*value);
      if (policy == L"shell") {
        cfg.output_policy = OutputPolicy::shell;
      } else if (policy == L"normal") {
        cfg.output_policy = OutputPolicy::normal;
      } else {
        return std::unexpected{"output-policy 只支持 normal 或 shell。"};
      }
      continue;
    }

    if (lower == L"-m" || lower == L"--template" ||
        lower == L"--output-template") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.output_template = *value;
      continue;
    }

    if (lower == L"-p" || lower == L"--preset") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      if (value->empty()) {
        return std::unexpected{"--preset 名称不能为空。"};
      }
      preset_name = *value;
      continue;
    }

    if (lower == L"--preset-file") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto path = normalize_path_argument(*value, "预设文件路径");
      if (!path) {
        return std::unexpected{path.error()};
      }
      preset_file = *path;
      continue;
    }

    if (lower == L"-q" || lower == L"--quality") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto quality = config_detail::parse_quality(*value);
      if (!quality) {
        return std::unexpected{quality.error()};
      }
      cfg.quality = *quality;
      quality_was_set = true;
      continue;
    }

    if (lower == L"--visual-quality" || lower == L"--visual_quality") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto visual_quality =
          config_detail::parse_int_range(*value, 1, 100, "visual-quality");
      if (!visual_quality) {
        return std::unexpected{visual_quality.error()};
      }
      cfg.visual_quality = *visual_quality;
      continue;
    }

    if (lower == L"--studio-cancel-event") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.studio_cancel_event_name = *value;
      continue;
    }

    if (lower == L"--studio-queue-manifest") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.studio_queue_manifest = *value;
      continue;
    }

    if (lower == L"--studio-large-action") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto lower_value = config_detail::lower_copy(*value);
      if (lower_value != L"grid") {
        return std::unexpected{"studio-large-action 只支持 grid；旧编码路径已移除。"};
      }
      cfg.studio_large_action = lower_value;
      continue;
    }


    if (lower == L"--large-image-priority") {
      return std::unexpected{"large-image-priority 已移除；超出单图上限时自动使用 AOM Grid。"};
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

    if (lower == L"--visual-quality-gpu" || lower == L"--visual_quality_gpu") {
      cfg.visual_quality_gpu = true;
      continue;
    }

    if (lower == L"--no-visual-quality-gpu" ||
        lower == L"--no_visual_quality_gpu") {
      cfg.visual_quality_gpu = false;
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
        return std::unexpected{value.error()};
      }
      const auto bit_depth =
          config_detail::parse_int_range(*value, 1, 16, "bit-depth");
      if (!bit_depth) {
        return std::unexpected{bit_depth.error()};
      }
      cfg.bit_depth = std::optional<int>{*bit_depth};
      continue;
    }

    if (lower == L"--chroma" || lower == L"--sampling" ||
        lower == L"--subsampling") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto chroma = config_detail::parse_chroma(*value);
      if (!chroma) {
        return std::unexpected{
            std::format("chroma 不支持: {}。可选值：auto、444、422、420。",
                        config_detail::narrow_ascii_for_diagnostics(*value))};
      }
      cfg.chroma_mode = *chroma;
      continue;
    }

    if (lower == L"--avif-color-representation" ||
        lower == L"--avif_color_representation") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto representation =
          config_detail::parse_avif_color_representation(*value);
      if (!representation) {
        return std::unexpected{std::format(
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
    if (lower == L"--no-append-png-suffix") {
      cfg.append_png_suffix = false;
      continue;
    }

    if (lower == L"--jpegli-progressive-level" ||
        lower == L"--jpgli-progressive-level" ||
        lower == L"--jpegli-progressive_level" ||
        lower == L"--jpgli-progressive_level") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto level =
          config_detail::parse_int_range(*value, 0, 2, "jpegli-progressive-level");
      if (!level) {
        return std::unexpected{level.error()};
      }
      cfg.jpegli_progressive_level = *level;
      if (cfg.jpegli_progressive_level > 0) {
        cfg.jpegli_optimize_huffman = true;
      }
      continue;
    }

    if (lower == L"--jpegli-progressive" ||
        lower == L"--jpgli-progressive") {
      cfg.jpegli_progressive_level = 2;
      cfg.jpegli_optimize_huffman = true;
      continue;
    }

    if (lower == L"--no-jpegli-progressive" ||
        lower == L"--no-jpgli-progressive") {
      cfg.jpegli_progressive_level = 0;
      continue;
    }

    if (lower == L"--jpegli-optimize-huffman" ||
        lower == L"--jpgli-optimize-huffman" ||
        lower == L"--jpegli-optimize-huffman-table" ||
        lower == L"--jpgli-optimize-huffman-table") {
      cfg.jpegli_optimize_huffman = true;
      continue;
    }

    if (lower == L"--no-jpegli-optimize-huffman" ||
        lower == L"--no-jpgli-optimize-huffman" ||
        lower == L"--jpegli-fixed-huffman" ||
        lower == L"--jpgli-fixed-huffman" ||
        lower == L"--jpegli-fixed-code" ||
        lower == L"--jpgli-fixed-code") {
      cfg.jpegli_optimize_huffman = false;
      continue;
    }

    if (lower == L"--jpegli-xyb" || lower == L"--jpgli-xyb" ||
        lower == L"--jpegli-xyz" || lower == L"--jpgli-xyz") {
      cfg.jpegli_xyb = true;
      continue;
    }

    if (lower == L"--no-jpegli-xyb" || lower == L"--no-jpgli-xyb" ||
        lower == L"--no-jpegli-xyz" || lower == L"--no-jpgli-xyz") {
      cfg.jpegli_xyb = false;
      continue;
    }

    if (lower == L"--avif-encoder" || lower == L"--avif_encoder") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto encoder = config_detail::parse_avif_encoder(*value);
      if (!encoder) {
        return std::unexpected{
            std::format("AVIF encoder 不支持: "
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
        return std::unexpected{value.error()};
      }
      const auto policy = config_detail::parse_alpha_policy(*value);
      if (!policy) {
        return std::unexpected{
            std::format("alpha 不支持: {}。可选值：force、auto、off。",
                        config_detail::narrow_ascii_for_diagnostics(*value))};
      }
      cfg.alpha_policy = *policy;
      continue;
    }

    if (lower == L"--experimental-encoders" || lower == L"--enable-experimental-encoders" ||
        lower == L"--no-experimental-encoders") {
      return std::unexpected{"experimental-encoders 已移除；AVIF 仅支持 AOM。"};
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
        return std::unexpected{value.error()};
      }
      const auto jobs = config_detail::parse_auto_jobs(*value);
      if (!jobs) {
        return std::unexpected{jobs.error()};
      }
      cfg.max_jobs = *jobs;
      continue;
    }

    if (lower == L"--memory-limit" || lower == L"--memory") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto memory_limit = config_detail::parse_memory_limit(*value);
      if (!memory_limit) {
        return std::unexpected{memory_limit.error()};
      }
      cfg.memory_limit_bytes = *memory_limit;
      continue;
    }

    if (lower == L"--image-size-limit") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto mode = config_detail::parse_image_size_limit_mode(*value);
      if (!mode) {
        return std::unexpected{"image-size-limit 只支持 auto、none 或 manual。"};
      }
      cfg.image_size_limit.mode = *mode;
      continue;
    }

    if (lower == L"--max-width" || lower == L"--image-max-width") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(*value, 1, 1000000, "max-width");
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.image_size_limit.max_width = *parsed;
      continue;
    }

    if (lower == L"--max-height" || lower == L"--image-max-height") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(*value, 1, 1000000, "max-height");
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.image_size_limit.max_height = *parsed;
      continue;
    }

    if (lower == L"--max-long-edge" || lower == L"--image-max-long-edge") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(*value, 1, 1000000, "max-long-edge");
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.image_size_limit.max_long_edge = *parsed;
      continue;
    }

    if (lower == L"--max-short-edge" || lower == L"--image-max-short-edge") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(*value, 1, 1000000, "max-short-edge");
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.image_size_limit.max_short_edge = *parsed;
      continue;
    }

    if (lower == L"--speed") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto speed = config_detail::parse_int_range(*value, 0, 10, "speed");
      if (!speed) {
        return std::unexpected{speed.error()};
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
      return std::unexpected{std::format("参数 {} 所属编码器已移除；请移除此参数并使用 auto/aom。",
          config_detail::narrow_ascii_for_diagnostics(args[i]))};
    }

    if (lower == L"--color-primaries") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto parsed =
          config_detail::parse_int_range(*value, 0, 255, "color-primaries");
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.color_primaries = *parsed;
      continue;
    }

    if (lower == L"--transfer-characteristics") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto parsed = config_detail::parse_int_range(
          *value, 0, 255, "transfer-characteristics");
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.transfer_characteristics = *parsed;
      continue;
    }

    if (lower == L"--matrix-coefficients") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto parsed =
          config_detail::parse_int_range(*value, 0, 255, "matrix-coefficients");
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.matrix_coefficients = *parsed;
      continue;
    }

    if (lower == L"--color-range") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto parsed =
          config_detail::parse_int_range(*value, 0, 1, "color-range");
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.color_range = *parsed;
      continue;
    }

    if (lower == L"--timeout-encode") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto timeout =
          config_detail::parse_int_range(*value, 1, 24 * 60, "timeout-encode");
      if (!timeout) {
        return std::unexpected{timeout.error()};
      }
      cfg.encode_timeout_minutes = *timeout;
      continue;
    }

    if (lower == L"--option") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      if (auto valid = config_detail::validate_native_option(*value); !valid) {
        return std::unexpected{valid.error()};
      }
      continue;
    }

    if (lower == L"--define") {
      return std::unexpected{
          "--define 是旧外部后端选项，当前版本仅支持 native 编码。"};
    }

    if (lower == L"--collision") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto collision = config_detail::parse_collision(*value);
      if (!collision) {
        return std::unexpected{std::format(
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

    if (lower == L"--summary") {
      cfg.write_summary = true;
      continue;
    }

    if (lower == L"--no-summary") {
      cfg.write_summary = false;
      continue;
    }

    if (lower == L"--log") {
      cfg.write_log = true;
      continue;
    }

    if (lower == L"--no-log") {
      cfg.write_log = false;
      continue;
    }

    if (!args[i].empty() && args[i].front() != L'-') {
      positional_args.push_back(args[i]);
      continue;
    }

    return std::unexpected{std::format(
        "未知参数: {}", config_detail::narrow_ascii_for_diagnostics(args[i]))};
  }

  if (cfg.output_policy == OutputPolicy::shell) {
    for (const auto& arg : positional_args) {
      shell_inputs.emplace_back(arg);
    }
    if (shell_inputs.empty()) {
      shell_inputs.push_back(cfg.input_path);
    }
  } else if (!positional_args.empty()) {
    return std::unexpected{std::format(
        "未知参数: {}",
        config_detail::narrow_ascii_for_diagnostics(positional_args.front()))};
  }

  if (auto valid =
          finalize_config_defaults(cfg, quality_was_set, preset_was_set);
      !valid) {
    return std::unexpected{valid.error()};
  }

  std::vector<std::filesystem::path> unique_shell_inputs;
  if (cfg.output_policy == OutputPolicy::shell) {
    std::vector<std::wstring> seen_keys;
    unique_shell_inputs.reserve(shell_inputs.size());
    seen_keys.reserve(shell_inputs.size());
    for (const auto& input : shell_inputs) {
      const auto key = config_detail::shell_input_key(input);
      if (std::ranges::find(seen_keys, key) != seen_keys.end()) {
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
                     .warnings = std::move(warnings)};
}

std::expected<ParseResult, std::string> parse_arguments(
    const std::vector<std::wstring>& args) {
  return parse_arguments_impl(args, default_app_config(), false);
}

std::expected<ParseResult, std::string> parse_arguments_with_preset_base(
    const std::vector<std::wstring>& args, AppConfig base) {
  return parse_arguments_impl(args, std::move(base), true);
}

}  // namespace awj
