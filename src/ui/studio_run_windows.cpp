#include "studio_run_windows.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <dwmapi.h>
#include <scn/scan.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <slint.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
#include "studio_config.h"
#include "studio_config_io.h"
#include "studio_encode_dispatch.h"
#include "studio_fields.h"
#include "studio_fonts.h"
#include "studio_import.h"
#include "studio_import_dispatch.h"
#include "studio_json.h"
#include "studio_menu_params.h"
#include "studio_parameter_page.h"
#include "studio_presets.h"
#include "studio_queue_format.h"
#include "studio_queue_rows.h"
#include "studio_shell_cli.h"
#include "studio_state.h"
#include "studio_template_theme.h"
#include "studio_ui_util.h"
#include "studio_update_ui.h"
#include "studio_worker_control.h"
#include "studio_worker_events.h"

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
import awj.update_keyring;
import awj.update_manifest_v2;
import awj.update_model;
import awj.update_runtime;
import awj.update_windows;

namespace {

// 共享状态类型已拆到 studio_state.h，这里引入以便本文件其余代码沿用原名。
using awj::studio::adopt_win32_handle;
using awj::studio::clear_run_if_callback_not_posted;
using awj::studio::post_to_ui;
using awj::studio::report_ui_callback_failure;
using awj::studio::reset_failed_run;
using awj::studio::run_ui_callback;
using awj::studio::force_stop_current_worker;
using awj::studio::ForceStopResult;
using awj::studio::request_all_workers_stop;
using awj::studio::request_all_workers_stop_locked;
using awj::studio::set_status_text_noexcept;
using awj::studio::worker_active;
using awj::studio::MenuFormatParams;
using awj::studio::active_parameter_params;
using awj::studio::apply_parameter_params_to_ui;
using awj::studio::capture_parameter_params_from_ui;
using awj::studio::config_from_menu_params;
using awj::studio::default_menu_params_for_index;
using awj::studio::default_parameter_params_for_index;
using awj::studio::image_size_limit_from_fields;
using awj::studio::avif_encoder_options;
using awj::studio::output_format_from_index;
using awj::studio::parameter_editor_format_index;
using awj::studio::queue_format_choice_from_index;
using awj::studio::refresh_avif_encoder_options;
using awj::studio::parameter_index_from_output_format;
using awj::studio::store_current_parameter_params;
using awj::studio::validate_menu_params;
using awj::studio::ParameterFormatParams;
using awj::studio::parse_bit_depth_field;
using awj::studio::parse_int_field;
using awj::studio::parse_jobs_field;
using awj::studio::parse_memory_limit_field;
using awj::studio::parse_optional_int_field;
using awj::studio::parse_quality_field;
using awj::studio::parse_visual_quality_field;
using awj::studio::trim_copy;
using awj::studio::QueueImageItem;
using awj::studio::apply_system_ui_font;
using awj::studio::load_system_font_options;
using awj::studio::QueueItemStatus;
using awj::studio::select_system_ui_font_family;
using awj::studio::queue_item_editable;
using awj::studio::queue_item_runnable;
using awj::studio::queue_item_selected_for_run;
using awj::studio::queue_status_code;
using awj::studio::queue_status_label;
using awj::studio::stage_seconds_text;
using awj::studio::stage_timings_text;
using awj::studio::StudioChildProcess;
using awj::studio::StudioConfigSnapshot;
using awj::studio::UiState;
using awj::studio::parse_studio_worker_detail_event;
using awj::studio::parse_studio_worker_item_event;
using awj::studio::StudioWorkerDetailEvent;
using awj::studio::StudioWorkerItemEvent;
using awj::studio::shared_to_string;
using awj::studio::combo_option;
using awj::studio::set_combo_options;
using awj::studio::text_from_int;
using awj::studio::text_from_wide;
using awj::studio::to_shared;
using awj::studio::UniqueWin32Handle;
using awj::studio::json_escape;
using awj::studio::apply_menu_params_to_ui;
using awj::studio::capture_menu_params_from_ui;
using awj::studio::load_menu_params_for_index;
using awj::studio::menu_config_key;
using awj::studio::menu_params_snapshot;
using awj::studio::store_current_menu_params;
using awj::studio::menu_config_prefixes;
using awj::studio::studio_config_path;
using awj::studio::write_file_atomically;
using awj::studio::write_studio_config_file;
using awj::studio::awj_exe_path_for_shell_menu;
using awj::studio::cleanup_forced_worker_temp_files;
using awj::studio::cleanup_studio_queue_manifest;
using awj::studio::cli_arguments_from_config;
using awj::studio::collect_shell_launch_inputs;
using awj::studio::command_line_from_args;
using awj::studio::constrain_window_to_work_area;
using awj::studio::queue_path_key;
using awj::studio::reject_when_worker_active;
using awj::studio::remove_shell_context_menu;
using awj::studio::shell_context_menu_warning;
using awj::studio::start_studio_cli_worker;
using awj::studio::synchronize_shell_context_menu;
using awj::studio::trim_process_working_set;
using awj::studio::append_log_row;
using awj::studio::begin_child_conversion_run;
using awj::studio::begin_queue_conversion_run;
using awj::studio::build_run_files;
using awj::studio::create_studio_queue_manifest;
using awj::studio::guarded_worker;
using awj::studio::handle_queue_drag_can_drop;
using awj::studio::handle_queue_drag_dropped;
using awj::studio::handle_queue_menu_action;
using awj::studio::handle_queue_pointer_event;
using awj::studio::make_queue_drag_data;
using awj::studio::queue_drop_target_index;
using awj::studio::set_large_image_status;
using awj::studio::add_task_row;
using awj::studio::append_pending_event;
using awj::studio::first_pending_index;
using awj::studio::last_pending_index;
using awj::studio::make_queue_task_row;
using awj::studio::mark_task_row_running;
using awj::studio::move_queue_item;
using awj::studio::pending_shell_task_row;
using awj::studio::push_task_row;
using awj::studio::queue_index_for_id;
using awj::studio::queue_index_for_run_index;
using awj::studio::refresh_queue_rows;
using awj::studio::result_log_text;
using awj::studio::result_status_text;
using awj::studio::task_row_from_result;
using awj::studio::copy_text_to_clipboard;
using awj::studio::open_file_with_default_app;
using awj::studio::open_path;
using awj::studio::apply_studio_config_file;
using awj::studio::add_manual_large_images_from_picker;
using awj::studio::add_queue_from_path;
using awj::studio::append_prepared_import_file;
using awj::studio::add_manual_large_image_path;
using awj::studio::append_queue_image_path;
using awj::studio::manual_large_image_decision;
using awj::studio::output_dir_is_empty;
using awj::studio::set_input_path_preserving_output;
using awj::studio::apply_title_bar_theme;
using awj::studio::apply_import_result;
using awj::studio::apply_format_defaults_to_ui;
using awj::studio::config_from_parameter_params;
using awj::studio::parameter_params_from_config;
using awj::studio::parameter_params_from_user_preset;
using awj::studio::user_preset_from_parameter_params;
using awj::studio::collision_from_index;
using awj::studio::config_from_ui;
using awj::studio::effective_output_dir;
using awj::studio::initialize_ui_defaults;
using awj::studio::reload_user_preset_options;
using awj::studio::select_parameter_preset;
using awj::studio::select_queue_preset;
using awj::studio::enqueue_import;
using awj::studio::start_import_dispatcher;
using awj::studio::start_native_drop_registration;
using awj::studio::apply_ui_language;
using awj::studio::effective_studio_dark_mode;
using awj::studio::shell_window_dark_mode;
using awj::studio::sync_template_flags;
using awj::studio::windows_prefers_dark_mode;
using awj::studio::template_contains_token;
using awj::studio::toggle_template_token;
using awj::studio::large_image_manual_availability;
using awj::studio::queue_contains_path;
using awj::studio::queue_relative_dir_for;
using awj::studio::supported_files_in_folder;
using awj::studio::capture_studio_config;
using awj::studio::capture_update_state;
using awj::studio::current_studio_window_size;
using awj::studio::persist_studio_config_if_changed;
using awj::studio::changelog_first_start_for_current_version;
using awj::studio::changelog_should_open_on_start;
using awj::studio::changelog_visible_for_current_session;
using awj::studio::clear_pending_update;
using awj::studio::format_update_check_time;
using awj::studio::pending_update_is_newer;
using awj::studio::restore_cached_update_history;
using awj::studio::restore_update_state;
using awj::studio::start_update_check;
using awj::studio::sync_update_history;
using awj::studio::sync_update_ui;
using awj::studio::update_preference;
using awj::studio::update_summary;
using awj::studio::add_large_image_task_row;
using awj::studio::large_image_action_available;
using awj::studio::large_image_action_status;
using awj::studio::large_image_actions_summary;
using awj::studio::large_image_grid_available;
using awj::studio::make_large_image_row;
using awj::studio::push_large_image_row;
using awj::studio::select_first_large_image_from_state;
using awj::studio::output_template_contains;


}  // namespace

