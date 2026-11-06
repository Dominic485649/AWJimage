#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace awj {

// Select work only when its reservation fits, so waiting large groups cannot
// occupy all workers while smaller groups remain runnable.
class WorkMemoryAdmission {
 public:
  struct Ticket {
    std::size_t index;
    std::uint64_t bytes;
  };

  template <typename Groups>
  WorkMemoryAdmission(std::uint64_t limit, const Groups& groups) : limit_(limit) {
    for (std::size_t i = 0; i < groups.size(); ++i) {
      const auto estimate = std::max<std::uint64_t>(1, groups[i].estimated_bytes);
      bool exclusive{};
      if constexpr (requires { groups[i].exclusive_refinement; })
        exclusive = groups[i].exclusive_refinement;
      // 独占精化是明确的调度模式：保留原估算，不将超预算估算伪装成可容纳值。
      if (limit && estimate > limit && !exclusive)
        throw std::invalid_argument("Work group exceeds memory budget");
      const auto reservation = exclusive && limit ? limit : estimate;
      pending_.emplace(reservation, i);
    }
  }

  std::optional<Ticket> acquire(std::stop_token stop) {
    std::unique_lock lock{mutex_};
    cv_.wait(lock, stop, [&] {
      return pending_.empty() || !limit_ ||
             pending_.begin()->first <= limit_ - reserved_;
    });
    if (stop.stop_requested() || pending_.empty()) return std::nullopt;
    auto it = limit_ ? pending_.upper_bound(limit_ - reserved_) : pending_.end();
    --it;
    Ticket ticket{it->second, limit_ ? it->first : 0};
    reserved_ += ticket.bytes;
    pending_.erase(it);
    cv_.notify_all();
    return ticket;
  }

  // 调用方独占 owned；锁只保护总预约，不在持有解码图时阻塞等待其他任务。
  bool try_resize(std::uint64_t& owned, std::uint64_t requested) noexcept {
    std::lock_guard lock{mutex_};
    if (!limit_) return true;
    if (owned > reserved_) return false;
    const auto other = reserved_ - owned;
    if (requested > limit_ - other) return false;
    reserved_ = other + requested;
    owned = requested;
    cv_.notify_all();
    return true;
  }

  void release(std::uint64_t bytes) noexcept {
    std::lock_guard lock{mutex_};
    reserved_ -= bytes;
    cv_.notify_all();
  }

 private:
  std::uint64_t limit_{};
  std::uint64_t reserved_{};
  std::multimap<std::uint64_t, std::size_t> pending_{};
  std::mutex mutex_{};
  std::condition_variable_any cv_{};
};

}  // namespace awj
