module;
#include "../compat/stdlib_compat.h"
#include "hdr_cost.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

export module awj.resource_planner;

import awj.config;
import awj.encoding_defaults;

export namespace awj {

using MemoryStatus = encoding_defaults::PhysicalMemoryStatus;

MemoryStatus current_memory_status() noexcept {
  return encoding_defaults::physical_memory_status();
}

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
  // 批处理 ticket 是每文件预约，不是 memory_limit_bytes / file_parallelism。
  // 0 表示未提供 ticket，绝不授权无限制分配。
  std::uint64_t memory_reservation_bytes{};
  // 传入几何操作的图像之外，仍存活的存储与工作区。
  std::uint64_t geometry_retained_bytes{};
};

int per_file_thread_budget(const ResourcePlan& plan) noexcept {
  return std::max(1, std::min(plan.encoder_threads_per_file,
      std::max(1, plan.global_thread_budget / std::max(1, plan.file_parallelism))));
}

// UINT64_MAX 是内部无限制哨兵；有限预算除法得到 0 时仍表示耗尽，
// 不能变成 ResourcePlan 中表示无限制的 0。
std::uint64_t resource_memory_budget(const ResourcePlan& plan) noexcept {
  if (!plan.memory_limit_bytes) return UINT64_MAX;
  return plan.memory_reservation_bytes
      ? std::min(plan.memory_limit_bytes, plan.memory_reservation_bytes)
      : plan.memory_limit_bytes / static_cast<std::uint64_t>(std::max(1, plan.file_parallelism));
}

std::uint64_t regular_memory_fixed_bytes(int workers) noexcept {
  return hdr_cost::add(16ull * 1024 * 1024,
      hdr_cost::multiply(static_cast<std::uint64_t>(std::max(1, workers)), 256 * 1024));
}

// 预检与手动 AVIF 预览几何共用预约模型；编码工作区余量不是分配器/RSS 硬上限。
std::uint64_t avif_codec_workspace_bytes(std::uint64_t padded_pixels,
    std::uint64_t padded_planes, int workers, int depth, bool hdr,
    bool slow_or_lossless, int layer_count = 1) noexcept {
  const auto block = !hdr && depth <= 10 ? (depth > 8 ? 48u : 32u) : 64u;
  auto bytes = hdr_cost::add(hdr_cost::multiply(padded_pixels, block), hdr_cost::multiply(padded_planes, 3));
  if (slow_or_lossless) bytes = hdr_cost::add(bytes, hdr_cost::multiply(padded_pixels, 8));
  bytes = hdr_cost::add(bytes, hdr_cost::multiply(static_cast<std::uint64_t>(std::max(1, workers) - 1), 1024 * 1024));
  return hdr_cost::add(bytes, hdr_cost::multiply(padded_pixels,
      24ull * static_cast<std::uint64_t>(std::max(1, layer_count) - 1)));
}

// Direct backend/WGC callers may supply a previously finite resource plan.
// The session opt-out wins there too; it must not be reinterpreted as auto.
ResourcePlan normalize_memory_policy_resources(const AppConfig& cfg, ResourcePlan plan) noexcept {
  if (cfg.unlock_memory_limit) {
    plan.memory_limit_bytes = 0;
    plan.memory_reservation_bytes = 0;
    plan.memory_file_parallelism = std::max(1, plan.file_parallelism);
  }
  return plan;
}

struct HeifDecodeThreadPlan {
  int tile_parallelism{1};
  int codec_threads{1};
};

HeifDecodeThreadPlan plan_heif_decode_threads(int requested, std::uint64_t tiles, std::uint64_t tile_pixels) noexcept {
  const int budget = std::clamp(requested, 1, encoding_defaults::max_automatic_thread_budget);
  const int inner = tile_pixels >= 1024ull * 1024 ? 4 : tile_pixels >= 512ull * 512 ? 2 : 1;
  const int parallel = static_cast<int>(std::min<std::uint64_t>(std::max<std::uint64_t>(1, tiles),
      static_cast<std::uint64_t>(std::max(1, budget / inner))));
  return {.tile_parallelism = parallel, .codec_threads = std::max(1, budget / parallel)};
}