namespace awj::studio {

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
        std::make_shared<awj::ui::DeferredModel<UpdateHistoryRow>>();
    sync_update_history(state->update_history_rows,
                        awj::update::Manifest{.schema = 1});
    auto weak = slint::ComponentWeakHandle(app);
    app->on_settings_page_opened([weak, state] {
      run_ui_callback(weak, "加载系统字体列表失败", [&] {
        auto app = weak.lock();
        if (!app || state->ui_font_options_loaded) return;
        load_system_font_options(**app);
        state->ui_font_options_loaded = true;
      });
    });
    start_import_dispatcher(weak, state);

    awj::ui::bind_queue_model(*app, state->task_rows);
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
    if (auto recovered = awj::recover_user_preset_change([&] {
          return synchronize_shell_context_menu(state->menu_params);
        }); !recovered) config_warning = recovered.error();
    if (auto synced = synchronize_shell_context_menu(state->menu_params); !synced)
      config_warning = synced.error();
    if (auto legacy = awj::shell_context_menu::legacy_machine_commands(); legacy)
      app->set_legacy_machine_menu_present(!legacy->empty());
    reload_user_preset_options(*app, *state);
    if (!state->user_preset_errors.empty() && !config_warning) {
      config_warning = std::format("有 {} 个用户预设未加载：{}",
                                   state->user_preset_errors.size(),
                                   state->user_preset_errors.front());
    }
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
        std::vector<QueueImageItem>{}.swap(state->queue_items);
        state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
        awj::ui::bind_queue_model(**app, state->task_rows);
        decltype(state->queue_id_indices){}.swap(state->queue_id_indices);
        decltype(state->queue_run_indices){}.swap(state->queue_run_indices);
        state->queue_path_keys.clear();
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
          (*app)->set_preset_editor_existing(index > 0);
          (*app)->set_preset_editor_shell_menu(index > 0 && state->user_presets[static_cast<std::size_t>(index - 1)].shell_menu);
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

