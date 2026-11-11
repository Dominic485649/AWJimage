#pragma once

#include "../compat/stdlib_compat.h"
#include "memory_policy.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace awj::parallel_rows_detail {

// budget 是调用方已解析的自动额度或显式线程数。第 0 个工作线程由调用线程承担；不查询
// 硬件，不使用常驻线程池或嵌套任务。此处仅计少量堆/控制区，不含新增的图像级临时空间。
// 操作系统线程栈仍按规划器现有的每个工作线程固定额度计算。
inline constexpr int max_worker_budget = 128;
inline constexpr std::uint64_t worker_control_bytes = 1'024;
inline constexpr std::size_t reduction_block_pixels = 16'384;
inline constexpr std::size_t reduction_max_blocks = max_worker_budget;
inline constexpr std::size_t reduction_state_bytes_limit = 64;
inline constexpr std::uint64_t reduction_control_bytes = reduction_max_blocks * reduction_state_bytes_limit;

struct RunOptions {
  int worker_budget{1};
  // 这里只能借用现有预留中尚未使用的部分。0 表示串行而非不限额；UINT64_MAX 是明确的不限额哨兵值。
  std::uint64_t scratch_limit_bytes{};
  std::size_t minimum_work_per_worker{16'384};
  std::optional<memory_policy::Policy> policy{};
};

inline int worker_count(std::size_t units, std::size_t work_per_unit,
                        const RunOptions& options) noexcept {
  if (!units) return 1;
  const auto work = memory_policy::saturating_multiply(units, work_per_unit);
  const auto useful = std::max<std::uint64_t>(1,
      work / std::max<std::size_t>(1, options.minimum_work_per_worker));
  const auto affordable = options.scratch_limit_bytes / worker_control_bytes;
  return static_cast<int>(std::max<std::uint64_t>(1, std::min<std::uint64_t>(
      {units, useful, affordable, static_cast<std::uint64_t>(std::clamp(options.worker_budget, 1, max_worker_budget))})));
}

struct ThreadLauncher {
  template <class Work>
  awj::compat::scoped_worker launch(Work&& work) const {
    return awj::compat::scoped_worker(std::forward<Work>(work));
  }
};

enum class Failure { none, callback, memory, length, worker };

class FailureState {
 public:
  Failure reason() const noexcept { return reason_.load(std::memory_order_relaxed); }
  bool record(Failure reason) noexcept {
    auto empty = Failure::none;
    return reason_.compare_exchange_strong(empty, reason, std::memory_order_relaxed);
  }
  void capture(std::string& message) noexcept {
    if (record(Failure::callback)) message_.swap(message);
  }
  // 仅 CAS 获胜者写入 message_；所有线程汇合后由所有者读取。
  std::string take_message() noexcept { return std::move(message_); }
 private:
  std::atomic<Failure> reason_{Failure::none};
  std::string message_;
};

// 每个单元独占互不重叠的输出行（或完整的色度对齐行块）。回调及其引用的所有存储
// 必须存活到所有线程汇合之后，包括启动失败路径。部分启动仍共用同一队列，不会漏掉
// 任何分区。启动器注入仅供内部使用，并用于测试部分启动路径。
template <class Function, class Launcher = ThreadLauncher>
awj::compat::expected<void, std::string> for_each(std::size_t units,
    std::size_t work_per_unit, const RunOptions& options, awj::compat::stop_token stop,
    const Function& function, const Launcher& launcher = {}) {
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  const int workers = worker_count(units, work_per_unit, options);
  FailureState failure;
  std::atomic<std::size_t> next{};
  const auto work = [&]() noexcept {
    try {
      while (!stop.stop_requested() && failure.reason() == Failure::none) {
        auto unit = next.load(std::memory_order_relaxed);
        while (unit < units && !next.compare_exchange_weak(unit, unit + 1, std::memory_order_relaxed)) {}
        if (unit >= units) break;
        if (auto result = function(unit); !result) { failure.capture(result.error()); break; }
      }
    } catch (const std::bad_alloc&) { failure.record(Failure::memory); }
      catch (const std::length_error&) { failure.record(Failure::length); }
      catch (...) { failure.record(Failure::worker); }
  };
  // 在线程句柄创建前构造控制状态、失败状态和闭包。即使非标准启动器启动部分工作线程后
  // 又意外失败，RAII 也会等待这些线程结束。
  std::vector<awj::compat::scoped_worker> threads;
  if (workers > 1) {
    try {
      const auto policy = options.policy ? *options.policy : memory_policy::current();
      threads.reserve(static_cast<std::size_t>(workers - 1));
      for (int i = 1; i < workers && !stop.stop_requested() && failure.reason() == Failure::none; ++i) {
        threads.push_back(launcher.launch([&, policy]() noexcept {
          const memory_policy::Scope memory_scope{policy};
          work();
        }));
      }
    } catch (const std::bad_alloc&) {
      // 分配或启动线程受压时回退为串行或部分线程池，不改变质量；调用方会处理所有仍未
      // 认领的单元。
    } catch (const std::length_error&) {
    } catch (const std::system_error&) {
    } catch (...) { failure.record(Failure::worker); }
  }
  work();
  for (auto& thread : threads) thread.join();
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  switch (failure.reason()) {
    case Failure::none: return {};
    case Failure::callback: return awj::compat::unexpected{failure.take_message()};
    case Failure::memory: return awj::compat::unexpected{"像素 worker 内存不足。"};
    case Failure::length: return awj::compat::unexpected{"像素 worker 工作区超过限制。"};
    case Failure::worker: return awj::compat::unexpected{"像素 worker 处理失败。"};
  }
  return awj::compat::unexpected{"像素 worker 状态无效。"};
}

// 分区仅依赖尺寸并按升序归并；结果不受工作线程数、调度或内存回退影响。将 16K 对齐的
// 基础块分配给最多 128 个逻辑分区。整轮共用一组有作用域的工作线程，避免大图的每个小批次
// 都重新启动线程。最多使用 128 个小型状态对象，不分配整图或每线程像素缓冲区。
template <class State, class Calculate, class Merge>
awj::compat::expected<State, std::string> reduce(std::size_t pixels,
    RunOptions options, awj::compat::stop_token stop,
    const Calculate& calculate, const Merge& merge) {
  static_assert(std::is_trivially_copyable_v<State> && std::is_nothrow_default_constructible_v<State> &&
      sizeof(State) <= reduction_state_bytes_limit, "Row reductions require small, non-owning value states.");
  static_assert(std::is_nothrow_invocable_v<const Merge&, State&, const State&>,
      "Row reduction merges must not throw.");
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  State total{};
  if (!pixels) return total;
  const auto blocks = pixels / reduction_block_pixels + (pixels % reduction_block_pixels != 0);
  const auto units = std::min(blocks, reduction_max_blocks);
  const auto work_per_unit = pixels / units + (pixels % units != 0);
  const auto evaluate = [&](std::size_t unit, State& state) {
    const auto quotient = blocks / units, remainder = blocks % units;
    const auto first_block = unit * quotient + std::min(unit, remainder);
    const auto end_block = first_block + quotient + (unit < remainder);
    const auto first = first_block * reduction_block_pixels;
    const auto end = end_block == blocks ? pixels : end_block * reduction_block_pixels;
    return calculate(first, end, state);
  };
  const auto serial = [&]() -> awj::compat::expected<State, std::string> {
    auto single = options;
    single.worker_budget = 1;
    auto result = for_each(units, work_per_unit, single, stop,
        [&](std::size_t unit) -> awj::compat::expected<void, std::string> {
          State state{};
          if (auto evaluated = evaluate(unit, state); !evaluated) return awj::compat::unexpected{evaluated.error()};
          merge(total, state);
          return {};
        });
    if (!result) return awj::compat::unexpected{result.error()};
    return total;
  };
  const auto state_bytes = sizeof(State) * units;
  if (options.scratch_limit_bytes < state_bytes + 2 * worker_control_bytes ||
      worker_count(units, work_per_unit, options) == 1) return serial();
  // 仅在准入后分配。将状态放在堆上，避免零暂存串行回退时仍无条件使用
  // 大型栈帧或触发栈探测。
  std::unique_ptr<State[]> states;
  try { states = std::make_unique<State[]>(units); }
  catch (const std::bad_alloc&) { return serial(); }
  options.scratch_limit_bytes -= state_bytes;
  auto result = for_each(units, work_per_unit, options, stop,
      [&](std::size_t unit) -> awj::compat::expected<void, std::string> {
        return evaluate(unit, states[unit]);
      });
  if (!result) return awj::compat::unexpected{result.error()};
  for (std::size_t unit = 0; unit < units; ++unit) merge(total, states[unit]);
  return total;
}

}  // namespace awj::parallel_rows_detail
