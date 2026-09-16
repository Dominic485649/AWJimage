#include "studio_queue_format.h"

#include <format>
#include <string>

namespace awj::studio {

std::string queue_status_label(QueueItemStatus status) {
  switch (status) {
    case QueueItemStatus::running:
      return "正在编码";
    case QueueItemStatus::done:
      return "完成";
    case QueueItemStatus::failed:
      return "失败";
    case QueueItemStatus::skipped:
      return "已跳过";
    case QueueItemStatus::canceled:
      return "已取消";
    case QueueItemStatus::pending:
    default:
      return "等待编码";
  }
}

int queue_status_code(QueueItemStatus status) noexcept {
  switch (status) {
    case QueueItemStatus::running:
      return 1;
    case QueueItemStatus::done:
    case QueueItemStatus::skipped:
      return 2;
    case QueueItemStatus::failed:
      return 3;
    case QueueItemStatus::canceled:
      return 4;
    case QueueItemStatus::pending:
    default:
      return 0;
  }
}

std::string stage_seconds_text(double seconds) {
  return seconds < 0.0 ? std::string{"-"} : std::format("{:.3f}s", seconds);
}

std::string stage_timings_text(double decode_seconds, double prepare_seconds,
                               double encode_seconds, double write_seconds) {
  if (decode_seconds < 0.0 && prepare_seconds < 0.0 &&
      encode_seconds < 0.0 && write_seconds < 0.0) {
    return {};
  }
  return std::format("decode {} · prepare {} · encode {} · write {}",
                     stage_seconds_text(decode_seconds),
                     stage_seconds_text(prepare_seconds),
                     stage_seconds_text(encode_seconds),
                     stage_seconds_text(write_seconds));
}

bool queue_item_editable(const QueueImageItem& item) noexcept {
  return item.status == QueueItemStatus::pending;
}

bool queue_item_runnable(const QueueImageItem& item) noexcept {
  return item.status == QueueItemStatus::pending ||
         item.status == QueueItemStatus::failed ||
         item.status == QueueItemStatus::canceled;
}

bool queue_item_selected_for_run(const QueueImageItem& item,
                                 bool failed_only) noexcept {
  return failed_only ? item.status == QueueItemStatus::failed
                     : queue_item_runnable(item);
}

}  // namespace awj::studio
