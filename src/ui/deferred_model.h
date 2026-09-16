#pragma once

#include <slint.h>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace awj::ui {

template<typename Row>
class DeferredModel final : public slint::Model<Row> {
 public:
  void set_loader(std::function<std::vector<Row>()> loader) {
    rows_.reset();
    loader_ = std::move(loader);
    this->notify_reset();
  }
  std::size_t row_count() const override { return rows().size(); }
  std::optional<Row> row_data(std::size_t index) const override {
    const auto& values = rows();
    return index < values.size() ? std::optional<Row>{values[index]} : std::nullopt;
  }

 private:
  const std::vector<Row>& rows() const {
    if (!rows_) {
      rows_ = loader_ ? loader_() : std::vector<Row>{};
      loader_ = {};
    }
    return *rows_;
  }
  mutable std::optional<std::vector<Row>> rows_;
  mutable std::function<std::vector<Row>()> loader_;
};

}  // namespace awj::ui
