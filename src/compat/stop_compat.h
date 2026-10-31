#pragma once

#ifndef AWJ_USE_PORTABLE_STDLIB
#define AWJ_USE_PORTABLE_STDLIB 0
#endif

#if AWJ_USE_PORTABLE_STDLIB
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>
#else
#include <stop_token>
#endif

namespace awj::compat {
#if AWJ_USE_PORTABLE_STDLIB
namespace detail {
struct stop_state {
  std::atomic<bool> requested{};
  std::atomic<std::size_t> sources{1};
};
}

class stop_source;

// SDK 仅需要协作式轮询；状态由 source/token 共同持有，不借用 context 内存。
class stop_token {
 public:
  stop_token() noexcept = default;
  bool stop_requested() const noexcept {
    return state_ && state_->requested.load(std::memory_order_acquire);
  }
  bool stop_possible() const noexcept {
    return state_ && (state_->sources.load(std::memory_order_acquire) != 0 ||
                      state_->requested.load(std::memory_order_acquire));
  }
  friend bool operator==(const stop_token&, const stop_token&) noexcept = default;

 private:
  friend class stop_source;
  explicit stop_token(std::shared_ptr<detail::stop_state> state) noexcept
      : state_(std::move(state)) {}
  std::shared_ptr<detail::stop_state> state_;
};

class stop_source {
 public:
  stop_source() : state_(std::make_shared<detail::stop_state>()) {}
  stop_source(const stop_source& other) noexcept : state_(other.state_) {
    if (state_) state_->sources.fetch_add(1, std::memory_order_relaxed);
  }
  stop_source(stop_source&&) noexcept = default;
  stop_source& operator=(const stop_source& other) noexcept {
    stop_source copy(other);
    swap(copy);
    return *this;
  }
  stop_source& operator=(stop_source&& other) noexcept {
    if (this != &other) {
      release_source();
      state_ = std::move(other.state_);
    }
    return *this;
  }
  ~stop_source() { release_source(); }
  stop_token get_token() const noexcept { return stop_token(state_); }
  bool stop_possible() const noexcept { return static_cast<bool>(state_); }
  bool stop_requested() const noexcept {
    return state_ && state_->requested.load(std::memory_order_acquire);
  }
  bool request_stop() noexcept {
    return state_ && !state_->requested.exchange(true, std::memory_order_acq_rel);
  }
  void swap(stop_source& other) noexcept { state_.swap(other.state_); }

 private:
  void release_source() noexcept {
    if (state_) state_->sources.fetch_sub(1, std::memory_order_acq_rel);
  }
  std::shared_ptr<detail::stop_state> state_;
};
#else
using std::stop_source;
using std::stop_token;
#endif
}
