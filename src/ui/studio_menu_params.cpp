#include "studio_menu_params.h"

#include <algorithm>
#include <cstddef>
#include <string>

import awj.core;

namespace awj::studio {

MenuFormatParams capture_menu_params_from_ui(const AwjStudio& app) {
  return MenuFormatParams{.quality_text = shared_to_string(app.get_menu_quality_text()),
                          .bit_depth_text = shared_to_string(app.get_menu_bit_depth_text()),
                          .speed_text = shared_to_string(app.get_menu_speed_text()),
                          .avif_encoder_index = app.get_menu_avif_encoder_index(),
                          .avif_color_representation_index =
                              app.get_menu_avif_color_representation_index(),
                          .chroma_index = app.get_menu_chroma_index(),
                          .alpha_policy_index = app.get_menu_alpha_policy_index(),
                          .jpegli_progressive_index = app.get_menu_jpegli_progressive_index(),
                          .jpegli_optimize_huffman = app.get_menu_jpegli_optimize_huffman(),
                          .jpegli_xyb = app.get_menu_jpegli_xyb(),
                          .jxl_jpeg_lossless = app.get_menu_jxl_jpeg_lossless(),
                          .strip_metadata = app.get_menu_strip_metadata(),
                          .allow_wic_fallback = app.get_menu_allow_wic_fallback(),
                          .close_on_finish = app.get_menu_close_on_finish(),
                          .install_avif_png_command =
                              app.get_menu_install_avif_png_command(),
                          .size_limit_index = app.get_menu_size_limit_index(),
                          .max_width_text = shared_to_string(app.get_menu_max_width_text()),
                          .max_height_text = shared_to_string(app.get_menu_max_height_text()),
                          .max_long_edge_text = shared_to_string(app.get_menu_max_long_edge_text()),
                          .max_short_edge_text = shared_to_string(app.get_menu_max_short_edge_text()),
                          .scale_percent_text = shared_to_string(app.get_menu_scale_percent_text())};
}

void apply_menu_params_to_ui(AwjStudio& app, const MenuFormatParams& params) {
  app.set_menu_quality_text(to_shared(params.quality_text));
  app.set_menu_bit_depth_text(to_shared(params.bit_depth_text));
  app.set_menu_speed_text(to_shared(params.speed_text));
  app.set_menu_avif_encoder_index(params.avif_encoder_index);
  app.set_menu_avif_color_representation_index(
      params.avif_color_representation_index);
  app.set_menu_chroma_index(params.chroma_index);
  app.set_menu_alpha_policy_index(params.alpha_policy_index);
  app.set_menu_jpegli_progressive_index(params.jpegli_progressive_index);
  app.set_menu_jpegli_optimize_huffman(params.jpegli_progressive_index > 0 || params.jpegli_optimize_huffman);
  app.set_menu_jpegli_xyb(params.jpegli_xyb);
  app.set_menu_jxl_jpeg_lossless(params.jxl_jpeg_lossless);
  app.set_menu_strip_metadata(params.strip_metadata);
  app.set_menu_allow_wic_fallback(params.allow_wic_fallback);
  app.set_menu_close_on_finish(params.close_on_finish);
  app.set_menu_install_avif_png_command(params.install_avif_png_command);
  app.set_menu_size_limit_index(params.size_limit_index);
  app.set_menu_max_width_text(to_shared(params.max_width_text));
  app.set_menu_max_height_text(to_shared(params.max_height_text));
  app.set_menu_max_long_edge_text(to_shared(params.max_long_edge_text));
  app.set_menu_max_short_edge_text(to_shared(params.max_short_edge_text));
  app.set_menu_scale_percent_text(to_shared(params.scale_percent_text));
}

void store_current_menu_params(AwjStudio& app, UiState& state) {
  const int index = std::clamp(state.last_menu_format_index, 0, 4);
  state.menu_params[static_cast<std::size_t>(index)] = capture_menu_params_from_ui(app);
}

void load_menu_params_for_index(AwjStudio& app, UiState& state, int index) {
  index = std::clamp(index, 0, 4);
  state.last_menu_format_index = index;
  apply_menu_params_to_ui(app, state.menu_params[static_cast<std::size_t>(index)]);
}

std::array<MenuFormatParams, 5> menu_params_snapshot(const AwjStudio& app,
                                                     const UiState* state) {
  std::array<MenuFormatParams, 5> params{};
  if (state != nullptr) {
    params = state->menu_params;
  }
  const int index = std::clamp(state != nullptr ? state->last_menu_format_index
                                                : app.get_menu_format_index(),
                               0, 4);
  params[static_cast<std::size_t>(index)] = capture_menu_params_from_ui(app);
  return params;
}

}  // namespace awj::studio
