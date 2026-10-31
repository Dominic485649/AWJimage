#pragma once
#include "stop_compat.h"
#include <thread>
#include <type_traits>
#include <utility>

namespace awj::compat {
#if AWJ_USE_PORTABLE_STDLIB
// 只管理 worker 所有权；取消由调用方传入的同一 token 决定。
class scoped_worker {
 public:
  scoped_worker() noexcept = default;
  template <class Function, class... Args>
    requires (!std::is_same_v<std::remove_cvref_t<Function>, scoped_worker>)
  explicit scoped_worker(Function&& function, Args&&... args)
      : thread_(std::forward<Function>(function), std::forward<Args>(args)...) {}
  scoped_worker(const scoped_worker&) = delete;
  scoped_worker& operator=(const scoped_worker&) = delete;
  scoped_worker(scoped_worker&&) noexcept = default;
  scoped_worker& operator=(scoped_worker&& other) noexcept {
    if (this != &other) {
      if (thread_.joinable()) thread_.join();
      thread_ = std::move(other.thread_);
    }
    return *this;
  }
  ~scoped_worker() {
    if (thread_.joinable()) thread_.join();
  }
  bool joinable() const noexcept { return thread_.joinable(); }
  void join() { thread_.join(); }

 private:
  std::thread thread_;
};
#else
using scoped_worker = std::jthread;
#endif
}
