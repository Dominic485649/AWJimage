#pragma once

// 队列预设只保存已验证目录清单中的 UTF-8 文件名，不把配置文本当作路径加载。
// 索引仅是当前下拉列表的投影；文件名才是跨排序、语言和显示名修改的身份。

#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>

import awj.preset;

namespace awj::studio {

[[nodiscard]] inline std::string user_preset_file_name(
    const std::filesystem::path& source_path) {
  const auto filename = source_path.filename().u8string();
  return {filename.begin(), filename.end()};
}

[[nodiscard]] inline int queue_preset_index_for_file(
    std::span<const awj::UserPreset> presets, std::string_view filename) {
  // 配置值不做路径解析或 filename() 裁剪，只原样匹配有效清单中的 basename。
  // POSIX 文件名中的冒号或反斜杠也仅是身份文本，绝不会据此另行打开路径。
  if (filename.empty() || filename == "." || filename == ".." ||
      filename.find('/') != std::string_view::npos ||
      filename.find('\0') != std::string_view::npos) {
    return 0;
  }
  int selected = 0;
  for (std::size_t i = 0; i < presets.size(); ++i) {
    if (user_preset_file_name(presets[i].source_path) != filename) continue;
    if (selected != 0 || i >= static_cast<std::size_t>(
                                  std::numeric_limits<int>::max())) {
      return 0;
    }
    selected = static_cast<int>(i) + 1;
  }
  return selected;
}

[[nodiscard]] inline std::string queue_preset_file_for_index(
    std::span<const awj::UserPreset> presets, int index) {
  if (index <= 0 || static_cast<std::size_t>(index) > presets.size()) return {};
  auto filename = user_preset_file_name(
      presets[static_cast<std::size_t>(index - 1)].source_path);
  // 反向映射也执行同一校验，避免把不可恢复的身份写入配置。
  return queue_preset_index_for_file(presets, filename) == index
             ? filename
             : std::string{};
}

}  // namespace awj::studio
