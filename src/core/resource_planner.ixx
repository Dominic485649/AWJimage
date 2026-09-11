module;

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <string>

export module awj.resource_planner;

import awj.encoding_defaults;

export namespace awj {

struct MemoryStatus {
  std::uint64_t total_bytes{};
  std::uint64_t available_bytes{};
};

struct ResourcePlanRequest {
  int automatic_thread_budget{1};
  int file_count{1};
  std::uint64_t memory_limit_bytes{};
  std::uint64_t estimated_bytes_per_file{};
};

struct ResourcePlan {
  int file_parallelism{1};
  int encoder_threads_per_file{1};
  int global_thread_budget{1};
  std::uint64_t memory_limit_bytes{};
  int memory_file_parallelism{1};
};

// 自动内存上限取 min(总内存 80%, 当前可用 50%)：总内存那一项防止在空闲机器上
// 把整台机器吃满，可用内存那一项给其他进程留出继续申请的余量。只读到一项时用该项。
std::uint64_t automatic_memory_limit(MemoryStatus status) noexcept {
  const auto total_headroom = static_cast<std::uint64_t>(
      static_cast<long double>(status.total_bytes) * 0.8L);
  const auto available_headroom = status.available_bytes / 2;
  if (total_headroom == 0) {
    return available_headroom;
  }
  if (available_headroom == 0) {
    return total_headroom;
  }
  return std::min(total_headroom, available_headroom);
}

ResourcePlan plan_resources(ResourcePlanRequest request) noexcept {
  const int budget = std::max(1, request.automatic_thread_budget);
  const int files = std::max(1, request.file_count);
  const std::uint64_t memory_limit = request.memory_limit_bytes;
  const std::uint64_t per_file = std::max<std::uint64_t>(1, request.estimated_bytes_per_file);

  int memory_parallelism = budget;
  if (memory_limit > 0) {
    const auto memory_bound = std::max<std::uint64_t>(1, memory_limit / per_file);
    memory_parallelism = static_cast<int>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(budget), memory_bound));
  }

  const int file_parallelism = std::min({budget, files, memory_parallelism});
  const int encoder_threads = budget / file_parallelism;

  return ResourcePlan{.file_parallelism = file_parallelism,
                      .encoder_threads_per_file = encoder_threads,
                      .global_thread_budget = budget,
                      .memory_limit_bytes = memory_limit,
                      .memory_file_parallelism = memory_parallelism};
}

ResourcePlan plan_grid_encode_resources(ResourcePlan base,
                                               int tile_count) noexcept {
  const int budget = std::max(1, std::min(base.encoder_threads_per_file, base.global_thread_budget));
  const int tiles = std::max(1, tile_count);
  const int tile_parallelism = std::min(tiles, budget);
  const int per_tile_threads = budget / tile_parallelism;
  return ResourcePlan{.file_parallelism = tile_parallelism,
                      .encoder_threads_per_file = per_tile_threads,
                      .global_thread_budget = budget,
                      .memory_limit_bytes = base.memory_limit_bytes,
                      .memory_file_parallelism = tile_parallelism};
}

ResourcePlan plan_large_mode_resources(ResourcePlan base,
                                               int file_count,
                                               std::uint64_t largest_working_set_bytes) noexcept {
  return plan_resources(ResourcePlanRequest{
      .automatic_thread_budget = base.global_thread_budget,
      .file_count = file_count,
      .memory_limit_bytes = base.memory_limit_bytes,
      .estimated_bytes_per_file = largest_working_set_bytes});
}

}  // namespace awj
