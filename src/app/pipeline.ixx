module;
#include "../compat/stdlib_compat.h"
#include "../core/work_memory_admission.h"
#include "../core/work_cpu_admission.h"
#include "../core/hdr_cost.h"
#include "../core/spline36_resample.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <objbase.h>
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <print>
#include <ranges>
#include <stdexcept>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module awj.pipeline;

export import awj.batch_protocol;

import awj.avif_aom_codec;
import awj.animation_decoder;
import awj.avif_registry;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.large_image_plan;
import awj.resource_planner;
import awj.visual_quality;
import awj.native_backend;
import awj.image_memory_plan;

export namespace awj {

namespace pipeline_detail {

struct WorkGroup {
  std::uintmax_t weight{};
  // 组内串行处理，峰值内存取成员估算的最大值，供运行时内存准入使用。
  std::uint64_t estimated_bytes{1};
  bool exclusive_refinement{};
  std::vector<ImageFile> files{};
  std::vector<BatchLargeImageItem> large_items{};
};

bool checked_add_uintmax(std::uintmax_t& value,
                         std::uintmax_t addend) noexcept {
  if (value > std::numeric_limits<std::uintmax_t>::max() - addend) {
    return false;
  }
  value += addend;
  return true;
}

std::uintmax_t saturated_add_uintmax(std::uintmax_t value,
                                     std::uintmax_t addend) noexcept {
  return checked_add_uintmax(value, addend)
             ? value
             : std::numeric_limits<std::uintmax_t>::max();
}

struct ClassifiedImageFile {
  ImageFile file{};
  std::uint64_t estimated_bytes{1};
  ImageDimensions dimensions{};
  bool grid{};
  bool exclusive_refinement{};
  std::string preflight_error{};
};

struct ClassifiedWork {
  std::vector<ClassifiedImageFile> ordinary{};
  std::vector<ClassifiedImageFile> memory_rejected{};
  std::vector<BatchLargeImageItem> large_mode{};
};

#ifdef _WIN32
class WicFallbackComApartment {
 public:
  explicit WicFallbackComApartment(bool enabled) noexcept
      : enabled_{enabled},
        init_{enabled ? CoInitializeEx(nullptr, COINIT_MULTITHREADED |
                                                    COINIT_DISABLE_OLE1DDE)
                      : S_FALSE} {}

  ~WicFallbackComApartment() {
    if (enabled_ && SUCCEEDED(init_)) {
      CoUninitialize();
    }
  }

  WicFallbackComApartment(const WicFallbackComApartment&) = delete;
  WicFallbackComApartment& operator=(const WicFallbackComApartment&) = delete;

  [[nodiscard]] bool usable() const noexcept {
    return !enabled_ || SUCCEEDED(init_) || init_ == RPC_E_CHANGED_MODE;
  }

  [[nodiscard]] HRESULT init() const noexcept { return init_; }

 private:
  bool enabled_{};
  HRESULT init_{S_FALSE};
};
#else
class WicFallbackComApartment {
 public:
  explicit WicFallbackComApartment(bool) noexcept {}
  [[nodiscard]] bool usable() const noexcept { return true; }
  [[nodiscard]] int init() const noexcept { return 0; }
};
#endif

int count_to_int_saturated(std::size_t value) noexcept {
  return value > static_cast<std::size_t>(std::numeric_limits<int>::max())
             ? std::numeric_limits<int>::max()
             : static_cast<int>(value);
}

std::string large_image_actions_text(const LargeImageDecision& decision) {
  std::string actions;
  if (decision.available_grid) {
    actions = "grid";
  }

  if (actions.empty()) {
    actions = "无可用大图编码器";
  }
  return actions;
}

std::string large_image_report_message(const BatchLargeImageItem& item) {
  return awj::compat::format("超大图自动处理：{}x{}，原因 {}；可用处理方式：{}；{}",
                     item.dimensions.width, item.dimensions.height,
                     large_image_reason_name(item.decision.reason),
                     large_image_actions_text(item.decision),
                     item.decision.reason_text);
}

std::string large_image_action_text(const BatchLargeImageItem& item) {
  const auto actions = large_image_actions_text(item.decision);
  return awj::compat::format(
      "[LARGE] {:04} {} ({}x{}, {}) -> 自动大图：{}；{}", item.file.index + 1,
      path_to_utf8(item.file.path.filename()), item.dimensions.width,
      item.dimensions.height, large_image_reason_name(item.decision.reason),
      actions, item.decision.reason_text);
}

std::string large_image_log_text(const BatchLargeImageItem& item) {
  const auto actions = large_image_actions_text(item.decision);
  return awj::compat::format("[LARGE] {:04} ({}x{}, {}) -> 自动大图：{}；{}",
                     item.file.index + 1, item.dimensions.width,
                     item.dimensions.height,
                     large_image_reason_name(item.decision.reason), actions,
                     item.decision.reason_text);
}

bool avif_capability_available_for_large_mode(
    const AvifEncoderCapability& capability) noexcept {
  return capability.enabled;
}

void clear_unstarted_execution_metrics(EncodeResult& result) noexcept {
  // 未启动的条目尚未做几何变换判定。
  result.resize_seconds = -1.0;
  result.resize_workers = 0;
  result.encoder_threads = 0;
  result.gain_map_render_workers = result.gain_map_compute_workers = 0;
  result.heif_tile_parallelism = result.heif_codec_thread_budget = 0;
  result.base_decode_seconds = result.auxiliary_decode_seconds = result.gain_map_render_seconds =
      result.gain_map_compute_seconds = result.color_convert_seconds = 0.0;
  result.seconds = result.decode_seconds = result.prepare_seconds = result.encode_seconds =
      result.avif_rgb_to_yuv_seconds = result.avif_add_image_seconds = result.avif_finish_seconds =
      result.avif_output_copy_seconds = result.write_seconds = result.visual_quality_search_seconds =
      result.visual_quality_candidate_encode_seconds = result.visual_quality_candidate_decode_seconds =
      result.visual_quality_candidate_io_seconds = result.visual_quality_luma_seconds =
      result.gmsd_seconds = result.ms_ssim_seconds = result.visual_quality_metric_seconds = 0.0;
}

void record_memory_diagnostics(EncodeResult& result, const AppConfig& cfg,
                                MemoryStatus status, std::uint64_t effective_limit,
                                const ImageMemoryEstimate& estimate) {
  result.memory_budget_bytes = effective_limit;
  result.memory_budget_mode = memory_limit_mode(cfg);
  result.memory_budget_source = memory_limit_source(cfg, status);
  store_memory_estimate(result, estimate);
}

// Backend results replace the pending result. Move the original admission
// snapshot (including its original system-memory source), never a new sample.
void inherit_memory_diagnostics(EncodeResult& result, EncodeResult& pending) {
  result.memory_budget_bytes = pending.memory_budget_bytes;
  result.memory_budget_mode = std::move(pending.memory_budget_mode);
  result.memory_budget_source = std::move(pending.memory_budget_source);
  // 保留后端真实像素计划的精化值，不能被解码前旧快照再次覆盖。
  if (!result.memory_plan.refined) {
    const auto reserved_peak = std::max(result.memory_plan.reserved_peak_bytes, pending.memory_plan.reserved_peak_bytes);
    result.estimated_working_set_bytes = pending.estimated_working_set_bytes;
    result.memory_estimate_profile = std::move(pending.memory_estimate_profile);
    result.memory_plan = std::move(pending.memory_plan);
    result.memory_plan.reserved_peak_bytes = reserved_peak;
  } else {
    result.memory_plan.initial_bytes = pending.memory_plan.initial_bytes;
    result.memory_plan.exclusive_refinement = pending.memory_plan.exclusive_refinement;
    result.memory_plan.reserved_peak_bytes = std::max(result.memory_plan.reserved_peak_bytes,
                                                     pending.memory_plan.reserved_peak_bytes);
  }
  result.memory_source_width = pending.memory_source_width;
  result.memory_source_height = pending.memory_source_height;
  result.memory_target_width = pending.memory_target_width;
  result.memory_target_height = pending.memory_target_height;
  result.memory_source_has_gain_map = pending.memory_source_has_gain_map;
}

awj::compat::expected<ClassifiedWork, std::string> classify_work_for_avif(
    const AppConfig& cfg, const std::vector<ImageFile>& files,
    std::uint64_t memory_limit, awj::compat::stop_token stop = {}) {
  try {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    struct ProbeResult {
      std::optional<ImageDimensions> dimensions;
      std::uint64_t reservation{};
      bool memory_rejected{};
      std::string error;
    };
    struct ProbeWork {
      std::uint64_t estimated_bytes;
      std::size_t file_index;
    };
    std::vector<ProbeResult> probed(files.size());
    std::vector<ProbeWork> pending;
    pending.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
      if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
      auto& result = probed[i];
      result.reservation = dimension_probe_working_set_bytes(files[i].path, files[i].bytes);
      if (memory_limit && result.reservation > memory_limit) {
        result.memory_rejected = true;
        result.error = awj::compat::format(
            "尺寸探测工作集 {} 超过内存预算 {}，未读取完整输入。",
            format_size(result.reservation), format_size(memory_limit));
      } else {
        pending.push_back({result.reservation, i});
      }
    }
    WorkMemoryAdmission admission{memory_limit, pending};
    std::atomic<bool> failed{};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto run_probes = [&, policy = encoding_defaults::current_memory_policy()] {
      const encoding_defaults::ScopedMemoryPolicy memory_scope{policy};
      try {
        WicFallbackComApartment com{cfg.allow_wic_fallback};
        while (!failed.load() && !stop.stop_requested()) {
          const auto ticket = admission.acquire(stop);
          if (!ticket) break;
          struct Reservation {
            WorkMemoryAdmission& owner;
            std::uint64_t bytes;
            ~Reservation() { owner.release(bytes); }
          } reservation{admission, ticket->bytes};
          const auto index = pending[ticket->index].file_index;
          auto& result = probed[index];
          const DecoderRegistryOptions options{
              .allow_wic_fallback = cfg.allow_wic_fallback && com.usable(),
              .decode_threads = 1,
              .stop_token = stop,
              .probe_memory_reservation_bytes = memory_limit ? ticket->bytes : 0};
          auto dimensions = probe_image_dimensions_for_path(files[index].path, options);
          if (stop.stop_requested()) break;
          if (dimensions) result.dimensions = std::move(*dimensions);
          else {
            result.error = std::move(dimensions.error());
            result.memory_rejected = result.error.starts_with("尺寸探测工作集 ");
          }
        }
      } catch (...) {
        std::lock_guard lock{failure_mutex};
        if (!failure) failure = std::current_exception();
        failed.store(true);
      }
    };
    const int jobs = std::min(std::clamp(cfg.max_jobs, 1,
        encoding_defaults::max_automatic_thread_budget), count_to_int_saturated(pending.size()));
    std::vector<awj::compat::scoped_worker> workers;
    workers.reserve(static_cast<std::size_t>(std::max(0, jobs - 1)));
    for (int i = 1; i < jobs; ++i) {
      try { workers.emplace_back(run_probes); }
      catch (const std::system_error&) { break; }
      catch (const std::bad_alloc&) { break; }
    }
    run_probes();
    workers.clear();
    if (failure) std::rethrow_exception(failure);
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};