// 自动内存上限取 min(总内存 80%, 当前可用 50%)：总内存那一项防止在空闲机器上
// 把整台机器吃满，可用内存那一项给其他进程留出继续申请的余量。只读到一项时用该项。
std::uint64_t automatic_memory_limit(MemoryStatus status) noexcept {
  const auto total_headroom = encoding_defaults::physical_input_limit(status.total_bytes);
  const auto available_headroom = status.available_bytes / 2;
  if (total_headroom == 0) {
    return available_headroom;
  }
  if (available_headroom == 0) {
    return total_headroom;
  }
  return std::min(total_headroom, available_headroom);
}

// AppConfig keeps 0 = automatic for compatibility. Resolve the session unlock
// before consulting that value; downstream 0 must not turn back into automatic.
std::uint64_t effective_memory_limit(const AppConfig& cfg,
                                    MemoryStatus status) noexcept {
  if (cfg.unlock_memory_limit) return 0;
  return cfg.memory_limit_bytes != 0 ? cfg.memory_limit_bytes
                                     : automatic_memory_limit(status);
}

// A raw zero from an unavailable automatic estimate must never reach a
// ResourcePlan: zero there means unlimited, not an unknown system budget.
[[nodiscard]] awj::compat::expected<std::uint64_t, std::string> checked_effective_memory_limit(
    const AppConfig& cfg, MemoryStatus status) {
  const auto limit = effective_memory_limit(cfg, status);
  if (!cfg.unlock_memory_limit && limit == 0) {
    return awj::compat::unexpected{
        "无法确定非零的 auto 工作集内存预算（系统内存信息不可用或过小）；"
        "请重试或通过 --memory-limit 显式指定预算。"};
  }
  return limit;
}

[[nodiscard]] awj::compat::expected<std::uint64_t, std::string> checked_effective_memory_limit(
    const AppConfig& cfg) {
  // Neither an explicit budget nor the full session unlock needs an OS query.
  return checked_effective_memory_limit(cfg,
      cfg.unlock_memory_limit || cfg.memory_limit_bytes != 0 ? MemoryStatus{} : current_memory_status());
}

// Preserve already-resolved finite plans (including per-file reservations).
// A direct caller's zero plan needs resolution unless the full unlock is set;
// the legacy input-only opt-out never authorizes unlimited working memory.
[[nodiscard]] awj::compat::expected<ResourcePlan, std::string> checked_memory_policy_resources(
    const AppConfig& cfg, ResourcePlan plan, MemoryStatus status) {
  plan = normalize_memory_policy_resources(cfg, plan);
  if (cfg.unlock_memory_limit || plan.memory_limit_bytes != 0) return plan;
  const auto limit = checked_effective_memory_limit(cfg, status);
  if (!limit) return awj::compat::unexpected{limit.error()};
  plan.memory_limit_bytes = *limit;
  return plan;
}

[[nodiscard]] awj::compat::expected<ResourcePlan, std::string> checked_memory_policy_resources(
    const AppConfig& cfg, ResourcePlan plan) {
  return checked_memory_policy_resources(cfg, plan,
      cfg.unlock_memory_limit || cfg.memory_limit_bytes != 0 || plan.memory_limit_bytes != 0
          ? MemoryStatus{} : current_memory_status());
}

std::string_view memory_limit_mode(const AppConfig& cfg) noexcept {
  if (cfg.unlock_memory_limit) return "unlocked";
  return cfg.memory_limit_bytes != 0 ? "manual" : "automatic";
}

std::string_view memory_limit_source(const AppConfig& cfg,
                                     MemoryStatus status) noexcept {
  if (cfg.unlock_memory_limit) return "session_unlock";
  if (cfg.memory_limit_bytes != 0) return "configuration";
  if (status.total_bytes == 0 && status.available_bytes == 0)
    return "system_unavailable";
  const auto total_headroom = encoding_defaults::physical_input_limit(status.total_bytes);
  const auto available_headroom = status.available_bytes / 2;
  if (total_headroom == 0) return "system_available";
  if (available_headroom == 0) return "system_total";
  return available_headroom < total_headroom ? "system_available" : "system_total";
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
