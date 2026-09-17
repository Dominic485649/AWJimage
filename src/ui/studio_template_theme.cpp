#include "studio_template_theme.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <dwmapi.h>
#include <windows.h>

#include <cstdint>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#include "studio_config.h"
#include "studio_json.h"
#include "studio_state.h"

import awj.core;
import awj.encoding_defaults;
import awj.studio_defaults;

namespace awj::studio {

bool template_contains_token(std::string_view text, std::string_view token) {
  return text.find(token) != std::string_view::npos;
}

void sync_template_flags(AwjStudio& app) {
  const auto text = shared_to_string(app.get_template_text());
  app.set_template_params_selected(template_contains_token(text, "{params}"));
  app.set_template_date_selected(template_contains_token(text, "{date}"));
  app.set_template_time_selected(template_contains_token(text, "{time}"));
  app.set_template_rand_selected(template_contains_token(text, "{rand}"));
  app.set_template_hash8_selected(template_contains_token(text, "{hash8}"));
  app.set_template_sha2568_selected(
      template_contains_token(text, "{sha2568}") ||
      template_contains_token(text, "{sha256_8}"));
}

std::string token_with_separator(std::string_view text,
                                 std::string_view token) {
  if (text.empty()) {
    return std::string{token};
  }
  const char last = text.back();
  const bool needs_separator =
      last != '_' && last != '-' && last != ' ' && last != '.';
  return std::format("{}{}{}", text, needs_separator ? "_" : "", token);
}

void toggle_template_token(AwjStudio& app, std::string_view token) {
  auto text = shared_to_string(app.get_template_text());
  const auto pos = text.find(token);
  if (pos != std::string::npos) {
    text.erase(pos, token.size());
    if (pos > 0 && text[pos - 1] == '_') {
      text.erase(pos - 1, 1);
    } else if (pos < text.size() && text[pos] == '_') {
      text.erase(pos, 1);
    }
    if (text.empty()) {
      text = std::string{awj::encoding_defaults::default_output_template_text};
    }
  } else {
    text = token_with_separator(text, token);
  }
  app.set_template_text(to_shared(text));
  sync_template_flags(app);
}

bool windows_prefers_dark_mode() {
  DWORD apps_use_light_theme = 1;
  DWORD value_size = sizeof(apps_use_light_theme);
  const auto status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &apps_use_light_theme,
      &value_size);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  return apps_use_light_theme == 0;
}

// 把界面语言切到 language_index 指定的语言。
//
// 0 = 中文，也就是 .slint 里 @tr() 的 msgid 原文和 bundled 默认语言；
// 1 = English，对应 ui/translations/en/LC_MESSAGES/awj.po 这份 bundled 翻译。
//
// select_bundled_translation 写的是 translations_dirty 这个真实的 Slint 属性，
// 所以每个 @tr() 绑定都会失效并在下一帧重算——不需要重启，也不需要自己去
// 逐个属性重新赋值。两点约束：必须在第一个组件创建之后才能调用，否则拿不到
// bundle；传空串会明确回到 bundled 默认语言（中文 msgid）。
//
// 返回值刻意忽略：翻译缺失时回退到中文原文，是可接受的降级，不该阻断设置操作。
void apply_ui_language(int language_index) noexcept {
  try {
    static_cast<void>(
        slint::select_bundled_translation(language_index == 1 ? "en" : ""));
  } catch (...) {
  }
}

bool effective_studio_dark_mode(const AwjStudio& app) {
  return app.get_theme_index() == 2 ||
         (app.get_theme_index() == 0 && app.get_system_dark_mode());
}

bool shell_window_dark_mode() {
  int theme_index = 0;
  const auto path = studio_config_path();
  if (!path.empty()) {
    std::ifstream input{path, std::ios::binary};
    if (input) {
      std::string source{std::istreambuf_iterator<char>{input}, {}};
      if (auto values = awj::studio_json::parse_jsonc_config(source)) {
        if (auto value = awj::studio_json::config_int(*values, "theme_index", 0, 2)) {
          theme_index = *value;
        }
      }
    }
  }
  return theme_index == 2 || (theme_index == 0 && windows_prefers_dark_mode());
}

void apply_title_bar_theme(slint::Window& window, bool dark_mode) noexcept {
  try {
    const HWND hwnd = window.win32_hwnd();
    if (hwnd == nullptr) {
      return;
    }
    const BOOL use_dark_mode = dark_mode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark_mode,
                          sizeof(use_dark_mode));
    // Keep the native caption in sync with StudioTheme.nav-bg.
    const COLORREF caption_color = dark_mode ? RGB(36, 36, 36) : RGB(248, 251, 255);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption_color,
                          sizeof(caption_color));
  } catch (...) {
  }
}



}  // namespace awj::studio
