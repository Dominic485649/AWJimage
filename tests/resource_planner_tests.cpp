#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <array>
#include <future>
#include <chrono>
#include <limits>
#include <bit>
#include <vector>
#include "../src/core/work_memory_admission.h"
#include "../src/core/hdr_cost.h"

import awj.codec;
import awj.config;
import awj.encoding_defaults;
import awj.large_image_plan;
import awj.image;
import awj.resource_planner;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

}  // namespace

int main() {
  awj::ImageBuffer reference{};
  reference.bit_depth = 16;
  reference.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::yuv420, .bit_depth = 8};
  if (awj::resolve_rendition_bit_depth(awj::OutputFormat::avif, reference, false).value_or(0) != 10 ||
      awj::resolve_rendition_bit_depth(awj::OutputFormat::jxl, reference, false).value_or(0) != 16 ||
      awj::resolve_rendition_bit_depth(awj::OutputFormat::png, reference, false).value_or(0) != 8 ||
      awj::resolve_rendition_bit_depth(awj::OutputFormat::png, reference, true).value_or(0) != 16 ||
      awj::resolve_rendition_bit_depth(awj::OutputFormat::avif, reference, false, 8).value_or(0) != 8 ||
      awj::resolve_rendition_bit_depth(awj::OutputFormat::png, reference, false, 16).value_or(0) != 16 ||
      awj::resolve_rendition_bit_depth(awj::OutputFormat::png, reference, false, 12) ||
      awj::resolve_rendition_bit_depth(awj::OutputFormat::webp, reference, false, 10))
    return fail("Rendition precision/PNG compatibility/explicit-parameter policy mismatch.");
  reference.source_info->pixel_format = awj::PixelFormat::rgba;
  reference.source_info->bit_depth = 16;
  if (awj::resolve_rendition_bit_depth(awj::OutputFormat::avif, reference, true).value_or(0) != 12 ||
      awj::avif_auto_chroma(reference.source_info->pixel_format) != awj::ChromaMode::yuv444)
    return fail("Reconstructed RGB16 HDR did not select AVIF12/444.");
  reference.source_info->pixel_format = awj::PixelFormat::yuv420;
  reference.source_info->bit_depth = 10;
  if (awj::resolve_rendition_bit_depth(awj::OutputFormat::avif, reference, true).value_or(0) != 10 ||
      awj::avif_auto_chroma(reference.source_info->pixel_format) != awj::ChromaMode::yuv420)
    return fail("Retained reverse-map HDR base lost native precision/sampling.");
  if (awj::gain_map_mode_for_output(awj::OutputFormat::png, awj::JpegHdrMode::automatic) != awj::GainMapDecodeMode::hdr_only ||
      awj::gain_map_mode_for_output(awj::OutputFormat::webp, awj::JpegHdrMode::automatic) != awj::GainMapDecodeMode::sdr_only ||
      awj::gain_map_mode_for_output(awj::OutputFormat::jpgli, awj::JpegHdrMode::sdr) != awj::GainMapDecodeMode::sdr_only ||
      awj::gain_map_mode_for_output(awj::OutputFormat::avif, awj::JpegHdrMode::automatic) != awj::GainMapDecodeMode::preserve_both)
    return fail("Target capability selected the wrong Gain Map rendition mode.");
  for (int budget : {0, 1, 2, 3, 12, 64}) for (std::uint64_t tiles : {0ull, 1ull, 4ull, 100ull})
    for (std::uint64_t pixels : {0ull, 65536ull, 262144ull, 4194304ull}) {
      const auto plan = awj::plan_heif_decode_threads(budget, tiles, pixels);
      if (plan.tile_parallelism < 1 || plan.codec_threads < 1 ||
          plan.tile_parallelism * plan.codec_threads > std::max(1, budget) ||
          static_cast<std::uint64_t>(plan.tile_parallelism) > std::max<std::uint64_t>(1, tiles))
        return fail("HEIF tile/codec split multiplied the per-file thread budget.");
    }
  if (awj::default_max_jobs() != awj::automatic_thread_budget(awj::available_processor_count()))
    return fail("Automatic threads ignored available CPU affinity.");
