module;
#include "../core/work_memory_admission.h"
#include "../core/hdr_cost.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <objbase.h>
#include <windows.h>
#else
#include <sys/sysinfo.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
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

export namespace awj {

namespace pipeline_detail {

struct WorkGroup {
  std::uintmax_t weight{};
  // 组内串行处理，峰值内存取成员估算的最大值，供运行时内存准入使用。
  std::uint64_t estimated_bytes{1};
  std::vector<ImageFile> files{};
};

struct LargeWorkGroup {
  std::uintmax_t weight{};
  std::uint64_t estimated_bytes{1};
  std::vector<BatchLargeImageItem> items{};
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
  return std::format("超大图自动处理：{}x{}，原因 {}；可用处理方式：{}；{}",
                     item.dimensions.width, item.dimensions.height,
                     large_image_reason_name(item.decision.reason),
                     large_image_actions_text(item.decision),
                     item.decision.reason_text);
}

std::string large_image_action_text(const BatchLargeImageItem& item) {
  const auto actions = large_image_actions_text(item.decision);
  return std::format(
      "[LARGE] {:04} {} ({}x{}, {}) -> 自动大图：{}；{}", item.file.index + 1,
      path_to_utf8(item.file.path.filename()), item.dimensions.width,
      item.dimensions.height, large_image_reason_name(item.decision.reason),
      actions, item.decision.reason_text);
}

std::string large_image_log_text(const BatchLargeImageItem& item) {
  const auto actions = large_image_actions_text(item.decision);
  return std::format("[LARGE] {:04} ({}x{}, {}) -> 自动大图：{}；{}",
                     item.file.index + 1, item.dimensions.width,
                     item.dimensions.height,
                     large_image_reason_name(item.decision.reason), actions,
                     item.decision.reason_text);
}

bool avif_capability_available_for_large_mode(
    const AvifEncoderCapability& capability) noexcept {
  return capability.enabled;
}

std::uint64_t estimated_regular_working_set_bytes(
    ImageDimensions dimensions, const AppConfig& cfg,
    std::uint64_t compressed_bytes = 0) noexcept {
  if (cfg.output_format == OutputFormat::avif) {
    const bool opaque_sdr_420 = dimensions.opaque_sdr8_yuv420 &&
        cfg.quality < 100 && !cfg.visual_quality && cfg.alpha_policy != AlphaModePolicy::force &&
        !cfg.color_primaries && !cfg.transfer_characteristics && !cfg.matrix_coefficients && !cfg.color_range &&
        cfg.avif_color_representation != AvifColorRepresentation::rgb_identity &&
        cfg.bit_depth.value_or(10) <= 10 &&
        (cfg.chroma_mode == ChromaMode::auto_keep || cfg.chroma_mode == ChromaMode::yuv420);
    const auto encoding = avif_encode_working_set_bytes_for_dimensions(dimensions, compressed_bytes, opaque_sdr_420);
    if (!cfg.visual_quality) return encoding;
    const auto metrics = visual_quality_working_set_bytes_for_dimensions(dimensions);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return encoding > maximum - metrics ? maximum : encoding + metrics;
  }
  if (cfg.output_format == OutputFormat::jpgli) {
    // Probe dimensions do not yet identify HDR reliably (forced SDR can still
    // need HDR tone buffers). Reserve the maximum simultaneously-live HDR JPEG
    // phase with the encoder helper; admission still admits files dynamically.
    const auto hdr = hdr_cost::jpeg_working_set_bytes(dimensions.width, dimensions.height,
        compressed_bytes, static_cast<std::uint64_t>(std::max(1, cfg.max_jobs)));
    return cfg.visual_quality
        ? std::max(hdr, visual_quality_working_set_bytes_for_dimensions(dimensions)) : hdr;
  }
  if (cfg.visual_quality) {
    return visual_quality_working_set_bytes_for_dimensions(dimensions);
  }
  if (cfg.output_format == OutputFormat::png) {
    // Includes 16-bit decoded samples and the growing encoded byte vector.
    // ponytail: conservative dimension estimate; refine from measurements.
    constexpr auto overhead = 32ull * 1024 * 1024;
    const auto rgba = decoded_rgba_bytes_for_dimensions(dimensions);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return rgba > (maximum - overhead) / 8 ? maximum : rgba * 8 + overhead;
  }
  return decoded_rgba_bytes_for_dimensions(dimensions);
}

std::expected<ClassifiedWork, std::string> classify_work_for_avif(
    const AppConfig& cfg, const std::vector<ImageFile>& files,
    std::uint64_t memory_limit) {
  try {
    WicFallbackComApartment com{cfg.allow_wic_fallback};
    const bool allow_wic_fallback = cfg.allow_wic_fallback && com.usable();

    ClassifiedWork classified{};
    if (cfg.output_format != OutputFormat::avif) {
      classified.ordinary.reserve(files.size());
      DecoderRegistryOptions decoder_options{.allow_wic_fallback =
                                                 allow_wic_fallback};
      for (const auto& image : files) {
        // Unknown decoded dimensions must not admit several compressed-small
        // images together. Reserve the whole budget until decode completes.
        auto estimated_bytes = std::max<std::uint64_t>(memory_limit,
            std::max<std::uintmax_t>(1, image.bytes));
        if (auto dimensions =
                probe_image_dimensions_for_path(image.path, decoder_options)) {
          estimated_bytes =
              estimated_regular_working_set_bytes(*dimensions, cfg, image.bytes);
        }
        classified.ordinary.push_back(ClassifiedImageFile{
            .file = image, .estimated_bytes = estimated_bytes});
      }
      return classified;
    }

    const auto capabilities = avif_encoder_capabilities_for_current_build();
    const bool grid_available =
        std::ranges::any_of(capabilities, [](const auto& capability) {
          return capability.mode == AvifEncoderMode::aom &&
                 avif_capability_available_for_large_mode(capability) &&
                 capability.supports_avif_grid;
        });
    DecoderRegistryOptions decoder_options{.allow_wic_fallback =
                                               allow_wic_fallback};
    for (const auto& image : files) {
      auto dimensions =
          probe_image_dimensions_for_path(image.path, decoder_options);
      if (!dimensions) {
        classified.ordinary.push_back(ClassifiedImageFile{
            .file = image,
            .estimated_bytes = std::max<std::uint64_t>(memory_limit,
                std::max<std::uintmax_t>(1, image.bytes))});
        continue;
      }
      auto decision = classify_large_image(*dimensions, grid_available);
      if (decision.klass == LargeImageClass::large_mode_required && grid_available) {
        auto animated = input_uses_animation_reader(image.path);
        if (!animated) return std::unexpected{animated.error()};
        if (*animated) decision = classify_large_image(*dimensions, false);
      }
      switch (decision.klass) {
        case LargeImageClass::ordinary:
          classified.ordinary.push_back(ClassifiedImageFile{
              .file = image,
              .estimated_bytes =
                  estimated_regular_working_set_bytes(*dimensions, cfg, image.bytes)});
          break;
        case LargeImageClass::large_mode_required:
          classified.large_mode.push_back(
              BatchLargeImageItem{.file = image,
                                  .dimensions = *dimensions,
                                  .decision = std::move(decision)});
          break;
      }
    }
    return classified;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"批处理分类列表内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"批处理分类列表数量超过运行时限制。"};
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

std::uint64_t estimated_large_working_set_bytes(const BatchLargeImageItem& item) noexcept;

std::expected<void, std::string> reject_over_memory_budget(
    ClassifiedWork& classified, std::uint64_t memory_limit) {
  if (memory_limit == 0) {
    return {};
  }

  auto reject_from = [&](std::vector<ClassifiedImageFile>& source)
      -> std::expected<void, std::string> {
    try {
      if (classified.memory_rejected.size() >
          std::numeric_limits<std::size_t>::max() - source.size()) {
        return std::unexpected{"内存预算预检列表数量超过运行时限制。"};
      }
      classified.memory_rejected.reserve(classified.memory_rejected.size() +
                                         source.size());
      std::vector<ClassifiedImageFile> retained;
      retained.reserve(source.size());
      for (auto& image : source) {
        if (image.estimated_bytes > memory_limit) {
          classified.memory_rejected.push_back(std::move(image));
        } else {
          retained.push_back(std::move(image));
        }
      }
      source = std::move(retained);
    } catch (const std::bad_alloc&) {
      return std::unexpected{"内存预算预检列表内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"内存预算预检列表数量超过运行时限制。"};
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
      const auto estimate = estimated_large_working_set_bytes(image);
      if (estimate > memory_limit)
        classified.memory_rejected.push_back({.file = std::move(image.file),
                                               .estimated_bytes = estimate});
      else retained.push_back(std::move(image));
    }
    classified.large_mode = std::move(retained);
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"大图内存预算预检列表内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"大图内存预算预检列表数量超过运行时限制。"};
  }
}

std::expected<std::vector<WorkGroup>, std::string> build_work_groups(
    const AppConfig& cfg, const std::vector<ClassifiedImageFile>& classified) {
  std::vector<WorkGroup> groups;
  std::unordered_map<PathText, std::size_t> index_by_output;

  try {
    // 同一输出路径的文件必须串行处理，避免并发覆盖；不同输出路径按总大小分组调度。
    groups.reserve(classified.size());
    index_by_output.reserve(classified.size());
    for (const auto& entry : classified) {
      const auto& image = entry.file;
      const auto output = output_path_for(cfg, image);
      auto key = normalized_lower_path_key(output);
      const auto [it, inserted] = index_by_output.emplace(key, groups.size());
      if (inserted) {
        groups.push_back(WorkGroup{});
      }

      auto& group = groups[it->second];
      group.weight = saturated_add_uintmax(group.weight, image.bytes);
      // 组内串行，峰值取组内最大估算；供运行时内存准入使用。
      group.estimated_bytes =
          std::max(group.estimated_bytes, entry.estimated_bytes);
      group.files.push_back(image);
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected{"批处理工作组内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"批处理工作组数量超过运行时限制。"};
  }

  // 不同输出路径之间按总大小调度；覆盖/跳过模式下同一路径保留扫描顺序。
  std::ranges::sort(groups, [](const WorkGroup& left, const WorkGroup& right) {
    return left.weight > right.weight;
  });
  return groups;
}

std::uint64_t estimated_large_working_set_bytes(
    const BatchLargeImageItem& item) noexcept {
  return avif_encode_working_set_bytes_for_dimensions(item.dimensions, item.file.bytes);
}

std::uint64_t largest_large_mode_working_set(
    const std::vector<BatchLargeImageItem>& items) noexcept {
  std::uint64_t estimate = 1;
  for (const auto& item : items) {
    estimate = std::max(estimate, estimated_large_working_set_bytes(item));
  }
  return estimate;
}

std::expected<std::vector<LargeWorkGroup>, std::string> build_large_work_groups(
    const AppConfig& cfg, const std::vector<BatchLargeImageItem>& items) {
  std::vector<LargeWorkGroup> groups;
  std::unordered_map<PathText, std::size_t> index_by_output;

  try {
    groups.reserve(items.size());
    index_by_output.reserve(items.size());
    for (const auto& item : items) {
      const auto output = output_path_for(cfg, item.file);
      auto key = normalized_lower_path_key(output);
      const auto [it, inserted] = index_by_output.emplace(key, groups.size());
      if (inserted) {
        groups.push_back(LargeWorkGroup{});
      }

      auto& group = groups[it->second];
      group.weight = saturated_add_uintmax(group.weight, item.file.bytes);
      group.estimated_bytes =
          std::max(group.estimated_bytes, estimated_large_working_set_bytes(item));
      group.items.push_back(item);
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected{"大图工作组内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"大图工作组数量超过运行时限制。"};
  }

  std::ranges::sort(groups, [](const LargeWorkGroup& left,
                               const LargeWorkGroup& right) {
    return left.weight > right.weight;
  });
  return groups;
}

std::string format_result_line(const EncodeResult& result) {
  const auto warning_suffix = [&] {
    return result.ok && !result.skipped && !result.message.empty() &&
                   result.message != "OK"
               ? std::format(" [WARN] {}", result.message)
               : std::string{};
  };
  if (result.ok) {
    if (result.skipped) {
      return std::format("[SKIP] {:04} {} -> 已存在", result.index + 1,
                         path_to_utf8(result.input_path.filename()));
    }

    const double ratio = result.original_bytes == 0
                             ? 0.0
                             : static_cast<double>(result.output_bytes) /
                                   static_cast<double>(result.original_bytes);
    if (result.requested_visual_quality) {
      if (result.lossless) {
        return std::format(
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
      return std::format(
          "[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s, VQ {}→{:.2f}{}, q{}, "
          "{} 次){}",
          result.index + 1, path_to_utf8(result.input_path.filename()),
          path_to_utf8(result.output_path.filename()),
          format_size(result.output_bytes), ratio * 100.0, result.seconds,
          *result.requested_visual_quality, result.visual_score, target_state,
          result.final_encoder_quality, result.search_attempt_count,
          warning_suffix());
    }
    return std::format(
        "[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s){}", result.index + 1,
        path_to_utf8(result.input_path.filename()),
        path_to_utf8(result.output_path.filename()),
        format_size(result.output_bytes), ratio * 100.0, result.seconds,
        warning_suffix());
  }

  if (result.canceled) {
    return std::format("[CANCEL] {:04} {} -> {}", result.index + 1,
                       path_to_utf8(result.input_path.filename()),
                       result.message);
  }

  return std::format("[FAIL] {:04} {} -> {}", result.index + 1,
                     path_to_utf8(result.input_path.filename()),
                     result.message);
}

}  // namespace pipeline_detail

void emit_progress(const ProgressCallback& progress,
                   const BatchProgress& event);

namespace pipeline_detail {

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
  MemoryAdmissionGuard(const MemoryAdmissionGuard&) = delete;
  MemoryAdmissionGuard& operator=(const MemoryAdmissionGuard&) = delete;
};

WorkExecutionResult encode_work_groups(
    const AppConfig& cfg, FileLogger& logger, const ResourcePlan& resource_plan,
    const std::vector<WorkGroup>& work, std::size_t progress_total,
    std::atomic<std::size_t>& completed, std::vector<EncodeResult>& results,
    const ProgressCallback& progress, std::stop_token stop_token) {
  WorkExecutionResult execution{};
  if (work.empty()) {
    return execution;
  }

  const int jobs =
      std::max(1, std::min({resource_plan.file_parallelism,
                            resource_plan.memory_file_parallelism,
                            count_to_int_saturated(work.size())}));
  std::atomic<int> worker_failures{0};
  WorkMemoryAdmission admission{resource_plan.memory_limit_bytes, work};

  std::vector<std::jthread> workers;
  try {
    workers.reserve(static_cast<std::size_t>(jobs));
  } catch (const std::bad_alloc&) {
    worker_failures.fetch_add(1);
    try {
      const auto text =
          std::format("[WARN] 工作线程列表内存不足，需要 {} 个线程槽。", jobs);
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
      workers.emplace_back([&] {
        WicFallbackComApartment com{cfg.allow_wic_fallback};
        try {
          std::optional<AppConfig> worker_cfg;
          if (cfg.allow_wic_fallback && !com.usable()) {
            worker_cfg.emplace(cfg);
            worker_cfg->allow_wic_fallback = false;
            try {
              const auto text = std::format(
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
          NativeBackend native_backend{effective_cfg, logger, resource_plan};
          while (true) {
            if (stop_token.stop_requested()) {
              break;
            }

            const auto ticket = admission.acquire(stop_token);
            if (!ticket) break;
            const auto& group = work[ticket->index];
            MemoryAdmissionGuard reservation{admission, ticket->bytes};
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
                                  .text = std::format(
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
              auto result = native_backend.encode(image, stop_token);
              const auto result_index = result.index;
              results[result_index] = std::move(result);
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
        const auto text = std::format(
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
  workers.clear();
  execution.worker_failures = worker_failures.load();
  return execution;
}

EncodeResult encode_large_mode_item(const AppConfig& cfg, FileLogger& logger,
                                    const BatchLargeImageItem& item,
                                    ResourcePlan resource_plan,
                                    std::stop_token stop_token) {
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
  if (stop_token.stop_requested()) {
    failed.canceled = true;
    failed.message = "任务已取消。";
    return failed;
  }

  AppConfig large_cfg = cfg;
  const auto required_memory = estimated_large_working_set_bytes(item);
  if (resource_plan.memory_limit_bytes > 0 && required_memory > resource_plan.memory_limit_bytes) {
    failed.message = std::format("AOM Grid 预估工作集 {} 超过内存预算 {}。",
        format_size(required_memory), format_size(resource_plan.memory_limit_bytes));
    return failed;
  }
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
    auto planned = plan_grid(GridPlanRequest{
        .width = item.dimensions.width,
        .height = item.dimensions.height,
        .mode = GridMode::auto_grid,
        .clamped_padding_enabled = large_cfg.experimental_clamped_grid_padding});
    if (!planned) {
      failed.message = std::format("Grid 规划失败：{}", planned.error());
      return failed;
    }
    large_cfg.avif_encoder = AvifEncoderMode::aom;
    NativeBackend backend{large_cfg, logger, resource_plan};
    return backend.encode_avif_grid(item.file, *planned, stop_token);
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

WorkExecutionResult encode_large_work_groups(
    const AppConfig& cfg, FileLogger& logger, const ResourcePlan& resource_plan,
    const std::vector<LargeWorkGroup>& work, std::size_t progress_total,
    std::atomic<std::size_t>& completed, std::vector<EncodeResult>& results,
    const ProgressCallback& progress, std::stop_token stop_token) {
  WorkExecutionResult execution{};
  if (work.empty()) {
    return execution;
  }

  const int jobs =
      std::max(1, std::min({resource_plan.file_parallelism,
                            resource_plan.memory_file_parallelism,
                            count_to_int_saturated(work.size())}));
  std::atomic<int> worker_failures{0};
  WorkMemoryAdmission admission{resource_plan.memory_limit_bytes, work};

  std::vector<std::jthread> workers;
  try {
    workers.reserve(static_cast<std::size_t>(jobs));
  } catch (const std::bad_alloc&) {
    worker_failures.fetch_add(1);
    try {
      const auto text =
          std::format("[WARN] 大图工作线程列表内存不足，需要 {} 个线程槽。", jobs);
      report_worker_warning_noexcept(logger, progress, completed.load(),
                                     progress_total, text, text);
    } catch (...) {
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "large worker list allocation failed",
          "[WARN] 大图工作线程列表内存不足。");
    }
    execution.worker_failures = worker_failures.load();
    return execution;
  } catch (const std::length_error&) {
    worker_failures.fetch_add(1);
    constexpr std::string_view text = "[WARN] 大图工作线程列表数量超过运行时限制。";
    report_worker_warning_noexcept(logger, progress, completed.load(),
                                   progress_total, text, text);
    execution.worker_failures = worker_failures.load();
    return execution;
  }

  for (const int worker_index : std::views::iota(0, jobs)) {
    try {
      workers.emplace_back([&] {
        WicFallbackComApartment com{cfg.allow_wic_fallback};
        try {
          std::optional<AppConfig> worker_cfg;
          if (cfg.allow_wic_fallback && !com.usable()) {
            worker_cfg.emplace(cfg);
            worker_cfg->allow_wic_fallback = false;
            try {
              const auto text = std::format(
                  "[WARN] 大图工作线程 WIC fallback COM 初始化失败，已禁用该线程的 "
                  "WIC fallback: 0x{:08X}",
                  static_cast<unsigned int>(com.init()));
              report_worker_warning_noexcept(logger, progress, completed.load(),
                                             progress_total, text, text);
            } catch (...) {
              report_worker_warning_noexcept(
                  logger, progress, completed.load(), progress_total,
                  "large worker WIC fallback COM initialization failed",
                  "[WARN] 大图工作线程 WIC fallback COM 初始化失败，已禁用该线程的 "
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
            if (stop_token.stop_requested()) {
              break;
            }
            if (group.items.size() > 1 &&
                cfg.collision_mode == CollisionMode::overwrite) {
              best_effort([&] {
                emit_progress(
                    progress,
                    BatchProgress{.kind = BatchEventKind::warning,
                                  .completed = completed.load(),
                                  .total = progress_total,
                                  .text = std::format(
                                      "[WARN] 大图输出重名: {} 个输入将依次覆盖 {}",
                                      group.items.size(),
                                      display_path_for_user(output_path_for(
                                          cfg, group.items.back().file)))});
              });
            }

            for (const auto& item : group.items) {
              if (stop_token.stop_requested()) {
                break;
              }
              emit_item_started_noexcept(cfg, item.file, completed.load(),
                                         progress_total, progress);
              best_effort([&] {
                logger.info(large_image_log_text(item));
                emit_progress(
                    progress,
                    BatchProgress{.kind = BatchEventKind::message,
                                  .completed = completed.load(),
                                  .total = progress_total,
                                  .large_image = item,
                                  .text = large_image_action_text(item)});
              });
              auto result = encode_large_mode_item(
                  effective_cfg, logger, item, resource_plan, stop_token);
              const auto result_index = result.index;
              results[result_index] = std::move(result);
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
                    "large item progress reporting failed",
                    "[WARN] 大图单项进度报告生成失败，结果已记录。");
              }
            }
          }
        } catch (const std::exception&) {
          worker_failures.fetch_add(1);
          report_worker_warning_noexcept(
              logger, progress, completed.load(), progress_total,
              "large worker failed",
              "[WARN] 大图工作线程异常，已停止该线程。");
        } catch (...) {
          worker_failures.fetch_add(1);
          report_worker_warning_noexcept(
              logger, progress, completed.load(), progress_total,
              "large worker failed: unknown exception",
              "[WARN] 大图工作线程异常，已停止该线程: 未知异常");
        }
      });
    } catch (const std::bad_alloc&) {
      worker_failures.fetch_add(1);
      try {
        const auto text = std::format(
            "[WARN] 创建大图工作线程失败，线程状态内存不足，已继续等待已启动线程: "
            "{}/{}",
            worker_index, jobs);
        report_worker_warning_noexcept(logger, progress, completed.load(),
                                       progress_total, text, text);
      } catch (...) {
        report_worker_warning_noexcept(
            logger, progress, completed.load(), progress_total,
            "large worker thread creation allocation failed",
            "[WARN] 创建大图工作线程失败，线程状态内存不足，已继续等待已启动线程。");
      }
      break;
    } catch (const std::system_error&) {
      worker_failures.fetch_add(1);
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "large worker thread creation failed",
          "[WARN] 创建大图工作线程失败，已继续等待已启动线程。");
      break;
    } catch (const std::exception&) {
      worker_failures.fetch_add(1);
      report_worker_warning_noexcept(
          logger, progress, completed.load(), progress_total,
          "large worker thread creation failed",
          "[WARN] 创建大图工作线程失败，已继续等待已启动线程。");
      break;
    }
  }

  workers.clear();
  execution.worker_failures = worker_failures.load();
  return execution;
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
  std::println("{}", text);
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

MemoryStatus current_memory_status() noexcept {
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) {
    return {};
  }
  return MemoryStatus{.total_bytes = status.ullTotalPhys,
                      .available_bytes = status.ullAvailPhys};
#else
  struct sysinfo status {};
  if (sysinfo(&status) != 0) {
    return {};
  }
  const auto unit = static_cast<std::uint64_t>(status.mem_unit == 0 ? 1 : status.mem_unit);
  return MemoryStatus{.total_bytes = static_cast<std::uint64_t>(status.totalram) * unit,
                      .available_bytes = static_cast<std::uint64_t>(status.freeram + status.bufferram) * unit};
#endif
}

void apply_runtime_input_limit_policy(const AppConfig& cfg) noexcept {
  encoding_defaults::unlock_max_input_file_bytes.store(
      cfg.unlock_max_input_file_bytes, std::memory_order_relaxed);
}

std::expected<BatchSummary, std::string> run_batch(
    AppConfig cfg, ProgressCallback progress = {},
    std::stop_token stop_token = {},
    std::span<const std::filesystem::path> input_paths = {}) {
  try {
    apply_runtime_input_limit_policy(cfg);
#ifndef _WIN32
    cfg.allow_wic_fallback = false;
#endif
    if (!cfg.studio_queue_manifest.empty() &&
        !cfg.studio_large_action.empty()) {
      return std::unexpected{
          "Studio 队列 manifest 不能与手动大图 worker 同时使用。"};
    }
    if (!cfg.studio_large_action.empty()) {
      if (auto valid = validate_config(cfg); !valid) {
        return std::unexpected{valid.error()};
      }
      if (cfg.output_format != OutputFormat::avif) {
        return std::unexpected{"Studio 大图 worker 仅支持 AVIF 输出。"};
      }
      auto dimensions = probe_image_dimensions_for_path(
          cfg.input_path,
          DecoderRegistryOptions{.allow_wic_fallback = cfg.allow_wic_fallback});
      if (!dimensions) {
        return std::unexpected{dimensions.error()};
      }
      std::error_code file_ec;
      const auto bytes = std::filesystem::file_size(cfg.input_path, file_ec);
      if (file_ec) {
        return std::unexpected{std::format("读取文件大小失败: {}；系统错误：{}。",
                                           display_path_for_user(cfg.input_path),
                                           file_ec.message())};
      }
      const bool grid_action = cfg.studio_large_action == L"grid";
      if (grid_action) {
        auto animated = input_uses_animation_reader(cfg.input_path);
        if (!animated) return std::unexpected{animated.error()};
        if (*animated) return std::unexpected{"保留动画时不能使用静态 Grid。"};
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
      const auto output_dir = output_dir_for(cfg);
      FileLogger logger{output_dir, cfg.write_log};
      const auto configured_memory_limit =
          cfg.memory_limit_bytes == 0 ? automatic_memory_limit(current_memory_status())
                                      : cfg.memory_limit_bytes;
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::message,
                                    .completed = 0,
                                    .total = 1,
                                    .large_image = item,
                                    .text = pipeline_detail::large_image_action_text(item)});
      });
      const auto large_resource_plan = plan_large_mode_resources(
          plan_resources(ResourcePlanRequest{
              .automatic_thread_budget = cfg.max_jobs,
              .file_count = 1,
              .memory_limit_bytes = configured_memory_limit,
              .estimated_bytes_per_file =
                  pipeline_detail::estimated_large_working_set_bytes(item)}),
          1,
          pipeline_detail::estimated_large_working_set_bytes(item));
      pipeline_detail::emit_item_started_noexcept(
          cfg, item.file, 0, 1, progress);
      auto result = pipeline_detail::encode_large_mode_item(
          cfg, logger, item, large_resource_plan, stop_token);
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
          return std::unexpected{csv.error()};
        }
      }
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::summary,
                                    .completed = 1,
                                    .total = 1,
                                    .summary = summary,
                                    .text = std::format("{}：成功 {}，失败 {}，取消 {}。",
                                                        canceled ? "已取消" : "完成",
                                                        summary.ok_count,
                                                        summary.failed_count,
                                                        summary.canceled_count)});
      });
      return summary;
    }
    if (auto valid = validate_config(cfg); !valid) {
      return std::unexpected{valid.error()};
    }

    const auto output_dir = output_dir_for(cfg);

    std::vector<ImageFile> files;
    if (!cfg.studio_queue_manifest.empty()) {
      auto manifest = read_studio_queue_manifest(cfg.studio_queue_manifest);
      if (!manifest) {
        return std::unexpected{manifest.error()};
      }
      if (auto valid = pipeline_detail::validate_manifest_paths_for_config(
              cfg, manifest->files);
          !valid) {
        return std::unexpected{valid.error()};
      }
      files = std::move(manifest->files);
    } else {
      if (auto scanned = input_paths.empty() ? scan_images(cfg, files)
                                             : scan_images(cfg, input_paths, files);
          !scanned) {
        return std::unexpected{scanned.error()};
      }
    }

    FileLogger logger{output_dir, cfg.write_log};
    logger.info("native backend enabled");
    if (cfg.write_log && !logger.enabled()) {
      pipeline_detail::best_effort([&] {
        const auto text =
            std::format("[WARN] 日志写入失败: {}", logger.last_error());
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
        std::ranges::count_if(files, &ImageFile::extension_disambiguated);
    if (disambiguated_count > 0) {
      pipeline_detail::best_effort([&] {
        const auto text = std::format(
            "[WARN] 同名不同扩展: {} 个输入会自动保留源扩展名，"
            "例如 1.jpg.avif / 1.bmp.avif",
            disambiguated_count);
        logger.warn(text);
        emit_progress(progress, BatchProgress{.kind = BatchEventKind::warning,
                                              .total = files.size(),
                                              .text = text});
      });
    }

    const auto configured_memory_limit =
        cfg.memory_limit_bytes == 0
            ? automatic_memory_limit(current_memory_status())
            : cfg.memory_limit_bytes;
    auto classified = pipeline_detail::classify_work_for_avif(cfg, files, configured_memory_limit);
    if (!classified) {
      return std::unexpected{classified.error()};
    }
    if (auto memory_filter = pipeline_detail::reject_over_memory_budget(
            *classified, configured_memory_limit);
        !memory_filter) {
      return std::unexpected{memory_filter.error()};
    }
    auto ordinary_work =
        pipeline_detail::build_work_groups(cfg, classified->ordinary);
    if (!ordinary_work) {
      return std::unexpected{ordinary_work.error()};
    }
    auto large_work =
        pipeline_detail::build_large_work_groups(cfg, classified->large_mode);
    if (!large_work) {
      return std::unexpected{large_work.error()};
    }
    // 并发度按「典型」单图占用规划（中位数），而不是队列最大值：最大值会让
    // 一张大图把整批压成串行。真正吃内存的组由 encode_work_groups 的运行时
    // 内存准入单独限制，不会同时进入。
    const auto ordinary_estimated_bytes_per_file =
        pipeline_detail::typical_decoded_bytes_per_file(classified->ordinary);
    const auto resource_plan = plan_resources(ResourcePlanRequest{
        .automatic_thread_budget = cfg.max_jobs,
        .file_count = pipeline_detail::count_to_int_saturated(
            std::max<std::size_t>(1, ordinary_work->size())),
        .memory_limit_bytes = configured_memory_limit,
        .estimated_bytes_per_file = ordinary_estimated_bytes_per_file});
    const auto large_largest_working_set =
        pipeline_detail::largest_large_mode_working_set(classified->large_mode);
    const auto large_base_resource_plan = plan_resources(ResourcePlanRequest{
        .automatic_thread_budget = cfg.max_jobs,
        .file_count = pipeline_detail::count_to_int_saturated(
            std::max<std::size_t>(1, large_work->size())),
        .memory_limit_bytes = configured_memory_limit,
        .estimated_bytes_per_file = large_largest_working_set});
    const auto large_resource_plan = plan_large_mode_resources(
        large_base_resource_plan,
        pipeline_detail::count_to_int_saturated(std::max<std::size_t>(
            1, large_work->size())),
        large_largest_working_set);
    const int ordinary_jobs =
        ordinary_work->empty()
            ? 0
            : std::max(1, std::min({resource_plan.file_parallelism,
                                    resource_plan.memory_file_parallelism,
                                    pipeline_detail::count_to_int_saturated(
                                        ordinary_work->size())}));
    const int large_jobs =
        large_work->empty()
            ? 0
            : std::max(1, std::min({large_resource_plan.file_parallelism,
                                    large_resource_plan.memory_file_parallelism,
                                    pipeline_detail::count_to_int_saturated(
                                        large_work->size())}));
    pipeline_detail::best_effort([&] {
      emit_progress(
          progress,
          BatchProgress{
              .kind = BatchEventKind::message,
              .total = files.size(),
              .text = std::format(
                  "共 {} 个文件；普通 {}，内存超限 {}，Grid "
                  "{}。普通并发 {}，Grid 并发 {}，编码器线程/文件 "
                  "{}/{}，内存限制 {}。",
                  files.size(), classified->ordinary.size(),
                  classified->memory_rejected.size(),
                  classified->large_mode.size(), ordinary_jobs,
                  large_jobs,
                  resource_plan.encoder_threads_per_file,
                  large_resource_plan.encoder_threads_per_file,
                  resource_plan.memory_limit_bytes == 0
                      ? std::string{"未限制"}
                      : format_size(resource_plan.memory_limit_bytes))});
    });

    std::vector<EncodeResult> results;
    try {
      results.resize(files.size());
    } catch (const std::bad_alloc&) {
      return std::unexpected{"批处理结果列表内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"批处理结果列表数量超过运行时限制。"};
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
    }

    for (const auto& item : classified->large_mode) {
      auto& result = results[item.file.index];
      result.processed = false;
      result.message = pipeline_detail::large_image_report_message(item);
    }

    for (const auto& image : classified->memory_rejected) {
      auto& result = results[image.file.index];
      result.processed = true;
      result.message =
          std::format("估算工作集 {} 超过内存限制 {}，未启动编码。",
                      format_size(image.estimated_bytes),
                      format_size(configured_memory_limit));
    }

    std::atomic<std::size_t> completed{0};
    int worker_failures = 0;

    if (!stop_token.stop_requested()) {
      for (const auto& image : classified->memory_rejected) {
        const auto done = completed.fetch_add(1) + 1;
        pipeline_detail::best_effort([&] {
          const auto& result = results[image.file.index];
          logger.warn(
              std::format("[FAIL] {:04} {}", result.index + 1, result.message));
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

    const auto ordinary_execution = pipeline_detail::encode_work_groups(
        cfg, logger, resource_plan, *ordinary_work, files.size(), completed,
        results, progress, stop_token);
    worker_failures += ordinary_execution.worker_failures;

    if (!stop_token.stop_requested() && !classified->large_mode.empty()) {
      pipeline_detail::best_effort([&] {
        emit_progress(progress,
                      BatchProgress{.kind = BatchEventKind::message,
                                    .completed = completed.load(),
                                    .total = files.size(),
                                    .text = std::format(
                                        "开始自动处理超大图：{} 个文件，并发 "
                                        "{}，编码器线程/文件 {}，使用 AOM Grid。",
                                        classified->large_mode.size(),
                                        large_jobs,
                                        large_resource_plan
                                            .encoder_threads_per_file)});
      });
      const auto large_execution = pipeline_detail::encode_large_work_groups(
          cfg, logger, large_resource_plan, *large_work, files.size(),
          completed, results, progress, stop_token);
      worker_failures += large_execution.worker_failures;
    }

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
        summary_report = "，报告 summary.csv";
        if (csv_write_failed) {
          summary_report += "，报告写入失败";
        }
      }
      const auto volume_incomplete =
          original_total_incomplete || output_total_incomplete;
      const auto volume_text =
          volume_incomplete
              ? std::format("体积 {} -> 未统计", format_size(original_total))
              : std::format("体积 {} -> {} ({:.1f}%)",
                            format_size(original_total),
                            format_size(output_total), total_ratio * 100.0);
      emit_progress(
          progress,
          BatchProgress{
              .kind = BatchEventKind::summary,
              .completed = completed.load(),
              .total = files.size(),
              .summary = summary,
              .text = std::format(
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
    return std::unexpected{"批处理运行内存不足，程序已安全退出。"};
  } catch (const std::length_error&) {
    return std::unexpected{"批处理运行数据超过运行时限制，程序已安全退出。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"批处理运行文件系统访问失败，程序已安全退出。"};
  } catch (const std::exception&) {
    return std::unexpected{"批处理运行时异常，程序已安全退出。"};
  } catch (...) {
    return std::unexpected{"未知异常，程序已安全退出。"};
  }
}

// 顶层流水线返回进程退出码；单张图片错误会落到 CSV，不会让程序闪退。
int run_pipeline(const AppConfig& cfg,
                 std::span<const std::filesystem::path> input_paths,
                 std::stop_token stop_token);

int run_pipeline(const AppConfig& cfg, std::stop_token stop_token = {}) {
  return run_pipeline(cfg, std::span<const std::filesystem::path>{}, stop_token);
}

int run_pipeline(const AppConfig& cfg,
                 std::span<const std::filesystem::path> input_paths,
                 std::stop_token stop_token) {
  std::mutex print_mutex;
  auto runtime_cfg = cfg;
  runtime_cfg.frame_progress = [&](std::size_t item, std::size_t completed, std::size_t total) {
    // Bound worker event volume for long animations while retaining first/last progress.
    if (completed != 1 && completed != total && completed % 16 != 0) return;
    std::scoped_lock lock{print_mutex};
    if ((!cfg.studio_queue_manifest.empty() || !cfg.studio_cancel_event_name.empty())) {
      print_line(std::format("@AWJ-STUDIO/1 FRAME {} {} {}", item, completed, total));
    } else {
      print_line(std::format("动画 {}: 帧 {}/{}", item + 1, completed, total));
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
          print_line(std::format("@AWJ-STUDIO/1 ITEM {} {} {} {}",
                                 event.result.index, status, event.completed,
                                 event.total));
          if (event.kind == BatchEventKind::item_finished) {
            const auto microseconds = [](double seconds) -> std::int64_t {
              return seconds < 0.0
                         ? -1
                         : static_cast<std::int64_t>(seconds * 1'000'000.0 +
                                                     0.5);
            };
            print_line(std::format(
                "@AWJ-STUDIO/1 DETAIL {} {} {} {} {} {} {}",
                event.result.index,
                event.result.encoder_id.empty() ? "-" : event.result.encoder_id,
                event.result.encoder_threads,
                microseconds(event.result.decode_seconds),
                microseconds(event.result.prepare_seconds),
                microseconds(event.result.encode_seconds),
                microseconds(event.result.write_seconds)));
          }
        }
        if (event.kind == BatchEventKind::summary) {
          print_line("");
        }
        if (cfg.studio_queue_manifest.empty() && cfg.studio_cancel_event_name.empty() &&
            event.kind == BatchEventKind::item_finished && event.result.ok &&
            !event.result.skipped && !event.result.message.empty() &&
            event.result.message != "OK") {
          std::println(stderr, "[WARN] {}", event.result.message);
          std::fflush(stderr);
        }
        if (!event.text.empty()) {
          print_line(event.text);
        }
      },
      stop_token,
      input_paths);
  if (!summary) {
    print_line(std::format("[FAIL] {}", summary.error()));
    return 1;
  }
  return summary->exit_code;
}

}  // namespace awj
