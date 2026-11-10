#pragma once

// AWJ Studio worker（CLI 子进程）stdout 事件行的解析。
// 跨进程约定：行前缀与字段顺序必须与 src/app/pipeline.ixx 的输出保持一致。

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace awj::studio {

struct StudioWorkerItemEvent {
  std::size_t index{};
  char status{};
  std::size_t completed{};
  std::size_t total{};
};

struct StudioWorkerOutputEvent {
  std::size_t index{};
  std::string path_utf8{};
};

// OUTPUT is emitted before the successful ITEM. Hex framing preserves spaces,
// Unicode and embedded newlines without treating a display log as a path.
inline std::optional<StudioWorkerOutputEvent> parse_studio_worker_output_event(
    std::string_view line) {
  constexpr std::string_view prefix = "@AWJ-STUDIO/1 OUTPUT ";
  if (!line.starts_with(prefix)) return std::nullopt;
  line.remove_prefix(prefix.size());
  const auto separator = line.find(' ');
  if (separator == std::string_view::npos || separator == 0) return std::nullopt;
  std::size_t index{};
  const auto parsed = std::from_chars(line.data(), line.data() + separator, index);
  if (parsed.ec != std::errc{} || parsed.ptr != line.data() + separator)
    return std::nullopt;
  line.remove_prefix(separator + 1);
  if (line.empty() || line.size() % 2 != 0) return std::nullopt;
  const auto digit = [](char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  };
  std::string path;
  path.reserve(line.size() / 2);
  for (std::size_t i = 0; i < line.size(); i += 2) {
    const int high = digit(line[i]);
    const int low = digit(line[i + 1]);
    if (high < 0 || low < 0 || (high == 0 && low == 0)) return std::nullopt;
    path.push_back(static_cast<char>((high << 4) | low));
  }
  return StudioWorkerOutputEvent{.index = index, .path_utf8 = std::move(path)};
}

struct StudioWorkerDetailEvent {
  std::size_t index{};
  std::string encoder_id{};
  int encoder_threads{};
  std::int64_t decode_microseconds{-1};
  std::int64_t prepare_microseconds{-1};
  std::int64_t encode_microseconds{-1};
  std::int64_t write_microseconds{-1};
};

// 独立于 DETAIL；连接旧 worker 而未收到此事件时，resize 仍为未知。
struct StudioWorkerResizeEvent {
  std::size_t index{};
  std::int64_t resize_microseconds{-1};
  int resize_workers{};
};

inline bool is_studio_worker_resize_event_line(std::string_view line) noexcept {
  constexpr std::string_view marker = "@AWJ-STUDIO/1 RESIZE";
  return line == marker ||
         (line.starts_with(marker) && line.size() > marker.size() &&
          line[marker.size()] == ' ');
}

inline double studio_worker_seconds(std::int64_t microseconds) noexcept {
  return microseconds < 0 ? -1.0
                          : static_cast<double>(microseconds) / 1'000'000.0;
}

// TaskRow 仅保存显示字符串；右键窗口另外保留原始计时，
// 合并互不重叠的事件时，不反解析或再次舍入 UI 文本。
struct StudioWorkerStageTimings {
  double decode_seconds{-1.0};
  double prepare_seconds{-1.0};
  double encode_seconds{-1.0};
  double write_seconds{-1.0};
  double resize_seconds{-1.0};
  int resize_workers{};

  void apply(const StudioWorkerDetailEvent& event) noexcept {
    decode_seconds = studio_worker_seconds(event.decode_microseconds);
    prepare_seconds = studio_worker_seconds(event.prepare_microseconds);
    encode_seconds = studio_worker_seconds(event.encode_microseconds);
    write_seconds = studio_worker_seconds(event.write_microseconds);
  }

  void apply(const StudioWorkerResizeEvent& event) noexcept {
    resize_seconds = studio_worker_seconds(event.resize_microseconds);
    resize_workers = event.resize_workers;
  }
};

struct StudioWorkerFrameEvent {
  std::size_t index{}, completed{}, total{};
};
std::optional<StudioWorkerFrameEvent> parse_studio_worker_frame_event(std::string_view line);

std::optional<StudioWorkerItemEvent> parse_studio_worker_item_event(
    std::string_view line);
std::optional<StudioWorkerDetailEvent> parse_studio_worker_detail_event(
    std::string_view line);
std::optional<StudioWorkerResizeEvent> parse_studio_worker_resize_event(
    std::string_view line);

}  // namespace awj::studio
