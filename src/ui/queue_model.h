#pragma once

#include <slint.h>
#include <memory>

namespace awj::ui {

template<typename App, typename Row>
void bind_queue_model(App& app, const std::shared_ptr<slint::VectorModel<Row>>& rows) {
  auto failed = std::make_shared<slint::FilterModel<Row>>(
      rows, [](const Row& row) { return row.state == 3; });
  app.set_task_rows(rows);
  app.set_failed_task_rows(failed);
  app.on_failed_row_source_index([failed](int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= failed->row_count()) return -1;
    return failed->unfiltered_row(index);
  });
}

template<typename App>
void adjust_queue_count(App& app, int status, int delta) {
  switch (status) {
    case 1: app.set_queue_running_count(app.get_queue_running_count() + delta); break;
    case 2: app.set_queue_success_count(app.get_queue_success_count() + delta); break;
    case 3: app.set_queue_failed_count(app.get_queue_failed_count() + delta); break;
    default: app.set_queue_pending_count(app.get_queue_pending_count() + delta); break;
  }
}

template<typename App, typename Row>
void replace_queue_row(App& app, const std::shared_ptr<slint::VectorModel<Row>>& rows,
                       std::size_t index, Row row) {
  const auto previous = rows->row_data(index);
  if (!previous) return;
  if (previous->state != row.state) {
    adjust_queue_count(app, previous->state, -1);
    adjust_queue_count(app, row.state, 1);
  }
  rows->set_row_data(index, row);
}

}  // namespace awj::ui