    // 少数 ICC/输入增长只有读取过程中才能精化需求。并行预约均已释放后，
    // 最多独占重试一次，不持有部分 metadata 等待其他探测释放内存。
    if (memory_limit) {
      WicFallbackComApartment com{cfg.allow_wic_fallback};
      for (std::size_t i = 0; i < probed.size(); ++i) {
        if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
        auto& result = probed[i];
        if (!result.memory_rejected || result.reservation >= memory_limit) continue;
        auto dimensions = probe_image_dimensions_for_path(files[i].path,
            DecoderRegistryOptions{.allow_wic_fallback = cfg.allow_wic_fallback && com.usable(),
                .decode_threads = 1, .stop_token = stop,
                .probe_memory_reservation_bytes = memory_limit});
        if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
        if (dimensions) {
          result.dimensions = std::move(*dimensions);
          result.memory_rejected = false;
          result.error.clear();
        } else {
          result.error = std::move(dimensions.error());
          result.memory_rejected = result.error.starts_with("尺寸探测工作集 ");
        }
      }
    }

    ClassifiedWork classified{};
    classified.ordinary.reserve(files.size());
    const auto capabilities = avif_encoder_capabilities_for_current_build();
    const bool grid_available = cfg.output_format == OutputFormat::avif &&
        awj::compat::ranges::any_of(capabilities, [](const auto& capability) {
          return capability.mode == AvifEncoderMode::aom &&
                 avif_capability_available_for_large_mode(capability) &&
                 capability.supports_avif_grid;
        });
    for (std::size_t i = 0; i < files.size(); ++i) {
      if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
      const auto& image = files[i];
      auto& probe = probed[i];
      if (probe.memory_rejected) {
        classified.memory_rejected.push_back(ClassifiedImageFile{
            .file = image, .estimated_bytes = probe.reservation,
            .preflight_error = std::move(probe.error)});
        continue;
      }
      // 未知画像独占精化，不能把压缩体积小误作低解码工作集。
      if (!probe.dimensions) {
        classified.ordinary.push_back(ClassifiedImageFile{
            .file = image,
            .estimated_bytes = std::max<std::uint64_t>(memory_limit,
                std::max<std::uintmax_t>(1, image.bytes))});
        continue;
      }
      const auto& dimensions = *probe.dimensions;
      if (cfg.output_format != OutputFormat::avif) {
        classified.ordinary.push_back(ClassifiedImageFile{
            .file = image,
            .estimated_bytes = estimated_regular_working_set_bytes(dimensions, cfg, image.bytes, memory_limit),
            .dimensions = dimensions});
        continue;
      }
      const auto target = target_dimensions_for_config(dimensions, cfg);
      auto decision = classify_large_image(target, grid_available);
      if (decision.klass == LargeImageClass::large_mode_required && grid_available) {
        auto animated = input_uses_animation_reader(image.path, stop);
        if (!animated) return awj::compat::unexpected{animated.error()};
        if (*animated) decision = classify_large_image(target, false);
      }
      if (decision.klass == LargeImageClass::large_mode_required) {
        classified.large_mode.push_back(BatchLargeImageItem{
            .file = image, .dimensions = dimensions, .decision = std::move(decision)});
      } else {
        classified.ordinary.push_back(ClassifiedImageFile{
            .file = image,
            .estimated_bytes = estimated_regular_working_set_bytes(dimensions, cfg, image.bytes, memory_limit),
            .dimensions = dimensions});
      }
    }
    return classified;
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"批处理分类列表内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"批处理分类列表数量超过运行时限制。"};
  } catch (const std::exception&) {
    return awj::compat::unexpected{"批处理尺寸探测发生异常。"};
  }
}

// 并发规划用的「每图占用」估计。取中位数而不是最大值：一个队列里只要有一张
// 大图，用最大值会把 memory_limit / per_file 压到 1，整批退化成串行（实测并发
// 恒为 1）。中位数代表大多数文件的实际占用，让常规并发能起来；少数确实吃内存的
// 大图由运行时内存准入（reserve_work_memory）单独限制，不会同时挤进内存。
std::uint64_t typical_decoded_bytes_per_file(
    const std::vector<ClassifiedImageFile>& files) noexcept {
  if (files.empty()) {
    return 1;
  }
  std::vector<std::uint64_t> sorted;
  try {
    sorted.reserve(files.size());
    for (const auto& image : files) {
      sorted.push_back(image.estimated_bytes);
    }
    const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(
                                             sorted.size() / 2);
    std::nth_element(sorted.begin(), middle, sorted.end());
    return std::max<std::uint64_t>(1, *middle);
  } catch (...) {
    std::uint64_t largest = 1;
    for (const auto& file : files) largest = std::max(largest, file.estimated_bytes);
    return largest;
  }
}

std::uint64_t estimated_large_working_set_bytes(
    const BatchLargeImageItem& item, const AppConfig& cfg, std::uint64_t memory_budget) noexcept;

awj::compat::expected<void, std::string> reject_over_memory_budget(
    ClassifiedWork& classified, std::uint64_t memory_limit, const AppConfig& cfg) {
  if (memory_limit == 0) {
    return {};
  }

  auto reject_from = [&](std::vector<ClassifiedImageFile>& source)
      -> awj::compat::expected<void, std::string> {
    try {
      if (classified.memory_rejected.size() >
          std::numeric_limits<std::size_t>::max() - source.size()) {
        return awj::compat::unexpected{"内存预算预检列表数量超过运行时限制。"};
      }
      classified.memory_rejected.reserve(classified.memory_rejected.size() +
                                         source.size());
      std::vector<ClassifiedImageFile> retained;
      retained.reserve(source.size());
      for (auto& image : source) {
        if (image.estimated_bytes > memory_limit) {
          const auto plan = estimated_regular_memory(image.dimensions, cfg, image.file.bytes, false, nullptr, memory_limit);
          if (!image.grid && plan.bytes != UINT64_MAX && plan.decode_bytes &&
              plan.decode_bytes <= memory_limit && plan.prepare_bytes <= memory_limit &&
              plan.mandatory_bytes <= memory_limit) {
            image.exclusive_refinement = true;
            retained.push_back(std::move(image));
          } else classified.memory_rejected.push_back(std::move(image));
        } else {
          retained.push_back(std::move(image));
        }
      }
      source = std::move(retained);
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"内存预算预检列表内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"内存预算预检列表数量超过运行时限制。"};
    }
    return {};
  };

  if (auto result = reject_from(classified.ordinary); !result) {
    return result;
  }
  try {
    std::vector<BatchLargeImageItem> retained;
    retained.reserve(classified.large_mode.size());
    for (auto& image : classified.large_mode) {
      const auto estimate = estimated_large_working_set_bytes(image, cfg, memory_limit);
      if (estimate > memory_limit)
        classified.memory_rejected.push_back({.file = std::move(image.file),
                                               .estimated_bytes = estimate,
                                               .dimensions = image.dimensions,
                                               .grid = true});
      else retained.push_back(std::move(image));
    }
    classified.large_mode = std::move(retained);
    return {};
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"大图内存预算预检列表内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"大图内存预算预检列表数量超过运行时限制。"};
  }
}

awj::compat::expected<std::vector<WorkGroup>, std::string> build_work_groups(
    const AppConfig& cfg, const std::vector<ClassifiedImageFile>& classified,
    std::span<const BatchLargeImageItem> large_items = {}, std::uint64_t memory_budget = 0) {
  std::vector<WorkGroup> groups;
  std::unordered_map<PathText, std::size_t> index_by_output;

  try {
    if (large_items.size() > groups.max_size() ||
        classified.size() > groups.max_size() - large_items.size())
      return awj::compat::unexpected{"批处理工作组数量超过运行时限制。"};
    const auto count = classified.size() + large_items.size();
    groups.reserve(count);
    index_by_output.reserve(count);
    const auto append = [&](const ImageFile& image, std::uint64_t estimate,
                            bool exclusive, const BatchLargeImageItem* large) {
      auto key = normalized_lower_path_key(output_path_for(cfg, image));
      const auto [it, inserted] = index_by_output.emplace(std::move(key), groups.size());
      if (inserted) groups.push_back(WorkGroup{});
      auto& group = groups[it->second];
      group.weight = saturated_add_uintmax(group.weight, image.bytes);
      group.estimated_bytes = std::max(group.estimated_bytes, estimate);
      group.exclusive_refinement = group.exclusive_refinement || exclusive;
      group.files.push_back(image);
      if (large) group.large_items.push_back(*large);
    };
    for (const auto& entry : classified)
      append(entry.file, entry.estimated_bytes, entry.exclusive_refinement, nullptr);
    for (const auto& item : large_items)
      append(item.file, estimated_large_working_set_bytes(item, cfg, memory_budget), false, &item);
    // 跨普通/Grid 的同名输出仍严格依照原输入索引串行，不能按分类次序重排。
    for (auto& group : groups) {
      awj::compat::ranges::sort(group.files, [](const auto& a, const auto& b) { return a.index < b.index; });
      awj::compat::ranges::sort(group.large_items, [](const auto& a, const auto& b) { return a.file.index < b.file.index; });
    }
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"批处理工作组内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"批处理工作组数量超过运行时限制。"};
  }

  awj::compat::ranges::sort(groups, [](const WorkGroup& left, const WorkGroup& right) {
    return left.weight > right.weight;
  });
  return groups;
}

std::uint64_t estimated_large_working_set_bytes(
    const BatchLargeImageItem& item, const AppConfig& cfg, std::uint64_t memory_budget) noexcept {
  return estimated_regular_memory(item.dimensions, cfg, item.file.bytes, true, nullptr, memory_budget).bytes;
}

std::uint64_t largest_large_mode_working_set(
    const std::vector<BatchLargeImageItem>& items, const AppConfig& cfg, std::uint64_t memory_budget) noexcept {
  std::uint64_t estimate = 1;
  for (const auto& item : items) {
    estimate = std::max(estimate, estimated_large_working_set_bytes(item, cfg, memory_budget));
  }
  return estimate;
}

