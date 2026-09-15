#pragma once

// 预设与参数联动：预设下拉刷新、参数 ↔ AppConfig 转换、参数页默认值，
// 以及从 UI 采集最终 AppConfig（config_from_ui）。从 main.cpp 拆出。

#include <expected>
#include <filesystem>
#include <string>

#include "awj_studio.h"
#include "studio_state.h"

import awj.config;
import awj.preset;

namespace awj::studio {

awj::CollisionMode collision_from_index(int index);
void apply_format_defaults_to_ui(AwjStudio& app, int format_index,
                                 UiState& state);
void initialize_ui_defaults(AwjStudio& app, UiState& state);
std::expected<awj::AppConfig, std::string> config_from_parameter_params(
    awj::OutputFormat format, const ParameterFormatParams& params);
ParameterFormatParams parameter_params_from_config(const awj::AppConfig& config);
std::array<ParameterFormatParams, 5> parameter_params_from_user_preset(
    const awj::UserPreset& preset);
std::expected<awj::UserPreset, std::string> user_preset_from_parameter_params(
    std::string name, std::string description,
    const std::array<ParameterFormatParams, 5>& params);
void reload_user_preset_options(AwjStudio& app, UiState& state);
void select_parameter_preset(AwjStudio& app, UiState& state, int index);
void select_queue_preset(AwjStudio& app, UiState& state, int index);
std::expected<awj::AppConfig, std::string> config_from_ui(AwjStudio& app,
                                                          UiState& state);
std::filesystem::path effective_output_dir(const AwjStudio& app);

}  // namespace awj::studio
