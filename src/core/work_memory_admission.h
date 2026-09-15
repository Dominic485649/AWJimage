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
      const auto bytes = std::max<std::uint64_t>(1, groups[i].estimated_bytes);
      if (limit && bytes > limit)
        throw std::invalid_argument("Work group exceeds memory budget");
      pending_.emplace(bytes, i);
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