std::string format_result_line(const EncodeResult& result) {
  const auto warning_suffix = [&] {
    return result.ok && !result.skipped && !result.message.empty() &&
                   result.message != "OK"
               ? awj::compat::format(" [WARN] {}", result.message)
               : std::string{};
  };
  if (result.ok) {
    if (result.skipped) {
      return awj::compat::format("[SKIP] {:04} {} -> 已存在", result.index + 1,
                         path_to_utf8(result.input_path.filename()));
    }

    const double ratio = result.original_bytes == 0
                             ? 0.0
                             : static_cast<double>(result.output_bytes) /
                                   static_cast<double>(result.original_bytes);
    if (result.requested_visual_quality) {
      if (result.lossless) {
        return awj::compat::format(
            "[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s, VQ {}→lossless, q{}, "
            "{} 次){}",
            result.index + 1, path_to_utf8(result.input_path.filename()),
            path_to_utf8(result.output_path.filename()),
            format_size(result.output_bytes), ratio * 100.0, result.seconds,
            *result.requested_visual_quality, result.final_encoder_quality,
            result.search_attempt_count, warning_suffix());
      }
      // 注意：Studio 会在 src/ui/main.cpp 里对子进程 stdout 嗅探 "未达标" 来给
      // 队列行打警告标记。这条文本属于跨进程约定，必须保持中文；worker 输出与
      // 日志不跟随界面语言。改动前先看那边的说明。
      const char* target_state =
          result.visual_quality_target_met ? "" : ", 未达标兜底";
      return awj::compat::format(
          "[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s, VQ {}→{:.2f}{}, q{}, "
          "{} 次){}",
          result.index + 1, path_to_utf8(result.input_path.filename()),
          path_to_utf8(result.output_path.filename()),
          format_size(result.output_bytes), ratio * 100.0, result.seconds,
          *result.requested_visual_quality, result.visual_score, target_state,
          result.final_encoder_quality, result.search_attempt_count,
          warning_suffix());
    }
    return awj::compat::format(
        "[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s){}", result.index + 1,
        path_to_utf8(result.input_path.filename()),
        path_to_utf8(result.output_path.filename()),
        format_size(result.output_bytes), ratio * 100.0, result.seconds,
        warning_suffix());
  }

  if (result.canceled) {
    return awj::compat::format("[CANCEL] {:04} {} -> {}", result.index + 1,
                       path_to_utf8(result.input_path.filename()),
                       result.message);
  }

  return awj::compat::format("[FAIL] {:04} {} -> {}", result.index + 1,
                     path_to_utf8(result.input_path.filename()),
                     result.message);
}

}  // namespace pipeline_detail

void emit_progress(const ProgressCallback& progress,
                   const BatchProgress& event);

namespace pipeline_detail {

struct RecycleObject {
  std::uint64_t volume{};
  std::array<std::uint64_t, 2> file{};
  bool operator==(const RecycleObject&) const = default;
};
struct RecycleObjectHash {
  std::size_t operator()(const RecycleObject& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.volume ^ value.file[0] ^ value.file[1]);
  }
};
struct RecycleIdentity {
  RecycleObject object{};
  std::uint64_t bytes{}, modified{}, changed{}, modified_ns{}, changed_ns{}, links{};
  bool operator==(const RecycleIdentity&) const = default;
};

// Capture before any worker starts, then compare immediately before recycling.
// Identity capture needs no content read; optional CSV SHA-256 is captured
// later, before recycling. Reads may change atime, so it is not an identity
// field. Unsupported identities fail closed.
awj::compat::expected<RecycleIdentity, std::string> recycle_file_identity(const fs::path& path) {
  std::error_code path_ec;
  const auto absolute = core_detail::absolute_path_preserving_components(path, path_ec);
  if (path_ec || !core_detail::safe_unlinked_path(absolute.parent_path(), true))
    return awj::compat::unexpected{"路径包含链接或不可验证的父目录。"};
#ifdef _WIN32
  UniqueFileHandle file{CreateFileW(absolute.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_ID_INFO id{};
  FILE_BASIC_INFO basic{};
  LARGE_INTEGER size{};
  if (!file || GetFileType(file.get()) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(file.get(), &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      !GetFileInformationByHandleEx(file.get(), FileIdInfo, &id, sizeof(id)) ||
      !GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof(basic)) ||
      !GetFileSizeEx(file.get(), &size) || size.QuadPart < 0)
    return awj::compat::unexpected{"无法取得普通文件的稳定身份。"};
  RecycleIdentity identity{.object = {.volume = id.VolumeSerialNumber},
      .bytes = static_cast<std::uint64_t>(size.QuadPart),
      .modified = static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart),
      .changed = static_cast<std::uint64_t>(basic.ChangeTime.QuadPart),
      .links = info.nNumberOfLinks};
  static_assert(sizeof(identity.object.file) == sizeof(id.FileId.Identifier));
  std::memcpy(identity.object.file.data(), id.FileId.Identifier, sizeof(identity.object.file));
  return identity;
#else
  UniqueFileHandle file{::open(absolute.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  struct stat info{};
  if (!file || ::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
    return awj::compat::unexpected{"无法取得普通文件的稳定身份。"};
  return RecycleIdentity{.object = {.volume = static_cast<std::uint64_t>(info.st_dev),
                                  .file = {static_cast<std::uint64_t>(info.st_ino), 0}},
      .bytes = static_cast<std::uint64_t>(info.st_size),
      .modified = static_cast<std::uint64_t>(info.st_mtim.tv_sec),
      .changed = static_cast<std::uint64_t>(info.st_ctim.tv_sec),
      .modified_ns = static_cast<std::uint64_t>(info.st_mtim.tv_nsec),
      .changed_ns = static_cast<std::uint64_t>(info.st_ctim.tv_nsec),
      .links = static_cast<std::uint64_t>(info.st_nlink)};
#endif
}

struct SourceRecycleEntry {
  fs::path source{};
  std::optional<RecycleIdentity> identity{};
  std::string retain_reason{};
};
struct SourceRecyclePlan {
  std::vector<SourceRecycleEntry> entries{};
};

awj::compat::expected<SourceRecyclePlan, std::string> make_source_recycle_plan(
    const AppConfig& cfg, std::span<const ImageFile> files,
    awj::compat::stop_token stop = {}) {
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  SourceRecyclePlan plan;
  plan.entries.resize(files.size());
  std::unordered_map<PathText, std::size_t> inputs;
  std::unordered_map<RecycleObject, std::size_t, RecycleObjectHash> objects;
  std::vector<PathText> keys(files.size());
  std::vector<std::optional<RecycleObject>> dependency_objects(files.size());
  for (const auto& file : files) {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (file.index >= files.size()) return awj::compat::unexpected{"输入序号无效。"};
    auto& entry = plan.entries[file.index];
    if (!entry.source.empty()) return awj::compat::unexpected{"输入序号重复。"};
    std::error_code ec;
    entry.source = core_detail::absolute_path_preserving_components(file.path, ec);
    if (ec) return awj::compat::unexpected{"无法规范化输入保护路径。"};
    const auto canonical = fs::weakly_canonical(entry.source, ec);
    if (ec) return awj::compat::unexpected{"无法规范化输入保护路径。"};
    keys[file.index] = normalized_lower_path_key(canonical);
    ++inputs[keys[file.index]];
    auto identity = recycle_file_identity(entry.source);
    if (identity) {
      entry.identity = *identity;
      dependency_objects[file.index] = identity->object;
    } else {
      entry.retain_reason = identity.error();
      // An unsafe alias is not eligible for recycling, but its actual target
      // still protects every other input/output that shares that file. Never
      // promote this canonical-only observation to a recyclable source identity.
      if (auto dependency = recycle_file_identity(canonical); dependency)
        dependency_objects[file.index] = dependency->object;
    }
    if (dependency_objects[file.index]) ++objects[*dependency_objects[file.index]];
  }
  // Apply input protection for every mode, including explicit overwrite and
  // manifests. Hard-link aliases are not caught by a pathname-only comparison.
  for (const auto& file : files) {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    std::error_code ec;
    const auto output = fs::weakly_canonical(output_path_for(cfg, file), ec);
    if (ec || inputs.contains(normalized_lower_path_key(output)))
      return awj::compat::unexpected{"输出路径会覆盖队列输入，已保留所有源文件。"};
    if (auto identity = recycle_file_identity(output); identity && objects.contains(identity->object))
      return awj::compat::unexpected{"输出路径是队列输入的硬链接，已保留所有源文件。"};
  }
  for (std::size_t i = 0; i < plan.entries.size(); ++i) {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    auto& entry = plan.entries[i];
    if (inputs[keys[i]] > 1 || (dependency_objects[i] && objects[*dependency_objects[i]] > 1))
      entry.retain_reason = "重复源或其他输入仍依赖同一文件。";
    else if (entry.identity && entry.identity->links != 1)
      entry.retain_reason = "源文件存在硬链接，无法安全独立回收。";
  }
  return plan;
}

#ifdef _WIN32
// FOF_ALLOWUNDO only preserves undo *if possible* and is not safe here.
// FOFX_RECYCLEONDELETE explicitly requests the system Recycle Bin (Win8+).
// The sink also vetoes a permanent-delete notification and rechecks identity.
class RecycleOnlySink final : public IFileOperationProgressSink {
 public:
  RecycleOnlySink(const fs::path& source, const RecycleIdentity& identity,
      const fs::path& output, const RecycleIdentity& output_identity,
      awj::compat::stop_token stop) noexcept
      : source_{source}, identity_{identity}, output_{output}, output_identity_{output_identity}, stop_{stop} {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IFileOperationProgressSink)) return E_NOINTERFACE;
    *object = static_cast<IFileOperationProgressSink*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs_; } // stack-owned for this synchronous call
  HRESULT STDMETHODCALLTYPE StartOperations() override { started = true; return S_OK; }
  HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT result) override {
    finished = true;
    finish_result = result;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return E_ABORT; }
  HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ABORT; }
  HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ABORT; }
  HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem* item) override {
    pre_delete_seen = true;
    pre_delete_flags = flags;
    const auto reject = [&](std::string_view reason) noexcept {
      rejection = reason;
      pre_delete_result = E_ABORT;
      return E_ABORT;
    };
    if (!(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE)) return reject("permanent-delete notification");
    if (stop_.stop_requested()) return reject("canceled");
    try {
      if (!item) return reject("missing Shell item");
      PWSTR raw{};
      pre_name_result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
      struct FreeName { PWSTR value; ~FreeName() { CoTaskMemFree(value); } } name{raw};
      if (FAILED(pre_name_result) || !raw) return reject("source path unavailable");
      const fs::path actual{raw};
      if (normalized_lower_path_key(actual) != normalized_lower_path_key(source_))
        return reject("source path mismatch");
      const auto current = recycle_file_identity(source_);
      const auto output = recycle_file_identity(output_);
      if (!current) return reject("source identity unavailable");
      if (*current != identity_) return reject("source identity changed");
      if (!output) return reject("output identity unavailable");
      if (*output != output_identity_) return reject("output identity changed");
      permitted = true;
      pre_delete_result = S_OK;
      return S_OK;
    } catch (...) { return reject("validation exception"); }
  }
  HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD flags, IShellItem*, HRESULT result, IShellItem* recycled) override {
    post_delete_seen = true;
    post_delete_flags = flags;
    post_delete_result = result;
    recycled_item_returned = recycled != nullptr;
    confirmed = permitted && SUCCEEDED(result) && recycled != nullptr;
    if (confirmed) {
      try {
        PWSTR raw{};
        if (SUCCEEDED(recycled->GetDisplayName(SIGDN_FILESYSPATH, &raw))) {
          struct FreeName { PWSTR value; ~FreeName() { CoTaskMemFree(value); } } name{raw};
          recycled_path = fs::path{raw};
        }
        raw = nullptr;
        if (SUCCEEDED(recycled->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw))) {
          struct FreeName { PWSTR value; ~FreeName() { CoTaskMemFree(value); } } name{raw};
          recycled_shell_item = raw;
        }
      } catch (...) { /* the Shell still confirmed a recoverable item */ }
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return E_ABORT; }
  HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
  bool started{}, finished{}, pre_delete_seen{}, post_delete_seen{};
  bool permitted{}, confirmed{}, recycled_item_returned{};
  DWORD pre_delete_flags{}, post_delete_flags{};
  HRESULT finish_result{E_PENDING}, pre_delete_result{E_PENDING},
      pre_name_result{E_PENDING}, post_delete_result{E_PENDING};
  std::string_view rejection{"none"};
  fs::path recycled_path{};
  std::wstring recycled_shell_item{};
 private:
  std::atomic<ULONG> refs_{1};
  const fs::path& source_;
  const RecycleIdentity& identity_;
  const fs::path& output_;
  const RecycleIdentity& output_identity_;
  awj::compat::stop_token stop_;
};
#else
struct GioTrashApi {
  void* library{};
  void* (*new_for_path)(const char*){};
  int (*trash)(void*, void*, void**){};
  void (*unref)(void*){};
  explicit operator bool() const noexcept { return library && new_for_path && trash && unref; }
};
const GioTrashApi& gio_trash_api() noexcept {
  // GIO can retain global module callbacks/threads: keep it loaded for process
  // lifetime, rather than dlclose-ing code still referenced by the desktop.
  static const auto api = [] {
    GioTrashApi value;
    value.library = ::dlopen("libgio-2.0.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (value.library) {
      value.new_for_path = reinterpret_cast<decltype(value.new_for_path)>(::dlsym(value.library, "g_file_new_for_path"));
      value.trash = reinterpret_cast<decltype(value.trash)>(::dlsym(value.library, "g_file_trash"));
      value.unref = reinterpret_cast<decltype(value.unref)>(::dlsym(value.library, "g_object_unref"));
    }
    return value;
  }();
  return api;
}
#endif

