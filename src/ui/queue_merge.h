#pragma once

#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace awj::ui_queue {

using PathKey = std::filesystem::path::string_type;
inline constexpr auto no_index = std::numeric_limits<std::size_t>::max();

inline PathKey path_key(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  auto key = (ec ? path : absolute).lexically_normal().native();
#ifdef _WIN32
  for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
#endif
  return key;
}

struct MergeEntry {
  std::size_t previous{no_index};
  std::size_t incoming{no_index};
};

struct MergePlan {
  std::vector<MergeEntry> entries;
  std::size_t added{};
  std::size_t requeued{};
  std::size_t folded{};
};

// 只规划位置，不修改任何模型。调用方准备成功后再提交，路径归一化各做一次。
// 旧条目保持相对顺序；命中项按本批最后一次出现的位置追加，稳定身份由调用方保留。
template<class PreviousKey, class IncomingKey>
MergePlan plan_merge(std::size_t previous_count, std::size_t incoming_count,
                     PreviousKey previous_key, IncomingKey incoming_key) {
  if (previous_count > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      incoming_count > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::length_error{"队列数量超过界面索引范围。"};
  std::unordered_map<PathKey, std::size_t> previous, latest;
  previous.reserve(previous_count);
  latest.reserve(incoming_count);
  std::vector<PathKey> previous_keys, incoming_keys;
  previous_keys.reserve(previous_count);
  incoming_keys.reserve(incoming_count);
  for (std::size_t i = 0; i < previous_count; ++i) {
    previous_keys.push_back(previous_key(i));
    previous.insert_or_assign(previous_keys.back(), i);
  }
  for (std::size_t i = 0; i < incoming_count; ++i) {
    incoming_keys.push_back(incoming_key(i));
    latest.insert_or_assign(incoming_keys.back(), i);
  }
  MergePlan result;
  result.folded = incoming_count - latest.size();
  const auto capacity = previous_count + latest.size();
  if (capacity > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::length_error{"队列数量超过界面索引范围。"};
  result.entries.reserve(capacity);
  for (std::size_t i = 0; i < previous_count; ++i) {
    const auto& key = previous_keys[i];
    if (!latest.contains(key) && previous.at(key) == i)
      result.entries.push_back({i, no_index});
  }
  for (std::size_t i = 0; i < incoming_count; ++i) {
    const auto& key = incoming_keys[i];
    if (latest.at(key) != i) continue;
    const auto found = previous.find(key);
    const auto old = found == previous.end() ? no_index : found->second;
    result.entries.push_back({old, i});
    if (old == no_index) ++result.added;
    else ++result.requeued;
  }
  return result;
}

}  // namespace awj::ui_queue
