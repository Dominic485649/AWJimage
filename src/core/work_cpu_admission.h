#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace awj {

// 每批次使用一个准入账本。budget 是调用方已解析的自动额度或显式线程数，不重新
// 查询硬件并发数，也不另行应用自动策略。额度限制已准入的 CPU 工作量，而非操作系统
// 线程数；等待中的工作线程不占额度。
//
// 先取得内存票据，再申请 CPU。runnable_hint 包含当前 CPU 持有者，以及在现有内存和
// 工作线程容量内预计可运行的其他文件；它不是全部待处理文件数。启动工作线程前设置，
// 并在内存准入、精化、释放、任务完成及工作线程部分启动失败后更新。内存阻塞的文件
// 不计入提示。调用方应根据批次协同的内存状态计算提示，不得按当前活动工作线程数单独
// 均分预算。
//
// 公平份额按需求封顶并采用整数最大最小分配，剩余额度按准入顺序分发。预计但尚未
// 启动的文件也预留公平份额；实际 FIFO 等待者还会为持有者扩额至少预留一个单位。
// 这些预留不计入已使用的 CPU 额度。降低提示后，下一次 try_resize() 可借用末尾余量。
// 提示或队列变化不会撤销或静默扩大运行中的租约；新任务可能要等当前编码器到达安全边界。
//
// 此处不创建或调用工作线程、执行器或应用回调。acquire() 是唯一会等待容量的操作。
// 等待时（包括等待内存）调用方不得持有其他 CPU 租约。取得 CPU 后扩大内存票据必须
// 非阻塞：失败时依据 try_resize() 返回的实际额度缩至内存安全需求，或 reset() 并回退
// 该任务。持有 CPU 租约或保留已解码像素时，绝不等待内存扩额。
class WorkCpuAdmission final {
 public:
  static constexpr int max_budget = 128;

  struct Snapshot {
    int budget{};
    int used{};
    int active_leases{};
    std::size_t waiting{};
    int runnable_hint{};
  };

 private:
  struct Slot {
    int units{};
    int demand{};
  };

  struct State {
    State(int limit, std::size_t hint)
        : budget{limit}, runnable_hint{clamp_hint(hint)} {}

    [[nodiscard]] int clamp_hint(std::size_t hint) const noexcept {
      return static_cast<int>(std::min(hint, static_cast<std::size_t>(budget)));
    }

    [[nodiscard]] int participants() const noexcept {
      const auto known = static_cast<std::size_t>(active) + waiting.size();
      return std::max(runnable_hint, clamp_hint(known));
    }

    [[nodiscard]] int rank_of(std::size_t slot) const noexcept {
      for (int rank = 0; rank < active; ++rank)
        if (order[static_cast<std::size_t>(rank)] == slot) return rank;
      assert(false && "CPU lease is absent from its ledger");
      return 0;
    }

    // 候选项和已授予额度最多各 128 个；不分配内存，也不调用回调。
    [[nodiscard]] int fair_share(int rank, int count) const noexcept {
      assert(rank >= 0 && rank < count && count <= budget);
      std::array<int, max_budget> demands{};
      std::array<int, max_budget> shares{};
      int cursor = 0;
      for (; cursor < active; ++cursor)
        demands[static_cast<std::size_t>(cursor)] =
            slots[order[static_cast<std::size_t>(cursor)]].demand;
      for (const int demand : waiting) {
        if (cursor == count) break;
        demands[static_cast<std::size_t>(cursor++)] = demand;
      }
      while (cursor < count)
        demands[static_cast<std::size_t>(cursor++)] = budget;

      int remaining = budget;
      while (remaining > 0) {
        bool advanced = false;
        for (int i = 0; i < count && remaining > 0; ++i) {
          const auto index = static_cast<std::size_t>(i);
          if (shares[index] < demands[index]) {
            ++shares[index];
            --remaining;
            advanced = true;
          }
        }
        if (!advanced) break;  // All known demands are satisfied.
      }
      return shares[static_cast<std::size_t>(rank)];
    }

    const int budget;
    int runnable_hint{};
    int used{};
    int active{};
    std::array<Slot, max_budget> slots{};
    std::array<std::size_t, max_budget> order{};
    std::list<int> waiting{};
    std::mutex mutex{};
    std::condition_variable_any cv{};
  };

  // 声明在 acquire 锁之后，确保无论取消还是等待异常，析构时都在锁仍持有期间移除请求。
  struct WaitingRequest {
    WaitingRequest(State& owner, int demand)
        : state{owner}, position{state.waiting.insert(state.waiting.end(), demand)} {}
    ~WaitingRequest() {
      state.waiting.erase(position);
      state.cv.notify_all();
    }
    WaitingRequest(const WaitingRequest&) = delete;
    WaitingRequest& operator=(const WaitingRequest&) = delete;

    State& state;
    std::list<int>::iterator position;
  };

  static int checked_budget(int budget) {
    if (budget < 1 || budget > max_budget)
      throw std::invalid_argument("CPU admission budget must be in [1, 128]");
    return budget;
  }