awj::compat::expected<void, std::string> recycle_successful_source(
    const AppConfig& cfg, const SourceRecyclePlan& plan, EncodeResult& result,
    awj::compat::stop_token stop_token = {}) {
  if (!cfg.delete_source || !result.processed || !result.ok || result.skipped ||
      result.canceled || stop_token.stop_requested()) return {};
  if (result.index >= plan.entries.size())
    return awj::compat::unexpected{"源文件身份记录缺失。"};
  const auto& entry = plan.entries[result.index];
  if (!entry.retain_reason.empty()) return awj::compat::unexpected{entry.retain_reason};
  if (!entry.identity) return awj::compat::unexpected{"源文件身份不可验证。"};
  std::error_code input_ec;
  const auto actual_input = core_detail::absolute_path_preserving_components(result.input_path, input_ec);
  // Validate the result's raw spelling too; a lexical key alone can conflate
  // a link/.. path with an unrelated equal-sized source.
  const auto input_identity = recycle_file_identity(actual_input);
  if (input_ec || !input_identity || *input_identity != *entry.identity ||
      normalized_lower_path_key(actual_input) != normalized_lower_path_key(entry.source) ||
      result.original_bytes != entry.identity->bytes)
    return awj::compat::unexpected{"成功结果与扫描源文件不匹配。"};
  auto current = recycle_file_identity(entry.source);
  if (!current || *current != *entry.identity)
    return awj::compat::unexpected{"源路径或文件身份已变化。"};
  std::error_code output_ec;
  const auto output_path = core_detail::absolute_path_preserving_components(result.output_path, output_ec);
  auto output = recycle_file_identity(output_path);
  if (output_ec || !output || !result.output_bytes || output->bytes != result.output_bytes || output->links != 1)
    return awj::compat::unexpected{"最终输出未落盘或无法安全确认。"};
  if (output->object == current->object ||
      normalized_lower_path_key(output_path) == normalized_lower_path_key(entry.source))
    return awj::compat::unexpected{"最终输出与源文件相同。"};
  if (stop_token.stop_requested()) return {};
  if (cfg.write_log || cfg.write_summary) {
    auto digest = file_sha256_hex(entry.source);
    const auto after_hash = recycle_file_identity(entry.source);
    if (!digest || !after_hash || *after_hash != *entry.identity)
      return awj::compat::unexpected{"回收前无法保存可靠的源 SHA-256，源文件已保留。"};
    result.input_sha256_snapshot = std::move(*digest);
  }
#ifdef _WIN32
  const auto recycle_on_sta = [&]() -> awj::compat::expected<void, std::string> {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    struct ComScope { HRESULT result; ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); } } com{initialized};
    if (FAILED(initialized))
      return awj::compat::unexpected{awj::compat::format(
          "系统回收站 STA 初始化失败：HRESULT=0x{:08X}。", static_cast<unsigned int>(initialized))};
    RecycleOnlySink sink{entry.source, *current, output_path, *output, stop_token};
    Microsoft::WRL::ComPtr<IFileOperation> operation;
    Microsoft::WRL::ComPtr<IShellItem> item;
    std::string_view stage = "CoCreateInstance";
    HRESULT hr = CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&operation));
    if (SUCCEEDED(hr)) {
      stage = "SetOperationFlags";
      hr = operation->SetOperationFlags(
          FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE | FOFX_NOCOPYHOOKS |
          FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMATION | FOF_NO_CONNECTED_ELEMENTS | FOF_NORECURSION);
    }
    if (SUCCEEDED(hr)) {
      stage = "SHCreateItemFromParsingName";
      // Win32 file I/O accepts '/', but the Shell parser can reject it with
      // E_INVALIDARG. Change separators only; never collapse raw link/.. parts.
      auto shell_source = entry.source;
      shell_source.make_preferred();
      hr = SHCreateItemFromParsingName(shell_source.c_str(), nullptr, IID_PPV_ARGS(&item));
    }
    if (SUCCEEDED(hr)) {
      stage = "DeleteItem";
      hr = operation->DeleteItem(item.Get(), &sink);
    }
    if (SUCCEEDED(hr)) {
      stage = "PerformOperations";
      hr = operation->PerformOperations();
    }
    BOOL aborted = TRUE;
    if (SUCCEEDED(hr)) {
      stage = "GetAnyOperationsAborted";
      hr = operation->GetAnyOperationsAborted(&aborted);
    }
    // Preserve any exact recovery reference even if a later operation check
    // fails, so diagnostics/tests can restore this item without bin enumeration.
    result.recycled_source_path = std::move(sink.recycled_path);
    result.recycled_source_shell_item = std::move(sink.recycled_shell_item);
    if (FAILED(hr) || aborted || !sink.confirmed)
      return awj::compat::unexpected{awj::compat::format(
          "系统回收未完成或未确认，不会降级永久删除：stage={}, HRESULT=0x{:08X}, aborted={}, "
          "Start={}, Finish={} (0x{:08X}), Pre={} (flags=0x{:08X}, HRESULT=0x{:08X}, name=0x{:08X}, reject={}), "
          "Post={} (flags=0x{:08X}, HRESULT=0x{:08X}, recycled_item={})。",
          stage, static_cast<unsigned int>(hr), aborted != FALSE,
          sink.started, sink.finished, static_cast<unsigned int>(sink.finish_result),
          sink.pre_delete_seen, static_cast<unsigned int>(sink.pre_delete_flags),
          static_cast<unsigned int>(sink.pre_delete_result), static_cast<unsigned int>(sink.pre_name_result), sink.rejection,
          sink.post_delete_seen, static_cast<unsigned int>(sink.post_delete_flags),
          static_cast<unsigned int>(sink.post_delete_result), sink.recycled_item_returned)};
    return {};
  };
  // WIC workers use MTA. Shell file operations require STA; use a short-lived,
  // joined owner instead of changing the decoder's apartment or detaching work.
  awj::compat::expected<void, std::string> status = awj::compat::unexpected{"系统回收线程未启动。"};
  {
    awj::compat::scoped_worker owner{[&] {
      try { status = recycle_on_sta(); }
      catch (...) { status = awj::compat::unexpected{"系统回收检查异常，未降级永久删除。"}; }
    }};
  }
  return status;
#else
  const auto expected_output = *output;
  const auto& api = gio_trash_api();
  if (!api) return awj::compat::unexpected{"桌面 GIO 回收机制不可用。"};
  void* file = api.new_for_path(entry.source.c_str());
  if (!file) return awj::compat::unexpected{"无法创建桌面回收请求。"};
  struct GioFileScope { const GioTrashApi& api; void* file; ~GioFileScope() { api.unref(file); } } guard{api, file};
  // g_file_trash fails (e.g. G_IO_ERROR_NOT_SUPPORTED) if recovery is not
  // available; never substitute g_file_delete, unlink, or a directory named trash.
  current = recycle_file_identity(entry.source);
  output = recycle_file_identity(output_path);
  if (stop_token.stop_requested()) return {};
  if (!current || *current != *entry.identity || !output || *output != expected_output ||
      output->object == current->object)
    return awj::compat::unexpected{"回收前源文件或输出身份已变化。"};
  if (!api.trash(file, nullptr, nullptr))
    return awj::compat::unexpected{"桌面回收失败，源文件已保留；不会降级永久删除。"};
#endif
  return {};
}

struct WorkExecutionResult {
  int worker_failures{};
};

template <class Function>
void best_effort(Function&& fn) noexcept {
  try {
    fn();
  } catch (...) {
  }
}

void log_item_failure_noexcept(FileLogger& logger, const EncodeResult& result) noexcept {
  if (result.ok || !result.processed || result.canceled) return;
  best_effort([&] {
    if (!logger.enabled()) return;
    if (result.decode_stage.empty()) {
      logger.warn(format_result_line(result));
      return;
    }
    logger.warn(awj::compat::format(
        "{} [decoder={}, decode_stage={}, source_metadata_available={}, source_icc={}]",
        format_result_line(result), result.decoder_id.empty() ? "unknown" : result.decoder_id,
        result.decode_stage, result.source_metadata_available,
        result.source_metadata_available ? (result.source_has_icc ? "true" : "false") : "unknown"));
  });
}

