#pragma once

// 队列项的纯逻辑：状态文案/编码、阶段耗时文案、可编辑与可运行判定。
// 从 main.cpp 拆出，不依赖 UI 句柄，便于单独测试。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "studio_state.h"

namespace awj::studio {

struct StudioWorkerResizeEvent;
// 调用方须持有 state.mutex；大图子任务通过稳定队列 ID 绑定，仅接受 worker 索引 0。
std::optional<std::size_t> apply_queue_worker_resize(
    UiState& state, std::uint64_t run_id, const StudioWorkerResizeEvent& event,
    std::optional<std::uint64_t> queue_item_id = std::nullopt) noexcept;

TaskRow make_queue_task_row(const QueueImageItem& item, std::size_t order);
std::string queue_status_label(QueueItemStatus status);
int queue_status_code(QueueItemStatus status) noexcept;
std::string stage_seconds_text(double seconds);
std::string stage_timings_text(double decode_seconds, double prepare_seconds,
                               double encode_seconds, double write_seconds,
                               double resize_seconds = -1.0,
                               int resize_workers = 0);

const std::filesystem::path& queue_image_open_target(
    const QueueImageItem& item) noexcept;
void reset_queue_item_execution(QueueImageItem& item);
bool queue_item_editable(const QueueImageItem& item) noexcept;
bool queue_item_runnable(const QueueImageItem& item) noexcept;
bool queue_item_selected_for_run(const QueueImageItem& item,
                                 bool failed_only) noexcept;

std::expected<std::vector<awj::ImageFile>, std::string> build_run_files(
    const awj::AppConfig& cfg, const std::vector<QueueImageItem>& queue,
    bool failed_only);

}  // namespace awj::studio
