#pragma once

// 参数表单字段的文本解析：质量、速度/作业数、位深、内存上限与尺寸限制。
// 纯函数，从 main.cpp 拆出。

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "studio_state.h"

namespace awj::studio {

std::string trim_copy(std::string value);

std::expected<int, std::string> parse_int_field(std::string text,
                                                std::string_view name,
                                                int minimum, int maximum);
std::expected<int, std::string> parse_quality_field(std::string text);
std::expected<std::optional<int>, std::string> parse_optional_int_field(
    std::string text, std::string_view name, int minimum, int maximum);
std::expected<std::optional<int>, std::string> parse_visual_quality_field(
    std::string text);
std::expected<int, std::string> parse_jobs_field(std::string text);
std::expected<std::optional<int>, std::string> parse_bit_depth_field(
    std::string text);
std::expected<std::uint64_t, std::string> parse_memory_limit_field(
    std::string text);
std::expected<awj::ImageSizeLimit, std::string> image_size_limit_from_fields(
    int mode_index, std::string max_width_text, std::string max_height_text,
    std::string max_long_edge_text, std::string max_short_edge_text,
    std::string scale_percent_text);

}  // namespace awj::studio