void emit_item_started_noexcept(
    const AppConfig& cfg, const ImageFile& image, std::size_t completed,
    std::size_t total, const ProgressCallback& progress) noexcept {
  best_effort([&] {
    emit_progress(
        progress,
        BatchProgress{
            .kind = BatchEventKind::item_started,
            .completed = completed,
            .total = total,
            .result = EncodeResult{.index = image.index,
                                   .input_path = image.path,
                                   .output_path = output_path_for(cfg, image),
                                   .original_bytes = image.bytes,
                                   .message = "正在转码"}});
  });
}

void report_worker_warning_noexcept(
    FileLogger& logger, const ProgressCallback& progress, std::size_t completed,
    std::size_t total, std::string_view log_message,
    std::string_view progress_message) noexcept {
  try {
    logger.error(log_message);
  } catch (...) {
  }
  try {
    emit_progress(progress,
                  BatchProgress{.kind = BatchEventKind::warning,
                                .completed = completed,
                                .total = total,
                                .text = std::string{progress_message}});
  } catch (...) {
  }
}

void recycle_source_noexcept(const AppConfig& cfg, const SourceRecyclePlan& plan,
    EncodeResult& result, FileLogger& logger, const ProgressCallback& progress,
    std::size_t completed, std::size_t total, awj::compat::stop_token stop_token) noexcept {
  if (!cfg.delete_source) return;
  try {
    auto recycled = recycle_successful_source(cfg, plan, result, stop_token);
    if (recycled) return;
    const auto warning = awj::compat::format("源文件未确认回收：{}", recycled.error());
    result.message = result.message.empty() || result.message == "OK"
        ? warning : awj::compat::format("{}；{}", result.message, warning);
    logger.warn(warning);
    emit_progress(progress, BatchProgress{.kind = BatchEventKind::warning,
        .completed = completed, .total = total, .text = "[WARN] " + warning});
  } catch (...) {
    logger.warn("源文件回收检查异常，未执行永久删除。");
    best_effort([&] {
      emit_progress(progress, BatchProgress{.kind = BatchEventKind::warning,
          .completed = completed, .total = total,
          .text = "[WARN] 源文件回收检查异常，未执行永久删除。"});
    });
  }
}

void report_worker_exception_noexcept(FileLogger& logger,
                                      const ProgressCallback& progress,
                                      std::size_t completed,
                                      std::size_t total) noexcept {
  report_worker_warning_noexcept(logger, progress, completed, total,
                                 "worker failed",
                                 "[WARN] 工作线程异常，已停止该线程。");
}


struct MemoryAdmissionGuard {
  WorkMemoryAdmission* admission{};
  std::uint64_t amount{};

  MemoryAdmissionGuard() = default;
  MemoryAdmissionGuard(WorkMemoryAdmission& owner, std::uint64_t reserved) noexcept
      : admission(&owner), amount(reserved) {}
  ~MemoryAdmissionGuard() {
    if (admission != nullptr) {
      admission->release(amount);
    }
  }
  bool try_resize(std::uint64_t requested) noexcept {
    return admission && admission->try_resize(amount, requested);
  }
  MemoryAdmissionGuard(const MemoryAdmissionGuard&) = delete;
  MemoryAdmissionGuard& operator=(const MemoryAdmissionGuard&) = delete;
};

EncodeResult encode_large_mode_item(const AppConfig& cfg, FileLogger& logger,
    const BatchLargeImageItem& item, ResourcePlan resource_plan,
    awj::compat::stop_token stop_token,
    std::function<bool(std::uint64_t)> memory_reservation = {},
    NativeBackend::CpuBudgetCallback cpu_budget = {});

WorkExecutionResult encode_work_groups(
    const AppConfig& cfg, FileLogger& logger, const ResourcePlan& resource_plan,
    const std::vector<WorkGroup>& work, const SourceRecyclePlan& recycle_plan,
    std::size_t progress_total,
    std::atomic<std::size_t>& completed, std::vector<EncodeResult>& results,
    const ProgressCallback& progress, awj::compat::stop_token stop_token) {
  WorkExecutionResult execution{};
  if (work.empty()) {
    return execution;
  }

  const int jobs = std::max(1, std::min(resource_plan.global_thread_budget,
                                       count_to_int_saturated(work.size())));
  std::atomic<int> worker_failures{0};
  std::atomic<int> worker_capacity{jobs};
  WorkMemoryAdmission admission{resource_plan.memory_limit_bytes, work};
  WorkCpuAdmission cpu{resource_plan.global_thread_budget,
      static_cast<std::size_t>(admission.runnable_count(jobs))};

  std::vector<awj::compat::scoped_worker> workers;
  try {
    workers.reserve(static_cast<std::size_t>(jobs));
  } catch (const std::bad_alloc&) {
    worker_failures.fetch_add(1);
    try {
      const auto text =
          awj::compat::format("[WARN] 工作线程列表内存不足，需要 {} 个线程槽。", jobs);
      report_worker_warning_noexcept(logger, progress, completed.load(),
                                     progress_total, text, text);
    } catch (...) {
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "worker list allocation failed", "[WARN] 工作线程列表内存不足。");
    }
    execution.worker_failures = worker_failures.load();
    return execution;
  } catch (const std::length_error&) {
    worker_failures.fetch_add(1);
    constexpr std::string_view text = "[WARN] 工作线程列表数量超过运行时限制。";
    report_worker_warning_noexcept(logger, progress, completed.load(),
                                   progress_total, text, text);
    execution.worker_failures = worker_failures.load();
    return execution;
  }
  for (const int worker_index : std::views::iota(0, jobs)) {
    try {
      workers.emplace_back([&, policy = encoding_defaults::current_memory_policy()] {
        struct WorkerCapacityGuard {
          std::atomic<int>& capacity;
          WorkMemoryAdmission& memory;
          WorkCpuAdmission& cpu;
          ~WorkerCapacityGuard() {
            capacity.fetch_sub(1);
            cpu.set_runnable_hint(static_cast<std::size_t>(memory.runnable_count(capacity.load())));
          }
        } capacity_guard{worker_capacity, admission, cpu};
        const encoding_defaults::ScopedMemoryPolicy memory_scope{policy};
        WicFallbackComApartment com{cfg.allow_wic_fallback};
        try {
          std::optional<AppConfig> worker_cfg;
          if (cfg.allow_wic_fallback && !com.usable()) {
            worker_cfg.emplace(cfg);
            worker_cfg->allow_wic_fallback = false;
            try {
              const auto text = awj::compat::format(
                  "[WARN] 工作线程 WIC fallback COM 初始化失败，已禁用该线程的 "
                  "WIC fallback: 0x{:08X}",
                  static_cast<unsigned int>(com.init()));
              report_worker_warning_noexcept(logger, progress, completed.load(),
                                             progress_total, text, text);
            } catch (...) {
              report_worker_warning_noexcept(
                  logger, progress, completed.load(), progress_total,
                  "worker WIC fallback COM initialization failed",
                  "[WARN] 工作线程 WIC fallback COM 初始化失败，已禁用该线程的 "
                  "WIC fallback。");
            }
          }
          const auto& effective_cfg = worker_cfg ? *worker_cfg : cfg;
          while (true) {
            if (stop_token.stop_requested()) {
              break;
            }

            const auto ticket = admission.acquire(stop_token);
            if (!ticket) break;
            const auto& group = work[ticket->index];
            MemoryAdmissionGuard reservation{admission, ticket->bytes};
            const auto group_floor = group.files.size() > 1 ? ticket->bytes : 0;
            const auto update_cpu_hint = [&] {
              cpu.set_runnable_hint(static_cast<std::size_t>(
                  admission.runnable_count(worker_capacity.load())));
            };
            const auto resize_memory = [&](std::uint64_t bytes) {
              const bool resized = reservation.try_resize(std::max(bytes, group_floor));
              if (resized) update_cpu_hint();
              return resized;
            };
            update_cpu_hint();
            if (stop_token.stop_requested()) {
              break;
            }
            if (group.files.size() > 1 &&
                cfg.collision_mode == CollisionMode::overwrite) {
              best_effort([&] {
                emit_progress(
                    progress,
                    BatchProgress{.kind = BatchEventKind::warning,
                                  .completed = completed.load(),
                                  .total = progress_total,
                                  .text = awj::compat::format(
                                      "[WARN] 输出重名: {} 个输入将依次覆盖 {}",
                                      group.files.size(),
                                      display_path_for_user(output_path_for(
                                          cfg, group.files.back())))});
              });
            }
            for (const auto& image : group.files) {
              if (stop_token.stop_requested()) {
                break;
              }
              emit_item_started_noexcept(cfg, image, completed.load(),
                                         progress_total, progress);
              WorkCpuAdmission::Lease lease;
              const NativeBackend::CpuBudgetCallback cpu_budget = [&](int demand) {
                if (demand <= 0) { lease.reset(); update_cpu_hint(); return 0; }
                update_cpu_hint();
                if (!lease) {
                  auto acquired = cpu.acquire(demand, stop_token);
                  if (!acquired) return 0;
                  lease = std::move(*acquired);
                  return lease.threads();
                }
                return lease.try_resize(demand, stop_token);
              };
              auto file_resources = resource_plan;
              file_resources.file_parallelism = 1;
              file_resources.encoder_threads_per_file = 1;
              file_resources.memory_reservation_bytes = reservation.amount;
              results[image.index].memory_plan.reserved_peak_bytes = reservation.amount;
              const auto large = std::lower_bound(group.large_items.begin(), group.large_items.end(), image.index,
                  [](const auto& item, std::size_t index) { return item.file.index < index; });
              const bool is_grid = large != group.large_items.end() && large->file.index == image.index;
              if (is_grid) {
                best_effort([&] {
                  logger.info(large_image_log_text(*large));
                  emit_progress(progress, BatchProgress{.kind = BatchEventKind::message,
                      .completed = completed.load(), .total = progress_total,
                      .large_image = *large, .text = large_image_action_text(*large)});
                });
              }
              auto result = is_grid
                  ? encode_large_mode_item(effective_cfg, logger, *large, file_resources,
                      stop_token, resize_memory, cpu_budget)
                  : NativeBackend{effective_cfg, logger, file_resources, resize_memory, cpu_budget}.encode(image, stop_token);
              const auto result_index = result.index;
              inherit_memory_diagnostics(result, results[result_index]);
              results[result_index] = std::move(result);
              // Recycle before item_finished, never in the end-of-batch sweep.
              recycle_source_noexcept(cfg, recycle_plan, results[result_index], logger,
                  progress, completed.load(), progress_total, stop_token);
              log_item_failure_noexcept(logger, results[result_index]);
              const auto done = completed.fetch_add(1) + 1;
              try {
                BatchProgress event{
                    .kind = BatchEventKind::item_finished,
                    .completed = done,
                    .total = progress_total,
                    .result = results[result_index],
                    .text = format_result_line(results[result_index])};
                emit_progress(progress, event);
              } catch (...) {
                report_worker_warning_noexcept(
                    logger, progress, done, progress_total,
                    "item progress reporting failed",
                    "[WARN] 单项进度报告生成失败，结果已记录。");
              }
            }
          }
        } catch (const std::exception&) {
          worker_failures.fetch_add(1);
          report_worker_exception_noexcept(logger, progress, completed.load(),
                                           progress_total);
        } catch (...) {
          worker_failures.fetch_add(1);
          report_worker_warning_noexcept(
              logger, progress, completed.load(), progress_total,
              "worker failed: unknown exception",
              "[WARN] 工作线程异常，已停止该线程: 未知异常");
        }
      });
    } catch (const std::bad_alloc&) {
      worker_failures.fetch_add(1);
      try {
        const auto text = awj::compat::format(
            "[WARN] 创建工作线程失败，线程状态内存不足，已继续等待已启动线程: "
            "{}/{}",
            worker_index, jobs);
        report_worker_warning_noexcept(logger, progress, completed.load(),
                                       progress_total, text, text);
      } catch (...) {
        report_worker_warning_noexcept(
            logger, progress, completed.load(), progress_total,
            "worker thread creation allocation failed",
            "[WARN] "
            "创建工作线程失败，线程状态内存不足，已继续等待已启动线程。");
      }
      break;
    } catch (const std::system_error&) {
      worker_failures.fetch_add(1);
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "worker thread creation failed",
          "[WARN] 创建工作线程失败，已继续等待已启动线程。");
      break;
    } catch (const std::exception&) {
      worker_failures.fetch_add(1);
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "worker thread creation failed",
          "[WARN] 创建工作线程失败，已继续等待已启动线程。");
      break;
    }
  }
  worker_capacity.fetch_sub(jobs - static_cast<int>(workers.size()));
  cpu.set_runnable_hint(static_cast<std::size_t>(admission.runnable_count(worker_capacity.load())));
  workers.clear();
  execution.worker_failures = worker_failures.load();
  return execution;
}

