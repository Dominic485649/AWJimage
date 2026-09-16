#pragma once

// Studio 配置文件的 JSONC 解析与取值工具。
// 纯函数，不依赖 Slint 或 Studio 状态，从 main.cpp 拆出以缩小单个翻译单元。

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>

namespace awj::studio_json {

struct JsonConfigValue {
  enum class Kind { boolean, integer, string };
  Kind kind{};
  bool boolean{};
  // 用 64 位存整数：更新状态里有 Unix 时间戳，2038 年会超出 32 位 int。
  // 取值时再按各自的合法区间收窄回 int。
  std::int64_t integer{};
  std::string string{};
};

// 去掉 // 与 /* */ 注释（JSONC），保留字符串内的内容。
std::string strip_jsonc_comments(std::string_view text);

std::expected<std::unordered_map<std::string, JsonConfigValue>, std::string>
parse_jsonc_config(std::string_view source);

// 缺失的键返回空 error 的 unexpected，调用方据此区分「未设置」与「非法」。
std::expected<int, std::string> config_int(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, int minimum, int maximum);
std::expected<std::int64_t, std::string> config_int64(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, std::int64_t minimum, std::int64_t maximum);
std::expected<bool, std::string> config_bool(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key);
std::expected<std::string, std::string> config_string(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key);
bool config_has_key(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key);

}  // namespace awj::studio_json
