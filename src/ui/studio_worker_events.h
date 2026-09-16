#pragma once

// AWJ Studio worker（CLI 子进程）stdout 事件行的解析。
// 跨进程约定：行前缀与字段顺序必须与 src/app/pipeline.ixx 的输出保持一致。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace awj::studio {

struct StudioWorkerItemEvent {
  std::size_t index{};
  char status{};
  std::size_t completed{};
  std::size_t total{};
};

struct StudioWorkerDetailEvent {
  std::size_t index{};
  std::string encoder_id{};
  int encoder_threads{};
  std::int64_t decode_microseconds{-1};
  std::int64_t prepare_microseconds{-1};
  std::int64_t encode_microseconds{-1};
  std::int64_t write_microseconds{-1};
};

std::optional<StudioWorkerItemEvent> parse_studio_worker_item_event(
    std::string_view line);
std::optional<StudioWorkerDetailEvent> parse_studio_worker_detail_event(
    std::string_view line);

}  // namespace awj::studio