EncodeResult encode_large_mode_item(const AppConfig& cfg, FileLogger& logger,
                                    const BatchLargeImageItem& item,
                                    ResourcePlan resource_plan,
                                    awj::compat::stop_token stop_token,
                                    std::function<bool(std::uint64_t)> memory_reservation,
                                    NativeBackend::CpuBudgetCallback cpu_budget) {
  resource_plan = normalize_memory_policy_resources(cfg, resource_plan);
  const auto estimate = estimated_regular_memory(item.dimensions, cfg, item.file.bytes, true, nullptr,
      resource_memory_budget(resource_plan));
  EncodeResult failed{
      .index = item.file.index,
      .input_path = item.file.path,
      .output_path = output_path_for(cfg, item.file),
      .original_bytes = item.file.bytes,
      .quality = cfg.quality,
      .requested_visual_quality = cfg.visual_quality,
      .final_encoder_quality = cfg.quality,
      .speed = cfg.speed.value_or(default_speed_for(cfg.output_format)),
      .processed = true};
  clear_unstarted_execution_metrics(failed);
  record_memory_diagnostics(failed, cfg, {}, resource_plan.memory_limit_bytes, estimate);
  if (stop_token.stop_requested()) {
    failed.canceled = true;
    failed.message = "任务已取消。";
    return failed;
  }

  const auto required_memory = estimate.bytes;
  if (resource_plan.memory_limit_bytes > 0 && required_memory > resource_plan.memory_limit_bytes) {
    failed.message = awj::compat::format("AOM Grid 预估工作集 {} 超过内存预算 {}。",
        format_size(required_memory), format_size(resource_plan.memory_limit_bytes));
    return failed;
  }
  AppConfig large_cfg = cfg;
  large_cfg.output_format = OutputFormat::avif;
  large_cfg.input_path = item.file.path;
  large_cfg.visual_quality.reset();

  try {
    if (!cfg.studio_large_action.empty() && cfg.studio_large_action != L"grid") {
      failed.message = "大图编码仅支持 AOM Grid。";
      return failed;
    }
    if (!item.decision.available_grid) {
      failed.message = "当前构建没有可用的 AOM Grid 编码器。";
      return failed;
    }
    const auto target = target_dimensions_for_config(item.dimensions, cfg);
    auto planned = plan_grid(GridPlanRequest{
        .width = target.width,
        .height = target.height,
        .mode = GridMode::auto_grid,
        .clamped_padding_enabled = large_cfg.experimental_clamped_grid_padding});
    if (!planned) {
      failed.message = awj::compat::format("Grid 规划失败：{}", planned.error());
      return failed;
    }
    large_cfg.avif_encoder = AvifEncoderMode::aom;
    NativeBackend backend{large_cfg, logger, resource_plan,
        std::move(memory_reservation), std::move(cpu_budget)};
    auto result = backend.encode_avif_grid(item.file, *planned, stop_token);
    inherit_memory_diagnostics(result, failed);
    return result;
  } catch (const std::exception&) {
    failed.message = "自动大图工作线程异常。";
    return failed;
  } catch (...) {
    failed.message = "自动大图工作线程异常：未知异常。";
    return failed;
  }

  failed.message = "没有可用的大图自动处理方式。";
  return failed;
}


void set_result_message_noexcept(EncodeResult& result,
                                 std::string_view message) noexcept {
  try {
    result.message = message;
  } catch (...) {
  }
}

void mark_unstarted_results(std::vector<EncodeResult>& results, bool canceled,
                            bool worker_failed) noexcept {
  for (auto& result : results) {
    if (result.large_image_queued || result.processed || result.ok ||
        result.canceled) {
      continue;
    }
    if (canceled) {
      result.canceled = true;
      set_result_message_noexcept(result, "任务已取消。");
    } else if (worker_failed) {
      result.processed = true;
      set_result_message_noexcept(result, "任务未启动：工作线程提前停止。");
    }
  }
}

}  // namespace pipeline_detail

void print_line(std::string_view text) {
  awj::compat::println("{}", text);
  std::fflush(stdout);
}

void emit_progress(const ProgressCallback& progress,
                   const BatchProgress& event) {
  try {
    if (progress) {
      progress(event);
    }
  } catch (...) {
    // 进度回调不应该影响编码任务；UI 层异常会被吞掉，批处理继续写 CSV。
  }
}

