#include "studio_queue_import.h"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "queue_merge.h"
#include "studio_queue_format.h"

namespace awj::studio {

std::expected<QueueImportSummary, std::string> merge_queue_import(
    UiState& state, std::span<const awj::ui_import::File> files) {
  std::scoped_lock lock{state.mutex};
  if (state.worker_active) return std::unexpected{"编码仍在运行或收尾，不能导入。"};
  if (files.empty()) return QueueImportSummary{};
  try {
    const auto plan = awj::ui_queue::plan_merge(state.queue_items.size(), files.size(),
        [&](std::size_t i) { return awj::ui_queue::path_key(state.queue_items[i].path); },
        [&](std::size_t i) { return awj::ui_queue::path_key(files[i].path); });
    auto next_id = state.next_queue_id;
    std::vector<QueueImageItem> items;
    std::unordered_set<std::wstring> keys;
    std::unordered_map<std::uint64_t, std::size_t> ids;
    std::unordered_map<std::size_t, std::size_t> runs;
    items.reserve(plan.entries.size());
    keys.reserve(plan.entries.size());
    ids.reserve(plan.entries.size());
    runs.reserve(plan.entries.size());
    for (const auto& entry : plan.entries) {
      auto item = entry.previous == awj::ui_queue::no_index
          ? QueueImageItem{} : state.queue_items[entry.previous];
      if (entry.incoming != awj::ui_queue::no_index) {
        const auto& file = files[entry.incoming];
        if (entry.previous == awj::ui_queue::no_index) {
          if (next_id == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected{"队列身份编号已用尽。"};
          item.id = next_id++;
        }
        item.path = file.path;
        item.source_root = file.source_root;
        item.relative_dir.clear();
        if (!file.source_root.empty()) {
          std::error_code ec;
          auto relative = std::filesystem::relative(file.path.parent_path(), file.source_root, ec);
          if (!ec && relative != L".") item.relative_dir = std::move(relative);
        }
        item.bytes = file.bytes;
        reset_queue_item_execution(item);
      }
      keys.insert(awj::ui_queue::path_key(item.path));
      ids.emplace(item.id, items.size());
      if (item.run_index != awj::ui_queue::no_index) runs.emplace(item.run_index, items.size());
      items.push_back(std::move(item));
    }
    state.queue_items.swap(items);
    state.queue_path_keys.swap(keys);
    state.queue_id_indices.swap(ids);
    state.queue_run_indices.swap(runs);
    state.next_queue_id = next_id;
    ++state.run_id;  // 所有已结束运行的迟到回调立即失效。
    return QueueImportSummary{plan.added, plan.requeued, plan.folded};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"准备导入队列时内存不足，原队列未修改。"};
  } catch (const std::length_error&) {
    return std::unexpected{"导入队列数量超过运行时限制，原队列未修改。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"准备导入路径失败，原队列未修改。"};
  }
}

}  // namespace awj::studio
