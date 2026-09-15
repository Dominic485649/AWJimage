#include "studio_worker_control.h"

#include <mutex>

namespace awj::studio {

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

}  // namespace awj::studio
