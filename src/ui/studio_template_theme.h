#pragma once

// 输出模板 token 判定/切换、界面语言与深浅主题判定、标题栏主题。
// 从 main.cpp 拆出。

#include <string>
#include <string_view>

#include "awj_studio.h"

namespace awj::studio {

bool template_contains_token(std::string_view text, std::string_view token);
void sync_template_flags(AwjStudio& app);
void toggle_template_token(AwjStudio& app, std::string_view token);
void apply_ui_language(int language_index) noexcept;
bool effective_studio_dark_mode(const AwjStudio& app);
void apply_title_bar_theme(slint::Window& window, bool dark_mode) noexcept;
bool windows_prefers_dark_mode();
bool shell_window_dark_mode();

}  // namespace awj::studio