#ifdef _WIN32
  DWORD_PTR original{}, system{};
  if (!GetProcessAffinityMask(GetCurrentProcess(), &original, &system) || !original)
    return fail("Cannot inspect test CPU affinity.");
  const auto one_cpu = original & (~original + 1);
  if (!awj::apply_cpu_affinity({static_cast<unsigned>(std::countr_zero(one_cpu))}))
    return fail("Cannot restrict test CPU affinity through CLI API.");
  DWORD_PTR applied{}, applied_system{};
  if (!GetProcessAffinityMask(GetCurrentProcess(), &applied, &applied_system) || applied != one_cpu ||
      awj::apply_cpu_affinity({awj::cpu_affinity_capacity()})) return fail("Affinity API mask/bound validation failed.");
  const auto outside = original & ~one_cpu;
  if (outside && awj::apply_cpu_affinity({static_cast<unsigned>(std::countr_zero(outside))}))
    return fail("Affinity API widened inherited limits.");
  const bool affinity_ok = awj::available_processor_count() == 1 && awj::default_max_jobs() == 1;
  if (!std::async(std::launch::async, [one_cpu] {
        DWORD_PTR mask{}, system_mask{};
        return GetProcessAffinityMask(GetCurrentProcess(), &mask, &system_mask) && mask == one_cpu;
      }).get()) return fail("New workers lost CPU affinity.");
  if (!SetProcessAffinityMask(GetCurrentProcess(), original) || !affinity_ok)
    return fail("Automatic threads did not respect a one-CPU affinity mask.");
#else
  cpu_set_t original{};
  if (sched_getaffinity(0, sizeof(original), &original)) return fail("Cannot inspect test CPU affinity.");
  cpu_set_t one_cpu{}; CPU_ZERO(&one_cpu);
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &original)) { CPU_SET(cpu, &one_cpu); break; }
  unsigned selected{};
  for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &one_cpu)) { selected = cpu; break; }
  if (!awj::apply_cpu_affinity({selected})) return fail("Cannot restrict test CPU affinity through CLI API.");
  cpu_set_t applied{};
  if (sched_getaffinity(0, sizeof(applied), &applied) || !CPU_EQUAL(&applied, &one_cpu) ||
      awj::apply_cpu_affinity({awj::cpu_affinity_capacity()})) return fail("Affinity API mask/bound validation failed.");
  for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &original) && cpu != selected) {
    if (awj::apply_cpu_affinity({cpu})) return fail("Affinity API widened inherited limits.");
    break;
  }
  const bool affinity_ok = awj::available_processor_count() == 1 && awj::default_max_jobs() == 1;
  if (!std::async(std::launch::async, [one_cpu] {
        cpu_set_t mask{};
        return sched_getaffinity(0, sizeof(mask), &mask) == 0 && CPU_EQUAL(&mask, &one_cpu);
      }).get()) return fail("New workers lost CPU affinity.");
  if (sched_setaffinity(0, sizeof(original), &original) || !affinity_ok)
    return fail("Automatic threads did not respect a one-CPU affinity mask.");
