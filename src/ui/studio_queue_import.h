#pragma once

#include <expected>
#include <span>
#include <string>

#include "import_service.h"
#include "studio_state.h"

namespace awj::studio {

struct QueueImportSummary {
  std::size_t added{};
  std::size_t requeued{};
  std::size_t folded{};
};

// UI 线程调用；准备失败时 queue/items/映射/next-ID 均保持原值。
std::expected<QueueImportSummary, std::string> merge_queue_import(
    UiState& state, std::span<const awj::ui_import::File> files);

}  // namespace awj::studio
