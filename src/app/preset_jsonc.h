#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <set>
#include <utility>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace awj::preset_jsonc {

using Json = nlohmann::ordered_json;
inline constexpr std::size_t max_file_bytes = 1024u * 1024u;
inline constexpr int max_depth = 64;

struct Document {
  Json active;
  std::string raw;
  std::vector<std::string> comments;
};

// JSON 的语义和语法由 nlohmann 校验；这里只提取注释，不解释注释里的内容。
inline Document parse(std::string raw) {
  std::vector<std::set<std::string>> object_keys;
  auto active = Json::parse(raw, [&](int depth, Json::parse_event_t event, Json& value) {
    if (depth > max_depth) throw std::runtime_error{"JSONC 嵌套超过 64 层。"};
    if (event == Json::parse_event_t::object_start) object_keys.emplace_back();
    if (event == Json::parse_event_t::key &&
        !object_keys.back().insert(value.get<std::string>()).second)
      throw std::runtime_error{"JSONC 同一对象不能包含重复键。"};
    if (event == Json::parse_event_t::object_end) object_keys.pop_back();
    return true;
  }, true, true);
  std::vector<std::string> comments;
  bool quoted = false;
  bool escaped = false;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char ch = raw[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') quoted = false;
      continue;
    }
    if (ch == '"') {
      quoted = true;
    } else if (ch == '/' && i + 1 < raw.size() && raw[i + 1] == '/') {
      const auto end = raw.find('\n', i + 2);
      const auto length = (end == std::string::npos ? raw.size() : end) - i;
      comments.push_back(raw.substr(i, length));
      i += length - 1;
    } else if (ch == '/' && i + 1 < raw.size() && raw[i + 1] == '*') {
      const auto end = raw.find("*/", i + 2);
      // 上面的正式解析器已拒绝未闭合注释，不将嵌套起始标记解释为新注释。
      if (end == std::string::npos) throw std::runtime_error{"JSONC 块注释未闭合。"};
      comments.push_back(raw.substr(i, end + 2 - i));
      i = end + 1;
    }
  }
  return {std::move(active), std::move(raw), std::move(comments)};
}

inline void retain_unknown(const Json& original, const Json& known,
                           const std::string& path,
                           std::vector<std::string>& comments) {
  if (!original.is_object() || !known.is_object()) return;
  for (const auto& [key, value] : original.items()) {
    const auto field_path = path + "[" + Json(key).dump() + "]";
    if (known.contains(key)) {
      retain_unknown(value, known.at(key), field_path, comments);
      continue;
    }
    std::string retained = "// 兼容保留（不读取）：" + field_path + "\n";
    const auto text = Json{{key, value}}.dump(2);
    std::size_t start = 0;
    while (start < text.size()) {
      const auto end = text.find('\n', start);
      const auto length = (end == std::string::npos ? text.size() : end) - start;
      retained += "// ";
      retained.append(text, start, length);
      retained += '\n';
      start += length + 1;
    }
    comments.push_back(std::move(retained));
  }
}

inline std::string serialize(const Json& known, const Document* original = nullptr) {
  // 没有结构或数值变化时不重排注释、不改变排版，也不触发写盘。
  if (original && original->active == known) return original->raw;
  auto comments = original ? original->comments : std::vector<std::string>{};
  if (original) retain_unknown(original->active, known, "$", comments);
  std::string text;
  for (const auto& comment : comments) {
    text += comment;
    if (comment.empty() || comment.back() != '\n') text += '\n';
  }
  text += known.dump(2);
  text += '\n';
  if (text.size() > max_file_bytes)
    throw std::runtime_error{"兼容后的预设超过 1 MiB 限制，原文件未修改。"};
  return text;
}

}  // namespace awj::preset_jsonc
