#pragma once

// Studio 的 UI 回调与状态辅助：异常兜底、跨线程投递、状态文案。
// 从 main.cpp 拆出，供各页面/调度模块共用。

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "awj_studio.h"
#include "studio_state.h"

namespace awj::studio {

void set_status_text_noexcept(AwjStudio& app, std::string_view text) noexcept;
void reset_failed_run(AwjStudio& app, UiState& state, std::uint64_t run_id,
                      std::string_view message) noexcept;
bool clear_run_if_callback_not_posted(UiState& state,
                                      std::uint64_t run_id) noexcept;

void report_ui_callback_failure(slint::ComponentWeakHandle<AwjStudio> weak,
                                std::string_view context,
                                std::string_view detail) noexcept;

// UI 回调的统一异常边界：Slint/Winit 的 Rust 栈帧不允许异常逃逸
// （panic=abort），所有回调体都要经过这里。
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

}  // namespace awj::studio
