#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "changelog_history.h"

std::string read_normalized(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::string input((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());
  std::string normalized;
  normalized.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\r') {
      if (i + 1 < input.size() && input[i + 1] == '\n') ++i;
      normalized.push_back('\n');
    } else {
      normalized.push_back(input[i]);
    }
  }
  return normalized;
}

bool verify_display_limit() {
  const auto newer = [](const auto& a, const auto& b) { return a.version > b.version; };
  using awj::ui::ChangelogHistoryEntry;
  std::vector<ChangelogHistoryEntry> versions;
  for (int i = 0; i < 11; ++i) {
    versions.push_back({.version = std::string(1, static_cast<char>('a' + i)),
                        .changelog_zh_cn = std::string(20000, 'x') + "完整末行"});
  }
  for (const std::size_t count : {0u, 1u, 10u, 11u}) {
    const std::vector<ChangelogHistoryEntry> input(versions.begin(), versions.begin() + count);
    const auto display = awj::ui::changelog_display_history(input, {}, newer);
    if (display.size() != std::min<std::size_t>(count, 10) || input.size() != count ||
        (!display.empty() && (display.front().version != input.back().version ||
                             display.front().changelog_zh_cn != input.back().changelog_zh_cn))) {
      return false;
    }
  }
  const std::vector<ChangelogHistoryEntry> verified{{.version = "a", .changelog_zh_cn = "signed"}};
  const auto merged = awj::ui::changelog_display_history(
      {versions[0], versions[0], versions[1]}, verified, newer);
  return merged.size() == 2 && merged[0].version == "b" &&
         merged[1].changelog_zh_cn == "signed" && verified.front().changelog_zh_cn == "signed" &&
         versions.size() == 11 && versions.front().changelog_zh_cn.ends_with("完整末行") &&
         awj::ui::changelog_display_history(versions, verified, newer, 0).empty();
}

bool verify_markdown_blocks() {
  using awj::ui::changelog_blocks;
  const auto blocks = changelog_blocks(
      "# **标题** [Docs](https://example.com)\n\n- **bold** and `code`\n\n"
      "```cpp\n# not a heading\n```\nSetext\n------\n");
  if (blocks.size() != 4 || blocks[0].kind != 1 ||
      blocks[0].text != "**标题** [Docs](https://example.com)" || blocks[1].kind != 0 ||
      blocks[2].kind != 7 || blocks[2].text != "# not a heading" ||
      blocks[3].kind != 2 || blocks[3].text != "Setext") return false;

  for (const auto literal : {"1. item\n---", "2) item\n===", "- item\n---", "> quote\n---",
                              "    code\n---", "[ref]: https://example.com\n---",
                              "- item\n\n  continuation\n---", "Title\n    ---"}) {
    const auto result = changelog_blocks(literal);
    if (result.size() != 1 || result[0].kind != 0 || result[0].text != literal) return false;
  }
  const auto inline_fence = changelog_blocks("```literal```\n# Next\n");
  if (inline_fence.size() != 2 || inline_fence[0].kind != 0 ||
      inline_fence[0].text != "```literal```" || inline_fence[1].kind != 1) return false;
  const auto fences = changelog_blocks("````cpp\r\n```\r\n# code\r\n````\r\n~~~lang`ok\nbody\n~~~\n");
  if (fences.size() != 2 || fences[0].kind != 7 || fences[0].text != "```\n# code" ||
      fences[1].kind != 7 || fences[1].text != "body") return false;
  const auto unclosed = changelog_blocks("```cpp\nline\n# code");
  const auto empty_code = changelog_blocks("```\n```\n");
  if (unclosed.size() != 1 || unclosed[0].kind != 7 || unclosed[0].text != "line\n# code" ||
      empty_code.size() != 1 || empty_code[0].kind != 7 || !empty_code[0].text.empty()) return false;
  const auto setext = changelog_blocks("Title\n===  \n\n## Closing ###\n###### Six\n");
  if (setext.size() != 3 || setext[0].kind != 1 || setext[0].text != "Title" ||
      setext[1].text != "Closing" || setext[2].kind != 6) return false;
  constexpr std::string_view references = "[Docs][ref]\n\n[ref]: https://example.com/";
  const auto reference_blocks = changelog_blocks(references);
  if (reference_blocks.size() != 1 || reference_blocks[0].text != references) return false;

  const auto safe = awj::ui::changelog_safe_markdown(
      "- **bold** a < b and `x < y`\n<u>literal</u>\n![image](https://example.com/a)");
  if (!safe || *safe !=
      "- **bold** a \\< b and `x < y`\n\\<u>literal\\</u>\n\\!\\[image\\]\\(https\\:\\/\\/example\\.com\\/a\\)") return false;
  const auto autolink = awj::ui::changelog_safe_markdown(
      "<https://example.com/> [Docs](<https://example.com/docs>)");
  if (!autolink || *autolink != "<https://example.com/> [Docs](<https://example.com/docs>)") return false;
  const auto unclosed_inline = awj::ui::changelog_safe_markdown("`open <u>x</u>");
  if (!unclosed_inline || *unclosed_inline != "`open \\<u>x\\</u>" ||
      awj::ui::changelog_safe_markdown("`open\n\n<u>x</u>\n\n`") ||
      awj::ui::changelog_safe_markdown("`open\r\r<u>x</u>\r\r`")) return false;
  for (const auto ambiguous : {
      "[x](https://example.com/ \"`title\") <font color=\"#00000000\">HIDDEN</font> `",
      "[x](https://example.com/ \"a) `title\") <u>visible</u> `",
      "[ref]: https://example.com/ \"`title\"\ntext `"}) {
    if (awj::ui::changelog_safe_markdown(ambiguous)) return false;
  }
  return true;
}

