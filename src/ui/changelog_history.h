#pragma once

#include "awj_changelog_data.h"

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace awj::ui {

struct ChangelogHistoryEntry {
  std::string version{};
  std::string channel{};
  std::string published_at{};
  std::string release_url{};
  std::string changelog_zh_cn{};
  std::string changelog_en{};
};

namespace changelog_detail {

struct Section {
  std::string_view heading{};
  std::string_view body{};
};

inline std::vector<Section> parse_sections(std::string_view source) {
  std::vector<Section> sections;
  std::vector<std::pair<std::size_t, std::size_t>> headings;

  std::size_t cursor = 0;
  while (cursor < source.size()) {
    const auto line_end = source.find('\n', cursor);
    const auto end = line_end == std::string_view::npos ? source.size() : line_end;
    auto line = source.substr(cursor, end - cursor);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.starts_with("## ")) headings.emplace_back(cursor, end);
    cursor = end == source.size() ? source.size() : end + 1;
  }

  sections.reserve(headings.size());
  for (std::size_t index = 0; index < headings.size(); ++index) {
    const auto [heading_begin, heading_end] = headings[index];
    auto heading = source.substr(heading_begin + 3, heading_end - heading_begin - 3);
    if (!heading.empty() && heading.back() == '\r') heading.remove_suffix(1);
    const auto body_begin = heading_end == source.size() ? source.size() : heading_end + 1;
    const auto body_end = index + 1 < headings.size() ? headings[index + 1].first : source.size();
    auto body = source.substr(body_begin, body_end - body_begin);
    while (!body.empty() && (body.front() == '\n' || body.front() == '\r')) body.remove_prefix(1);
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.remove_suffix(1);
    sections.push_back(Section{.heading = heading, .body = body});
  }
  return sections;
}

inline bool starts_prerelease_marker(std::string_view body) {
  while (!body.empty() &&
         std::isspace(static_cast<unsigned char>(body.front())) != 0) {
    body.remove_prefix(1);
  }
  constexpr std::string_view marker = "- prerelease";
  if (body.size() < marker.size()) return false;
  for (std::size_t i = 0; i < marker.size(); ++i) {
    const auto left = static_cast<unsigned char>(body[i]);
    const auto right = static_cast<unsigned char>(marker[i]);
    if (std::tolower(left) != right) return false;
  }
  return body.size() == marker.size() ||
         body[marker.size()] == ':' || body[marker.size()] == '\xEF' ||
         std::isspace(static_cast<unsigned char>(body[marker.size()])) != 0;
}

inline std::vector<ChangelogHistoryEntry> parse(std::string_view chinese,
                                                std::string_view english) {
  const auto zh_sections = parse_sections(chinese);
  const auto en_sections = parse_sections(english);
  std::vector<ChangelogHistoryEntry> history;
  history.reserve(zh_sections.size());
  for (const auto& section : zh_sections) {
    const auto separator = section.heading.find(" - ");
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 3 >= section.heading.size() || section.body.empty()) {
      continue;
    }
    const auto version = section.heading.substr(0, separator);
    const auto date = section.heading.substr(separator + 3);
    std::string english_body;
    for (const auto& english_section : en_sections) {
      if (english_section.heading.starts_with(version) &&
          english_section.heading.size() > version.size() &&
          english_section.heading[version.size()] == ' ' &&
          !english_section.body.empty()) {
        english_body = std::string{english_section.body};
        break;
      }
    }
    // Keep the complete Chinese source history visible even while a legacy
    // entry has not yet been translated. Dropping it made the English UI
    // silently start at 1.0.0 instead of showing the full changelog.
    if (english_body.empty()) english_body = std::string{section.body};
    history.push_back(ChangelogHistoryEntry{
        .version = std::string{version},
        .channel = (starts_prerelease_marker(section.body) ||
                    starts_prerelease_marker(english_body))
                       ? "prerelease"
                       : "",
        .published_at = std::string{date},
        .changelog_zh_cn = std::string{section.body},
        .changelog_en = std::move(english_body)});
  }
  return history;
}

}  // namespace changelog_detail

inline std::vector<ChangelogHistoryEntry> embedded_changelog_history() {
  return changelog_detail::parse(embedded_changelog::zh, embedded_changelog::en);
}

// 只限制展示副本；内嵌历史与已验签的源数据始终保持完整。
template <class Less>
std::vector<ChangelogHistoryEntry> changelog_display_history(
    std::vector<ChangelogHistoryEntry> embedded,
    const std::vector<ChangelogHistoryEntry>& verified, Less newer,
    std::size_t limit = 10) {
  std::vector<ChangelogHistoryEntry> merged;
  merged.reserve(embedded.size() + verified.size());
  auto merge_one = [&](const auto& entry) {
    const auto found = std::find_if(merged.begin(), merged.end(),
        [&](const auto& value) { return value.version == entry.version; });
    if (found == merged.end()) merged.push_back(entry);
    else *found = entry;
  };
  for (const auto& entry : embedded) merge_one(entry);
  for (const auto& entry : verified) merge_one(entry);
  std::stable_sort(merged.begin(), merged.end(), newer);
  if (merged.size() > limit) merged.resize(limit);
  return merged;
}