#endif
  if (awj::classify_large_image(awj::make_image_dimensions(8192, 8192), true).klass != awj::LargeImageClass::ordinary ||
      awj::classify_large_image(awj::make_image_dimensions(65537, 1), true).klass != awj::LargeImageClass::large_mode_required)
    return fail("Ordinary large images were deferred or an encoder limit lost Grid routing.");
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
  {
    const std::array groups{Group{11}, Group{12}};
    awj::WorkMemoryAdmission queue{0, groups};
    const auto first = queue.acquire({});
    const auto second = queue.acquire({});
    if (!first || !second) return fail("Resolved zero budget did not mean unlimited admission.");
    queue.release(first->bytes);
    queue.release(second->bytes);
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
  if (awj::avif_encode_working_set_bytes_for_dimensions(
          awj::make_image_dimensions(4096, 4096), 16 * 1024 * 1024) !=
      aom_working_set + 32 * 1024 * 1024 ||
      awj::avif_encode_working_set_bytes_for_dimensions(
          awj::make_image_dimensions(4096, 4096), std::numeric_limits<std::uint64_t>::max()) !=
      std::numeric_limits<std::uint64_t>::max())
    return fail("AVIF estimate omitted retained compressed input or overflowed");
  const auto opaque = awj::avif_encode_working_set_bytes_for_dimensions(awj::make_image_dimensions(4096, 4096), 0, true);
  if (opaque >= aom_working_set || opaque < gib ||
      awj::avif_encode_working_set_bytes_for_dimensions(awj::make_image_dimensions(1, 65)) !=
      awj::avif_encode_working_set_bytes_for_dimensions(awj::make_image_dimensions(64, 128)))
    return fail("Known opaque memory profile or codec block padding is invalid.");
  {
    constexpr std::uint64_t n = 2048ull * 1536;
    const auto sdr = awj::hdr_cost::jpeg_sdr_working_set_bytes(2048, 1536, 0, n * 4);
    const auto hdr = awj::hdr_cost::jpeg_working_set_bytes(2048, 1536, 0, 1, n * 4);
    if (sdr >= hdr || sdr < n * 4 ||
        awj::hdr_cost::jpeg_sdr_working_set_bytes(2048, 1536, 7, n * 4) != sdr + 14 ||
        awj::hdr_cost::jpeg_sdr_working_set_bytes(2048, 1536, UINT64_MAX) != UINT64_MAX ||
        awj::hdr_cost::jpeg_sdr_working_set_bytes(0, 1536) != UINT64_MAX)
      return fail("SDR JPEG phases retained the HDR graph, lost input storage or overflowed.");
  }

  const auto measured_budget = awj::plan_resources({.automatic_thread_budget = 8,
      .file_count = 4, .memory_limit_bytes = 3 * gib, .estimated_bytes_per_file = aom_working_set});
  if (measured_budget.file_parallelism != 1 || measured_budget.encoder_threads_per_file != 8)
    return fail("AOM memory limit did not reassign the thread budget to the remaining file");

  {
    auto cfg = awj::default_app_config();
    const awj::MemoryStatus status{.total_bytes = 32 * gib, .available_bytes = 10 * gib};
    cfg.memory_limit_bytes = 0;
    if (awj::effective_memory_limit(cfg, status) != 5 * gib ||
        awj::memory_limit_mode(cfg) != "automatic" ||
        awj::memory_limit_source(cfg, status) != "system_available")
      return fail("Configured zero no longer means automatic memory admission.");
    cfg.unlock_memory_limit = true;
    if (awj::effective_memory_limit(cfg, status) != 0 ||
        awj::memory_limit_mode(cfg) != "unlocked" ||
        awj::memory_limit_source(cfg, status) != "session_unlock")
      return fail("Session unlock fell back to an automatic memory limit.");
    cfg.memory_limit_bytes = 3 * gib;
    if (awj::effective_memory_limit(cfg, status) != 0 || cfg.memory_limit_bytes != 3 * gib)
      return fail("Session unlock erased or failed to override a manual memory value.");
    cfg.unlock_memory_limit = false;
    if (awj::effective_memory_limit(cfg, status) != 3 * gib ||
        awj::memory_limit_mode(cfg) != "manual" ||
        awj::memory_limit_source(cfg, status) != "configuration")
      return fail("Disabling unlock did not restore the manual budget.");
    cfg.memory_limit_bytes = 0;
    cfg.unlock_max_input_file_bytes = true;
    if (awj::effective_memory_limit(cfg, status) != 5 * gib ||
        awj::memory_limit_source(cfg, {}) != "system_unavailable")
      return fail("The input/buffer unlock unexpectedly changed the working-set budget.");
  }

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
