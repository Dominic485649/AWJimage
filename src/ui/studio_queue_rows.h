#pragma once

// 队列行的构建与刷新、按 id/run_index 查找、重排与待处理索引。
// 从 main.cpp 拆出。

#include <cstddef>
#include <cstdint>
#include <optional>

#include "awj_studio.h"
#include "studio_state.h"

namespace awj::studio {

TaskRow make_queue_task_row(const QueueImageItem& item, std::size_t order);
void refresh_queue_rows(AwjStudio& app, UiState& state);
std::optional<std::size_t> queue_index_for_id(const UiState& state,
                                              std::uint64_t id) noexcept;
std::optional<std::size_t> queue_index_for_run_index(
    const UiState& state, std::size_t run_index) noexcept;
bool move_queue_item(UiState& state, std::size_t from, std::size_t to);
std::size_t first_pending_index(const UiState& state) noexcept;
std::size_t last_pending_index(const UiState& state) noexcept;

}  // namespace awj::studio
