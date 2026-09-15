#pragma once

// 参数设置页的格式索引映射、参数捕获/应用与预设缓冲。
// 从 main.cpp 拆出；与 studio_menu_params 一起构成参数 UI 层。

#include <array>
#include <vector>
#include <expected>
#include <string>

#include "awj_studio.h"
#include "studio_state.h"

namespace awj::studio {

struct QueueFormatChoice {
  int format_index{};
  bool append_png_suffix{};
};

// 队列下拉索引 → 输出格式 + 是否追加 .png 后缀。constexpr 内联，
// 供 static_assert 与调用方共用同一份定义。
constexpr QueueFormatChoice queue_format_choice_from_index(int index) noexcept {
  const int choice = index < 0 ? 0 : (index > 5 ? 5 : index);
  if (choice == 1) {
    return {.format_index = 0, .append_png_suffix = true};
  }
  return {.format_index = choice == 0 ? 0 : choice - 1,
          .append_png_suffix = false};
}
std::vector<ComboOption> avif_encoder_options();
void refresh_avif_encoder_options(AwjStudio& app);

awj::OutputFormat output_format_from_index(int index);
int parameter_index_from_output_format(awj::OutputFormat format) noexcept;
int parameter_editor_format_index(int index) noexcept;

std::expected<awj::AppConfig, std::string> config_from_menu_params(
    awj::OutputFormat format, const MenuFormatParams& params);
std::expected<void, std::string> validate_menu_params(
    const std::array<MenuFormatParams, 5>& params);
MenuFormatParams default_menu_params_for_index(int index);
ParameterFormatParams default_parameter_params_for_index(int index);

ParameterFormatParams capture_parameter_params_from_ui(const AwjStudio& app);
void apply_parameter_params_to_ui(AwjStudio& app,
                                  const ParameterFormatParams& params,
                                  int format_index);
std::array<ParameterFormatParams, 5>& active_parameter_params(UiState& state);
const std::array<ParameterFormatParams, 5>& active_parameter_params(
    const UiState& state);
void store_current_parameter_params(AwjStudio& app, UiState& state);

}  // namespace awj::studio
