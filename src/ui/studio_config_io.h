#pragma once

// Studio 配置文件的读取与应用、配置快照捕获与变更持久化。
// 从 main.cpp 拆出。

#include <expected>
#include <string>

#include "awj_studio.h"
#include "studio_state.h"

namespace awj::studio {

std::pair<int, int> current_studio_window_size(const AwjStudio& app) noexcept;
StudioConfigSnapshot capture_studio_config(const AwjStudio& app,
                                           const UiState* state = nullptr);
std::expected<void, std::string> apply_studio_config_file(AwjStudio& app,
                                                          UiState& state);
std::expected<void, std::string> persist_studio_config_if_changed(
    AwjStudio& app, UiState& state);

}  // namespace awj::studio
