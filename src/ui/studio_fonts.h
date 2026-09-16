#pragma once

// 系统 UI 字体探测与字体列表加载（Windows）。
// 从 main.cpp 拆出；回调函数在本模块内部使用。

#include "studio_state.h"

namespace awj::studio {

std::string select_system_ui_font_family();
void apply_system_ui_font(AwjStudio& app);
void load_system_font_options(AwjStudio& app);

}  // namespace awj::studio
