#include "studio_import.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "studio_encode_dispatch.h"
#include "studio_shell_cli.h"
#include "studio_fields.h"
#include "studio_queue_rows.h"
#include "studio_ui_util.h"
#include "studio_worker_control.h"

import awj.avif_aom_codec;
import awj.core;
import awj.decoder_registry;
import awj.encoding_defaults;

namespace awj::studio {

awj::LargeImageDecision manual_large_image_decision(
    awj::ImageDimensions dimensions, bool grid_available) {
  auto decision =
      awj::classify_large_image(dimensions, grid_available);
  decision.klass = awj::LargeImageClass::large_mode_required;
  if (decision.reason == awj::LargeImageReason::none) {
    decision.reason_text = "用户手动加入大图队列。";
  }
  return decision;
}


std::expected<void, std::string> add_manual_large_image_path(
    UiState& state, const std::filesystem::path& path, bool allow_wic_fallback,
    bool grid_available) {
  try {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      return std::unexpected{std::format("不是可添加的文件: {}。",
                                         awj::display_path_for_user(path))};
    }
    if (!awj::is_supported_image_extension(path)) {
      return std::unexpected{std::format("文件格式不受支持: {}。支持 {}。",
                                         awj::display_path_for_user(path),
                                         awj::kSupportedImageExtensionsText)};
    }
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec) {
      return std::unexpected{std::format("读取文件大小失败: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
    const auto dimensions = awj::probe_image_dimensions_for_path(
        path,
        awj::DecoderRegistryOptions{.allow_wic_fallback = allow_wic_fallback});
    if (!dimensions) {
      return std::unexpected{dimensions.error()};
    }
    const auto index = state.large_image_items.size();
    awj::ImageFile file{.index = index, .path = path, .bytes = bytes};
    auto decision = manual_large_image_decision(*dimensions, grid_available);
    auto item = awj::BatchLargeImageItem{.file = std::move(file),
                                         .dimensions = *dimensions,
                                         .decision = std::move(decision)};
    auto row = make_large_image_row(item, "等待选择");
    state.large_image_items.push_back(std::move(item));
    state.large_image_rows->push_back(std::move(row));
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"添加大图任务时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"添加大图任务时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"添加大图任务时文件系统访问失败。"};
  }
}
bool output_dir_is_empty(const AwjStudio& app) {
  return trim_copy(shared_to_string(app.get_output_dir())).empty();
}

void set_input_path_preserving_output(AwjStudio& app,
                                      const std::filesystem::path& path) {
  const bool should_fill_output = output_dir_is_empty(app);
  app.set_input_path(to_shared(awj::path_to_utf8(path)));
  if (should_fill_output) {
    app.set_output_dir(
        to_shared(awj::path_to_utf8(awj::default_output_dir_for(path))));
  }
}

LargeImageManualAvailability large_image_manual_availability() {
  const auto capabilities =
      awj::avif_encoder_capabilities_for_current_build();
  LargeImageManualAvailability result{};
  for (const auto& capability : capabilities) {
    const bool enabled = capability.enabled;
    if (!enabled) {
      continue;
    }
    if (capability.mode == awj::AvifEncoderMode::aom &&
        capability.supports_avif_grid) {
      result.grid = true;
    }

  }
  return result;
}

std::expected<std::vector<std::filesystem::path>, std::string>
supported_files_in_folder(const std::filesystem::path& folder) {
  try {
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it{
        folder, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) {
      return std::unexpected{std::format("扫描文件夹失败: {}；系统错误：{}。",
                                         awj::display_path_for_user(folder),
                                         ec.message())};
    }
    for (std::filesystem::recursive_directory_iterator end; it != end;
         it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      if (!it->is_regular_file(ec) || ec) {
        ec.clear();
        continue;
      }
      if (awj::is_supported_image_extension(it->path())) {
        paths.push_back(it->path());
      }
    }
    const auto path_key = [](const std::filesystem::path& path) {
      std::error_code key_ec;
      const auto absolute = std::filesystem::absolute(path, key_ec);
      return awj::normalized_lower_path_key(key_ec ? path : absolute);
    };
    std::ranges::sort(paths, [&](const auto& left, const auto& right) {
      const auto left_key = path_key(left);
      const auto right_key = path_key(right);
      return left_key == right_key ? left.wstring() < right.wstring()
                                   : left_key < right_key;
    });
    const auto duplicate = std::ranges::unique(paths, {}, path_key);
    paths.erase(duplicate.begin(), duplicate.end());
    return paths;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"扫描文件夹时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"扫描文件夹时文件数量超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"扫描文件夹时文件系统访问失败。"};
  }
}

bool queue_contains_path(const UiState& state,
                         const std::filesystem::path& path) {
  return state.queue_path_keys.contains(queue_path_key(path));
}

std::filesystem::path queue_relative_dir_for(
    const std::filesystem::path& root, const std::filesystem::path& path) {
  std::error_code ec;
  const auto relative = std::filesystem::relative(path.parent_path(), root, ec);
  if (ec || relative.empty() || relative == L".") {
    return {};
  }
  return relative;
}

