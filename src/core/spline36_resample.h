#pragma once
#include "../compat/stdlib_compat.h"
#include "hdr_cost.h"
#include "memory_policy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace awj::spline36_detail {

using Pixel = std::array<double, 4>;

struct Sample {
  std::size_t first{};
  std::vector<double> weights;
};

inline bool valid_axis(std::size_t source, std::size_t target) noexcept {
  // 图像 API 的尺寸范围是 uint32；直接调用此内部辅助函数时，
  // 也必须保证后续 double 到索引的转换精确可表示。
  return source && target && source <= UINT32_MAX && target <= UINT32_MAX;
}

inline std::size_t support_bound(std::size_t source, std::size_t target) noexcept {
  if (!valid_axis(source, target)) return 0;
  const auto six_source = static_cast<std::uint64_t>(source) * 6;
  return static_cast<std::size_t>(std::min<std::uint64_t>(source,
      std::max<std::uint64_t>(6, six_source / target + (six_source % target != 0)) + 2));
}

inline std::uint64_t axis_bytes(std::size_t source, std::size_t target) noexcept {
  if (!valid_axis(source, target)) return UINT64_MAX;
  return hdr_cost::multiply(target, hdr_cost::add(sizeof(Sample),
      hdr_cost::multiply(support_bound(source, target), sizeof(double))));
}

// 这些是存储预约量，不是 RSS、分配器或操作系统线程栈的硬上限。
// 准入、分配回退和估算使用同一套字节计算规则。
inline constexpr std::uint64_t worker_control_bytes = 1'024;
inline constexpr std::uint64_t shared_control_bytes = 1'024;
inline constexpr std::uint64_t pixels_per_worker = 16'384;

struct ScratchPlan {
  int workers{};
  std::size_t cache_rows{};
  bool cache_source{};
  std::uint64_t coefficients{};
  std::uint64_t bytes{UINT64_MAX};
};

inline std::uint64_t worker_bytes(std::size_t width, std::size_t target_width,
    std::size_t rows, bool cache_source) noexcept {
  const auto row_bytes = hdr_cost::multiply(target_width, sizeof(Pixel));
  return hdr_cost::add(worker_control_bytes, hdr_cost::add(row_bytes,
      hdr_cost::add(hdr_cost::multiply(rows, hdr_cost::add(row_bytes, sizeof(std::size_t))),
          cache_source ? hdr_cost::multiply(width, sizeof(Pixel)) : 0)));
}

inline void update_bytes(ScratchPlan& plan, std::size_t width, std::size_t target_width) noexcept {
  plan.bytes = hdr_cost::add(hdr_cost::add(plan.coefficients, shared_control_bytes),
      hdr_cost::multiply(static_cast<std::uint64_t>(plan.workers),
          worker_bytes(width, target_width, plan.cache_rows, plan.cache_source)));
}

inline int worker_limit(std::size_t width, std::size_t height,
    std::size_t target_width, std::size_t target_height, int budget) noexcept {
  const auto work = std::max(hdr_cost::multiply(width, height), hdr_cost::multiply(target_width, target_height));
  return static_cast<int>(std::min<std::uint64_t>({static_cast<std::uint64_t>(std::max(1, budget)),
      target_height, std::max<std::uint64_t>(1, work / pixels_per_worker)}));
}

