#include "studio_import_dispatch.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "studio_import.h"
#include "studio_queue_rows.h"
#include "studio_ui_util.h"

import awj.core;
import awj.encoding_defaults;

namespace awj::studio {

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
      .target_at = [weak](POINT point, std::size_t count) {
        auto app = weak.lock();
        if (!app || (*app)->get_selected_page() != 1 || count == 0) return 0;
        const float scale = (*app)->window().scale_factor();
        if (scale <= 0) return 0;
        const float x = point.x / scale;
        const float y = point.y / scale;
        auto regions = (*app)->get_drop_regions();
        for (std::size_t i = 0; i < regions->row_count(); ++i) {
          const auto region = regions->row_data(i);
          if (region && x >= region->x && y >= region->y &&
              x < region->x + region->width && y < region->y + region->height) {
            return i == 1 && count != 1 ? 0 : static_cast<int>(i + 1);
          }
        }
        return 0;
      },
      .hover_changed = [weak](awj::ui_drop::HoverState hover) {
        run_ui_callback(weak, "更新外部拖放状态失败", [&] {
          if (auto app = weak.lock()) {
            (*app)->set_external_drag_active(hover.active);
            (*app)->set_external_drag_valid(hover.valid);
            (*app)->set_external_drag_target(hover.target);
            (*app)->set_external_drag_item_count(
                static_cast<int>(std::min<std::size_t>(
                    hover.item_count,
                    static_cast<std::size_t>(std::numeric_limits<int>::max()))));
          }
        });
      },
      .paths_dropped = [weak, weak_state](
                           std::vector<std::filesystem::path> paths, int target) mutable {
        bool accepted = false;
        run_ui_callback(weak, "接收 Windows 原生拖放失败", [&] {
          const auto state = weak_state.lock();
          if (!state || paths.empty()) return;
          if (target == 2) {
            auto app = weak.lock();
            if (!app || paths.size() != 1) return;
            std::scoped_lock lock{state->mutex};
            if (state->worker_active) return;
            std::error_code ec;
            const auto path = std::filesystem::absolute(paths.front(), ec);
            if (ec) return;
            auto output = path;
            if (!std::filesystem::is_directory(path, ec)) {
              if (ec || !std::filesystem::is_regular_file(path, ec) || ec) return;
              output = path.parent_path();
            }
            if (ec || output.empty()) return;
            (*app)->set_output_dir(to_shared(awj::path_to_utf8(output)));
            accepted = true;
            return;
          }
          if (target != 1 && target != 3) return;
          awj::ui_import::Request job;
          job.origin = awj::ui_import::Origin::drag_drop;
          job.input_hint = paths.front();
          job.update_input_path = true;
          job.roots.reserve(paths.size());
          for (auto& path : paths) {
            job.roots.push_back({.path = std::move(path)});
          }
          if (enqueue_import(state, std::move(job))) {
            accepted = true;
            if (auto app = weak.lock()) {
              (*app)->set_status_text(to_shared("正在导入拖入的文件与文件夹…"));
            }
          }
        });
        return accepted;
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


}  // namespace awj::studio
