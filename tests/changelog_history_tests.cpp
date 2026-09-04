#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

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
  const auto prerelease = awj::ui::changelog_detail::parse(prerelease_zh,
                                                             prerelease_en);
  if (prerelease.size() != 1 || prerelease.front().channel != "prerelease") {
    std::cerr << "embedded 1.0.5 prerelease channel was not inferred\n";
    return 1;
  }

  const auto embedded = awj::ui::embedded_changelog_history();
  const auto all_zh = awj::ui::changelog_detail::parse_sections(
      awj::embedded_changelog::zh);
  if (embedded.size() != all_zh.size() || embedded.empty()) {
    std::cerr << "embedded changelog does not include every historical entry\n";
    return 1;
  }
  if (argc != 3) {
    std::cerr << "expected source changelog paths\n";
    return 1;
  }
  const auto source_zh = read_normalized(argv[1]);
  const auto source_en = read_normalized(argv[2]);
  if (source_zh != awj::embedded_changelog::zh ||
      source_en != awj::embedded_changelog::en) {
    std::cerr << "embedded changelog bytes do not equal normalized source bytes\n";
    return 1;
  }
  return 0;
}
