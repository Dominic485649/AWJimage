#include "studio_json.h"

#include <scn/scan.h>

#include <cctype>
#include <format>

namespace awj::studio_json {

std::string strip_jsonc_comments(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (in_string) {
      out.push_back(ch);
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
      out.push_back(ch);
      continue;
    }
    if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') {
        ++i;
      }
      if (i < text.size()) {
        out.push_back('\n');
      }
      continue;
    }
    if (ch == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        out.push_back(text[i] == '\n' ? '\n' : ' ');
        ++i;
      }
      if (i + 1 < text.size()) {
        ++i;
      }
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

void skip_json_ws(std::string_view text, std::size_t& pos) noexcept {
  while (pos < text.size() &&
         std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
}

std::expected<std::string, std::string> parse_json_string(std::string_view text,
                                                          std::size_t& pos) {
  if (pos >= text.size() || text[pos] != '"') {
    return std::unexpected{"期望字符串。"};
  }
  ++pos;
  std::string out;
  while (pos < text.size()) {
    const char ch = text[pos++];
    if (ch == '"') {
      return out;
    }
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (pos >= text.size()) {
      return std::unexpected{"字符串转义不完整。"};
    }
    const char esc = text[pos++];
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        out.push_back(esc);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        return std::unexpected{"不支持的字符串转义。"};
    }
  }
  return std::unexpected{"字符串未闭合。"};
}

std::expected<JsonConfigValue, std::string> parse_json_value(
    std::string_view text, std::size_t& pos) {
  skip_json_ws(text, pos);
  if (pos >= text.size()) {
    return std::unexpected{"配置值不完整。"};
  }
  if (text[pos] == '"') {
    auto value = parse_json_string(text, pos);
    if (!value) {
      return std::unexpected{value.error()};
    }
    return JsonConfigValue{.kind = JsonConfigValue::Kind::string,
                           .string = std::move(*value)};
  }
  if (text.substr(pos, 4) == "true") {
    pos += 4;
    return JsonConfigValue{.kind = JsonConfigValue::Kind::boolean,
                           .boolean = true};
  }
  if (text.substr(pos, 5) == "false") {
    pos += 5;
    return JsonConfigValue{.kind = JsonConfigValue::Kind::boolean};
  }
  const std::size_t start = pos;
  if (text[pos] == '-') {
    ++pos;
  }
  while (pos < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  if (pos == start || (pos == start + 1 && text[start] == '-')) {
    return std::unexpected{"配置值只支持布尔、整数或字符串。"};
  }
  const auto parsed = scn::scan_int<std::int64_t>(text.substr(start, pos - start));
  if (!parsed) {
    return std::unexpected{"整数配置值无效。"};
  }
  return JsonConfigValue{.kind = JsonConfigValue::Kind::integer,
                         .integer = parsed->value()};
}

std::expected<std::unordered_map<std::string, JsonConfigValue>, std::string>
parse_jsonc_config(std::string_view source) {
  const auto text = strip_jsonc_comments(source);
  std::string_view view{text};
  std::unordered_map<std::string, JsonConfigValue> values;
  std::size_t pos = 0;
  skip_json_ws(view, pos);
  if (pos >= view.size()) {
    return values;
  }
  if (view[pos++] != '{') {
    return std::unexpected{"配置文件根节点必须是对象。"};
  }
  while (true) {
    skip_json_ws(view, pos);
    if (pos < view.size() && view[pos] == '}') {
      ++pos;
      break;
    }
    auto key = parse_json_string(view, pos);
    if (!key) {
      return std::unexpected{key.error()};
    }
    skip_json_ws(view, pos);
    if (pos >= view.size() || view[pos++] != ':') {
      return std::unexpected{"配置项缺少冒号。"};
    }
    auto value = parse_json_value(view, pos);
    if (!value) {
      return std::unexpected{value.error()};
    }
    values.insert_or_assign(std::move(*key), std::move(*value));
    skip_json_ws(view, pos);
    if (pos < view.size() && view[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < view.size() && view[pos] == '}') {
      ++pos;
      break;
    }
    return std::unexpected{"配置项之间缺少逗号。"};
  }
  skip_json_ws(view, pos);
  if (pos != view.size()) {
    return std::unexpected{"配置对象后存在多余内容。"};
  }
  return values;
}

std::expected<int, std::string> config_int(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, int minimum, int maximum) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::integer) {
    return std::unexpected{std::format("{} 必须是整数。", key)};
  }
  // 先按 64 位比较再收窄：值本身可能超出 int，直接转换是未定义行为。
  if (it->second.integer < static_cast<std::int64_t>(minimum) ||
      it->second.integer > static_cast<std::int64_t>(maximum)) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", key, minimum, maximum)};
  }
  return static_cast<int>(it->second.integer);
}

// 64 位整数配置项（Unix 时间戳、manifest 序号）。
std::expected<std::int64_t, std::string> config_int64(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, std::int64_t minimum, std::int64_t maximum) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::integer) {
    return std::unexpected{std::format("{} 必须是整数。", key)};
  }
  if (it->second.integer < minimum || it->second.integer > maximum) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", key, minimum, maximum)};
  }
  return it->second.integer;
}

std::expected<bool, std::string> config_bool(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::boolean) {
    return std::unexpected{std::format("{} 必须是布尔值。", key)};
  }
  return it->second.boolean;
}

std::expected<std::string, std::string> config_string(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::string) {
    return std::unexpected{std::format("{} 必须是字符串。", key)};
  }
  return it->second.string;
}

bool config_has_key(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key) {
  return values.contains(std::string{key});
}
}  // namespace awj::studio_json
