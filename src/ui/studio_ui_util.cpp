#include "studio_ui_util.h"

#include <format>
#include <mutex>
#include <string>

namespace awj::studio {

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

}  // namespace awj::studio
