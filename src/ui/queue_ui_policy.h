#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace awj::studio {

// Keep the persisted 0..3 indices unchanged; numbering is the appended option.
inline constexpr int default_queue_collision_index = 4;

struct QueueRunOptions {
  int collision_index{default_queue_collision_index};
  bool write_log{};
  bool delete_source{};

  bool operator==(const QueueRunOptions&) const = default;
};

constexpr QueueRunOptions queue_options_from_config(
    std::optional<int> collision_index = {},
    std::optional<bool> write_log = {},
    std::optional<bool> legacy_write_summary = {},
    std::optional<bool> delete_source = {}) noexcept {
  const int index = collision_index.value_or(default_queue_collision_index);
  return {.collision_index = index >= 0 && index <= 4
                                 ? index : default_queue_collision_index,
          .write_log = write_log.value_or(false) ||
                       legacy_write_summary.value_or(false),
          // Recycling is opt-in, never inferred from either legacy log flag.
          .delete_source = delete_source.value_or(false)};
}

constexpr std::wstring_view queue_collision_argument(int index) noexcept {
  switch (index) {
    case 0: return L"overwrite";
    case 1: return L"skip";
    case 2: return L"time";
    case 3: return L"random";
    case 4:
    default: return L"number";
  }
}

// queue_page.slint: button 0 = left, 1 = right, 2 = other/none;
// kind 0 = down, 1 = up, 2 = move, 3 = cancel, 4 = actual drag.
// Slint sends no-button hover moves as Other/Move between click releases.
constexpr bool queue_pointer_resets_double_click(int button, int kind) noexcept {
  return kind == 3 || kind == 4 ||
         (button != 0 && (kind == 0 || kind == 1));
}

// Only a confirmed successful encode may open an output. In particular, do not
// fall back to a predicted name (or to a recycled source) when success has no
// final path. Filesystem existence is checked by the platform opener.
inline const std::filesystem::path& queue_image_open_target(
    const std::filesystem::path& source,
    const std::filesystem::path& final_output,
    bool encode_succeeded) noexcept {
  return encode_succeeded ? final_output : source;
}

}  // namespace awj::studio
