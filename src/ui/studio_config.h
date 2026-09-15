#pragma once

// Studio 配置的 JSONC 序列化、原子写盘与配置文件定位。
// 从 main.cpp 拆出；依赖 studio_state.h 的快照类型与 studio_json.h 的转义。

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "studio_state.h"

namespace awj::studio {

// 五种格式在配置键里的前缀顺序（avif/webp/jxl/jpgli/png）。
inline constexpr std::array<std::string_view, 5> menu_config_prefixes{
    "avif", "webp", "jxl", "jpgli", "png"};

std::string menu_config_key(std::string_view prefix, std::string_view name);

// 与可执行文件同目录的 AWJ.jsonc 路径；定位失败时返回空路径。
std::filesystem::path studio_config_path();

std::string json_escape(std::string_view value);

// 原子写：同目录临时文件 → 刷盘 → 原子替换，失败时清理临时文件、不动原文件。
std::expected<void, std::string> write_file_atomically(
    const std::filesystem::path& path, std::string_view content);

// 把当前快照写成配置文本并原子落盘；defaults 用于省略与默认值相同的项。
std::expected<void, std::string> write_studio_config_file(
    const StudioConfigSnapshot& current,
    const StudioConfigSnapshot& defaults);

}  // namespace awj::studio