awj::compat::expected<BatchSummary, std::string> run_batch(
    AppConfig cfg, ProgressCallback progress = {},
    awj::compat::stop_token stop_token = {},
    std::span<const std::filesystem::path> input_paths = {}) {
  try {
    const auto canceled_preflight = [&](std::size_t total) {
      const BatchSummary summary{.canceled_count = pipeline_detail::count_to_int_saturated(total),
                                 .canceled = true, .exit_code = 130};
      emit_progress(progress, BatchProgress{.kind = BatchEventKind::summary,
          .total = total, .summary = summary, .text = "已取消，未启动后续分析或编码。"});
      return summary;
    };
    if (stop_token.stop_requested())
      return canceled_preflight(cfg.studio_large_action.empty() ? input_paths.size() : 1);
    cfg.write_log = cfg.write_summary = cfg.write_log || cfg.write_summary;
    if (auto frozen = freeze_operation_memory_policy(cfg); !frozen)
      return awj::compat::unexpected{frozen.error()};
    const encoding_defaults::ScopedMemoryPolicy memory_scope{*cfg.runtime_memory_policy};
#ifndef _WIN32
    cfg.allow_wic_fallback = false;
#endif
    if (!cfg.studio_queue_manifest.empty() &&
        !cfg.studio_large_action.empty()) {
      return awj::compat::unexpected{
          "Studio 队列 manifest 不能与手动大图 worker 同时使用。"};
    }
    if (!cfg.studio_large_action.empty()) {
      if (auto valid = validate_config(cfg); !valid) {
        return awj::compat::unexpected{valid.error()};
      }
      if (cfg.output_format != OutputFormat::avif) {
        return awj::compat::unexpected{"Studio 大图 worker 仅支持 AVIF 输出。"};
      }
      const auto memory_status = cfg.unlock_memory_limit || cfg.memory_limit_bytes != 0
          ? MemoryStatus{} : current_memory_status();
      const auto resolved_memory_limit = checked_effective_memory_limit(cfg, memory_status);
      if (!resolved_memory_limit) return awj::compat::unexpected{resolved_memory_limit.error()};
      const auto configured_memory_limit = *resolved_memory_limit;
      auto dimensions = probe_image_dimensions_for_path(
          cfg.input_path,
          DecoderRegistryOptions{.allow_wic_fallback = cfg.allow_wic_fallback,
              .stop_token = stop_token, .probe_memory_reservation_bytes = configured_memory_limit});
      if (!dimensions) {
        if (stop_token.stop_requested()) return canceled_preflight(1);
        return awj::compat::unexpected{dimensions.error()};
      }
      std::error_code file_ec;
      const auto bytes = std::filesystem::file_size(cfg.input_path, file_ec);
      if (file_ec) {
        return awj::compat::unexpected{awj::compat::format("读取文件大小失败: {}；系统错误：{}。",
                                           display_path_for_user(cfg.input_path),
                                           file_ec.message())};
      }
      const bool grid_action = cfg.studio_large_action == L"grid";
      if (grid_action) {
        auto animated = input_uses_animation_reader(cfg.input_path, stop_token);
        if (!animated) {
          if (stop_token.stop_requested()) return canceled_preflight(1);
          return awj::compat::unexpected{animated.error()};
        }
        if (*animated) return awj::compat::unexpected{"保留动画时不能使用静态 Grid。"};
      }
      auto decision = classify_large_image(*dimensions, grid_action);
      decision.klass = LargeImageClass::large_mode_required;
      if (decision.reason == LargeImageReason::none) {
        decision.reason_text = "Studio 手动大图 worker。";
      }
      auto item = BatchLargeImageItem{
          .file = ImageFile{.index = 0, .path = cfg.input_path, .bytes = bytes},
          .dimensions = *dimensions,
          .decision = std::move(decision)};
      std::vector<ImageFile> single_file{item.file};
      if (auto resolved = resolve_batch_output_paths(cfg, single_file, stop_token); !resolved) {
        if (stop_token.stop_requested()) return canceled_preflight(1);
        return awj::compat::unexpected{resolved.error()};
      }
      item.file = single_file.front();
      auto recycle_plan = pipeline_detail::make_source_recycle_plan(cfg, single_file, stop_token);
      if (!recycle_plan) {
        if (stop_token.stop_requested()) return canceled_preflight(1);
        return awj::compat::unexpected{recycle_plan.error()};
      }
      const auto output_dir = output_dir_for(cfg);
      FileLogger logger{output_dir, cfg.write_log};
      if (stop_token.stop_requested()) return canceled_preflight(1);
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::message,
                                    .completed = 0,
                                    .total = 1,
                                    .large_image = item,
                                    .text = pipeline_detail::large_image_action_text(item)});
      });
      const auto estimate = estimated_regular_memory(
          item.dimensions, cfg, item.file.bytes, true, nullptr, configured_memory_limit);
      const auto large_resource_plan = plan_large_mode_resources(
          plan_resources(ResourcePlanRequest{
              .automatic_thread_budget = cfg.max_jobs,
              .file_count = 1,
              .memory_limit_bytes = configured_memory_limit,
              .estimated_bytes_per_file = estimate.bytes}),
          1, estimate.bytes);
      pipeline_detail::emit_item_started_noexcept(
          cfg, item.file, 0, 1, progress);
      auto result = pipeline_detail::encode_large_mode_item(
          cfg, logger, item, large_resource_plan, stop_token);
      pipeline_detail::record_memory_diagnostics(
          result, cfg, memory_status, configured_memory_limit, estimate);
      pipeline_detail::recycle_source_noexcept(
          cfg, *recycle_plan, result, logger, progress, 0, 1, stop_token);
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::item_finished,
                                    .completed = 1,
                                    .total = 1,
                                    .result = result,
                                    .text = pipeline_detail::format_result_line(result)});
      });
      const bool canceled = stop_token.stop_requested() || result.canceled;
      const bool ok = result.ok;
      const BatchSummary summary{.ok_count = ok ? 1 : 0,
                                 .failed_count = !ok && !canceled ? 1 : 0,
                                 .canceled_count = canceled ? 1 : 0,
                                 .large_image_queued_count = 0,
                                 .original_total = result.original_bytes,
                                 .output_total = ok ? result.output_bytes : 0,
                                 .canceled = canceled,
                                 .exit_code = canceled ? 130 : (ok ? 0 : 2)};
      if (cfg.write_summary) {
        if (auto csv = write_csv(output_dir, std::span<const EncodeResult>{&result, 1}); !csv) {
          return awj::compat::unexpected{csv.error()};
        }
      }
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::summary,
                                    .completed = 1,
                                    .total = 1,
                                    .summary = summary,
                                    .text = awj::compat::format("{}：成功 {}，失败 {}，取消 {}。",
                                                        canceled ? "已取消" : "完成",
                                                        summary.ok_count,
                                                        summary.failed_count,
                                                        summary.canceled_count)});
      });
      return summary;
    }
    if (auto valid = validate_config(cfg); !valid) {
      return awj::compat::unexpected{valid.error()};
    }

    const auto output_dir = output_dir_for(cfg);

    std::vector<ImageFile> files;
    if (!cfg.studio_queue_manifest.empty()) {
      auto manifest = read_studio_queue_manifest(cfg.studio_queue_manifest, stop_token);
      if (!manifest) {
        if (stop_token.stop_requested()) return canceled_preflight(0);
        return awj::compat::unexpected{manifest.error()};
      }
      if (auto valid = pipeline_detail::validate_manifest_paths_for_config(
              cfg, manifest->files, stop_token);
          !valid) {
        if (stop_token.stop_requested()) return canceled_preflight(manifest->files.size());
        return awj::compat::unexpected{valid.error()};
      }
      files = std::move(manifest->files);
    } else {
      if (auto scanned = input_paths.empty() ? scan_images(cfg, files, stop_token)
                                             : scan_images(cfg, input_paths, files, stop_token);
          !scanned) {
        if (stop_token.stop_requested()) return canceled_preflight(files.size());
        return awj::compat::unexpected{scanned.error()};
      }
    }
    if (stop_token.stop_requested()) return canceled_preflight(files.size());

    // 统一快照保护所有任务；普通图与 Grid 使用相同输入依赖与回收契约。
    auto recycle_plan = pipeline_detail::make_source_recycle_plan(cfg, files, stop_token);
    if (!recycle_plan) {
      if (stop_token.stop_requested()) return canceled_preflight(files.size());
      return awj::compat::unexpected{recycle_plan.error()};
    }
    FileLogger logger{output_dir, cfg.write_log};
    logger.info("native backend enabled");
    if (cfg.write_log && !logger.enabled()) {
      pipeline_detail::best_effort([&] {
        const auto text =
            awj::compat::format("[WARN] 日志写入失败: {}", logger.last_error());
        emit_progress(progress, BatchProgress{.kind = BatchEventKind::warning,
                                              .text = text});
      });
    }
    if (files.empty()) {
      pipeline_detail::best_effort([&] {
        emit_progress(
            progress,
            BatchProgress{
                .kind = BatchEventKind::message,
                .text = "未找到图片。支持: "
                        "jpg, jpeg, jpe, jfif, png, webp, bmp, dib, rle, "
                        "tif, tiff, gif, jxl, avif, awsraw, dng, cr2, cr3, "
                        "nef, arw, rw2, orf, raf, pef, srw, x3f, 3fr, erf, "
                        "kdc, mrw, raw, heic, heif, jxr, wdp, hdp。"
                        "请确认输入目录中有支持格式的文件。"});
      });
      return BatchSummary{.exit_code = 0};
    }

    const auto disambiguated_count =
        awj::compat::ranges::count_if(files, &ImageFile::extension_disambiguated);
    if (disambiguated_count > 0) {
      pipeline_detail::best_effort([&] {
        const auto text = awj::compat::format(
            "[WARN] 同名不同扩展: {} 个输入会自动保留源扩展名，"
            "例如 1.jpg.avif / 1.bmp.avif",
            disambiguated_count);
        logger.warn(text);
        emit_progress(progress, BatchProgress{.kind = BatchEventKind::warning,
                                              .total = files.size(),
                                              .text = text});
      });
    }

    const auto memory_status = cfg.unlock_memory_limit || cfg.memory_limit_bytes != 0
        ? MemoryStatus{} : current_memory_status();
    const auto resolved_memory_limit = checked_effective_memory_limit(cfg, memory_status);
    if (!resolved_memory_limit) return awj::compat::unexpected{resolved_memory_limit.error()};
    const auto configured_memory_limit = *resolved_memory_limit;
    const auto probe_started = std::chrono::steady_clock::now();
    auto classified = pipeline_detail::classify_work_for_avif(cfg, files, configured_memory_limit, stop_token);
    if (!classified) {
      if (stop_token.stop_requested()) return canceled_preflight(files.size());
      return awj::compat::unexpected{classified.error()};
    }
    if (stop_token.stop_requested()) return canceled_preflight(files.size());
    pipeline_detail::best_effort([&] {
      const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - probe_started).count();
      const auto text = awj::compat::format("尺寸与内存分析完成：{} 个文件，计算线程预算 {}，耗时 {:.3f}s。",
          files.size(), cfg.max_jobs, seconds);
      logger.info(text);
      emit_progress(progress, BatchProgress{.kind = BatchEventKind::message,
          .total = files.size(), .text = text});
    });
    if (auto memory_filter = pipeline_detail::reject_over_memory_budget(
            *classified, configured_memory_limit, cfg);
        !memory_filter) {
      return awj::compat::unexpected{memory_filter.error()};
    }
    if (stop_token.stop_requested()) return canceled_preflight(files.size());
    auto work = pipeline_detail::build_work_groups(cfg, classified->ordinary,
        classified->large_mode, configured_memory_limit);
    if (!work) return awj::compat::unexpected{work.error()};
    const auto typical_bytes = classified->ordinary.empty()
        ? pipeline_detail::largest_large_mode_working_set(classified->large_mode, cfg, configured_memory_limit)
        : pipeline_detail::typical_decoded_bytes_per_file(classified->ordinary);
    const auto resource_plan = plan_resources(ResourcePlanRequest{
        .automatic_thread_budget = cfg.max_jobs,
        .file_count = pipeline_detail::count_to_int_saturated(std::max<std::size_t>(1, work->size())),
        .memory_limit_bytes = configured_memory_limit,
        .estimated_bytes_per_file = typical_bytes});
    const int worker_slots = std::min(resource_plan.global_thread_budget,
        pipeline_detail::count_to_int_saturated(work->size()));
    pipeline_detail::best_effort([&] {
      emit_progress(
          progress,
          BatchProgress{
              .kind = BatchEventKind::message,
              .total = files.size(),
              .text = awj::compat::format(
                  "共 {} 个文件；普通 {}，内存超限 {}，Grid "
                  "{}。共享任务槽位 {}，全局计算线程预算 {}，"
                  "按阶段与实际内存动态分配，内存限制 {}。",
                  files.size(), classified->ordinary.size(),
                  classified->memory_rejected.size(),
                  classified->large_mode.size(), worker_slots,
                  resource_plan.global_thread_budget,
                  resource_plan.memory_limit_bytes == 0
                      ? std::string{"未限制"}
                      : format_size(resource_plan.memory_limit_bytes))});
    });

    std::vector<EncodeResult> results;
    try {
      results.resize(files.size());
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"批处理结果列表内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"批处理结果列表数量超过运行时限制。"};
    }
    for (const auto& image : files) {
      results[image.index] =
          EncodeResult{.index = image.index,
                       .input_path = image.path,
                       .output_path = output_path_for(cfg, image),
                       .original_bytes = image.bytes,
                       .quality = cfg.quality,
                       .requested_visual_quality = cfg.visual_quality,
                       .gmsd_weight = GMSD_WEIGHT,
                       .msssim_weight = MSSSIM_WEIGHT,
                       .final_encoder_quality = cfg.quality,
                       .speed = cfg.speed.value_or(-1),
                       .quality_overridden_by_visual_quality =
                           cfg.visual_quality.has_value(),
                       .message = "未处理。"};
      pipeline_detail::clear_unstarted_execution_metrics(results[image.index]);
    }
    const auto record_classified = [&](const pipeline_detail::ClassifiedImageFile& image) {
      auto estimate = image.dimensions.width && image.dimensions.height
          ? estimated_regular_memory(image.dimensions, cfg, image.file.bytes, image.grid, nullptr, configured_memory_limit)
          : ImageMemoryEstimate{};
      estimate.bytes = image.estimated_bytes;
      results[image.file.index].memory_plan.exclusive_refinement = image.exclusive_refinement;
      pipeline_detail::record_memory_diagnostics(
          results[image.file.index], cfg, memory_status, configured_memory_limit, estimate);
    };
    for (const auto& image : classified->ordinary) record_classified(image);
    for (const auto& image : classified->memory_rejected) record_classified(image);

    for (const auto& item : classified->large_mode) {
      auto& result = results[item.file.index];
      pipeline_detail::record_memory_diagnostics(result, cfg, memory_status, configured_memory_limit,
          estimated_regular_memory(item.dimensions, cfg, item.file.bytes, true, nullptr, configured_memory_limit));
      result.processed = false;
      result.message = pipeline_detail::large_image_report_message(item);
    }

    for (const auto& image : classified->memory_rejected) {
      auto& result = results[image.file.index];
      result.processed = true;
      result.message = image.preflight_error.empty()
          ? awj::compat::format("估算工作集 {} 超过内存限制 {}，未启动编码。",
                      format_size(image.estimated_bytes),
                      format_size(configured_memory_limit))
          : image.preflight_error;
    }

    std::atomic<std::size_t> completed{0};
    int worker_failures = 0;

    if (!stop_token.stop_requested()) {
      for (const auto& image : classified->memory_rejected) {
        const auto done = completed.fetch_add(1) + 1;
        pipeline_detail::best_effort([&] {
          const auto& result = results[image.file.index];
          logger.warn(
              awj::compat::format("[FAIL] {:04} {}", result.index + 1, result.message));
          emit_progress(
              progress,
              BatchProgress{
                  .kind = BatchEventKind::item_finished,
                  .completed = done,
                  .total = files.size(),
                  .result = result,
                  .text = pipeline_detail::format_result_line(result)});
        });
      }
    }

    const auto execution = pipeline_detail::encode_work_groups(
        cfg, logger, resource_plan, *work, *recycle_plan, files.size(), completed,
        results, progress, stop_token);
    worker_failures += execution.worker_failures;

    const bool canceled = stop_token.stop_requested();
    pipeline_detail::mark_unstarted_results(results, canceled,
                                            worker_failures > 0);

    std::uintmax_t original_total = 0;
    std::unordered_map<PathText, std::uintmax_t> final_output_sizes;
    bool original_total_incomplete = false;
    bool output_total_incomplete = false;
    std::size_t ok_count = 0;
    std::size_t failed_count = 0;
    std::size_t canceled_count = 0;
    for (const auto& result : results) {
      if (result.ok) {
        ++ok_count;
        if (!original_total_incomplete &&
            !pipeline_detail::checked_add_uintmax(original_total,
                                                  result.original_bytes)) {
          original_total = 0;
          original_total_incomplete = true;
        }
        if (!output_total_incomplete) {
          try {
            final_output_sizes[normalized_lower_path_key(result.output_path)] =
                result.output_bytes;
          } catch (const std::bad_alloc&) {
            output_total_incomplete = true;
          } catch (const std::length_error&) {
            output_total_incomplete = true;
          }
        }
      } else if (result.canceled || (!result.processed && canceled)) {
        ++canceled_count;
      } else {
        ++failed_count;
        if (result.processed && !original_total_incomplete &&
            !pipeline_detail::checked_add_uintmax(original_total,
                                                  result.original_bytes)) {
          original_total = 0;
          original_total_incomplete = true;
        }
      }
    }
    std::uintmax_t output_total = 0;
    if (!output_total_incomplete) {
      for (const auto& [_, bytes] : final_output_sizes) {
        if (!pipeline_detail::checked_add_uintmax(output_total, bytes)) {
          output_total = 0;
          output_total_incomplete = true;
          break;
        }
      }
    }

    bool summary_failed = original_total_incomplete || output_total_incomplete;
    bool csv_write_failed = false;
    if (original_total_incomplete || output_total_incomplete) {
      try {
        logger.warn("体积汇总不可用，压缩率未统计。");
      } catch (...) {
      }
    }
    if (cfg.write_summary) {
      if (auto csv = write_csv(output_dir, results); !csv) {
        summary_failed = true;
        csv_write_failed = true;
        try {
          logger.warn(csv.error());
        } catch (...) {
        }
      }
    }
    const double total_ratio = original_total_incomplete ||
                                       output_total_incomplete ||
                                       original_total == 0
                                   ? 0.0
                                   : static_cast<double>(output_total) /
                                         static_cast<double>(original_total);

    const int worker_failure_count = worker_failures;
    const bool has_failures =
        failed_count > 0 || worker_failure_count > 0 || summary_failed;

    BatchSummary summary{
        .ok_count = pipeline_detail::count_to_int_saturated(ok_count),
        .failed_count = pipeline_detail::count_to_int_saturated(failed_count),
        .canceled_count =
            pipeline_detail::count_to_int_saturated(canceled_count),
        .large_image_queued_count = 0,
        .original_total = original_total,
        .output_total = output_total,
        .canceled = canceled,
        .exit_code = canceled ? 130 : (has_failures ? 2 : 0)};
    try {
      std::string summary_report;
      if (cfg.write_summary) {
        summary_report = "，报告 log/summery.csv";
        if (csv_write_failed) {
          summary_report += "，报告写入失败";
        }
      }
      const auto volume_incomplete =
          original_total_incomplete || output_total_incomplete;
      const auto volume_text =
          volume_incomplete
              ? awj::compat::format("体积 {} -> 未统计", format_size(original_total))
              : awj::compat::format("体积 {} -> {} ({:.1f}%)",
                            format_size(original_total),
                            format_size(output_total), total_ratio * 100.0);
      emit_progress(
          progress,
          BatchProgress{
              .kind = BatchEventKind::summary,
              .completed = completed.load(),
              .total = files.size(),
              .summary = summary,
              .text = awj::compat::format(
                  "{}：成功 {}，失败 {}，取消 {}，大图模式 {}{}；{}{}",
                  canceled ? "已取消" : "完成", ok_count, failed_count,
                  canceled_count, classified->large_mode.size(),
                  classified->large_mode.empty() ? std::string{}
                                                 : "（已自动处理）",
                  volume_text, summary_report)});
    } catch (...) {
    }
    return summary;
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"批处理运行内存不足，程序已安全退出。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"批处理运行数据超过运行时限制，程序已安全退出。"};
  } catch (const std::filesystem::filesystem_error&) {
    return awj::compat::unexpected{"批处理运行文件系统访问失败，程序已安全退出。"};
  } catch (const std::exception&) {
    return awj::compat::unexpected{"批处理运行时异常，程序已安全退出。"};
  } catch (...) {
    return awj::compat::unexpected{"未知异常，程序已安全退出。"};
  }
}