    app->on_delete_parameter_preset([weak, state] {
      auto app = weak.lock();
      if (!app || (*app)->get_running()) return;
      const auto index = state->parameter_preset_index;
      if (index <= 0 || index > static_cast<int>(state->user_presets.size())) return;
      const auto queue_index = (*app)->get_queue_preset_index();
      auto removed = awj::delete_user_preset(state->user_presets[static_cast<std::size_t>(index - 1)], [state] {
        return synchronize_shell_context_menu(state->menu_params);
      });
      if (!removed) { (*app)->set_preset_editor_error(to_shared(removed.error())); return; }
      state->parameter_preset_index = 0;
      (*app)->set_queue_preset_index(
          queue_index == index ? 0 : queue_index > index ? queue_index - 1 : queue_index);
      reload_user_preset_options(**app, *state);
      select_parameter_preset(**app, *state, 0);
      (*app)->set_preset_editor_open(false);
      (*app)->set_status_text(to_shared("用户预设已删除。"));
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
        preset->shell_menu = (*app)->get_preset_editor_shell_menu();
          const auto edit_index = state->parameter_preset_index;
          if (edit_index > 0 && edit_index <= static_cast<int>(state->user_presets.size())) {
            const auto& original = state->user_presets[static_cast<std::size_t>(edit_index - 1)];
            preset->source_path = original.source_path;
            const auto original_ui = parameter_params_from_user_preset(original);
            for (std::size_t i = 0; i < preset->formats.size(); ++i) {
              if (active_parameter_params(*state)[i].memory_limit_text == original_ui[i].memory_limit_text)
                preset->formats[i].memory_limit_bytes = original.formats[i].memory_limit_bytes;
              if (active_parameter_params(*state)[i].speed_text == original_ui[i].speed_text)
                preset->formats[i].speed = original.formats[i].speed;
            }
          }
          auto saved = awj::save_user_preset(*preset, edit_index > 0, [state] {
            return synchronize_shell_context_menu(state->menu_params);
          });
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
          if (auto result = synchronize_shell_context_menu(state->menu_params, true); !result) {
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
          if (auto saved = persist_studio_config_if_changed(**app, *state); !saved) {
            (*app)->set_context_menu_status(to_shared(saved.error()));
            (*app)->set_status_text(to_shared(saved.error()));
            return;
          }
          if (auto synced = synchronize_shell_context_menu(state->menu_params); !synced) {
            (*app)->set_context_menu_warning(to_shared(synced.error()));
            return;
          }
          (*app)->set_context_menu_warning({});
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
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        auto result = synchronize_shell_context_menu(state->menu_params);
        (*app)->set_context_menu_warning(result ? slint::SharedString{} : to_shared(result.error()));
        (*app)->set_status_text(to_shared(result ? "右键菜单已修复。" : result.error()));
      }
    });
    app->on_cleanup_legacy_machine_menu([weak] {
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        (*app)->set_status_text(to_shared("AWJ 不会修改或删除 HKLM 系统菜单，请使用系统级注册表管理工具处理历史项目。"));
      }
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

    app->on_close_confirm_dismissed([weak] {
      run_ui_callback(weak, "关闭确认取消失败", [&] {
        if (auto app = weak.lock()) {
          (*app)->set_close_confirm_open(false);
        }
      });
    });

    app->on_close_confirm_force_quit([weak, state] {
      run_ui_callback(weak, "强制停止退出失败", [&] {
        force_stop_current_worker(state);
        if (auto app = weak.lock()) {
          (*app)->set_close_confirm_open(false);
          // 置 false 后再关窗，避免再次进入确认分支。
          (*app)->set_running(false);
          (*app)->window().hide();
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
            if (action != "grid") {
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
      // 编码中先弹确认层，不直接关窗：用户可返回继续，或确认强制停止退出。
      if (worker_active(state)) {
        run_ui_callback(weak, "显示关闭确认失败", [&] {
          if (auto app = weak.lock()) {
            (*app)->set_close_confirm_open(true);
          }
        });
        return slint::CloseRequestResponse::KeepWindowShown;
      }
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
    auto parsed = awj::parse_arguments_with_user_preset(args);
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

}  // namespace awj::studio
