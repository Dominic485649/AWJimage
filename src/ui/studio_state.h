#pragma once

// AWJ Studio（Windows 半区）的共享状态类型。
// 从 main.cpp 拆出，供 studio_json / studio_config / studio_queue 等模块共用。

#include "queue_model.h"
#include "deferred_model.h"

#ifdef _WIN32

#include <dwmapi.h>
#include <slint.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <limits>
#include <vector>

#include "awj_studio.h"
#include "file_drop_win32.h"
#include "import_service.h"
#include "slint_string_util.h"

import awj.config;
import awj.core;
import awj.large_image_plan;
import awj.pipeline;
import awj.preset;
import awj.studio_defaults;

namespace awj::studio {

// 下拉选项构造与批量赋值。多个页面模块共用。
inline ComboOption combo_option(std::string_view text, bool enabled = true) {
  return ComboOption{.text = to_shared(text), .enabled = enabled};
}

inline void set_combo_options(
    AwjStudio& app, const std::vector<ComboOption>& options,
    void (AwjStudio::*setter)(const std::shared_ptr<slint::Model<ComboOption>>&)
        const) {
  auto model = std::make_shared<slint::VectorModel<ComboOption>>();
  model->set_vector(options);
  (app.*setter)(model);
}

inline std::string text_from_wide(std::wstring_view text) {
  return awj::utf8_from_wide(text);
}
inline std::string text_from_int(int value) { return std::format("{}", value); }

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
[[nodiscard]] inline UniqueWin32Handle adopt_win32_handle(HANDLE value) noexcept {
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
  // 用 slint::SharedString 是为了让每行重建时只增加引用计数，不复制整行日志；
  // 代价是这个类型带有危险的 operator=(const char*) 重载——清空必须调用
  // clear_shared_string()，不能写 `log_text = {}`（详见该函数的注释）。
  slint::SharedString log_text{};
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
  bool jxl_jpeg_lossless{true};
  bool strip_metadata{};
  bool allow_wic_fallback{true};
  bool close_on_finish{true};
  bool install_avif_png_command{};
  int size_limit_index{};
  std::string max_width_text{};
  std::string max_height_text{};
  std::string max_long_edge_text{};
  std::string max_short_edge_text{};
  std::string scale_percent_text{};

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
  bool jxl_jpeg_lossless{true};
  std::string threads_text{};
  std::string memory_limit_text{};
  int size_limit_index{};
  std::string max_width_text{};
  std::string max_height_text{};
  std::string max_long_edge_text{};
  std::string max_short_edge_text{};
  std::string scale_percent_text{};
};

struct StudioConfigSnapshot {
  int theme_index{};
  // 界面语言：0 = 中文（.slint 里的 msgid 原文），1 = English（bundled 翻译）。
  int language_index{};
  std::string ui_font_family{};
  bool allow_wic_fallback{};
  bool shell_menu_compatibility{};
  bool visual_quality_gpu{true};
  bool visual_quality_fallback{true};
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
  std::jthread menu_worker{};
  bool menu_operation_active{};
  slint::Timer menu_timer{};
  std::unique_ptr<awj::ui_import::Dispatcher> import_dispatcher{};
  std::optional<awj::ui_drop::Registration> native_drop{};
  slint::Timer native_drop_timer{};
  std::size_t native_drop_attempts{};
  bool native_drop_registration_finished{};
  std::shared_ptr<slint::VectorModel<TaskRow>> task_rows{};
  std::shared_ptr<slint::VectorModel<LargeImageRow>> large_image_rows{};
  std::shared_ptr<awj::ui::DeferredModel<UpdateHistoryRow>> update_history_rows{};
  bool ui_font_options_loaded{};
  std::vector<QueueImageItem> queue_items{};
  std::unordered_map<std::uint64_t, std::size_t> queue_id_indices{};
  std::unordered_map<std::size_t, std::size_t> queue_run_indices{};
  // 与 queue_items 同步维护的路径键集合（绝对+规范化+Windows 小写）。
  // 加入队列时用它做 O(1) 判重，避免逐个新文件线性扫描整个队列并重复
  // 规范化路径——上万张图时那是 O(n²)。
  std::unordered_set<std::wstring> queue_path_keys{};
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
}  // namespace awj::studio

#endif  // _WIN32