bool verify_link_policy() {
  constexpr std::string_view bad[] = {
      "file:///tmp/x", "javascript:alert(1)", "mailto:a@example.com", "//example.com",
      "https://", "https:///path", "https://?x", "https://host\\path", "https://user@host/",
      "https://user%40host/", "https://host:abc/", "https://host:/", "https://host:65536/",
      "https://host:0/", "https://[::1", "https://host/a b", "https://host/a\tb",
      "https://host/a\nb", "https://host/a\rb", "https://host/a\x7f", "https://host/%00",
      "https://host/%0a", "https://host/%5C", "https://host/%gg", "https://host/%",
      "https://host/<u>", "https://host/\"x\"", "https://host/\xC2\xA0",
      "https://host/\xE3\x80\x80", "https://host/\xE2\x80\xAE", "https://host/\xFF"};
  for (const auto url : bad) if (awj::ui::changelog_safe_link(url)) return false;
  constexpr char nul[] = "https://host/a\0b";
  if (awj::ui::changelog_safe_link(std::string_view{nul, sizeof(nul) - 1})) return false;
  for (const auto url : {"https://example.com/path", "http://example.com/",
                         "HTTPS://Example.COM/中文?q=a%20b", "http://localhost:8080/path?q=x#f",
                         "https://[::1]:443/", "https://example.com/a;b?x=1&y=2"}) {
    if (!awj::ui::changelog_safe_link(url)) return false;
  }
  return true;
}

int main(int argc, char** argv) {
  constexpr std::string_view zh =
      "## 1.0.4 - 2026-08-21\n\n- 新日志\n\n"
      "## 0.1 - 2026-05-16\n\n- 旧日志\n";
  constexpr std::string_view en = "## 1.0.4 - 2026-08-21\n\n- New log\n";
  const auto parsed = awj::ui::changelog_detail::parse(zh, en);
  if (parsed.size() != 2 || parsed[0].changelog_en != "- New log" ||
      parsed[1].changelog_en != parsed[1].changelog_zh_cn) {
    std::cerr << "legacy changelog fallback lost an entry\n";
    return 1;
  }
  constexpr std::string_view prerelease_zh =
      "## 1.0.5 - 2026-08-24\n\n- prerelease：桥接更新\n";
  constexpr std::string_view prerelease_en =
      "## 1.0.5 - 2026-08-24\n\n- Prerelease: bridge update\n";
  const auto prerelease = awj::ui::changelog_detail::parse(prerelease_zh, prerelease_en);
  if (prerelease.size() != 1 || prerelease.front().channel != "prerelease") {
    std::cerr << "embedded 1.0.5 prerelease channel was not inferred\n";
    return 1;
  }
  if (!verify_display_limit() || !verify_markdown_blocks() || !verify_link_policy()) {
    std::cerr << "changelog display, Markdown or safe-link regression failed\n";
    return 1;
  }
  const auto embedded = awj::ui::embedded_changelog_history();
  const auto all_zh = awj::ui::changelog_detail::parse_sections(awj::embedded_changelog::zh);
  if (embedded.size() != all_zh.size() || embedded.empty()) {
    std::cerr << "embedded changelog does not include every historical entry\n";
    return 1;
  }
  if (argc == 2 && std::string_view{argv[1]} == "--changelog-only") return 0;
  if (argc != 3) {
    std::cerr << "expected source changelog paths, or --changelog-only\n";
    return 1;
  }
  const auto source_zh = read_normalized(argv[1]);
  const auto source_en = read_normalized(argv[2]);
  if (source_zh != awj::embedded_changelog::zh || source_en != awj::embedded_changelog::en) {
    std::cerr << "embedded changelog bytes do not equal normalized source bytes\n";
    return 1;
  }
  return 0;
}
