#pragma once

// 队列项的纯逻辑：状态文案/编码、阶段耗时文案、可编辑与可运行判定。
// 从 main.cpp 拆出，不依赖 UI 句柄，便于单独测试。

#include <cstddef>
#include <string>

#include "studio_state.h"

namespace awj::studio {

std::string queue_status_label(QueueItemStatus status);
int queue_status_code(QueueItemStatus status) noexcept;
std::string stage_seconds_text(double seconds);
std::string stage_timings_text(double decode_seconds, double prepare_seconds,
                               double encode_seconds, double write_seconds);

bool queue_item_editable(const QueueImageItem& item) noexcept;
bool queue_item_runnable(const QueueImageItem& item) noexcept;
bool queue_item_selected_for_run(const QueueImageItem& item,
                                 bool failed_only) noexcept;

}  // namespace awj::studio