// 顶层流水线返回进程退出码；单张图片错误会落到 CSV，不会让程序闪退。
int run_pipeline(const AppConfig& cfg,
                 std::span<const std::filesystem::path> input_paths,
                 awj::compat::stop_token stop_token);

int run_pipeline(const AppConfig& cfg, awj::compat::stop_token stop_token = {}) {
  return run_pipeline(cfg, std::span<const std::filesystem::path>{}, stop_token);
}

int run_pipeline(const AppConfig& cfg,
                 std::span<const std::filesystem::path> input_paths,
                 awj::compat::stop_token stop_token) {
  std::mutex print_mutex;
  auto runtime_cfg = cfg;
  runtime_cfg.frame_progress = [&](std::size_t item, std::size_t completed, std::size_t total) {
    // Bound worker event volume for long animations while retaining first/last progress.
    if (completed != 1 && completed != total && completed % 16 != 0) return;
    std::scoped_lock lock{print_mutex};
    if ((!cfg.studio_queue_manifest.empty() || !cfg.studio_cancel_event_name.empty())) {
      print_line(awj::compat::format("@AWJ-STUDIO/1 FRAME {} {} {}", item, completed, total));
    } else {
      print_line(awj::compat::format("动画 {}: 帧 {}/{}", item + 1, completed, total));
    }
    if (cfg.frame_progress) cfg.frame_progress(item, completed, total);
  };
  const auto summary = run_batch(
      runtime_cfg,
      [&](const BatchProgress& event) {
        std::scoped_lock lock{print_mutex};
        if ((!cfg.studio_queue_manifest.empty() || !cfg.studio_cancel_event_name.empty()) &&
            (event.kind == BatchEventKind::item_started ||
             event.kind == BatchEventKind::item_finished)) {
          const char status =
              event.kind == BatchEventKind::item_started
                  ? 'R'
                  : (event.result.ok
                         ? (event.result.skipped ? 'S' : 'D')
                         : (event.result.canceled ? 'C' : 'F'));
          if (status == 'D') {
            constexpr std::string_view hex = "0123456789abcdef";
            const auto path = path_to_utf8(event.result.output_path);
            std::string encoded;
            encoded.reserve(path.size() * 2);
            for (const unsigned char byte : path) {
              encoded.push_back(hex[byte >> 4]);
              encoded.push_back(hex[byte & 15]);
            }
            print_line(awj::compat::format("@AWJ-STUDIO/1 OUTPUT {} {}", event.result.index, encoded));
          }
          print_line(awj::compat::format("@AWJ-STUDIO/1 ITEM {} {} {} {}",
                                 event.result.index, status, event.completed,
                                 event.total));
          if (event.kind == BatchEventKind::item_finished) {
            const auto microseconds = [](double seconds) -> std::int64_t {
              if (!std::isfinite(seconds) || seconds < 0.0) return -1;
              const auto rounded = std::round(seconds * 1'000'000.0);
              if (rounded >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                return std::numeric_limits<std::int64_t>::max();
              return static_cast<std::int64_t>(rounded);
            };
            print_line(awj::compat::format(
                "@AWJ-STUDIO/1 DETAIL {} {} {} {} {} {} {}",
                event.result.index,
                event.result.encoder_id.empty() ? "-" : event.result.encoder_id,
                event.result.encoder_threads,
                microseconds(event.result.decode_seconds),
                microseconds(event.result.prepare_seconds),
                microseconds(event.result.encode_seconds),
                microseconds(event.result.write_seconds)));
            const auto resize_microseconds = microseconds(event.result.resize_seconds);
            print_line(awj::compat::format(
                "@AWJ-STUDIO/1 RESIZE {} {} {}", event.result.index,
                resize_microseconds,
                resize_microseconds < 0 ? 0 : std::max(0, event.result.resize_workers)));
          }
        }
        if (event.kind == BatchEventKind::summary) {
          print_line("");
        }
        if (cfg.studio_queue_manifest.empty() && cfg.studio_cancel_event_name.empty() &&
            event.kind == BatchEventKind::item_finished && event.result.ok &&
            !event.result.skipped && !event.result.message.empty() &&
            event.result.message != "OK") {
          awj::compat::println(stderr, "[WARN] {}", event.result.message);
          std::fflush(stderr);
        }
        if (!event.text.empty()) {
          print_line(event.text);
        }
      },
      stop_token,
      input_paths);
  if (!summary) {
    print_line(awj::compat::format("[FAIL] {}", summary.error()));
    return 1;
  }
  return summary->exit_code;
}

}  // namespace awj
