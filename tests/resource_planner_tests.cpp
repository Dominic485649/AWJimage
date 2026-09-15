#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <array>
#include <future>
#include <chrono>
#include <limits>
#include "../src/core/work_memory_admission.h"

import awj.codec;
import awj.config;
import awj.encoding_defaults;
import awj.large_image_plan;
import awj.resource_planner;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

}  // namespace

int main() {
  struct Group { std::uint64_t estimated_bytes; };
  {
    const std::array groups{Group{8}, Group{7}, Group{2}};
    awj::WorkMemoryAdmission queue{10, groups};
    const auto first = queue.acquire({});
    const auto second = queue.acquire({});
    if (!first || !second || first->index != 0 || second->index != 2)
      return fail("A waiting large group prevented small-work backfill");
    std::stop_source cancel;
    auto waiter = std::async(std::launch::async, [&] { return queue.acquire(cancel.get_token()); });
    cancel.request_stop();
    if (waiter.wait_for(std::chrono::seconds{2}) != std::future_status::ready || waiter.get())
      return fail("Memory admission did not wake on cancellation");
    queue.release(first->bytes);
    const auto third = queue.acquire({});
    if (!third || third->index != 1) return fail("Released capacity was not reused");
    queue.release(second->bytes);
    queue.release(third->bytes);
    if (queue.acquire({})) return fail("Work was dispatched more than once");
  }
  {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::array groups{Group{maximum - 1}, Group{2}, Group{1}};
    awj::WorkMemoryAdmission queue{maximum, groups};
    const auto first = queue.acquire({});
    const auto second = queue.acquire({});
    if (!first || !second || second->bytes != 1)
      return fail("Memory reservation overflow admitted excess work");
    queue.release(first->bytes);
    queue.release(second->bytes);
    const auto third = queue.acquire({});
    if (!third || third->bytes != 2) return fail("Overflow boundary lost work");
    queue.release(third->bytes);
  }
  try {
    const std::array groups{Group{11}};
    awj::WorkMemoryAdmission queue{10, groups};
    return fail("Oversize work was silently clamped to the memory budget");
  } catch (const std::invalid_argument&) {}
  constexpr std::uint64_t gib = 1024ull * 1024ull * 1024ull;
  const auto aom_working_set = awj::avif_encode_working_set_bytes_for_dimensions(
      awj::make_image_dimensions(4096, 4096));
  if (aom_working_set < gib + gib / 2)
    return fail("AOM estimate is below measured encoder memory plus headroom");
  const auto measured_budget = awj::plan_resources({.automatic_thread_budget = 8,
      .file_count = 4, .memory_limit_bytes = 3 * gib, .estimated_bytes_per_file = aom_working_set});
  if (measured_budget.file_parallelism != 1 || measured_budget.encoder_threads_per_file != 8)
    return fail("AOM memory limit did not reassign the thread budget to the remaining file");

  // 可用内存偏紧时由 available*0.5 决定。
  const auto memory = awj::automatic_memory_limit(
      awj::MemoryStatus{.total_bytes = 32ull * gib,
                         .available_bytes = 10ull * gib});
  if (memory != 5ull * gib) {
    return fail("自动内存限制未使用 min(total*0.8, available*0.5)。");
  }

  // 机器很空闲时由 total*0.8 兜住，不能把整台机器吃满。
  const auto memory_total_bound = awj::automatic_memory_limit(
      awj::MemoryStatus{.total_bytes = 10ull * gib,
                         .available_bytes = 32ull * gib});
  if (memory_total_bound != 8ull * gib) {
    return fail("自动内存限制未用 total*0.8 兜住空闲机器。");
  }

  // 只读到一项时使用该项，不能退化成 0。
  if (awj::automatic_memory_limit(
          awj::MemoryStatus{.total_bytes = 0, .available_bytes = 8ull * gib}) !=
      4ull * gib) {
    return fail("读不到总内存时未回退到 available*0.5。");
  }
  if (awj::automatic_memory_limit(
          awj::MemoryStatus{.total_bytes = 10ull * gib, .available_bytes = 0}) !=
      8ull * gib) {
    return fail("读不到可用内存时未回退到 total*0.8。");
  }

  const auto single_av1 = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (single_av1.file_parallelism != 1 ||
      single_av1.encoder_threads_per_file != 12) {
    return fail("单文件未把完整自动线程预算传给 encoder。");
  }

  const auto low_budget_av1 = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 6,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (low_budget_av1.file_parallelism != 1 ||
      low_budget_av1.encoder_threads_per_file != 6) {
    return fail("单文件 AOM 低预算不应额外扣减线程。");
  }

  const auto batch_av1 = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 100,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (batch_av1.file_parallelism != 12 ||
      batch_av1.file_parallelism * batch_av1.encoder_threads_per_file != 12 ||
      batch_av1.encoder_threads_per_file != 1) {
    return fail("大批量 AV1 未降低 encoder 内部线程。");
  }

  const auto wide_batch = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 28,
                                .file_count = 13,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (wide_batch.file_parallelism != 13 ||
      wide_batch.encoder_threads_per_file != 2 ||
      wide_batch.file_parallelism * wide_batch.encoder_threads_per_file > 28) {
    return fail("文件数低于 CPU 预算时应分配剩余线程。");
  }

  const auto three_files = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 8,
                                .file_count = 3,
                                .estimated_bytes_per_file = 1});
  const auto five_files = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 5,
                                .estimated_bytes_per_file = 1});
  const auto prime_budget = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 7,
                                .file_count = 3,
                                .estimated_bytes_per_file = 1});
  const auto threshold_batch = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 28,
                                .file_count = 12,
                                .estimated_bytes_per_file = 1});
  if (three_files.file_parallelism != 3 || three_files.encoder_threads_per_file != 2 ||
      five_files.file_parallelism != 5 || five_files.encoder_threads_per_file != 2 ||
      prime_budget.file_parallelism != 3 || prime_budget.encoder_threads_per_file != 2 ||
      threshold_batch.file_parallelism != 12 ||
      threshold_batch.encoder_threads_per_file != 2) {
    return fail("线程预算未精确拆分为 encoder 线程与文件并发。");
  }

  const auto jxl_single = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 20,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (jxl_single.file_parallelism != 1 ||
      jxl_single.encoder_threads_per_file != 20) {
    return fail("单文件 JXL 未收到完整线程预算。");
  }

  const auto avif_single = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 20,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (avif_single.file_parallelism != 1 ||
      avif_single.encoder_threads_per_file != 20) {
    return fail("单文件 AVIF 未收到完整线程预算。");
  }

  const auto memory_limited = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 12,
                                .memory_limit_bytes = 300,
                                .estimated_bytes_per_file = 128});
  if (memory_limited.file_parallelism != 2 ||
      memory_limited.encoder_threads_per_file != 6 ||
      memory_limited.memory_file_parallelism != 2) {
    return fail("内存限制降低并发后必须重新分配每图线程。");
  }

  const auto grid_resources = awj::plan_grid_encode_resources(
      awj::ResourcePlan{.file_parallelism = 1,
                         .encoder_threads_per_file = 8,
                         .global_thread_budget = 12,
                         .memory_limit_bytes = 0,
                         .memory_file_parallelism = 1},
      4);
  if (grid_resources.file_parallelism != 4 ||
      grid_resources.encoder_threads_per_file != 2 ||
      grid_resources.file_parallelism * grid_resources.encoder_threads_per_file != 8) {
    return fail("AVIF grid 线程预算未拆分为 tile 并行与 per-tile 线程。");
  }

  const auto large_resources = awj::plan_large_mode_resources(
      awj::ResourcePlan{.file_parallelism = 3,
                         .encoder_threads_per_file = 4,
                         .global_thread_budget = 12,
                         .memory_limit_bytes = 900,
                         .memory_file_parallelism = 4},
      8, 300);
  if (large_resources.file_parallelism != 3 ||
      large_resources.encoder_threads_per_file != 4 ||
      large_resources.file_parallelism * large_resources.encoder_threads_per_file != 12 ||
      large_resources.memory_file_parallelism != 3) {
    return fail("大图模式资源规划未同时约束线程预算和内存预算。");
  }

  const auto large_mode_single = awj::plan_large_mode_resources(
      awj::plan_resources(
          awj::ResourcePlanRequest{.automatic_thread_budget = 20,
                                   .file_count = 1,
                                   .memory_limit_bytes = 0,
                                   .estimated_bytes_per_file = 1}),
      1, 1);
  if (large_mode_single.file_parallelism != 1 ||
      large_mode_single.encoder_threads_per_file != 20) {
    return fail("单个大图未保留完整线程预算。");
  }

  const auto large_mode_memory_tight = awj::plan_large_mode_resources(
      awj::ResourcePlan{.file_parallelism = 4,
                         .encoder_threads_per_file = 4,
                         .global_thread_budget = 16,
                         .memory_limit_bytes = 700,
                         .memory_file_parallelism = 8},
      4, 300);
  if (large_mode_memory_tight.file_parallelism != 2 ||
      large_mode_memory_tight.encoder_threads_per_file != 8 ||
      large_mode_memory_tight.memory_file_parallelism != 2) {
    return fail("大图模式内存预算未继续约束文件并发。");
  }

  const auto grid_single_tile = awj::plan_grid_encode_resources(
      awj::ResourcePlan{.file_parallelism = 1,
                         .encoder_threads_per_file = 3,
                         .global_thread_budget = 3,
                         .memory_limit_bytes = 0,
                         .memory_file_parallelism = 1},
      1);
  if (grid_single_tile.file_parallelism != 1 ||
      grid_single_tile.encoder_threads_per_file != 3) {
    return fail("单 tile grid 不应拆分出额外并行开销。");
  }

  const auto avif_speed = awj::map_speed_for_format(awj::OutputFormat::avif, 10);
  const auto webp_speed = awj::map_speed_for_format(awj::OutputFormat::webp, 10);
  const auto jxl_speed = awj::map_speed_for_format(awj::OutputFormat::jxl, 10);
  const auto jpegli_speed = awj::map_speed_for_format(awj::OutputFormat::jpgli, 10);
  if (avif_speed.codec_value != 10 || webp_speed.codec_value != 0 ||
      jxl_speed.codec_value != 1 || jpegli_speed.codec_value != -1 ||
      !jpegli_speed.codec_key.empty()) {
    return fail("speed=10 未映射到最快 codec 档位。");
  }

  for (int budget = 1; budget <= 32; ++budget) {
    for (int files = 1; files <= 40; ++files) {
      for (int memory_files = 1; memory_files <= 8; ++memory_files) {
        const auto plan = awj::plan_resources({.automatic_thread_budget = budget,
            .file_count = files, .memory_limit_bytes = static_cast<std::uint64_t>(memory_files) * 128,
            .estimated_bytes_per_file = 128});
        if (plan.file_parallelism > files || plan.file_parallelism > memory_files ||
            plan.file_parallelism * plan.encoder_threads_per_file > budget ||
            plan.encoder_threads_per_file != budget / plan.file_parallelism)
          return fail("CPU/memory/file limits or thread redistribution invariant failed.");
        const auto grid = awj::plan_grid_encode_resources(plan, 5);
        if (grid.file_parallelism * grid.encoder_threads_per_file > plan.encoder_threads_per_file)
          return fail("Grid exceeded its per-file CPU budget.");
      }
    }
  }
  return 0;
}