std::expected<bool, std::string> append_queue_image_path(
    UiState& state, const std::filesystem::path& path,
    const std::filesystem::path& source_root) {
  try {
    const auto key = queue_path_key(path);
    if (state.queue_path_keys.contains(key)) {
      return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      return std::unexpected{std::format("不是可加入队列的文件: {}。",
                                         awj::display_path_for_user(path))};
    }
    if (!awj::is_supported_image_extension(path)) {
      return std::unexpected{std::format("文件格式不受支持: {}。支持 {}。",
                                         awj::display_path_for_user(path),
                                         awj::kSupportedImageExtensionsText)};
    }
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec) {
      return std::unexpected{std::format("读取文件大小失败: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
    if (bytes > static_cast<std::uintmax_t>(
                    awj::encoding_defaults::effective_max_input_file_bytes())) {
      return std::unexpected{std::format("输入文件超过当前输入上限: {}。",
                                         awj::display_path_for_user(path))};
    }
    QueueImageItem item{
        .id = state.next_queue_id,
        .path = path,
        .source_root = source_root,
        .relative_dir =
            source_root.empty() ? std::filesystem::path{}
                                : queue_relative_dir_for(source_root, path),
        .bytes = bytes};
    const auto [position, inserted] = state.queue_path_keys.insert(key);
    if (!inserted) return false;
    try {
      state.queue_items.push_back(std::move(item));
    } catch (...) {
      state.queue_path_keys.erase(position);
      throw;
    }
    ++state.next_queue_id;
    return true;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"添加队列项时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"添加队列项时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"添加队列项时文件系统访问失败。"};
  }
}

std::expected<bool, std::string> append_prepared_import_file(
    UiState& state, const awj::ui_import::File& file) {
  try {
    const auto key = queue_path_key(file.path);
    if (state.queue_path_keys.contains(key)) return false;
    QueueImageItem item{
        .id = state.next_queue_id,
        .path = file.path,
        .source_root = file.source_root,
        .relative_dir = file.source_root.empty()
                            ? std::filesystem::path{}
                            : queue_relative_dir_for(file.source_root, file.path),
        .bytes = file.bytes};
    const auto [position, inserted] = state.queue_path_keys.insert(key);
    if (!inserted) return false;
    try {
      state.queue_items.push_back(std::move(item));
    } catch (...) {
      state.queue_path_keys.erase(position);
      throw;
    }
    ++state.next_queue_id;
    return true;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"添加导入结果时内存不足。"};
  }
}

bool add_queue_from_path(AwjStudio& app, UiState& state,
                         const std::filesystem::path& picked,
                         bool pick_folder, bool update_input_path = true) {
  std::error_code ec;
  const bool is_folder = pick_folder ||
                         (std::filesystem::is_directory(picked, ec) && !ec);
  std::vector<std::filesystem::path> paths;
  if (is_folder) {
    auto scanned = supported_files_in_folder(picked);
    if (!scanned) {
      app.set_status_text(to_shared(scanned.error()));
      return false;
    }
    paths = std::move(*scanned);
    if (paths.empty()) {
      app.set_status_text(to_shared("文件夹中没有支持的图片文件。"));
      return false;
    }
  } else {
    paths.push_back(picked);
  }

  std::size_t added = 0;
  std::size_t skipped = 0;
  std::size_t failed = 0;
  std::string first_error;
  const auto source_root = is_folder ? picked : std::filesystem::path{};
  bool accepted_target = false;
  for (const auto& path : paths) {
    auto appended = append_queue_image_path(state, path, source_root);
    if (appended && *appended) {
      ++added;
      accepted_target = true;
    } else if (appended) {
      ++skipped;
      accepted_target = true;
    } else {
      ++failed;
      if (first_error.empty()) {
        first_error = appended.error();
      }
    }
  }
  if (update_input_path && accepted_target) {
    set_input_path_preserving_output(app, picked);
  }
  refresh_queue_rows(app, state);
  if (added == 0) {
    app.set_status_text(
        to_shared(first_error.empty()
                      ? std::format("没有新图片加入队列{}。",
                                    skipped > 0 ? "，重复项已跳过" : "")
                      : first_error));
    return accepted_target;
  }
  app.set_status_text(to_shared(std::format(
      "已加入 {} 张图片{}{}。", added,
      skipped == 0 ? "" : std::format("，跳过 {} 个重复项", skipped),
      failed == 0 ? "" : std::format("，{} 个失败", failed))));
  return accepted_target;
}

void add_manual_large_images_from_picker(AwjStudio& app, UiState& state,
                                         const std::filesystem::path& picked,
                                         bool folder) {
  const auto availability =
      large_image_manual_availability();
  const bool allow_wic_fallback = app.get_allow_wic_fallback();
  std::vector<std::filesystem::path> paths;
  if (folder) {
    auto scanned = supported_files_in_folder(picked);
    if (!scanned) {
      app.set_status_text(to_shared(scanned.error()));
      return;
    }
    paths = std::move(*scanned);
    if (paths.empty()) {
      app.set_status_text(to_shared("文件夹中没有支持的图片文件。"));
      return;
    }
  } else {
    paths.push_back(picked);
  }

  const bool was_empty = state.large_image_items.empty();
  std::size_t added = 0;
  std::size_t failed = 0;
  std::string first_error;
  for (const auto& path : paths) {
    auto result =
        add_manual_large_image_path(state, path, allow_wic_fallback,
                                    availability.grid);
    if (result) {
      ++added;
    } else {
      ++failed;
      if (first_error.empty()) {
        first_error = result.error();
      }
    }
  }
  if (was_empty && added > 0) {
    app.set_selected_large_image_index(0);
  }
  if (added == 0) {
    app.set_status_text(
        to_shared(first_error.empty() ? "未添加任何大图任务。" : first_error));
    return;
  }
  app.set_status_text(to_shared(
      std::format("已添加 {} 个大图任务{}。", added,
                  failed == 0 ? "" : std::format("，{} 个失败", failed))));
}


}  // namespace awj::studio