// UINT64_MAX 表示不限制 scratch；0 表示预算耗尽，绝不表示无限制。
// 纵向系数表的精确支撑可收紧分配前的保守上界；单个目标行不能复用水平行缓存。
inline ScratchPlan make_plan(std::size_t width, std::size_t height,
    std::size_t target_width, std::size_t target_height, int worker_budget = 1,
    std::uint64_t available = UINT64_MAX, std::size_t vertical_support = 0) noexcept {
  ScratchPlan plan;
  if (!valid_axis(width, target_width) || !valid_axis(height, target_height)) return plan;
  plan.coefficients = hdr_cost::add(axis_bytes(width, target_width), axis_bytes(height, target_height));
  const auto fixed = hdr_cost::add(plan.coefficients, shared_control_bytes);
  if (fixed == UINT64_MAX || fixed > available) return plan;
  const auto remaining = available - fixed;
  const auto support = support_bound(height, target_height);
  const auto maximum_workers = worker_limit(width, height, target_width, target_height, worker_budget);
  plan.cache_rows = std::min(support, vertical_support ? vertical_support : support);
  // 单个目标行分区不能复用水平行；大幅缩小时若支撑覆盖整个源图高度，
  // 同样逐行流式处理，避免局部缓存退化为整幅浮点中间图。
  if (target_height == static_cast<std::size_t>(maximum_workers) || support == height) plan.cache_rows = 1;
  plan.cache_source = true;
  const auto full_worker = worker_bytes(width, target_width, plan.cache_rows, true);
  const auto fitting = remaining / full_worker;
  if (fitting) {
    plan.workers = static_cast<int>(std::min<std::uint64_t>(maximum_workers, fitting));
  } else {
    // 先降低并发，再减少水平行复用，最后关闭源行缓存。
    // 最终逐像素读取路径保留原来的逐 tap 运算顺序。
    plan.workers = 1;
    for (const bool source_cache : {true, false}) {
      const auto base = worker_bytes(width, target_width, 0, source_cache);
      const auto row = hdr_cost::add(hdr_cost::multiply(target_width, sizeof(Pixel)), sizeof(std::size_t));
      const auto rows = remaining > base ? (remaining - base) / row : 0;
      if (rows) {
        plan.cache_source = source_cache;
        plan.cache_rows = static_cast<std::size_t>(std::min<std::uint64_t>(plan.cache_rows, rows));
        update_bytes(plan, width, target_width);
        return plan;
      }
    }
    return ScratchPlan{};
  }
  update_bytes(plan, width, target_width);
  if (plan.bytes == UINT64_MAX || plan.bytes > available) return ScratchPlan{};
  return plan;
}

inline std::uint64_t scratch_bytes(std::size_t width, std::size_t height,
    std::size_t target_width, std::size_t target_height, int worker_budget = 1) noexcept {
  return make_plan(width, height, target_width, target_height, worker_budget).bytes;
}

inline double kernel(double x) noexcept {
  x = std::abs(x);
  // 半径 3 的分段三次 Spline36；缩小时由系数生成器扩大支撑。
  if (x < 1) return ((13.0 / 11 * x - 453.0 / 209) * x - 3.0 / 209) * x + 1;
  if (x < 2) {
    x -= 1;
    return ((-6.0 / 11 * x + 270.0 / 209) * x - 156.0 / 209) * x;
  }
  if (x < 3) {
    x -= 2;
    return ((1.0 / 11 * x - 45.0 / 209) * x + 26.0 / 209) * x;
  }
  return 0;
}

