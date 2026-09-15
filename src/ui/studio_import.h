#pragma once

// 从路径或导入结果把文件加入队列：去重键、目录扫描、大图手动添加。
// 从 main.cpp 拆出。

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "awj_studio.h"
#include "import_service.h"
#include "studio_state.h"

import awj.large_image_plan;

namespace awj::studio {

awj::LargeImageDecision manual_large_image_decision(
    awj::ImageDimensions dimensions, bool grid_available);
std::expected<void, std::string> add_manual_large_image_path(
    UiState& state, const std::filesystem::path& path, bool allow_wic_fallback,
    bool grid_available);
bool output_dir_is_empty(const AwjStudio& app);
void set_input_path_preserving_output(AwjStudio& app,
                                      const std::filesystem::path& path);

struct LargeImageManualAvailability {
  bool grid{};
};
LargeImageManualAvailability large_image_manual_availability();

std::expected<std::vector<std::filesystem::path>, std::string>
supported_files_in_folder(const std::filesystem::path& folder);

bool queue_contains_path(const UiState& state,
                         const std::filesystem::path& path);
std::filesystem::path queue_relative_dir_for(
    const std::filesystem::path& root, const std::filesystem::path& path);

std::expected<bool, std::string> append_queue_image_path(
    UiState& state, const std::filesystem::path& path,
    const std::filesystem::path& source_root);
std::expected<bool, std::string> append_prepared_import_file(
    UiState& state, const awj::ui_import::File& file);
bool add_queue_from_path(AwjStudio& app, UiState& state,
                         const std::filesystem::path& path,
                         bool allow_wic_fallback);
void add_manual_large_images_from_picker(AwjStudio& app, UiState& state,
                                         const std::filesystem::path& picked,
                                         bool folder);

}  // namespace awj::studio
