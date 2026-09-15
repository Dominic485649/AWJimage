#include "studio_worker_events.h"

#include <scn/scan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace awj::studio {

std::optional<StudioWorkerItemEvent> parse_studio_worker_item_event(
    std::string_view line) {
  constexpr std::string_view prefix = "@AWJ-STUDIO/1 ITEM ";
  if (!line.starts_with(prefix)) {
    return std::nullopt;
  }
  line.remove_prefix(prefix.size());
  std::array<std::string_view, 4> fields;
  for (auto& field : fields) {
    const auto separator = line.find(' ');
    if (separator == std::string_view::npos) {
      field = line;
      line = {};
    } else {
      field = line.substr(0, separator);
      line.remove_prefix(separator + 1);
    }
    if (field.empty()) {
      return std::nullopt;
    }
  }
  if (!line.empty() || fields[1].size() != 1 ||
      (fields[1][0] != 'R' && fields[1][0] != 'D' &&
       fields[1][0] != 'S' &&
       fields[1][0] != 'C' && fields[1][0] != 'F')) {
    return std::nullopt;
  }
  const auto parse_size = [](std::string_view field)
      -> std::optional<std::size_t> {
    const auto value = scn::scan_int<std::size_t>(field);
    if (!value || value->begin() != value->end()) {
      return std::nullopt;
    }
    return value->value();
  };
  const auto index = parse_size(fields[0]);
  const auto completed = parse_size(fields[2]);
  const auto total = parse_size(fields[3]);
  if (!index || !completed || !total || *total == 0 ||
      *index >= *total || *completed > *total) {
    return std::nullopt;
  }
  return StudioWorkerItemEvent{.index = *index,
                               .status = fields[1][0],
                               .completed = *completed,
                               .total = *total};
}

std::optional<StudioWorkerDetailEvent> parse_studio_worker_detail_event(
    std::string_view line) {
  constexpr std::string_view prefix = "@AWJ-STUDIO/1 DETAIL ";
  if (!line.starts_with(prefix)) {
    return std::nullopt;
  }
  line.remove_prefix(prefix.size());
  std::array<std::string_view, 7> fields;
  for (auto& field : fields) {
    const auto separator = line.find(' ');
    if (separator == std::string_view::npos) {
      field = line;
      line = {};
    } else {
      field = line.substr(0, separator);
      line.remove_prefix(separator + 1);
    }
    if (field.empty()) {
      return std::nullopt;
    }
  }
  if (!line.empty()) {
    return std::nullopt;
  }
  const auto index = scn::scan_int<std::size_t>(fields[0]);
  const auto threads = scn::scan_int<int>(fields[2]);
  const auto decode = scn::scan_int<std::int64_t>(fields[3]);
  const auto prepare = scn::scan_int<std::int64_t>(fields[4]);
  const auto encode = scn::scan_int<std::int64_t>(fields[5]);
  const auto write = scn::scan_int<std::int64_t>(fields[6]);
  if (!index || index->begin() != index->end() || !threads ||
      threads->begin() != threads->end() || !decode ||
      decode->begin() != decode->end() || !prepare ||
      prepare->begin() != prepare->end() || !encode ||
      encode->begin() != encode->end() || !write ||
      write->begin() != write->end() || threads->value() < 0 ||
      decode->value() < -1 || prepare->value() < -1 ||
      encode->value() < -1 || write->value() < -1) {
    return std::nullopt;
  }
  return StudioWorkerDetailEvent{
      .index = index->value(),
      .encoder_id = fields[1] == "-" ? std::string{} : std::string{fields[1]},
      .encoder_threads = threads->value(),
      .decode_microseconds = decode->value(),
      .prepare_microseconds = prepare->value(),
      .encode_microseconds = encode->value(),
      .write_microseconds = write->value()};
}

}  // namespace awj::studio
