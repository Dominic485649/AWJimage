#include "studio_queue_rows.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "studio_queue_format.h"

import awj.core;

namespace awj::studio {

TaskRow make_queue_task_row(const QueueImageItem& item, std::size_t order) {
  const auto folder = item.path.parent_path();
  const auto output =
      item.locked_output_path.empty()
          ? std::string{}
          : awj::path_to_utf8(item.locked_output_path.filename());
  const auto output_path = item.locked_output_path.empty()
                               ? std::string{}
                               : awj::path_to_utf8(item.locked_output_path);
  const auto status = item.status_text.empty()
                          ? queue_status_label(item.status)
                          : item.status_text;
  return TaskRow{.order = to_shared(std::format("{}", order + 1)),
                 .filename = to_shared(awj::path_to_utf8(item.path.filename())),
                 .folder = to_shared(awj::path_to_utf8(folder)),
                 .size = to_shared(awj::format_size(item.bytes)),
                 .status = to_shared(status),
                 .output = to_shared(output),
                  .log = item.log_text,
                  .warning = item.warning,
                  .locked = !queue_item_editable(item),
                  .state = queue_status_code(item.status),
                  .input_path = to_shared(awj::path_to_utf8(item.path)),
                  .output_path = to_shared(output_path),
                  .encoder = to_shared(item.encoder_id),
                  .threads = item.encoder_threads > 0
                                 ? to_shared(std::format("{}", item.encoder_threads))
                                 : slint::SharedString{},
                  .stage_timings = to_shared(stage_timings_text(
                      item.decode_seconds, item.prepare_seconds,
                      item.encode_seconds, item.write_seconds))};
}

void refresh_queue_rows(AwjStudio& app, UiState& state) {
  state.queue_id_indices.clear();
  state.queue_run_indices.clear();
  std::vector<TaskRow> rows;
  rows.reserve(state.queue_items.size());
  int pending_count = 0;
  int running_count = 0;
  int success_count = 0;
  int failed_count = 0;
  for (std::size_t i = 0; i < state.queue_items.size(); ++i) {
    const auto& item = state.queue_items[i];
    state.queue_id_indices.emplace(item.id, i);
    if (item.run_index != std::numeric_limits<std::size_t>::max()) {
      state.queue_run_indices.emplace(item.run_index, i);
    }
    rows.push_back(make_queue_task_row(item, i));
    switch (item.status) {
      case QueueItemStatus::running:
        ++running_count;
        break;
      case QueueItemStatus::done:
      case QueueItemStatus::skipped:
        ++success_count;
        break;
      case QueueItemStatus::failed:
        ++failed_count;
        break;
      case QueueItemStatus::pending:
      case QueueItemStatus::canceled:
      default:
        ++pending_count;
        break;
    }
  }
  const auto previous_count = state.task_rows->row_count();
  for (std::size_t i = 0; i < std::min(previous_count, rows.size()); ++i) {
    if (state.task_rows->row_data(i) != rows[i]) state.task_rows->set_row_data(i, rows[i]);
  }
  while (state.task_rows->row_count() > rows.size()) {
    state.task_rows->erase(state.task_rows->row_count() - 1);
  }
  for (std::size_t i = previous_count; i < rows.size(); ++i) state.task_rows->push_back(rows[i]);
  app.set_queue_pending_count(pending_count);
  app.set_queue_running_count(running_count);
  app.set_queue_success_count(success_count);
  app.set_queue_failed_count(failed_count);
  if (app.get_selected_queue_index() >=
      static_cast<int>(state.queue_items.size())) {
    app.set_selected_queue_index(-1);
  }
}

std::optional<std::size_t> queue_index_for_id(const UiState& state,
                                              std::uint64_t id) noexcept {
  if (const auto it = state.queue_id_indices.find(id);
      it != state.queue_id_indices.end()) return it->second;
  return std::nullopt;
}

std::optional<std::size_t> queue_index_for_run_index(
    const UiState& state, std::size_t run_index) noexcept {
  if (const auto it = state.queue_run_indices.find(run_index);
      it != state.queue_run_indices.end()) return it->second;
  return std::nullopt;
}

bool move_queue_item(UiState& state, std::size_t from, std::size_t to) {
  if (from >= state.queue_items.size() || to >= state.queue_items.size() ||
      from == to || !queue_item_editable(state.queue_items[from])) {
    return false;
  }
  if (!queue_item_editable(state.queue_items[to])) {
    return false;
  }
  auto item = std::move(state.queue_items[from]);
  state.queue_items.erase(state.queue_items.begin() +
                          static_cast<std::ptrdiff_t>(from));
  state.queue_items.insert(
      state.queue_items.begin() + static_cast<std::ptrdiff_t>(to),
      std::move(item));
  return true;
}

std::size_t first_pending_index(const UiState& state) noexcept {
  for (std::size_t i = 0; i < state.queue_items.size(); ++i) {
    if (queue_item_editable(state.queue_items[i])) {
      return i;
    }
  }
  return state.queue_items.size();
}

std::size_t last_pending_index(const UiState& state) noexcept {
  for (std::size_t i = state.queue_items.size(); i > 0; --i) {
    if (queue_item_editable(state.queue_items[i - 1])) {
      return i - 1;
    }
  }
  return state.queue_items.size();
}


struct LargeImageManualAvailability {
  bool grid{};
};

}  // namespace awj::studio