inline awj::compat::expected<std::vector<Sample>, std::string> make_axis(
    std::size_t source, std::size_t target, awj::compat::stop_token stop) {
  if (!valid_axis(source, target)) return awj::compat::unexpected{"Spline36 轴尺寸无效。"};
  std::vector<Sample> axis;
  axis.reserve(target);
  const auto step = static_cast<double>(source) / static_cast<double>(target);
  const auto support = std::max(1.0, step);
  for (std::size_t x = 0; x < target; ++x) {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    const auto center = (static_cast<double>(x) + 0.5) * step - 0.5;
    const auto first = static_cast<std::size_t>(std::max(0.0, std::ceil(center - 3 * support)));
    const auto last = static_cast<std::size_t>(std::min(static_cast<double>(source - 1), std::floor(center + 3 * support)));
    Sample sample{.first = first};
    sample.weights.reserve(last - first + 1);
    double sum{};
    for (auto p = first; p <= last; ++p) {
      if ((p - first) % 4096 == 0 && stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
      const auto value = kernel((static_cast<double>(p) - center) / support);
      sample.weights.push_back(value); sum += value;
    }
    if (!std::isfinite(sum) || std::abs(sum) < 1e-12)
      return awj::compat::unexpected{"Spline36 滤波系数无效。"};
    for (auto& value : sample.weights) value /= sum;
    axis.push_back(std::move(sample));
  }
  return axis;
}

inline std::uint64_t axis_storage_bytes(const std::vector<Sample>& axis) noexcept {
  auto bytes = hdr_cost::multiply(axis.capacity(), sizeof(Sample));
  for (const auto& sample : axis) bytes = hdr_cost::add(bytes, hdr_cost::multiply(sample.weights.capacity(), sizeof(double)));
  return bytes;
}

struct WorkerScratch {
  std::vector<Pixel> source;
  std::vector<Pixel> horizontal;
  std::vector<Pixel> accumulated;
  std::vector<std::size_t> row_ids;

  std::uint64_t storage_bytes() const noexcept {
    auto bytes = hdr_cost::add(hdr_cost::multiply(source.capacity(), sizeof(Pixel)),
        hdr_cost::multiply(horizontal.capacity(), sizeof(Pixel)));
    bytes = hdr_cost::add(bytes, hdr_cost::multiply(accumulated.capacity(), sizeof(Pixel)));
    return hdr_cost::add(bytes, hdr_cost::multiply(row_ids.capacity(), sizeof(std::size_t)));
  }
};

struct WorkerOperations {
  void allocate(WorkerScratch& scratch, std::size_t width, std::size_t target_width, const ScratchPlan& plan) const {
    if (plan.cache_rows > SIZE_MAX / target_width) throw std::length_error{"Spline36 row cache"};
    if (plan.cache_source) scratch.source.resize(width);
    scratch.horizontal.resize(plan.cache_rows * target_width);
    scratch.accumulated.resize(target_width);
    scratch.row_ids.assign(plan.cache_rows, SIZE_MAX);
  }
  template <class Work>
  awj::compat::scoped_worker launch(Work&& work) const {
    return awj::compat::scoped_worker(std::forward<Work>(work));
  }
};

struct RunOptions {
  int worker_budget{1};
  std::uint64_t scratch_limit_bytes{UINT64_MAX};
  std::optional<memory_policy::Policy> policy{};
};

struct FilterDiagnostics {
  int workers{}; // 实际参与者数量（含调用线程）；失败时为 0。
  std::uint64_t scratch_bytes{}; // 包含回退尝试的计划存储峰值。
};

enum class Failure { none, input, write, memory, length, worker };

class FailureState {
 public:
  Failure reason() const noexcept { return reason_.load(std::memory_order_relaxed); }
  bool record(Failure reason) noexcept {
    auto empty = Failure::none;
    return reason_.compare_exchange_strong(empty, reason, std::memory_order_relaxed);
  }
  void capture(std::string& message) noexcept {
    if (record(Failure::write)) message_.swap(message);
  }
  // 只有 CAS 获胜者写入 message_；调用线程在所有 join 完成后读取。
  std::string take_message() noexcept { return std::move(message_); }
 private:
  std::atomic<Failure> reason_{Failure::none};
  std::string message_;
};

inline bool reduce_plan(ScratchPlan& plan, std::size_t width, std::size_t target_width) noexcept {
  if (plan.workers > 1) plan.workers = std::max(1, plan.workers / 2);
  else if (plan.cache_rows > 1) plan.cache_rows = std::max<std::size_t>(1, plan.cache_rows / 2);
  else if (plan.cache_source) plan.cache_source = false;
  else return false;
  update_bytes(plan, width, target_width);
  return true;
}

// read/write 在返回前借用源与目标；read 须支持并发只读访问，write 只触及指定像素。
// 每个分区独占连续目标行和私有 scratch；回调及存储的生命周期覆盖所有 worker 的 join。
// Operations 是每次调用独立的分配/启动故障测试接口，不使用进程全局故障开关。
template <class Read, class Write, class Operations = WorkerOperations>
awj::compat::expected<void, std::string> filter(std::size_t width, std::size_t height,
    std::size_t target_width, std::size_t target_height, Read read, Write write,
    awj::compat::stop_token stop, RunOptions options = {}, FilterDiagnostics* diagnostics = nullptr,
    Operations operations = {}) {
  if (diagnostics) *diagnostics = {};
  try {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    auto plan = make_plan(width, height, target_width, target_height,
        options.worker_budget, options.scratch_limit_bytes);
    if (!plan.workers) return awj::compat::unexpected{"Spline36 必要工作区超过内存预算。"};
    const auto policy = options.policy ? *options.policy : memory_policy::current();
    const memory_policy::Scope caller_memory_scope{policy};
    auto horizontal = make_axis(width, target_width, stop);
    if (!horizontal) return awj::compat::unexpected{horizontal.error()};
    auto vertical = make_axis(height, target_height, stop);
    if (!vertical) return awj::compat::unexpected{vertical.error()};
    if (hdr_cost::add(axis_storage_bytes(*horizontal), axis_storage_bytes(*vertical)) > plan.coefficients)
      return awj::compat::unexpected{"Spline36 系数容量超过预约。"};
    std::size_t support{};
    for (const auto& sample : *vertical) support = std::max(support, sample.weights.size());
    plan = make_plan(width, height, target_width, target_height,
        options.worker_budget, options.scratch_limit_bytes, support);
    if (!plan.workers) return awj::compat::unexpected{"Spline36 必要工作区超过内存预算。"};
    const auto peak_scratch = plan.bytes;
    std::vector<WorkerScratch> scratch;
    for (;;) {
      if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
      try {
        scratch.resize(static_cast<std::size_t>(plan.workers));
        auto storage = hdr_cost::multiply(scratch.capacity(), sizeof(WorkerScratch));
        for (auto& buffers : scratch) {
          operations.allocate(buffers, width, target_width, plan);
          storage = hdr_cost::add(storage, buffers.storage_bytes());
        }
        storage = hdr_cost::add(storage, hdr_cost::multiply(static_cast<std::uint64_t>(plan.workers), sizeof(awj::compat::scoped_worker)));
        if (storage > hdr_cost::multiply(static_cast<std::uint64_t>(plan.workers),
            worker_bytes(width, target_width, plan.cache_rows, plan.cache_source)))
          throw std::length_error{"Spline36 scratch capacity exceeds reservation"};
        break;
      } catch (const std::bad_alloc&) {
        std::vector<WorkerScratch>{}.swap(scratch);
        if (!reduce_plan(plan, width, target_width)) return awj::compat::unexpected{"Spline36 工作区内存不足。"};
      } catch (const std::length_error&) {
        std::vector<WorkerScratch>{}.swap(scratch);
        if (!reduce_plan(plan, width, target_width)) return awj::compat::unexpected{"Spline36 工作区超过限制。"};
      }
    }
    FailureState failure;
    const auto stopped = [&] { return stop.stop_requested() || failure.reason() != Failure::none; };
    const auto work = [&](int partition, WorkerScratch& buffers) noexcept {
      try {
        const auto parts = static_cast<std::size_t>(plan.workers);
        const auto index = static_cast<std::size_t>(partition);
        const auto first = target_height / parts * index + std::min(index, target_height % parts);
        const auto end = first + target_height / parts + (index < target_height % parts);
        for (auto y = first; y < end; ++y) {
          if (stopped()) return;
          awj::compat::ranges::fill(buffers.accumulated, Pixel{});
          const auto& ys = (*vertical)[y];
          for (std::size_t v = 0; v < ys.weights.size(); ++v) {
            if (stopped()) return;
            const auto sy = ys.first + v;
            const auto slot = sy % plan.cache_rows;
            auto* row = buffers.horizontal.data() + slot * target_width;
            if (buffers.row_ids[slot] != sy) {
              if (plan.cache_source) for (std::size_t x = 0; x < width; ++x) {
                if (x % 256 == 0 && stopped()) return;
                const auto rgba = read(x, sy);
                for (const auto value : rgba) if (!std::isfinite(value)) { failure.record(Failure::input); return; }
                for (std::size_t c = 0; c < 4; ++c) buffers.source[x][c] = rgba[c];
              }
              for (std::size_t x = 0; x < target_width; ++x) {
                if (x % 256 == 0 && stopped()) return;
                Pixel sum{};
                const auto& xs = (*horizontal)[x];
                for (std::size_t h = 0; h < xs.weights.size(); ++h) {
                  if (h % 4096 == 0 && stopped()) return;
                  Pixel rgba{};
                  if (plan.cache_source) rgba = buffers.source[xs.first + h];
                  else {
                    const auto sample = read(xs.first + h, sy);
                    for (const auto value : sample) if (!std::isfinite(value)) { failure.record(Failure::input); return; }
                    for (std::size_t c = 0; c < 4; ++c) rgba[c] = sample[c];
                  }
                  const auto alpha = std::clamp(static_cast<double>(rgba[3]), 0.0, 1.0);
                  for (std::size_t c = 0; c < 3; ++c) sum[c] += rgba[c] * alpha * xs.weights[h];
                  sum[3] += alpha * xs.weights[h];
                }
                row[x] = sum;
              }
              buffers.row_ids[slot] = sy;
            }
            for (std::size_t x = 0; x < target_width; ++x) {
              if (x % 256 == 0 && stopped()) return;
              for (std::size_t c = 0; c < 4; ++c) buffers.accumulated[x][c] += row[x][c] * ys.weights[v];
            }
          }
          for (std::size_t x = 0; x < target_width; ++x) {
            if (x % 256 == 0 && stopped()) return;
            auto rgba = buffers.accumulated[x];
            const auto alpha = rgba[3];
            for (std::size_t c = 0; c < 3; ++c) rgba[c] = alpha > 1e-12 ? rgba[c] / alpha : 0;
            rgba[3] = std::clamp(alpha, 0.0, 1.0);
            if (auto result = write(x, y, rgba); !result) { failure.capture(result.error()); return; }
          }
        }
      } catch (const std::bad_alloc&) { failure.record(Failure::memory); }
        catch (const std::length_error&) { failure.record(Failure::length); }
        catch (...) { failure.record(Failure::worker); }
    };
    // scratch 和错误状态先于 RAII 句柄构造，启动器意外抛出时也能安全回收；
    // 不 detach，也不遗漏尚未启动的分区。
    std::vector<awj::compat::scoped_worker> threads;
    try {
      threads.reserve(static_cast<std::size_t>(plan.workers - 1));
      for (int i = 1; i < plan.workers && !stopped(); ++i)
        threads.push_back(operations.launch([&, i, policy]() noexcept {
          const memory_policy::Scope memory_scope{policy};
          work(i, scratch[static_cast<std::size_t>(i)]);
        }));
    } catch (const std::system_error&) {
      // 调用线程补齐所有未能启动线程的分区。
    } catch (const std::bad_alloc&) {
    } catch (const std::length_error&) {
    } catch (...) { failure.record(Failure::worker); }
    work(0, scratch[0]);
    for (auto i = static_cast<int>(threads.size()) + 1; i < plan.workers && !stopped(); ++i) work(i, scratch[0]);
    for (auto& thread : threads) thread.join();
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    switch (failure.reason()) {
      case Failure::input: return awj::compat::unexpected{"Spline36 输入含非有限像素。"};
      case Failure::write: return awj::compat::unexpected{failure.take_message()};
      case Failure::memory: return awj::compat::unexpected{"Spline36 worker 内存不足。"};
      case Failure::length: return awj::compat::unexpected{"Spline36 worker 工作区超过限制。"};
      case Failure::worker: return awj::compat::unexpected{"Spline36 worker 处理失败。"};
      case Failure::none: break;
    }
    if (diagnostics) *diagnostics = {static_cast<int>(threads.size()) + 1, peak_scratch};
    return {};
  } catch (const std::bad_alloc&) { return awj::compat::unexpected{"Spline36 工作区内存不足。"}; }
    catch (const std::length_error&) { return awj::compat::unexpected{"Spline36 工作区超过限制。"}; }
    catch (const std::exception&) { return awj::compat::unexpected{"Spline36 工作区准备失败。"}; }
    catch (...) { return awj::compat::unexpected{"Spline36 工作区准备失败。"}; }
}

}  // namespace awj::spline36_detail
