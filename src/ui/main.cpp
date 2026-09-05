#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <dwmapi.h>
#include <scn/scan.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <slint.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "awj_studio.h"
#include "changelog_history.h"
#include "file_drop_win32.h"
#include "import_service.h"
#include "path_picker_win32.h"
#include "shell_context_menu.hpp"

import awj.avif_aom_codec;
import awj.avif_registry;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.large_image_plan;
import awj.native_backend;
import awj.pipeline;
import awj.preset;
import awj.resource_planner;
import awj.studio_defaults;
import awj.update_model;
import awj.update_keyring;
import awj.update_manifest_v2;
import awj.update_runtime;
import awj.update_windows;

namespace {

awj::LargeImageDecision manual_large_image_decision(
    awj::ImageDimensions dimensions, bool grid_available,
    bool zenrav1e_available) {
  auto decision =
      awj::classify_large_image(dimensions, grid_available, zenrav1e_available);
  decision.klass = awj::LargeImageClass::large_mode_required;
  if (decision.reason == awj::LargeImageReason::none) {
    decision.reason_text = "用户手动加入大图队列。";
  }
  return decision;
}

struct Win32HandleDeleter {
  using pointer = HANDLE;
  void operator()(HANDLE value) const noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
};

using UniqueWin32Handle = std::unique_ptr<void, Win32HandleDeleter>;

// CreateFileW / CreateMailslotW 这类 API 失败时返回 INVALID_HANDLE_VALUE 而不是
// nullptr，而 unique_ptr 的 operator bool 只和 nullptr 比较——直接把返回值包进去，
// 失败会被当成成功，随后对 (HANDLE)-1 发起 I/O。统一在这里归一化成 nullptr，
// 让 `if (handle)` 对两类 API 都成立。返回 nullptr 的 API（CreateMutexW、
// CreateJobObjectW）走这里同样正确。
[[nodiscard]] UniqueWin32Handle adopt_win32_handle(HANDLE value) noexcept {
  return UniqueWin32Handle{value == INVALID_HANDLE_VALUE ? nullptr : value};
}

enum class QueueItemStatus {
  pending,
  running,
  done,
  failed,
  skipped,
  canceled
};

struct QueueImageItem {
  std::uint64_t id{};
  std::filesystem::path path{};
  std::filesystem::path source_root{};
  std::filesystem::path relative_dir{};
  std::uintmax_t bytes{};
  QueueItemStatus status{QueueItemStatus::pending};
  std::size_t run_index{std::numeric_limits<std::size_t>::max()};
  std::filesystem::path locked_output_path{};
  std::string status_text{"等待编码"};
  std::string log_text{};
  std::string encoder_id{};
  int encoder_threads{};
  double decode_seconds{-1.0};
  double prepare_seconds{-1.0};
  double encode_seconds{-1.0};
  double write_seconds{-1.0};
  bool warning{};
};

struct StudioChildProcess {
  UniqueWin32Handle process{};
  UniqueWin32Handle thread{};
  UniqueWin32Handle job{};
  UniqueWin32Handle cancel_event{};
  UniqueWin32Handle output_read{};
  std::wstring command_line{};
  std::filesystem::path queue_manifest_path{};
  std::vector<std::filesystem::path> temp_directories{};
  DWORD process_id{};
  std::atomic_bool cancel_requested{};
  std::mutex termination_mutex{};
  bool force_terminated{};
  bool process_tree_terminated{};

  void request_cancel() noexcept {
    cancel_requested.store(true, std::memory_order_release);
    if (cancel_event != nullptr) {
      SetEvent(cancel_event.get());
    }
  }

  bool terminate(DWORD exit_code =
                     awj::studio_defaults::worker_force_stop_exit_code) noexcept {
    std::scoped_lock lock{termination_mutex};
    if (process_tree_terminated) {
      return true;
    }
    request_cancel();
    if (job != nullptr && TerminateJobObject(job.get(), exit_code) != FALSE) {
      force_terminated = true;
      process_tree_terminated = true;
      return true;
    }
    if (!force_terminated && process != nullptr &&
        WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT &&
        TerminateProcess(process.get(), exit_code) != FALSE) {
      force_terminated = true;
      process_tree_terminated = true;
      return true;
    }
    return false;
  }

  bool was_force_terminated() noexcept {
    std::scoped_lock lock{termination_mutex};
    return force_terminated;
  }
};


struct MenuFormatParams {
  std::string quality_text{};
  std::string bit_depth_text{};
  std::string speed_text{};
  int avif_encoder_index{};
  int avif_color_representation_index{};
  int chroma_index{};
  int alpha_policy_index{1};
  int jpegli_progressive_index{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  bool strip_metadata{};
  bool allow_wic_fallback{true};
  bool close_on_finish{true};
  bool install_avif_png_command{};
  int size_limit_index{};
  std::string max_width_text{};
  std::string max_height_text{};
  std::string max_long_edge_text{};
  std::string max_short_edge_text{};

  bool operator==(const MenuFormatParams&) const = default;
};

// 参数页的五组会话内参数。它们从不写入 AWJ.jsonc；队列在启动时只取选中
// 格式的一个快照，因此切换“编辑格式”绝不会暗中改变队列输出格式。
struct ParameterFormatParams {
  std::string quality_text{};
  std::string visual_quality_text{};
  std::string bit_depth_text{};
  std::string speed_text{};
  int avif_encoder_index{};
  int avif_color_representation_index{};
  int chroma_index{};
  int alpha_policy_index{1};
  int jpegli_progressive_index{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  std::string threads_text{};
  std::string memory_limit_text{};
  int size_limit_index{};
  std::string max_width_text{};
  std::string max_height_text{};
  std::string max_long_edge_text{};
  std::string max_short_edge_text{};
};

struct StudioConfigSnapshot {
  int theme_index{};
  // 界面语言：0 = 中文（.slint 里的 msgid 原文），1 = English（bundled 翻译）。
  int language_index{};
  std::string ui_font_family{};
  bool allow_wic_fallback{};
  bool visual_quality_gpu{true};
  bool visual_quality_fallback{true};
  bool experimental_encoders{true};
  std::array<MenuFormatParams, 5> menu_params{};

  std::string update_channel{"stable"};
  bool show_update_changelog{true};
  bool hide_update_changelog_after_exit{true};
  bool show_update_changelog_after_update{true};
  std::string last_changelog_exit_version{};
  std::int64_t last_successful_update_check_at{};
  // schema 1 remains cached solely for already-installed 1.0.3 bridge
  // clients; current Studio uses the independent v2 replay counter.
  std::int64_t last_verified_manifest_sequence{};
  std::int64_t last_verified_manifest_v2_sequence{};
  std::string pending_update_version{};
  std::string pending_update_channel{};
  std::string pending_update_release_url{};
  std::string pending_update_published_at{};
  std::string pending_update_changelog_zh_cn{};
  std::string pending_update_changelog_en{};
  // 已通过 Ed25519 验证的 manifest 缓存。启动时会重新验签后才用于
  // 展示更新历史，避免把本地可写配置直接当成发布记录。
  std::string update_manifest_raw{};
  std::string update_manifest_signature{};
  std::string update_manifest_v2_raw{};
  std::string update_manifest_v2_signature{};
  std::string update_keyring_raw{};
  std::string update_keyring_signature{};

  bool operator==(const StudioConfigSnapshot&) const = default;
};

struct UiState {
  std::jthread worker{};
  std::jthread update_worker{};
  std::unique_ptr<awj::ui_import::Dispatcher> import_dispatcher{};
  std::optional<awj::ui_drop::Registration> native_drop{};
  slint::Timer native_drop_timer{};
  std::size_t native_drop_attempts{};
  bool native_drop_registration_finished{};
  std::shared_ptr<slint::VectorModel<TaskRow>> task_rows{};
  std::shared_ptr<slint::VectorModel<LargeImageRow>> large_image_rows{};
  std::shared_ptr<slint::VectorModel<UpdateHistoryRow>> update_history_rows{};
  std::vector<QueueImageItem> queue_items{};
  std::vector<awj::BatchLargeImageItem> large_image_items{};
  slint::Timer theme_timer{};
  slint::Timer update_timer{};
  slint::Timer config_timer{};
  std::optional<StudioConfigSnapshot> config_defaults{};
  std::optional<StudioConfigSnapshot> last_config_snapshot{};
  std::uint64_t run_id{};
  std::uint64_t next_queue_id{1};
  std::mutex mutex{};
  std::vector<awj::BatchProgress> pending_events{};
  std::shared_ptr<StudioChildProcess> active_child{};
  bool worker_active{};
  std::uint64_t last_click_id{};
  std::chrono::steady_clock::time_point last_click_time{};
  bool drag_reordered{};
  std::array<ParameterFormatParams, 5> builtin_params{};
  // 用户预设在参数页内有独立的编辑缓冲；只有点击保存才写入 preset/*.jsonc，
  // 因此它不会意外改变“内置默认”队列的会话参数。
  std::array<ParameterFormatParams, 5> parameter_preset_params{};
  std::array<MenuFormatParams, 5> menu_params{};
  std::vector<awj::UserPreset> user_presets{};
  std::vector<std::string> user_preset_errors{};
  int parameter_preset_index{};
  int last_format_index{};
  int last_menu_format_index{};

  std::string update_channel{"stable"};
  bool show_update_changelog{true};
  bool hide_update_changelog_after_exit{true};
  bool show_update_changelog_after_update{true};
  std::string last_changelog_exit_version{};
  std::int64_t last_successful_update_check_at{};
  std::int64_t last_verified_manifest_sequence{};
  std::int64_t last_verified_manifest_v2_sequence{};
  std::string pending_update_version{};
  std::string pending_update_channel{};
  std::string pending_update_release_url{};
  std::string pending_update_published_at{};
  std::string pending_update_changelog_zh_cn{};
  std::string pending_update_changelog_en{};
  std::string update_manifest_raw{};
  std::string update_manifest_signature{};
  std::string update_manifest_v2_raw{};
  std::string update_manifest_v2_signature{};
  std::string update_keyring_raw{};
  std::string update_keyring_signature{};
  bool update_check_active{};
  std::string update_status_zh{"尚未检查"};
  std::string update_status_en{"Not checked yet"};
};

LargeImageRow make_large_image_row(const awj::BatchLargeImageItem& item,
                                   std::string_view status);
int preferred_large_image_action_index(
    const awj::BatchLargeImageItem& item) noexcept;

std::string shared_to_string(const slint::SharedString& value) {
  return std::string{value.data(), value.size()};
}

slint::SharedString to_shared(std::string_view text);

std::expected<void, std::string> add_manual_large_image_path(
    UiState& state, const std::filesystem::path& path, bool allow_wic_fallback,
    bool grid_available, bool zenrav1e_available) {
  try {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      return std::unexpected{std::format("不是可添加的文件: {}。",
                                         awj::display_path_for_user(path))};
    }
    if (!awj::is_supported_image_extension(path)) {
      return std::unexpected{std::format("文件格式不受支持: {}。支持 {}。",
                                         awj::display_path_for_user(path),
                                         awj::kSupportedImageExtensionsText)};
    }
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec) {
      return std::unexpected{std::format("读取文件大小失败: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
    const auto dimensions = awj::probe_image_dimensions_for_path(
        path,
        awj::DecoderRegistryOptions{.allow_wic_fallback = allow_wic_fallback});
    if (!dimensions) {
      return std::unexpected{dimensions.error()};
    }
    const auto index = state.large_image_items.size();
    awj::ImageFile file{.index = index, .path = path, .bytes = bytes};
    auto decision = manual_large_image_decision(*dimensions, grid_available,
                                                zenrav1e_available);
    auto item = awj::BatchLargeImageItem{.file = std::move(file),
                                         .dimensions = *dimensions,
                                         .decision = std::move(decision)};
    auto row = make_large_image_row(item, "等待选择");
    state.large_image_items.push_back(std::move(item));
    state.large_image_rows->push_back(std::move(row));
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"添加大图任务时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"添加大图任务时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"添加大图任务时文件系统访问失败。"};
  }
}
std::string trim_copy(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  const auto first = std::ranges::find_if_not(value, is_space);
  const auto last =
      std::ranges::find_if_not(value | std::views::reverse, is_space).base();
  if (first >= last) {
    return {};
  }
  return std::string{first, last};
}

bool output_dir_is_empty(const AwjStudio& app) {
  return trim_copy(shared_to_string(app.get_output_dir())).empty();
}

void set_input_path_preserving_output(AwjStudio& app,
                                      const std::filesystem::path& path) {
  const bool should_fill_output = output_dir_is_empty(app);
  app.set_input_path(to_shared(awj::path_to_utf8(path)));
  if (should_fill_output) {
    app.set_output_dir(
        to_shared(awj::path_to_utf8(awj::default_output_dir_for(path))));
  }
}

std::expected<int, std::string> parse_int_field(std::string text,
                                                std::string_view name,
                                                int minimum, int maximum) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::unexpected{std::format("{} 不能为空。", name)};
  }
  const std::string_view source{text};
  const auto parsed = scn::scan_int<int>(source);
  if (!parsed || parsed->begin() != parsed->end()) {
    return std::unexpected{std::format("{} 必须是整数。", name)};
  }
  const int value = parsed->value();
  if (value < minimum || value > maximum) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", name, minimum, maximum)};
  }
  return value;
}

std::expected<int, std::string> parse_quality_field(std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::unexpected{"质量不能为空。"};
  }
  return awj::parse_quality(awj::wide_from_utf8(text));
}

std::expected<std::optional<int>, std::string> parse_optional_int_field(
    std::string text, std::string_view name, int minimum, int maximum) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::optional<int>{};
  }
  const auto value = parse_int_field(std::move(text), name, minimum, maximum);
  if (!value) {
    return std::unexpected{value.error()};
  }
  return std::optional<int>{*value};
}

std::expected<std::optional<int>, std::string> parse_visual_quality_field(
    std::string text) {
  return parse_optional_int_field(std::move(text), "视觉质量", 1, 100);
}

std::expected<int, std::string> parse_jobs_field(std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return awj::default_max_jobs();
  }
  return awj::parse_auto_jobs(awj::wide_from_utf8(text));
}

std::expected<std::optional<int>, std::string> parse_bit_depth_field(
    std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::optional<int>{};
  }
  const auto value = parse_int_field(std::move(text), "位深", 1, 16);
  if (!value) {
    return std::unexpected{value.error()};
  }
  return std::optional<int>{*value};
}

std::expected<std::uint64_t, std::string> parse_memory_limit_field(
    std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::uint64_t{};
  }
  const auto gib = parse_int_field(std::move(text), "内存", 1, 1024);
  if (!gib) {
    return std::unexpected{"内存只允许填写 GiB 数字，例如 4；留空表示自动。"};
  }
  return static_cast<std::uint64_t>(*gib) *
         awj::studio_defaults::bytes_per_gib;
}

std::expected<awj::ImageSizeLimit, std::string> image_size_limit_from_fields(
    int mode_index, std::string max_width_text, std::string max_height_text,
    std::string max_long_edge_text, std::string max_short_edge_text) {
  awj::ImageSizeLimit limit{};
  limit.mode = mode_index == 1   ? awj::ImageSizeLimitMode::none
               : mode_index == 2 ? awj::ImageSizeLimitMode::manual
                                 : awj::ImageSizeLimitMode::automatic;
  if (limit.mode != awj::ImageSizeLimitMode::manual) {
    return limit;
  }
  const auto parse = [](std::string text, std::string_view name)
      -> std::expected<std::optional<int>, std::string> {
    return parse_optional_int_field(std::move(text), name, 1, 1000000);
  };
  if (auto value = parse(std::move(max_width_text), "最大宽"); !value) {
    return std::unexpected{value.error()};
  } else {
    limit.max_width = *value;
  }
  if (auto value = parse(std::move(max_height_text), "最大高"); !value) {
    return std::unexpected{value.error()};
  } else {
    limit.max_height = *value;
  }
  if (auto value = parse(std::move(max_long_edge_text), "最大长边"); !value) {
    return std::unexpected{value.error()};
  } else {
    limit.max_long_edge = *value;
  }
  if (auto value = parse(std::move(max_short_edge_text), "最大短边"); !value) {
    return std::unexpected{value.error()};
  } else {
    limit.max_short_edge = *value;
  }
  return limit;
}

slint::SharedString to_shared(std::string_view text) {
  return slint::SharedString{std::string{text}.c_str()};
}

std::string text_from_wide(std::wstring_view text) {
  return awj::utf8_from_wide(text);
}

std::string text_from_int(int value) { return std::format("{}", value); }

int CALLBACK enum_font_family_proc(const LOGFONTW*, const TEXTMETRICW*, DWORD,
                                   LPARAM param) {
  *reinterpret_cast<bool*>(param) = true;
  return 0;
}

int CALLBACK collect_font_family_proc(const LOGFONTW* font, const TEXTMETRICW*,
                                      DWORD, LPARAM param) {
  const std::wstring_view family{font->lfFaceName};
  if (!family.empty() && family.front() != L'@') {
    reinterpret_cast<std::unordered_set<std::wstring>*>(param)->emplace(family);
  }
  return 1;
}

bool system_font_available(std::wstring_view family) noexcept {
  if (family.empty()) {
    return false;
  }
  HDC dc = GetDC(nullptr);
  if (dc == nullptr) {
    return false;
  }
  LOGFONTW query{};
  query.lfCharSet = DEFAULT_CHARSET;
  const auto length =
      std::min<std::size_t>(family.size(), std::size(query.lfFaceName) - 1);
  std::copy_n(family.begin(), length, query.lfFaceName);
  bool found = false;
  EnumFontFamiliesExW(dc, &query, enum_font_family_proc,
                      reinterpret_cast<LPARAM>(&found), 0);
  ReleaseDC(nullptr, dc);
  return found;
}

std::string select_system_ui_font_family() {
  const std::array<std::wstring_view, 8> candidates{
      L"鸿蒙黑体",       L"HarmonyOS Sans SC", L"Microsoft YaHei UI",
      L"Microsoft YaHei", L"微软雅黑",          L"Segoe UI",
      L"SimHei",         L"SimSun"};
  for (const auto family : candidates) {
    if (system_font_available(family)) {
      return awj::utf8_from_wide(family);
    }
  }
  return {};
}

void apply_system_ui_font(AwjStudio& app) {
  const auto family = select_system_ui_font_family();
  app.set_ui_font_family(to_shared(family));
}

void load_system_font_options(AwjStudio& app) {
  std::unordered_set<std::wstring> families;
  if (HDC dc = GetDC(nullptr); dc != nullptr) {
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &query, collect_font_family_proc,
                        reinterpret_cast<LPARAM>(&families), 0);
    ReleaseDC(nullptr, dc);
  }
  std::vector<std::string> sorted;
  sorted.reserve(families.size());
  for (const auto& family : families) sorted.push_back(awj::utf8_from_wide(family));
  std::ranges::sort(sorted);
  std::vector<ComboOption> options;
  options.reserve(sorted.size() + 1);
  options.push_back(ComboOption{.text = to_shared("系统默认字体"), .enabled = true});
  for (const auto& family : sorted) {
    options.push_back(ComboOption{.text = to_shared(family), .enabled = true});
  }
  app.set_ui_font_options(std::make_shared<slint::VectorModel<ComboOption>>(std::move(options)));
  const auto selected = shared_to_string(app.get_ui_font_family());
  const auto found = std::ranges::find(sorted, selected);
  if (found == sorted.end()) {
    app.set_ui_font_family({});
    app.set_ui_font_index(0);
  } else {
    app.set_ui_font_index(static_cast<int>(std::distance(sorted.begin(), found)) + 1);
  }
}

std::filesystem::path studio_config_path() {
  if (auto directory = awj::executable_directory()) {
    return *directory /
           awj::wide_from_utf8(
               std::string{awj::studio_defaults::config_file_name});
  }
  return {};
}

std::pair<int, int> current_studio_window_size(const AwjStudio& app) noexcept {
  try {
    const auto physical_size = app.window().size();
    const auto scale = std::max(app.window().scale_factor(), 1.0f);
    const auto width = physical_size.width == 0
                           ? awj::studio_defaults::default_window_width
                           : std::lround(static_cast<double>(physical_size.width) /
                                         scale);
    const auto height = physical_size.height == 0
                            ? awj::studio_defaults::default_window_height
                            : std::lround(
                                  static_cast<double>(physical_size.height) / scale);
    return {std::clamp(static_cast<int>(width),
                       awj::studio_defaults::min_window_width,
                       awj::studio_defaults::max_window_width),
            std::clamp(static_cast<int>(height),
                       awj::studio_defaults::min_window_height,
                       awj::studio_defaults::max_window_height)};
  } catch (...) {
    return {static_cast<int>(awj::studio_defaults::default_window_width),
            static_cast<int>(awj::studio_defaults::default_window_height)};
  }
}


MenuFormatParams capture_menu_params_from_ui(const AwjStudio& app) {
  return MenuFormatParams{.quality_text = shared_to_string(app.get_menu_quality_text()),
                          .bit_depth_text = shared_to_string(app.get_menu_bit_depth_text()),
                          .speed_text = shared_to_string(app.get_menu_speed_text()),
                          .avif_encoder_index = app.get_menu_avif_encoder_index(),
                          .avif_color_representation_index =
                              app.get_menu_avif_color_representation_index(),
                          .chroma_index = app.get_menu_chroma_index(),
                          .alpha_policy_index = app.get_menu_alpha_policy_index(),
                          .jpegli_progressive_index = app.get_menu_jpegli_progressive_index(),
                          .jpegli_optimize_huffman = app.get_menu_jpegli_optimize_huffman(),
                          .jpegli_xyb = app.get_menu_jpegli_xyb(),
                          .strip_metadata = app.get_menu_strip_metadata(),
                          .allow_wic_fallback = app.get_menu_allow_wic_fallback(),
                          .close_on_finish = app.get_menu_close_on_finish(),
                          .install_avif_png_command =
                              app.get_menu_install_avif_png_command(),
                          .size_limit_index = app.get_menu_size_limit_index(),
                          .max_width_text = shared_to_string(app.get_menu_max_width_text()),
                          .max_height_text = shared_to_string(app.get_menu_max_height_text()),
                          .max_long_edge_text = shared_to_string(app.get_menu_max_long_edge_text()),
                          .max_short_edge_text = shared_to_string(app.get_menu_max_short_edge_text())};
}

void apply_menu_params_to_ui(AwjStudio& app, const MenuFormatParams& params) {
  app.set_menu_quality_text(to_shared(params.quality_text));
  app.set_menu_bit_depth_text(to_shared(params.bit_depth_text));
  app.set_menu_speed_text(to_shared(params.speed_text));
  app.set_menu_avif_encoder_index(params.avif_encoder_index);
  app.set_menu_avif_color_representation_index(
      params.avif_color_representation_index);
  app.set_menu_chroma_index(params.chroma_index);
  app.set_menu_alpha_policy_index(params.alpha_policy_index);
  app.set_menu_jpegli_progressive_index(params.jpegli_progressive_index);
  app.set_menu_jpegli_optimize_huffman(params.jpegli_optimize_huffman);
  app.set_menu_jpegli_xyb(params.jpegli_xyb);
  app.set_menu_strip_metadata(params.strip_metadata);
  app.set_menu_allow_wic_fallback(params.allow_wic_fallback);
  app.set_menu_close_on_finish(params.close_on_finish);
  app.set_menu_install_avif_png_command(params.install_avif_png_command);
  app.set_menu_size_limit_index(params.size_limit_index);
  app.set_menu_max_width_text(to_shared(params.max_width_text));
  app.set_menu_max_height_text(to_shared(params.max_height_text));
  app.set_menu_max_long_edge_text(to_shared(params.max_long_edge_text));
  app.set_menu_max_short_edge_text(to_shared(params.max_short_edge_text));
}

void store_current_menu_params(AwjStudio& app, UiState& state) {
  const int index = std::clamp(state.last_menu_format_index, 0, 4);
  state.menu_params[static_cast<std::size_t>(index)] = capture_menu_params_from_ui(app);
}

void load_menu_params_for_index(AwjStudio& app, UiState& state, int index) {
  index = std::clamp(index, 0, 4);
  state.last_menu_format_index = index;
  apply_menu_params_to_ui(app, state.menu_params[static_cast<std::size_t>(index)]);
}

std::array<MenuFormatParams, 5> menu_params_snapshot(const AwjStudio& app,
                                                     const UiState* state) {
  std::array<MenuFormatParams, 5> params{};
  if (state != nullptr) {
    params = state->menu_params;
  }
  const int index = std::clamp(state != nullptr ? state->last_menu_format_index
                                                : app.get_menu_format_index(),
                               0, 4);
  params[static_cast<std::size_t>(index)] = capture_menu_params_from_ui(app);
  return params;
}

StudioConfigSnapshot capture_studio_config(const AwjStudio& app,
                                           const UiState* state = nullptr) {
  StudioConfigSnapshot snapshot{
      .theme_index = app.get_theme_index(),
      .language_index = app.get_language_index(),
      .ui_font_family = shared_to_string(app.get_ui_font_family()),
      .allow_wic_fallback = app.get_allow_wic_fallback(),
      .visual_quality_gpu = app.get_visual_quality_gpu(),
      .visual_quality_fallback = app.get_visual_quality_fallback(),
      .experimental_encoders = app.get_experimental_encoders(),
      .menu_params = menu_params_snapshot(app, state)};
  // 后台状态只在 UiState 中维护；所有写入仍由 UI 线程走统一的原子提交。
  if (state != nullptr) {
    snapshot.update_channel = state->update_channel;
    snapshot.show_update_changelog = state->show_update_changelog;
    snapshot.hide_update_changelog_after_exit =
        state->hide_update_changelog_after_exit;
    snapshot.show_update_changelog_after_update =
        state->show_update_changelog_after_update;
    snapshot.last_changelog_exit_version = state->last_changelog_exit_version;
    snapshot.last_successful_update_check_at =
        state->last_successful_update_check_at;
    snapshot.last_verified_manifest_sequence =
        state->last_verified_manifest_sequence;
    snapshot.last_verified_manifest_v2_sequence =
        state->last_verified_manifest_v2_sequence;
    snapshot.pending_update_version = state->pending_update_version;
    snapshot.pending_update_channel = state->pending_update_channel;
    snapshot.pending_update_release_url = state->pending_update_release_url;
    snapshot.pending_update_published_at = state->pending_update_published_at;
    snapshot.pending_update_changelog_zh_cn =
        state->pending_update_changelog_zh_cn;
    snapshot.pending_update_changelog_en = state->pending_update_changelog_en;
    snapshot.update_manifest_raw = state->update_manifest_raw;
    snapshot.update_manifest_signature = state->update_manifest_signature;
    snapshot.update_manifest_v2_raw = state->update_manifest_v2_raw;
    snapshot.update_manifest_v2_signature = state->update_manifest_v2_signature;
    snapshot.update_keyring_raw = state->update_keyring_raw;
    snapshot.update_keyring_signature = state->update_keyring_signature;
  }
  return snapshot;
}

std::string strip_jsonc_comments(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (in_string) {
      out.push_back(ch);
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
      out.push_back(ch);
      continue;
    }
    if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') {
        ++i;
      }
      if (i < text.size()) {
        out.push_back('\n');
      }
      continue;
    }
    if (ch == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        out.push_back(text[i] == '\n' ? '\n' : ' ');
        ++i;
      }
      if (i + 1 < text.size()) {
        ++i;
      }
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

struct JsonConfigValue {
  enum class Kind { boolean, integer, string };
  Kind kind{};
  bool boolean{};
  // 用 64 位存整数：更新状态里有 Unix 时间戳，2038 年会超出 32 位 int。
  // 取值时再按各自的合法区间收窄回 int。
  std::int64_t integer{};
  std::string string{};
};

void skip_json_ws(std::string_view text, std::size_t& pos) noexcept {
  while (pos < text.size() &&
         std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
}

std::expected<std::string, std::string> parse_json_string(std::string_view text,
                                                          std::size_t& pos) {
  if (pos >= text.size() || text[pos] != '"') {
    return std::unexpected{"期望字符串。"};
  }
  ++pos;
  std::string out;
  while (pos < text.size()) {
    const char ch = text[pos++];
    if (ch == '"') {
      return out;
    }
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (pos >= text.size()) {
      return std::unexpected{"字符串转义不完整。"};
    }
    const char esc = text[pos++];
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        out.push_back(esc);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        return std::unexpected{"不支持的字符串转义。"};
    }
  }
  return std::unexpected{"字符串未闭合。"};
}

std::expected<JsonConfigValue, std::string> parse_json_value(
    std::string_view text, std::size_t& pos) {
  skip_json_ws(text, pos);
  if (pos >= text.size()) {
    return std::unexpected{"配置值不完整。"};
  }
  if (text[pos] == '"') {
    auto value = parse_json_string(text, pos);
    if (!value) {
      return std::unexpected{value.error()};
    }
    return JsonConfigValue{.kind = JsonConfigValue::Kind::string,
                           .string = std::move(*value)};
  }
  if (text.substr(pos, 4) == "true") {
    pos += 4;
    return JsonConfigValue{.kind = JsonConfigValue::Kind::boolean,
                           .boolean = true};
  }
  if (text.substr(pos, 5) == "false") {
    pos += 5;
    return JsonConfigValue{.kind = JsonConfigValue::Kind::boolean};
  }
  const std::size_t start = pos;
  if (text[pos] == '-') {
    ++pos;
  }
  while (pos < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  if (pos == start || (pos == start + 1 && text[start] == '-')) {
    return std::unexpected{"配置值只支持布尔、整数或字符串。"};
  }
  const auto parsed = scn::scan_int<std::int64_t>(text.substr(start, pos - start));
  if (!parsed) {
    return std::unexpected{"整数配置值无效。"};
  }
  return JsonConfigValue{.kind = JsonConfigValue::Kind::integer,
                         .integer = parsed->value()};
}

std::expected<std::unordered_map<std::string, JsonConfigValue>, std::string>
parse_jsonc_config(std::string_view source) {
  const auto text = strip_jsonc_comments(source);
  std::string_view view{text};
  std::unordered_map<std::string, JsonConfigValue> values;
  std::size_t pos = 0;
  skip_json_ws(view, pos);
  if (pos >= view.size()) {
    return values;
  }
  if (view[pos++] != '{') {
    return std::unexpected{"配置文件根节点必须是对象。"};
  }
  while (true) {
    skip_json_ws(view, pos);
    if (pos < view.size() && view[pos] == '}') {
      ++pos;
      break;
    }
    auto key = parse_json_string(view, pos);
    if (!key) {
      return std::unexpected{key.error()};
    }
    skip_json_ws(view, pos);
    if (pos >= view.size() || view[pos++] != ':') {
      return std::unexpected{"配置项缺少冒号。"};
    }
    auto value = parse_json_value(view, pos);
    if (!value) {
      return std::unexpected{value.error()};
    }
    values.insert_or_assign(std::move(*key), std::move(*value));
    skip_json_ws(view, pos);
    if (pos < view.size() && view[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < view.size() && view[pos] == '}') {
      ++pos;
      break;
    }
    return std::unexpected{"配置项之间缺少逗号。"};
  }
  skip_json_ws(view, pos);
  if (pos != view.size()) {
    return std::unexpected{"配置对象后存在多余内容。"};
  }
  return values;
}

std::expected<int, std::string> config_int(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, int minimum, int maximum) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::integer) {
    return std::unexpected{std::format("{} 必须是整数。", key)};
  }
  // 先按 64 位比较再收窄：值本身可能超出 int，直接转换是未定义行为。
  if (it->second.integer < static_cast<std::int64_t>(minimum) ||
      it->second.integer > static_cast<std::int64_t>(maximum)) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", key, minimum, maximum)};
  }
  return static_cast<int>(it->second.integer);
}

// 64 位整数配置项（Unix 时间戳、manifest 序号）。
std::expected<std::int64_t, std::string> config_int64(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, std::int64_t minimum, std::int64_t maximum) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::integer) {
    return std::unexpected{std::format("{} 必须是整数。", key)};
  }
  if (it->second.integer < minimum || it->second.integer > maximum) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", key, minimum, maximum)};
  }
  return it->second.integer;
}

std::expected<bool, std::string> config_bool(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::boolean) {
    return std::unexpected{std::format("{} 必须是布尔值。", key)};
  }
  return it->second.boolean;
}

std::expected<std::string, std::string> config_string(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::string) {
    return std::unexpected{std::format("{} 必须是字符串。", key)};
  }
  return it->second.string;
}

bool config_has_key(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key) {
  return values.contains(std::string{key});
}

template <class Setter>
std::expected<void, std::string> apply_config_int(
    AwjStudio& app,
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, int minimum, int maximum, Setter setter) {
  auto value = config_int(values, key, minimum, maximum);
  if (!value) {
    return value.error().empty() ? std::expected<void, std::string>{}
                                 : std::unexpected{value.error()};
  }
  (app.*setter)(*value);
  return {};
}

template <class Setter>
std::expected<void, std::string> apply_config_bool(
    AwjStudio& app,
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, Setter setter) {
  auto value = config_bool(values, key);
  if (!value) {
    return value.error().empty() ? std::expected<void, std::string>{}
                                 : std::unexpected{value.error()};
  }
  (app.*setter)(*value);
  return {};
}

template <class Setter>
std::expected<void, std::string> apply_config_string(
    AwjStudio& app,
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, Setter setter) {
  auto value = config_string(values, key);
  if (!value) {
    return value.error().empty() ? std::expected<void, std::string>{}
                                 : std::unexpected{value.error()};
  }
  (app.*setter)(to_shared(*value));
  return {};
}

std::expected<void, std::string> apply_config_window_size(
    AwjStudio& app,
    const std::unordered_map<std::string, JsonConfigValue>& values) {
  const bool has_width = config_has_key(values, "window_width");
  const bool has_height = config_has_key(values, "window_height");
  if (!has_width && !has_height) {
    return {};
  }
  if (has_width != has_height) {
    return std::unexpected{"window_width 与 window_height 必须同时设置。"};
  }
  auto width = config_int(values, "window_width",
                          awj::studio_defaults::min_window_width,
                          awj::studio_defaults::max_window_width);
  if (!width) {
    return std::unexpected{width.error()};
  }
  auto height = config_int(values, "window_height",
                           awj::studio_defaults::min_window_height,
                           awj::studio_defaults::max_window_height);
  if (!height) {
    return std::unexpected{height.error()};
  }
  app.window().set_size(slint::LogicalSize{
      {static_cast<float>(*width), static_cast<float>(*height)}});
  return {};
}

constexpr std::array<std::string_view, 5> menu_config_prefixes{
    "avif", "webp", "jxl", "jpgli", "png"};

std::string menu_config_key(std::string_view prefix, std::string_view name) {
  return std::format("menu_{}_{}", prefix, name);
}

std::expected<void, std::string> apply_menu_config_values(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::array<MenuFormatParams, 5>& params) {
  const auto apply_int = [&](std::string_view key, int minimum, int maximum,
                             int& target) -> std::expected<void, std::string> {
    auto value = config_int(values, key, minimum, maximum);
    if (!value) {
      return value.error().empty() ? std::expected<void, std::string>{}
                                   : std::unexpected{value.error()};
    }
    target = *value;
    return {};
  };
  const auto apply_bool = [&](std::string_view key,
                              bool& target) -> std::expected<void, std::string> {
    auto value = config_bool(values, key);
    if (!value) {
      return value.error().empty() ? std::expected<void, std::string>{}
                                   : std::unexpected{value.error()};
    }
    target = *value;
    return {};
  };
  const auto apply_string = [&](std::string_view key, std::string& target)
      -> std::expected<void, std::string> {
    auto value = config_string(values, key);
    if (!value) {
      return value.error().empty() ? std::expected<void, std::string>{}
                                   : std::unexpected{value.error()};
    }
    target = std::move(*value);
    return {};
  };

  for (std::size_t i = 0; i < params.size(); ++i) {
    const auto prefix = menu_config_prefixes[i];
    auto& param = params[i];
    const auto one = [&](auto result) -> std::expected<void, std::string> {
      if (!result) return std::unexpected{result.error()};
      return {};
    };
    // menu_*_preset_index 在 1.0.0 随右键预设下拉一并移除；旧配置里的残留键会被忽略。
    if (auto r = one(apply_string(menu_config_key(prefix, "quality_text"), param.quality_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "bit_depth_text"), param.bit_depth_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "speed_text"), param.speed_text)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "avif_encoder_index"), 0, 3, param.avif_encoder_index)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "avif_color_representation_index"), 0, 2, param.avif_color_representation_index)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "chroma_index"), 0, 3, param.chroma_index)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "alpha_policy_index"), 0, 2, param.alpha_policy_index)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "jpegli_progressive_index"), 0, 2, param.jpegli_progressive_index)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "jpegli_optimize_huffman"), param.jpegli_optimize_huffman)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "jpegli_xyb"), param.jpegli_xyb)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "strip_metadata"), param.strip_metadata)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "allow_wic_fallback"), param.allow_wic_fallback)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "close_on_finish"), param.close_on_finish)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "install_avif_png_command"), param.install_avif_png_command)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "size_limit_index"), 0, 2, param.size_limit_index)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "max_width_text"), param.max_width_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "max_height_text"), param.max_height_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "max_long_edge_text"), param.max_long_edge_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "max_short_edge_text"), param.max_short_edge_text)); !r) return r;
  }
  return {};
}

std::expected<void, std::string> apply_studio_config_file(AwjStudio& app, UiState& state) {
  const auto path = studio_config_path();
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return {};
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{"无法读取 Studio 配置文件。"};
  }
  std::string source{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
  auto values = parse_jsonc_config(source);
  if (!values) {
    return std::unexpected{values.error()};
  }

  const auto apply = [&](auto result) -> std::expected<void, std::string> {
    if (!result) {
      return std::unexpected{result.error()};
    }
    return {};
  };

  if (auto result = apply(apply_config_int(app, *values, "theme_index", 0, 2,
                                           &AwjStudio::set_theme_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(app, *values, "language_index", 0, 1,
                                           &AwjStudio::set_language_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_string(
          app, *values, "ui_font_family", &AwjStudio::set_ui_font_family));
      !result) {
    return result;
  }

  if (auto result = apply(apply_config_bool(
          app, *values, "allow_wic_fallback",
          &AwjStudio::set_allow_wic_fallback));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "visual_quality_gpu",
          &AwjStudio::set_visual_quality_gpu));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "visual_quality_fallback",
          &AwjStudio::set_visual_quality_fallback));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "experimental_encoders",
          &AwjStudio::set_experimental_encoders));
      !result) {
    return result;
  }
  if (auto result = apply_menu_config_values(*values, state.menu_params); !result) {
    return result;
  }

  // 更新状态：读进 UiState 而不是 Slint 属性。缺失的键保持默认值——首次运行、
  // 或旧版本写出的配置里没有这些键，都属于正常情况，不是错误。
  {
    const auto load_int64 =
        [&](std::string_view key, std::int64_t minimum, std::int64_t maximum,
            std::int64_t& target) -> std::expected<void, std::string> {
      auto parsed = config_int64(*values, key, minimum, maximum);
      if (parsed) {
        target = *parsed;
        return {};
      }
      return parsed.error().empty() ? std::expected<void, std::string>{}
                                    : std::unexpected{parsed.error()};
    };
    const auto load_bool = [&](std::string_view key,
                               bool& target) -> std::expected<void, std::string> {
      auto parsed = config_bool(*values, key);
      if (parsed) {
        target = *parsed;
        return {};
      }
      return parsed.error().empty() ? std::expected<void, std::string>{}
                                    : std::unexpected{parsed.error()};
    };
    const auto load_string =
        [&](std::string_view key,
            std::string& target) -> std::expected<void, std::string> {
      auto parsed = config_string(*values, key);
      if (parsed) {
        target = *parsed;
        return {};
      }
      return parsed.error().empty() ? std::expected<void, std::string>{}
                                    : std::unexpected{parsed.error()};
    };

    constexpr std::int64_t max_unix_seconds = 4102444800;  // 2100-01-01Z
    if (auto r = load_string("update_channel", state.update_channel); !r) return r;
    if (state.update_channel != "stable" && state.update_channel != "prerelease") {
      return std::unexpected{"配置 update_channel 只能是 stable 或 prerelease。"};
    }
    if (auto r = load_bool("show_update_changelog", state.show_update_changelog); !r) return r;
    if (auto r = load_bool("hide_update_changelog_after_exit",
                           state.hide_update_changelog_after_exit); !r) return r;
    if (auto r = load_bool("show_update_changelog_after_update",
                           state.show_update_changelog_after_update); !r) return r;
    if (auto r = load_string("last_changelog_exit_version",
                             state.last_changelog_exit_version); !r) return r;
    if (auto r = load_int64("last_successful_update_check_at", 0,
                            max_unix_seconds,
                            state.last_successful_update_check_at); !r) return r;
    if (auto r = load_int64("last_verified_manifest_sequence", 0,
                            std::numeric_limits<std::int64_t>::max(),
                            state.last_verified_manifest_sequence); !r) return r;
    if (auto r = load_int64("last_verified_manifest_v2_sequence", 0,
                            std::numeric_limits<std::int64_t>::max(),
                            state.last_verified_manifest_v2_sequence); !r) return r;
    if (auto r = load_string("pending_update_version", state.pending_update_version); !r) return r;
    if (auto r = load_string("pending_update_channel", state.pending_update_channel); !r) return r;
    if (auto r = load_string("pending_update_release_url", state.pending_update_release_url); !r) return r;
    if (auto r = load_string("pending_update_published_at", state.pending_update_published_at); !r) return r;
    if (auto r = load_string("pending_update_changelog_zh_cn",
                             state.pending_update_changelog_zh_cn); !r) return r;
    if (auto r = load_string("pending_update_changelog_en",
                             state.pending_update_changelog_en); !r) return r;
    if (auto r = load_string("update_manifest_raw", state.update_manifest_raw);
        !r) return r;
    if (auto r = load_string("update_manifest_signature",
                             state.update_manifest_signature); !r) return r;
    if (auto r = load_string("update_manifest_v2_raw",
                             state.update_manifest_v2_raw); !r) return r;
    if (auto r = load_string("update_manifest_v2_signature",
                             state.update_manifest_v2_signature); !r) return r;
    if (auto r = load_string("update_keyring_raw", state.update_keyring_raw);
        !r) return r;
    if (auto r = load_string("update_keyring_signature",
                             state.update_keyring_signature); !r) return r;
  }

  app.set_update_channel_index(state.update_channel == "prerelease" ? 1 : 0);
  app.set_hide_update_changelog_after_exit(
      state.hide_update_changelog_after_exit);
  app.set_show_update_changelog_after_update(
      state.show_update_changelog_after_update);
  load_menu_params_for_index(app, state, app.get_menu_format_index());
  return {};
}

std::string json_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

void append_json_config_line(std::vector<std::string>& lines,
                             std::string_view key, std::string value) {
  lines.push_back(std::format("  \"{}\": {}", key, value));
}

// ---------------------------------------------------------------------------
// 原子写文件：同目录临时文件 → 刷新磁盘 → 原子替换。
//
// 三步缺一不可：
//   * 临时文件必须和目标同目录，否则跨卷时替换退化成「复制+删除」，不再原子；
//   * 替换前必须把数据刷到盘上，否则崩溃后可能得到一个大小正确但内容为零的文件
//     （元数据先于数据落盘）；
//   * 替换本身要用平台的原子接口，让读取方要么看到旧内容、要么看到新内容。
//
// 失败时清理临时文件，绝不动原文件——写失败的正确结果是「配置没变」，
// 而不是「配置没了」。
//
// 这里是 main.cpp 的 Windows 半区；Linux Studio 有自己的同名实现，见文件后半段。
// ---------------------------------------------------------------------------
std::expected<void, std::string> write_file_atomically(
    const std::filesystem::path& path, std::string_view content) {
  std::error_code ec;
  auto temp_path = path;
  temp_path += L".tmp";

  // 上一次失败可能留下残留，先清掉；这里失败不致命，创建时还会再报一次。
  std::filesystem::remove(temp_path, ec);

  {
    const HANDLE file = CreateFileW(
        temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return std::unexpected{"无法创建配置临时文件。"};
    }
    struct HandleCloser {
      HANDLE handle{};
      ~HandleCloser() {
        if (handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
        }
      }
    } closer{file};

    std::size_t written_total = 0;
    while (written_total < content.size()) {
      const auto chunk = static_cast<DWORD>(
          std::min<std::size_t>(content.size() - written_total, 1u << 20));
      DWORD written = 0;
      if (WriteFile(file, content.data() + written_total, chunk, &written,
                    nullptr) == FALSE ||
          written == 0) {
        std::filesystem::remove(temp_path, ec);
        return std::unexpected{"写入配置临时文件失败。"};
      }
      written_total += written;
    }
    // 元数据可能先于数据落盘，必须显式 flush 才能保证替换后的文件内容完整。
    if (FlushFileBuffers(file) == FALSE) {
      std::filesystem::remove(temp_path, ec);
      return std::unexpected{"刷新配置临时文件到磁盘失败。"};
    }
  }

  // MoveFileEx 的替换在同卷上是原子的；WRITE_THROUGH 让目录项也落盘。
  if (MoveFileExW(temp_path.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    std::filesystem::remove(temp_path, ec);
    return std::unexpected{"替换程序同目录配置文件失败。"};
  }
  return {};
}

std::expected<void, std::string> write_studio_config_file(
    const StudioConfigSnapshot& current,
    const StudioConfigSnapshot& defaults) try {
  const auto path = studio_config_path();
  if (path.empty()) {
    return std::unexpected{"无法定位程序同目录配置文件路径。"};
  }
  std::vector<std::string> lines;
  const auto add_int = [&](std::string_view key, int value, int fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key, std::format("{}", value));
    }
  };
  const auto add_bool = [&](std::string_view key, bool value, bool fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key, value ? "true" : "false");
    }
  };
  const auto add_string = [&](std::string_view key, const std::string& value,
                              const std::string& fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key,
                              std::format("\"{}\"", json_escape(value)));
    }
  };
  // Unix 时间戳和 manifest 序号都会超出 int，必须单独走 64 位。
  const auto add_int64 = [&](std::string_view key, std::int64_t value,
                             std::int64_t fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key, std::format("{}", value));
    }
  };

  add_int("theme_index", current.theme_index, defaults.theme_index);
  add_int("language_index", current.language_index, defaults.language_index);
  add_string("ui_font_family", current.ui_font_family, defaults.ui_font_family);

  add_bool("allow_wic_fallback", current.allow_wic_fallback,
           defaults.allow_wic_fallback);
  add_bool("visual_quality_gpu", current.visual_quality_gpu,
           defaults.visual_quality_gpu);
  add_bool("visual_quality_fallback", current.visual_quality_fallback,
           defaults.visual_quality_fallback);
  add_bool("experimental_encoders", current.experimental_encoders,
           defaults.experimental_encoders);

  add_string("update_channel", current.update_channel,
             defaults.update_channel);
  add_bool("show_update_changelog", current.show_update_changelog,
           defaults.show_update_changelog);
  add_bool("hide_update_changelog_after_exit",
           current.hide_update_changelog_after_exit,
           defaults.hide_update_changelog_after_exit);
  add_bool("show_update_changelog_after_update",
           current.show_update_changelog_after_update,
           defaults.show_update_changelog_after_update);
  add_string("last_changelog_exit_version", current.last_changelog_exit_version,
             defaults.last_changelog_exit_version);
  add_int64("last_successful_update_check_at",
            current.last_successful_update_check_at,
            defaults.last_successful_update_check_at);
  add_int64("last_verified_manifest_sequence",
            current.last_verified_manifest_sequence,
            defaults.last_verified_manifest_sequence);
  add_int64("last_verified_manifest_v2_sequence",
            current.last_verified_manifest_v2_sequence,
            defaults.last_verified_manifest_v2_sequence);
  add_string("pending_update_version", current.pending_update_version,
             defaults.pending_update_version);
  add_string("pending_update_channel", current.pending_update_channel,
             defaults.pending_update_channel);
  add_string("pending_update_release_url", current.pending_update_release_url,
             defaults.pending_update_release_url);
  add_string("pending_update_published_at",
             current.pending_update_published_at,
             defaults.pending_update_published_at);
  add_string("pending_update_changelog_zh_cn",
             current.pending_update_changelog_zh_cn,
             defaults.pending_update_changelog_zh_cn);
  add_string("pending_update_changelog_en",
             current.pending_update_changelog_en,
             defaults.pending_update_changelog_en);
  add_string("update_manifest_raw", current.update_manifest_raw,
             defaults.update_manifest_raw);
  add_string("update_manifest_signature", current.update_manifest_signature,
             defaults.update_manifest_signature);
  add_string("update_manifest_v2_raw", current.update_manifest_v2_raw,
             defaults.update_manifest_v2_raw);
  add_string("update_manifest_v2_signature", current.update_manifest_v2_signature,
             defaults.update_manifest_v2_signature);
  add_string("update_keyring_raw", current.update_keyring_raw,
             defaults.update_keyring_raw);
  add_string("update_keyring_signature", current.update_keyring_signature,
             defaults.update_keyring_signature);

  for (std::size_t i = 0; i < current.menu_params.size(); ++i) {
    const auto prefix = menu_config_prefixes[i];
    const auto& value = current.menu_params[i];
    const auto& fallback = defaults.menu_params[i];
    add_string(menu_config_key(prefix, "quality_text"), value.quality_text, fallback.quality_text);
    add_string(menu_config_key(prefix, "bit_depth_text"), value.bit_depth_text, fallback.bit_depth_text);
    add_string(menu_config_key(prefix, "speed_text"), value.speed_text, fallback.speed_text);
    add_int(menu_config_key(prefix, "avif_encoder_index"), value.avif_encoder_index, fallback.avif_encoder_index);
    add_int(menu_config_key(prefix, "avif_color_representation_index"), value.avif_color_representation_index, fallback.avif_color_representation_index);
    add_int(menu_config_key(prefix, "chroma_index"), value.chroma_index, fallback.chroma_index);
    add_int(menu_config_key(prefix, "alpha_policy_index"), value.alpha_policy_index, fallback.alpha_policy_index);
    add_int(menu_config_key(prefix, "jpegli_progressive_index"), value.jpegli_progressive_index, fallback.jpegli_progressive_index);
    add_bool(menu_config_key(prefix, "jpegli_optimize_huffman"), value.jpegli_optimize_huffman, fallback.jpegli_optimize_huffman);
    add_bool(menu_config_key(prefix, "jpegli_xyb"), value.jpegli_xyb, fallback.jpegli_xyb);
    add_bool(menu_config_key(prefix, "strip_metadata"), value.strip_metadata, fallback.strip_metadata);
    add_bool(menu_config_key(prefix, "allow_wic_fallback"), value.allow_wic_fallback, fallback.allow_wic_fallback);
    add_bool(menu_config_key(prefix, "close_on_finish"), value.close_on_finish, fallback.close_on_finish);
    add_bool(menu_config_key(prefix, "install_avif_png_command"), value.install_avif_png_command, fallback.install_avif_png_command);
    add_int(menu_config_key(prefix, "size_limit_index"), value.size_limit_index, fallback.size_limit_index);
    add_string(menu_config_key(prefix, "max_width_text"), value.max_width_text, fallback.max_width_text);
    add_string(menu_config_key(prefix, "max_height_text"), value.max_height_text, fallback.max_height_text);
    add_string(menu_config_key(prefix, "max_long_edge_text"), value.max_long_edge_text, fallback.max_long_edge_text);
    add_string(menu_config_key(prefix, "max_short_edge_text"), value.max_short_edge_text, fallback.max_short_edge_text);
  }

  // 先在内存里拼出完整内容，再原子落盘。直接 truncate 写目标文件的话，进程在
  // 写到一半时被杀（或断电）会留下一个被截断的 AWJ.jsonc —— 下次启动解析失败，
  // 用户的全部设置一起丢。更新流程会往同一份配置里写待更新状态，出错代价更高。
  std::string content;
  content += "{\n";
  content +=
      "  // AWJ Studio runtime config. Only values that differ from "
      "built-in defaults are written.\n";
  for (std::size_t i = 0; i < lines.size(); ++i) {
    content += lines[i];
    if (i + 1 < lines.size()) {
      content += ',';
    }
    content += '\n';
  }
  content += "}\n";

  return write_file_atomically(path, content);
} catch (const std::bad_alloc&) {
  return std::unexpected{"写入 Studio 配置时内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"写入 Studio 配置时数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"写入 Studio 配置时发生文件系统错误。"};
}

std::expected<void, std::string> persist_studio_config_if_changed(
    AwjStudio& app, UiState& state) {
  if (!state.config_defaults) {
    return {};
  }
  auto current = capture_studio_config(app, &state);
  if (state.last_config_snapshot &&
      current == *state.last_config_snapshot) {
    return {};
  }
  if (auto saved = write_studio_config_file(current, *state.config_defaults);
      !saved) {
    return std::unexpected{saved.error()};
  }
  state.last_config_snapshot = std::move(current);
  return {};
}

std::string queue_status_label(QueueItemStatus status) {
  switch (status) {
    case QueueItemStatus::running:
      return "正在编码";
    case QueueItemStatus::done:
      return "完成";
    case QueueItemStatus::failed:
      return "失败";
    case QueueItemStatus::skipped:
      return "已跳过";
    case QueueItemStatus::canceled:
      return "已取消";
    case QueueItemStatus::pending:
    default:
      return "等待编码";
  }
}

int queue_status_code(QueueItemStatus status) noexcept {
  switch (status) {
    case QueueItemStatus::running:
      return 1;
    case QueueItemStatus::done:
    case QueueItemStatus::skipped:
      return 2;
    case QueueItemStatus::failed:
      return 3;
    case QueueItemStatus::canceled:
      return 4;
    case QueueItemStatus::pending:
    default:
      return 0;
  }
}

std::string stage_seconds_text(double seconds) {
  return seconds < 0.0 ? std::string{"-"} : std::format("{:.3f}s", seconds);
}

std::string stage_timings_text(double decode_seconds, double prepare_seconds,
                               double encode_seconds, double write_seconds) {
  if (decode_seconds < 0.0 && prepare_seconds < 0.0 &&
      encode_seconds < 0.0 && write_seconds < 0.0) {
    return {};
  }
  return std::format("decode {} · prepare {} · encode {} · write {}",
                     stage_seconds_text(decode_seconds),
                     stage_seconds_text(prepare_seconds),
                     stage_seconds_text(encode_seconds),
                     stage_seconds_text(write_seconds));
}

bool queue_item_editable(const QueueImageItem& item) noexcept {
  return item.status == QueueItemStatus::pending;
}

bool queue_item_runnable(const QueueImageItem& item) noexcept {
  return item.status == QueueItemStatus::pending ||
         item.status == QueueItemStatus::failed ||
         item.status == QueueItemStatus::canceled;
}

bool queue_item_selected_for_run(const QueueImageItem& item,
                                 bool failed_only) noexcept {
  return failed_only ? item.status == QueueItemStatus::failed
                     : queue_item_runnable(item);
}

TaskRow make_queue_task_row(const QueueImageItem& item, std::size_t order) {
  const auto folder = item.path.parent_path();
  const auto output =
      item.locked_output_path.empty()
          ? std::string{}
          : awj::path_to_utf8(item.locked_output_path.filename());
  const auto output_path = item.locked_output_path.empty()
                               ? std::string{}
                               : awj::path_to_utf8(item.locked_output_path);
  const auto status = item.status_text.empty()
                          ? queue_status_label(item.status)
                          : item.status_text;
  return TaskRow{.order = to_shared(std::format("{}", order + 1)),
                 .filename = to_shared(awj::path_to_utf8(item.path.filename())),
                 .folder = to_shared(awj::path_to_utf8(folder)),
                 .size = to_shared(awj::format_size(item.bytes)),
                 .status = to_shared(status),
                 .output = to_shared(output),
                  .log = to_shared(item.log_text),
                  .warning = item.warning,
                  .locked = !queue_item_editable(item),
                  .state = queue_status_code(item.status),
                  .input_path = to_shared(awj::path_to_utf8(item.path)),
                  .output_path = to_shared(output_path),
                  .encoder = to_shared(item.encoder_id),
                  .threads = item.encoder_threads > 0
                                 ? to_shared(std::format("{}", item.encoder_threads))
                                 : slint::SharedString{},
                  .stage_timings = to_shared(stage_timings_text(
                      item.decode_seconds, item.prepare_seconds,
                      item.encode_seconds, item.write_seconds))};
}

void refresh_queue_rows(AwjStudio& app, UiState& state) {
  std::vector<TaskRow> rows;
  rows.reserve(state.queue_items.size());
  int pending_count = 0;
  int running_count = 0;
  int success_count = 0;
  int failed_count = 0;
  for (const auto i : std::views::iota(std::size_t{}, state.queue_items.size())) {
    const auto& item = state.queue_items[i];
    rows.push_back(make_queue_task_row(item, i));
    switch (item.status) {
      case QueueItemStatus::running:
        ++running_count;
        break;
      case QueueItemStatus::done:
      case QueueItemStatus::skipped:
        ++success_count;
        break;
      case QueueItemStatus::failed:
        ++failed_count;
        break;
      case QueueItemStatus::pending:
      case QueueItemStatus::canceled:
      default:
        ++pending_count;
        break;
    }
  }
  state.task_rows->set_vector(std::move(rows));
  app.set_task_rows(state.task_rows);
  app.set_queue_pending_count(pending_count);
  app.set_queue_running_count(running_count);
  app.set_queue_success_count(success_count);
  app.set_queue_failed_count(failed_count);
  if (app.get_selected_queue_index() >=
      static_cast<int>(state.queue_items.size())) {
    app.set_selected_queue_index(-1);
  }
}

std::optional<std::size_t> queue_index_for_id(const UiState& state,
                                              std::uint64_t id) noexcept {
  for (const auto i : std::views::iota(std::size_t{}, state.queue_items.size())) {
    if (state.queue_items[i].id == id) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> queue_index_for_run_index(
    const UiState& state, std::size_t run_index) noexcept {
  for (const auto i : std::views::iota(std::size_t{}, state.queue_items.size())) {
    if (state.queue_items[i].run_index == run_index) {
      return i;
    }
  }
  return std::nullopt;
}

bool move_queue_item(UiState& state, std::size_t from, std::size_t to) {
  if (from >= state.queue_items.size() || to >= state.queue_items.size() ||
      from == to || !queue_item_editable(state.queue_items[from])) {
    return false;
  }
  if (!queue_item_editable(state.queue_items[to])) {
    return false;
  }
  auto item = std::move(state.queue_items[from]);
  state.queue_items.erase(state.queue_items.begin() +
                          static_cast<std::ptrdiff_t>(from));
  state.queue_items.insert(
      state.queue_items.begin() + static_cast<std::ptrdiff_t>(to),
      std::move(item));
  return true;
}

std::size_t first_pending_index(const UiState& state) noexcept {
  for (const auto i : std::views::iota(std::size_t{}, state.queue_items.size())) {
    if (queue_item_editable(state.queue_items[i])) {
      return i;
    }
  }
  return state.queue_items.size();
}

std::size_t last_pending_index(const UiState& state) noexcept {
  for (std::size_t i = state.queue_items.size(); i > 0; --i) {
    if (queue_item_editable(state.queue_items[i - 1])) {
      return i - 1;
    }
  }
  return state.queue_items.size();
}

ComboOption combo_option(std::string_view text, bool enabled = true) {
  return ComboOption{.text = to_shared(text), .enabled = enabled};
}

void set_combo_options(
    AwjStudio& app, const std::vector<ComboOption>& options,
    void (AwjStudio::*setter)(const std::shared_ptr<slint::Model<ComboOption>>&)
        const) {
  auto model = std::make_shared<slint::VectorModel<ComboOption>>();
  model->set_vector(options);
  (app.*setter)(model);
}

bool avif_encoder_enabled_for_ui(awj::AvifEncoderMode mode,
                                 bool enable_experimental) {
  if (mode == awj::AvifEncoderMode::automatic) {
    return true;
  }
  auto capabilities =
      awj::avif_encoder_capabilities_for_current_build(enable_experimental);
  const auto it =
      std::ranges::find(capabilities, mode, &awj::AvifEncoderCapability::mode);
  return it != capabilities.end() && it->enabled &&
         (!it->experimental || it->feature_enabled);
}

std::vector<ComboOption> avif_encoder_options(bool enable_experimental) {
  return {
      combo_option("自动"),
      combo_option("svt-av1-hdr",
                   avif_encoder_enabled_for_ui(awj::AvifEncoderMode::svt,
                                               enable_experimental)),
      combo_option("aom", avif_encoder_enabled_for_ui(awj::AvifEncoderMode::aom,
                                                      enable_experimental)),
      combo_option("zenrav1e",
                   avif_encoder_enabled_for_ui(awj::AvifEncoderMode::zenrav1e,
                                               enable_experimental))};
}

struct LargeImageManualAvailability {
  bool grid{};
  bool zenrav1e{};
};

LargeImageManualAvailability large_image_manual_availability(
    bool enable_experimental) {
  const auto capabilities =
      awj::avif_encoder_capabilities_for_current_build(enable_experimental);
  LargeImageManualAvailability result{};
  for (const auto& capability : capabilities) {
    const bool enabled = capability.enabled && (!capability.experimental ||
                                                capability.feature_enabled);
    if (!enabled) {
      continue;
    }
    if (capability.mode == awj::AvifEncoderMode::aom &&
        capability.supports_avif_grid) {
      result.grid = true;
    }
    if (capability.mode == awj::AvifEncoderMode::zenrav1e) {
      result.zenrav1e = true;
    }
  }
  return result;
}

std::expected<std::vector<std::filesystem::path>, std::string>
supported_files_in_folder(const std::filesystem::path& folder) {
  try {
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it{
        folder, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) {
      return std::unexpected{std::format("扫描文件夹失败: {}；系统错误：{}。",
                                         awj::display_path_for_user(folder),
                                         ec.message())};
    }
    for (std::filesystem::recursive_directory_iterator end; it != end;
         it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      if (!it->is_regular_file(ec) || ec) {
        ec.clear();
        continue;
      }
      if (awj::is_supported_image_extension(it->path())) {
        paths.push_back(it->path());
      }
    }
    const auto path_key = [](const std::filesystem::path& path) {
      std::error_code key_ec;
      const auto absolute = std::filesystem::absolute(path, key_ec);
      return awj::normalized_lower_path_key(key_ec ? path : absolute);
    };
    std::ranges::sort(paths, [&](const auto& left, const auto& right) {
      const auto left_key = path_key(left);
      const auto right_key = path_key(right);
      return left_key == right_key ? left.wstring() < right.wstring()
                                   : left_key < right_key;
    });
    const auto duplicate = std::ranges::unique(paths, {}, path_key);
    paths.erase(duplicate.begin(), duplicate.end());
    return paths;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"扫描文件夹时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"扫描文件夹时文件数量超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"扫描文件夹时文件系统访问失败。"};
  }
}

std::wstring queue_path_key(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return awj::normalized_lower_path_key(ec ? path : absolute);
}

bool queue_contains_path(const UiState& state,
                         const std::filesystem::path& path) {
  const auto key = queue_path_key(path);
  return std::ranges::any_of(state.queue_items, [&](const QueueImageItem& item) {
    return queue_path_key(item.path) == key;
  });
}

std::filesystem::path queue_relative_dir_for(
    const std::filesystem::path& root, const std::filesystem::path& path) {
  std::error_code ec;
  const auto relative = std::filesystem::relative(path.parent_path(), root, ec);
  if (ec || relative.empty() || relative == L".") {
    return {};
  }
  return relative;
}

std::expected<bool, std::string> append_queue_image_path(
    UiState& state, const std::filesystem::path& path,
    const std::filesystem::path& source_root) {
  try {
    if (queue_contains_path(state, path)) {
      return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      return std::unexpected{std::format("不是可加入队列的文件: {}。",
                                         awj::display_path_for_user(path))};
    }
    if (!awj::is_supported_image_extension(path)) {
      return std::unexpected{std::format("文件格式不受支持: {}。支持 {}。",
                                         awj::display_path_for_user(path),
                                         awj::kSupportedImageExtensionsText)};
    }
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec) {
      return std::unexpected{std::format("读取文件大小失败: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
    if (bytes > static_cast<std::uintmax_t>(
                    awj::encoding_defaults::effective_max_input_file_bytes())) {
      return std::unexpected{std::format("输入文件超过当前输入上限: {}。",
                                         awj::display_path_for_user(path))};
    }
    state.queue_items.push_back(QueueImageItem{
        .id = state.next_queue_id++,
        .path = path,
        .source_root = source_root,
        .relative_dir =
            source_root.empty() ? std::filesystem::path{}
                                : queue_relative_dir_for(source_root, path),
        .bytes = bytes});
    return true;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"添加队列项时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"添加队列项时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"添加队列项时文件系统访问失败。"};
  }
}

std::expected<bool, std::string> append_prepared_import_file(
    UiState& state, const awj::ui_import::File& file) {
  try {
    if (queue_contains_path(state, file.path)) return false;
    state.queue_items.push_back(QueueImageItem{
        .id = state.next_queue_id++,
        .path = file.path,
        .source_root = file.source_root,
        .relative_dir = file.source_root.empty()
                            ? std::filesystem::path{}
                            : queue_relative_dir_for(file.source_root, file.path),
        .bytes = file.bytes});
    return true;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"添加导入结果时内存不足。"};
  }
}

bool add_queue_from_path(AwjStudio& app, UiState& state,
                         const std::filesystem::path& picked,
                         bool pick_folder, bool update_input_path = true) {
  std::error_code ec;
  const bool is_folder = pick_folder ||
                         (std::filesystem::is_directory(picked, ec) && !ec);
  std::vector<std::filesystem::path> paths;
  if (is_folder) {
    auto scanned = supported_files_in_folder(picked);
    if (!scanned) {
      app.set_status_text(to_shared(scanned.error()));
      return false;
    }
    paths = std::move(*scanned);
    if (paths.empty()) {
      app.set_status_text(to_shared("文件夹中没有支持的图片文件。"));
      return false;
    }
  } else {
    paths.push_back(picked);
  }

  std::size_t added = 0;
  std::size_t skipped = 0;
  std::size_t failed = 0;
  std::string first_error;
  const auto source_root = is_folder ? picked : std::filesystem::path{};
  bool accepted_target = false;
  for (const auto& path : paths) {
    auto appended = append_queue_image_path(state, path, source_root);
    if (appended && *appended) {
      ++added;
      accepted_target = true;
    } else if (appended) {
      ++skipped;
      accepted_target = true;
    } else {
      ++failed;
      if (first_error.empty()) {
        first_error = appended.error();
      }
    }
  }
  if (update_input_path && accepted_target) {
    set_input_path_preserving_output(app, picked);
  }
  refresh_queue_rows(app, state);
  if (added == 0) {
    app.set_status_text(
        to_shared(first_error.empty()
                      ? std::format("没有新图片加入队列{}。",
                                    skipped > 0 ? "，重复项已跳过" : "")
                      : first_error));
    return accepted_target;
  }
  app.set_status_text(to_shared(std::format(
      "已加入 {} 张图片{}{}。", added,
      skipped == 0 ? "" : std::format("，跳过 {} 个重复项", skipped),
      failed == 0 ? "" : std::format("，{} 个失败", failed))));
  return accepted_target;
}

void add_manual_large_images_from_picker(AwjStudio& app, UiState& state,
                                         const std::filesystem::path& picked,
                                         bool folder) {
  const auto availability =
      large_image_manual_availability(app.get_experimental_encoders());
  const bool allow_wic_fallback = app.get_allow_wic_fallback();
  std::vector<std::filesystem::path> paths;
  if (folder) {
    auto scanned = supported_files_in_folder(picked);
    if (!scanned) {
      app.set_status_text(to_shared(scanned.error()));
      return;
    }
    paths = std::move(*scanned);
    if (paths.empty()) {
      app.set_status_text(to_shared("文件夹中没有支持的图片文件。"));
      return;
    }
  } else {
    paths.push_back(picked);
  }

  const bool was_empty = state.large_image_items.empty();
  std::size_t added = 0;
  std::size_t failed = 0;
  std::string first_error;
  for (const auto& path : paths) {
    auto result =
        add_manual_large_image_path(state, path, allow_wic_fallback,
                                    availability.grid, availability.zenrav1e);
    if (result) {
      ++added;
    } else {
      ++failed;
      if (first_error.empty()) {
        first_error = result.error();
      }
    }
  }
  if (was_empty && added > 0) {
    app.set_selected_large_image_index(0);
  }
  if (added == 0) {
    app.set_status_text(
        to_shared(first_error.empty() ? "未添加任何大图任务。" : first_error));
    return;
  }
  app.set_status_text(to_shared(
      std::format("已添加 {} 个大图任务{}。", added,
                  failed == 0 ? "" : std::format("，{} 个失败", failed))));
}

void refresh_avif_encoder_options(AwjStudio& app) {
  const auto options = avif_encoder_options(app.get_experimental_encoders());
  set_combo_options(app, options, &AwjStudio::set_avif_encoder_options);
  const auto selected = app.get_avif_encoder_index();
  if (selected < 0 || static_cast<std::size_t>(selected) >= options.size() ||
      !options[static_cast<std::size_t>(selected)].enabled) {
    app.set_avif_encoder_index(0);
  }
}

bool template_contains_token(std::string_view text, std::string_view token) {
  return text.find(token) != std::string_view::npos;
}

void sync_template_flags(AwjStudio& app) {
  const auto text = shared_to_string(app.get_template_text());
  app.set_template_params_selected(template_contains_token(text, "{params}"));
  app.set_template_date_selected(template_contains_token(text, "{date}"));
  app.set_template_time_selected(template_contains_token(text, "{time}"));
  app.set_template_rand_selected(template_contains_token(text, "{rand}"));
  app.set_template_hash8_selected(template_contains_token(text, "{hash8}"));
  app.set_template_sha2568_selected(
      template_contains_token(text, "{sha2568}") ||
      template_contains_token(text, "{sha256_8}"));
}

std::string token_with_separator(std::string_view text,
                                 std::string_view token) {
  if (text.empty()) {
    return std::string{token};
  }
  const char last = text.back();
  const bool needs_separator =
      last != '_' && last != '-' && last != ' ' && last != '.';
  return std::format("{}{}{}", text, needs_separator ? "_" : "", token);
}

void toggle_template_token(AwjStudio& app, std::string_view token) {
  auto text = shared_to_string(app.get_template_text());
  const auto pos = text.find(token);
  if (pos != std::string::npos) {
    text.erase(pos, token.size());
    if (pos > 0 && text[pos - 1] == '_') {
      text.erase(pos - 1, 1);
    } else if (pos < text.size() && text[pos] == '_') {
      text.erase(pos, 1);
    }
    if (text.empty()) {
      text = std::string{awj::encoding_defaults::default_output_template_text};
    }
  } else {
    text = token_with_separator(text, token);
  }
  app.set_template_text(to_shared(text));
  sync_template_flags(app);
}

bool windows_prefers_dark_mode() {
  DWORD apps_use_light_theme = 1;
  DWORD value_size = sizeof(apps_use_light_theme);
  const auto status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &apps_use_light_theme,
      &value_size);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  return apps_use_light_theme == 0;
}

// 把界面语言切到 language_index 指定的语言。
//
// 0 = 中文，也就是 .slint 里 @tr() 的 msgid 原文和 bundled 默认语言；
// 1 = English，对应 ui/translations/en/LC_MESSAGES/awj.po 这份 bundled 翻译。
//
// select_bundled_translation 写的是 translations_dirty 这个真实的 Slint 属性，
// 所以每个 @tr() 绑定都会失效并在下一帧重算——不需要重启，也不需要自己去
// 逐个属性重新赋值。两点约束：必须在第一个组件创建之后才能调用，否则拿不到
// bundle；传空串会明确回到 bundled 默认语言（中文 msgid）。
//
// 返回值刻意忽略：翻译缺失时回退到中文原文，是可接受的降级，不该阻断设置操作。
void apply_ui_language(int language_index) noexcept {
  try {
    static_cast<void>(
        slint::select_bundled_translation(language_index == 1 ? "en" : ""));
  } catch (...) {
  }
}

bool effective_studio_dark_mode(const AwjStudio& app) {
  return app.get_theme_index() == 2 ||
         (app.get_theme_index() == 0 && app.get_system_dark_mode());
}

bool shell_window_dark_mode() {
  int theme_index = 0;
  const auto path = studio_config_path();
  if (!path.empty()) {
    std::ifstream input{path, std::ios::binary};
    if (input) {
      std::string source{std::istreambuf_iterator<char>{input}, {}};
      if (auto values = parse_jsonc_config(source)) {
        if (auto value = config_int(*values, "theme_index", 0, 2)) {
          theme_index = *value;
        }
      }
    }
  }
  return theme_index == 2 || (theme_index == 0 && windows_prefers_dark_mode());
}

void apply_title_bar_theme(slint::Window& window, bool dark_mode) noexcept {
  try {
    const HWND hwnd = window.win32_hwnd();
    if (hwnd == nullptr) {
      return;
    }
    const BOOL use_dark_mode = dark_mode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark_mode,
                          sizeof(use_dark_mode));
  } catch (...) {
  }
}

void set_status_text_noexcept(AwjStudio& app, std::string_view text) noexcept {
  try {
    app.set_status_text(to_shared(text));
  } catch (...) {
  }
}

void reset_failed_run(AwjStudio& app, UiState& state, std::uint64_t run_id,
                      std::string_view message) noexcept {
  try {
    std::scoped_lock lock{state.mutex};
    if (run_id != 0 && state.run_id == run_id) {
      state.update_timer.stop();
      state.pending_events.clear();
      state.worker_active = false;
    }
  } catch (...) {
  }
  try {
    app.set_running(false);
  } catch (...) {
  }
  set_status_text_noexcept(app, message);
}

bool clear_run_if_callback_not_posted(UiState& state,
                                      std::uint64_t run_id) noexcept {
  try {
    std::scoped_lock lock{state.mutex};
    if (state.run_id != run_id) {
      return false;
    }
    state.pending_events.clear();
    state.worker_active = false;
    if (state.active_child) {
      state.active_child->terminate();
      state.active_child.reset();
    }
    return true;
  } catch (...) {
    return false;
  }
}

void report_ui_callback_failure(slint::ComponentWeakHandle<AwjStudio> weak,
                                std::string_view context,
                                std::string_view detail) noexcept {
  try {
    if (auto app = weak.lock()) {
      try {
        set_status_text_noexcept(**app, std::format("{}：{}", context, detail));
      } catch (...) {
        set_status_text_noexcept(**app, "界面操作失败。");
      }
    }
  } catch (...) {
  }
}

template <class Function>
void run_ui_callback(slint::ComponentWeakHandle<AwjStudio> weak,
                     std::string_view context, Function&& fn) noexcept {
  try {
    std::forward<Function>(fn)();
  } catch (const std::bad_alloc&) {
    report_ui_callback_failure(weak, context, "内存不足。");
  } catch (const std::length_error&) {
    report_ui_callback_failure(weak, context, "数据超过运行时限制。");
  } catch (const std::exception&) {
    report_ui_callback_failure(weak, context, "发生未预期异常。");
  } catch (...) {
    report_ui_callback_failure(weak, context, "发生未知异常。");
  }
}

template <class Function>
bool post_to_ui(slint::ComponentWeakHandle<AwjStudio> weak, Function&& fn) {
  try {
    slint::invoke_from_event_loop(
        [weak, fn = std::forward<Function>(fn)]() mutable {
          run_ui_callback(weak, "界面更新失败", [&] {
            if (auto app = weak.lock()) {
              fn(**app);
            }
          });
        });
    return true;
  } catch (...) {
    return false;
  }
}

void apply_import_result(AwjStudio& app, UiState& state,
                         awj::ui_import::Request request,
                         awj::ui_import::Result result) {
  if (result.cancelled) {
    app.set_status_text(to_shared("导入扫描已取消。"));
    return;
  }
  {
    std::scoped_lock lock{state.mutex};
    if (state.worker_active) {
      app.set_status_text(to_shared("编码已开始，本次后台导入结果未加入队列。"));
      return;
    }
  }
  std::size_t added = 0;
  std::size_t queue_duplicates = 0;
  for (const auto& file : result.files) {
    auto appended = append_prepared_import_file(state, file);
    if (appended && *appended) {
      ++added;
    } else if (appended) {
      ++queue_duplicates;
    } else if (result.errors.size() < 16) {
      result.errors.push_back(appended.error());
    }
  }
  if (request.update_input_path && added > 0 && !request.input_hint.empty()) {
    set_input_path_preserving_output(app, request.input_hint);
  }
  refresh_queue_rows(app, state);
  app.set_status_text(
      to_shared(awj::ui_import::summary_text(result, added, queue_duplicates)));
}

bool enqueue_import(const std::shared_ptr<UiState>& state,
                    awj::ui_import::Request request) {
  {
    std::scoped_lock run_lock{state->mutex};
    if (state->worker_active) return false;
  }
  return state->import_dispatcher &&
         state->import_dispatcher->enqueue(std::move(request));
}

void start_import_dispatcher(slint::ComponentWeakHandle<AwjStudio> weak,
                             const std::shared_ptr<UiState>& state) {
  state->import_dispatcher = std::make_unique<awj::ui_import::Dispatcher>(
      [] {
        return awj::ui_import::Options{
            .is_supported = [](const std::filesystem::path& path) {
              return awj::is_supported_image_extension(path);
            },
            .stop_requested = {},
            .maximum_file_bytes = static_cast<std::uintmax_t>(
                awj::encoding_defaults::effective_max_input_file_bytes())};
      },
      [weak, state](awj::ui_import::Request request,
                    awj::ui_import::Result result) mutable {
        post_to_ui(
            weak,
            [state, request = std::move(request), result = std::move(result)](
                AwjStudio& app) mutable {
              apply_import_result(app, *state, std::move(request),
                                  std::move(result));
            });
      });
}

constexpr std::chrono::milliseconds native_drop_retry_interval{20};
constexpr std::size_t native_drop_max_attempts = 100;

awj::ui_drop::Callbacks make_native_drop_callbacks(
    slint::ComponentWeakHandle<AwjStudio> weak,
    std::weak_ptr<UiState> weak_state) {
  return awj::ui_drop::Callbacks{
      .can_accept = [weak_state] {
        const auto state = weak_state.lock();
        if (!state) return false;
        std::scoped_lock lock{state->mutex};
        return !state->worker_active;
      },
      .hover_changed = [weak](awj::ui_drop::HoverState hover) {
        run_ui_callback(weak, "更新外部拖放状态失败", [&] {
          if (auto app = weak.lock()) {
            (*app)->set_external_drag_active(hover.active);
            (*app)->set_external_drag_valid(hover.valid);
            (*app)->set_external_drag_item_count(
                static_cast<int>(std::min<std::size_t>(
                    hover.item_count,
                    static_cast<std::size_t>(std::numeric_limits<int>::max()))));
          }
        });
      },
      .paths_dropped = [weak, weak_state](
                           std::vector<std::filesystem::path> paths) mutable {
        run_ui_callback(weak, "接收 Windows 原生拖放失败", [&] {
          const auto state = weak_state.lock();
          if (!state || paths.empty()) return;
          awj::ui_import::Request job;
          job.origin = awj::ui_import::Origin::drag_drop;
          job.input_hint = paths.front();
          job.update_input_path = true;
          job.roots.reserve(paths.size());
          for (auto& path : paths) {
            job.roots.push_back({.path = std::move(path)});
          }
          if (enqueue_import(state, std::move(job))) {
            if (auto app = weak.lock()) {
              (*app)->set_status_text(to_shared("正在导入拖入的文件与文件夹…"));
            }
          }
        });
      }};
}

void start_native_drop_registration(slint::ComponentWeakHandle<AwjStudio> weak,
                                    const std::shared_ptr<UiState>& state) {
  state->native_drop_attempts = 0;
  state->native_drop_registration_finished = false;
  const std::weak_ptr<UiState> weak_state = state;

  try {
    state->native_drop_timer.start(
        slint::TimerMode::Repeated, native_drop_retry_interval,
        [weak, weak_state] {
          const auto state = weak_state.lock();
          if (!state) return;
          try {
            if (state->native_drop_registration_finished || state->native_drop) {
              state->native_drop_timer.stop();
              return;
            }

            auto app = weak.lock();
            if (!app) {
              state->native_drop_registration_finished = true;
              state->native_drop_timer.stop();
              return;
            }

            const HWND hwnd = (*app)->window().win32_hwnd();
            const bool hwnd_ready = hwnd != nullptr && IsWindow(hwnd);
            const auto action = awj::ui_drop::install_retry_action(
                hwnd_ready, state->native_drop_attempts, native_drop_max_attempts);
            ++state->native_drop_attempts;

            if (action == awj::ui_drop::InstallRetryAction::retry) return;

            state->native_drop_timer.stop();
            state->native_drop_registration_finished = true;
            if (action == awj::ui_drop::InstallRetryAction::exhausted) {
              (*app)->set_status_text(to_shared(
                  "Windows 原生拖放未启用：等待 Windows 窗口句柄就绪超时。"));
              return;
            }

            auto registration = awj::ui_drop::install(
                hwnd, make_native_drop_callbacks(weak, weak_state));
            if (registration) {
              state->native_drop.emplace(std::move(*registration));
            } else {
              (*app)->set_status_text(to_shared(std::format(
                  "Windows 原生拖放未启用：{}", registration.error())));
            }
          } catch (...) {
            state->native_drop_registration_finished = true;
            state->native_drop_timer.stop();
            report_ui_callback_failure(weak, "注册 Windows 原生拖放失败",
                                       "发生未预期异常。");
          }
        });
  } catch (...) {
    state->native_drop_registration_finished = true;
    report_ui_callback_failure(weak, "安排 Windows 原生拖放注册失败",
                               "无法启动事件循环重试。");
  }
}

// 把一个 worker 线程体包进 catch-all。
//
// 这不是多余的保险：从线程函数逃出的异常会直接走 std::terminate -> abort，进程
// 静默消失，只在 Windows 错误报告里留下 0xC0000409 / FAST_FAIL_FATAL_APP_EXIT(7)。
// 注意包在 std::jthread 构造外面的 try 只能接住“创建线程失败”，接不到线程体内部。
//
// 出错后不能在这里直接写 Slint 属性——那是跨线程改 UI。状态清理走带锁的
// clear_run_if_callback_not_posted 终止仍在运行的子进程并清理状态，界面提示走
// post_to_ui 回到 UI 线程。
template <class Body>
auto guarded_worker(slint::ComponentWeakHandle<AwjStudio> weak,
                    std::shared_ptr<UiState> state, std::uint64_t run_id,
                    std::string_view what, Body body) {
  return [weak, state = std::move(state), run_id, what,
          body = std::move(body)](std::stop_token token) mutable {
    try {
      body(std::move(token));
    } catch (...) {
      clear_run_if_callback_not_posted(*state, run_id);
      post_to_ui(weak, [what](AwjStudio& app) {
        app.set_running(false);
        set_status_text_noexcept(
            app, std::format("{}发生未预期异常，任务已停止。", what));
      });
    }
  };
}

std::string result_status_text(const awj::EncodeResult& result) {
  if (result.ok) {
    if (result.skipped) {
      return "已跳过";
    }
    const double ratio = result.original_bytes == 0
                             ? 0.0
                             : static_cast<double>(result.output_bytes) /
                                   static_cast<double>(result.original_bytes) *
                                   100.0;
    if (result.requested_visual_quality) {
      if (result.lossless) {
        return std::format("完成 · {} · {:.1f}% · VQ lossless · q{} · {:.2f}s",
                           awj::format_size(result.output_bytes), ratio,
                           result.final_encoder_quality, result.seconds);
      }
      const char* target_state =
          result.visual_quality_target_met ? "" : " 未达标兜底";
      return std::format("完成 · {} · {:.1f}% · VQ {:.2f}{} · q{} · {:.2f}s",
                         awj::format_size(result.output_bytes), ratio,
                         result.visual_score, target_state,
                         result.final_encoder_quality, result.seconds);
    }
    return std::format("完成 · {} · {:.1f}% · {:.2f}s",
                       awj::format_size(result.output_bytes), ratio,
                       result.seconds);
  }
  if (result.canceled) {
    return "已取消";
  }
  return result.message.empty() ? "失败"
                                : std::format("失败 · {}", result.message);
}

std::string result_log_text(const awj::EncodeResult& result) {
  if (!result.requested_visual_quality || !result.ok || result.skipped) {
    return {};
  }
  if (result.lossless) {
    return std::format("requested={} · lossless=true · final q{} · {}",
                       *result.requested_visual_quality,
                       result.final_encoder_quality,
                       awj::format_size(result.output_bytes));
  }
  const char* target_state =
      result.visual_quality_target_met ? "" : " · 未达标兜底";
  return std::format(
      "score {:.2f}{} · GMSD {:.6f} · MS-SSIM {:.6f} · Qg {:.2f} · Qm {:.2f} · "
      "q{} · {} 次 · {}",
      result.visual_score, target_state, result.raw_gmsd, result.raw_ms_ssim,
      result.gmsd_quality_score, result.msssim_quality_score,
      result.final_encoder_quality, result.search_attempt_count,
      awj::format_size(result.output_bytes));
}

bool push_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                   TaskRow row) noexcept {
  if (rows == nullptr) {
    return false;
  }
  try {
    rows->push_back(std::move(row));
    while (rows->row_count() > awj::studio_defaults::max_task_rows) {
      rows->erase(0);
    }
  } catch (...) {
    return false;
  }
  return true;
}

TaskRow task_row_from_result(const awj::EncodeResult& result) {
  std::string output_format = result.output_format;
  if (output_format.empty()) {
    auto inferred = awj::OutputFormat::avif;
    auto ext = result.output_path.extension().wstring();
    std::ranges::transform(ext, ext.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    if (ext == L".png") {
      inferred = awj::OutputFormat::png;
    } else if (ext == L".webp") {
      inferred = awj::OutputFormat::webp;
    } else if (ext == L".jxl") {
      inferred = awj::OutputFormat::jxl;
    } else if (result.encoder_id == "jpegli") {
      inferred = awj::OutputFormat::jpgli;
    }
    output_format = awj::output_format_name(inferred);
  }
  const auto log_text = result.ok ? result_log_text(result) : result.message;
  return TaskRow{.order = to_shared(std::format("{}", result.index + 1)),
                 .filename = to_shared(awj::path_to_utf8(result.input_path.filename())),
                 .folder = to_shared(awj::path_to_utf8(result.input_path.parent_path())),
                 .size = to_shared(awj::format_size(result.original_bytes)),
                 .status = to_shared(result_status_text(result)),
                 .output = to_shared(awj::path_to_utf8(result.output_path.filename())),
                 .log = to_shared(log_text),
                 .warning = result.ok && result.requested_visual_quality.has_value() &&
                             !result.visual_quality_target_met,
                 .locked = result.processed,
                 .state = result.ok ? 2 : (result.canceled ? 4 : 3),
                 .input_path = to_shared(awj::path_to_utf8(result.input_path)),
                 .output_path = to_shared(awj::path_to_utf8(result.output_path)),
                 .encoder = to_shared(result.encoder_id),
                 .threads = result.encoder_threads > 0
                                ? to_shared(std::format("{}", result.encoder_threads))
                                : slint::SharedString{},
                 .stage_timings = to_shared(stage_timings_text(
                     result.decode_seconds, result.prepare_seconds,
                     result.encode_seconds, result.write_seconds))};
}

TaskRow pending_shell_task_row(const awj::AppConfig& cfg, const awj::ImageFile& image) {
  return TaskRow{.order = to_shared(std::format("{}", image.index + 1)),
                 .filename = to_shared(awj::path_to_utf8(image.path.filename())),
                 .folder = to_shared(awj::path_to_utf8(image.path.parent_path())),
                 .size = to_shared(awj::format_size(image.bytes)),
                 .status = to_shared("等待转码"),
                 .output = to_shared(awj::path_to_utf8(awj::output_path_for(cfg, image).filename())),
                 .log = {},
                 .warning = false,
                 .locked = true,
                 .state = 0,
                 .input_path = to_shared(awj::path_to_utf8(image.path)),
                 .output_path = to_shared(awj::path_to_utf8(awj::output_path_for(cfg, image)))};
}

void mark_task_row_running(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::EncodeResult& result) noexcept {
  if (rows == nullptr) {
    return;
  }
  try {
    if (result.index < rows->row_count()) {
      auto row = rows->row_data(result.index);
      if (row) {
        row->status = to_shared("正在转码");
        row->locked = true;
        row->state = 1;
        rows->set_row_data(result.index, *row);
        return;
      }
    }
    push_task_row(rows,
                  TaskRow{.order = to_shared(std::format("{}", result.index + 1)),
                          .filename = to_shared(awj::path_to_utf8(
                              result.input_path.filename())),
                          .folder = to_shared(awj::path_to_utf8(
                              result.input_path.parent_path())),
                          .size = to_shared(awj::format_size(result.original_bytes)),
                          .status = to_shared("正在转码"),
                           .output = to_shared(awj::path_to_utf8(
                               result.output_path.filename())),
                           .locked = true,
                           .state = 1,
                           .input_path = to_shared(awj::path_to_utf8(result.input_path)),
                           .output_path = to_shared(awj::path_to_utf8(result.output_path))});
  } catch (...) {
  }
}

void add_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                  const awj::EncodeResult& result) noexcept {
  try {
    push_task_row(rows, task_row_from_result(result));
  } catch (...) {
  }
}

void append_log_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                    std::string_view text) noexcept {
  try {
    push_task_row(rows, TaskRow{.order = {},
                                .filename = {},
                                .folder = {},
                                .size = {},
                                .status = {},
                                .output = {},
                                .log = to_shared(text),
                                .warning = false,
                                .locked = true});
  } catch (...) {
  }
}

bool large_image_grid_available(const awj::BatchLargeImageItem& item) noexcept {
  if (!item.decision.available_grid) {
    return false;
  }
  // 可用性判断必须和真正执行的 CLI 一致：手动 grid 会带上
  // --experimental-clamped-grid-padding（Studio 不提供关闭入口，始终是默认值），
  // 而 pipeline 的 try_grid 接受 clamped 计划，编码器也按右列/底行的真实剩余
  // 尺寸切 tile。这里再拒绝 uses_padding 只会把 CLI 能做的事灰掉。
  const auto plan = awj::plan_grid(awj::GridPlanRequest{
      .width = item.dimensions.width,
      .height = item.dimensions.height,
      .mode = awj::GridMode::auto_grid,
      .clamped_padding_enabled =
          awj::encoding_defaults::default_experimental_clamped_grid_padding});
  return plan.has_value();
}

bool large_image_zenrav1e_available(
    const awj::BatchLargeImageItem& item) noexcept {
  return item.decision.available_zenrav1e &&
         item.dimensions.width <=
             awj::encoding_defaults::avif_single_image_max_dimension &&
         item.dimensions.height <=
             awj::encoding_defaults::avif_single_image_max_dimension;
}

bool large_image_action_available(const awj::BatchLargeImageItem& item,
                                  std::string_view action) noexcept {
  if (action == "grid") {
    return large_image_grid_available(item);
  }
  if (action == "zenrav1e") {
    return large_image_zenrav1e_available(item);
  }
  return false;
}

int preferred_large_image_action_index(
    const awj::BatchLargeImageItem& item) noexcept {
  if (large_image_zenrav1e_available(item)) {
    return 0;
  }
  if (large_image_grid_available(item)) {
    return 1;
  }
  return 0;
}

std::string large_image_actions_summary(const awj::BatchLargeImageItem& item) {
  const auto grid = large_image_grid_available(item)
                        ? std::string{"grid 可用"}
                        : std::string{"grid 不可用"};
  std::string zenrav1e;
  if (large_image_zenrav1e_available(item)) {
    zenrav1e = "zenrav1e 可用";
  } else if (item.decision.available_zenrav1e) {
    zenrav1e = "zenrav1e 超出边长";
  } else {
    zenrav1e = "zenrav1e 不可用";
  }
  return std::format("{} / {}", grid, zenrav1e);
}

void add_large_image_task_row(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::BatchLargeImageItem& item) noexcept {
  try {
    push_task_row(
        rows,
        TaskRow{
            .order = to_shared(std::format("{}", item.file.index + 1)),
            .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
            .folder =
                to_shared(awj::path_to_utf8(item.file.path.parent_path())),
            .size = to_shared(awj::format_size(item.file.bytes)),
            .status = to_shared("大图模式"),
            .output = {},
            .log = to_shared(std::format(
                "原因：{}；{}；可用处理方式：{}。",
                awj::large_image_reason_name(item.decision.reason),
                item.decision.reason_text, large_image_actions_summary(item))),
            .warning = false,
            .locked = true});
  } catch (...) {
  }
}

LargeImageRow make_large_image_row(const awj::BatchLargeImageItem& item,
                                   std::string_view status) {
  const auto actions = large_image_actions_summary(item);

  return LargeImageRow{
      .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
      .dimensions = to_shared(std::format("{} x {}", item.dimensions.width,
                                          item.dimensions.height)),
      .reason = to_shared(std::format(
          "{} · {}", awj::large_image_reason_name(item.decision.reason),
          item.decision.reason_text)),
      .actions = to_shared(actions),
      .status = to_shared(status),
      .grid_available = large_image_grid_available(item),
      .zenrav1e_available = large_image_zenrav1e_available(item)};
}

bool push_large_image_row(UiState& state,
                          awj::BatchLargeImageItem item) noexcept {
  if (state.large_image_rows == nullptr) {
    return false;
  }
  try {
    const bool was_empty = state.large_image_items.empty();
    auto row = make_large_image_row(item, "等待选择");
    bool item_added = false;
    try {
      state.large_image_items.push_back(item);
      item_added = true;
      state.large_image_rows->push_back(std::move(row));
    } catch (...) {
      if (item_added) {
        state.large_image_items.pop_back();
      }
      return false;
    }
    return was_empty;
  } catch (...) {
    return false;
  }
}

void select_first_large_image_from_state(AwjStudio& app,
                                         const UiState& state) noexcept {
  try {
    if (state.large_image_items.empty()) {
      return;
    }
    app.set_selected_large_image_index(0);
    app.set_selected_page(0);
  } catch (...) {
  }
}

bool path_is_directory(const std::filesystem::path& path) noexcept {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) && !ec;
}

void set_large_image_status(UiState& state, int index,
                            std::string_view status) noexcept {
  if (state.large_image_rows == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= state.large_image_items.size()) {
    return;
  }
  try {
    state.large_image_rows->set_row_data(
        static_cast<std::size_t>(index),
        make_large_image_row(
            state.large_image_items[static_cast<std::size_t>(index)], status));
  } catch (...) {
  }
}

std::string large_image_action_status(const awj::BatchLargeImageItem& item,
                                      std::string_view action) {
  if (!large_image_action_available(item, action)) {
    return std::format("{} 不可用", action);
  }
  if (action == "grid") {
    const auto plan =
        awj::plan_grid(awj::GridPlanRequest{.width = item.dimensions.width,
                                            .height = item.dimensions.height,
                                            .mode = awj::GridMode::auto_grid});
    if (!plan) {
      return std::format("grid 规划失败：{}", plan.error());
    }
    return std::format("已选择 grid · {}x{} 分块 · tile {}x{}", plan->cols,
                       plan->rows, plan->tile_width, plan->tile_height);
  }
  if (action == "zenrav1e") {
    return "已选择 zenrav1e · 单项编码";
  }
  return "未知处理方式";
}

void append_pending_event(UiState& state, std::uint64_t run_id,
                          const awj::BatchProgress& event) noexcept {
  try {
    std::scoped_lock lock{state.mutex};
    if (state.run_id != run_id) {
      return;
    }
    if (state.pending_events.size() >=
        awj::studio_defaults::max_pending_events) {
      const auto keep = [](const awj::BatchProgress& pending) {
        return pending.kind == awj::BatchEventKind::item_finished ||
               pending.kind == awj::BatchEventKind::warning ||
               pending.kind == awj::BatchEventKind::large_image_queued;
      };
      const auto retained = std::ranges::remove_if(
          state.pending_events,
          [&](const awj::BatchProgress& pending) { return !keep(pending); });
      state.pending_events.erase(retained.begin(), retained.end());
      if (state.pending_events.size() >=
          awj::studio_defaults::max_pending_events) {
        state.pending_events.erase(
            state.pending_events.begin(),
            state.pending_events.begin() +
                static_cast<std::ptrdiff_t>(state.pending_events.size() -
                                            awj::studio_defaults::
                                                max_pending_events +
                                            1));
      }
    }
    state.pending_events.push_back(event);
  } catch (...) {
  }
}

bool request_all_workers_stop_locked(UiState& state) noexcept {
  if (state.active_child != nullptr) {
    state.active_child->request_cancel();
    return true;
  }
  if (!state.worker_active) {
    return false;
  }
  state.worker.request_stop();
  return true;
}

bool request_all_workers_stop(const std::shared_ptr<UiState>& state) noexcept {
  if (state == nullptr) {
    return false;
  }
  try {
    std::scoped_lock lock{state->mutex};
    return request_all_workers_stop_locked(*state);
  } catch (...) {
    return false;
  }
}

bool worker_active(const std::shared_ptr<UiState>& state) noexcept {
  if (state == nullptr) {
    return false;
  }
  try {
    std::scoped_lock lock{state->mutex};
    return state->worker_active;
  } catch (...) {
    return false;
  }
}

enum class ForceStopResult { no_worker, terminated, terminate_failed };

ForceStopResult force_stop_current_worker(
    const std::shared_ptr<UiState>& state) noexcept {
  if (state == nullptr) {
    return ForceStopResult::no_worker;
  }
  std::shared_ptr<StudioChildProcess> child;
  bool had_worker = false;
  try {
    std::scoped_lock lock{state->mutex};
    had_worker = state->worker_active || state->active_child != nullptr;
    child = state->active_child;
    if (child == nullptr && state->worker_active) {
      state->worker.request_stop();
    }
  } catch (...) {
    return ForceStopResult::terminate_failed;
  }
  if (child != nullptr) {
    return child->terminate() ? ForceStopResult::terminated
                              : ForceStopResult::terminate_failed;
  }
  return had_worker ? ForceStopResult::terminate_failed
                    : ForceStopResult::no_worker;
}

std::wstring cli_output_format_arg(awj::OutputFormat format) {
  switch (format) {
    case awj::OutputFormat::png:
      return L"png";
    case awj::OutputFormat::webp:
      return L"webp";
    case awj::OutputFormat::jxl:
      return L"jxl";
    case awj::OutputFormat::jpgli:
      return L"jpgli";
    case awj::OutputFormat::avif:
    default:
      return L"avif";
  }
}

std::wstring cli_collision_arg(awj::CollisionMode mode) {
  switch (mode) {
    case awj::CollisionMode::skip:
      return L"skip";
    case awj::CollisionMode::suffix_time:
      return L"time";
    case awj::CollisionMode::suffix_random:
      return L"random";
    case awj::CollisionMode::suffix_number:
      return L"number";
    case awj::CollisionMode::overwrite:
    default:
      return L"overwrite";
  }
}

std::wstring cli_chroma_arg(awj::ChromaMode mode) {
  return awj::wide_from_utf8(awj::chroma_mode_name(mode));
}

std::wstring cli_avif_encoder_arg(awj::AvifEncoderMode mode) {
  return awj::wide_from_utf8(awj::avif_encoder_mode_name(mode));
}

std::wstring cli_alpha_arg(awj::AlphaModePolicy policy) {
  return awj::wide_from_utf8(awj::alpha_mode_policy_name(policy));
}

void push_cli_option(std::vector<std::wstring>& args, std::wstring option,
                     std::wstring value) {
  args.push_back(std::move(option));
  args.push_back(std::move(value));
}

void push_cli_option(std::vector<std::wstring>& args, std::wstring option,
                     const std::filesystem::path& value) {
  push_cli_option(args, std::move(option), value.native());
}

std::wstring bytes_argument(std::uint64_t bytes) {
  return bytes == 0 ? std::wstring{L"auto"} : std::format(L"{}b", bytes);
}

std::vector<std::wstring> cli_arguments_from_config(
    const awj::AppConfig& cfg, std::wstring_view cancel_event_name) {
  std::vector<std::wstring> args;
  args.reserve(80 + cfg.svtav1hdr_params.size() * 2);

  push_cli_option(args, L"--input", cfg.input_path);
  if (!cfg.output_dir.empty()) {
    push_cli_option(args, L"--output", cfg.output_dir);
  }
  push_cli_option(args, L"--format", cli_output_format_arg(cfg.output_format));
  push_cli_option(args, L"--template", cfg.output_template);
  push_cli_option(args, L"--threads", std::to_wstring(cfg.max_jobs));
  push_cli_option(args, L"--memory-limit", bytes_argument(cfg.memory_limit_bytes));
  switch (cfg.image_size_limit.mode) {
    case awj::ImageSizeLimitMode::none:
      push_cli_option(args, L"--image-size-limit", std::wstring{L"none"});
      break;
    case awj::ImageSizeLimitMode::manual:
      push_cli_option(args, L"--image-size-limit", std::wstring{L"manual"});
      if (cfg.image_size_limit.max_width) push_cli_option(args, L"--max-width", std::to_wstring(*cfg.image_size_limit.max_width));
      if (cfg.image_size_limit.max_height) push_cli_option(args, L"--max-height", std::to_wstring(*cfg.image_size_limit.max_height));
      if (cfg.image_size_limit.max_long_edge) push_cli_option(args, L"--max-long-edge", std::to_wstring(*cfg.image_size_limit.max_long_edge));
      if (cfg.image_size_limit.max_short_edge) push_cli_option(args, L"--max-short-edge", std::to_wstring(*cfg.image_size_limit.max_short_edge));
      break;
    case awj::ImageSizeLimitMode::automatic:
    default:
      push_cli_option(args, L"--image-size-limit", std::wstring{L"auto"});
      break;
  }
  push_cli_option(args, L"--timeout-encode",
                  std::to_wstring(cfg.encode_timeout_minutes));
  push_cli_option(args, L"--collision", cli_collision_arg(cfg.collision_mode));
  push_cli_option(args, L"--studio-cancel-event",
                  std::wstring{cancel_event_name});
  if (!cfg.studio_queue_manifest.empty()) {
    push_cli_option(args, L"--studio-queue-manifest",
                    cfg.studio_queue_manifest);
  }
  if (!cfg.studio_large_action.empty()) {
    push_cli_option(args, L"--studio-large-action", cfg.studio_large_action);
  }
  push_cli_option(args, L"--large-image-priority", cfg.large_image_priority);
  if (cfg.unlock_max_input_file_bytes) {
    args.push_back(L"--unlock-max-input-file-bytes");
  }

  if (cfg.visual_quality) {
    push_cli_option(args, L"--visual-quality",
                    std::to_wstring(*cfg.visual_quality));
    args.push_back(cfg.visual_quality_fallback ? L"--visual-quality-fallback"
                                               : L"--no-visual-quality-fallback");
    args.push_back(cfg.visual_quality_gpu ? L"--visual-quality-gpu"
                                          : L"--no-visual-quality-gpu");
  } else {
    push_cli_option(args, L"--quality", std::to_wstring(cfg.quality));
  }

  if (cfg.bit_depth) {
    push_cli_option(args, L"--bit-depth", std::to_wstring(*cfg.bit_depth));
  }
  if (cfg.speed) {
    push_cli_option(args, L"--speed", std::to_wstring(*cfg.speed));
  }

  args.push_back(cfg.allow_wic_fallback ? L"--allow-wic-fallback"
                                        : L"--no-wic-fallback");
  args.push_back(cfg.enable_experimental_encoders ? L"--experimental-encoders"
                                                  : L"--no-experimental-encoders");
  args.push_back(cfg.experimental_clamped_grid_padding
                     ? L"--experimental-clamped-grid-padding"
                     : L"--no-experimental-clamped-grid-padding");
  args.push_back(cfg.strip_metadata ? L"--strip" : L"--keep-metadata");
  args.push_back(cfg.write_summary ? L"--summary" : L"--no-summary");
  args.push_back(cfg.write_log ? L"--log" : L"--no-log");
  if (cfg.preserve_creation_time) args.push_back(L"--preserve-creation-time");
  if (cfg.preserve_modification_time) args.push_back(L"--preserve-modification-time");
  if (cfg.preserve_access_time) args.push_back(L"--preserve-access-time");

  if (cfg.output_format == awj::OutputFormat::avif) {
    push_cli_option(args, L"--avif-encoder",
                    cli_avif_encoder_arg(cfg.avif_encoder));
    push_cli_option(args, L"--chroma", cli_chroma_arg(cfg.chroma_mode));
    push_cli_option(args, L"--alpha", cli_alpha_arg(cfg.alpha_policy));
  }
  if (cfg.output_format == awj::OutputFormat::jpgli) {
    push_cli_option(args, L"--chroma", cli_chroma_arg(cfg.chroma_mode));
    push_cli_option(args, L"--jpegli-progressive-level",
                    std::to_wstring(cfg.jpegli_progressive_level));
    args.push_back(cfg.jpegli_optimize_huffman
                       ? L"--jpegli-optimize-huffman"
                       : L"--no-jpegli-optimize-huffman");
    if (cfg.jpegli_xyb) {
      args.push_back(L"--jpegli-xyb");
    }
  }

  if (cfg.svtav1hdr_crf) {
    push_cli_option(args, L"--svtav1hdr-crf",
                    std::to_wstring(*cfg.svtav1hdr_crf));
  }
  if (cfg.svtav1hdr_preset) {
    push_cli_option(args, L"--svtav1hdr-preset",
                    std::to_wstring(*cfg.svtav1hdr_preset));
  }
  if (!cfg.svtav1hdr_tune.empty()) {
    push_cli_option(args, L"--svtav1hdr-tune",
                    awj::wide_from_utf8(cfg.svtav1hdr_tune));
  }
  if (cfg.svtav1hdr_keyint) {
    push_cli_option(args, L"--svtav1hdr-keyint",
                    std::to_wstring(*cfg.svtav1hdr_keyint));
  }
  for (const auto& param : cfg.svtav1hdr_params) {
    push_cli_option(args, L"--svtav1hdr-params", param);
  }
  if (cfg.color_primaries) {
    push_cli_option(args, L"--color-primaries",
                    std::to_wstring(*cfg.color_primaries));
  }
  if (cfg.transfer_characteristics) {
    push_cli_option(args, L"--transfer-characteristics",
                    std::to_wstring(*cfg.transfer_characteristics));
  }
  if (cfg.matrix_coefficients) {
    push_cli_option(args, L"--matrix-coefficients",
                    std::to_wstring(*cfg.matrix_coefficients));
  }
  if (cfg.color_range) {
    push_cli_option(args, L"--color-range", std::to_wstring(*cfg.color_range));
  }
  if (!cfg.mastering_display.empty()) {
    push_cli_option(args, L"--mastering-display", cfg.mastering_display);
  }
  if (!cfg.content_light.empty()) {
    push_cli_option(args, L"--content-light", cfg.content_light);
  }

  return args;
}

std::wstring quote_windows_command_arg(std::wstring_view arg,
                                      bool force_quotes = false) {
  if (arg.empty()) {
    return L"\"\"";
  }
  const bool needs_quotes =
      force_quotes || arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needs_quotes) {
    return std::wstring{arg};
  }
  std::wstring quoted;
  quoted.reserve(arg.size() + 2);
  quoted.push_back(L'\"');
  std::size_t backslashes = 0;
  for (const wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

std::wstring command_line_from_args(std::span<const std::wstring> args) {
  std::wstring command;
  bool first = true;
  for (const auto& arg : args) {
    if (!command.empty()) {
      command.push_back(L' ');
    }
    command += quote_windows_command_arg(arg, first);
    first = false;
  }
  return command;
}

std::expected<std::filesystem::path, std::string> awj_exe_path_for_shell_menu() {
  auto executable = awj::executable_path();
  if (!executable) {
    return std::unexpected{executable.error()};
  }
  auto path = *executable;
  auto extension = path.extension().wstring();
  std::ranges::transform(extension, extension.begin(),
                         [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  if (extension == L".com") {
    auto exe = path;
    exe.replace_extension(L".exe");
    std::error_code ec;
    if (std::filesystem::exists(exe, ec) && !ec) {
      return exe;
    }
  }
  auto sibling = path.parent_path() / L"AWJ.exe";
  std::error_code ec;
  if (std::filesystem::exists(sibling, ec) && !ec) {
    return sibling;
  }
  return path;
}

awj::shell_context_menu::FormatParams shell_format_params(const MenuFormatParams& params) {
  const auto text = [](const std::string& value) {
    return awj::wide_from_utf8(trim_copy(value));
  };
  return awj::shell_context_menu::FormatParams{
      .quality_text = text(params.quality_text),
      .bit_depth_text = text(params.bit_depth_text),
      .speed_text = text(params.speed_text),
      .avif_encoder_index = params.avif_encoder_index,
      .avif_color_representation_index = params.avif_color_representation_index,
      .chroma_index = params.chroma_index,
      .alpha_policy_index = params.alpha_policy_index,
      .jpegli_progressive_index = params.jpegli_progressive_index,
      .jpegli_optimize_huffman = params.jpegli_optimize_huffman,
      .jpegli_xyb = params.jpegli_xyb,
      .strip_metadata = params.strip_metadata,
      .allow_wic_fallback = params.allow_wic_fallback,
      .close_on_finish = params.close_on_finish,
      .install_avif_png_command = params.install_avif_png_command,
      .size_limit_index = params.size_limit_index,
      .max_width_text = text(params.max_width_text),
      .max_height_text = text(params.max_height_text),
      .max_long_edge_text = text(params.max_long_edge_text),
      .max_short_edge_text = text(params.max_short_edge_text)};
}

awj::shell_context_menu::MenuParams shell_menu_params(
    const std::array<MenuFormatParams, 5>& menu_params) {
  awj::shell_context_menu::MenuParams converted{};
  for (std::size_t i = 0; i < menu_params.size(); ++i) {
    converted[i] = shell_format_params(menu_params[i]);
  }
  return converted;
}

std::wstring shell_batch_name(awj::OutputFormat format, bool append_png_suffix) {
  return std::format(L"AWJimage.ShellBatch.{}{}",
                     cli_output_format_arg(format),
                     append_png_suffix ? L".png-suffix" : L"");
}

std::expected<bool, std::string> collect_shell_launch_inputs(
    awj::OutputFormat format, bool append_png_suffix,
    std::vector<std::filesystem::path>& inputs) {
  const auto suffix = shell_batch_name(format, append_png_suffix);
  const auto mutex_name = L"Local\\" + suffix;
  UniqueWin32Handle mutex{CreateMutexW(nullptr, TRUE, mutex_name.c_str())};
  if (!mutex) {
    return std::unexpected{std::format("创建右键队列锁失败，错误码 {}。",
                                       GetLastError())};
  }
  const bool leader = GetLastError() != ERROR_ALREADY_EXISTS;
  const auto send_mutex_name = mutex_name + L".Send";
  UniqueWin32Handle send_mutex{
      CreateMutexW(nullptr, FALSE, send_mutex_name.c_str())};
  if (!send_mutex) {
    const DWORD error = GetLastError();
    if (leader) ReleaseMutex(mutex.get());
    return std::unexpected{std::format("创建右键队列发送锁失败，错误码 {}。",
                                       error)};
  }
  const auto slot_name = L"\\\\.\\mailslot\\" + suffix;
  if (!leader) {
    std::vector<std::filesystem::path> unsent;
    unsent.reserve(inputs.size());
    for (int attempt = 0; attempt < 25; ++attempt) {
      const DWORD gate = WaitForSingleObject(send_mutex.get(), 1000);
      if (gate != WAIT_OBJECT_0 && gate != WAIT_ABANDONED) {
        return true;
      }
      const DWORD leader_state = WaitForSingleObject(mutex.get(), 0);
      if (leader_state != WAIT_TIMEOUT) {
        if (leader_state == WAIT_OBJECT_0 || leader_state == WAIT_ABANDONED) {
          ReleaseMutex(mutex.get());
        }
        ReleaseMutex(send_mutex.get());
        return true;
      }
      UniqueWin32Handle slot{adopt_win32_handle(
          CreateFileW(slot_name.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))};
      if (slot) {
        for (const auto& input : inputs) {
          const auto value = input.wstring();
          if (value.empty() || value.size() > 32760) {
            unsent.push_back(input);
            continue;
          }
          const auto byte_count =
              static_cast<DWORD>(value.size() * sizeof(wchar_t));
          DWORD written = 0;
          if (!WriteFile(slot.get(), value.data(), byte_count, &written,
                         nullptr) ||
              written != byte_count) {
            unsent.push_back(input);
          }
        }
        ReleaseMutex(send_mutex.get());
        if (unsent.empty()) return false;
        inputs = std::move(unsent);
        return true;
      }
      ReleaseMutex(send_mutex.get());
      Sleep(20);
    }
    return true;
  }

  const DWORD create_gate = WaitForSingleObject(send_mutex.get(), INFINITE);
  if (create_gate != WAIT_OBJECT_0 && create_gate != WAIT_ABANDONED) {
    ReleaseMutex(mutex.get());
    return std::unexpected{"无法锁定右键队列发送通道。"};
  }
  UniqueWin32Handle slot{
      adopt_win32_handle(CreateMailslotW(slot_name.c_str(), 65520, 0, nullptr))};
  if (!slot) {
    const DWORD error = GetLastError();
    ReleaseMutex(send_mutex.get());
    ReleaseMutex(mutex.get());
    return std::unexpected{std::format("创建右键队列通道失败，错误码 {}。",
                                       error)};
  }
  ReleaseMutex(send_mutex.get());
  std::unordered_set<std::wstring> seen;
  for (const auto& input : inputs) seen.insert(queue_path_key(input));
  const auto started = std::chrono::steady_clock::now();
  const auto hard_deadline = started + std::chrono::milliseconds{650};
  auto quiet_deadline = started + std::chrono::milliseconds{180};
  const auto drain_messages = [&] {
    DWORD next_size = MAILSLOT_NO_MESSAGE;
    DWORD message_count = 0;
    bool received_message = false;
    if (GetMailslotInfo(slot.get(), nullptr, &next_size, &message_count, nullptr) &&
        next_size != MAILSLOT_NO_MESSAGE) {
      while (message_count > 0 && next_size != MAILSLOT_NO_MESSAGE) {
        std::vector<wchar_t> buffer((next_size / sizeof(wchar_t)) + 1, L'\0');
        DWORD read = 0;
        if (ReadFile(slot.get(), buffer.data(), next_size, &read, nullptr) &&
            read > 0 && read % sizeof(wchar_t) == 0) {
          received_message = true;
          std::filesystem::path input{
              std::wstring{buffer.data(), read / sizeof(wchar_t)}};
          if (seen.insert(queue_path_key(input)).second) {
            inputs.push_back(std::move(input));
          }
        }
        if (!GetMailslotInfo(slot.get(), nullptr, &next_size, &message_count,
                             nullptr)) {
          break;
        }
      }
    }
    return received_message;
  };
  while (true) {
    const bool received_message = drain_messages();
    const auto now = std::chrono::steady_clock::now();
    if (received_message) {
      quiet_deadline = std::min(
          hard_deadline, now + std::chrono::milliseconds{80});
    }
    if (now >= hard_deadline || now >= quiet_deadline) break;
    Sleep(10);
  }

  // Stop new writers before the final drain so a successful late write cannot be lost.
  const DWORD close_gate = WaitForSingleObject(send_mutex.get(), INFINITE);
  if (close_gate != WAIT_OBJECT_0 && close_gate != WAIT_ABANDONED) {
    ReleaseMutex(mutex.get());
    return std::unexpected{"无法关闭右键队列发送通道。"};
  }
  (void)drain_messages();
  slot.reset();
  ReleaseMutex(mutex.get());
  ReleaseMutex(send_mutex.get());
  return true;
}

std::expected<void, std::string> install_shell_context_menu(
    const std::array<MenuFormatParams, 5>& menu_params) {
  auto awj_exe = awj_exe_path_for_shell_menu();
  if (!awj_exe) return std::unexpected{awj_exe.error()};
  return awj::shell_context_menu::install(*awj_exe, shell_menu_params(menu_params));
}

std::expected<void, std::string> remove_shell_context_menu() {
  return awj::shell_context_menu::remove();
}

std::optional<std::string> shell_context_menu_warning(
    const std::array<MenuFormatParams, 5>& menu_params) {
  auto awj_exe = awj_exe_path_for_shell_menu();
  if (!awj_exe) {
    return "无法检查右键菜单程序路径，请移除后重新安装。";
  }
  auto checked = awj::shell_context_menu::warning(*awj_exe,
                                                   shell_menu_params(menu_params));
  if (!checked) {
    return "检查右键菜单注册表失败：" + checked.error();
  }
  return *checked;
}

std::expected<std::shared_ptr<StudioChildProcess>, std::string>
start_studio_cli_worker(const awj::AppConfig& cfg, std::uint64_t run_id) {
  auto exe_path = awj::executable_path();
  if (!exe_path) {
    return std::unexpected{exe_path.error()};
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(*exe_path, ec) || ec) {
    return std::unexpected{std::format("找不到 Studio 编码 worker: {}。",
                                       awj::display_path_for_user(*exe_path))};
  }
  const auto exe_dir = exe_path->parent_path();

  auto child = std::make_shared<StudioChildProcess>();
  child->queue_manifest_path = cfg.studio_queue_manifest;
  child->temp_directories.push_back(awj::output_dir_for(cfg));
  const auto event_name = std::format(L"Local\\AWJStudioCancel-{}-{}",
                                      GetCurrentProcessId(), run_id);
  child->cancel_event.reset(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
  if (child->cancel_event == nullptr) {
    return std::unexpected{std::format("创建编码取消事件失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }

  auto args = cli_arguments_from_config(cfg, event_name);
  args.insert(args.begin(), exe_path->native());
  auto command_line = command_line_from_args(args);
  if (command_line.size() >= 32767) {
    return std::unexpected{"编码 worker 命令行超过 Windows 长度限制。"};
  }
  child->command_line = command_line;

  UniqueWin32Handle job{CreateJobObjectW(nullptr, nullptr)};
  if (job == nullptr) {
    return std::unexpected{std::format(
        "创建编码 worker Job Object 失败: {}",
        awj::win32_error_message(GetLastError()))};
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    return std::unexpected{std::format(
        "配置编码 worker Job Object 失败: {}",
        awj::win32_error_message(GetLastError()))};
  }

  HANDLE pipe_read = nullptr;
  HANDLE pipe_write = nullptr;
  SECURITY_ATTRIBUTES pipe_security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                    .lpSecurityDescriptor = nullptr,
                                    .bInheritHandle = TRUE};
  if (!CreatePipe(&pipe_read, &pipe_write, &pipe_security, 0)) {
    return std::unexpected{std::format("创建编码 worker 输出管道失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }
  UniqueWin32Handle output_read{pipe_read};
  UniqueWin32Handle output_write{pipe_write};
  SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output_write.get();
  startup.hStdError = output_write.get();
  PROCESS_INFORMATION process{};
  auto mutable_command_line = command_line;
  const auto cwd = exe_dir.native();
  const BOOL created = CreateProcessW(
      exe_path->c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
      nullptr, cwd.c_str(),
      &startup, &process);
  if (!created) {
    return std::unexpected{std::format("启动编码 worker 失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }
  child->process.reset(process.hProcess);
  child->thread.reset(process.hThread);
  child->output_read = std::move(output_read);
  output_write.reset();
  child->process_id = process.dwProcessId;
  if (!AssignProcessToJobObject(job.get(), child->process.get())) {
    const DWORD error = GetLastError();
    TerminateProcess(child->process.get(),
                     awj::studio_defaults::worker_force_stop_exit_code);
    WaitForSingleObject(child->process.get(), INFINITE);
    return std::unexpected{std::format(
        "将编码 worker 加入 Job Object 失败: {}",
        awj::win32_error_message(error))};
  }
  child->job = std::move(job);
  if (ResumeThread(child->thread.get()) == static_cast<DWORD>(-1)) {
    const DWORD error = GetLastError();
    TerminateJobObject(child->job.get(),
                       awj::studio_defaults::worker_force_stop_exit_code);
    WaitForSingleObject(child->process.get(), INFINITE);
    return std::unexpected{std::format(
        "恢复编码 worker 执行失败: {}",
        awj::win32_error_message(error))};
  }
  return child;
}

void cleanup_studio_queue_manifest(
    const std::shared_ptr<StudioChildProcess>& child) noexcept {
  if (child == nullptr || child->queue_manifest_path.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove(child->queue_manifest_path, ec);
  child->queue_manifest_path.clear();
}

void cleanup_forced_worker_temp_files(
    const std::shared_ptr<StudioChildProcess>& child) noexcept {
  try {
    if (child == nullptr || child->process_id == 0) {
      return;
    }
    const auto output_prefix =
        std::format(L".awj-output-{}-", child->process_id);
    const auto summary_prefix =
        std::format(L"summary.csv.tmp-{}-", child->process_id);
    for (const auto& directory : child->temp_directories) {
      std::error_code ec;
      std::filesystem::directory_iterator it{
          directory, std::filesystem::directory_options::skip_permission_denied,
          ec};
      const std::filesystem::directory_iterator end;
      while (!ec && it != end) {
        const auto entry = *it;
        it.increment(ec);
        const auto name = entry.path().filename().wstring();
        const bool output_temp =
            name.starts_with(output_prefix) && name.ends_with(L".tmp");
        const bool summary_temp = name.starts_with(summary_prefix);
        if (!output_temp && !summary_temp) {
          continue;
        }
        std::error_code type_ec;
        if (entry.is_regular_file(type_ec) && !type_ec) {
          std::error_code remove_ec;
          std::filesystem::remove(entry.path(), remove_ec);
        }
      }
    }
  } catch (...) {
  }
}

bool reject_when_worker_active(AwjStudio& app,
                               const std::shared_ptr<UiState>& state,
                               const char* message) {
  {
    std::scoped_lock lock{state->mutex};
    if (!state->worker_active) {
      return false;
    }
  }
  app.set_status_text(to_shared(message));
  return true;
}

void trim_process_working_set() {
  SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                           static_cast<SIZE_T>(-1));
}

LONG physical_extent_to_long(std::uint32_t value) noexcept {
  return static_cast<LONG>(std::min<std::uint32_t>(
      value, static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())));
}

LONG add_window_extent(LONG origin, LONG extent) noexcept {
  if (origin > std::numeric_limits<LONG>::max() - extent) {
    return std::numeric_limits<LONG>::max();
  }
  return origin + extent;
}

void constrain_window_to_work_area(slint::Window& window) {
  auto physical_size = window.size();
  auto position = window.position();
  const LONG current_width = physical_extent_to_long(physical_size.width);
  const LONG current_height = physical_extent_to_long(physical_size.height);
  RECT current_rect{position.x, position.y,
                    add_window_extent(position.x, current_width),
                    add_window_extent(position.y, current_height)};
  LONG non_client_width = 0;
  LONG non_client_height = 0;
  if (const auto hwnd = window.win32_hwnd(); hwnd != nullptr) {
    RECT window_rect{};
    RECT client_rect{};
    if (GetWindowRect(hwnd, &window_rect) && GetClientRect(hwnd, &client_rect)) {
      current_rect = window_rect;
      non_client_width = std::max<LONG>(
          0, (window_rect.right - window_rect.left) -
                 (client_rect.right - client_rect.left));
      non_client_height = std::max<LONG>(
          0, (window_rect.bottom - window_rect.top) -
                 (client_rect.bottom - client_rect.top));
    }
  }

  HMONITOR monitor = MonitorFromRect(&current_rect, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{.cbSize = sizeof(MONITORINFO)};
  if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitor_info)) {
    return;
  }

  const RECT& work = monitor_info.rcWork;
  const auto work_width = std::max<LONG>(1, work.right - work.left);
  const auto work_height = std::max<LONG>(1, work.bottom - work.top);
  const auto max_client_width = std::max<LONG>(1, work_width - non_client_width);
  const auto max_client_height =
      std::max<LONG>(1, work_height - non_client_height);
  const auto clamped_width = std::min<std::uint32_t>(
      physical_size.width, static_cast<std::uint32_t>(max_client_width));
  const auto clamped_height = std::min<std::uint32_t>(
      physical_size.height, static_cast<std::uint32_t>(max_client_height));

  if (clamped_width != physical_size.width ||
      clamped_height != physical_size.height) {
    physical_size = slint::PhysicalSize{{clamped_width, clamped_height}};
    window.set_size(physical_size);
  }

  const auto width =
      physical_extent_to_long(physical_size.width) + non_client_width;
  const auto height =
      physical_extent_to_long(physical_size.height) + non_client_height;
  const auto max_x = work.right - width;
  const auto max_y = work.bottom - height;
  const auto clamped_x =
      std::clamp<LONG>(current_rect.left, work.left,
                       std::max(work.left, max_x));
  const auto clamped_y =
      std::clamp<LONG>(current_rect.top, work.top,
                       std::max(work.top, max_y));
  if (clamped_x != current_rect.left || clamped_y != current_rect.top) {
    window.set_position(slint::PhysicalPosition{{clamped_x, clamped_y}});
  }
}

awj::CollisionMode collision_from_index(int index) {
  switch (index) {
    case 1:
      return awj::CollisionMode::skip;
    case 2:
      return awj::CollisionMode::suffix_time;
    case 3:
      return awj::CollisionMode::suffix_random;
    case 0:
    default:
      return awj::CollisionMode::overwrite;
  }
}

awj::OutputFormat output_format_from_index(int index) {
  switch (index) {
    case 1:
      return awj::OutputFormat::webp;
    case 2:
      return awj::OutputFormat::jxl;
    case 3:
      return awj::OutputFormat::jpgli;
    case 4:
      return awj::OutputFormat::png;
    case 0:
    default:
      return awj::OutputFormat::avif;
  }
}

struct QueueFormatChoice {
  int format_index{};
  bool append_png_suffix{};
};

constexpr QueueFormatChoice queue_format_choice_from_index(int index) noexcept {
  const int choice = std::clamp(index, 0, 5);
  if (choice == 1) {
    return {.format_index = 0, .append_png_suffix = true};
  }
  return {.format_index = choice == 0 ? 0 : choice - 1,
          .append_png_suffix = false};
}

static_assert(queue_format_choice_from_index(0).format_index == 0);
static_assert(queue_format_choice_from_index(1).format_index == 0 &&
              queue_format_choice_from_index(1).append_png_suffix);
static_assert(queue_format_choice_from_index(2).format_index == 1);
static_assert(queue_format_choice_from_index(3).format_index == 2);
static_assert(queue_format_choice_from_index(4).format_index == 3);
static_assert(queue_format_choice_from_index(5).format_index == 4);

awj::ChromaMode chroma_from_index(int index) {
  switch (index) {
    case 1:
      return awj::ChromaMode::yuv444;
    case 2:
      return awj::ChromaMode::yuv422;
    case 3:
      return awj::ChromaMode::yuv420;
    case 0:
    default:
      return awj::ChromaMode::auto_keep;
  }
}

awj::AvifColorRepresentation avif_color_representation_from_index(
    int index) noexcept {
  switch (index) {
    case 1:
      return awj::AvifColorRepresentation::source;
    case 2:
      return awj::AvifColorRepresentation::rgb_identity;
    case 0:
    default:
      return awj::AvifColorRepresentation::yuv;
  }
}

// The parameter-page order differs from OutputFormat's enum order.
int parameter_index_from_output_format(awj::OutputFormat format) noexcept {
  switch (format) {
    case awj::OutputFormat::avif:
      return 0;
    case awj::OutputFormat::webp:
      return 1;
    case awj::OutputFormat::jxl:
      return 2;
    case awj::OutputFormat::jpgli:
      return 3;
    case awj::OutputFormat::png:
    default:
      return 4;
  }
}

int parameter_editor_format_index(int index) noexcept {
  return index >= 0 && index < 5 ? index : 0;
}

awj::AlphaModePolicy alpha_policy_from_index(int index) {
  switch (index) {
    case 0:
      return awj::AlphaModePolicy::force;
    case 2:
      return awj::AlphaModePolicy::off;
    case 1:
    default:
      return awj::AlphaModePolicy::automatic;
  }
}

awj::AvifEncoderMode avif_encoder_from_index(int index) {
  switch (index) {
    case 1:
      return awj::AvifEncoderMode::svt;
    case 2:
      return awj::AvifEncoderMode::aom;
    case 3:
      return awj::AvifEncoderMode::zenrav1e;
    case 0:
    default:
      return awj::AvifEncoderMode::automatic;
  }
}

std::expected<awj::AppConfig, std::string> config_from_menu_params(
    awj::OutputFormat format, const MenuFormatParams& params) try {
  awj::AppConfig cfg = awj::default_app_config();
  cfg.output_format = format;
  cfg.output_policy = awj::OutputPolicy::shell;
  cfg.collision_mode = awj::CollisionMode::suffix_number;
  cfg.strip_metadata = params.strip_metadata;
  cfg.allow_wic_fallback = params.allow_wic_fallback;

  const auto quality = parse_quality_field(params.quality_text);
  if (!quality) return std::unexpected{quality.error()};
  cfg.quality = *quality;

  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jpgli || format == awj::OutputFormat::png) {
    const auto bit_depth = parse_bit_depth_field(params.bit_depth_text);
    if (!bit_depth) return std::unexpected{bit_depth.error()};
    cfg.bit_depth = *bit_depth;
  }
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    const auto speed = parse_optional_int_field(params.speed_text, "speed", 0, 10);
    if (!speed) return std::unexpected{speed.error()};
    cfg.speed = *speed;
  }
  if (format == awj::OutputFormat::avif) {
    cfg.avif_encoder = avif_encoder_from_index(params.avif_encoder_index);
    cfg.avif_color_representation = avif_color_representation_from_index(
        params.avif_color_representation_index);
    cfg.chroma_mode = chroma_from_index(params.chroma_index);
    cfg.alpha_policy = alpha_policy_from_index(params.alpha_policy_index);
  } else if (format == awj::OutputFormat::jpgli) {
    cfg.chroma_mode = chroma_from_index(params.chroma_index);
    cfg.jpegli_progressive_level = std::clamp(params.jpegli_progressive_index, 0, 2);
    cfg.jpegli_optimize_huffman = cfg.jpegli_progressive_level > 0
                                      ? true
                                      : params.jpegli_optimize_huffman;
    cfg.jpegli_xyb = params.jpegli_xyb;
  }
  const auto size_limit = image_size_limit_from_fields(
      params.size_limit_index, params.max_width_text, params.max_height_text,
      params.max_long_edge_text, params.max_short_edge_text);
  if (!size_limit) return std::unexpected{size_limit.error()};
  cfg.image_size_limit = *size_limit;
  if (auto valid = awj::finalize_config_defaults(cfg, true, false); !valid) {
    return std::unexpected{valid.error()};
  }
  return cfg;
} catch (const std::bad_alloc&) {
  return std::unexpected{"菜单参数解析内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"菜单参数解析数据超过运行时限制。"};
}

std::expected<void, std::string> validate_menu_params(
    const std::array<MenuFormatParams, 5>& params) {
  constexpr std::array<std::string_view, 5> labels{"AVIF", "WebP", "JXL", "JPGLI", "PNG"};
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (auto cfg = config_from_menu_params(output_format_from_index(static_cast<int>(i)), params[i]); !cfg) {
      return std::unexpected{std::format("{} 菜单参数错误：{}", labels[i], cfg.error())};
    }
  }
  return {};
}

MenuFormatParams default_menu_params_for_index(int index) {
  const auto format = output_format_from_index(index);
  MenuFormatParams params{};
  params.quality_text = text_from_int(awj::default_quality_for(format));
  if (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli) {
    params.bit_depth_text = text_from_int(awj::encoding_defaults::default_webp_bit_depth);
  }
  params.jpegli_progressive_index = awj::encoding_defaults::default_jpegli_progressive_level;
  params.jpegli_optimize_huffman = awj::encoding_defaults::default_jpegli_optimize_huffman;
  params.jpegli_xyb = awj::encoding_defaults::default_jpegli_xyb;
  params.allow_wic_fallback = awj::encoding_defaults::default_allow_wic_fallback;
  params.alpha_policy_index = 1;
  return params;
}

ParameterFormatParams default_parameter_params_for_index(int index) {
  const auto format = output_format_from_index(index);
  ParameterFormatParams params{};
  params.quality_text = text_from_int(awj::default_quality_for(format));
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    params.speed_text = text_from_int(awj::default_speed_for(format));
  }
  if (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli) {
    params.bit_depth_text =
        text_from_int(awj::encoding_defaults::default_webp_bit_depth);
  }
  params.jpegli_progressive_index =
      awj::encoding_defaults::default_jpegli_progressive_level;
  params.jpegli_optimize_huffman =
      awj::encoding_defaults::default_jpegli_optimize_huffman;
  params.jpegli_xyb = awj::encoding_defaults::default_jpegli_xyb;
  return params;
}

ParameterFormatParams capture_parameter_params_from_ui(const AwjStudio& app) {
  return ParameterFormatParams{
      .quality_text = shared_to_string(app.get_quality_text()),
      .visual_quality_text = shared_to_string(app.get_visual_quality_text()),
      .bit_depth_text = shared_to_string(app.get_bit_depth_text()),
      .speed_text = shared_to_string(app.get_speed_text()),
      .avif_encoder_index = app.get_avif_encoder_index(),
      .avif_color_representation_index =
          app.get_avif_color_representation_index(),
      .chroma_index = app.get_chroma_index(),
      .alpha_policy_index = app.get_alpha_policy_index(),
      .jpegli_progressive_index = app.get_jpegli_progressive_index(),
      .jpegli_optimize_huffman = app.get_jpegli_optimize_huffman(),
      .jpegli_xyb = app.get_jpegli_xyb(),
      .threads_text = shared_to_string(app.get_threads_text()),
      .memory_limit_text = shared_to_string(app.get_memory_limit_text()),
      .size_limit_index = app.get_size_limit_index(),
      .max_width_text = shared_to_string(app.get_max_width_text()),
      .max_height_text = shared_to_string(app.get_max_height_text()),
      .max_long_edge_text = shared_to_string(app.get_max_long_edge_text()),
      .max_short_edge_text = shared_to_string(app.get_max_short_edge_text())};
}

void apply_parameter_params_to_ui(AwjStudio& app,
                                  const ParameterFormatParams& params,
                                  int format_index) {
  const auto format = output_format_from_index(format_index);
  const bool png_lossless = format == awj::OutputFormat::png;
  app.set_quality_text(to_shared(
      png_lossless ? text_from_int(awj::default_quality_for(format))
                   : params.quality_text));
  app.set_visual_quality_text(
      to_shared(png_lossless ? std::string{} : params.visual_quality_text));
  app.set_bit_depth_text(to_shared(params.bit_depth_text));
  app.set_speed_text(to_shared(params.speed_text));
  refresh_avif_encoder_options(app);
  app.set_avif_encoder_index(params.avif_encoder_index);
  app.set_avif_color_representation_index(
      params.avif_color_representation_index);
  app.set_chroma_index(params.chroma_index);
  app.set_alpha_policy_index(params.alpha_policy_index);
  app.set_jpegli_progressive_index(params.jpegli_progressive_index);
  app.set_jpegli_optimize_huffman(params.jpegli_optimize_huffman);
  app.set_jpegli_xyb(params.jpegli_xyb);
  app.set_threads_text(to_shared(params.threads_text));
  app.set_memory_limit_text(to_shared(params.memory_limit_text));
  app.set_size_limit_index(params.size_limit_index);
  app.set_max_width_text(to_shared(params.max_width_text));
  app.set_max_height_text(to_shared(params.max_height_text));
  app.set_max_long_edge_text(to_shared(params.max_long_edge_text));
  app.set_max_short_edge_text(to_shared(params.max_short_edge_text));
  app.set_quality_follows_format(
      png_lossless ||
      params.quality_text == text_from_int(awj::default_quality_for(format)));
  app.set_bit_depth_follows_format(
      (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli)
          ? params.bit_depth_text == text_from_int(
                                      awj::encoding_defaults::default_webp_bit_depth)
          : params.bit_depth_text.empty());
}

std::array<ParameterFormatParams, 5>& active_parameter_params(UiState& state) {
  return state.parameter_preset_index == 0 ? state.builtin_params
                                           : state.parameter_preset_params;
}

const std::array<ParameterFormatParams, 5>& active_parameter_params(
    const UiState& state) {
  return state.parameter_preset_index == 0 ? state.builtin_params
                                           : state.parameter_preset_params;
}

void store_current_parameter_params(AwjStudio& app, UiState& state) {
  const auto index = parameter_editor_format_index(state.last_format_index);
  auto params = capture_parameter_params_from_ui(app);
  const auto format = output_format_from_index(index);
  if (format == awj::OutputFormat::png) {
    params.quality_text = text_from_int(awj::default_quality_for(format));
    params.visual_quality_text.clear();
  }
  if ((format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
       format == awj::OutputFormat::jxl) &&
      trim_copy(params.speed_text).empty()) {
    params.speed_text = text_from_int(awj::default_speed_for(format));
  }
  active_parameter_params(state)[static_cast<std::size_t>(index)] =
      std::move(params);
}

void apply_format_defaults_to_ui(AwjStudio& app, int format_index, UiState& state) {
  store_current_parameter_params(app, state);
  format_index = parameter_editor_format_index(format_index);
  if (app.get_format_index() != format_index) {
    app.set_format_index(format_index);
  }
  state.last_format_index = format_index;
  apply_parameter_params_to_ui(
      app, active_parameter_params(state)[static_cast<std::size_t>(format_index)],
      format_index);
}

void initialize_ui_defaults(AwjStudio& app, UiState& state) {
  const auto defaults = awj::default_app_config();
  app.set_input_path({});
  app.set_input_mode_index(0);
  app.set_template_text(to_shared(text_from_wide(defaults.output_template)));
  const auto avif_default = awj::default_quality_for(awj::OutputFormat::avif);
  state.last_format_index = 0;
  app.set_avif_quality_default(to_shared(text_from_int(avif_default)));
  app.set_webp_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::webp))));
  app.set_jxl_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::jxl))));
  app.set_jpegli_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::jpgli))));
  app.set_png_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::png))));
  app.set_webp_bit_depth_default(
      to_shared(text_from_int(awj::encoding_defaults::default_webp_bit_depth)));
  for (int i = 0; i < static_cast<int>(state.builtin_params.size()); ++i) {
    state.builtin_params[static_cast<std::size_t>(i)] =
        default_parameter_params_for_index(i);
  }
  apply_parameter_params_to_ui(app, state.builtin_params[0], 0);
  app.set_unlock_max_input_file_bytes(false);
  app.set_size_limit_index(0);
  app.set_max_width_text({});
  app.set_max_height_text({});
  app.set_max_long_edge_text({});
  app.set_max_short_edge_text({});
  app.set_format_index(0);
  app.set_experimental_encoders(defaults.enable_experimental_encoders);
  app.set_visual_quality_gpu(defaults.visual_quality_gpu);
  app.set_visual_quality_fallback(defaults.visual_quality_fallback);
  app.set_allow_wic_fallback(defaults.allow_wic_fallback);
  refresh_avif_encoder_options(app);
  app.set_avif_encoder_index(0);
  app.set_collision_index(0);
  app.set_chroma_index(0);
  app.set_jpegli_progressive_index(
      awj::encoding_defaults::default_jpegli_progressive_level);
  app.set_jpegli_optimize_huffman(
      awj::encoding_defaults::default_jpegli_optimize_huffman);
  app.set_jpegli_xyb(awj::encoding_defaults::default_jpegli_xyb);
  app.set_alpha_policy_index(1);
  app.set_quality_follows_format(true);
  app.set_bit_depth_follows_format(true);
  app.set_queue_format_index(0);
  app.set_queue_preset_index(0);
  app.set_strip_metadata(false);
  app.set_preserve_creation_time(false);
  app.set_preserve_modification_time(false);
  app.set_preserve_access_time(false);
  for (int i = 0; i < static_cast<int>(state.menu_params.size()); ++i) {
    state.menu_params[static_cast<std::size_t>(i)] = default_menu_params_for_index(i);
  }
  state.last_menu_format_index = 0;
  app.set_menu_format_index(0);
  load_menu_params_for_index(app, state, 0);
}

void reload_user_preset_options(AwjStudio& app, UiState& state) {
  auto catalog = awj::list_user_presets();
  if (!catalog) {
    state.user_presets.clear();
    state.user_preset_errors = {catalog.error()};
    const std::vector<ComboOption> builtin_only{
        {.text = to_shared("内置默认"), .enabled = true}};
    app.set_queue_preset_options(
        std::make_shared<slint::VectorModel<ComboOption>>(builtin_only));
    app.set_parameter_preset_options(
        std::make_shared<slint::VectorModel<ComboOption>>(builtin_only));
    app.set_queue_preset_index(0);
    app.set_queue_preset_description({});
    state.parameter_preset_index = 0;
    app.set_parameter_preset_index(0);
    app.set_parameter_preset_description({});
    return;
  }
  state.user_presets = std::move(catalog->presets);
  state.user_preset_errors = std::move(catalog->errors);
  std::vector<ComboOption> options;
  options.reserve(state.user_presets.size() + 1);
  options.push_back(ComboOption{.text = to_shared("内置默认"), .enabled = true});
  for (const auto& preset : state.user_presets) {
    options.push_back(ComboOption{.text = to_shared(preset.name), .enabled = true});
  }
  app.set_queue_preset_options(
      std::make_shared<slint::VectorModel<ComboOption>>(options));
  app.set_parameter_preset_options(
      std::make_shared<slint::VectorModel<ComboOption>>(std::move(options)));
  if (app.get_queue_preset_index() >
      static_cast<int>(state.user_presets.size())) {
    app.set_queue_preset_index(0);
  }
  if (state.parameter_preset_index >
      static_cast<int>(state.user_presets.size())) {
    state.parameter_preset_index = 0;
  }
  app.set_parameter_preset_index(state.parameter_preset_index);
  app.set_parameter_preset_description(
      state.parameter_preset_index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(
                                         state.parameter_preset_index - 1)]
                          .description));
  const auto queue_index = std::clamp(
      app.get_queue_preset_index(), 0,
      static_cast<int>(state.user_presets.size()));
  app.set_queue_preset_index(queue_index);
  app.set_queue_preset_description(
      queue_index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(
                          queue_index - 1)]
                          .description));
}

std::expected<awj::AppConfig, std::string> config_from_parameter_params(
    awj::OutputFormat format, const ParameterFormatParams& params) {
  const bool png_lossless = format == awj::OutputFormat::png;
  MenuFormatParams menu{
      .quality_text = png_lossless
                          ? text_from_int(awj::default_quality_for(format))
                          : params.quality_text,
      .bit_depth_text = params.bit_depth_text,
      .speed_text = params.speed_text,
      .avif_encoder_index = params.avif_encoder_index,
      .avif_color_representation_index =
          params.avif_color_representation_index,
      .chroma_index = params.chroma_index,
      .alpha_policy_index = params.alpha_policy_index,
      .jpegli_progressive_index = params.jpegli_progressive_index,
      .jpegli_optimize_huffman = params.jpegli_optimize_huffman,
      .jpegli_xyb = params.jpegli_xyb,
      .size_limit_index = params.size_limit_index,
      .max_width_text = params.max_width_text,
      .max_height_text = params.max_height_text,
      .max_long_edge_text = params.max_long_edge_text,
      .max_short_edge_text = params.max_short_edge_text};
  auto config = config_from_menu_params(format, menu);
  if (!config) return std::unexpected{config.error()};
  if (png_lossless) {
    config->visual_quality.reset();
  } else {
    const auto visual_quality =
        parse_visual_quality_field(params.visual_quality_text);
    if (!visual_quality) return std::unexpected{visual_quality.error()};
    config->visual_quality = *visual_quality;
  }
  const auto jobs = parse_jobs_field(params.threads_text);
  if (!jobs) return std::unexpected{jobs.error()};
  config->max_jobs = *jobs;
  const auto memory = parse_memory_limit_field(params.memory_limit_text);
  if (!memory) return std::unexpected{memory.error()};
  config->memory_limit_bytes = *memory;
  config->output_policy = awj::OutputPolicy::normal;
  return config;
}

int avif_encoder_index_from_mode(awj::AvifEncoderMode mode) noexcept {
  switch (mode) {
    case awj::AvifEncoderMode::svt:
      return 1;
    case awj::AvifEncoderMode::aom:
      return 2;
    case awj::AvifEncoderMode::zenrav1e:
      return 3;
    case awj::AvifEncoderMode::automatic:
    default:
      return 0;
  }
}

int chroma_index_from_mode(awj::ChromaMode mode) noexcept {
  switch (mode) {
    case awj::ChromaMode::yuv444:
      return 1;
    case awj::ChromaMode::yuv422:
      return 2;
    case awj::ChromaMode::yuv420:
      return 3;
    case awj::ChromaMode::auto_keep:
    default:
      return 0;
  }
}

int avif_color_representation_index_from_mode(
    awj::AvifColorRepresentation mode) noexcept {
  switch (mode) {
    case awj::AvifColorRepresentation::source:
      return 1;
    case awj::AvifColorRepresentation::rgb_identity:
      return 2;
    case awj::AvifColorRepresentation::yuv:
    default:
      return 0;
  }
}

int alpha_policy_index_from_mode(awj::AlphaModePolicy mode) noexcept {
  switch (mode) {
    case awj::AlphaModePolicy::force:
      return 0;
    case awj::AlphaModePolicy::off:
      return 2;
    case awj::AlphaModePolicy::automatic:
    default:
      return 1;
  }
}

ParameterFormatParams parameter_params_from_config(const awj::AppConfig& config) {
  const auto format = config.output_format;
  ParameterFormatParams params = default_parameter_params_for_index(
      parameter_index_from_output_format(format));
  const auto optional_text = [](const std::optional<int>& value) {
    return value ? text_from_int(*value) : std::string{};
  };
  params.quality_text = text_from_int(config.quality);
  params.visual_quality_text = optional_text(config.visual_quality);
  params.bit_depth_text = optional_text(config.bit_depth);
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    params.speed_text = text_from_int(
        config.speed.value_or(awj::default_speed_for(format)));
  }
  params.avif_encoder_index = avif_encoder_index_from_mode(config.avif_encoder);
  params.avif_color_representation_index =
      avif_color_representation_index_from_mode(
          config.avif_color_representation);
  params.chroma_index = chroma_index_from_mode(config.chroma_mode);
  params.alpha_policy_index = alpha_policy_index_from_mode(config.alpha_policy);
  params.jpegli_progressive_index = config.jpegli_progressive_level;
  params.jpegli_optimize_huffman = config.jpegli_optimize_huffman;
  params.jpegli_xyb = config.jpegli_xyb;
  params.threads_text = config.max_jobs == awj::default_max_jobs()
                            ? std::string{}
                            : text_from_int(config.max_jobs);
  if (config.memory_limit_bytes != 0) {
    params.memory_limit_text = text_from_int(static_cast<int>(
        (config.memory_limit_bytes + awj::studio_defaults::bytes_per_gib - 1) /
        awj::studio_defaults::bytes_per_gib));
  }
  switch (config.image_size_limit.mode) {
    case awj::ImageSizeLimitMode::none:
      params.size_limit_index = 1;
      break;
    case awj::ImageSizeLimitMode::manual:
      params.size_limit_index = 2;
      break;
    case awj::ImageSizeLimitMode::automatic:
    default:
      params.size_limit_index = 0;
      break;
  }
  params.max_width_text = optional_text(config.image_size_limit.max_width);
  params.max_height_text = optional_text(config.image_size_limit.max_height);
  params.max_long_edge_text = optional_text(config.image_size_limit.max_long_edge);
  params.max_short_edge_text = optional_text(config.image_size_limit.max_short_edge);
  return params;
}

std::array<ParameterFormatParams, 5> parameter_params_from_user_preset(
    const awj::UserPreset& preset) {
  std::array<ParameterFormatParams, 5> params{};
  for (int index = 0; index < static_cast<int>(params.size()); ++index) {
    const auto format = output_format_from_index(index);
    params[static_cast<std::size_t>(index)] = parameter_params_from_config(
        awj::config_from_user_preset(preset, format));
  }
  return params;
}

std::expected<awj::UserPreset, std::string> user_preset_from_parameter_params(
    std::string name, std::string description,
    const std::array<ParameterFormatParams, 5>& params) {
  awj::UserPreset preset = awj::default_user_preset();
  preset.name = std::move(name);
  preset.description = std::move(description);
  for (int index = 0; index < static_cast<int>(params.size()); ++index) {
    const auto format = output_format_from_index(index);
    auto config = config_from_parameter_params(
        format, params[static_cast<std::size_t>(index)]);
    if (!config) {
      return std::unexpected{std::format("{} 预设参数错误：{}",
                                         awj::output_format_name(format),
                                         config.error())};
    }
    preset.formats[static_cast<std::size_t>(index)] =
        awj::preset_format_from_config(*config);
  }
  return preset;
}

void select_parameter_preset(AwjStudio& app, UiState& state, int index) {
  store_current_parameter_params(app, state);
  index = std::clamp(index, 0, static_cast<int>(state.user_presets.size()));
  state.parameter_preset_index = index;
  if (index > 0) {
    state.parameter_preset_params = parameter_params_from_user_preset(
        state.user_presets[static_cast<std::size_t>(index - 1)]);
  }
  app.set_parameter_preset_index(index);
  app.set_parameter_preset_description(
      index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(index - 1)]
                          .description));
  const auto format_index = parameter_editor_format_index(app.get_format_index());
  state.last_format_index = format_index;
  apply_parameter_params_to_ui(
      app, active_parameter_params(state)[static_cast<std::size_t>(format_index)],
      format_index);
}

void select_queue_preset(AwjStudio& app, UiState& state, int index) {
  index = std::clamp(index, 0, static_cast<int>(state.user_presets.size()));
  app.set_queue_preset_index(index);
  app.set_queue_preset_description(
      index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(index - 1)]
                          .description));
}

std::expected<awj::AppConfig, std::string> config_from_ui(
    AwjStudio& app, UiState& state) try {
  store_current_parameter_params(app, state);
  const auto queue_choice =
      queue_format_choice_from_index(app.get_queue_format_index());
  const int queue_format_index = queue_choice.format_index;
  const auto format = output_format_from_index(queue_format_index);
  const int preset_index = app.get_queue_preset_index();
  awj::AppConfig cfg;
  if (preset_index == 0) {
    auto parsed = config_from_parameter_params(
        format, state.builtin_params[static_cast<std::size_t>(queue_format_index)]);
    if (!parsed) return std::unexpected{parsed.error()};
    cfg = std::move(*parsed);
  } else {
    const auto user_index = preset_index - 1;
    if (user_index < 0 || static_cast<std::size_t>(user_index) >=
                              state.user_presets.size()) {
      return std::unexpected{"选中的用户预设已不存在，请重新选择。"};
    }
    cfg = awj::config_from_user_preset(
        state.user_presets[static_cast<std::size_t>(user_index)], format);
  }
  const auto input_path = awj::normalize_path_argument(
      awj::wide_from_utf8(shared_to_string(app.get_input_path())), "输入路径");
  if (!input_path) return std::unexpected{input_path.error()};
  cfg.input_path = *input_path;
  const auto output_text = shared_to_string(app.get_output_dir());
  if (!output_text.empty()) {
    const auto output_path = awj::normalize_path_argument(
        awj::wide_from_utf8(output_text), "输出目录");
    if (!output_path) return std::unexpected{output_path.error()};
    cfg.output_dir = *output_path;
  }
  cfg.output_template =
      awj::wide_from_utf8(shared_to_string(app.get_template_text()));
  if (cfg.output_template.empty()) {
    cfg.output_template = awj::encoding_defaults::default_output_template;
  }
  cfg.allow_wic_fallback = app.get_allow_wic_fallback();
  cfg.output_format = format;
  cfg.append_png_suffix = queue_choice.append_png_suffix;
  cfg.enable_experimental_encoders = app.get_experimental_encoders();
  if (cfg.avif_encoder == awj::AvifEncoderMode::zenrav1e) {
    const auto capabilities = awj::avif_encoder_capabilities_for_current_build(
        cfg.enable_experimental_encoders);
    const auto it =
        std::ranges::find(capabilities, awj::AvifEncoderMode::zenrav1e,
                          &awj::AvifEncoderCapability::mode);
    if (it == capabilities.end() || !it->enabled ||
        (it->experimental && !it->feature_enabled)) {
      return std::unexpected{"zenrav1e 当前构建不可用，无法选择。"};
    }
  }
  cfg.collision_mode = collision_from_index(app.get_collision_index());
  cfg.visual_quality_gpu = app.get_visual_quality_gpu();
  cfg.visual_quality_fallback = app.get_visual_quality_fallback();
  cfg.unlock_max_input_file_bytes = app.get_unlock_max_input_file_bytes();
  awj::encoding_defaults::unlock_max_input_file_bytes.store(cfg.unlock_max_input_file_bytes, std::memory_order_relaxed);
  cfg.strip_metadata = app.get_strip_metadata();
  cfg.write_summary = app.get_write_summary();
  cfg.write_log = app.get_write_log();
  cfg.preserve_creation_time = app.get_preserve_creation_time();
  cfg.preserve_modification_time = app.get_preserve_modification_time();
  cfg.preserve_access_time = app.get_preserve_access_time();

  if (auto valid = awj::finalize_config_defaults(cfg, true, true); !valid) {
    return std::unexpected{valid.error()};
  }
  return cfg;
} catch (const std::bad_alloc&) {
  return std::unexpected{"Studio 配置解析内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"Studio 配置解析数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"Studio 配置解析文件系统访问失败。"};
}

std::filesystem::path effective_output_dir(const AwjStudio& app) {
  auto output = std::filesystem::path{
      awj::wide_from_utf8(shared_to_string(app.get_output_dir()))};
  if (!output.empty()) {
    return output;
  }
  const auto input = std::filesystem::path{
      awj::wide_from_utf8(shared_to_string(app.get_input_path()))};
  return awj::default_output_dir_for(input);
}

std::expected<void, std::string> open_path(std::filesystem::path path,
                                           bool create_if_missing) try {
  if (path.empty()) {
    return std::unexpected{"路径为空。"};
  }

  std::error_code ec;
  const bool regular_file = std::filesystem::is_regular_file(path, ec);
  if (ec) {
    return std::unexpected{std::format("无法判断路径类型: {}；系统错误：{}。",
                                       awj::display_path_for_user(path),
                                       ec.message())};
  }
  if (regular_file) {
    path = path.parent_path();
  }
  if (path.empty()) {
    auto current = std::filesystem::current_path(ec);
    if (ec) {
      return std::unexpected{
          std::format("无法定位当前目录；系统错误：{}。", ec.message())};
    }
    path = std::move(current);
  }
  if (create_if_missing) {
    std::filesystem::create_directories(path, ec);
    if (ec) {
      return std::unexpected{std::format("无法创建目录: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
  }

  bool exists = std::filesystem::exists(path, ec);
  if (ec) {
    return std::unexpected{std::format("无法检查路径: {}；系统错误：{}。",
                                       awj::display_path_for_user(path),
                                       ec.message())};
  }
  if (!exists) {
    const auto parent = path.parent_path();
    if (create_if_missing || parent.empty() || parent == path) {
      return std::unexpected{
          std::format("路径不存在: {}。", awj::display_path_for_user(path))};
    }
    path = parent;
    exists = std::filesystem::exists(path, ec);
    if (ec) {
      return std::unexpected{std::format("无法检查父目录: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
    if (!exists) {
      return std::unexpected{
          std::format("父目录不存在: {}。", awj::display_path_for_user(path))};
    }
  }
  const bool directory = std::filesystem::is_directory(path, ec);
  if (ec) {
    return std::unexpected{std::format("无法判断目录类型: {}；系统错误：{}。",
                                       awj::display_path_for_user(path),
                                       ec.message())};
  }
  if (!directory) {
    return std::unexpected{
        std::format("路径不是目录: {}。", awj::display_path_for_user(path))};
  }

  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    return std::unexpected{
        std::format("无法打开目录: {}；ShellExecuteW 返回码：{}。",
                    awj::display_path_for_user(path), result)};
  }
  return {};
} catch (const std::bad_alloc&) {
  return std::unexpected{"打开路径内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"打开路径数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"打开路径文件系统访问失败。"};
}

std::expected<void, std::string> open_file_with_default_app(
    const std::filesystem::path& path) try {
  if (path.empty()) {
    return std::unexpected{"路径为空。"};
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::unexpected{std::format("图片文件不存在: {}。",
                                       awj::display_path_for_user(path))};
  }
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    return std::unexpected{
        std::format("无法打开图片: {}；ShellExecuteW 返回码：{}。",
                    awj::display_path_for_user(path), result)};
  }
  return {};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"打开图片文件系统访问失败。"};
}

std::expected<void, std::string> copy_text_to_clipboard(
    std::wstring_view text) {
  if (text.empty()) {
    return std::unexpected{"复制内容为空。"};
  }
  if (!OpenClipboard(nullptr)) {
    return std::unexpected{std::format("打开剪贴板失败: {}。",
                                       awj::win32_error_message(GetLastError()))};
  }
  struct ClipboardGuard {
    ~ClipboardGuard() { CloseClipboard(); }
  } guard;
  if (!EmptyClipboard()) {
    return std::unexpected{std::format("清空剪贴板失败: {}。",
                                       awj::win32_error_message(GetLastError()))};
  }
  const auto bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    return std::unexpected{"分配剪贴板内存失败。"};
  }
  void* locked = GlobalLock(memory);
  if (locked == nullptr) {
    GlobalFree(memory);
    return std::unexpected{"锁定剪贴板内存失败。"};
  }
  std::memcpy(locked, text.data(), text.size() * sizeof(wchar_t));
  static_cast<wchar_t*>(locked)[text.size()] = L'\0';
  GlobalUnlock(memory);
  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    return std::unexpected{std::format("写入剪贴板失败: {}。",
                                       awj::win32_error_message(GetLastError()))};
  }
  return {};
}

bool output_template_contains(std::wstring_view text,
                              std::wstring_view token) {
  return text.find(token) != std::wstring_view::npos;
}

std::expected<std::vector<awj::ImageFile>, std::string> build_run_files(
    const awj::AppConfig& cfg, const std::vector<QueueImageItem>& queue,
    bool failed_only) {
  try {
    std::vector<awj::ImageFile> files;
    files.reserve(std::ranges::count_if(
        queue, [failed_only](const QueueImageItem& item) {
          return queue_item_selected_for_run(item, failed_only);
        }));
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    const bool needs_hash =
        output_template_contains(cfg.output_template, L"{hash}") ||
        output_template_contains(cfg.output_template, L"{hash8}");
    const bool needs_sha256 =
        output_template_contains(cfg.output_template, L"{sha256}") ||
        output_template_contains(cfg.output_template, L"{sha2568}") ||
        output_template_contains(cfg.output_template, L"{sha256_8}");
    for (const auto& item : queue) {
      if (!queue_item_selected_for_run(item, failed_only)) {
        continue;
      }
      std::wstring hash;
      std::wstring sha256;
      if (needs_hash) {
        if (auto ok = awj::file_hash_token(item.path, hash); !ok) {
          return std::unexpected{ok.error()};
        }
      }
      if (needs_sha256) {
        if (auto ok = awj::file_sha256_token(item.path, sha256); !ok) {
          return std::unexpected{ok.error()};
        }
      }
      files.push_back(awj::make_image_file(files.size(), item.path,
                                           item.relative_dir,
                                           item.bytes, rng,
                                           std::move(hash),
                                           std::move(sha256)));
    }
    if (auto disambiguated = awj::apply_source_extension_disambiguation(cfg, files);
        !disambiguated) {
      return std::unexpected{disambiguated.error()};
    }
    if (auto resolved = awj::resolve_batch_output_paths(cfg, files); !resolved) {
      return std::unexpected{resolved.error()};
    }
    return files;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"构建队列运行快照时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"构建队列运行快照时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"构建队列运行快照时文件系统访问失败。"};
  }
}

std::expected<std::filesystem::path, std::string>
create_studio_queue_manifest(std::uint64_t run_id,
                             std::span<const awj::ImageFile> files) {
  try {
    std::error_code ec;
    const auto temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
      return std::unexpected{std::format(
          "无法获取 Studio worker 临时目录：{}", ec.message())};
    }
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    for (int attempt = 0; attempt < 16; ++attempt) {
      const auto path = temp_dir / std::format(
                                       L"AWJStudioQueue-{}-{}-{:016x}.awjq",
                                       GetCurrentProcessId(), run_id, rng());
      if (std::filesystem::exists(path, ec)) {
        ec.clear();
        continue;
      }
      auto written = awj::write_studio_queue_manifest(path, files);
      if (written) {
        return path;
      }
      std::filesystem::remove(path, ec);
      return std::unexpected{written.error()};
    }
    return std::unexpected{"无法创建唯一的 Studio 队列 manifest。"};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"创建 Studio 队列 manifest 时内存不足。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"创建 Studio 队列 manifest 时文件系统访问失败。"};
  }
}

struct StudioWorkerItemEvent {
  std::size_t index{};
  char status{};
  std::size_t completed{};
  std::size_t total{};
};

struct StudioWorkerDetailEvent {
  std::size_t index{};
  std::string encoder_id{};
  int encoder_threads{};
  std::int64_t decode_microseconds{-1};
  std::int64_t prepare_microseconds{-1};
  std::int64_t encode_microseconds{-1};
  std::int64_t write_microseconds{-1};
};

std::optional<StudioWorkerItemEvent> parse_studio_worker_item_event(
    std::string_view line) {
  constexpr std::string_view prefix = "@AWJ-STUDIO/1 ITEM ";
  if (!line.starts_with(prefix)) {
    return std::nullopt;
  }
  line.remove_prefix(prefix.size());
  std::array<std::string_view, 4> fields;
  for (auto& field : fields) {
    const auto separator = line.find(' ');
    if (separator == std::string_view::npos) {
      field = line;
      line = {};
    } else {
      field = line.substr(0, separator);
      line.remove_prefix(separator + 1);
    }
    if (field.empty()) {
      return std::nullopt;
    }
  }
  if (!line.empty() || fields[1].size() != 1 ||
      (fields[1][0] != 'R' && fields[1][0] != 'D' &&
       fields[1][0] != 'S' &&
       fields[1][0] != 'C' && fields[1][0] != 'F')) {
    return std::nullopt;
  }
  const auto parse_size = [](std::string_view field)
      -> std::optional<std::size_t> {
    const auto value = scn::scan_int<std::size_t>(field);
    if (!value || value->begin() != value->end()) {
      return std::nullopt;
    }
    return value->value();
  };
  const auto index = parse_size(fields[0]);
  const auto completed = parse_size(fields[2]);
  const auto total = parse_size(fields[3]);
  if (!index || !completed || !total || *total == 0 ||
      *index >= *total || *completed > *total) {
    return std::nullopt;
  }
  return StudioWorkerItemEvent{.index = *index,
                               .status = fields[1][0],
                               .completed = *completed,
                               .total = *total};
}

std::optional<StudioWorkerDetailEvent> parse_studio_worker_detail_event(
    std::string_view line) {
  constexpr std::string_view prefix = "@AWJ-STUDIO/1 DETAIL ";
  if (!line.starts_with(prefix)) {
    return std::nullopt;
  }
  line.remove_prefix(prefix.size());
  std::array<std::string_view, 7> fields;
  for (auto& field : fields) {
    const auto separator = line.find(' ');
    if (separator == std::string_view::npos) {
      field = line;
      line = {};
    } else {
      field = line.substr(0, separator);
      line.remove_prefix(separator + 1);
    }
    if (field.empty()) {
      return std::nullopt;
    }
  }
  if (!line.empty()) {
    return std::nullopt;
  }
  const auto index = scn::scan_int<std::size_t>(fields[0]);
  const auto threads = scn::scan_int<int>(fields[2]);
  const auto decode = scn::scan_int<std::int64_t>(fields[3]);
  const auto prepare = scn::scan_int<std::int64_t>(fields[4]);
  const auto encode = scn::scan_int<std::int64_t>(fields[5]);
  const auto write = scn::scan_int<std::int64_t>(fields[6]);
  if (!index || index->begin() != index->end() || !threads ||
      threads->begin() != threads->end() || !decode ||
      decode->begin() != decode->end() || !prepare ||
      prepare->begin() != prepare->end() || !encode ||
      encode->begin() != encode->end() || !write ||
      write->begin() != write->end() || threads->value() < 0 ||
      decode->value() < -1 || prepare->value() < -1 ||
      encode->value() < -1 || write->value() < -1) {
    return std::nullopt;
  }
  return StudioWorkerDetailEvent{
      .index = index->value(),
      .encoder_id = fields[1] == "-" ? std::string{} : std::string{fields[1]},
      .encoder_threads = threads->value(),
      .decode_microseconds = decode->value(),
      .prepare_microseconds = prepare->value(),
      .encode_microseconds = encode->value(),
      .write_microseconds = write->value()};
}

void begin_queue_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                 const std::shared_ptr<UiState>& state,
                                 awj::AppConfig cfg,
                                 bool failed_only = false) {
  auto app = weak.lock();
  if (!app) {
    return;
  }

  std::vector<QueueImageItem> queue_snapshot;
  {
    std::scoped_lock lock{state->mutex};
    if (state->worker_active) {
      (*app)->set_status_text(to_shared("当前任务正在运行，请先停止任务或强制终止"));
      return;
    }
    if (state->queue_items.empty()) {
      (*app)->set_status_text(to_shared("队列为空，请先输入或选择图片。"));
      return;
    }
    queue_snapshot = state->queue_items;
  }

  auto files = build_run_files(cfg, queue_snapshot, failed_only);
  if (!files) {
    (*app)->set_status_text(
        to_shared(std::format("队列准备失败：{}", files.error())));
    return;
  }
  if (files->empty()) {
    (*app)->set_status_text(
        to_shared(failed_only
                      ? "队列中没有失败项。"
                      : "队列中没有待编码图片；请清空队列或添加新图片。"));
    return;
  }

  std::vector<std::filesystem::path> temp_directories;
  try {
    temp_directories.push_back(awj::output_dir_for(cfg));
    for (const auto& file : *files) {
      const auto directory = awj::output_path_for(cfg, file).parent_path();
      if (std::ranges::find(temp_directories, directory) ==
          temp_directories.end()) {
        temp_directories.push_back(directory);
      }
    }
  } catch (...) {
    (*app)->set_status_text(to_shared("队列准备失败：无法记录临时输出目录。"));
    return;
  }

  std::uint64_t run_id{};
  {
    std::scoped_lock lock{state->mutex};
    run_id = ++state->run_id;
    state->worker_active = true;
    state->active_child.reset();
    state->pending_events.clear();
    std::size_t run_index = 0;
    for (auto& item : state->queue_items) {
      item.run_index = std::numeric_limits<std::size_t>::max();
      if (!queue_item_selected_for_run(item, failed_only)) {
        continue;
      }
      item.status = QueueItemStatus::pending;
      item.status_text = "等待编码";
      item.log_text.clear();
      item.encoder_id.clear();
      item.encoder_threads = 0;
      item.decode_seconds = -1.0;
      item.prepare_seconds = -1.0;
      item.encode_seconds = -1.0;
      item.write_seconds = -1.0;
      item.warning = false;
      item.run_index = run_index;
      item.locked_output_path = awj::output_path_for(cfg, (*files)[run_index]);
      ++run_index;
    }
    refresh_queue_rows(**app, *state);
  }
  (*app)->set_running(true);
  (*app)->set_progress(0.0f);
  (*app)->set_status_text(to_shared("正在启动队列 worker…"));

  auto manifest_path = create_studio_queue_manifest(run_id, *files);
  if (!manifest_path) {
    reset_failed_run(**app, *state, run_id,
                     std::format("队列 worker 准备失败：{}",
                                 manifest_path.error()));
    return;
  }
  cfg.studio_queue_manifest = *manifest_path;

  auto child = start_studio_cli_worker(cfg, run_id);
  if (!child) {
    std::error_code ec;
    std::filesystem::remove(*manifest_path, ec);
    reset_failed_run(**app, *state, run_id,
                     std::format("启动队列 worker 失败：{}", child.error()));
    return;
  }
  (*child)->temp_directories = std::move(temp_directories);
  {
    std::scoped_lock lock{state->mutex};
    state->active_child = *child;
  }
  (*app)->set_status_text(to_shared(std::format(
      "队列 worker 已启动，PID {}，共 {} 张图片。", (*child)->process_id,
      files->size())));

  std::optional<std::jthread> monitor;
  try {
    monitor.emplace(guarded_worker(
        weak, state, run_id, "队列 worker 监控",
        [weak, state, run_id, child = *child,
                     file_count = files->size()](std::stop_token token) mutable {
      std::string pending;
      std::optional<StudioWorkerItemEvent> pending_item;

      auto publish_item = [&](StudioWorkerItemEvent event) {
        post_to_ui(weak, [state, run_id, event](AwjStudio& app) {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          if (auto queue_index =
                  queue_index_for_run_index(*state, event.index)) {
            auto& item = state->queue_items[*queue_index];
            item.warning = item.warning || event.status == 'F';
            switch (event.status) {
              case 'R':
                item.status = QueueItemStatus::running;
                item.status_text = "正在转码";
                break;
              case 'D':
                item.status = QueueItemStatus::done;
                item.status_text = "完成";
                break;
              case 'S':
                item.status = QueueItemStatus::skipped;
                item.status_text = "已跳过";
                break;
              case 'C':
                item.status = QueueItemStatus::canceled;
                item.status_text = "已取消";
                break;
              case 'F':
              default:
                item.status = QueueItemStatus::failed;
                item.status_text = "失败";
                break;
            }
          }
          refresh_queue_rows(app, *state);
          app.set_progress(static_cast<float>(event.completed) /
                           static_cast<float>(event.total));
          app.set_status_text(to_shared(
              event.status == 'R'
                  ? std::format("正在转码第 {} 项；已完成 {}/{}。",
                                event.index + 1, event.completed, event.total)
                  : std::format("已完成 {}/{}。", event.completed,
                                event.total)));
        });
      };

      auto publish_line = [&](std::string line) {
        line = trim_copy(std::move(line));
        if (line.empty()) {
          return;
        }
        if (auto event = parse_studio_worker_item_event(line)) {
          if (event->status != 'R') {
            pending_item = *event;
          }
          publish_item(*event);
          return;
        }
        if (auto detail = parse_studio_worker_detail_event(line)) {
          post_to_ui(weak, [state, run_id, detail = std::move(*detail)](
                               AwjStudio& app) {
            std::scoped_lock lock{state->mutex};
            if (state->run_id != run_id) {
              return;
            }
            if (auto queue_index =
                    queue_index_for_run_index(*state, detail.index)) {
              auto& item = state->queue_items[*queue_index];
              const auto seconds = [](std::int64_t value) {
                return value < 0 ? -1.0
                                 : static_cast<double>(value) /
                                       1'000'000.0;
              };
              item.encoder_id = std::move(detail.encoder_id);
              item.encoder_threads = detail.encoder_threads;
              item.decode_seconds = seconds(detail.decode_microseconds);
              item.prepare_seconds = seconds(detail.prepare_microseconds);
              item.encode_seconds = seconds(detail.encode_microseconds);
              item.write_seconds = seconds(detail.write_microseconds);
            }
            refresh_queue_rows(app, *state);
          });
          return;
        }
        if (pending_item) {
          const auto event = *pending_item;
          pending_item.reset();
          post_to_ui(weak, [state, run_id, event,
                             line = std::move(line)](AwjStudio& app) {
            std::scoped_lock lock{state->mutex};
            if (state->run_id != run_id) {
              return;
            }
            if (auto queue_index =
                    queue_index_for_run_index(*state, event.index)) {
              auto& item = state->queue_items[*queue_index];
              item.log_text = line;
              // 跨进程约定：这里嗅探的是 AWJ CLI 子进程 stdout 里的中文子串，
              // 生产方在 pipeline.ixx:461（", 未达标兜底"）。子进程的输出与日志
              // 固定为中文、不跟随界面语言，本判断才成立——1.0.0 的双语只覆盖
              // 界面标签。如果以后要翻译 worker 输出，必须先把这里换成不随语言
              // 变化的机器可读信号（稳定 ASCII 标记，或把
              // visual_quality_target_met 走 awj::BatchProgress 结构化通道传上来），
              // 否则视觉质量未达标的行会静默不再标警告，且不会有编译错误。
              item.warning = item.warning ||
                             line.find("未达标") != std::string::npos;
            }
            refresh_queue_rows(app, *state);
          });
          return;
        }
        post_to_ui(weak, [state, run_id,
                          line = std::move(line)](AwjStudio& app) {
          std::scoped_lock lock{state->mutex};
          if (state->run_id == run_id) {
            app.set_status_text(to_shared(line));
          }
        });
      };

      auto consume_output = [&] {
        if (child->output_read == nullptr) {
          return;
        }
        while (true) {
          DWORD available = 0;
          if (!PeekNamedPipe(child->output_read.get(), nullptr, 0, nullptr,
                             &available, nullptr) ||
              available == 0) {
            break;
          }
          std::array<char, 4096> buffer{};
          DWORD read_bytes = 0;
          if (!ReadFile(child->output_read.get(), buffer.data(),
                        static_cast<DWORD>(
                            std::min<std::size_t>(buffer.size(), available)),
                        &read_bytes, nullptr) ||
              read_bytes == 0) {
            break;
          }
          pending.append(buffer.data(), buffer.data() + read_bytes);
          std::size_t pos = 0;
          while ((pos = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
            pending.erase(0, pos + 1);
            publish_line(std::move(line));
          }
        }
      };

      DWORD exit_code = 1;
      while (!token.stop_requested()) {
        consume_output();
        const DWORD wait = WaitForSingleObject(child->process.get(), 80);
        if (wait == WAIT_OBJECT_0) {
          break;
        }
        if (wait != WAIT_TIMEOUT) {
          break;
        }
      }
      if (token.stop_requested() && child->process != nullptr) {
        child->terminate();
      }
      WaitForSingleObject(child->process.get(), INFINITE);
      consume_output();
      if (!pending.empty()) {
        publish_line(std::move(pending));
      }
      GetExitCodeProcess(child->process.get(), &exit_code);
      const bool forced = child->was_force_terminated();
      const bool canceled =
          child->cancel_requested.load(std::memory_order_acquire) &&
          !forced &&
          exit_code == awj::studio_defaults::worker_force_stop_exit_code;
      cleanup_studio_queue_manifest(child);
      if (forced) {
        cleanup_forced_worker_temp_files(child);
      }

      post_to_ui(weak, [state, run_id, exit_code, forced, canceled,
                        file_count](AwjStudio& app) {
        std::size_t ok_count = 0;
        std::size_t failed_count = 0;
        std::size_t canceled_count = 0;
        {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          state->pending_events.clear();
          state->worker_active = false;
          state->active_child.reset();
          for (auto& item : state->queue_items) {
            if (item.run_index == std::numeric_limits<std::size_t>::max()) {
              continue;
            }
            if (item.status == QueueItemStatus::pending ||
                item.status == QueueItemStatus::running) {
              if (forced || canceled) {
                item.status = QueueItemStatus::canceled;
                item.status_text =
                    forced ? "已强制终止" : "已取消";
                item.warning = false;
              } else {
                item.status = QueueItemStatus::failed;
                item.status_text = "未处理";
                item.warning = true;
              }
            }
            if (item.status == QueueItemStatus::done ||
                item.status == QueueItemStatus::skipped) {
              ++ok_count;
            } else if (item.status == QueueItemStatus::canceled) {
              ++canceled_count;
            } else if (item.status == QueueItemStatus::failed) {
              ++failed_count;
            }
          }
          refresh_queue_rows(app, *state);
        }
        app.set_running(false);
        app.set_progress(exit_code == 0 ? 1.0f : 0.0f);
        if (forced) {
          app.set_status_text(
              to_shared("编码已强制终止，Studio 仍可继续使用"));
        } else if (canceled) {
          app.set_status_text(to_shared(std::format(
              "已取消：成功 {}，失败 {}，取消 {}。", ok_count,
              failed_count, canceled_count)));
        } else if (exit_code == 0) {
          app.set_status_text(to_shared(std::format(
              "完成：成功 {}，失败 {}，取消 {}，共 {}。", ok_count,
              failed_count, canceled_count, file_count)));
        } else {
          app.set_status_text(to_shared(std::format(
              "队列 worker 失败，退出码 {}：成功 {}，失败 {}。",
              exit_code, ok_count, failed_count)));
        }
      });
    }));
  } catch (...) {
    (*child)->terminate();
    WaitForSingleObject((*child)->process.get(), INFINITE);
    cleanup_studio_queue_manifest(*child);
    cleanup_forced_worker_temp_files(*child);
    {
      std::scoped_lock lock{state->mutex};
      if (state->run_id == run_id) {
        state->active_child.reset();
      }
    }
    reset_failed_run(**app, *state, run_id, "队列 worker 监控启动失败。");
    return;
  }

  {
    std::scoped_lock lock{state->mutex};
    state->worker = std::move(*monitor);
  }
}
void handle_queue_menu_action(AwjStudio& app,
                              const std::shared_ptr<UiState>& state,
                              int index, std::string action) {
  std::optional<std::filesystem::path> path_to_open;
  std::optional<std::filesystem::path> image_to_open;
  std::optional<std::wstring> text_to_copy;
  std::string status;
  {
    std::scoped_lock lock{state->mutex};
    if (index < 0 ||
        static_cast<std::size_t>(index) >= state->queue_items.size()) {
      app.set_status_text(to_shared("队列项不存在。"));
      return;
    }
    auto& item = state->queue_items[static_cast<std::size_t>(index)];
    if (action == "open-folder") {
      path_to_open = item.path.parent_path();
    } else if (action == "open-image") {
      image_to_open = item.path;
    } else if (action == "copy-path") {
      text_to_copy = item.path.native();
    } else if (action == "remove") {
      if (state->worker_active) {
        app.set_status_text(to_shared("运行中不能移除队列项。"));
        return;
      }
      state->queue_items.erase(state->queue_items.begin() + index);
      app.set_selected_queue_index(-1);
      status = "已从队列移除。";
    } else if (state->worker_active) {
      app.set_status_text(to_shared("运行中不能调整队列顺序。"));
      return;
    } else if (!queue_item_editable(item)) {
      app.set_status_text(to_shared("该图片已开始编码，不能重新排序。"));
      return;
    } else if (action == "priority") {
      const auto target = first_pending_index(*state);
      if (target < state->queue_items.size()) {
        move_queue_item(*state, static_cast<std::size_t>(index), target);
      }
      status = "已移到未编码队列最前。";
    } else if (action == "last") {
      const auto target = last_pending_index(*state);
      if (target < state->queue_items.size()) {
        move_queue_item(*state, static_cast<std::size_t>(index), target);
      }
      status = "已移到未编码队列最后。";
    } else if (action == "move-up") {
      if (index > 0) {
        move_queue_item(*state, static_cast<std::size_t>(index),
                        static_cast<std::size_t>(index - 1));
      }
      status = "已上移。";
    } else if (action == "move-down") {
      move_queue_item(*state, static_cast<std::size_t>(index),
                      static_cast<std::size_t>(index + 1));
      status = "已下移。";
    }
    refresh_queue_rows(app, *state);
  }

  if (path_to_open) {
    if (auto opened = open_path(*path_to_open, false); !opened) {
      app.set_status_text(
          to_shared(std::format("打开所在位置失败：{}", opened.error())));
    }
    return;
  }
  if (image_to_open) {
    if (auto opened = open_file_with_default_app(*image_to_open); !opened) {
      app.set_status_text(
          to_shared(std::format("打开图片失败：{}", opened.error())));
    }
    return;
  }
  if (text_to_copy) {
    if (auto copied = copy_text_to_clipboard(*text_to_copy); !copied) {
      app.set_status_text(
          to_shared(std::format("复制路径失败：{}", copied.error())));
    } else {
      app.set_status_text(to_shared("已复制完整路径。"));
    }
    return;
  }
  if (!status.empty()) {
    app.set_status_text(to_shared(status));
  }
}


struct QueueDragPayload {
  std::uint64_t id{};
};

slint::DataTransfer make_queue_drag_data(
    const std::shared_ptr<UiState>& state, int index) {
  slint::DataTransfer transfer;
  std::scoped_lock lock{state->mutex};
  if (index < 0 ||
      static_cast<std::size_t>(index) >= state->queue_items.size()) {
    return transfer;
  }
  const auto& item = state->queue_items[static_cast<std::size_t>(index)];
  if (!state->worker_active && queue_item_editable(item)) {
    transfer.set_user_data(QueueDragPayload{.id = item.id});
  }
  return transfer;
}

std::optional<std::size_t> queue_drop_target_index(
    const UiState& state, std::size_t current, int target_slot) noexcept {
  if (target_slot < 0) {
    return std::nullopt;
  }
  auto target = static_cast<std::size_t>(target_slot);
  if (target > state.queue_items.size()) {
    return std::nullopt;
  }
  if (target > current) {
    --target;
  }
  if (target >= state.queue_items.size() || target == current ||
      !queue_item_editable(state.queue_items[target])) {
    return std::nullopt;
  }
  return target;
}

slint::language::DragAction handle_queue_drag_can_drop(
    const std::shared_ptr<UiState>& state, slint::language::DropEvent event,
    int target_slot) {
  const auto user_data = event.data.user_data();
  const auto* payload = std::any_cast<QueueDragPayload>(&user_data);
  // Windows Explorer 外部文件由原生 OLE IDropTarget 处理；这里仅处理 AWJ 队列内部
  // DataTransfer user_data，避免再次依赖 Slint 的 plain_text 路径序列化。
  if (payload == nullptr) return slint::language::DragAction::None;
  std::scoped_lock lock{state->mutex};
  if (state->worker_active) {
    return slint::language::DragAction::None;
  }
  const auto current = queue_index_for_id(*state, payload->id);
  if (!current || !queue_item_editable(state->queue_items[*current]) ||
      !queue_drop_target_index(*state, *current, target_slot)) {
    return slint::language::DragAction::None;
  }
  return slint::language::DragAction::Move;
}

slint::language::DragAction handle_queue_drag_dropped(
    AwjStudio& app, const std::shared_ptr<UiState>& state,
    slint::language::DropEvent event, int target_slot) {
  bool refresh = false;
  {
    const auto user_data = event.data.user_data();
    const auto* payload = std::any_cast<QueueDragPayload>(&user_data);
    if (payload == nullptr) return slint::language::DragAction::None;
    std::scoped_lock lock{state->mutex};
    if (state->worker_active) {
      return slint::language::DragAction::None;
    }
    const auto current = queue_index_for_id(*state, payload->id);
    if (!current || !queue_item_editable(state->queue_items[*current])) {
      return slint::language::DragAction::None;
    }
    const auto target = queue_drop_target_index(*state, *current, target_slot);
    if (!target) {
      return slint::language::DragAction::None;
    }
    refresh = move_queue_item(*state, *current, *target);
    state->drag_reordered = true;
    if (refresh) {
      refresh_queue_rows(app, *state);
    }
  }
  if (refresh) {
    app.set_status_text(to_shared("已调整未编码队列顺序。"));
  }
  return slint::language::DragAction::Move;
}

void handle_queue_pointer_event(AwjStudio& app,
                                const std::shared_ptr<UiState>& state,
                                int index, int button, int kind,
                                float /*local_y*/) {
  if (button != 0) {
    return;
  }
  std::optional<std::filesystem::path> double_click_folder;
  {
    std::scoped_lock lock{state->mutex};
    if (index < 0 ||
        static_cast<std::size_t>(index) >= state->queue_items.size()) {
      return;
    }
    auto& item = state->queue_items[static_cast<std::size_t>(index)];
    const auto now = std::chrono::steady_clock::now();
    if (kind == 0) {
      state->drag_reordered = false;
      return;
    }
    if (kind == 1) {
      const bool was_drag = state->drag_reordered;
      state->drag_reordered = false;
      if (!was_drag) {
        if (state->last_click_id == item.id &&
            now - state->last_click_time <=
                awj::studio_defaults::queue_double_click_delay) {
          double_click_folder = item.path.parent_path();
          state->last_click_id = 0;
        } else {
          state->last_click_id = item.id;
          state->last_click_time = now;
        }
      }
    }
  }
  if (double_click_folder) {
    if (auto opened = open_path(*double_click_folder, false); !opened) {
      app.set_status_text(
          to_shared(std::format("打开所在位置失败：{}", opened.error())));
    }
  }
}

void begin_child_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                const std::shared_ptr<UiState>& state,
                                awj::AppConfig cfg,
                                std::optional<int> large_index = std::nullopt);

void begin_child_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                const std::shared_ptr<UiState>& state,
                                awj::AppConfig cfg,
                                std::optional<int> large_index) {
  auto app = weak.lock();
  if (!app) {
    return;
  }

  std::uint64_t run_id{};
  std::shared_ptr<slint::VectorModel<TaskRow>> rows;
  std::shared_ptr<slint::VectorModel<LargeImageRow>> large_rows;
  {
    std::scoped_lock lock{state->mutex};
    if (state->worker_active) {
      (*app)->set_status_text(to_shared("当前任务正在运行，请先停止任务或强制终止"));
      return;
    }
    run_id = ++state->run_id;
    state->pending_events.clear();
    state->worker_active = true;
    state->active_child.reset();
    if (!large_index) {
      state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
      state->large_image_rows = std::make_shared<slint::VectorModel<LargeImageRow>>();
      state->large_image_items.clear();
      rows = state->task_rows;
      large_rows = state->large_image_rows;
    } else {
      set_large_image_status(*state, *large_index, "正在编码…");
      rows = state->task_rows;
      large_rows = state->large_image_rows;
    }
  }

  try {
    (*app)->set_running(true);
    (*app)->set_progress(0.0f);
    if (!large_index) {
      (*app)->set_task_rows(rows);
      (*app)->set_large_image_rows(large_rows);
      (*app)->set_selected_large_image_index(-1);
    }
    (*app)->set_status_text(to_shared("正在启动编码 worker…"));
  } catch (...) {
    reset_failed_run(**app, *state, run_id, "转换启动失败。");
    return;
  }

  auto child = start_studio_cli_worker(cfg, run_id);
  if (!child) {
    reset_failed_run(**app, *state, run_id,
                     std::format("启动编码 worker 失败：{}", child.error()));
    if (large_index) {
      set_large_image_status(*state, *large_index, "启动失败");
    }
    return;
  }
  try {
    (*app)->set_status_text(
        to_shared(std::format("编码 worker 已启动，PID {}", (*child)->process_id)));
  } catch (...) {
  }

  std::optional<std::jthread> worker;
  try {
    {
      std::scoped_lock lock{state->mutex};
      state->active_child = *child;
    }
    worker.emplace(guarded_worker(
        weak, state, run_id, "编码 worker 监控",
        [weak, state, run_id, child = *child,
                    large_index](std::stop_token token) mutable {
      std::string pending;
      auto publish_line = [&](std::string line) {
        line = trim_copy(std::move(line));
        if (line.empty()) {
          return;
        }
        post_to_ui(weak, [state, run_id, line = std::move(line)](AwjStudio&) {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          append_log_row(state->task_rows, line);
        });
      };
      auto consume_output = [&] {
        if (child->output_read == nullptr) {
          return;
        }
        while (true) {
          DWORD available = 0;
          if (!PeekNamedPipe(child->output_read.get(), nullptr, 0, nullptr,
                             &available, nullptr) ||
              available == 0) {
            break;
          }
          std::array<char, 4096> buffer{};
          DWORD read = 0;
          if (!ReadFile(child->output_read.get(), buffer.data(),
                        static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available)),
                        &read, nullptr) ||
              read == 0) {
            break;
          }
          pending.append(buffer.data(), buffer.data() + read);
          std::size_t pos = 0;
          while ((pos = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
            pending.erase(0, pos + 1);
            publish_line(std::move(line));
          }
        }
      };

      DWORD exit_code = 1;
      while (!token.stop_requested()) {
        consume_output();
        const DWORD wait = WaitForSingleObject(child->process.get(), 80);
        if (wait == WAIT_OBJECT_0) {
          break;
        }
        if (wait != WAIT_TIMEOUT) {
          break;
        }
      }
      if (token.stop_requested() && child->process != nullptr) {
        child->terminate();
      }
      WaitForSingleObject(child->process.get(), INFINITE);
      consume_output();
      if (!pending.empty()) {
        publish_line(std::move(pending));
      }
      GetExitCodeProcess(child->process.get(), &exit_code);
      const bool forced = child->was_force_terminated();
      const bool canceled =
          child->cancel_requested.load(std::memory_order_acquire) && !forced &&
          exit_code == awj::studio_defaults::worker_force_stop_exit_code;
      if (forced) {
        cleanup_forced_worker_temp_files(child);
      }
      post_to_ui(weak, [state, run_id, large_index, exit_code, forced,
                        canceled](AwjStudio& app) {
              {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          state->update_timer.stop();
          state->pending_events.clear();
          state->worker_active = false;
          state->active_child.reset();
                if (large_index) {
            set_large_image_status(
                *state, *large_index,
                exit_code == 0 ? "完成" : (forced ? "已强制终止"
                                                   : (canceled ? "已取消" : "失败")));
          }
              }
              try {
          app.set_running(false);
          app.set_progress(exit_code == 0 ? 1.0f : 0.0f);
          if (exit_code == 0) {
            app.set_status_text(to_shared(large_index ? "大图处理完成" : "完成"));
          } else if (forced) {
            app.set_status_text(to_shared("编码已强制终止，Studio 仍可继续使用"));
          } else if (canceled) {
            app.set_status_text(to_shared("已取消"));
          } else {
            app.set_status_text(
                to_shared(std::format("编码 worker 失败，退出码 {}", exit_code)));
          }
        } catch (...) {
        }
      });
    }));
  } catch (const std::exception&) {
    (*child)->terminate();
    WaitForSingleObject((*child)->process.get(), INFINITE);
    cleanup_forced_worker_temp_files(*child);
    {
      std::scoped_lock lock{state->mutex};
      if (state->run_id == run_id) {
        state->active_child.reset();
      }
    }
    reset_failed_run(**app, *state, run_id, "转换启动失败。");
    return;
  } catch (...) {
    (*child)->terminate();
    WaitForSingleObject((*child)->process.get(), INFINITE);
    cleanup_forced_worker_temp_files(*child);
    {
      std::scoped_lock lock{state->mutex};
      if (state->run_id == run_id) {
        state->active_child.reset();
      }
    }
    reset_failed_run(**app, *state, run_id, "转换启动失败。");
    return;
  }
  {
    std::scoped_lock lock{state->mutex};
    state->worker = std::move(*worker);
  }
}

struct UpdatePersistentState {
  std::string channel{};
  bool show_changelog{};
  bool hide_changelog_after_exit{};
  bool show_changelog_after_update{};
  std::string last_changelog_exit_version{};
  std::int64_t last_successful_check{};
  std::int64_t last_verified_sequence{};
  std::int64_t last_verified_v2_sequence{};
  std::string version{};
  std::string pending_channel{};
  std::string release_url{};
  std::string published_at{};
  std::string changelog_zh_cn{};
  std::string changelog_en{};
  std::string manifest_raw{};
  std::string manifest_signature{};
  std::string manifest_v2_raw{};
  std::string manifest_v2_signature{};
  std::string keyring_raw{};
  std::string keyring_signature{};
};

UpdatePersistentState capture_update_state(const UiState& state) {
  return {.channel = state.update_channel,
          .show_changelog = state.show_update_changelog,
          .hide_changelog_after_exit = state.hide_update_changelog_after_exit,
          .show_changelog_after_update = state.show_update_changelog_after_update,
          .last_changelog_exit_version = state.last_changelog_exit_version,
          .last_successful_check = state.last_successful_update_check_at,
          .last_verified_sequence = state.last_verified_manifest_sequence,
          .last_verified_v2_sequence = state.last_verified_manifest_v2_sequence,
          .version = state.pending_update_version,
          .pending_channel = state.pending_update_channel,
          .release_url = state.pending_update_release_url,
          .published_at = state.pending_update_published_at,
          .changelog_zh_cn = state.pending_update_changelog_zh_cn,
          .changelog_en = state.pending_update_changelog_en,
          .manifest_raw = state.update_manifest_raw,
          .manifest_signature = state.update_manifest_signature,
          .manifest_v2_raw = state.update_manifest_v2_raw,
          .manifest_v2_signature = state.update_manifest_v2_signature,
          .keyring_raw = state.update_keyring_raw,
          .keyring_signature = state.update_keyring_signature};
}

void restore_update_state(UiState& state, UpdatePersistentState value) {
  state.update_channel = std::move(value.channel);
  state.show_update_changelog = value.show_changelog;
  state.hide_update_changelog_after_exit = value.hide_changelog_after_exit;
  state.show_update_changelog_after_update = value.show_changelog_after_update;
  state.last_changelog_exit_version = std::move(value.last_changelog_exit_version);
  state.last_successful_update_check_at = value.last_successful_check;
  state.last_verified_manifest_sequence = value.last_verified_sequence;
  state.last_verified_manifest_v2_sequence = value.last_verified_v2_sequence;
  state.pending_update_version = std::move(value.version);
  state.pending_update_channel = std::move(value.pending_channel);
  state.pending_update_release_url = std::move(value.release_url);
  state.pending_update_published_at = std::move(value.published_at);
  state.pending_update_changelog_zh_cn = std::move(value.changelog_zh_cn);
  state.pending_update_changelog_en = std::move(value.changelog_en);
  state.update_manifest_raw = std::move(value.manifest_raw);
  state.update_manifest_signature = std::move(value.manifest_signature);
  state.update_manifest_v2_raw = std::move(value.manifest_v2_raw);
  state.update_manifest_v2_signature = std::move(value.manifest_v2_signature);
  state.update_keyring_raw = std::move(value.keyring_raw);
  state.update_keyring_signature = std::move(value.keyring_signature);
}

void clear_pending_update(UiState& state) {
  state.pending_update_version.clear();
  state.pending_update_channel.clear();
  state.pending_update_release_url.clear();
  state.pending_update_published_at.clear();
  state.pending_update_changelog_zh_cn.clear();
  state.pending_update_changelog_en.clear();
}

std::string update_summary(std::string_view changelog) {
  const auto line_end = changelog.find_first_of("\r\n");
  return std::string{changelog.substr(0, line_end)};
}

std::string format_update_check_time(std::int64_t unix_seconds) {
  if (unix_seconds <= 0) return {};
  const auto point = std::chrono::system_clock::time_point{
      std::chrono::seconds{unix_seconds}};
  return std::format("{:%Y-%m-%d %H:%M:%S} UTC",
                     std::chrono::floor<std::chrono::seconds>(point));
}

bool pending_update_is_newer(const UiState& state) {
  const auto current = awj::update::parse_version(AWJ_BUILD_VERSION);
  const auto pending = awj::update::parse_version(state.pending_update_version);
  const auto channel = awj::update::parse_channel(state.pending_update_channel);
  const auto preference = state.update_channel == "prerelease"
                              ? awj::update::ChannelPreference::stable_and_prerelease
                              : awj::update::ChannelPreference::stable_only;
  return current && pending && channel && *pending > *current &&
         awj::update::channel_visible_to(*channel, preference);
}

bool changelog_first_start_for_current_version(const UiState& state) {
  return state.last_changelog_exit_version != AWJ_BUILD_VERSION;
}

bool changelog_visible_for_current_session(const UiState& state) {
  const bool first_start = changelog_first_start_for_current_version(state);
  if (!state.show_update_changelog) {
    // 总开关关闭时，升级后的首次启动仍临时显示一次，退出后即恢复隐藏。
    return first_start;
  }
  return !state.hide_update_changelog_after_exit || first_start;
}

bool changelog_should_open_on_start(const UiState& state) {
  return changelog_first_start_for_current_version(state) &&
         (!state.show_update_changelog || state.show_update_changelog_after_update);
}

void sync_update_ui(AwjStudio& app, const UiState& state) {
  const bool english = app.get_language_index() == 1;
  const bool available = pending_update_is_newer(state);
  app.set_current_version(to_shared(AWJ_BUILD_VERSION));
  app.set_update_channel_index(state.update_channel == "prerelease" ? 1 : 0);
  app.set_show_update_changelog_enabled(state.show_update_changelog);
  app.set_show_update_changelog(changelog_visible_for_current_session(state));
  app.set_hide_update_changelog_after_exit(
      state.hide_update_changelog_after_exit);
  app.set_show_update_changelog_after_update(
      state.show_update_changelog_after_update);
  app.set_update_available(available);
  app.set_update_version(to_shared(available ? state.pending_update_version : ""));
  app.set_update_published_at(
      to_shared(available ? state.pending_update_published_at : ""));
  app.set_update_changelog_zh_cn(
      to_shared(available ? state.pending_update_changelog_zh_cn : ""));
  app.set_update_changelog_en(
      to_shared(available ? state.pending_update_changelog_en : ""));
  app.set_update_summary_zh_cn(
      to_shared(available ? update_summary(state.pending_update_changelog_zh_cn)
                          : ""));
  app.set_update_summary_en(
      to_shared(available ? update_summary(state.pending_update_changelog_en)
                          : ""));
  const auto last = format_update_check_time(state.last_successful_update_check_at);
  app.set_update_last_successful_check(
      to_shared(last.empty() ? (english ? "Never" : "从未") : last));
  app.set_update_status(
      to_shared(english ? state.update_status_en : state.update_status_zh));
}

void sync_update_history(
    const std::shared_ptr<slint::VectorModel<UpdateHistoryRow>>& rows,
    const awj::update::Manifest& manifest) {
  if (!rows) return;
  auto merged_history = awj::ui::embedded_changelog_history();
  for (const auto& entry : manifest.entries) {
    const auto version = awj::update::to_string(entry.version);
    const auto signed_entry = awj::ui::ChangelogHistoryEntry{
        .version = version,
        .channel = std::string{awj::update::channel_name(entry.channel)},
        .published_at = entry.published_at,
        .release_url = entry.release_url,
        .changelog_zh_cn = entry.changelog.zh_cn,
        .changelog_en = entry.changelog.en};
    const auto existing = std::ranges::find_if(
        merged_history, [&](const auto& item) { return item.version == version; });
    if (existing == merged_history.end()) {
      merged_history.push_back(signed_entry);
    } else {
      *existing = signed_entry;
    }
  }
  std::ranges::sort(merged_history, [](const auto& lhs, const auto& rhs) {
    const auto left = awj::update::parse_version(lhs.version);
    const auto right = awj::update::parse_version(rhs.version);
    return left && right ? *left > *right : lhs.version > rhs.version;
  });
  std::vector<UpdateHistoryRow> history_rows;
  history_rows.reserve(merged_history.size());
  for (const auto& entry : merged_history) {
    history_rows.push_back(UpdateHistoryRow{
        .version = to_shared(entry.version),
        .channel = to_shared(entry.channel),
        .published_at = to_shared(entry.published_at),
        .release_url = to_shared(entry.release_url),
        .changelog_zh_cn = to_shared(entry.changelog_zh_cn),
        .changelog_en = to_shared(entry.changelog_en)});
  }
  rows->set_vector(std::move(history_rows));
}

void restore_cached_update_history(UiState& state) {
  if (state.update_manifest_v2_raw.empty() ||
      state.update_manifest_v2_signature.empty() ||
      state.update_keyring_raw.empty() || state.update_keyring_signature.empty()) {
    return;
  }
  auto keyring = awj::update::verify_and_parse_update_keyring(
      state.update_keyring_raw, state.update_keyring_signature);
  if (!keyring) {
    state.update_manifest_v2_raw.clear();
    state.update_manifest_v2_signature.clear();
    state.update_keyring_raw.clear();
    state.update_keyring_signature.clear();
    return;
  }
  auto manifest = awj::update::verify_and_parse_archive_manifest_v2(
      state.update_manifest_v2_raw, state.update_manifest_v2_signature, *keyring);
  if (!manifest ||
      manifest->sequence <
          static_cast<std::uint64_t>(std::max<std::int64_t>(
              state.last_verified_manifest_v2_sequence, 0)) ||
      manifest->sequence >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    // 缓存只用于展示；验签失败或序号倒退时丢弃，联网检查仍按原状态继续。
    state.update_manifest_v2_raw.clear();
    state.update_manifest_v2_signature.clear();
    state.update_keyring_raw.clear();
    state.update_keyring_signature.clear();
    return;
  }
  state.last_verified_manifest_v2_sequence =
      std::max(state.last_verified_manifest_v2_sequence,
               static_cast<std::int64_t>(manifest->sequence));
  sync_update_history(state.update_history_rows,
                      awj::update::archive_manifest_v2_for_history(*manifest));
}

awj::update::ChannelPreference update_preference(const UiState& state) {
  return state.update_channel == "prerelease"
             ? awj::update::ChannelPreference::stable_and_prerelease
             : awj::update::ChannelPreference::stable_only;
}

void start_update_check(slint::ComponentWeakHandle<AwjStudio> weak,
                        const std::shared_ptr<UiState>& state) {
  if (state->update_check_active) return;
  if (state->update_worker.joinable()) state->update_worker.join();
  state->update_check_active = true;
  if (auto app = weak.lock()) {
    (*app)->set_update_checking(true);
    state->update_status_zh = "正在检查更新…";
    state->update_status_en = "Checking for updates...";
    sync_update_ui(**app, *state);
  }
  const auto last_sequence = state->last_verified_manifest_v2_sequence < 0
                                 ? std::uint64_t{0}
                                 : static_cast<std::uint64_t>(
                                       state->last_verified_manifest_v2_sequence);
  const auto preference = update_preference(*state);
  state->update_worker = std::jthread(
      [weak, state, last_sequence, preference](std::stop_token token) {
        auto fetched = awj::update::fetch_verified_archive_manifest_v2(
            last_sequence, token);
        post_to_ui(weak, [state, preference,
                          fetched = std::move(fetched)](AwjStudio& app) mutable {
          state->update_check_active = false;
          app.set_update_checking(false);
          if (!fetched) {
            state->update_status_zh =
                std::format("检查失败：{}", fetched.error());
            state->update_status_en = "Update check failed.";
            sync_update_ui(app, *state);
            return;
          }
          if (fetched->manifest.sequence >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
            state->update_status_zh = "检查失败：manifest sequence 超出本机范围。";
            state->update_status_en = "Update check failed: sequence is out of range.";
            sync_update_ui(app, *state);
            return;
          }

          const auto before = capture_update_state(*state);
          state->update_manifest_v2_raw = fetched->raw_bytes;
          state->update_manifest_v2_signature = fetched->signature_base64;
          state->update_keyring_raw = fetched->keyring_raw_bytes;
          state->update_keyring_signature = fetched->keyring_signature_envelope;
          if (const auto pending =
                  awj::update::parse_version(state->pending_update_version);
              pending && awj::update::should_clear_pending_for_revocation(
                             awj::update::archive_manifest_v2_for_history(
                                 fetched->manifest), *pending)) {
            clear_pending_update(*state);
          }
          const auto current = awj::update::parse_version(AWJ_BUILD_VERSION);
          if (!current) {
            restore_update_state(*state, before);
            state->update_status_zh = "检查失败：当前构建版本号非法。";
            state->update_status_en = "Update check failed: invalid build version.";
            sync_update_ui(app, *state);
            return;
          }
          const auto candidate = awj::update::select_archive_candidate_v2(
              fetched->manifest,
              {.current_version = *current,
               .updater_version = *current,
               .preference = preference});
          if (candidate) {
            state->pending_update_version =
                awj::update::to_string(candidate->version);
            state->pending_update_channel =
                std::string{awj::update::channel_name(candidate->channel)};
            state->pending_update_release_url = candidate->release_url;
            state->pending_update_published_at = candidate->published_at;
            state->pending_update_changelog_zh_cn = candidate->changelog.zh_cn;
            state->pending_update_changelog_en = candidate->changelog.en;
          }
          state->last_successful_update_check_at =
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
          state->last_verified_manifest_v2_sequence =
              static_cast<std::int64_t>(fetched->manifest.sequence);
          const bool retained_pending = !candidate && pending_update_is_newer(*state);
          state->update_status_zh =
              candidate ? "发现可用更新。"
                        : retained_pending ? "检查成功；保留之前发现的更新。"
                                           : "已是最新版。";
          state->update_status_en =
              candidate ? "An update is available."
                        : retained_pending
                              ? "Check succeeded; the previously found update remains available."
                              : "Up to date.";

          if (auto saved = persist_studio_config_if_changed(app, *state);
              !saved) {
            restore_update_state(*state, before);
            state->update_status_zh =
                std::format("检查失败：无法持久化状态：{}", saved.error());
            state->update_status_en =
                "Update check failed: state could not be saved.";
          } else {
            sync_update_history(state->update_history_rows,
                                awj::update::archive_manifest_v2_for_history(
                                    fetched->manifest));
          }
          sync_update_ui(app, *state);
        });
      });
}

}  // namespace

/// Probe whether the OpenGL driver exposes the functions FemtoVG needs
/// (glCreateShader, OpenGL 2.0+). If not, force Slint to use the software
/// renderer so the application starts instead of panicking.
void ensure_slint_backend() {
  // Respect an explicit user override.
  if (GetEnvironmentVariableA("SLINT_BACKEND", nullptr, 0) > 0) {
    return;
  }

  HMODULE gl = LoadLibraryW(L"opengl32.dll");
  if (gl == nullptr) {
    SetEnvironmentVariableA("SLINT_BACKEND", "software");
    return;
  }

  // Create a throwaway window so wglGetProcAddress has a current context.
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"AWJGLProbe";
  RegisterClassW(&wc);

  HWND tmp = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 1, 1, nullptr,
                             nullptr, wc.hInstance, nullptr);
  HDC dc = GetDC(tmp);

  PIXELFORMATDESCRIPTOR pfd{};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);

  using WglCreateCtx = HGLRC(__stdcall*)(HDC);
  auto wglCreateContext =
      reinterpret_cast<WglCreateCtx>(GetProcAddress(gl, "wglCreateContext"));
  HGLRC ctx = wglCreateContext != nullptr ? wglCreateContext(dc) : nullptr;

  bool driver_ok = false;
  if (ctx != nullptr) {
    wglMakeCurrent(dc, ctx);

    // glCreateShader lives in the ICD; opengl32.dll itself only forwards the
    // request via wglGetProcAddress, which requires a current context.
    using WglGetProc = void*(__stdcall*)(const char*);
    auto wglGetProcAddress = reinterpret_cast<WglGetProc>(
        GetProcAddress(gl, "wglGetProcAddress"));
    driver_ok =
        wglGetProcAddress != nullptr && wglGetProcAddress("glCreateShader");

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(ctx);
  }

  ReleaseDC(tmp, dc);
  DestroyWindow(tmp);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  FreeLibrary(gl);

  if (!driver_ok) {
    SetEnvironmentVariableA("SLINT_BACKEND", "software");
  }
}

int run_studio_ui(const wchar_t* health_event,
                  const wchar_t* installed_version) {
  try {
    ensure_slint_backend();
    auto app = AwjStudio::create();
    auto state = std::make_shared<UiState>();
    state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
    state->large_image_rows =
        std::make_shared<slint::VectorModel<LargeImageRow>>();
    state->update_history_rows =
        std::make_shared<slint::VectorModel<UpdateHistoryRow>>();
    sync_update_history(state->update_history_rows,
                        awj::update::Manifest{.schema = 1});
    auto weak = slint::ComponentWeakHandle(app);
    start_import_dispatcher(weak, state);

    app->set_task_rows(state->task_rows);
    app->set_large_image_rows(state->large_image_rows);
    app->set_update_history(state->update_history_rows);
    initialize_ui_defaults(*app, *state);
    apply_system_ui_font(*app);
    app->set_threads_text({});
    app->set_system_dark_mode(windows_prefers_dark_mode());
    state->config_defaults = capture_studio_config(*app, state.get());
    std::optional<std::string> config_warning;
    if (auto loaded = apply_studio_config_file(*app, *state); !loaded) {
      config_warning = std::format("读取 Studio 配置失败：{}", loaded.error());
    }
    reload_user_preset_options(*app, *state);
    if (!state->user_preset_errors.empty() && !config_warning) {
      config_warning = std::format("有 {} 个用户预设未加载：{}",
                                   state->user_preset_errors.size(),
                                   state->user_preset_errors.front());
    }
    load_system_font_options(*app);
    sync_template_flags(*app);
    // 配置已经读进 language_index，这里让 @tr() 立刻按存下来的语言重算。
    // 必须在组件创建之后调用，此时 AwjStudio::create() 早已完成。
    apply_ui_language(app->get_language_index());
    restore_cached_update_history(*state);
    bool health_check_ready = false;
    if (health_event != nullptr && installed_version != nullptr) {
      const auto health = awj::update::decide_update_health_handshake(
          awj::utf8_from_wide(installed_version), AWJ_BUILD_VERSION,
          state->pending_update_version);
      health_check_ready = health.signal_ready;
      if (health.clear_matching_pending) {
        clear_pending_update(*state);
        if (auto saved = write_studio_config_file(
                capture_studio_config(*app, state.get()),
                *state->config_defaults);
            !saved) {
          config_warning = std::format(
              "更新已启动，但清除待更新状态失败：{}", saved.error());
        }
      }
    }
    state->last_config_snapshot = capture_studio_config(*app, state.get());
    sync_update_ui(*app, *state);
    if (changelog_should_open_on_start(*state) &&
        changelog_visible_for_current_session(*state)) {
      app->set_selected_page(4);
    }
    if (auto warning = shell_context_menu_warning(state->menu_params)) {
      app->set_context_menu_warning(to_shared(*warning));
    }
    if (config_warning) {
      app->set_status_text(to_shared(*config_warning));
    }

    app->on_language_selection_requested([weak, state](int index) {
      run_ui_callback(weak, "切换界面语言失败", [&] {
        if (auto app = weak.lock()) {
          // 语言不受 running 限制：它只影响界面文字，不改变任何编码参数。
          (*app)->set_language_index(index);
          apply_ui_language(index);
          sync_update_ui(**app, *state);
        }
      });
    });

    app->on_update_channel_selection_requested([weak, state](int index) {
      run_ui_callback(weak, "切换更新渠道失败", [&] {
        auto app = weak.lock();
        if (!app || (index != 0 && index != 1)) return;
        const auto previous = state->update_channel;
        state->update_channel = index == 1 ? "prerelease" : "stable";
        (*app)->set_update_channel_index(index);
        if (auto saved = persist_studio_config_if_changed(**app, *state);
            !saved) {
          state->update_channel = previous;
          sync_update_ui(**app, *state);
          (*app)->set_update_status(to_shared(
              std::format("保存更新渠道失败：{}", saved.error())));
          return;
        }
        start_update_check(weak, state);
      });
    });

    app->on_show_update_changelog_requested([weak, state](bool visible) {
      run_ui_callback(weak, "保存更新日志显示设置失败", [&] {
        auto app = weak.lock();
        if (!app) return;
        const bool previous = state->show_update_changelog;
        state->show_update_changelog = visible;
        sync_update_ui(**app, *state);
        if (!changelog_visible_for_current_session(*state) &&
            (*app)->get_selected_page() == 4) {
          (*app)->set_selected_page(2);
        }
        if (auto saved = persist_studio_config_if_changed(**app, *state);
            !saved) {
          state->show_update_changelog = previous;
          sync_update_ui(**app, *state);
          (*app)->set_update_status(to_shared(
              std::format("保存更新日志设置失败：{}", saved.error())));
        }
      });
    });

    app->on_hide_update_changelog_after_exit_requested(
        [weak, state](bool enabled) {
          run_ui_callback(weak, "保存更新日志退出设置失败", [&] {
            auto app = weak.lock();
            if (!app) return;
            const bool previous = state->hide_update_changelog_after_exit;
            state->hide_update_changelog_after_exit = enabled;
            sync_update_ui(**app, *state);
            if (auto saved = persist_studio_config_if_changed(**app, *state);
                !saved) {
              state->hide_update_changelog_after_exit = previous;
              sync_update_ui(**app, *state);
              (*app)->set_update_status(to_shared(
                  std::format("保存更新日志设置失败：{}", saved.error())));
            }
          });
        });

    app->on_show_update_changelog_after_update_requested(
        [weak, state](bool enabled) {
          run_ui_callback(weak, "保存更新日志更新设置失败", [&] {
            auto app = weak.lock();
            if (!app) return;
            const bool previous = state->show_update_changelog_after_update;
            state->show_update_changelog_after_update = enabled;
            sync_update_ui(**app, *state);
            if (auto saved = persist_studio_config_if_changed(**app, *state);
                !saved) {
              state->show_update_changelog_after_update = previous;
              sync_update_ui(**app, *state);
              (*app)->set_update_status(to_shared(
                  std::format("保存更新日志设置失败：{}", saved.error())));
            }
          });
        });

    app->on_version_clicked([weak, state] {
      run_ui_callback(weak, "处理版本操作失败", [&] {
        auto app = weak.lock();
        if (!app) return;
        const auto open_release = [&](std::string_view url) {
          const auto wide = awj::wide_from_utf8(url);
          return reinterpret_cast<INT_PTR>(ShellExecuteW(
              nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        };
        if (!pending_update_is_newer(*state)) {
          const auto url = std::format(
              "https://github.com/Dominic485649/AWJimage/releases/tag/{}",
              AWJ_BUILD_VERSION);
          if (const auto result = open_release(url); result <= 32) {
            (*app)->set_update_status(to_shared(std::format(
                "打开版本页面失败；ShellExecuteW 返回码 {}。", result)));
          }
          return;
        }
        if (worker_active(state) || (*app)->get_running()) {
          (*app)->set_update_status(
              to_shared("编码任务运行时禁止更新；请先等待任务完成或取消任务。"));
          return;
        }
        if (state->update_check_active) {
          (*app)->set_update_status(to_shared("更新检查或下载正在进行中。"));
          return;
        }
        const auto confirmation = awj::wide_from_utf8(std::format(
            "将下载、验证并安装 AWJ {}。\n\n"
            "程序会在替换前关闭；启动健康检查失败时自动回滚。是否继续？",
            state->pending_update_version));
        if (MessageBoxW(nullptr, confirmation.c_str(), L"AWJ 自动更新",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
          return;
        }
        if (state->update_worker.joinable()) state->update_worker.join();
        const auto requested_version = state->pending_update_version;
        const auto release_url = state->pending_update_release_url;
        const auto preference = update_preference(*state);
        const auto sequence = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, state->last_verified_manifest_v2_sequence));
        state->update_check_active = true;
        state->update_status_zh = "正在重新验签并下载更新…";
        state->update_status_en = "Verifying and downloading the update...";
        (*app)->set_update_checking(true);
        sync_update_ui(**app, *state);
        state->update_worker = std::jthread(
            [weak, state, requested_version, release_url, preference,
             sequence](std::stop_token token) {
              auto staged = awj::update::stage_and_launch_update(
                  requested_version, preference, sequence, token);
              post_to_ui(weak, [state, release_url,
                                staged = std::move(staged)](AwjStudio& app) mutable {
                state->update_check_active = false;
                app.set_update_checking(false);
                if (!staged) {
                  state->update_status_zh =
                      std::format("更新失败：{}", staged.error());
                  state->update_status_en = "The update could not be installed.";
                  sync_update_ui(app, *state);
                  if (staged.error().starts_with("INSTALL_DIR_NOT_WRITABLE:")) {
                    const auto wide = awj::wide_from_utf8(release_url);
                    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr,
                                  nullptr, SW_SHOWNORMAL);
                  }
                  return;
                }
                state->update_status_zh = "更新 helper 已启动，正在关闭当前版本…";
                state->update_status_en =
                    "The update helper is ready; closing this version...";
                sync_update_ui(app, *state);
                app.window().hide();
              });
            });
      });
    });

    app->on_clear_tasks([weak, state] {
      run_ui_callback(weak, "清空任务失败", [&] {
        auto app = weak.lock();
        if (!app) {
          return;
        }
        if (reject_when_worker_active(**app, state,
                                      "当前任务正在运行，无法清空队列")) {
          return;
        }
        state->queue_items.clear();
        refresh_queue_rows(**app, *state);
        state->large_image_rows->set_vector({});
        state->large_image_items.clear();
        (*app)->set_selected_queue_index(-1);
        (*app)->set_selected_large_image_index(-1);
        (*app)->set_progress(0.0f);
        (*app)->set_status_text(to_shared("已清空全部队列文件。"));
      });
    });

    app->on_retry_failed([weak, state] {
      run_ui_callback(weak, "重试失败项失败", [&] {
        auto app = weak.lock();
        if (!app) {
          return;
        }
        if (reject_when_worker_active(**app, state,
                                      "当前任务正在运行，无法重试失败项")) {
          return;
        }
        std::optional<std::filesystem::path> fallback_input;
        {
          std::scoped_lock lock{state->mutex};
          const auto failed = std::ranges::find_if(
              state->queue_items, [](const QueueImageItem& item) {
                return item.status == QueueItemStatus::failed;
              });
          if (failed == state->queue_items.end()) {
            (*app)->set_status_text(to_shared("队列中没有失败项。"));
            return;
          }
          fallback_input = failed->source_root.empty() ? failed->path
                                                       : failed->source_root;
        }
        if (trim_copy(shared_to_string((*app)->get_input_path())).empty()) {
          (*app)->set_input_path(
              to_shared(awj::path_to_utf8(*fallback_input)));
          if (output_dir_is_empty(**app)) {
            (*app)->set_output_dir(to_shared(awj::path_to_utf8(
                awj::default_output_dir_for(*fallback_input))));
          }
        }
        auto cfg = config_from_ui(**app, *state);
        if (!cfg) {
          (*app)->set_status_text(
              to_shared(std::format("配置错误：{}", cfg.error())));
          return;
        }
        begin_queue_conversion_run(weak, state, std::move(*cfg), true);
      });
    });

    app->on_format_defaults_requested([weak, state](int index) {
      run_ui_callback(weak, "应用格式默认值失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(
                  **app, state, "当前任务正在运行，无法修改格式默认值")) {
            return;
          }
          apply_format_defaults_to_ui(**app, index, *state);
        }
      });
    });

    app->on_parameter_preset_selected([weak, state](int index) {
      run_ui_callback(weak, "切换参数预设失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法切换参数预设")) {
            return;
          }
          select_parameter_preset(**app, *state, index);
        }
      });
    });

    app->on_queue_preset_selected([weak, state](int index) {
      run_ui_callback(weak, "切换队列预设失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法切换队列预设")) {
            return;
          }
          select_queue_preset(**app, *state, index);
        }
      });
    });

    app->on_open_preset_editor([weak, state] {
      run_ui_callback(weak, "打开预设编辑器失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法保存参数预设")) {
            return;
          }
          store_current_parameter_params(**app, *state);
          const auto index = state->parameter_preset_index;
          (*app)->set_preset_editor_name(
              index == 0
                  ? slint::SharedString{}
                  : to_shared(state->user_presets[static_cast<std::size_t>(
                                                     index - 1)]
                                  .name));
          (*app)->set_preset_editor_description(
              index == 0
                  ? slint::SharedString{}
                  : to_shared(state->user_presets[static_cast<std::size_t>(
                                                     index - 1)]
                                  .description));
          (*app)->set_preset_editor_error({});
          (*app)->set_preset_editor_open(true);
        }
      });
    });

    app->on_cancel_preset_editor([weak] {
      run_ui_callback(weak, "关闭预设编辑器失败", [&] {
        if (auto app = weak.lock()) {
          (*app)->set_preset_editor_open(false);
          (*app)->set_preset_editor_error({});
        }
      });
    });

    app->on_save_parameter_preset([weak, state](slint::SharedString name,
                                                 slint::SharedString description) {
      run_ui_callback(weak, "保存用户预设失败", [&] {
        auto app = weak.lock();
        if (!app) return;
        if (reject_when_worker_active(**app, state,
                                      "当前任务正在运行，无法保存参数预设")) {
          return;
        }
        store_current_parameter_params(**app, *state);
        auto preset = user_preset_from_parameter_params(
            shared_to_string(name), shared_to_string(description),
            active_parameter_params(*state));
        if (!preset) {
          (*app)->set_preset_editor_error(to_shared(preset.error()));
          return;
        }
        auto saved = awj::save_user_preset(*preset, false);
        if (!saved && saved.error() == "同名预设已存在；请确认覆盖。") {
          const auto question = awj::wide_from_utf8(
              std::format("预设“{}”已存在。是否覆盖？", preset->name));
          if (MessageBoxW(nullptr, question.c_str(), L"AWJ Studio",
                          MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            saved = awj::save_user_preset(*preset, true);
          } else {
            (*app)->set_preset_editor_error(to_shared("已取消覆盖同名预设。"));
            return;
          }
        }
        if (!saved) {
          (*app)->set_preset_editor_error(to_shared(saved.error()));
          return;
        }
        reload_user_preset_options(**app, *state);
        const auto found = std::ranges::find(state->user_presets, preset->name,
                                             &awj::UserPreset::name);
        if (found != state->user_presets.end()) {
          select_parameter_preset(
              **app, *state,
              static_cast<int>(std::distance(state->user_presets.begin(), found)) +
                  1);
        }
        (*app)->set_preset_editor_open(false);
        (*app)->set_preset_editor_error({});
        (*app)->set_status_text(to_shared("用户预设已保存。"));
      });
    });

    app->on_install_context_menu_requested([weak, state] {
      run_ui_callback(weak, "安装右键菜单失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法修改右键菜单")) {
            return;
          }
          store_current_menu_params(**app, *state);
          if (auto valid = validate_menu_params(state->menu_params); !valid) {
            (*app)->set_context_menu_status(to_shared(valid.error()));
            (*app)->set_status_text(to_shared(valid.error()));
            return;
          }
          if (auto saved = write_studio_config_file(capture_studio_config(**app, state.get()),
                                                    *state->config_defaults); !saved) {
            (*app)->set_context_menu_status(to_shared(saved.error()));
            (*app)->set_status_text(to_shared(saved.error()));
            return;
          }
          if (auto result = install_shell_context_menu(state->menu_params); !result) {
            (*app)->set_context_menu_status(to_shared(result.error()));
            (*app)->set_status_text(to_shared(result.error()));
            return;
          }
          state->last_config_snapshot = capture_studio_config(**app, state.get());
          (*app)->set_context_menu_warning({});
          (*app)->set_context_menu_status(to_shared("右键菜单已安装。"));
          (*app)->set_status_text(to_shared("右键菜单已安装。"));
        }
      });
    });

    app->on_remove_context_menu_requested([weak, state] {
      run_ui_callback(weak, "移除右键菜单失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法修改右键菜单")) {
            return;
          }
          if (auto result = remove_shell_context_menu(); !result) {
            (*app)->set_context_menu_status(to_shared(result.error()));
            (*app)->set_status_text(to_shared(result.error()));
            return;
          }
          (*app)->set_context_menu_warning({});
          (*app)->set_context_menu_status(to_shared("右键菜单已移除。"));
          (*app)->set_status_text(to_shared("右键菜单已移除。"));
        }
      });
    });

    app->on_save_menu_params_requested([weak, state] {
      run_ui_callback(weak, "保存菜单参数失败", [&] {
        if (auto app = weak.lock()) {
          store_current_menu_params(**app, *state);
          if (auto valid = validate_menu_params(state->menu_params); !valid) {
            (*app)->set_context_menu_status(to_shared(valid.error()));
            (*app)->set_status_text(to_shared(valid.error()));
            return;
          }
          auto current = capture_studio_config(**app, state.get());
          if (auto saved = write_studio_config_file(current, *state->config_defaults); !saved) {
            (*app)->set_context_menu_status(to_shared(saved.error()));
            (*app)->set_status_text(to_shared(saved.error()));
            return;
          }
          state->last_config_snapshot = std::move(current);
          if (auto warning = shell_context_menu_warning(state->menu_params)) {
            (*app)->set_context_menu_warning(to_shared(*warning));
          } else {
            (*app)->set_context_menu_warning({});
          }
          (*app)->set_context_menu_status(to_shared("菜单参数已保存。"));
          (*app)->set_status_text(to_shared("菜单参数已保存。"));
        }
      });
    });

    app->on_menu_format_selected([weak, state](int index) {
      run_ui_callback(weak, "切换菜单参数失败", [&] {
        if (auto app = weak.lock()) {
          store_current_menu_params(**app, *state);
          load_menu_params_for_index(**app, *state, index);
        }
      });
    });

    app->on_context_menu_warning_clicked([weak, state] {
      run_ui_callback(weak, "移除旧右键菜单失败", [&] {
        if (auto app = weak.lock()) {
          if (auto result = remove_shell_context_menu(); !result) {
            (*app)->set_context_menu_status(to_shared(result.error()));
            (*app)->set_status_text(to_shared(result.error()));
            return;
          }
          (*app)->set_context_menu_warning({});
          (*app)->set_context_menu_status(to_shared("旧右键菜单已移除。"));
          (*app)->set_status_text(to_shared("旧右键菜单已移除。"));
        }
      });
    });

    app->on_toggle_template_token([weak, state](slint::SharedString token) {
      run_ui_callback(weak, "切换模板变量失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法修改命名规则")) {
            return;
          }
          toggle_template_token(**app, shared_to_string(token));
        }
      });
    });

    app->on_title_bar_theme_requested([weak](bool dark_mode) {
      try {
        if (auto app = weak.lock()) {
          apply_title_bar_theme((*app)->window(), dark_mode);
        }
      } catch (...) {
      }
    });

    state->theme_timer.start(
        slint::TimerMode::Repeated,
        awj::studio_defaults::theme_refresh_interval, [weak] {
          run_ui_callback(weak, "更新主题状态失败", [&] {
            if (auto app = weak.lock()) {
              (*app)->set_system_dark_mode(windows_prefers_dark_mode());
            }
          });
        });

    std::weak_ptr<UiState> weak_state = state;
    state->config_timer.start(
        slint::TimerMode::Repeated,
        awj::studio_defaults::config_save_interval,
        [weak, weak_state] {
          run_ui_callback(weak, "保存 Studio 配置失败", [&] {
            auto state = weak_state.lock();
            auto app = weak.lock();
            if (!state || !app) {
              return;
            }
            if (auto saved =
                    persist_studio_config_if_changed(**app, *state);
                !saved) {
              (*app)->set_status_text(
                  to_shared(std::format("保存 Studio 配置失败：{}",
                                        saved.error())));
              return;
            }
          });
        });

    app->on_browse_input([weak, state] {
      run_ui_callback(weak, "选择输入路径失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法选择输入路径")) {
            return;
          }
          const bool pick_folder = (*app)->get_input_mode_index() != 0;
          if (auto path = awj::ui_path_picker::choose_path(pick_folder)) {
            awj::ui_import::Request job{
                .roots = {{.path = *path, .force_directory = pick_folder}},
                .origin = pick_folder ? awj::ui_import::Origin::folder_dialog
                                      : awj::ui_import::Origin::file_dialog,
                .input_hint = *path,
                .update_input_path = true};
            if (enqueue_import(state, std::move(job))) {
              (*app)->set_status_text(to_shared(pick_folder ? "正在扫描文件夹…"
                                                            : "正在导入文件…"));
            }
          }
        }
      });
    });

    app->on_input_path_accepted(
        [weak, state](slint::SharedString input_text) {
          run_ui_callback(weak, "输入路径入队失败", [&] {
            if (auto app = weak.lock()) {
              if (reject_when_worker_active(**app, state,
                                            "当前任务正在运行，无法添加队列")) {
                return;
              }
              const auto path = awj::normalize_path_argument(
                  awj::wide_from_utf8(shared_to_string(input_text)), "输入路径");
              if (!path) {
                (*app)->set_status_text(to_shared(path.error()));
                return;
              }
              (*app)->set_input_path(to_shared(awj::path_to_utf8(*path)));
              awj::ui_import::Request job{.roots = {{.path = *path}},
                            .origin = awj::ui_import::Origin::command_line,
                            .input_hint = *path,
                            .update_input_path = true};
              if (enqueue_import(state, std::move(job))) {
                (*app)->set_status_text(to_shared("正在导入输入路径…"));
              }
            }
          });
        });

    // Windows Explorer 外部路径由原生 OLE IDropTarget 处理。
    // 保留 Slint callback 作为公开 API 兼容点，但 Windows 不再从 DropEvent.data.plain_text()
    // 解析文件系统路径。Linux 分支仍保留原有 DropArea 行为。

    app->on_queue_menu_action(
        [weak, state](int index, slint::SharedString action_text) {
          run_ui_callback(weak, "队列菜单操作失败", [&] {
            if (auto app = weak.lock()) {
              handle_queue_menu_action(**app, state, index,
                                       shared_to_string(action_text));
            }
          });
        });

    app->on_queue_drag_data([state](int index) {
      try {
        return make_queue_drag_data(state, index);
      } catch (...) {
        return slint::DataTransfer{};
      }
    });

    app->on_queue_drag_can_drop(
        [state](slint::language::DropEvent event, int target_slot) {
          try {
            return handle_queue_drag_can_drop(state, std::move(event),
                                             target_slot);
          } catch (...) {
            return slint::language::DragAction::None;
          }
        });

    app->on_queue_drag_dropped(
        [weak, state](slint::language::DropEvent event, int target_slot) {
          try {
            if (auto app = weak.lock()) {
              return handle_queue_drag_dropped(**app, state, std::move(event),
                                               target_slot);
            }
          } catch (const std::bad_alloc&) {
            report_ui_callback_failure(weak, "队列拖动失败", "内存不足。");
          } catch (const std::length_error&) {
            report_ui_callback_failure(weak, "队列拖动失败",
                                       "数据超过运行时限制。");
          } catch (const std::exception&) {
            report_ui_callback_failure(weak, "队列拖动失败",
                                       "发生未预期异常。");
          } catch (...) {
            report_ui_callback_failure(weak, "队列拖动失败", "发生未知异常。");
          }
          return slint::language::DragAction::None;
        });

    app->on_queue_row_pointer_event(
        [weak, state](int index, int button, int kind, float local_y) {
          run_ui_callback(weak, "队列交互失败", [&] {
            if (auto app = weak.lock()) {
              handle_queue_pointer_event(**app, state, index, button, kind,
                                         local_y);
            }
          });
        });

    app->on_browse_output([weak, state] {
      run_ui_callback(weak, "选择输出路径失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法选择输出目录")) {
            return;
          }
          if (auto folder = awj::ui_path_picker::choose_path(true)) {
            post_to_ui(weak, [folder = *folder](AwjStudio& app) {
              app.set_output_dir(to_shared(awj::path_to_utf8(folder)));
            });
          }
        }
      });
    });

    app->on_output_path_accepted(
        [weak, state](slint::SharedString output_text) {
          run_ui_callback(weak, "输出目录校验失败", [&] {
            if (auto app = weak.lock()) {
              if (reject_when_worker_active(**app, state,
                                            "当前任务正在运行，无法修改输出目录")) {
                return;
              }
              const auto raw = shared_to_string(output_text);
              if (trim_copy(raw).empty()) {
                (*app)->set_output_dir({});
                return;
              }
              const auto path = awj::normalize_path_argument(
                  awj::wide_from_utf8(raw), "输出目录");
              if (!path) {
                (*app)->set_status_text(to_shared(path.error()));
                return;
              }
              (*app)->set_output_dir(to_shared(awj::path_to_utf8(*path)));
            }
          });
        });

    // Windows 的 HWND 已由 AWJ 原生 IDropTarget 接管，Slint DropEvent 不再承担
    // Explorer 文件系统路径传输。输出目录仍通过选择器或文本框修改。

    app->on_browse_large_image_file([weak, state] {
      run_ui_callback(weak, "添加大图文件失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法添加大图任务")) {
            return;
          }
          if (auto path = awj::ui_path_picker::choose_path(false)) {
            add_manual_large_images_from_picker(**app, *state, *path, false);
          }
        }
      });
    });

    app->on_browse_large_image_folder([weak, state] {
      run_ui_callback(weak, "添加大图文件夹失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法添加大图任务")) {
            return;
          }
          if (auto folder = awj::ui_path_picker::choose_path(true)) {
            add_manual_large_images_from_picker(**app, *state, *folder, true);
          }
        }
      });
    });

    app->on_open_output([weak] {
      run_ui_callback(weak, "打开输出路径失败", [&] {
        if (auto app = weak.lock()) {
          if (auto opened = open_path(effective_output_dir(**app), true);
              !opened) {
            (*app)->set_status_text(
                to_shared(std::format("打开输出路径失败：{}", opened.error())));
          }
        }
      });
    });

    app->on_cancel_conversion([weak, state] {
      run_ui_callback(weak, "停止任务失败", [&] {
        const bool stop_requested = request_all_workers_stop(state);
        if (auto app = weak.lock()) {
          if (!stop_requested) {
            (*app)->set_running(false);
            (*app)->set_status_text(to_shared("没有正在运行的任务"));
            return;
          }
          (*app)->set_running(true);
          (*app)->set_status_text(to_shared("正在停止当前任务…"));
        }
      });
    });

    app->on_large_image_action_requested(
        [weak, state](int index, slint::SharedString action_text) {
          run_ui_callback(weak, "处理大图操作失败", [&] {
            auto app = weak.lock();
            if (!app) {
              return;
            }
            const auto action = shared_to_string(action_text);
            awj::BatchLargeImageItem item{};
            {
              std::scoped_lock lock{state->mutex};
              if (index < 0 || static_cast<std::size_t>(index) >=
                                   state->large_image_items.size()) {
                (*app)->set_status_text(to_shared("未选择大图任务"));
                return;
              }
              if (state->worker_active) {
                (*app)->set_status_text(
                    to_shared("当前任务正在终止，请稍后再处理大图"));
                return;
              }
              item = state->large_image_items[static_cast<std::size_t>(index)];
            }
            if (action != "grid" && action != "zenrav1e") {
              (*app)->set_status_text(to_shared("未知的大图处理方式"));
              return;
            }
            if (!large_image_action_available(item, action)) {
              (*app)->set_status_text(to_shared(
                  large_image_action_status(item, action)));
              return;
            }
            const auto previous_input =
                shared_to_string((*app)->get_input_path());
            (*app)->set_input_path(
                to_shared(awj::path_to_utf8(item.file.path)));
            auto cfg = config_from_ui(**app, *state);
            (*app)->set_input_path(to_shared(previous_input));
            if (!cfg) {
              (*app)->set_status_text(
                  to_shared(std::format("配置错误：{}", cfg.error())));
              return;
            }
            (*cfg).input_path = item.file.path;
            (*cfg).output_format = awj::OutputFormat::avif;
            (*cfg).studio_large_action = awj::wide_from_utf8(action);
            (*cfg).visual_quality.reset();
            set_large_image_status(*state, index,
                                   large_image_action_status(item, action));
            begin_child_conversion_run(weak, state, std::move(*cfg), index);
          });
        });

    app->on_start_conversion([weak, state] {
      run_ui_callback(weak, "启动转换失败", [&] {
        auto app = weak.lock();
        if (!app) {
          return;
        }

        if (worker_active(state)) {
          const auto stopped = force_stop_current_worker(state);
          (*app)->set_running(stopped != ForceStopResult::no_worker);
          switch (stopped) {
            case ForceStopResult::terminated:
              (*app)->set_status_text(
                  to_shared("正在强制终止当前编码任务…"));
              break;
            case ForceStopResult::terminate_failed:
              (*app)->set_status_text(to_shared(
                  "强制终止失败，任务仍在运行；请重试或关闭 Studio"));
              break;
            case ForceStopResult::no_worker:
              (*app)->set_status_text(to_shared("没有正在运行的编码任务"));
              break;
          }
          return;
        }

        if (trim_copy(shared_to_string((*app)->get_input_path())).empty()) {
          std::scoped_lock lock{state->mutex};
          if (!state->queue_items.empty()) {
            const auto& first = state->queue_items.front();
            const auto fallback =
                first.source_root.empty() ? first.path : first.source_root;
            (*app)->set_input_path(to_shared(awj::path_to_utf8(fallback)));
            if (output_dir_is_empty(**app)) {
              (*app)->set_output_dir(to_shared(
                  awj::path_to_utf8(awj::default_output_dir_for(fallback))));
            }
          }
        }

        auto cfg = config_from_ui(**app, *state);
        if (!cfg) {
          (*app)->set_running(false);
          (*app)->set_status_text(
              to_shared(std::format("配置错误：{}", cfg.error())));
          return;
        }

        begin_queue_conversion_run(weak, state, std::move(*cfg));
      });
    });

    const auto now = std::chrono::system_clock::now();
    const auto last_check = state->last_successful_update_check_at > 0
                                ? std::optional{
                                      std::chrono::system_clock::time_point{
                                          std::chrono::seconds{
                                              state->last_successful_update_check_at}}}
                                : std::nullopt;
    if (health_event == nullptr && awj::update::should_check_now(
            {.trigger = awj::update::CheckTrigger::startup,
             .last_successful_check = last_check,
             .now = now})) {
      start_update_check(weak, state);
    }

    app->show();
    start_native_drop_registration(weak, state);
    if (health_check_ready) {
      auto event = adopt_win32_handle(
          OpenEventW(EVENT_MODIFY_STATE, FALSE, health_event));
      if (event != nullptr) {
        SetEvent(event.get());
      }
    }
    apply_title_bar_theme(app->window(), effective_studio_dark_mode(*app));
    constrain_window_to_work_area(app->window());
    // 关窗回调在事件循环里执行，而 persist_studio_config_if_changed 会经由
    // capture_studio_config 分配三十多个 std::string，下面的 std::format 也会分配。
    // 这里抛出的异常会穿回 Rust 侧的 winit 栈帧（panic="abort"），必须自己接住；
    // 无论保存成功与否都要放行关窗，否则窗口会关不掉。
    app->window().on_close_requested([weak, state] {
      run_ui_callback(weak, "关闭窗口时保存配置失败", [&] {
        // 必须在 Slint/Winit 销毁 HWND 前停止尚未完成的注册重试并撤销 AWJ 的
        // IDropTarget；Timer/Registration 都在 UI/OLE 线程创建和销毁。
        state->native_drop_registration_finished = true;
        state->native_drop_timer.stop();
        state->native_drop.reset();
        if (state->import_dispatcher) {
          state->import_dispatcher->request_stop();
        }
        force_stop_current_worker(state);
        if (state->update_worker.joinable()) {
          state->update_worker.request_stop();
        }
        if (auto app = weak.lock()) {
          state->last_changelog_exit_version = AWJ_BUILD_VERSION;
          if (auto saved = persist_studio_config_if_changed(**app, *state);
              !saved) {
            set_status_text_noexcept(
                **app,
                std::format("保存 Studio 配置失败：{}", saved.error()));
          }
        }
      });
      return slint::CloseRequestResponse::HideWindow;
    });
    app->run();
    std::jthread worker;
    {
      std::scoped_lock lock{state->mutex};
      worker = std::move(state->worker);
    }
    if (worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
    if (state->update_worker.joinable()) {
      state->update_worker.request_stop();
      state->update_worker.join();
    }
    if (state->import_dispatcher) {
      state->import_dispatcher->request_stop();
      state->import_dispatcher->join();
    }
    return 0;
  } catch (const std::exception&) {
    MessageBoxW(nullptr, L"Studio 启动失败。", L"AWJ",
                MB_OK | MB_ICONERROR);
    return 1;
  } catch (...) {
    MessageBoxW(nullptr, L"Studio 启动失败：未知异常。", L"AWJ",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}


int run_shell_convert_window(int argc, wchar_t* argv[]) {
  try {
    std::vector<std::wstring> args;
    args.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
    for (int i = 1; i < argc; ++i) {
      if (std::wcscmp(argv[i], L"--shell-window") != 0) {
        args.emplace_back(argv[i]);
      }
    }
    auto parsed = awj::parse_arguments(args);
    if (!parsed || parsed->should_exit) {
      const auto text = parsed ? std::string{"右键转换参数无效。"} : parsed.error();
      MessageBoxW(nullptr, awj::wide_from_utf8(text).c_str(), L"AWJimage", MB_OK | MB_ICONERROR);
      return 1;
    }
    if (auto valid = awj::validate_execution_config(parsed->config); !valid) {
      MessageBoxW(nullptr, awj::wide_from_utf8(valid.error()).c_str(), L"AWJimage", MB_OK | MB_ICONERROR);
      return 1;
    }
    auto collected = collect_shell_launch_inputs(parsed->config.output_format,
                                                 parsed->config.append_png_suffix,
                                                 parsed->shell_inputs);
    if (!collected) {
      MessageBoxW(nullptr, awj::wide_from_utf8(collected.error()).c_str(),
                  L"AWJimage", MB_OK | MB_ICONERROR);
      return 1;
    }
    if (!*collected) return 0;

    ensure_slint_backend();
    auto app = ShellConvertWindow::create();
    app->set_ui_font_family(to_shared(select_system_ui_font_family()));
    const bool dark_mode = shell_window_dark_mode();
    app->set_dark_mode(dark_mode);
    auto rows = std::make_shared<slint::VectorModel<TaskRow>>();
    app->set_task_rows(rows);
    app->set_status_text(to_shared("正在扫描队列..."));
    std::vector<awj::ImageFile> shell_files;
    if (auto scanned = awj::scan_images(parsed->config, parsed->shell_inputs, shell_files)) {
      std::vector<TaskRow> pending_rows;
      pending_rows.reserve(shell_files.size());
      for (const auto& image : shell_files) {
        pending_rows.push_back(pending_shell_task_row(parsed->config, image));
      }
      rows->set_vector(std::move(pending_rows));
      app->set_status_text(to_shared(std::format("队列：{} 个文件。", shell_files.size())));
    } else {
      append_log_row(rows, scanned.error());
    }
    auto weak = slint::ComponentWeakHandle(app);
    std::stop_source stop_source;
    std::atomic_bool running{true};
    const auto output_dir = awj::output_dir_for(parsed->config);

    app->on_cancel_requested([weak, &stop_source, &running] {
      if (running.load(std::memory_order_acquire)) {
        stop_source.request_stop();
        if (auto app = weak.lock()) {
          (*app)->set_status_text(to_shared("正在停止任务…"));
        }
      } else if (auto app = weak.lock()) {
        (*app)->window().hide();
      }
    });
    app->on_force_terminate_requested([] {
      TerminateProcess(GetCurrentProcess(),
                       awj::studio_defaults::worker_force_stop_exit_code);
    });
    app->on_open_output_requested([output_dir] {
      if (!output_dir.empty()) {
        ShellExecuteW(nullptr, L"open", output_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
    });

    auto cfg = parsed->config;
    auto shell_inputs = parsed->shell_inputs;
    std::jthread worker{[weak, rows, cfg = std::move(cfg), shell_inputs = std::move(shell_inputs), token = stop_source.get_token(), &running]() mutable {
      const auto summary = awj::run_batch(
          cfg,
          [weak, rows](const awj::BatchProgress& event) {
            slint::invoke_from_event_loop([weak, rows, event] {
              if (auto app = weak.lock()) {
                if (event.kind == awj::BatchEventKind::item_started) {
                  mark_task_row_running(rows, event.result);
                } else if (event.kind == awj::BatchEventKind::item_finished) {
                  const auto row = task_row_from_result(event.result);
                  if (event.result.index < rows->row_count()) {
                    rows->set_row_data(event.result.index, row);
                  } else {
                    add_task_row(rows, event.result);
                  }
                } else if (event.kind == awj::BatchEventKind::large_image_queued) {
                  add_large_image_task_row(rows, event.large_image);
                } else if (event.kind == awj::BatchEventKind::warning) {
                  append_log_row(rows, event.text);
                }
                if (event.total > 0) {
                  (*app)->set_progress(static_cast<float>(event.completed) /
                                      static_cast<float>(event.total));
                }
                if (event.kind == awj::BatchEventKind::warning ||
                    event.kind == awj::BatchEventKind::large_image_queued) {
                  (*app)->set_status_text(to_shared(event.text));
                } else if (event.total > 0) {
                  (*app)->set_status_text(to_shared(std::format(
                      "处理中：{} / {}", event.completed, event.total)));
                } else if (!event.text.empty()) {
                  (*app)->set_status_text(to_shared(event.text));
                }
              }
            });
          },
          token,
          shell_inputs);
      slint::invoke_from_event_loop([weak, rows, summary, close_on_finish = cfg.shell_close_on_finish, &running] {
        running.store(false, std::memory_order_release);
        if (auto app = weak.lock()) {
          (*app)->set_running(false);
          (*app)->set_progress(1.0f);
          if (summary) {
            (*app)->set_status_text(to_shared(std::format("完成：成功 {}，失败 {}，取消 {}。",
                                                          summary->ok_count,
                                                          summary->failed_count,
                                                          summary->canceled_count)));
            if (close_on_finish && summary->failed_count == 0 &&
                summary->canceled_count == 0) {
              (*app)->window().hide();
            }
          } else {
            append_log_row(rows, summary.error());
            (*app)->set_status_text(to_shared(summary.error()));
          }
        }
      });
    }};

    app->show();
    apply_title_bar_theme(app->window(), dark_mode);
    constrain_window_to_work_area(app->window());
    // 同上：事件循环里的回调不能让异常逃回 Rust 栈帧。这里的调用本身都不分配，
    // 但加一层 catch-all 之后，将来往里加代码也不会把整个进程带走。
    app->window().on_close_requested([&stop_source, &running]() noexcept {
      try {
        if (running.load(std::memory_order_acquire)) {
          stop_source.request_stop();
          TerminateProcess(GetCurrentProcess(),
                           awj::studio_defaults::worker_force_stop_exit_code);
        }
      } catch (...) {
      }
      return slint::CloseRequestResponse::HideWindow;
    });
    slint::run_event_loop();
    const int rc = 0;
    stop_source.request_stop();
    worker.request_stop();
    if (worker.joinable()) {
      worker.join();
    }
    return rc;
  } catch (const std::exception&) {
    MessageBoxW(nullptr, L"右键转换窗口启动失败。", L"AWJimage", MB_OK | MB_ICONERROR);
    return 1;
  } catch (...) {
    MessageBoxW(nullptr, L"右键转换窗口启动失败：未知异常。", L"AWJimage", MB_OK | MB_ICONERROR);
    return 1;
  }
}

#else

#include <slint.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <any>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <exception>
#include <expected>
#include <format>
#include <print>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "awj_studio.h"
#include "changelog_history.h"

import awj.config;
import awj.core;
import awj.encoding_defaults;
import awj.large_image_plan;
import awj.pipeline;
import awj.preset;
import awj.studio_defaults;
import awj.update_linux;
import awj.update_keyring;
import awj.update_manifest;
import awj.update_manifest_v2;
import awj.update_model;
import awj.update_runtime;

extern char** environ;

namespace {

namespace fs = std::filesystem;

struct LinuxMenuParams {
  std::string quality_text{};
  std::string bit_depth_text{};
  std::string speed_text{};
  int avif_encoder_index{};
  int avif_color_representation_index{};
  int chroma_index{};
  int alpha_policy_index{1};
  int jpegli_progressive_index{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  bool strip_metadata{};
  bool install_avif_png_command{};
  int size_limit_index{};
  std::string max_width_text{};
  std::string max_height_text{};
  std::string max_long_edge_text{};
  std::string max_short_edge_text{};
};

// 参数页按格式保存会话内的编辑值；普通队列只在开始时读取 queue-format
// 指向的这一组，避免“切换编辑格式”悄悄改变输出格式。
struct LinuxParameterParams {
  std::string quality_text{};
  std::string visual_quality_text{};
  std::string bit_depth_text{};
  std::string speed_text{};
  int avif_encoder_index{};
  int avif_color_representation_index{};
  int chroma_index{};
  int alpha_policy_index{1};
  int jpegli_progressive_index{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  std::string threads_text{};
  std::string memory_limit_text{};
  int size_limit_index{};
  std::string max_width_text{};
  std::string max_height_text{};
  std::string max_long_edge_text{};
  std::string max_short_edge_text{};
};

struct LinuxUiState {
  std::jthread worker{};
  std::jthread update_worker{};
  std::shared_ptr<slint::VectorModel<TaskRow>> task_rows{};
  std::shared_ptr<slint::VectorModel<LargeImageRow>> large_image_rows{};
  std::shared_ptr<slint::VectorModel<UpdateHistoryRow>> update_history_rows{};
  std::vector<awj::BatchLargeImageItem> large_image_items{};
  std::vector<fs::path> failed_paths{};
  // The visible queue is the source of truth. A manifest is written only
  // immediately before a run so changing UI settings cannot rescan a folder.
  std::vector<awj::ImageFile> queue_files{};
  std::uint64_t next_queue_run_id{1};
  std::array<LinuxParameterParams, 5> builtin_params{};
  std::array<LinuxParameterParams, 5> parameter_preset_params{};
  std::vector<awj::UserPreset> user_presets{};
  std::vector<std::string> user_preset_errors{};
  int parameter_preset_index{};
  int last_format_index{};
  std::array<LinuxMenuParams, 5> menu_params{};
  int menu_format_index{};
  fs::path config_path{};
  nlohmann::ordered_json config_document{nlohmann::ordered_json::object()};
  bool config_readable{true};
  bool update_check_active{};
  std::string update_channel{"stable"};
  bool show_update_changelog{true};
  bool hide_update_changelog_after_exit{true};
  bool show_update_changelog_after_update{true};
  std::string last_changelog_exit_version{};
  std::int64_t last_successful_update_check_at{};
  // v1 remains only as cached state for 1.0.3 bridge compatibility. Linux
  // Studio itself consumes the independent v2 counter and signed cache.
  std::int64_t last_verified_manifest_sequence{};
  std::int64_t last_verified_manifest_v2_sequence{};
  std::string pending_update_version{};
  std::string pending_update_channel{};
  std::string pending_update_release_url{};
  std::string pending_update_published_at{};
  std::string pending_update_changelog_zh_cn{};
  std::string pending_update_changelog_en{};
  std::string update_manifest_raw{};
  std::string update_manifest_signature{};
  std::string update_manifest_v2_raw{};
  std::string update_manifest_v2_signature{};
  std::string update_keyring_raw{};
  std::string update_keyring_signature{};
  std::string update_status_zh{"尚未检查"};
  std::string update_status_en{"Not checked yet"};
};

nlohmann::ordered_json linux_menu_params_json(const LinuxMenuParams& params) {
  return {{"quality_text", params.quality_text},
          {"bit_depth_text", params.bit_depth_text},
          {"speed_text", params.speed_text},
          {"avif_encoder_index", params.avif_encoder_index},
          {"avif_color_representation_index",
           params.avif_color_representation_index},
          {"chroma_index", params.chroma_index},
          {"alpha_policy_index", params.alpha_policy_index},
          {"jpegli_progressive_index", params.jpegli_progressive_index},
          {"jpegli_optimize_huffman", params.jpegli_optimize_huffman},
          {"jpegli_xyb", params.jpegli_xyb},
          {"strip_metadata", params.strip_metadata},
          {"install_avif_png_command", params.install_avif_png_command},
          {"size_limit_index", params.size_limit_index},
          {"max_width_text", params.max_width_text},
          {"max_height_text", params.max_height_text},
          {"max_long_edge_text", params.max_long_edge_text},
          {"max_short_edge_text", params.max_short_edge_text}};
}

void load_linux_menu_params(const nlohmann::ordered_json& document,
                            std::array<LinuxMenuParams, 5>& output) {
  const auto found = document.find("menu_params");
  if (found == document.end() || !found->is_array() || found->size() != output.size()) {
    return;
  }
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto& value = (*found)[index];
    if (!value.is_object()) continue;
    auto& params = output[index];
    const auto text = [&](std::string_view key, std::string& target) {
      const auto it = value.find(std::string{key});
      if (it != value.end() && it->is_string()) target = it->get<std::string>();
    };
    const auto integer = [&](std::string_view key, int minimum, int maximum,
                             int& target) {
      const auto it = value.find(std::string{key});
      if (it != value.end() && it->is_number_integer()) {
        const auto candidate = it->get<long long>();
        if (candidate >= minimum && candidate <= maximum) {
          target = static_cast<int>(candidate);
        }
      }
    };
    const auto boolean = [&](std::string_view key, bool& target) {
      const auto it = value.find(std::string{key});
      if (it != value.end() && it->is_boolean()) target = it->get<bool>();
    };
    text("quality_text", params.quality_text);
    text("bit_depth_text", params.bit_depth_text);
    text("speed_text", params.speed_text);
    integer("avif_encoder_index", 0, 3, params.avif_encoder_index);
    integer("avif_color_representation_index", 0, 2,
            params.avif_color_representation_index);
    integer("chroma_index", 0, 3, params.chroma_index);
    integer("alpha_policy_index", 0, 2, params.alpha_policy_index);
    integer("jpegli_progressive_index", 0, 2, params.jpegli_progressive_index);
    boolean("jpegli_optimize_huffman", params.jpegli_optimize_huffman);
    boolean("jpegli_xyb", params.jpegli_xyb);
    boolean("strip_metadata", params.strip_metadata);
    boolean("install_avif_png_command", params.install_avif_png_command);
    integer("size_limit_index", 0, 2, params.size_limit_index);
    text("max_width_text", params.max_width_text);
    text("max_height_text", params.max_height_text);
    text("max_long_edge_text", params.max_long_edge_text);
    text("max_short_edge_text", params.max_short_edge_text);
  }
}

std::string trim_copy(std::string value) {
  const auto first = std::ranges::find_if_not(value, [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  }).base();
  if (first >= last) return {};
  return std::string(first, last);
}

std::wstring trim_copy(std::wstring value) {
  const auto is_space = [](wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
  };
  const auto first = std::ranges::find_if_not(value, is_space);
  const auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
  if (first >= last) return {};
  return std::wstring(first, last);
}

std::wstring memory_arg_from_ui(std::wstring value) {
  value = trim_copy(std::move(value));
  if (value.empty()) return value;
  const bool has_unit = std::ranges::any_of(value, [](wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
  });
  return has_unit ? value : value + L"GiB";
}

std::string shell_quote(std::string_view value) {
  std::string out{"'"};
  for (const char ch : value) {
    if (ch == '\'') out += "'\\''";
    else out += ch;
  }
  out += "'";
  return out;
}

bool command_exists(std::string_view command) {
  return std::system(std::format("command -v {} >/dev/null 2>&1", command).c_str()) == 0;
}

struct PcloseDeleter {
  void operator()(FILE* file) const noexcept {
    if (file != nullptr) pclose(file);
  }
};

slint::SharedString to_shared(std::string_view text);
std::string shared_to_string(const slint::SharedString& value);

std::optional<std::string> run_capture(const std::string& command) {
  std::unique_ptr<FILE, PcloseDeleter> pipe{popen(command.c_str(), "r")};
  if (!pipe) return std::nullopt;
  std::string output;
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
    output += buffer.data();
  }
  output = trim_copy(std::move(output));
  if (output.empty()) return std::nullopt;
  return output;
}

void load_system_font_options(AwjStudio& app) {
  std::unordered_set<std::string> families;
  if (auto output = run_capture("fc-list -f '%{family}\\n' 2>/dev/null")) {
    std::size_t line_start = 0;
    while (line_start <= output->size()) {
      const auto line_end = output->find('\n', line_start);
      const auto line = output->substr(
          line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
      std::size_t name_start = 0;
      while (name_start <= line.size()) {
        const auto name_end = line.find(',', name_start);
        auto name = trim_copy(line.substr(
            name_start, name_end == std::string::npos ? std::string::npos : name_end - name_start));
        if (!name.empty() && name.front() != '@') families.insert(std::move(name));
        if (name_end == std::string::npos) break;
        name_start = name_end + 1;
      }
      if (line_end == std::string::npos) break;
      line_start = line_end + 1;
    }
  }
  std::vector<std::string> sorted(families.begin(), families.end());
  std::ranges::sort(sorted);
  std::vector<ComboOption> options;
  options.reserve(sorted.size() + 1);
  options.push_back(ComboOption{.text = to_shared("系统默认字体"), .enabled = true});
  for (const auto& family : sorted) {
    options.push_back(ComboOption{.text = to_shared(family), .enabled = true});
  }
  app.set_ui_font_options(std::make_shared<slint::VectorModel<ComboOption>>(std::move(options)));
  const auto selected = shared_to_string(app.get_ui_font_family());
  const auto found = std::ranges::find(sorted, selected);
  if (found == sorted.end()) {
    app.set_ui_font_index(0);
    app.set_ui_font_family({});
  } else {
    app.set_ui_font_index(static_cast<int>(std::distance(sorted.begin(), found)) + 1);
  }
}

std::expected<fs::path, std::string> choose_path(bool directory) {
  std::vector<std::string> commands;
  if (command_exists("zenity")) commands.push_back(directory ? "zenity --file-selection --directory" : "zenity --file-selection");
  if (command_exists("yad")) commands.push_back(directory ? "yad --file-selection --directory" : "yad --file-selection");
  if (command_exists("kdialog")) commands.push_back(directory ? "kdialog --getexistingdirectory" : "kdialog --getopenfilename");
  if (commands.empty()) {
    return std::unexpected{"未找到 Linux 文件选择器；请安装 zenity/yad/kdialog，或手动输入路径。"};
  }
  for (const auto& command : commands) {
    if (auto selected = run_capture(command)) return fs::path{*selected};
  }
  return std::unexpected{"未选择路径。"};
}

std::expected<void, std::string> open_path(fs::path path) {
  if (path.empty()) return std::unexpected{"没有可打开的路径。"};
  std::error_code ec;
  if (fs::is_regular_file(path, ec) && !ec) path = path.parent_path();
  if (!fs::exists(path, ec) || ec) path = path.parent_path();
  if (path.empty()) return std::unexpected{"路径不存在，无法打开。"};
  const auto quoted = shell_quote(awj::path_to_utf8(path));
  if (command_exists("gio") && std::system(std::format("gio open {} >/dev/null 2>&1 &", quoted).c_str()) == 0) return {};
  if (command_exists("xdg-open") && std::system(std::format("xdg-open {} >/dev/null 2>&1 &", quoted).c_str()) == 0) return {};
  if (command_exists("thunar") && std::system(std::format("thunar {} >/dev/null 2>&1 &", quoted).c_str()) == 0) return {};
  return std::unexpected{"未找到可用的目录打开工具（gio/xdg-open/thunar）。"};
}

slint::SharedString to_shared(std::string_view text) {
  return slint::SharedString{std::string{text}.c_str()};
}

std::string shared_to_string(const slint::SharedString& value) {
  return std::string{value.data(), value.size()};
}

std::vector<std::string> native_drop_paths(const slint::SharedString& value) {
  std::vector<std::string> paths;
  const auto text = shared_to_string(value);
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const auto end = text.find('\n', begin);
    auto path = text.substr(begin, end == std::string::npos ? std::string::npos
                                                              : end - begin);
    if (!path.empty() && path.back() == '\r') path.pop_back();
    if (!path.empty()) paths.push_back(std::move(path));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return paths;
}

struct LinuxUpdatePersistentState {
  std::string channel{};
  bool show_changelog{};
  bool hide_changelog_after_exit{};
  bool show_changelog_after_update{};
  std::string last_changelog_exit_version{};
  std::int64_t last_successful_check{};
  std::int64_t last_verified_sequence{};
  std::int64_t last_verified_v2_sequence{};
  std::string version{};
  std::string pending_channel{};
  std::string release_url{};
  std::string published_at{};
  std::string changelog_zh_cn{};
  std::string changelog_en{};
  std::string manifest_raw{};
  std::string manifest_signature{};
  std::string manifest_v2_raw{};
  std::string manifest_v2_signature{};
  std::string keyring_raw{};
  std::string keyring_signature{};
};

LinuxUpdatePersistentState capture_linux_update_state(
    const LinuxUiState& state) {
  return {.channel = state.update_channel,
          .show_changelog = state.show_update_changelog,
          .hide_changelog_after_exit = state.hide_update_changelog_after_exit,
          .show_changelog_after_update = state.show_update_changelog_after_update,
          .last_changelog_exit_version = state.last_changelog_exit_version,
          .last_successful_check = state.last_successful_update_check_at,
          .last_verified_sequence = state.last_verified_manifest_sequence,
          .last_verified_v2_sequence = state.last_verified_manifest_v2_sequence,
          .version = state.pending_update_version,
          .pending_channel = state.pending_update_channel,
          .release_url = state.pending_update_release_url,
          .published_at = state.pending_update_published_at,
          .changelog_zh_cn = state.pending_update_changelog_zh_cn,
          .changelog_en = state.pending_update_changelog_en,
          .manifest_raw = state.update_manifest_raw,
          .manifest_signature = state.update_manifest_signature,
          .manifest_v2_raw = state.update_manifest_v2_raw,
          .manifest_v2_signature = state.update_manifest_v2_signature,
          .keyring_raw = state.update_keyring_raw,
          .keyring_signature = state.update_keyring_signature};
}

void restore_linux_update_state(LinuxUiState& state,
                                LinuxUpdatePersistentState value) {
  state.update_channel = std::move(value.channel);
  state.show_update_changelog = value.show_changelog;
  state.hide_update_changelog_after_exit = value.hide_changelog_after_exit;
  state.show_update_changelog_after_update = value.show_changelog_after_update;
  state.last_changelog_exit_version = std::move(value.last_changelog_exit_version);
  state.last_successful_update_check_at = value.last_successful_check;
  state.last_verified_manifest_sequence = value.last_verified_sequence;
  state.last_verified_manifest_v2_sequence = value.last_verified_v2_sequence;
  state.pending_update_version = std::move(value.version);
  state.pending_update_channel = std::move(value.pending_channel);
  state.pending_update_release_url = std::move(value.release_url);
  state.pending_update_published_at = std::move(value.published_at);
  state.pending_update_changelog_zh_cn = std::move(value.changelog_zh_cn);
  state.pending_update_changelog_en = std::move(value.changelog_en);
  state.update_manifest_raw = std::move(value.manifest_raw);
  state.update_manifest_signature = std::move(value.manifest_signature);
  state.update_manifest_v2_raw = std::move(value.manifest_v2_raw);
  state.update_manifest_v2_signature = std::move(value.manifest_v2_signature);
  state.update_keyring_raw = std::move(value.keyring_raw);
  state.update_keyring_signature = std::move(value.keyring_signature);
}

void clear_linux_pending_update(LinuxUiState& state) {
  state.pending_update_version.clear();
  state.pending_update_channel.clear();
  state.pending_update_release_url.clear();
  state.pending_update_published_at.clear();
  state.pending_update_changelog_zh_cn.clear();
  state.pending_update_changelog_en.clear();
}

std::string linux_update_summary(std::string_view changelog) {
  return std::string{changelog.substr(0, changelog.find_first_of("\r\n"))};
}

std::string linux_update_check_time(std::int64_t unix_seconds) {
  if (unix_seconds <= 0) return {};
  const auto point = std::chrono::system_clock::time_point{
      std::chrono::seconds{unix_seconds}};
  return std::format("{:%Y-%m-%d %H:%M:%S} UTC",
                     std::chrono::floor<std::chrono::seconds>(point));
}

bool linux_changelog_first_start_for_current_version(const LinuxUiState& state) {
  return state.last_changelog_exit_version != AWJ_BUILD_VERSION;
}

bool linux_changelog_visible_for_current_session(const LinuxUiState& state) {
  const bool first_start = linux_changelog_first_start_for_current_version(state);
  if (!state.show_update_changelog) {
    // 总开关关闭时，升级后的首次启动仍临时显示一次；该版本退出后隐藏。
    return first_start;
  }
  return !state.hide_update_changelog_after_exit || first_start;
}

bool linux_changelog_should_open_on_start(const LinuxUiState& state) {
  return linux_changelog_first_start_for_current_version(state) &&
         (!state.show_update_changelog || state.show_update_changelog_after_update);
}

bool linux_pending_update_is_newer(const LinuxUiState& state) {
  const auto current = awj::update::parse_version(AWJ_BUILD_VERSION);
  const auto pending = awj::update::parse_version(state.pending_update_version);
  const auto channel = awj::update::parse_channel(state.pending_update_channel);
  const auto preference = state.update_channel == "prerelease"
                              ? awj::update::ChannelPreference::stable_and_prerelease
                              : awj::update::ChannelPreference::stable_only;
  return current && pending && channel && *pending > *current &&
         awj::update::channel_visible_to(*channel, preference);
}

void sync_linux_update_ui(AwjStudio& app, const LinuxUiState& state) {
  const bool english = app.get_language_index() == 1;
  const bool available = linux_pending_update_is_newer(state);
  app.set_current_version(to_shared(AWJ_BUILD_VERSION));
  app.set_update_channel_index(state.update_channel == "prerelease" ? 1 : 0);
  app.set_show_update_changelog_enabled(state.show_update_changelog);
  app.set_show_update_changelog(linux_changelog_visible_for_current_session(state));
  app.set_hide_update_changelog_after_exit(
      state.hide_update_changelog_after_exit);
  app.set_show_update_changelog_after_update(
      state.show_update_changelog_after_update);
  app.set_update_available(available);
  app.set_update_version(to_shared(available ? state.pending_update_version : ""));
  app.set_update_published_at(
      to_shared(available ? state.pending_update_published_at : ""));
  app.set_update_changelog_zh_cn(
      to_shared(available ? state.pending_update_changelog_zh_cn : ""));
  app.set_update_changelog_en(
      to_shared(available ? state.pending_update_changelog_en : ""));
  app.set_update_summary_zh_cn(to_shared(
      available ? linux_update_summary(state.pending_update_changelog_zh_cn) : ""));
  app.set_update_summary_en(to_shared(
      available ? linux_update_summary(state.pending_update_changelog_en) : ""));
  const auto last = linux_update_check_time(state.last_successful_update_check_at);
  app.set_update_last_successful_check(
      to_shared(last.empty() ? (english ? "Never" : "从未") : last));
  app.set_update_status(
      to_shared(english ? state.update_status_en : state.update_status_zh));
}

void sync_linux_update_history(
    const std::shared_ptr<slint::VectorModel<UpdateHistoryRow>>& rows,
    const awj::update::Manifest& manifest) {
  if (!rows) return;
  auto merged_history = awj::ui::embedded_changelog_history();
  for (const auto& entry : manifest.entries) {
    const auto version = awj::update::to_string(entry.version);
    const auto signed_entry = awj::ui::ChangelogHistoryEntry{
        .version = version,
        .channel = std::string{awj::update::channel_name(entry.channel)},
        .published_at = entry.published_at,
        .release_url = entry.release_url,
        .changelog_zh_cn = entry.changelog.zh_cn,
        .changelog_en = entry.changelog.en};
    const auto existing = std::ranges::find_if(
        merged_history, [&](const auto& item) { return item.version == version; });
    if (existing == merged_history.end()) {
      merged_history.push_back(signed_entry);
    } else {
      *existing = signed_entry;
    }
  }
  std::ranges::sort(merged_history, [](const auto& lhs, const auto& rhs) {
    const auto left = awj::update::parse_version(lhs.version);
    const auto right = awj::update::parse_version(rhs.version);
    return left && right ? *left > *right : lhs.version > rhs.version;
  });
  std::vector<UpdateHistoryRow> history_rows;
  history_rows.reserve(merged_history.size());
  for (const auto& entry : merged_history) {
    history_rows.push_back(UpdateHistoryRow{
        .version = to_shared(entry.version),
        .channel = to_shared(entry.channel),
        .published_at = to_shared(entry.published_at),
        .release_url = to_shared(entry.release_url),
        .changelog_zh_cn = to_shared(entry.changelog_zh_cn),
        .changelog_en = to_shared(entry.changelog_en)});
  }
  rows->set_vector(std::move(history_rows));
}

void restore_cached_linux_update_history(LinuxUiState& state) {
  if (state.update_manifest_v2_raw.empty() ||
      state.update_manifest_v2_signature.empty() ||
      state.update_keyring_raw.empty() || state.update_keyring_signature.empty()) {
    return;
  }
  auto keyring = awj::update::verify_and_parse_update_keyring(
      state.update_keyring_raw, state.update_keyring_signature);
  if (!keyring) {
    state.update_manifest_v2_raw.clear();
    state.update_manifest_v2_signature.clear();
    state.update_keyring_raw.clear();
    state.update_keyring_signature.clear();
    return;
  }
  auto manifest = awj::update::verify_and_parse_archive_manifest_v2(
      state.update_manifest_v2_raw, state.update_manifest_v2_signature, *keyring);
  if (!manifest ||
      manifest->sequence <
          static_cast<std::uint64_t>(std::max<std::int64_t>(
              state.last_verified_manifest_v2_sequence, 0)) ||
      manifest->sequence >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    state.update_manifest_v2_raw.clear();
    state.update_manifest_v2_signature.clear();
    state.update_keyring_raw.clear();
    state.update_keyring_signature.clear();
    return;
  }
  state.last_verified_manifest_v2_sequence =
      std::max(state.last_verified_manifest_v2_sequence,
               static_cast<std::int64_t>(manifest->sequence));
  sync_linux_update_history(
      state.update_history_rows,
      awj::update::archive_manifest_v2_for_history(*manifest));
}

std::expected<void, std::string> atomic_write_linux_file(
    const fs::path& path, std::string_view bytes) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto temporary = path;
  temporary += std::format(".tmp-{}-{}", static_cast<long long>(::getpid()),
                           stamp);
  const int descriptor = ::open(temporary.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return std::unexpected{std::format("无法创建配置临时文件：{}",
                                       std::strerror(errno))};
  }
  bool ok = true;
  std::string error{};
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto count = ::write(descriptor, bytes.data() + written,
                               bytes.size() - written);
    if (count < 0) {
      if (errno == EINTR) continue;
      ok = false;
      error = std::format("写入配置失败：{}", std::strerror(errno));
      break;
    }
    if (count == 0) {
      ok = false;
      error = "写入配置失败：未写入任何字节";
      break;
    }
    written += static_cast<std::size_t>(count);
  }
  if (ok && ::fsync(descriptor) != 0) {
    ok = false;
    error = std::format("刷新配置失败：{}", std::strerror(errno));
  }
  if (::close(descriptor) != 0 && ok) {
    ok = false;
    error = std::format("关闭配置失败：{}", std::strerror(errno));
  }
  if (!ok) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return std::unexpected{std::move(error)};
  }
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    const auto message = std::format("原子替换配置失败：{}", std::strerror(errno));
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return std::unexpected{message};
  }
  const int directory = ::open(path.parent_path().c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) {
    return std::unexpected{std::format("无法打开配置目录进行刷新：{}",
                                       std::strerror(errno))};
  }
  const int sync_result = ::fsync(directory);
  const auto sync_error = errno;
  ::close(directory);
  if (sync_result != 0) {
    return std::unexpected{std::format("刷新配置目录失败：{}",
                                       std::strerror(sync_error))};
  }
  return {};
}

std::int64_t linux_config_int64(const nlohmann::ordered_json& document,
                                std::string_view key) {
  const auto it = document.find(std::string{key});
  if (it == document.end()) return 0;
  try {
    if (it->is_number_unsigned()) {
      const auto value = it->get<std::uint64_t>();
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())
                 ? static_cast<std::int64_t>(value)
                 : 0;
    }
    if (it->is_number_integer()) {
      const auto value = it->get<std::int64_t>();
      return value >= 0 ? value : 0;
    }
    return 0;
  } catch (const nlohmann::json::exception&) {
    return 0;
  }
}

std::string linux_config_string(const nlohmann::ordered_json& document,
                                std::string_view key) {
  const auto it = document.find(std::string{key});
  return it != document.end() && it->is_string() ? it->get<std::string>()
                                                 : std::string{};
}

void load_linux_update_config(AwjStudio& app, LinuxUiState& state) {
  auto executable = awj::executable_path();
  if (!executable) {
    state.config_readable = false;
    state.update_status_zh = std::format("无法定位 AWJ.jsonc：{}", executable.error());
    state.update_status_en = "AWJ.jsonc could not be located.";
    return;
  }
  state.config_path = executable->parent_path() / "AWJ.jsonc";
  std::error_code ec;
  if (!fs::exists(state.config_path, ec)) return;
  try {
    std::ifstream input{state.config_path, std::ios::binary};
    if (!input) throw std::runtime_error{"无法读取配置文件"};
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    state.config_document = nlohmann::ordered_json::parse(
        bytes.begin(), bytes.end(), nullptr, true, true);
    if (!state.config_document.is_object()) {
      throw std::runtime_error{"配置根值不是对象"};
    }
  } catch (const std::exception& error) {
    state.config_readable = false;
    state.update_status_zh = std::format("AWJ.jsonc 无法安全读取：{}", error.what());
    state.update_status_en = "AWJ.jsonc could not be read safely.";
    return;
  }
  const auto apply_int = [&](std::string_view key, int minimum, int maximum,
                             auto setter) {
    const auto it = state.config_document.find(std::string{key});
    if (it == state.config_document.end() ||
        !(it->is_number_integer() || it->is_number_unsigned())) {
      return;
    }
    try {
      const auto value = it->get<long long>();
      if (value >= minimum && value <= maximum) {
        (app.*setter)(static_cast<int>(value));
      }
    } catch (const nlohmann::json::exception&) {
    }
  };
  const auto apply_bool = [&](std::string_view key, auto setter) {
    const auto it = state.config_document.find(std::string{key});
    if (it != state.config_document.end() && it->is_boolean()) {
      (app.*setter)(it->get<bool>());
    }
  };
  const auto font = state.config_document.find("ui_font_family");
  if (font != state.config_document.end() && font->is_string()) {
    app.set_ui_font_family(to_shared(font->get<std::string>()));
  }
  apply_int("theme_index", 0, 2, &AwjStudio::set_theme_index);
  apply_int("language_index", 0, 1, &AwjStudio::set_language_index);
  apply_bool("allow_wic_fallback", &AwjStudio::set_allow_wic_fallback);
  apply_bool("visual_quality_gpu", &AwjStudio::set_visual_quality_gpu);
  apply_bool("visual_quality_fallback", &AwjStudio::set_visual_quality_fallback);
  apply_bool("experimental_encoders", &AwjStudio::set_experimental_encoders);
  try {
    static_cast<void>(slint::select_bundled_translation(
        app.get_language_index() == 1 ? "en" : "zh-CN"));
  } catch (...) {
  }
  const auto channel = linux_config_string(state.config_document, "update_channel");
  if (channel == "stable" || channel == "prerelease") {
    state.update_channel = channel;
  }
  if (const auto it = state.config_document.find("show_update_changelog");
      it != state.config_document.end() && it->is_boolean()) {
    state.show_update_changelog = it->get<bool>();
  }
  if (const auto it = state.config_document.find("hide_update_changelog_after_exit");
      it != state.config_document.end() && it->is_boolean()) {
    state.hide_update_changelog_after_exit = it->get<bool>();
  }
  if (const auto it = state.config_document.find("show_update_changelog_after_update");
      it != state.config_document.end() && it->is_boolean()) {
    state.show_update_changelog_after_update = it->get<bool>();
  }
  state.last_changelog_exit_version =
      linux_config_string(state.config_document, "last_changelog_exit_version");
  state.last_successful_update_check_at =
      linux_config_int64(state.config_document, "last_successful_update_check_at");
  state.last_verified_manifest_sequence =
      linux_config_int64(state.config_document, "last_verified_manifest_sequence");
  state.last_verified_manifest_v2_sequence = linux_config_int64(
      state.config_document, "last_verified_manifest_v2_sequence");
  state.pending_update_version =
      linux_config_string(state.config_document, "pending_update_version");
  state.pending_update_channel =
      linux_config_string(state.config_document, "pending_update_channel");
  state.pending_update_release_url =
      linux_config_string(state.config_document, "pending_update_release_url");
  state.pending_update_published_at =
      linux_config_string(state.config_document, "pending_update_published_at");
  state.pending_update_changelog_zh_cn =
      linux_config_string(state.config_document, "pending_update_changelog_zh_cn");
  state.pending_update_changelog_en =
      linux_config_string(state.config_document, "pending_update_changelog_en");
  state.update_manifest_raw =
      linux_config_string(state.config_document, "update_manifest_raw");
  state.update_manifest_signature =
      linux_config_string(state.config_document, "update_manifest_signature");
  state.update_manifest_v2_raw =
      linux_config_string(state.config_document, "update_manifest_v2_raw");
  state.update_manifest_v2_signature = linux_config_string(
      state.config_document, "update_manifest_v2_signature");
  state.update_keyring_raw =
      linux_config_string(state.config_document, "update_keyring_raw");
  state.update_keyring_signature =
      linux_config_string(state.config_document, "update_keyring_signature");
  load_linux_menu_params(state.config_document, state.menu_params);
}

std::expected<void, std::string> persist_linux_update_config(
    const AwjStudio& app, LinuxUiState& state) {
  if (!state.config_readable || state.config_path.empty()) {
    return std::unexpected{"AWJ.jsonc 当前不可安全写入。"};
  }
  // 根配置是白名单写入：迁移时故意不复制旧文档，以清理普通队列、路径、
  // 窗口状态以及曾经持久化的参数页数据。
  nlohmann::ordered_json document = nlohmann::ordered_json::object();
  document["theme_index"] = app.get_theme_index();
  document["language_index"] = app.get_language_index();
  document["ui_font_family"] = shared_to_string(app.get_ui_font_family());
  document["allow_wic_fallback"] = app.get_allow_wic_fallback();
  document["visual_quality_gpu"] = app.get_visual_quality_gpu();
  document["visual_quality_fallback"] = app.get_visual_quality_fallback();
  document["experimental_encoders"] = app.get_experimental_encoders();
  document["update_channel"] = state.update_channel;
  document["show_update_changelog"] = state.show_update_changelog;
  document["hide_update_changelog_after_exit"] =
      state.hide_update_changelog_after_exit;
  document["show_update_changelog_after_update"] =
      state.show_update_changelog_after_update;
  document["last_changelog_exit_version"] = state.last_changelog_exit_version;
  document["last_successful_update_check_at"] =
      state.last_successful_update_check_at;
  document["last_verified_manifest_sequence"] =
      state.last_verified_manifest_sequence;
  document["last_verified_manifest_v2_sequence"] =
      state.last_verified_manifest_v2_sequence;
  document["pending_update_version"] = state.pending_update_version;
  document["pending_update_channel"] = state.pending_update_channel;
  document["pending_update_release_url"] = state.pending_update_release_url;
  document["pending_update_published_at"] = state.pending_update_published_at;
  document["pending_update_changelog_zh_cn"] =
      state.pending_update_changelog_zh_cn;
  document["pending_update_changelog_en"] = state.pending_update_changelog_en;
  document["update_manifest_raw"] = state.update_manifest_raw;
  document["update_manifest_signature"] = state.update_manifest_signature;
  document["update_manifest_v2_raw"] = state.update_manifest_v2_raw;
  document["update_manifest_v2_signature"] = state.update_manifest_v2_signature;
  document["update_keyring_raw"] = state.update_keyring_raw;
  document["update_keyring_signature"] = state.update_keyring_signature;
  document["menu_params"] = nlohmann::ordered_json::array();
  for (const auto& params : state.menu_params) {
    document["menu_params"].push_back(linux_menu_params_json(params));
  }
  auto bytes = document.dump(2);
  bytes.push_back('\n');
  auto saved = atomic_write_linux_file(state.config_path, bytes);
  if (!saved) return saved;
  state.config_document = std::move(document);
  return {};
}

awj::update::ChannelPreference linux_update_preference(
    const LinuxUiState& state) {
  return state.update_channel == "prerelease"
             ? awj::update::ChannelPreference::stable_and_prerelease
             : awj::update::ChannelPreference::stable_only;
}

std::expected<void, std::string> open_linux_url(std::string url) {
  if (!awj::update::parse_allowed_https_url(url)) {
    return std::unexpected{"拒绝打开不受信任的更新 URL。"};
  }
  const auto spawn = [&](const char* command, std::vector<char*> arguments)
      -> std::optional<pid_t> {
    pid_t child = 0;
    arguments.push_back(nullptr);
    if (::posix_spawnp(&child, command, nullptr, nullptr, arguments.data(),
                       environ) != 0) {
      return std::nullopt;
    }
    return child;
  };
  std::string xdg{"xdg-open"};
  if (auto child = spawn(xdg.c_str(), {xdg.data(), url.data()})) {
    std::thread{[pid = *child] { ::waitpid(pid, nullptr, 0); }}.detach();
    return {};
  }
  std::string gio{"gio"};
  std::string open{"open"};
  if (auto child = spawn(gio.c_str(), {gio.data(), open.data(), url.data()})) {
    std::thread{[pid = *child] { ::waitpid(pid, nullptr, 0); }}.detach();
    return {};
  }
  return std::unexpected{"未找到可用的 URL 打开工具（xdg-open/gio）。"};
}

void start_linux_update_check(slint::ComponentWeakHandle<AwjStudio> weak,
                              const std::shared_ptr<LinuxUiState>& state) {
  if (state->update_check_active) return;
  if (state->update_worker.joinable()) state->update_worker.join();
  state->update_check_active = true;
  if (auto app = weak.lock()) {
    (*app)->set_update_checking(true);
    state->update_status_zh = "正在检查更新…";
    state->update_status_en = "Checking for updates...";
    sync_linux_update_ui(**app, *state);
  }
  const auto last_sequence = state->last_verified_manifest_v2_sequence < 0
                                 ? std::uint64_t{0}
                                 : static_cast<std::uint64_t>(
                                       state->last_verified_manifest_v2_sequence);
  const auto preference = linux_update_preference(*state);
  state->update_worker = std::jthread(
      [weak, state, last_sequence, preference](std::stop_token token) {
        auto fetched = awj::update::fetch_verified_archive_manifest_v2(
            last_sequence, token);
        static_cast<void>(slint::invoke_from_event_loop(
            [weak, state, preference, fetched = std::move(fetched)]() mutable {
              auto app = weak.lock();
              if (!app) return;
              state->update_check_active = false;
              (*app)->set_update_checking(false);
              if (!fetched) {
                state->update_status_zh =
                    std::format("检查失败：{}", fetched.error());
                state->update_status_en = "Update check failed.";
                sync_linux_update_ui(**app, *state);
                return;
              }
              if (fetched->manifest.sequence >
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
                state->update_status_zh =
                    "检查失败：manifest sequence 超出本机范围。";
                state->update_status_en =
                    "Update check failed: sequence is out of range.";
                sync_linux_update_ui(**app, *state);
                return;
              }
              const auto before = capture_linux_update_state(*state);
              state->update_manifest_v2_raw = fetched->raw_bytes;
              state->update_manifest_v2_signature = fetched->signature_base64;
              state->update_keyring_raw = fetched->keyring_raw_bytes;
              state->update_keyring_signature =
                  fetched->keyring_signature_envelope;
              if (const auto pending =
                      awj::update::parse_version(state->pending_update_version);
                  pending && awj::update::should_clear_pending_for_revocation(
                                 awj::update::archive_manifest_v2_for_history(
                                     fetched->manifest),
                                 *pending)) {
                clear_linux_pending_update(*state);
              }
              const auto current = awj::update::parse_version(AWJ_BUILD_VERSION);
              if (!current) {
                restore_linux_update_state(*state, before);
                state->update_status_zh = "检查失败：当前构建版本号非法。";
                state->update_status_en =
                    "Update check failed: invalid build version.";
                sync_linux_update_ui(**app, *state);
                return;
              }
              const auto candidate = awj::update::select_archive_candidate_v2(
                  fetched->manifest,
                  {.current_version = *current,
                   .updater_version = *current,
                   .preference = preference});
              if (candidate) {
                state->pending_update_version =
                    awj::update::to_string(candidate->version);
                state->pending_update_channel =
                    std::string{awj::update::channel_name(candidate->channel)};
                state->pending_update_release_url = candidate->release_url;
                state->pending_update_published_at = candidate->published_at;
                state->pending_update_changelog_zh_cn = candidate->changelog.zh_cn;
                state->pending_update_changelog_en = candidate->changelog.en;
              }
              state->last_successful_update_check_at =
                  std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
              state->last_verified_manifest_v2_sequence =
                  static_cast<std::int64_t>(fetched->manifest.sequence);
              const bool retained =
                  !candidate && linux_pending_update_is_newer(*state);
              state->update_status_zh =
                  candidate ? "发现可用更新。"
                            : retained ? "检查成功；保留之前发现的更新。"
                                       : "已是最新版。";
              state->update_status_en =
                  candidate ? "An update is available."
                            : retained
                                  ? "Check succeeded; the previously found update remains available."
                                  : "Up to date.";
              if (auto saved = persist_linux_update_config(**app, *state); !saved) {
                restore_linux_update_state(*state, before);
                state->update_status_zh =
                    std::format("检查失败：无法持久化状态：{}", saved.error());
                state->update_status_en =
                    "Update check failed: state could not be saved.";
              } else {
                sync_linux_update_history(
                    state->update_history_rows,
                    awj::update::archive_manifest_v2_for_history(
                        fetched->manifest));
              }
              sync_linux_update_ui(**app, *state);
            }));
      });
}

std::wstring wide_from_shared(const slint::SharedString& value) {
  return awj::wide_from_utf8(shared_to_string(value));
}

LinuxMenuParams capture_linux_menu_params(const AwjStudio& app) {
  return LinuxMenuParams{
      .quality_text = shared_to_string(app.get_menu_quality_text()),
      .bit_depth_text = shared_to_string(app.get_menu_bit_depth_text()),
      .speed_text = shared_to_string(app.get_menu_speed_text()),
      .avif_encoder_index = app.get_menu_avif_encoder_index(),
      .avif_color_representation_index =
          app.get_menu_avif_color_representation_index(),
      .chroma_index = app.get_menu_chroma_index(),
      .alpha_policy_index = app.get_menu_alpha_policy_index(),
      .jpegli_progressive_index = app.get_menu_jpegli_progressive_index(),
      .jpegli_optimize_huffman = app.get_menu_jpegli_optimize_huffman(),
      .jpegli_xyb = app.get_menu_jpegli_xyb(),
      .strip_metadata = app.get_menu_strip_metadata(),
      .install_avif_png_command = app.get_menu_install_avif_png_command(),
      .size_limit_index = app.get_menu_size_limit_index(),
      .max_width_text = shared_to_string(app.get_menu_max_width_text()),
      .max_height_text = shared_to_string(app.get_menu_max_height_text()),
      .max_long_edge_text = shared_to_string(app.get_menu_max_long_edge_text()),
      .max_short_edge_text = shared_to_string(app.get_menu_max_short_edge_text())};
}

void apply_linux_menu_params(AwjStudio& app, const LinuxMenuParams& params) {
  app.set_menu_quality_text(to_shared(params.quality_text));
  app.set_menu_bit_depth_text(to_shared(params.bit_depth_text));
  app.set_menu_speed_text(to_shared(params.speed_text));
  app.set_menu_avif_encoder_index(params.avif_encoder_index);
  app.set_menu_avif_color_representation_index(
      params.avif_color_representation_index);
  app.set_menu_chroma_index(params.chroma_index);
  app.set_menu_alpha_policy_index(params.alpha_policy_index);
  app.set_menu_jpegli_progressive_index(params.jpegli_progressive_index);
  app.set_menu_jpegli_optimize_huffman(params.jpegli_optimize_huffman);
  app.set_menu_jpegli_xyb(params.jpegli_xyb);
  app.set_menu_strip_metadata(params.strip_metadata);
  app.set_menu_install_avif_png_command(params.install_avif_png_command);
  app.set_menu_size_limit_index(params.size_limit_index);
  app.set_menu_max_width_text(to_shared(params.max_width_text));
  app.set_menu_max_height_text(to_shared(params.max_height_text));
  app.set_menu_max_long_edge_text(to_shared(params.max_long_edge_text));
  app.set_menu_max_short_edge_text(to_shared(params.max_short_edge_text));
}

void store_linux_menu_params(AwjStudio& app, LinuxUiState& state) {
  state.menu_params[static_cast<std::size_t>(std::clamp(state.menu_format_index, 0, 4))] =
      capture_linux_menu_params(app);
}

LinuxMenuParams default_linux_menu_params(int format_index) {
  LinuxMenuParams params{};
  const auto format = [format_index] {
    switch (format_index) {
      case 1: return awj::OutputFormat::webp;
      case 2: return awj::OutputFormat::jxl;
      case 3: return awj::OutputFormat::jpgli;
      case 4: return awj::OutputFormat::png;
      default: return awj::OutputFormat::avif;
    }
  }();
  params.quality_text = std::format("{}", awj::default_quality_for(format));
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    params.speed_text = std::format("{}", awj::default_speed_for(format));
  }
  if (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli) {
    params.bit_depth_text = std::format("{}", awj::encoding_defaults::default_webp_bit_depth);
  }
  params.jpegli_progressive_index = awj::encoding_defaults::default_jpegli_progressive_level;
  params.jpegli_optimize_huffman = awj::encoding_defaults::default_jpegli_optimize_huffman;
  params.jpegli_xyb = awj::encoding_defaults::default_jpegli_xyb;
  return params;
}


std::string stage_seconds_text(double seconds) {
  return seconds < 0.0 ? std::string{"-"} : std::format("{:.3f}s", seconds);
}

std::string stage_timings_text(double decode_seconds, double prepare_seconds,
                               double encode_seconds, double write_seconds) {
  if (decode_seconds < 0.0 && prepare_seconds < 0.0 &&
      encode_seconds < 0.0 && write_seconds < 0.0) {
    return {};
  }
  return std::format("decode {} · prepare {} · encode {} · write {}",
                     stage_seconds_text(decode_seconds),
                     stage_seconds_text(prepare_seconds),
                     stage_seconds_text(encode_seconds),
                     stage_seconds_text(write_seconds));
}

std::string linux_result_status_text(const awj::EncodeResult& result) {
  if (result.ok) {
    return result.skipped ? "已跳过" : "完成";
  }
  if (result.canceled) {
    return "已取消";
  }
  return result.message.empty() ? "失败"
                                : std::format("失败 · {}", result.message);
}

TaskRow task_row_from_result(const awj::EncodeResult& result) {
  return TaskRow{.order = to_shared(std::format("{}", result.index + 1)),
                 .filename = to_shared(awj::path_to_utf8(result.input_path.filename())),
                 .folder = to_shared(awj::path_to_utf8(result.input_path.parent_path())),
                 .size = to_shared(awj::format_size(result.original_bytes)),
                 .status = to_shared(linux_result_status_text(result)),
                 .output = to_shared(awj::path_to_utf8(result.output_path.filename())),
                 .log = to_shared(result.ok ? std::string{} : result.message),
                 .warning = !result.ok,
                 .locked = true,
                 .state = result.ok ? 2 : (result.canceled ? 4 : 3),
                 .input_path = to_shared(awj::path_to_utf8(result.input_path)),
                 .output_path = to_shared(awj::path_to_utf8(result.output_path)),
                 .encoder = to_shared(result.encoder_id),
                 .threads = result.encoder_threads > 0
                                ? to_shared(std::format("{}", result.encoder_threads))
                                : slint::SharedString{},
                 .stage_timings = to_shared(stage_timings_text(
                     result.decode_seconds, result.prepare_seconds,
                     result.encode_seconds, result.write_seconds))};
}

std::optional<std::size_t> linux_task_row_index_for_path(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const fs::path& path) {
  if (!rows) {
    return std::nullopt;
  }
  const auto expected = awj::path_to_utf8(path);
  for (std::size_t index = 0; index < rows->row_count(); ++index) {
    if (auto row = rows->row_data(index);
        row && shared_to_string(row->input_path) == expected) {
      return index;
    }
  }
  return std::nullopt;
}

void refresh_linux_queue_counts(
    AwjStudio& app,
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows) {
  int pending = 0;
  int running = 0;
  int success = 0;
  int failed = 0;
  if (rows) {
    for (std::size_t index = 0; index < rows->row_count(); ++index) {
      const auto row = rows->row_data(index);
      if (!row) {
        continue;
      }
      switch (row->state) {
        case 1:
          ++running;
          break;
        case 2:
          ++success;
          break;
        case 3:
          ++failed;
          break;
        case 0:
        case 4:
        default:
          ++pending;
          break;
      }
    }
  }
  app.set_queue_pending_count(pending);
  app.set_queue_running_count(running);
  app.set_queue_success_count(success);
  app.set_queue_failed_count(failed);
  if (!rows || app.get_selected_queue_index() >=
                   static_cast<int>(rows->row_count())) {
    app.set_selected_queue_index(-1);
  }
}

void push_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                   TaskRow row) noexcept {
  if (!rows) return;
  try {
    rows->push_back(std::move(row));
  } catch (...) {
  }
}

void mark_task_row_running(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::EncodeResult& result) noexcept {
  if (!rows) return;
  try {
    if (const auto index = linux_task_row_index_for_path(rows, result.input_path)) {
      auto row = rows->row_data(*index);
      if (row) {
        row->status = to_shared("正在转码");
        row->locked = true;
        row->state = 1;
        rows->set_row_data(*index, *row);
        return;
      }
    }
    push_task_row(rows,
                  TaskRow{.order = to_shared(std::format("{}", result.index + 1)),
                          .filename = to_shared(awj::path_to_utf8(
                              result.input_path.filename())),
                          .folder = to_shared(awj::path_to_utf8(
                              result.input_path.parent_path())),
                          .size = to_shared(awj::format_size(result.original_bytes)),
                          .status = to_shared("正在转码"),
                           .output = to_shared(awj::path_to_utf8(
                               result.output_path.filename())),
                           .locked = true,
                           .state = 1,
                           .input_path = to_shared(awj::path_to_utf8(result.input_path)),
                           .output_path = to_shared(awj::path_to_utf8(result.output_path))});
  } catch (...) {
  }
}

void set_linux_task_row_result(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::EncodeResult& result) {
  auto row = task_row_from_result(result);
  if (const auto index = linux_task_row_index_for_path(rows, result.input_path)) {
    rows->set_row_data(*index, row);
  } else {
    push_task_row(rows, std::move(row));
  }
}

void add_large_image_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                              const awj::BatchLargeImageItem& item) noexcept {
  push_task_row(rows, TaskRow{.order = to_shared(std::format("{}", item.file.index + 1)),
                              .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
                              .folder = to_shared(awj::path_to_utf8(item.file.path.parent_path())),
                              .size = to_shared(awj::format_size(item.file.bytes)),
                              .status = to_shared("大图模式"),
                              .output = {},
                              .log = to_shared(std::format("{}；{}", awj::large_image_reason_name(item.decision.reason), item.decision.reason_text)),
                              .warning = false,
                              .locked = true});
}


bool linux_large_image_grid_available(const awj::BatchLargeImageItem& item) noexcept {
  if (!item.decision.available_grid) return false;
  // 与 Windows 分支同因：clamped 边缘 cell 是 pipeline 已支持的默认路径。
  const auto plan = awj::plan_grid(awj::GridPlanRequest{
      .width = item.dimensions.width,
      .height = item.dimensions.height,
      .mode = awj::GridMode::auto_grid,
      .clamped_padding_enabled =
          awj::encoding_defaults::default_experimental_clamped_grid_padding});
  return plan.has_value();
}

bool linux_large_image_zenrav1e_available(const awj::BatchLargeImageItem& item) noexcept {
  return item.decision.available_zenrav1e &&
         item.dimensions.width <= awj::encoding_defaults::avif_single_image_max_dimension &&
         item.dimensions.height <= awj::encoding_defaults::avif_single_image_max_dimension;
}

LargeImageRow make_linux_large_image_row(const awj::BatchLargeImageItem& item,
                                         std::string_view status) {
  const auto grid = linux_large_image_grid_available(item) ? "grid 可用" : "grid 不可用";
  const auto zen = linux_large_image_zenrav1e_available(item)
                       ? "zenrav1e 可用"
                       : (item.decision.available_zenrav1e ? "zenrav1e 超出边长" : "zenrav1e 不可用");
  return LargeImageRow{
      .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
      .dimensions = to_shared(std::format("{} x {}", item.dimensions.width, item.dimensions.height)),
      .reason = to_shared(std::format("{} · {}", awj::large_image_reason_name(item.decision.reason),
                                      item.decision.reason_text)),
      .actions = to_shared(std::format("{} / {}", grid, zen)),
      .status = to_shared(status),
      .grid_available = linux_large_image_grid_available(item),
      .zenrav1e_available = linux_large_image_zenrav1e_available(item)};
}

void set_linux_large_image_status(LinuxUiState& state, int index, std::string_view status) {
  if (!state.large_image_rows || index < 0 ||
      static_cast<std::size_t>(index) >= state.large_image_items.size()) {
    return;
  }
  try {
    state.large_image_rows->set_row_data(
        static_cast<std::size_t>(index),
        make_linux_large_image_row(state.large_image_items[static_cast<std::size_t>(index)], status));
  } catch (...) {
  }
}

void push_linux_large_image(LinuxUiState& state, awj::BatchLargeImageItem item) {
  if (!state.large_image_rows) return;
  try {
    auto row = make_linux_large_image_row(item, "等待选择");
    state.large_image_items.push_back(std::move(item));
    state.large_image_rows->push_back(std::move(row));
  } catch (...) {
  }
}
void set_input_path_preserving_output(AwjStudio& app, const fs::path& path) {
  app.set_input_path(to_shared(awj::path_to_utf8(path)));
  if (shared_to_string(app.get_output_dir()).empty()) {
    app.set_output_dir(to_shared(awj::path_to_utf8(awj::default_output_dir_for(path))));
  }
}

std::wstring linux_queue_path_key(const fs::path& path) {
  std::error_code ec;
  const auto absolute = fs::absolute(path, ec);
  return awj::normalized_lower_path_key(ec ? path : absolute);
}

bool linux_queue_contains_path(const LinuxUiState& state, const fs::path& path) {
  const auto key = linux_queue_path_key(path);
  return std::ranges::any_of(state.queue_files, [&](const awj::ImageFile& file) {
    return linux_queue_path_key(file.path) == key;
  });
}

TaskRow pending_linux_queue_row(const awj::ImageFile& file, std::size_t index,
                                bool locked = false) {
  return TaskRow{
      .order = to_shared(std::format("{}", index + 1)),
      .filename = to_shared(awj::path_to_utf8(file.path.filename())),
      .folder = to_shared(awj::path_to_utf8(file.path.parent_path())),
      .size = to_shared(awj::format_size(file.bytes)),
      .status = to_shared("等待编码"),
      .output = file.resolved_output_path.empty()
                    ? slint::SharedString{}
                    : to_shared(awj::path_to_utf8(
                          file.resolved_output_path.filename())),
      .locked = locked,
      .state = 0,
      .input_path = to_shared(awj::path_to_utf8(file.path)),
      .output_path = to_shared(awj::path_to_utf8(file.resolved_output_path))};
}

void refresh_linux_pending_queue(AwjStudio& app, LinuxUiState& state,
                                 bool locked = false) {
  std::vector<TaskRow> rows;
  rows.reserve(state.queue_files.size());
  for (std::size_t index = 0; index < state.queue_files.size(); ++index) {
    rows.push_back(pending_linux_queue_row(state.queue_files[index], index,
                                           locked));
  }
  state.task_rows->set_vector(std::move(rows));
  refresh_linux_queue_counts(app, state.task_rows);
}

std::expected<bool, std::string> add_linux_queue_from_path(
    AwjStudio& app, LinuxUiState& state, const fs::path& path,
    bool update_input_path) {
  auto scan_cfg = awj::default_app_config();
  scan_cfg.input_path = path;
  scan_cfg.output_dir = fs::path{shared_to_string(app.get_output_dir())};
  scan_cfg.output_policy = awj::OutputPolicy::normal;
  std::vector<awj::ImageFile> scanned;
  if (auto result = awj::scan_images(scan_cfg, scanned); !result) {
    return std::unexpected{result.error()};
  }
  if (scanned.empty()) {
    return false;
  }

  std::size_t added = 0;
  for (auto& file : scanned) {
    std::error_code ec;
    const auto absolute = fs::absolute(file.path, ec);
    if (!ec) {
      file.path = absolute;
    }
    if (linux_queue_contains_path(state, file.path)) {
      continue;
    }
    file.index = state.queue_files.size();
    file.source_extension_disambiguator.clear();
    file.extension_disambiguated = false;
    file.resolved_output_path.clear();
    file.output_path_resolved = false;
    state.queue_files.push_back(std::move(file));
    ++added;
  }
  if (update_input_path) {
    set_input_path_preserving_output(app, path);
  }
  refresh_linux_pending_queue(app, state);
  app.set_status_text(to_shared(
      added == 0 ? "没有新图片加入队列，重复项已跳过。"
                 : std::format("已加入 {} 张图片。", added)));
  return true;
}

std::expected<std::vector<awj::ImageFile>, std::string>
build_linux_queue_files(const awj::AppConfig& cfg,
                        const std::vector<awj::ImageFile>& queue,
                        const std::vector<fs::path>* only_paths = nullptr) {
  try {
    std::unordered_set<std::wstring> selected;
    if (only_paths != nullptr) {
      selected.reserve(only_paths->size());
      for (const auto& path : *only_paths) {
        selected.insert(linux_queue_path_key(path));
      }
    }
    std::vector<awj::ImageFile> files;
    files.reserve(queue.size());
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    const auto template_text = std::wstring_view{cfg.output_template};
    const bool needs_hash = template_text.find(L"{hash}") != std::wstring_view::npos ||
                            template_text.find(L"{hash8}") != std::wstring_view::npos;
    const bool needs_sha256 = template_text.find(L"{sha256}") != std::wstring_view::npos ||
                              template_text.find(L"{sha2568}") != std::wstring_view::npos ||
                              template_text.find(L"{sha256_8}") != std::wstring_view::npos;
    for (const auto& item : queue) {
      if (only_paths != nullptr &&
          !selected.contains(linux_queue_path_key(item.path))) {
        continue;
      }
      std::wstring hash;
      if (needs_hash) {
        if (auto result = awj::file_hash_token(item.path, hash); !result) {
          return std::unexpected{result.error()};
        }
      }
      std::wstring sha256;
      if (needs_sha256) {
        if (auto result = awj::file_sha256_token(item.path, sha256); !result) {
          return std::unexpected{result.error()};
        }
      }
      files.push_back(awj::make_image_file(files.size(), item.path,
                                           item.relative_dir, item.bytes, rng,
                                           std::move(hash), std::move(sha256)));
    }
    if (files.empty()) {
      return std::unexpected{"队列为空，或没有可重试的项目。"};
    }
    if (auto result = awj::apply_source_extension_disambiguation(cfg, files);
        !result) {
      return std::unexpected{result.error()};
    }
    if (auto result = awj::resolve_batch_output_paths(cfg, files); !result) {
      return std::unexpected{result.error()};
    }
    return files;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"构建队列运行快照时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"构建队列运行快照时数据超过运行时限制。"};
  } catch (const fs::filesystem_error&) {
    return std::unexpected{"构建队列运行快照时文件系统访问失败。"};
  }
}

std::expected<fs::path, std::string> create_linux_queue_manifest(
    std::uint64_t run_id, const std::vector<awj::ImageFile>& files) {
  try {
    std::error_code ec;
    const auto temp_dir = fs::temp_directory_path(ec);
    if (ec) {
      return std::unexpected{std::format("无法获取 Studio worker 临时目录：{}",
                                         ec.message())};
    }
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    for (int attempt = 0; attempt < 16; ++attempt) {
      const auto path = temp_dir / std::format(
          "AWJStudioQueue-{}-{}-{:016x}.awjq", static_cast<long long>(getpid()),
          run_id, rng());
      if (fs::exists(path, ec)) {
        ec.clear();
        continue;
      }
      if (auto written = awj::write_studio_queue_manifest(path, files);
          written) {
        return path;
      } else {
        fs::remove(path, ec);
        return std::unexpected{written.error()};
      }
    }
    return std::unexpected{"无法创建唯一的 Studio 队列 manifest。"};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"创建 Studio 队列 manifest 时内存不足。"};
  } catch (const fs::filesystem_error&) {
    return std::unexpected{"创建 Studio 队列 manifest 时文件系统访问失败。"};
  }
}

struct LinuxQueueDragPayload {
  std::wstring path_key{};
};

slint::DataTransfer make_linux_queue_drag_data(const LinuxUiState& state,
                                               int index) {
  slint::DataTransfer transfer;
  if (index < 0 || static_cast<std::size_t>(index) >= state.queue_files.size()) {
    return transfer;
  }
  transfer.set_user_data(
      LinuxQueueDragPayload{linux_queue_path_key(
          state.queue_files[static_cast<std::size_t>(index)].path)});
  return transfer;
}

std::optional<std::size_t> linux_queue_index_for_key(
    const LinuxUiState& state, std::wstring_view key) {
  for (std::size_t index = 0; index < state.queue_files.size(); ++index) {
    if (linux_queue_path_key(state.queue_files[index].path) == key) {
      return index;
    }
  }
  return std::nullopt;
}

slint::language::DragAction linux_queue_drag_can_drop(
    const LinuxUiState& state, slint::language::DropEvent event,
    int target_slot, bool running) {
  if (running) {
    return slint::language::DragAction::None;
  }
  const auto data = event.data.user_data();
  const auto* payload = std::any_cast<LinuxQueueDragPayload>(&data);
  if (payload == nullptr) {
    const auto text = event.data.plain_text();
    return text && !native_drop_paths(*text).empty()
               ? slint::language::DragAction::Copy
               : slint::language::DragAction::None;
  }
  const auto current = linux_queue_index_for_key(state, payload->path_key);
  if (!current || target_slot < 0 ||
      static_cast<std::size_t>(target_slot) > state.queue_files.size()) {
    return slint::language::DragAction::None;
  }
  auto target = static_cast<std::size_t>(target_slot);
  if (target > *current) {
    --target;
  }
  return target == *current ? slint::language::DragAction::None
                            : slint::language::DragAction::Move;
}

slint::language::DragAction linux_queue_drag_dropped(
    AwjStudio& app, LinuxUiState& state, slint::language::DropEvent event,
    int target_slot) {
  if (app.get_running()) {
    return slint::language::DragAction::None;
  }
  const auto data = event.data.user_data();
  const auto* payload = std::any_cast<LinuxQueueDragPayload>(&data);
  if (payload == nullptr) {
    const auto text = event.data.plain_text();
    if (!text) {
      return slint::language::DragAction::None;
    }
    const auto paths = native_drop_paths(*text);
    if (paths.empty()) {
      app.set_status_text(to_shared("拖入内容不包含本地文件或文件夹路径。"));
      return slint::language::DragAction::None;
    }
    std::string first_error;
    for (const auto& raw : paths) {
      const auto path = awj::normalize_path_argument(
          awj::wide_from_utf8(raw), "拖入队列");
      if (!path) {
        if (first_error.empty()) first_error = path.error();
        continue;
      }
      if (auto added = add_linux_queue_from_path(app, state, *path, false);
          !added && first_error.empty()) {
        first_error = added.error();
      }
    }
    if (!first_error.empty()) {
      app.set_status_text(to_shared(first_error));
    }
    return slint::language::DragAction::Copy;
  }
  const auto current = linux_queue_index_for_key(state, payload->path_key);
  if (!current || target_slot < 0 ||
      static_cast<std::size_t>(target_slot) > state.queue_files.size()) {
    return slint::language::DragAction::None;
  }
  auto target = static_cast<std::size_t>(target_slot);
  if (target > *current) {
    --target;
  }
  if (target == *current) {
    return slint::language::DragAction::None;
  }
  auto file = std::move(state.queue_files[*current]);
  state.queue_files.erase(state.queue_files.begin() +
                          static_cast<std::ptrdiff_t>(*current));
  state.queue_files.insert(state.queue_files.begin() +
                               static_cast<std::ptrdiff_t>(target),
                           std::move(file));
  refresh_linux_pending_queue(app, state);
  return slint::language::DragAction::Move;
}
std::wstring format_arg(int index) {
  switch (index) {
    case 1:
      return L"webp";
    case 2:
      return L"jxl";
    case 3:
      return L"jpgli";
    case 4:
      return L"png";
    case 0:
    default:
      return L"avif";
  }
}

std::wstring collision_arg(int index) {
  switch (index) {
    case 1:
      return L"skip";
    case 2:
      return L"time";
    case 3:
      return L"random";
    case 0:
    default:
      return L"overwrite";
  }
}
std::wstring avif_encoder_arg(int index) {
  switch (index) {
    case 1: return L"svt";
    case 2: return L"aom";
    case 3: return L"zenrav1e";
    default: return L"auto";
  }
}

std::wstring avif_color_representation_arg(int index) {
  switch (index) {
    case 1: return L"source";
    case 2: return L"rgb";
    default: return L"yuv";
  }
}

std::wstring chroma_arg(int index) {
  switch (index) {
    case 1: return L"444";
    case 2: return L"422";
    case 3: return L"420";
    default: return L"auto";
  }
}

std::wstring alpha_arg(int index) {
  switch (index) {
    case 0: return L"force";
    case 2: return L"off";
    default: return L"auto";
  }
}

std::wstring size_limit_arg(int index) {
  switch (index) {
    case 1: return L"none";
    case 2: return L"manual";
    default: return L"auto";
  }
}

awj::OutputFormat linux_output_format_from_index(int index) noexcept {
  switch (index) {
    case 1: return awj::OutputFormat::webp;
    case 2: return awj::OutputFormat::jxl;
    case 3: return awj::OutputFormat::jpgli;
    case 4: return awj::OutputFormat::png;
    default: return awj::OutputFormat::avif;
  }
}

struct LinuxQueueFormatChoice {
  int format_index{};
  bool append_png_suffix{};
};

constexpr LinuxQueueFormatChoice linux_queue_format_choice_from_index(
    int index) noexcept {
  const int choice = std::clamp(index, 0, 5);
  if (choice == 1) {
    return {.format_index = 0, .append_png_suffix = true};
  }
  return {.format_index = choice == 0 ? 0 : choice - 1,
          .append_png_suffix = false};
}

static_assert(linux_queue_format_choice_from_index(0).format_index == 0);
static_assert(linux_queue_format_choice_from_index(1).format_index == 0 &&
              linux_queue_format_choice_from_index(1).append_png_suffix);
static_assert(linux_queue_format_choice_from_index(2).format_index == 1);
static_assert(linux_queue_format_choice_from_index(3).format_index == 2);
static_assert(linux_queue_format_choice_from_index(4).format_index == 3);
static_assert(linux_queue_format_choice_from_index(5).format_index == 4);

int linux_parameter_editor_format_index(int index) noexcept {
  return index >= 0 && index < 5 ? index : 0;
}

int linux_avif_color_representation_index(
    awj::AvifColorRepresentation value) noexcept {
  switch (value) {
    case awj::AvifColorRepresentation::source: return 1;
    case awj::AvifColorRepresentation::rgb_identity: return 2;
    default: return 0;
  }
}

LinuxParameterParams default_linux_parameter_params(int index) {
  const auto format = linux_output_format_from_index(index);
  LinuxParameterParams params{};
  params.quality_text = std::format("{}", awj::default_quality_for(format));
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    params.speed_text = std::format("{}", awj::default_speed_for(format));
  }
  if (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli) {
    params.bit_depth_text = std::format("{}", awj::encoding_defaults::default_webp_bit_depth);
  }
  params.jpegli_progressive_index =
      awj::encoding_defaults::default_jpegli_progressive_level;
  params.jpegli_optimize_huffman =
      awj::encoding_defaults::default_jpegli_optimize_huffman;
  params.jpegli_xyb = awj::encoding_defaults::default_jpegli_xyb;
  return params;
}

LinuxParameterParams capture_linux_parameter_params(const AwjStudio& app) {
  return {.quality_text = shared_to_string(app.get_quality_text()),
          .visual_quality_text = shared_to_string(app.get_visual_quality_text()),
          .bit_depth_text = shared_to_string(app.get_bit_depth_text()),
      .speed_text = shared_to_string(app.get_speed_text()),
      .avif_encoder_index = app.get_avif_encoder_index(),
      .avif_color_representation_index =
          app.get_avif_color_representation_index(),
      .chroma_index = app.get_chroma_index(),
          .alpha_policy_index = app.get_alpha_policy_index(),
          .jpegli_progressive_index = app.get_jpegli_progressive_index(),
          .jpegli_optimize_huffman = app.get_jpegli_optimize_huffman(),
          .jpegli_xyb = app.get_jpegli_xyb(),
          .threads_text = shared_to_string(app.get_threads_text()),
          .memory_limit_text = shared_to_string(app.get_memory_limit_text()),
          .size_limit_index = app.get_size_limit_index(),
          .max_width_text = shared_to_string(app.get_max_width_text()),
          .max_height_text = shared_to_string(app.get_max_height_text()),
          .max_long_edge_text = shared_to_string(app.get_max_long_edge_text()),
          .max_short_edge_text = shared_to_string(app.get_max_short_edge_text())};
}

void apply_linux_parameter_params(AwjStudio& app,
                                  const LinuxParameterParams& params,
                                  int format_index) {
  const auto format = linux_output_format_from_index(format_index);
  const bool png_lossless = format == awj::OutputFormat::png;
  app.set_quality_text(to_shared(
      png_lossless ? std::format("{}", awj::default_quality_for(format))
                   : params.quality_text));
  app.set_visual_quality_text(
      to_shared(png_lossless ? std::string{} : params.visual_quality_text));
  app.set_bit_depth_text(to_shared(params.bit_depth_text));
  app.set_speed_text(to_shared(params.speed_text));
  app.set_avif_encoder_index(params.avif_encoder_index);
  app.set_avif_color_representation_index(
      params.avif_color_representation_index);
  app.set_chroma_index(params.chroma_index);
  app.set_alpha_policy_index(params.alpha_policy_index);
  app.set_jpegli_progressive_index(params.jpegli_progressive_index);
  app.set_jpegli_optimize_huffman(params.jpegli_optimize_huffman);
  app.set_jpegli_xyb(params.jpegli_xyb);
  app.set_threads_text(to_shared(params.threads_text));
  app.set_memory_limit_text(to_shared(params.memory_limit_text));
  app.set_size_limit_index(params.size_limit_index);
  app.set_max_width_text(to_shared(params.max_width_text));
  app.set_max_height_text(to_shared(params.max_height_text));
  app.set_max_long_edge_text(to_shared(params.max_long_edge_text));
  app.set_max_short_edge_text(to_shared(params.max_short_edge_text));
  app.set_quality_follows_format(
      png_lossless ||
      params.quality_text == std::format("{}", awj::default_quality_for(format)));
  app.set_bit_depth_follows_format(
      (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli)
          ? params.bit_depth_text == std::format(
                "{}", awj::encoding_defaults::default_webp_bit_depth)
          : params.bit_depth_text.empty());
}

std::array<LinuxParameterParams, 5>& active_linux_parameter_params(
    LinuxUiState& state) {
  return state.parameter_preset_index == 0 ? state.builtin_params
                                           : state.parameter_preset_params;
}

const std::array<LinuxParameterParams, 5>& active_linux_parameter_params(
    const LinuxUiState& state) {
  return state.parameter_preset_index == 0 ? state.builtin_params
                                           : state.parameter_preset_params;
}

void store_current_linux_parameter_params(AwjStudio& app, LinuxUiState& state) {
  const int index = linux_parameter_editor_format_index(state.last_format_index);
  auto params = capture_linux_parameter_params(app);
  const auto format = linux_output_format_from_index(index);
  if (format == awj::OutputFormat::png) {
    params.quality_text = std::format("{}", awj::default_quality_for(format));
    params.visual_quality_text.clear();
  }
  if ((format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
       format == awj::OutputFormat::jxl) && trim_copy(params.speed_text).empty()) {
    params.speed_text = std::format("{}", awj::default_speed_for(format));
  }
  active_linux_parameter_params(state)[static_cast<std::size_t>(index)] =
      std::move(params);
}

void apply_linux_format_parameters(AwjStudio& app, LinuxUiState& state,
                                   int index) {
  store_current_linux_parameter_params(app, state);
  state.last_format_index = linux_parameter_editor_format_index(index);
  if (app.get_format_index() != state.last_format_index) {
    app.set_format_index(state.last_format_index);
  }
  apply_linux_parameter_params(
      app, active_linux_parameter_params(state)[static_cast<std::size_t>(
               state.last_format_index)],
      state.last_format_index);
}

void push_flag(std::vector<std::wstring>& args, bool enabled,
               std::wstring enabled_arg, std::wstring disabled_arg) {
  args.push_back(enabled ? std::move(enabled_arg) : std::move(disabled_arg));
}

void push_option(std::vector<std::wstring>& args, std::wstring option,
                 std::wstring value) {
  if (!value.empty()) {
    args.push_back(std::move(option));
    args.push_back(std::move(value));
  }
}

std::expected<awj::AppConfig, std::string> linux_config_from_parameter_params(
    const AwjStudio* app, int format_index, const LinuxParameterParams& params) {
  std::vector<std::wstring> args;
  push_option(args, L"--format", format_arg(format_index));
  args.push_back(L"--no-wic-fallback");
  const auto format = linux_output_format_from_index(format_index);
  const bool png_lossless = format == awj::OutputFormat::png;
  const auto visual_quality = png_lossless
                                  ? std::wstring{}
                                  : awj::wide_from_utf8(params.visual_quality_text);
  if (!trim_copy(visual_quality).empty()) {
    push_option(args, L"--visual-quality", visual_quality);
  } else {
    push_option(args, L"--quality",
                png_lossless
                    ? std::to_wstring(awj::default_quality_for(format))
                    : awj::wide_from_utf8(params.quality_text));
  }
  push_option(args, L"--threads", awj::wide_from_utf8(params.threads_text));
  push_option(args, L"--memory-limit",
              memory_arg_from_ui(awj::wide_from_utf8(params.memory_limit_text)));
  push_option(args, L"--bit-depth", awj::wide_from_utf8(params.bit_depth_text));
  push_option(args, L"--speed", awj::wide_from_utf8(params.speed_text));
  push_option(args, L"--image-size-limit", size_limit_arg(params.size_limit_index));
  if (params.size_limit_index == 2) {
    push_option(args, L"--max-width", awj::wide_from_utf8(params.max_width_text));
    push_option(args, L"--max-height", awj::wide_from_utf8(params.max_height_text));
    push_option(args, L"--max-long-edge", awj::wide_from_utf8(params.max_long_edge_text));
    push_option(args, L"--max-short-edge", awj::wide_from_utf8(params.max_short_edge_text));
  }
  if (format_index == 0) {
    push_option(args, L"--avif-encoder", avif_encoder_arg(params.avif_encoder_index));
    push_option(args, L"--avif-color-representation",
                avif_color_representation_arg(
                    params.avif_color_representation_index));
    push_option(args, L"--chroma", chroma_arg(params.chroma_index));
    push_option(args, L"--alpha", alpha_arg(params.alpha_policy_index));
  } else if (format_index == 3) {
    push_option(args, L"--chroma", chroma_arg(params.chroma_index));
    push_option(args, L"--jpegli-progressive-level",
                std::to_wstring(params.jpegli_progressive_index));
    push_flag(args, params.jpegli_optimize_huffman,
              L"--jpegli-optimize-huffman", L"--no-jpegli-optimize-huffman");
    if (params.jpegli_xyb) {
      args.push_back(L"--jpegli-xyb");
    }
  }
  if (app != nullptr) {
    push_option(args, L"--input", wide_from_shared(app->get_input_path()));
    push_option(args, L"--output", wide_from_shared(app->get_output_dir()));
    push_option(args, L"--template", wide_from_shared(app->get_template_text()));
    push_option(args, L"--collision", collision_arg(app->get_collision_index()));
    push_flag(args, app->get_experimental_encoders(), L"--experimental-encoders",
              L"--no-experimental-encoders");
    if (!trim_copy(visual_quality).empty()) {
      push_flag(args, app->get_visual_quality_gpu(), L"--visual-quality-gpu",
                L"--no-visual-quality-gpu");
      push_flag(args, app->get_visual_quality_fallback(),
                L"--visual-quality-fallback", L"--no-visual-quality-fallback");
    }
    if (app->get_unlock_max_input_file_bytes()) {
      args.push_back(L"--unlock-max-input-file-bytes");
    }
    push_flag(args, app->get_strip_metadata(), L"--strip", L"--keep-metadata");
    push_flag(args, app->get_write_summary(), L"--summary", L"--no-summary");
    push_flag(args, app->get_write_log(), L"--log", L"--no-log");
  }

  auto parsed = awj::parse_arguments(args);
  if (!parsed) {
    return std::unexpected{parsed.error()};
  }
  return parsed->config;
}

int linux_avif_encoder_index(awj::AvifEncoderMode value) noexcept {
  switch (value) {
    case awj::AvifEncoderMode::svt: return 1;
    case awj::AvifEncoderMode::aom: return 2;
    case awj::AvifEncoderMode::zenrav1e: return 3;
    default: return 0;
  }
}

int linux_chroma_index(awj::ChromaMode value) noexcept {
  switch (value) {
    case awj::ChromaMode::yuv444: return 1;
    case awj::ChromaMode::yuv422: return 2;
    case awj::ChromaMode::yuv420: return 3;
    default: return 0;
  }
}

int linux_alpha_index(awj::AlphaModePolicy value) noexcept {
  switch (value) {
    case awj::AlphaModePolicy::force: return 0;
    case awj::AlphaModePolicy::off: return 2;
    default: return 1;
  }
}

LinuxParameterParams linux_parameter_params_from_config(const awj::AppConfig& config) {
  const int index = [&] {
    switch (config.output_format) {
      case awj::OutputFormat::avif: return 0;
      case awj::OutputFormat::webp: return 1;
      case awj::OutputFormat::jxl: return 2;
      case awj::OutputFormat::jpgli: return 3;
      case awj::OutputFormat::png:
      default: return 4;
    }
  }();
  auto params = default_linux_parameter_params(index);
  params.quality_text = std::format("{}", config.quality);
  params.visual_quality_text = config.visual_quality
                                   ? std::format("{}", *config.visual_quality)
                                   : std::string{};
  params.bit_depth_text = config.bit_depth ? std::format("{}", *config.bit_depth)
                                            : std::string{};
  if (config.output_format == awj::OutputFormat::avif ||
      config.output_format == awj::OutputFormat::webp ||
      config.output_format == awj::OutputFormat::jxl) {
    params.speed_text = std::format(
        "{}", config.speed.value_or(awj::default_speed_for(config.output_format)));
  }
  params.avif_encoder_index = linux_avif_encoder_index(config.avif_encoder);
  params.avif_color_representation_index =
      linux_avif_color_representation_index(config.avif_color_representation);
  params.chroma_index = linux_chroma_index(config.chroma_mode);
  params.alpha_policy_index = linux_alpha_index(config.alpha_policy);
  params.jpegli_progressive_index = config.jpegli_progressive_level;
  params.jpegli_optimize_huffman = config.jpegli_optimize_huffman;
  params.jpegli_xyb = config.jpegli_xyb;
  params.threads_text = config.max_jobs == awj::default_max_jobs()
                            ? std::string{}
                            : std::format("{}", config.max_jobs);
  if (config.memory_limit_bytes != 0) {
    params.memory_limit_text = std::format(
        "{}", (config.memory_limit_bytes + awj::studio_defaults::bytes_per_gib - 1) /
                  awj::studio_defaults::bytes_per_gib);
  }
  switch (config.image_size_limit.mode) {
    case awj::ImageSizeLimitMode::none: params.size_limit_index = 1; break;
    case awj::ImageSizeLimitMode::manual: params.size_limit_index = 2; break;
    default: params.size_limit_index = 0; break;
  }
  const auto optional_text = [](const std::optional<int>& value) {
    return value ? std::format("{}", *value) : std::string{};
  };
  params.max_width_text = optional_text(config.image_size_limit.max_width);
  params.max_height_text = optional_text(config.image_size_limit.max_height);
  params.max_long_edge_text = optional_text(config.image_size_limit.max_long_edge);
  params.max_short_edge_text = optional_text(config.image_size_limit.max_short_edge);
  return params;
}

std::array<LinuxParameterParams, 5> linux_parameter_params_from_user_preset(
    const awj::UserPreset& preset) {
  std::array<LinuxParameterParams, 5> params{};
  for (int index = 0; index < static_cast<int>(params.size()); ++index) {
    params[static_cast<std::size_t>(index)] = linux_parameter_params_from_config(
        awj::config_from_user_preset(preset, linux_output_format_from_index(index)));
  }
  return params;
}

void reload_linux_user_preset_options(AwjStudio& app, LinuxUiState& state) {
  auto catalog = awj::list_user_presets();
  if (!catalog) {
    state.user_presets.clear();
    state.user_preset_errors = {catalog.error()};
  } else {
    state.user_presets = std::move(catalog->presets);
    state.user_preset_errors = std::move(catalog->errors);
  }
  std::vector<ComboOption> options;
  options.push_back({.text = to_shared("内置默认"), .enabled = true});
  for (const auto& preset : state.user_presets) {
    options.push_back({.text = to_shared(preset.name), .enabled = true});
  }
  app.set_queue_preset_options(
      std::make_shared<slint::VectorModel<ComboOption>>(options));
  app.set_parameter_preset_options(
      std::make_shared<slint::VectorModel<ComboOption>>(std::move(options)));
  if (app.get_queue_preset_index() > static_cast<int>(state.user_presets.size())) {
    app.set_queue_preset_index(0);
  }
  if (state.parameter_preset_index > static_cast<int>(state.user_presets.size())) {
    state.parameter_preset_index = 0;
  }
  app.set_parameter_preset_index(state.parameter_preset_index);
  app.set_parameter_preset_description(
      state.parameter_preset_index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(
                          state.parameter_preset_index - 1)]
                          .description));
  const auto queue_index = std::clamp(
      app.get_queue_preset_index(), 0,
      static_cast<int>(state.user_presets.size()));
  app.set_queue_preset_index(queue_index);
  app.set_queue_preset_description(
      queue_index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(queue_index - 1)]
                          .description));
  if (!state.user_preset_errors.empty()) {
    app.set_status_text(to_shared(
        std::format("发现无效用户预设：{}", state.user_preset_errors.front())));
  }
}

void select_linux_parameter_preset(AwjStudio& app, LinuxUiState& state, int index) {
  store_current_linux_parameter_params(app, state);
  index = std::clamp(index, 0, static_cast<int>(state.user_presets.size()));
  state.parameter_preset_index = index;
  if (index > 0) {
    state.parameter_preset_params = linux_parameter_params_from_user_preset(
        state.user_presets[static_cast<std::size_t>(index - 1)]);
  }
  app.set_parameter_preset_index(index);
  app.set_parameter_preset_description(
      index == 0 ? slint::SharedString{}
                 : to_shared(state.user_presets[static_cast<std::size_t>(index - 1)]
                                 .description));
  apply_linux_format_parameters(app, state, app.get_format_index());
}

void select_linux_queue_preset(AwjStudio& app, LinuxUiState& state, int index) {
  index = std::clamp(index, 0, static_cast<int>(state.user_presets.size()));
  app.set_queue_preset_index(index);
  app.set_queue_preset_description(
      index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(index - 1)]
                          .description));
}

std::expected<awj::AppConfig, std::string> config_from_ui(
    AwjStudio& app, LinuxUiState& state) {
  store_current_linux_parameter_params(app, state);
  const auto queue_choice =
      linux_queue_format_choice_from_index(app.get_queue_format_index());
  const int format_index = queue_choice.format_index;
  const int preset_index = app.get_queue_preset_index();
  LinuxParameterParams params{};
  if (preset_index == 0) {
    params = state.builtin_params[static_cast<std::size_t>(format_index)];
  } else {
    const int user_index = preset_index - 1;
    if (user_index < 0 || static_cast<std::size_t>(user_index) >=
                              state.user_presets.size()) {
      return std::unexpected{"选中的用户预设已不存在，请重新选择。"};
    }
    params = linux_parameter_params_from_config(awj::config_from_user_preset(
        state.user_presets[static_cast<std::size_t>(user_index)],
        linux_output_format_from_index(format_index)));
  }
  auto config = linux_config_from_parameter_params(&app, format_index, params);
  if (config) {
    config->append_png_suffix = queue_choice.append_png_suffix;
  }
  return config;
}

std::expected<awj::UserPreset, std::string> linux_user_preset_from_parameters(
    std::string name, std::string description,
    const std::array<LinuxParameterParams, 5>& params) {
  auto preset = awj::default_user_preset();
  preset.name = std::move(name);
  preset.description = std::move(description);
  for (int index = 0; index < static_cast<int>(params.size()); ++index) {
    auto config = linux_config_from_parameter_params(
        nullptr, index, params[static_cast<std::size_t>(index)]);
    if (!config) {
      return std::unexpected{std::format("{} 预设参数错误：{}",
                                         awj::output_format_name(
                                             linux_output_format_from_index(index)),
                                         config.error())};
    }
    preset.formats[static_cast<std::size_t>(index)] =
        awj::preset_format_from_config(*config);
  }
  return preset;
}

std::expected<bool, std::string> confirm_linux_preset_overwrite(
    std::string_view name) {
  const auto question = shell_quote(std::format("预设“{}”已存在，是否覆盖？", name));
  if (command_exists("zenity")) {
    const int status = std::system(
        std::format("zenity --question --title='AWJ Studio' --text={} >/dev/null 2>&1",
                    question).c_str());
    return status == 0;
  }
  if (command_exists("kdialog")) {
    const int status = std::system(
        std::format("kdialog --title 'AWJ Studio' --yesno {} >/dev/null 2>&1",
                    question).c_str());
    return status == 0;
  }
  return std::unexpected{"同名预设已存在；请安装 zenity 或 kdialog 以确认覆盖，或改用新名称。"};
}
std::string xml_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

std::string remove_awj_thunar_actions(std::string xml) {
  if (xml.empty()) return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<actions>\n</actions>\n";
  std::string out;
  std::size_t pos = 0;
  while (true) {
    auto start = xml.find("<action", pos);
    while (start != std::string::npos) {
      const auto after_name = start + std::string_view{"<action"}.size();
      if (after_name >= xml.size() || xml[after_name] == '>' ||
          xml[after_name] == ' ' || xml[after_name] == '\t' ||
          xml[after_name] == '\r' || xml[after_name] == '\n') {
        break;
      }
      // Do not treat the <actions> container as an individual action.
      start = xml.find("<action", after_name);
    }
    if (start == std::string::npos) {
      out += xml.substr(pos);
      break;
    }
    const auto end = xml.find("</action>", start);
    if (end == std::string::npos) {
      out += xml.substr(pos);
      break;
    }
    const auto block_end = end + std::string_view{"</action>"}.size();
    const auto block = xml.substr(start, block_end - start);
    out += xml.substr(pos, start - pos);
    if (block.find("<unique-id>awjimage-") == std::string::npos) out += block;
    pos = block_end;
  }
  return out;
}

void append_linux_shell_arg(std::string& command, std::string_view value) {
  command.push_back(' ');
  command += shell_quote(value);
}

void append_linux_shell_option(std::string& command, std::string_view option,
                               std::string_view value) {
  append_linux_shell_arg(command, option);
  append_linux_shell_arg(command, value);
}

std::string awj_cli_command(const fs::path& exe, int format_index,
                            const LinuxMenuParams& params,
                            bool append_png_suffix = false) {
  auto command = shell_quote(awj::path_to_utf8(exe));
  const auto format = awj::utf8_from_wide(format_arg(format_index));
  append_linux_shell_option(command, "--output-policy", "shell");
  append_linux_shell_option(command, "--format", format);
  append_linux_shell_option(command, "--collision", "number");
  append_linux_shell_arg(command, "--no-wic-fallback");

  const bool is_avif = format_index == 0;
  const bool is_webp = format_index == 1;
  const bool is_jxl = format_index == 2;
  const bool is_jpgli = format_index == 3;
  const bool is_png = format_index == 4;
  if (!is_png && !trim_copy(params.quality_text).empty()) {
    append_linux_shell_option(command, "--quality", trim_copy(params.quality_text));
  }
  if ((is_avif || is_webp || is_jpgli || is_png) &&
      !trim_copy(params.bit_depth_text).empty()) {
    append_linux_shell_option(command, "--bit-depth", trim_copy(params.bit_depth_text));
  }
  if ((is_avif || is_webp || is_jxl) && !trim_copy(params.speed_text).empty()) {
    append_linux_shell_option(command, "--speed", trim_copy(params.speed_text));
  }
  append_linux_shell_arg(command, params.strip_metadata ? "--strip" : "--keep-metadata");
  append_linux_shell_option(command, "--image-size-limit",
                            awj::utf8_from_wide(size_limit_arg(params.size_limit_index)));
  if (params.size_limit_index == 2) {
    if (!trim_copy(params.max_width_text).empty()) append_linux_shell_option(command, "--max-width", trim_copy(params.max_width_text));
    if (!trim_copy(params.max_height_text).empty()) append_linux_shell_option(command, "--max-height", trim_copy(params.max_height_text));
    if (!trim_copy(params.max_long_edge_text).empty()) append_linux_shell_option(command, "--max-long-edge", trim_copy(params.max_long_edge_text));
    if (!trim_copy(params.max_short_edge_text).empty()) append_linux_shell_option(command, "--max-short-edge", trim_copy(params.max_short_edge_text));
  }
  if (is_avif) {
    append_linux_shell_option(command, "--avif-encoder",
                              awj::utf8_from_wide(avif_encoder_arg(params.avif_encoder_index)));
    append_linux_shell_option(
        command, "--avif-color-representation",
        awj::utf8_from_wide(avif_color_representation_arg(
            params.avif_color_representation_index)));
    append_linux_shell_option(command, "--chroma",
                              awj::utf8_from_wide(chroma_arg(params.chroma_index)));
    append_linux_shell_option(command, "--alpha",
                              awj::utf8_from_wide(alpha_arg(params.alpha_policy_index)));
    if (append_png_suffix) {
      append_linux_shell_arg(command, "--append-png-suffix");
    }
  } else if (is_jpgli) {
    append_linux_shell_option(command, "--chroma",
                              awj::utf8_from_wide(chroma_arg(params.chroma_index)));
    append_linux_shell_option(command, "--jpegli-progressive-level",
                              std::to_string(std::clamp(params.jpegli_progressive_index, 0, 2)));
    append_linux_shell_arg(command, params.jpegli_optimize_huffman
                                       ? "--jpegli-optimize-huffman"
                                       : "--no-jpegli-optimize-huffman");
    if (params.jpegli_xyb) append_linux_shell_arg(command, "--jpegli-xyb");
  }
  return command;
}

std::expected<void, std::string> validate_linux_menu_params(
    const std::array<LinuxMenuParams, 5>& menu_params) {
  constexpr std::array<std::string_view, 5> labels{"AVIF", "WebP", "JXL", "JPGLI", "PNG"};
  for (int format_index = 0; format_index < 5; ++format_index) {
    const auto& params = menu_params[static_cast<std::size_t>(format_index)];
    std::vector<std::wstring> args{L"--format", format_arg(format_index),
                                   L"--no-wic-fallback"};
    const bool is_avif = format_index == 0;
    const bool is_webp = format_index == 1;
    const bool is_jxl = format_index == 2;
    const bool is_jpgli = format_index == 3;
    const bool is_png = format_index == 4;
    if (!is_png) push_option(args, L"--quality", awj::wide_from_utf8(trim_copy(params.quality_text)));
    if (is_avif || is_webp || is_jpgli || is_png) {
      push_option(args, L"--bit-depth", awj::wide_from_utf8(trim_copy(params.bit_depth_text)));
    }
    if (is_avif || is_webp || is_jxl) {
      push_option(args, L"--speed", awj::wide_from_utf8(trim_copy(params.speed_text)));
    }
    push_flag(args, params.strip_metadata, L"--strip", L"--keep-metadata");
    push_option(args, L"--image-size-limit", size_limit_arg(params.size_limit_index));
    if (params.size_limit_index == 2) {
      push_option(args, L"--max-width", awj::wide_from_utf8(trim_copy(params.max_width_text)));
      push_option(args, L"--max-height", awj::wide_from_utf8(trim_copy(params.max_height_text)));
      push_option(args, L"--max-long-edge", awj::wide_from_utf8(trim_copy(params.max_long_edge_text)));
      push_option(args, L"--max-short-edge", awj::wide_from_utf8(trim_copy(params.max_short_edge_text)));
    }
    if (is_avif) {
      push_option(args, L"--avif-encoder", avif_encoder_arg(params.avif_encoder_index));
      push_option(args, L"--avif-color-representation",
                  avif_color_representation_arg(
                      params.avif_color_representation_index));
      push_option(args, L"--chroma", chroma_arg(params.chroma_index));
      push_option(args, L"--alpha", alpha_arg(params.alpha_policy_index));
    } else if (is_jpgli) {
      push_option(args, L"--chroma", chroma_arg(params.chroma_index));
      push_option(args, L"--jpegli-progressive-level",
                  std::to_wstring(std::clamp(params.jpegli_progressive_index, 0, 2)));
      push_flag(args, params.jpegli_optimize_huffman, L"--jpegli-optimize-huffman",
                L"--no-jpegli-optimize-huffman");
      if (params.jpegli_xyb) args.push_back(L"--jpegli-xyb");
    }
    auto parsed = awj::parse_arguments(args);
    if (!parsed) {
      return std::unexpected{std::format("{} 菜单参数错误：{}",
                                         labels[static_cast<std::size_t>(format_index)],
                                         parsed.error())};
    }
    if (auto valid = awj::validate_execution_config(parsed->config); !valid) {
      return std::unexpected{std::format("{} 菜单参数错误：{}",
                                         labels[static_cast<std::size_t>(format_index)],
                                         valid.error())};
    }
  }
  return {};
}

std::string nautilus_awj_script(const fs::path& exe, int format_index,
                                const LinuxMenuParams& params,
                                bool append_png_suffix = false) {
  std::string script = "#!/bin/sh\nset -eu\nfiles=${NAUTILUS_SCRIPT_SELECTED_FILE_PATHS:-}\n[ -n \"$files\" ] || exit 0\n";
  script += "printf '%s\n' \"$files\" | while IFS= read -r file; do\n";
  script += "  [ -n \"$file\" ] || continue\n";
  script += "  ";
  script += awj_cli_command(exe, format_index, params, append_png_suffix);
  script += " \"$file\"\n";
  script += "done\n";
  return script;
}

std::expected<void, std::string> write_nautilus_scripts(
    const fs::path& exe, const std::array<LinuxMenuParams, 5>& menu_params) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) return std::unexpected{"无法定位 HOME，不能写入 Nautilus 脚本。"};
  const auto scripts_dir = fs::path{home} / ".local" / "share" / "nautilus" / "scripts";
  std::error_code ec;
  fs::create_directories(scripts_dir, ec);
  if (ec) return std::unexpected{std::format("创建 Nautilus 脚本目录失败: {}", ec.message())};
  constexpr std::array<std::string_view, 5> labels{"AVIF", "WebP", "JXL", "JPGLI", "PNG"};
  for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
    const auto script_path = scripts_dir / std::format("AWJ 转换为 {}", labels[static_cast<std::size_t>(i)]);
    std::ofstream out{script_path, std::ios::binary | std::ios::trunc};
    if (!out) return std::unexpected{std::format("写入 Nautilus {} 脚本失败。", labels[static_cast<std::size_t>(i)])};
    out << nautilus_awj_script(exe, i, menu_params[static_cast<std::size_t>(i)]);
    out.close();
    fs::permissions(script_path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    if (ec) return std::unexpected{std::format("设置 Nautilus 脚本权限失败: {}", ec.message())};
  }
  if (menu_params.front().install_avif_png_command) {
    const auto script_path = scripts_dir / "AWJ 转换为 AVIF.png";
    std::ofstream out{script_path, std::ios::binary | std::ios::trunc};
    if (!out) return std::unexpected{"写入 Nautilus AVIF.png 脚本失败。"};
    out << nautilus_awj_script(exe, 0, menu_params.front(), true);
    out.close();
    fs::permissions(script_path,
                    fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    fs::perm_options::add, ec);
    if (ec) return std::unexpected{std::format("设置 Nautilus 脚本权限失败: {}", ec.message())};
  } else {
    fs::remove(scripts_dir / "AWJ 转换为 AVIF.png", ec);
  }
  return {};
}

std::expected<void, std::string> remove_nautilus_script() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) return {};
  std::error_code ec;
  constexpr std::array<std::string_view, 5> labels{"AVIF", "WebP", "JXL", "JPGLI", "PNG"};
  const auto scripts_dir = fs::path{home} / ".local" / "share" / "nautilus" / "scripts";
  for (const auto label : labels) fs::remove(scripts_dir / std::format("AWJ 转换为 {}", label), ec);
  fs::remove(scripts_dir / "AWJ 转换为 AVIF.png", ec);
  return {};
}
std::string thunar_action_xml(const fs::path& exe, int format_index,
                              const LinuxMenuParams& params,
                              bool append_png_suffix = false) {
  const auto format = append_png_suffix ? std::string{"AVIF.png"}
                                        : awj::utf8_from_wide(format_arg(format_index));
  auto command = awj_cli_command(exe, format_index, params, append_png_suffix);
  command += " %F";
  return std::format(
      "  <action>\n"
      "    <icon>image-x-generic</icon>\n"
      "    <name>{}</name>\n"
      "    <submenu>AWJimage 转换</submenu>\n"
      "    <unique-id>awjimage-{}</unique-id>\n"
      "    <command>{}</command>\n"
      "    <description>使用 AWJimage 转换选中的图片</description>\n"
      "    <patterns>*</patterns>\n"
      "    <startup-notify/>\n"
      "    <image-files/>\n"
      "    <directories/>\n"
      "  </action>\n",
      xml_escape(std::format("AWJ 转换为 {}", format)),
      xml_escape(append_png_suffix ? "avif-png" : format),
      xml_escape(command));
}

std::expected<void, std::string> write_thunar_actions(
    const std::array<LinuxMenuParams, 5>& menu_params) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) {
    return std::unexpected{"无法定位 HOME，不能写入 Thunar 右键菜单。"};
  }
  auto exe = awj::executable_path();
  if (!exe) return std::unexpected{exe.error()};
  const auto config_dir = fs::path{home} / ".config" / "Thunar";
  std::error_code ec;
  fs::create_directories(config_dir, ec);
  if (ec) return std::unexpected{std::format("创建 Thunar 配置目录失败: {}", ec.message())};
  const auto uca = config_dir / "uca.xml";
  std::string xml;
  if (std::ifstream in{uca}; in) {
    xml.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
  }
  xml = remove_awj_thunar_actions(std::move(xml));
  if (xml.find("</actions>") == std::string::npos) {
    xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<actions>\n</actions>\n";
  }
  std::string actions;
  for (int i = 0; i < 5; ++i) {
    actions += thunar_action_xml(*exe, i, menu_params[static_cast<std::size_t>(i)]);
  }
  if (menu_params.front().install_avif_png_command) {
    actions += thunar_action_xml(*exe, 0, menu_params.front(), true);
  }
  xml.insert(xml.rfind("</actions>"), actions);
  std::ofstream out{uca, std::ios::binary | std::ios::trunc};
  if (!out) return std::unexpected{"写入 Thunar 右键菜单失败。"};
  out << xml;
  return {};
}

std::expected<void, std::string> remove_thunar_actions() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) return {};
  const auto uca = fs::path{home} / ".config" / "Thunar" / "uca.xml";
  std::ifstream in{uca};
  if (!in) return {};
  std::string xml{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  xml = remove_awj_thunar_actions(std::move(xml));
  std::ofstream out{uca, std::ios::binary | std::ios::trunc};
  if (!out) return std::unexpected{"写入 Thunar 右键菜单失败。"};
  out << xml;
  return {};
}
std::expected<void, std::string> write_linux_file_manager_actions(
    const std::array<LinuxMenuParams, 5>& menu_params) {
  auto exe = awj::executable_path();
  if (!exe) return std::unexpected{exe.error()};
  auto nautilus = write_nautilus_scripts(*exe, menu_params);
  auto thunar = write_thunar_actions(menu_params);
  if (!nautilus && !thunar) {
    return std::unexpected{std::format("Nautilus: {}; Thunar: {}", nautilus.error(), thunar.error())};
  }
  return {};
}

std::expected<void, std::string> remove_linux_file_manager_actions() {
  auto nautilus = remove_nautilus_script();
  auto thunar = remove_thunar_actions();
  if (!nautilus && !thunar) {
    return std::unexpected{std::format("Nautilus: {}; Thunar: {}", nautilus.error(), thunar.error())};
  }
  return {};
}

std::optional<std::string> linux_context_menu_warning(
    const std::array<LinuxMenuParams, 5>& menu_params) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) {
    return std::nullopt;
  }
  const auto scripts_dir = fs::path{home} / ".local" / "share" /
                           "nautilus" / "scripts";
  const auto avif_script = scripts_dir / "AWJ 转换为 AVIF";
  const auto avif_png_script = scripts_dir / "AWJ 转换为 AVIF.png";
  std::error_code ec;
  const bool avif_installed = fs::exists(avif_script, ec) && !ec;
  ec.clear();
  const bool avif_png_installed = fs::exists(avif_png_script, ec) && !ec;
  if (!avif_installed && !avif_png_installed) {
    return std::nullopt;
  }
  if (!avif_installed ||
      avif_png_installed != menu_params.front().install_avif_png_command) {
    return "右键菜单与当前 AVIF.png 设置不一致，请重新安装右键菜单。";
  }
  auto executable = awj::executable_path();
  if (!executable) {
    return "无法检查右键菜单程序路径，请重新安装右键菜单。";
  }
  std::ifstream input{avif_script, std::ios::binary};
  const std::string actual{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
  if (!input || actual != nautilus_awj_script(*executable, 0,
                                                menu_params.front())) {
    return "右键菜单与当前菜单参数不一致，请重新安装右键菜单。";
  }
  if (menu_params.front().install_avif_png_command) {
    std::ifstream png_input{avif_png_script, std::ios::binary};
    const std::string png_actual{std::istreambuf_iterator<char>{png_input},
                                 std::istreambuf_iterator<char>{}};
    if (!png_input ||
        png_actual != nautilus_awj_script(*executable, 0,
                                          menu_params.front(), true)) {
      return "右键菜单与当前 AVIF.png 设置不一致，请重新安装右键菜单。";
    }
  }
  return std::nullopt;
}
void set_format_quality(AwjStudio& app, int index) {
  const auto quality = [index] {
    switch (index) {
      case 1:
        return awj::default_quality_for(awj::OutputFormat::webp);
      case 2:
        return awj::default_quality_for(awj::OutputFormat::jxl);
      case 3:
        return awj::default_quality_for(awj::OutputFormat::jpgli);
      case 4:
        return awj::default_quality_for(awj::OutputFormat::png);
      case 0:
      default:
        return awj::default_quality_for(awj::OutputFormat::avif);
    }
  }();
  app.set_quality_text(to_shared(std::format("{}", quality)));
}

void initialize_ui(AwjStudio& app) {
  app.set_task_rows(std::make_shared<slint::VectorModel<TaskRow>>());
  app.set_large_image_rows(std::make_shared<slint::VectorModel<LargeImageRow>>());
  app.set_wic_ui_visible(false);
  app.set_allow_wic_fallback(false);
  app.set_force_terminate_supported(false);
  app.set_menu_allow_wic_fallback(false);
  app.set_timestamp_ui_visible(false);
  app.set_visual_quality_fallback(true);
  app.set_visual_quality_gpu_help_text(to_shared("默认启用 Vulkan 加速 visual_quality 的 luma、GMSD 与 MS-SSIM 指标；codec 编码/解码仍使用 native CPU 库，失败或小图会自动回退 CPU。"));
  app.set_avif_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::avif))));
  app.set_webp_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::webp))));
  app.set_jxl_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::jxl))));
  app.set_jpegli_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::jpgli))));
  app.set_png_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::png))));
  app.set_webp_bit_depth_default(to_shared(std::format("{}", awj::encoding_defaults::default_webp_bit_depth)));
  app.set_quality_text(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::avif))));
  app.set_template_text(to_shared(std::string{awj::encoding_defaults::default_output_template_text}));
  app.set_memory_limit_text(to_shared(std::string{awj::encoding_defaults::default_memory_limit_text}));
  app.set_unlock_max_input_file_bytes(false);
  app.set_status_text(to_shared("就绪"));
}

}  // namespace

int run_studio_ui() {
  try {
    auto app = AwjStudio::create();
    auto state = std::make_shared<LinuxUiState>();
    state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
    state->large_image_rows = std::make_shared<slint::VectorModel<LargeImageRow>>();
    state->update_history_rows =
        std::make_shared<slint::VectorModel<UpdateHistoryRow>>();
    sync_linux_update_history(state->update_history_rows,
                              awj::update::Manifest{.schema = 1});
    auto weak = slint::ComponentWeakHandle(app);
    initialize_ui(*app);
    for (int i = 0; i < static_cast<int>(state->builtin_params.size()); ++i) {
      state->builtin_params[static_cast<std::size_t>(i)] =
          default_linux_parameter_params(i);
      state->parameter_preset_params[static_cast<std::size_t>(i)] =
          state->builtin_params[static_cast<std::size_t>(i)];
    }
    state->last_format_index = 0;
    apply_linux_parameter_params(*app, state->builtin_params.front(), 0);
    app->set_queue_format_index(0);
    app->set_queue_preset_index(0);
    reload_linux_user_preset_options(*app, *state);
    for (int i = 0; i < 5; ++i) {
      state->menu_params[static_cast<std::size_t>(i)] =
          default_linux_menu_params(i);
    }
    load_linux_update_config(*app, *state);
    restore_cached_linux_update_history(*state);
    sync_linux_update_ui(*app, *state);
    load_system_font_options(*app);
    state->menu_format_index = 0;
    apply_linux_menu_params(*app, state->menu_params.front());
    if (auto warning = linux_context_menu_warning(state->menu_params)) {
      app->set_context_menu_warning(to_shared(*warning));
    }
    app->set_task_rows(state->task_rows);
    app->set_large_image_rows(state->large_image_rows);
    app->set_update_history(state->update_history_rows);
    app->set_selected_large_image_index(-1);
    if (linux_changelog_should_open_on_start(*state) &&
        linux_changelog_visible_for_current_session(*state)) {
      app->set_selected_page(4);
    }

    // 与 Windows 分支同一套机制：0 = bundled 默认中文 msgid，1 = bundled 英文翻译。
    app->on_language_selection_requested([weak, state](int index) {
      if (auto app = weak.lock()) {
        (*app)->set_language_index(index);
        try {
          static_cast<void>(
              slint::select_bundled_translation(index == 1 ? "en" : ""));
        } catch (...) {
        }
        sync_linux_update_ui(**app, *state);
      }
    });
    app->on_update_channel_selection_requested([weak, state](int index) {
      if (auto app = weak.lock()) {
        const auto before = capture_linux_update_state(*state);
        state->update_channel = index == 1 ? "prerelease" : "stable";
        if (auto saved = persist_linux_update_config(**app, *state); !saved) {
          restore_linux_update_state(*state, before);
          state->update_status_zh =
              std::format("更新渠道保存失败：{}", saved.error());
          state->update_status_en = "The update channel could not be saved.";
          sync_linux_update_ui(**app, *state);
          return;
        }
        state->update_status_zh = "更新渠道已保存，正在检查…";
        state->update_status_en = "Update channel saved; checking...";
        sync_linux_update_ui(**app, *state);
        start_linux_update_check(weak, state);
      }
    });
    app->on_show_update_changelog_requested([weak, state](bool visible) {
      if (auto app = weak.lock()) {
        const auto before = capture_linux_update_state(*state);
        state->show_update_changelog = visible;
        if (!linux_changelog_visible_for_current_session(*state) &&
            (*app)->get_selected_page() == 4) {
          (*app)->set_selected_page(2);
        }
        if (auto saved = persist_linux_update_config(**app, *state); !saved) {
          restore_linux_update_state(*state, before);
          state->update_status_zh =
              std::format("更新日志设置保存失败：{}", saved.error());
          state->update_status_en =
              "The changelog setting could not be saved.";
        }
        sync_linux_update_ui(**app, *state);
      }
    });
    app->on_hide_update_changelog_after_exit_requested(
        [weak, state](bool enabled) {
          if (auto app = weak.lock()) {
            const auto before = capture_linux_update_state(*state);
            state->hide_update_changelog_after_exit = enabled;
            if (auto saved = persist_linux_update_config(**app, *state); !saved) {
              restore_linux_update_state(*state, before);
              state->update_status_zh =
                  std::format("更新日志设置保存失败：{}", saved.error());
              state->update_status_en =
                  "The changelog setting could not be saved.";
            }
            sync_linux_update_ui(**app, *state);
          }
        });
    app->on_show_update_changelog_after_update_requested(
        [weak, state](bool enabled) {
          if (auto app = weak.lock()) {
            const auto before = capture_linux_update_state(*state);
            state->show_update_changelog_after_update = enabled;
            if (auto saved = persist_linux_update_config(**app, *state); !saved) {
              restore_linux_update_state(*state, before);
              state->update_status_zh =
                  std::format("更新日志设置保存失败：{}", saved.error());
              state->update_status_en =
                  "The changelog setting could not be saved.";
            }
            sync_linux_update_ui(**app, *state);
          }
        });
    app->on_version_clicked([weak, state] {
      if (auto app = weak.lock()) {
        if (!linux_pending_update_is_newer(*state)) {
          const auto url = std::format(
              "https://github.com/Dominic485649/AWJimage/releases/tag/{}",
              AWJ_BUILD_VERSION);
          if (auto opened = open_linux_url(url); !opened) {
            state->update_status_zh =
                std::format("无法打开更新页面：{}", opened.error());
            state->update_status_en = "The release page could not be opened.";
            sync_linux_update_ui(**app, *state);
          }
          return;
        }
        if ((*app)->get_running()) {
          state->update_status_zh = "编码任务运行时禁止更新；请先完成或取消任务。";
          state->update_status_en =
              "Finish or cancel the current encoding task before updating.";
          sync_linux_update_ui(**app, *state);
          return;
        }
        if (state->update_check_active) {
          state->update_status_zh = "更新检查或下载正在进行中。";
          state->update_status_en = "An update operation is already running.";
          sync_linux_update_ui(**app, *state);
          return;
        }
        if (state->update_worker.joinable()) state->update_worker.join();
        const auto requested_version = state->pending_update_version;
        const auto release_url = state->pending_update_release_url;
        const auto preference = linux_update_preference(*state);
        const auto sequence = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, state->last_verified_manifest_v2_sequence));
        state->update_check_active = true;
        state->update_status_zh = "正在重新验签并下载更新…";
        state->update_status_en = "Verifying and downloading the update...";
        (*app)->set_update_checking(true);
        sync_linux_update_ui(**app, *state);
        state->update_worker = std::jthread(
            [weak, state, requested_version, release_url, preference,
             sequence](std::stop_token token) {
              auto staged = awj::update::stage_and_launch_linux_update(
                  requested_version, preference, sequence, token);
              static_cast<void>(slint::invoke_from_event_loop(
                  [weak, state, release_url, staged = std::move(staged)]() mutable {
                    auto app = weak.lock();
                    if (!app) return;
                    state->update_check_active = false;
                    (*app)->set_update_checking(false);
                    if (!staged) {
                      state->update_status_zh =
                          std::format("更新失败：{}", staged.error());
                      state->update_status_en = "The update could not be installed.";
                      sync_linux_update_ui(**app, *state);
                      if (staged.error().starts_with("INSTALL_DIR_NOT_WRITABLE:")) {
                        (void)open_linux_url(release_url);
                      }
                      return;
                    }
                    state->update_status_zh = "更新 helper 已启动，正在关闭当前版本…";
                    state->update_status_en =
                        "The update helper is ready; closing this version...";
                    sync_linux_update_ui(**app, *state);
                    (*app)->window().hide();
                  }));
            });
      }
    });
    app->on_format_defaults_requested([weak, state](int index) {
      if (auto app = weak.lock()) {
        if (!(*app)->get_running()) {
          apply_linux_format_parameters(**app, *state, index);
        }
      }
    });
    app->on_parameter_preset_selected([weak, state](int index) {
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        select_linux_parameter_preset(**app, *state, index);
      }
    });
    app->on_queue_preset_selected([weak, state](int index) {
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        select_linux_queue_preset(**app, *state, index);
      }
    });
    app->on_queue_drag_data([weak, state](int index) {
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        return make_linux_queue_drag_data(*state, index);
      }
      return slint::DataTransfer{};
    });
    app->on_queue_drag_can_drop(
        [weak, state](slint::language::DropEvent event, int target_slot) {
          if (auto app = weak.lock()) {
            return linux_queue_drag_can_drop(*state, std::move(event),
                                             target_slot, (*app)->get_running());
          }
          return slint::language::DragAction::None;
        });
    app->on_queue_drag_dropped(
        [weak, state](slint::language::DropEvent event, int target_slot) {
          if (auto app = weak.lock()) {
            return linux_queue_drag_dropped(**app, *state, std::move(event),
                                            target_slot);
          }
          return slint::language::DragAction::None;
        });
    app->on_open_preset_editor([weak, state] {
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        store_current_linux_parameter_params(**app, *state);
        const auto index = state->parameter_preset_index;
        (*app)->set_preset_editor_name(
            index == 0 ? slint::SharedString{}
                       : to_shared(state->user_presets[static_cast<std::size_t>(index - 1)]
                                       .name));
        (*app)->set_preset_editor_description(
            index == 0 ? slint::SharedString{}
                       : to_shared(state->user_presets[static_cast<std::size_t>(index - 1)]
                                       .description));
        (*app)->set_preset_editor_error({});
        (*app)->set_preset_editor_open(true);
      }
    });
    app->on_cancel_preset_editor([weak] {
      if (auto app = weak.lock()) {
        (*app)->set_preset_editor_open(false);
        (*app)->set_preset_editor_error({});
      }
    });
    app->on_save_parameter_preset(
        [weak, state](slint::SharedString name, slint::SharedString description) {
          auto app = weak.lock();
          if (!app || (*app)->get_running()) return;
          store_current_linux_parameter_params(**app, *state);
          auto preset = linux_user_preset_from_parameters(
              shared_to_string(name), shared_to_string(description),
              active_linux_parameter_params(*state));
          if (!preset) {
            (*app)->set_preset_editor_error(to_shared(preset.error()));
            return;
          }
          auto saved = awj::save_user_preset(*preset, false);
          if (!saved && saved.error() == "同名预设已存在；请确认覆盖。") {
            auto confirmed = confirm_linux_preset_overwrite(preset->name);
            if (!confirmed) {
              (*app)->set_preset_editor_error(to_shared(confirmed.error()));
              return;
            }
            if (!*confirmed) {
              (*app)->set_preset_editor_error(to_shared("已取消覆盖同名预设。"));
              return;
            }
            saved = awj::save_user_preset(*preset, true);
          }
          if (!saved) {
            (*app)->set_preset_editor_error(to_shared(saved.error()));
            return;
          }
          reload_linux_user_preset_options(**app, *state);
          const auto found = std::ranges::find(
              state->user_presets, preset->name, &awj::UserPreset::name);
          if (found != state->user_presets.end()) {
            select_linux_parameter_preset(
                **app, *state,
                static_cast<int>(std::distance(state->user_presets.begin(), found)) +
                    1);
          }
          (*app)->set_preset_editor_open(false);
          (*app)->set_preset_editor_error({});
          (*app)->set_status_text(to_shared("用户预设已保存。"));
        });
    app->on_clear_tasks([weak, state] {
      if (auto app = weak.lock()) {
        if ((*app)->get_running()) {
          (*app)->set_status_text(to_shared("当前任务正在运行，无法清空状态。"));
          return;
        }
        state->queue_files.clear();
        state->task_rows->set_vector({});
        state->failed_paths.clear();
        refresh_linux_queue_counts(**app, state->task_rows);
        (*app)->set_selected_queue_index(-1);
        (*app)->set_progress(0.0f);
        (*app)->set_status_text(to_shared("已清空状态。"));
      }
    });
    app->on_browse_input([weak, state] {
      if (auto app = weak.lock()) {
        auto selected = choose_path((*app)->get_input_mode_index() == 1);
        if (!selected) {
          (*app)->set_status_text(to_shared(selected.error()));
          return;
        }
        if (auto added = add_linux_queue_from_path(**app, *state, *selected,
                                                    true);
            !added) {
          (*app)->set_status_text(to_shared(added.error()));
        }
      }
    });
    app->on_input_path_accepted([weak, state](slint::SharedString text) {
      if (auto app = weak.lock()) {
        const auto path = awj::normalize_path_argument(
            awj::wide_from_utf8(shared_to_string(text)), "输入路径");
        if (!path) {
          (*app)->set_status_text(to_shared(path.error()));
          return;
        }
        if (auto added = add_linux_queue_from_path(**app, *state, *path, true);
            !added) {
          (*app)->set_status_text(to_shared(added.error()));
        }
      }
    });
    app->on_input_path_dropped(
        [weak, state](slint::language::DropEvent event) {
          if (auto app = weak.lock()) {
            if ((*app)->get_running()) {
              (*app)->set_status_text(to_shared("当前任务正在运行，无法添加队列。"));
              return;
            }
            const auto text = event.data.plain_text();
            if (!text) {
              (*app)->set_status_text(to_shared("拖入内容不是本地文件或文件夹路径。"));
              return;
            }
            const auto paths = native_drop_paths(*text);
            if (paths.empty()) {
              (*app)->set_status_text(to_shared("拖入内容不包含本地文件或文件夹路径。"));
              return;
            }
            bool update_input_path = true;
            std::string first_error;
            for (const auto& raw : paths) {
              const auto path = awj::normalize_path_argument(
                  awj::wide_from_utf8(raw), "拖入输入路径");
              if (!path) {
                if (first_error.empty()) first_error = path.error();
                continue;
              }
              if (auto added = add_linux_queue_from_path(
                      **app, *state, *path, update_input_path);
                  added && *added) {
                update_input_path = false;
              } else if (!added && first_error.empty()) {
                first_error = added.error();
              }
            }
            if (!first_error.empty()) {
              (*app)->set_status_text(to_shared(first_error));
            }
          }
        });

    app->on_browse_output([weak] {
      if (auto app = weak.lock()) {
        auto selected = choose_path(true);
        if (!selected) {
          (*app)->set_status_text(to_shared(selected.error()));
          return;
        }
        (*app)->set_output_dir(to_shared(awj::path_to_utf8(*selected)));
        (*app)->set_status_text(to_shared("已选择输出目录。"));
      }
    });
    app->on_open_output([weak] {
      if (auto app = weak.lock()) {
        fs::path path{shared_to_string((*app)->get_output_dir())};
        if (path.empty()) {
          path = awj::default_output_dir_for(fs::path{shared_to_string((*app)->get_input_path())});
        }
        if (auto opened = open_path(path); !opened) {
          (*app)->set_status_text(to_shared(opened.error()));
        }
      }
    });
    app->on_output_path_accepted([weak](slint::SharedString text) {
      if (auto app = weak.lock()) {
        const auto raw = shared_to_string(text);
        if (trim_copy(raw).empty()) {
          (*app)->set_output_dir({});
          return;
        }
        const auto path = awj::normalize_path_argument(
            awj::wide_from_utf8(raw), "输出目录");
        if (!path) {
          (*app)->set_status_text(to_shared(path.error()));
          return;
        }
        (*app)->set_output_dir(to_shared(awj::path_to_utf8(*path)));
      }
    });
    app->on_output_path_dropped(
        [weak](slint::language::DropEvent event) {
          if (auto app = weak.lock()) {
            if ((*app)->get_running()) {
              (*app)->set_status_text(to_shared("当前任务正在运行，无法修改输出目录。"));
              return;
            }
            const auto text = event.data.plain_text();
            if (!text) {
              (*app)->set_status_text(to_shared("拖入内容不是本地文件或文件夹路径。"));
              return;
            }
            const auto paths = native_drop_paths(*text);
            if (paths.size() != 1) {
              (*app)->set_status_text(to_shared("输出目录一次只能拖入一个文件或文件夹。"));
              return;
            }
            const auto path = awj::normalize_path_argument(
                awj::wide_from_utf8(paths.front()), "拖入输出目录");
            if (!path) {
              (*app)->set_status_text(to_shared(path.error()));
              return;
            }
            std::error_code ec;
            if (!std::filesystem::exists(*path, ec) || ec) {
              (*app)->set_status_text(to_shared("拖入的输出目标不存在或无法访问。"));
              return;
            }
            const auto output = std::filesystem::is_directory(*path, ec) && !ec
                                    ? *path
                                    : path->parent_path();
            if (output.empty()) {
              (*app)->set_status_text(to_shared("无法从拖入目标确定输出目录。"));
              return;
            }
            (*app)->set_output_dir(to_shared(awj::path_to_utf8(output)));
            (*app)->set_status_text(to_shared("已更新输出目录。"));
          }
        });
    app->on_menu_format_selected([weak, state](int index) {
      if (auto app = weak.lock()) {
        store_linux_menu_params(**app, *state);
        state->menu_format_index = std::clamp(index, 0, 4);
        apply_linux_menu_params(
            **app, state->menu_params[static_cast<std::size_t>(state->menu_format_index)]);
      }
    });
    app->on_save_menu_params_requested([weak, state] {
      if (auto app = weak.lock()) {
        store_linux_menu_params(**app, *state);
        auto valid = validate_linux_menu_params(state->menu_params);
        if (valid) valid = persist_linux_update_config(**app, *state);
        if (valid) {
          if (auto warning = linux_context_menu_warning(state->menu_params)) {
            (*app)->set_context_menu_warning(to_shared(*warning));
          } else {
            (*app)->set_context_menu_warning({});
          }
        }
        (*app)->set_context_menu_status(to_shared(
            valid ? "菜单参数已保存；重新安装菜单后生效。" : valid.error()));
        (*app)->set_status_text((*app)->get_context_menu_status());
      }
    });
    app->on_install_context_menu_requested([weak, state] {
      if (auto app = weak.lock()) {
        store_linux_menu_params(**app, *state);
        if (auto valid = validate_linux_menu_params(state->menu_params); !valid) {
          (*app)->set_context_menu_status(to_shared(valid.error()));
          (*app)->set_status_text(to_shared(valid.error()));
          return;
        }
        if (auto saved = persist_linux_update_config(**app, *state); !saved) {
          (*app)->set_context_menu_status(to_shared(saved.error()));
          (*app)->set_status_text(to_shared(saved.error()));
          return;
        }
        auto result = write_linux_file_manager_actions(state->menu_params);
        if (result) {
          (*app)->set_context_menu_warning({});
        }
        (*app)->set_context_menu_status(to_shared(
            result ? "已写入 Nautilus 脚本和 Thunar 右键菜单；重开文件管理器后生效。"
                   : result.error()));
        (*app)->set_status_text((*app)->get_context_menu_status());
      }
    });
    app->on_remove_context_menu_requested([weak] {
      if (auto app = weak.lock()) {
        auto result = remove_linux_file_manager_actions();
        if (result) {
          (*app)->set_context_menu_warning({});
        }
        (*app)->set_context_menu_status(to_shared(
            result ? "已移除 Nautilus 脚本和 Thunar AWJ 右键菜单。" : result.error()));
        (*app)->set_status_text((*app)->get_context_menu_status());
      }
    });
    app->on_context_menu_warning_clicked([weak] {
      if (auto app = weak.lock()) {
        auto result = remove_linux_file_manager_actions();
        (*app)->set_context_menu_status(to_shared(
            result ? "旧右键菜单已移除，请重新安装。" : result.error()));
        (*app)->set_status_text((*app)->get_context_menu_status());
        if (result) {
          (*app)->set_context_menu_warning({});
        }
      }
    });
    app->on_cancel_conversion([weak, state] {
      if (auto app = weak.lock()) {
        state->worker.request_stop();
        (*app)->set_status_text(to_shared("正在停止当前任务…"));
      }
    });
    app->on_retry_failed([weak, state] {
      auto app = weak.lock();
      if (!app) {
        return;
      }
      if ((*app)->get_running()) {
        (*app)->set_status_text(to_shared("当前任务正在运行，无法重试失败项。"));
        return;
      }
      if (state->failed_paths.empty()) {
        (*app)->set_status_text(to_shared("队列中没有失败项。"));
        return;
      }
      auto cfg = config_from_ui(**app, *state);
      if (!cfg) {
        (*app)->set_status_text(
            to_shared(std::format("配置错误：{}", cfg.error())));
        return;
      }
      auto retry_paths = state->failed_paths;
      auto files = build_linux_queue_files(*cfg, state->queue_files, &retry_paths);
      if (!files) {
        (*app)->set_status_text(
            to_shared(std::format("重试准备失败：{}", files.error())));
        return;
      }
      auto manifest = create_linux_queue_manifest(state->next_queue_run_id++, *files);
      if (!manifest) {
        (*app)->set_status_text(
            to_shared(std::format("重试准备失败：{}", manifest.error())));
        return;
      }
      cfg->studio_queue_manifest = *manifest;
      for (std::size_t index = 0; index < state->task_rows->row_count();
           ++index) {
        auto row = state->task_rows->row_data(index);
        if (row && row->state == 3) {
          row->state = 0;
          row->status = to_shared("等待重试");
          row->warning = false;
          state->task_rows->set_row_data(index, *row);
        }
      }
      refresh_linux_queue_counts(**app, state->task_rows);
      (*app)->set_running(true);
      (*app)->set_progress(0.0f);
      (*app)->set_status_text(to_shared(
          std::format("正在重试 {} 个失败项…", retry_paths.size())));
      auto rows = state->task_rows;
      state->worker = std::jthread(
          [weak, state, rows, cfg = std::move(*cfg),
           retry_paths = std::move(retry_paths), manifest_path = *manifest](
              std::stop_token token) mutable {
            auto progress = [weak, state, rows](
                                const awj::BatchProgress& event) {
              slint::invoke_from_event_loop([weak, state, rows, event] {
                if (auto app = weak.lock()) {
                  if (event.kind == awj::BatchEventKind::item_started) {
                    mark_task_row_running(rows, event.result);
                  } else if (event.kind == awj::BatchEventKind::item_finished) {
                    set_linux_task_row_result(rows, event.result);
                    std::erase(state->failed_paths, event.result.input_path);
                    if (!event.result.ok && !event.result.canceled) {
                      state->failed_paths.push_back(event.result.input_path);
                    }
                  } else if (event.kind ==
                             awj::BatchEventKind::large_image_queued) {
                    add_large_image_task_row(rows, event.large_image);
                    push_linux_large_image(*state, event.large_image);
                  }
                  refresh_linux_queue_counts(**app, rows);
                  if (event.total > 0) {
                    (*app)->set_progress(
                        static_cast<float>(event.completed) /
                        static_cast<float>(event.total));
                  }
                  if (!event.text.empty()) {
                    (*app)->set_status_text(to_shared(event.text));
                  }
                }
              });
            };
            auto summary = awj::run_batch(cfg, progress, token);
            std::error_code cleanup_ec;
            fs::remove(manifest_path, cleanup_ec);
            slint::invoke_from_event_loop(
                [weak, state, rows, summary = std::move(summary),
                 retry_paths = std::move(retry_paths)]() mutable {
                  if (auto app = weak.lock()) {
                    (*app)->set_running(false);
                    if (!summary) {
                      state->failed_paths = retry_paths;
                      for (const auto& path : retry_paths) {
                        if (const auto index =
                                linux_task_row_index_for_path(rows, path)) {
                          auto row = rows->row_data(*index);
                          if (row) {
                            row->state = 3;
                            row->status = to_shared("失败");
                            row->log = to_shared(summary.error());
                            row->warning = true;
                            rows->set_row_data(*index, *row);
                          }
                        }
                      }
                      refresh_linux_queue_counts(**app, rows);
                      (*app)->set_status_text(to_shared(
                          std::format("重试失败：{}", summary.error())));
                      return;
                    }
                    (*app)->set_progress(1.0f);
                    (*app)->set_status_text(to_shared(std::format(
                        "重试完成：成功 {}，失败 {}，取消 {}。",
                        summary->ok_count, summary->failed_count,
                        summary->canceled_count)));
                  }
                });
          });
    });
    app->on_toggle_template_token([weak](slint::SharedString token) {
      if (auto app = weak.lock()) {
        auto text = shared_to_string((*app)->get_template_text());
        const auto value = shared_to_string(token);
        if (const auto pos = text.find(value); pos != std::string::npos) {
          text.erase(pos, value.size());
        } else {
          if (!text.empty() && text.back() != '_') text += '_';
          text += value;
        }
        (*app)->set_template_text(to_shared(text));
      }
    });
    app->on_title_bar_theme_requested([](bool) {});
    app->on_combo_popup_requested([] {});
    app->on_combo_popup_finished([] {});
    app->on_large_image_action_requested([weak, state](int index, slint::SharedString action_text) {
      auto app = weak.lock();
      if (!app) return;
      if ((*app)->get_running()) {
        (*app)->set_status_text(to_shared("当前任务正在运行，请先停止任务"));
        return;
      }
      const auto action = shared_to_string(action_text);
      if (index < 0 || static_cast<std::size_t>(index) >= state->large_image_items.size()) {
        (*app)->set_status_text(to_shared("未选择大图任务"));
        return;
      }
      const auto item = state->large_image_items[static_cast<std::size_t>(index)];
      const bool available = (action == "grid" && linux_large_image_grid_available(item)) ||
                             (action == "zenrav1e" && linux_large_image_zenrav1e_available(item));
      if (!available) {
        (*app)->set_status_text(to_shared(std::format("{} 不可用", action)));
        return;
      }
      auto cfg = config_from_ui(**app, *state);
      if (!cfg) {
        (*app)->set_status_text(to_shared(std::format("配置错误：{}", cfg.error())));
        return;
      }
      (*cfg).input_path = item.file.path;
      (*cfg).output_format = awj::OutputFormat::avif;
      (*cfg).studio_large_action = awj::wide_from_utf8(action);
      (*cfg).visual_quality.reset();
      set_linux_large_image_status(*state, index, std::format("已选择 {} · 正在编码…", action));
      (*app)->set_running(true);
      (*app)->set_progress(0.0f);
      (*app)->set_status_text(to_shared(std::format("大图处理：{}", action)));
      state->worker = std::jthread([weak, state, cfg = std::move(*cfg), index, action](std::stop_token token) mutable {
        auto progress = [weak](const awj::BatchProgress& event) {
          slint::invoke_from_event_loop([weak, event] {
            if (auto app = weak.lock()) {
              if (event.total > 0) {
                (*app)->set_progress(static_cast<float>(event.completed) / static_cast<float>(event.total));
              }
              if (!event.text.empty()) {
                (*app)->set_status_text(to_shared(event.text));
              } else if (event.kind == awj::BatchEventKind::item_finished) {
                (*app)->set_status_text(to_shared(event.result.message));
              }
            }
          });
        };
        auto summary = awj::run_batch(cfg, progress, token);
        slint::invoke_from_event_loop([weak, state, summary = std::move(summary), index, action] {
          if (auto app = weak.lock()) {
            (*app)->set_running(false);
            if (!summary) {
              set_linux_large_image_status(*state, index, "失败");
              (*app)->set_status_text(to_shared(std::format("大图失败：{}", summary.error())));
              return;
            }
            const bool ok = summary->ok_count > 0 && summary->failed_count == 0;
            set_linux_large_image_status(*state, index, ok ? std::format("{} 完成", action) : std::format("{} 失败", action));
            (*app)->set_progress(1.0f);
            (*app)->set_status_text(to_shared(std::format("大图完成：成功 {}，失败 {}，取消 {}。",
                                                         summary->ok_count, summary->failed_count, summary->canceled_count)));
          }
        });
      });
    });

    app->on_start_conversion([weak, state] {
      auto app = weak.lock();
      if (!app) {
        return;
      }
      if ((*app)->get_running()) {
        state->worker.request_stop();
        (*app)->set_status_text(to_shared("正在停止当前任务…"));
        return;
      }
      auto cfg = config_from_ui(**app, *state);
      if (!cfg) {
        (*app)->set_status_text(to_shared(std::format("配置错误：{}", cfg.error())));
        return;
      }
      if (state->queue_files.empty()) {
        (*app)->set_status_text(to_shared("队列为空，请先输入、选择或拖入图片。"));
        return;
      }
      auto files = build_linux_queue_files(*cfg, state->queue_files);
      if (!files) {
        (*app)->set_status_text(
            to_shared(std::format("队列准备失败：{}", files.error())));
        return;
      }
      auto manifest = create_linux_queue_manifest(state->next_queue_run_id++, *files);
      if (!manifest) {
        (*app)->set_status_text(
            to_shared(std::format("队列准备失败：{}", manifest.error())));
        return;
      }
      cfg->studio_queue_manifest = *manifest;
      state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
      state->large_image_rows = std::make_shared<slint::VectorModel<LargeImageRow>>();
      state->large_image_items.clear();
      state->failed_paths.clear();
      std::vector<TaskRow> pending_rows;
      pending_rows.reserve(files->size());
      for (std::size_t index = 0; index < files->size(); ++index) {
        pending_rows.push_back(pending_linux_queue_row((*files)[index], index,
                                                       true));
      }
      state->task_rows->set_vector(std::move(pending_rows));
      (*app)->set_task_rows(state->task_rows);
      (*app)->set_large_image_rows(state->large_image_rows);
      (*app)->set_selected_large_image_index(-1);
      refresh_linux_queue_counts(**app, state->task_rows);
      (*app)->set_running(true);
      (*app)->set_progress(0.0f);
      (*app)->set_status_text(to_shared("正在转换…"));
      auto rows = state->task_rows;
      state->worker = std::jthread([weak, state, rows, cfg = std::move(*cfg),
                                    manifest_path = *manifest](std::stop_token token) mutable {
        auto progress = [weak, state, rows](const awj::BatchProgress& event) {
          slint::invoke_from_event_loop([weak, state, rows, event] {
            if (auto app = weak.lock()) {
              if (event.kind == awj::BatchEventKind::item_started) {
                mark_task_row_running(rows, event.result);
              } else if (event.kind == awj::BatchEventKind::item_finished) {
                set_linux_task_row_result(rows, event.result);
                std::erase(state->failed_paths, event.result.input_path);
                if (!event.result.ok && !event.result.canceled) {
                  state->failed_paths.push_back(event.result.input_path);
                }
              } else if (event.kind == awj::BatchEventKind::large_image_queued) {
                add_large_image_task_row(rows, event.large_image);
                push_linux_large_image(*state, event.large_image);
                if ((*app)->get_selected_large_image_index() < 0 && !state->large_image_items.empty()) {
                  (*app)->set_selected_large_image_index(0);
                }
              }
              refresh_linux_queue_counts(**app, rows);
              if (event.total > 0) {
                (*app)->set_progress(static_cast<float>(event.completed) / static_cast<float>(event.total));
              }
              if (!event.text.empty()) {
                (*app)->set_status_text(to_shared(event.text));
              } else if (event.kind == awj::BatchEventKind::item_finished) {
                (*app)->set_status_text(to_shared(event.result.message));
              }
            }
          });
        };
        auto summary = awj::run_batch(cfg, progress, token);
        std::error_code cleanup_ec;
        fs::remove(manifest_path, cleanup_ec);
        slint::invoke_from_event_loop([weak, summary = std::move(summary)] {
          if (auto app = weak.lock()) {
            (*app)->set_running(false);
            if (!summary) {
              (*app)->set_status_text(to_shared(std::format("转换失败：{}", summary.error())));
              return;
            }
            (*app)->set_progress(1.0f);
            (*app)->set_status_text(to_shared(std::format("完成：成功 {}，失败 {}，取消 {}。",
                                                         summary->ok_count,
                                                         summary->failed_count,
                                                         summary->canceled_count)));
          }
        });
      });
    });

    std::optional<std::chrono::system_clock::time_point> last_check{};
    if (state->last_successful_update_check_at > 0) {
      last_check = std::chrono::system_clock::time_point{
          std::chrono::seconds{state->last_successful_update_check_at}};
    }
    if (awj::update::should_check_now(
            {.trigger = awj::update::CheckTrigger::startup,
             .last_successful_check = last_check,
             .now = std::chrono::system_clock::now()})) {
      start_linux_update_check(weak, state);
    }
    app->window().on_close_requested([weak, state] {
      state->worker.request_stop();
      state->update_worker.request_stop();
      if (auto app = weak.lock()) {
        const auto before = capture_linux_update_state(*state);
        state->last_changelog_exit_version = AWJ_BUILD_VERSION;
        if (auto saved = persist_linux_update_config(**app, *state); !saved) {
          restore_linux_update_state(*state, before);
          (*app)->set_status_text(to_shared(
              std::format("关闭时保存更新设置失败：{}", saved.error())));
        }
      }
      return slint::CloseRequestResponse::HideWindow;
    });
    app->run();
    state->worker.request_stop();
    state->update_worker.request_stop();
    if (state->update_worker.joinable()) state->update_worker.join();
    return 0;
  } catch (const std::exception& ex) {
    std::println(stderr, "[FAIL] 启动 Slint UI 失败: {}", ex.what());
    return 1;
  } catch (...) {
    std::println(stderr, "[FAIL] 启动 Slint UI 失败。");
    return 1;
  }
}

#endif