 public:
  // Lease 由单一所有者使用，同一租约不可并发调用；不同租约和准入门面由共享账本同步。
  // State 由租约持有而非借用，因此未归还的租约可晚于门面销毁。
  // 门面必须晚于其自身正在执行的调用。
  class Lease final {
   public:
    Lease() noexcept = default;
    ~Lease() { reset(); }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept
        : state_{std::move(other.state_)}, slot_{other.slot_},
          units_{std::exchange(other.units_, 0)} {}
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        slot_ = other.slot_;
        units_ = std::exchange(other.units_, 0);
      }
      return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return units_ != 0; }
    [[nodiscard]] int threads() const noexcept { return units_; }

    // 仅在安全边界调用：使用旧额度的所有工作都必须已完成，相关线程均已等待结束。
    // 不等待容量，只获取记账互斥锁。返回新额度（不是成功/失败布尔值）；公平分配可能
    // 缩小额度或只部分扩大。需求规范到 [1, budget]；reset() 表示不再执行 CPU 工作。
    // 即使当前只获部分额度，也会记住需求。
    //
    // 取消采用协作式处理：此处观察到停止请求时会释放租约并返回 0。编解码执行期间收到
    // 停止请求不会释放仍在使用的 CPU；调整或重置前必须先等待执行结束并完成清理。
    [[nodiscard]] int try_resize(int requested, std::stop_token stop = {}) noexcept {
      if (!state_) return 0;
      const auto state = state_;
      std::unique_lock lock{state->mutex};
      if (stop.stop_requested()) {
        lock.unlock();
        reset();
        return 0;
      }
      auto& slot = state->slots[slot_];
      slot.demand = std::clamp(requested, 1, state->budget);
      const int count = state->participants();
      const int target = state->fair_share(state->rank_of(slot_), count);
      const int available = state->budget - (state->used - units_);
      // 持有者不可抢占，其当前额度可能高于新的公平份额。
      // 即便如此，也不得占用等待者或未来文件最后可用的额度。
      const int floor = count - state->active;
      const int granted = std::min(target, std::max(1, available - floor));
      state->used += granted - units_;
      slot.units = units_ = granted;
      lock.unlock();
      state->cv.notify_all();
      return granted;
    }

    // 无 CPU 工作时开始阻塞等待或 I/O 前，以及任务失败时，也应调用。
    // 线程启动失败时，销毁已移动的启动闭包会归还其额度。
    void reset() noexcept {
      auto state = std::move(state_);
      if (!state) return;
      {
        std::lock_guard lock{state->mutex};
        assert(state->slots[slot_].units == units_);
        const int rank = state->rank_of(slot_);
        state->used -= std::exchange(units_, 0);
        state->slots[slot_] = {};
        --state->active;
        for (int i = rank; i < state->active; ++i)
          state->order[static_cast<std::size_t>(i)] =
              state->order[static_cast<std::size_t>(i + 1)];
      }
      state->cv.notify_all();
    }

   private:
    friend class WorkCpuAdmission;
    Lease(std::shared_ptr<State> state, std::size_t slot, int units) noexcept
        : state_{std::move(state)}, slot_{slot}, units_{units} {}

    std::shared_ptr<State> state_{};
    std::size_t slot_{};
    int units_{};
  };

  explicit WorkCpuAdmission(int budget, std::size_t runnable_hint = 1)
      : state_{std::make_shared<State>(checked_budget(budget), runnable_hint)} {}
  WorkCpuAdmission(const WorkCpuAdmission&) = delete;
  WorkCpuAdmission& operator=(const WorkCpuAdmission&) = delete;
  WorkCpuAdmission(WorkCpuAdmission&&) = delete;
  WorkCpuAdmission& operator=(WorkCpuAdmission&&) = delete;

  // 排队期间不占 CPU 额度。先入先出（FIFO）防止后到的申请越过等待者。最多允许 128 个等待者
  // （另加已准入的持有者）；超出时抛出 length_error。分配或等待失败会向上传播，且不
  // 保留请求或额度。已收到停止请求时直接返回 nullopt。
  [[nodiscard]] std::optional<Lease> acquire(int requested, std::stop_token stop = {}) {
    const auto state = state_;
    std::unique_lock lock{state->mutex};
    if (stop.stop_requested()) return std::nullopt;
    if (state->waiting.size() >= static_cast<std::size_t>(max_budget))
      throw std::length_error("Too many CPU admission waiters");
    WaitingRequest request{*state, std::clamp(requested, 1, state->budget)};
    state->cv.wait(lock, stop, [&] {
      return request.position == state->waiting.begin() && state->used < state->budget;
    });
    if (stop.stop_requested()) return std::nullopt;

    const int count = state->participants();
    const int target = state->fair_share(state->active, count);
    const int available = state->budget - state->used;
    const int future_floor = count - state->active - 1;
    const int granted = std::min(target, std::max(1, available - future_floor));
    std::size_t slot = 0;
    while (slot < state->slots.size() && state->slots[slot].units != 0) ++slot;
    assert(slot < state->slots.size());
    state->slots[slot] = {.units = granted, .demand = *request.position};
    state->order[static_cast<std::size_t>(state->active++)] = slot;
    state->used += granted;
    // Lease 和 optional 的移动不抛异常；request 会在解锁前从等待队列移除。
    return Lease{state, slot, granted};
  }

  // 没有待运行文件时可设为 0；实际持有者和等待者始终构成下限。过高的旧提示只会让
  // 配额空闲，不会伪造活动租约。工作线程部分启动失败时也要更新。
  void set_runnable_hint(std::size_t hint) noexcept {
    {
      std::lock_guard lock{state_->mutex};
      state_->runnable_hint = state_->clamp_hint(hint);
    }
    state_->cv.notify_all();
  }

  [[nodiscard]] Snapshot snapshot() const noexcept {
    std::lock_guard lock{state_->mutex};
    return {.budget = state_->budget, .used = state_->used,
            .active_leases = state_->active, .waiting = state_->waiting.size(),
            .runnable_hint = state_->runnable_hint};
  }

 private:
  const std::shared_ptr<State> state_;
};

}  // namespace awj
