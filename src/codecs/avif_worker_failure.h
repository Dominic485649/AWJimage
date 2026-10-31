#pragma once
#include <atomic>
#include <string>
#include <utility>

namespace awj::avif_worker {
enum class GridTileFailure { none, tile, memory, length, worker, unknown };

class GridTileFailureState {
 public:
  GridTileFailure reason() const noexcept { return reason_.load(std::memory_order_relaxed); }
  bool record(GridTileFailure reason) noexcept {
    if (reason == GridTileFailure::none) return false;
    auto expected = GridTileFailure::none;
    return reason_.compare_exchange_strong(expected, reason, std::memory_order_relaxed);
  }
  void capture(std::string& error) noexcept {
    if (record(GridTileFailure::tile)) error_.swap(error);
  }
  // message 的唯一写者是首个 tile 失败者；只允许在全部 worker join 后取出。
  std::string take_error() noexcept { return std::move(error_); }

 private:
  std::atomic<GridTileFailure> reason_{GridTileFailure::none};
  std::string error_;
};
}