struct ChangelogBlock {
  // 0 为 Markdown 正文，1..6 为标题，7 为纯文本围栏代码。
  int kind{};
  std::string text;
};

namespace changelog_detail {

inline std::string_view block_indent(std::string_view line) {
  for (int spaces = 0; spaces < 3 && line.starts_with(' '); ++spaces) {
    line.remove_prefix(1);
  }
  return line;
}

inline std::string_view trim_horizontal(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

inline std::vector<std::string_view> markdown_lines(std::string_view source) {
  std::vector<std::string_view> lines;
  for (std::size_t pos = 0; pos < source.size();) {
    const auto end = source.find_first_of("\r\n", pos);
    const auto next = end == std::string_view::npos ? source.size() : end;
    lines.push_back(source.substr(pos, next - pos));
    pos = next == source.size() ? source.size() : next + 1;
    if (next < source.size() && source[next] == '\r' && pos < source.size() && source[pos] == '\n') ++pos;
  }
  return lines;
}

inline std::size_t fence_run(std::string_view line) {
  if (line.empty() || (line.front() != '`' && line.front() != '~')) return 0;
  return line.find_first_not_of(line.front()) == std::string_view::npos
             ? line.size() : line.find_first_not_of(line.front());
}

inline bool reference_start(std::string_view line) {
  line = block_indent(line);
  if (!line.starts_with('[')) return false;
  for (std::size_t pos = 1; pos < line.size(); ++pos) {
    if (line[pos] == '\\' && pos + 1 < line.size()) { ++pos; continue; }
    if (line[pos] == '[') return false;
    if (line[pos] == ']') return pos > 1 && line.substr(pos + 1).starts_with(':');
  }
  return false;
}

inline bool list_start(std::string_view line) {
  line = block_indent(line);
  if (line.empty()) return false;
  const auto followed_by_space = [&](std::size_t pos) {
    return pos == line.size() || line[pos] == ' ' || line[pos] == '\t';
  };
  if ((line.front() == '-' || line.front() == '+' || line.front() == '*') &&
      followed_by_space(1)) return true;
  std::size_t digits = 0;
  while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') ++digits;
  return digits > 0 && digits <= 9 && digits < line.size() &&
         (line[digits] == '.' || line[digits] == ')') && followed_by_space(digits + 1);
}

inline int setext_kind(std::string_view line) {
  line = block_indent(line);
  if (line.empty() || (line.front() != '=' && line.front() != '-')) return 0;
  line = trim_horizontal(line);
  if (line.find_first_not_of(line.front()) != std::string_view::npos) return 0;
  return line.front() == '=' ? 1 : 2;
}

inline bool setext_paragraph(std::string_view line) {
  // 缩进、容器块及引用定义的上下文不在这里猜测；交回解析器或原文回退。
  if (line.empty() || line.front() == ' ' || line.front() == '\t' ||
      line.front() == '>' || line.front() == '<' || list_start(line) ||
      reference_start(line) || setext_kind(line)) return false;
  const auto compact = trim_horizontal(line);
  if (compact.find_first_not_of("*_ \t") == std::string_view::npos) return false;
  return true;
}

inline bool url_text_safe(std::string_view text) {
  for (std::size_t pos = 0; pos < text.size();) {
    const auto first = static_cast<unsigned char>(text[pos++]);
    if (first < 0x80) {
      if (first <= 0x20 || first == 0x7f || first == '\\' ||
          first == '<' || first == '>' || first == '"' || first == '`') return false;
      continue;
    }
    const unsigned count = first >= 0xc2 && first <= 0xdf ? 1 :
                           first >= 0xe0 && first <= 0xef ? 2 :
                           first >= 0xf0 && first <= 0xf4 ? 3 : 0;
    if (!count || text.size() - pos < count) return false;
    unsigned codepoint = first & (0x7f >> (count + 1));
    for (unsigned i = 0; i < count; ++i) {
      const auto c = static_cast<unsigned char>(text[pos++]);
      if ((c & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (c & 0x3f);
    }
    if (codepoint < (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) ||
        codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        codepoint <= 0xa0 || codepoint == 0x1680 || codepoint == 0x180e ||
        (codepoint >= 0x2000 && codepoint <= 0x200f) ||
        (codepoint >= 0x2028 && codepoint <= 0x202f) ||
        (codepoint >= 0x205f && codepoint <= 0x206f) ||
        codepoint == 0x3000 || codepoint == 0xfeff) return false;
  }
  return true;
}

}  // namespace changelog_detail

inline std::vector<ChangelogBlock> changelog_blocks(std::string_view source) {
  using namespace changelog_detail;
  std::vector<ChangelogBlock> blocks;
  std::string body;
  char fence_char = 0;
  std::size_t fence_size = 0;
  bool paragraph_start = true;
  auto flush = [&](int kind) {
    auto text = std::string_view{body};
    if (kind == 7) {
      if (text.ends_with('\n')) text.remove_suffix(1);
    } else {
      while (text.starts_with('\n')) text.remove_prefix(1);
      while (text.ends_with('\n')) text.remove_suffix(1);
    }
    if (!text.empty() || kind == 7) blocks.push_back({kind, std::string{text}});
    body.clear();
  };
  const auto lines = markdown_lines(source);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const auto line = lines[index];
    const auto trimmed = block_indent(line);
    const auto run = fence_run(trimmed);
    if (fence_char) {
      if (run >= fence_size && trimmed.front() == fence_char &&
          trim_horizontal(trimmed.substr(run)).empty()) {
        flush(7);
        fence_char = 0;
        paragraph_start = true;
      } else {
        body.append(line);
        body.push_back('\n');
      }
      continue;
    }
    // 反引号围栏的 info string 不得再含反引号；```literal``` 是行内代码。
    if (run >= 3 && (trimmed.front() != '`' ||
                    trimmed.substr(run).find('`') == std::string_view::npos) &&
        (line == trimmed || paragraph_start)) {
      flush(0);
      fence_char = trimmed.front();
      fence_size = run;
      continue;
    }
    std::size_t hashes = 0;
    while (hashes < trimmed.size() && trimmed[hashes] == '#') ++hashes;
    if (hashes >= 1 && hashes <= 6 &&
        (hashes == trimmed.size() || trimmed[hashes] == ' ' || trimmed[hashes] == '\t') &&
        (line == trimmed || paragraph_start)) {
      flush(0);
      auto heading = trim_horizontal(trimmed.substr(hashes));
      auto closing = heading.size();
      while (closing > 0 && heading[closing - 1] == '#') --closing;
      if (closing < heading.size() &&
          (closing == 0 || heading[closing - 1] == ' ' || heading[closing - 1] == '\t')) {
        heading = trim_horizontal(heading.substr(0, closing));
      }
      blocks.push_back({static_cast<int>(hashes), std::string{heading}});
      paragraph_start = true;
    } else if (paragraph_start && setext_paragraph(line) && index + 1 < lines.size() &&
               setext_kind(lines[index + 1])) {
      flush(0);
      blocks.push_back({setext_kind(lines[++index]), std::string{trim_horizontal(line)}});
      paragraph_start = true;
    } else {
      body.append(line);
      body.push_back('\n');
      // 空行仍属于同一个 Markdown 文档，让列表与引用式链接保持上下文。
      paragraph_start = trim_horizontal(line).empty();
    }
  }
  flush(fence_char ? 7 : 0);
  return blocks;
}

inline bool changelog_safe_link(std::string_view url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return false;
  auto scheme = url.substr(0, scheme_end);
  std::string_view expected = scheme.size() == 5 ? "https" : "http";
  if (scheme.size() != expected.size()) return false;
  for (std::size_t i = 0; i < scheme.size(); ++i) {
    const char c = scheme[i] >= 'A' && scheme[i] <= 'Z' ? scheme[i] + ('a' - 'A') : scheme[i];
    if (c != expected[i]) return false;
  }
  if (!changelog_detail::url_text_safe(url)) return false;
  const auto prefix = scheme_end + 3;
  const auto authority_end = url.find_first_of("/?#", prefix);
  auto host = url.substr(prefix, authority_end - prefix);
  if (host.empty() || host.find_first_of("@%") != std::string_view::npos) return false;
  std::string_view port;
  if (host.starts_with('[')) {
    const auto close = host.find(']');
    if (close == std::string_view::npos) return false;
    const auto address = host.substr(1, close - 1);
    if (address.find(':') == std::string_view::npos ||
        address.find_first_not_of("0123456789abcdefABCDEF:.") != std::string_view::npos) return false;
    const auto suffix = host.substr(close + 1);
    if (!suffix.empty()) {
      if (!suffix.starts_with(':')) return false;
      port = suffix.substr(1);
      if (port.empty()) return false;
    }
  } else {
    const auto colon = host.find(':');
    if (colon != std::string_view::npos) {
      port = host.substr(colon + 1);
      host = host.substr(0, colon);
      if (port.empty()) return false;
    }
    if (host.empty() || host.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") != std::string_view::npos ||
        host.front() == '.' || host.find("..") != std::string_view::npos) return false;
  }
  unsigned port_number = 0;
  for (const char c : port) {
    if (c < '0' || c > '9') return false;
    port_number = port_number * 10 + static_cast<unsigned>(c - '0');
    if (port_number > 65535) return false;
  }
  if (!port.empty() && !port_number) return false;
  const auto hex = [](char c) -> int {
    return c >= '0' && c <= '9' ? c - '0' :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  };
  for (std::size_t pos = prefix; pos < url.size(); ++pos) {
    if (url[pos] != '%') continue;
    if (pos + 2 >= url.size() || hex(url[pos + 1]) < 0 || hex(url[pos + 2]) < 0) return false;
    const auto decoded = hex(url[pos + 1]) * 16 + hex(url[pos + 2]);
    if (decoded < 0x20 || decoded == 0x7f || decoded == '\\') return false;
    pos += 2;
  }
  return true;
}

inline std::optional<std::string> changelog_safe_markdown(std::string_view source) {
  // 链接目标/标题中的反引号不是代码跨度；遇到这类歧义时整块安全显示原文。
  std::size_t link_depth = 0;
  char link_quote = 0;
  bool reference_line = false;
  for (std::size_t i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (c == '\\' && i + 1 < source.size()) { ++i; continue; }
    if (reference_line) {
      if (c == '`') return std::nullopt;
      if (c == '\r' || c == '\n') reference_line = false;
    }
    if (link_depth) {
      if (c == '`') return std::nullopt;
      if (link_quote) {
        if (c == link_quote) link_quote = 0;
      } else if (c == '\'' || c == '"') {
        link_quote = c;
      } else if (c == '(') {
        ++link_depth;
      } else if (c == ')') {
        --link_depth;
      }
    } else if (c == ']' && i + 1 < source.size()) {
      if (source[i + 1] == '(') { link_depth = 1; ++i; }
      else if (source[i + 1] == ':') { reference_line = true; ++i; }
    }
  }
  std::string result;
  result.reserve(source.size());
  for (std::size_t pos = 0; pos < source.size();) {
    if (source[pos] == '\\' && pos + 1 < source.size()) {
      result.append(source.substr(pos, 2));
      pos += 2;
    } else if (source[pos] == '`') {
      const auto begin = pos;
      while (pos < source.size() && source[pos] == '`') ++pos;
      const auto count = pos - begin;
      auto close = source.find('`', pos);
      while (close != std::string_view::npos) {
        auto end = close;
        while (end < source.size() && source[end] == '`') ++end;
        if (end - close == count) break;
        close = source.find('`', end);
      }
      // 跨行 code span 可能被段落或容器边界打断，不能靠扫描猜测其 HTML 上下文。
      if (close != std::string_view::npos &&
          source.substr(pos, close - pos).find_first_of("\r\n") != std::string_view::npos) return std::nullopt;
      // 只跳过真正闭合的同行 code span，未闭合的反引号不能掩护后续 HTML。
      if (close != std::string_view::npos) pos = close + count;
      result.append(source.substr(begin, pos - begin));
    } else if (source[pos] == '<') {
      const auto end = source.find('>', pos + 1);
      if (end != std::string_view::npos && changelog_safe_link(source.substr(pos + 1, end - pos - 1))) {
        result.append(source.substr(pos, end - pos + 1));
        pos = end + 1;
      } else {
        result += "\\<";
        ++pos;
      }
    } else if (source.substr(pos).starts_with("![")) {
      const auto matching_end = [&](std::size_t open, char left, char right) {
        std::size_t depth = 1;
        for (auto cursor = open + 1; cursor < source.size(); ++cursor) {
          if (source[cursor] == '\\' && cursor + 1 < source.size()) { ++cursor; continue; }
          if (source[cursor] == left) ++depth;
          if (source[cursor] == right && --depth == 0) return cursor + 1;
        }
        return std::string_view::npos;
      };
      auto end = matching_end(pos + 1, '[', ']');
      if (end == std::string_view::npos) return std::nullopt;
      if (end < source.size() && (source[end] == '(' || source[end] == '[')) {
        end = matching_end(end, source[end], source[end] == '(' ? ')' : ']');
        if (end == std::string_view::npos) return std::nullopt;
      }
      // 整个图片语法按原文显示，包括引用式图片的第二组括号与嵌套标记。
      while (pos < end) {
        const auto c = static_cast<unsigned char>(source[pos++]);
        if ((c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40) ||
            (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e)) result.push_back('\\');
        result.push_back(static_cast<char>(c));
      }
    } else {
      result.push_back(source[pos++]);
    }
  }
  return result;
}

}  // namespace awj::ui
