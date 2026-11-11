#pragma once

#include "changelog_history.h"

#include <memory>
#include <string>
#include <vector>

// 在生成的 Slint 类型和 update 模块 import 之后包含。
namespace awj::ui {

namespace changelog_detail {

inline std::string reference_context(const std::vector<ChangelogBlock>& blocks) {
  std::string context;
  const slint::StyledText empty;
  for (const auto& block : blocks) {
    if (block.kind != 0) continue;
    const auto lines = markdown_lines(block.text);
    for (std::size_t index = 0; index < lines.size();) {
      if (trim_horizontal(lines[index]).empty()) { ++index; continue; }
      auto paragraph_end = index + 1;
      while (paragraph_end < lines.size() && !trim_horizontal(lines[paragraph_end]).empty()) ++paragraph_end;
      const auto paragraph_begin = index;
      while (index < paragraph_end && reference_start(lines[index])) {
        // 每段只尝试一次完整定义段；邻接正文时再尝试一至两行，避免反复解析长段落。
        const std::size_t ends[] = {
            index == paragraph_begin ? paragraph_end : index + 1,
            std::min(index + 2, paragraph_end), index + 1};
        bool accepted = false;
        for (const auto end : ends) {
          std::string candidate;
          for (auto line = index; line < end; ++line) {
            candidate.append(lines[line]);
            candidate.push_back('\n');
          }
          auto safe = changelog_safe_markdown(candidate);
          if (!safe) continue;
          const auto parsed = slint::StyledText::from_markdown(*safe);
          // 只导出经同一解析器确认不产生可见文本的定义，绝不吞掉正文。
          if (!parsed || !(*parsed == empty)) continue;
          context.append(*safe);
          context.push_back('\n');
          index = end;
          accepted = true;
          break;
        }
        if (!accepted) break;
      }
      index = paragraph_end;
    }
  }
  return context;
}

inline std::string_view code_font_family() {
#ifdef _WIN32
  return "Consolas";
#else
  return "DejaVu Sans Mono";
#endif
}

}  // namespace changelog_detail

inline std::shared_ptr<slint::Model<ChangelogBlockRow>> changelog_block_rows(
    std::string_view source) {
  const auto blocks = changelog_blocks(source);
  const auto references = changelog_detail::reference_context(blocks);
  std::vector<ChangelogBlockRow> result;
  result.reserve(blocks.size());
  const slint::StyledText empty;
  for (const auto& block : blocks) {
    auto text = slint::StyledText::from_plain_text(block.text);
    if (block.kind != 7) {
      if (auto safe = changelog_safe_markdown(block.text)) {
        // 全文定义在前，跨标题的引用与重复定义均遵守首次定义优先。
        if (auto parsed = slint::StyledText::from_markdown(references + "\n" + *safe)) {
          if (*parsed == empty) continue;
          text = std::move(*parsed);
        }
      }
    }
    result.push_back(ChangelogBlockRow{
        .kind = block.kind,
        .text = std::move(text),
        .plain_text = slint::SharedString{block.text},
        .font_family = slint::SharedString{block.kind == 7 ? changelog_detail::code_font_family()
                                                        : std::string_view{}}});
  }
  return std::make_shared<slint::VectorModel<ChangelogBlockRow>>(std::move(result));
}

inline std::vector<UpdateHistoryRow> changelog_history_rows(
    const awj::update::Manifest& manifest,
    std::vector<ChangelogHistoryEntry> embedded = embedded_changelog_history()) {
  std::vector<ChangelogHistoryEntry> verified;
  verified.reserve(manifest.entries.size());
  for (const auto& entry : manifest.entries) {
    verified.push_back(ChangelogHistoryEntry{
        .version = awj::update::to_string(entry.version),
        .channel = std::string{awj::update::channel_name(entry.channel)},
        .published_at = entry.published_at,
        .release_url = entry.release_url,
        .changelog_zh_cn = entry.changelog.zh_cn,
        .changelog_en = entry.changelog.en});
  }
  const auto history = changelog_display_history(
      std::move(embedded), verified, [](const auto& lhs, const auto& rhs) {
        const auto left = awj::update::parse_version(lhs.version);
        const auto right = awj::update::parse_version(rhs.version);
        if (left && right) return *left > *right;
        if (static_cast<bool>(left) != static_cast<bool>(right)) return !!left;
        return lhs.version > rhs.version;
      });
  std::vector<UpdateHistoryRow> result;
  result.reserve(history.size());
  for (const auto& entry : history) {
    const auto& chinese = entry.changelog_zh_cn.empty() ? entry.changelog_en
                                                      : entry.changelog_zh_cn;
    const auto& english = entry.changelog_en.empty() ? chinese : entry.changelog_en;
    result.push_back(UpdateHistoryRow{
        .version = slint::SharedString(entry.version),
        .channel = slint::SharedString(entry.channel),
        .published_at = slint::SharedString(entry.published_at),
        .release_url = slint::SharedString(entry.release_url),
        .changelog_zh_cn = slint::SharedString(chinese),
        .changelog_en = slint::SharedString(english),
        .blocks_zh_cn = changelog_block_rows(chinese),
        .blocks_en = changelog_block_rows(english)});
  }
  return result;
}

}  // namespace awj::ui
