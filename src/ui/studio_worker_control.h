#pragma once

// 编码 worker 的停止/强制终止控制。从 main.cpp 拆出。
// 关窗、取消、清空队列等路径共用；终止语义见各自注释。

#include <memory>

#include "studio_state.h"

namespace awj::studio {

enum class ForceStopResult { no_worker, terminated, terminate_failed };

// 已在持锁状态下调用。
bool request_all_workers_stop_locked(UiState& state) noexcept;
bool request_all_workers_stop(const std::shared_ptr<UiState>& state) noexcept;
bool worker_active(const std::shared_ptr<UiState>& state) noexcept;
ForceStopResult force_stop_current_worker(
    const std::shared_ptr<UiState>& state) noexcept;

}  // namespace awj::studio
