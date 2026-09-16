#include <scn/scan.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "studio_fields.h"

import awj.core;
import awj.encoding_defaults;
import awj.studio_defaults;

namespace awj::studio {

std::string trim_copy(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  auto first = value.begin();
  while (first != value.end() && is_space(static_cast<unsigned char>(*first))) {
    ++first;
  }
  auto last = value.end();
  while (last != first && is_space(static_cast<unsigned char>(*(last - 1)))) {
    --last;
  }
  return std::string{first, last};
}


std::expected<int, std::string> parse_int_field(std::string text,
                                                std::string_view name,
                                                int minimum, int maximum) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::unexpected{std::format("{} 不能为空。", name)};
  }
  const std::string_view source{text};
  const auto parsed = scn::scan_int<int>(source);
  if (!parsed || parsed->begin() != parsed->end()) {
    return std::unexpected{std::format("{} 必须是整数。", name)};
  }
  const int value = parsed->value();
  if (value < minimum || value > maximum) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", name, minimum, maximum)};
  }
  return value;
}

std::expected<int, std::string> parse_quality_field(std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::unexpected{"质量不能为空。"};
  }
  return awj::parse_quality(awj::wide_from_utf8(text));
}

std::expected<std::optional<int>, std::string> parse_optional_int_field(
    std::string text, std::string_view name, int minimum, int maximum) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::optional<int>{};
  }
  const auto value = parse_int_field(std::move(text), name, minimum, maximum);
  if (!value) {
    return std::unexpected{value.error()};
  }
  return std::optional<int>{*value};
}

std::expected<std::optional<int>, std::string> parse_visual_quality_field(
    std::string text) {
  return parse_optional_int_field(std::move(text), "视觉质量", 1, 100);
}

std::expected<int, std::string> parse_jobs_field(std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return awj::default_max_jobs();
  }
  return awj::parse_auto_jobs(awj::wide_from_utf8(text));
}

std::expected<std::optional<int>, std::string> parse_bit_depth_field(
    std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::optional<int>{};
  }
  const auto value = parse_int_field(std::move(text), "位深", 1, 16);
  if (!value) {
    return std::unexpected{value.error()};
  }
  return std::optional<int>{*value};
}

std::expected<std::uint64_t, std::string> parse_memory_limit_field(
    std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::uint64_t{};
  }
  const auto gib = parse_int_field(std::move(text), "内存", 1, 1024);
  if (!gib) {
    return std::unexpected{"内存只允许填写 GiB 数字，例如 4；留空表示自动。"};
  }
  return static_cast<std::uint64_t>(*gib) *
         awj::studio_defaults::bytes_per_gib;
}

std::expected<awj::ImageSizeLimit, std::string> image_size_limit_from_fields(
    int mode_index, std::string max_width_text, std::string max_height_text,
    std::string max_long_edge_text, std::string max_short_edge_text, std::string scale_percent_text) {
  awj::ImageSizeLimit limit{};
  limit.mode = mode_index == 1   ? awj::ImageSizeLimitMode::none
               : mode_index == 2 ? awj::ImageSizeLimitMode::manual
                                 : awj::ImageSizeLimitMode::automatic;
  if (limit.mode != awj::ImageSizeLimitMode::manual) {
    return limit;
  }
  const auto parse = [](std::string text, std::string_view name)
      -> std::expected<std::optional<int>, std::string> {
    return parse_optional_int_field(std::move(text), name, 1, 1000000);
  };
  if (auto value = parse(std::move(max_width_text), "最大宽"); !value) {
    return std::unexpected{value.error()};
  } else {
    limit.max_width = *value;
  }
  if (auto value = parse(std::move(max_height_text), "最大高"); !value) {
    return std::unexpected{value.error()};
  } else {
    limit.max_height = *value;
  }
  if (auto value = parse(std::move(max_long_edge_text), "最大长边"); !value) {
    return std::unexpected{value.error()};
  } else {
    limit.max_long_edge = *value;
  }
  if (auto value = parse(std::move(max_short_edge_text), "最大短边"); !value) {
    return std::unexpected{value.error()};
  } else {
    limit.max_short_edge = *value;
  }
  if (auto value = parse_optional_int_field(std::move(scale_percent_text), "缩小至 (%)", 1, 100); !value) {
    return std::unexpected{value.error()};
  } else {
    limit.scale_percent = *value;
  }
  return limit;
}

}  // namespace awj::studio
