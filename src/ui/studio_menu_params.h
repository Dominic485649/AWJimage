#pragma once

// 菜单参数页的 UI ↔ 快照转换。从 main.cpp 拆出。

#include <array>

#include "awj_studio.h"
#include "studio_state.h"

namespace awj::studio {

MenuFormatParams capture_menu_params_from_ui(const AwjStudio& app);
void apply_menu_params_to_ui(AwjStudio& app, const MenuFormatParams& params);
void store_current_menu_params(AwjStudio& app, UiState& state);
void load_menu_params_for_index(AwjStudio& app, UiState& state, int index);
std::array<MenuFormatParams, 5> menu_params_snapshot(const AwjStudio& app,
                                                     const UiState* state);

}  // namespace awj::studio
