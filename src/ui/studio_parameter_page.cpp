#include "studio_parameter_page.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>

#include "studio_fields.h"

import awj.avif_aom_codec;
import awj.avif_registry;
import awj.core;
import awj.encoding_defaults;

namespace awj::studio {

awj::OutputFormat output_format_from_index(int index) {
  switch (index) {
    case 1:
      return awj::OutputFormat::webp;
    case 2:
      return awj::OutputFormat::jxl;
    case 3:
      return awj::OutputFormat::jpgli;
    case 4:
      return awj::OutputFormat::png;
    case 0:
    default:
      return awj::OutputFormat::avif;
  }
}

static_assert(queue_format_choice_from_index(0).format_index == 0);
static_assert(queue_format_choice_from_index(1).format_index == 0 &&
              queue_format_choice_from_index(1).append_png_suffix);
static_assert(queue_format_choice_from_index(2).format_index == 1);
static_assert(queue_format_choice_from_index(3).format_index == 2);
static_assert(queue_format_choice_from_index(4).format_index == 3);
static_assert(queue_format_choice_from_index(5).format_index == 4);

awj::ChromaMode chroma_from_index(int index) {
  switch (index) {
    case 1:
      return awj::ChromaMode::yuv444;
    case 2:
      return awj::ChromaMode::yuv422;
    case 3:
      return awj::ChromaMode::yuv420;
    case 0:
    default:
      return awj::ChromaMode::auto_keep;
  }
}

awj::AvifColorRepresentation avif_color_representation_from_index(
    int index) noexcept {
  switch (index) {
    case 1:
      return awj::AvifColorRepresentation::source;
    case 2:
      return awj::AvifColorRepresentation::rgb_identity;
    case 0:
    default:
      return awj::AvifColorRepresentation::yuv;
  }
}

// The parameter-page order differs from OutputFormat's enum order.
int parameter_index_from_output_format(awj::OutputFormat format) noexcept {
  switch (format) {
    case awj::OutputFormat::avif:
      return 0;
    case awj::OutputFormat::webp:
      return 1;
    case awj::OutputFormat::jxl:
      return 2;
    case awj::OutputFormat::jpgli:
      return 3;
    case awj::OutputFormat::png:
    default:
      return 4;
  }
}

int parameter_editor_format_index(int index) noexcept {
  return index >= 0 && index < 5 ? index : 0;
}

awj::AlphaModePolicy alpha_policy_from_index(int index) {
  switch (index) {
    case 0:
      return awj::AlphaModePolicy::force;
    case 2:
      return awj::AlphaModePolicy::off;
    case 1:
    default:
      return awj::AlphaModePolicy::automatic;
  }
}

awj::AvifEncoderMode avif_encoder_from_index(int index) {
  return index == 0 ? awj::AvifEncoderMode::automatic
       : index == 1 ? awj::AvifEncoderMode::aom
                    : static_cast<awj::AvifEncoderMode>(-1);
}

std::expected<awj::AppConfig, std::string> config_from_menu_params(
    awj::OutputFormat format, const MenuFormatParams& params) try {
  awj::AppConfig cfg = awj::default_app_config();
  cfg.output_format = format;
  cfg.output_policy = awj::OutputPolicy::shell;
  cfg.collision_mode = awj::CollisionMode::suffix_number;
  cfg.strip_metadata = params.strip_metadata;
  cfg.allow_wic_fallback = params.allow_wic_fallback;

  const auto quality = parse_quality_field(params.quality_text);
  if (!quality) return std::unexpected{quality.error()};
  cfg.quality = *quality;

  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jpgli || format == awj::OutputFormat::png) {
    const auto bit_depth = parse_bit_depth_field(params.bit_depth_text);
    if (!bit_depth) return std::unexpected{bit_depth.error()};
    cfg.bit_depth = *bit_depth;
  }
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    const auto speed = parse_optional_int_field(params.speed_text, "speed", 0, 10);
    if (!speed) return std::unexpected{speed.error()};
    cfg.speed = *speed;
  }
  if (format == awj::OutputFormat::avif) {
    cfg.avif_encoder = avif_encoder_from_index(params.avif_encoder_index);
    cfg.avif_color_representation = avif_color_representation_from_index(
        params.avif_color_representation_index);
    cfg.chroma_mode = chroma_from_index(params.chroma_index);
    cfg.alpha_policy = alpha_policy_from_index(params.alpha_policy_index);
  } else if (format == awj::OutputFormat::jpgli) {
    cfg.chroma_mode = chroma_from_index(params.chroma_index);
    cfg.jpegli_progressive_level = std::clamp(params.jpegli_progressive_index, 0, 2);
    cfg.jpegli_optimize_huffman = cfg.jpegli_progressive_level > 0
                                      ? true
                                      : params.jpegli_optimize_huffman;
    cfg.jpegli_xyb = params.jpegli_xyb;
  }
  cfg.jxl_jpeg_lossless = params.jxl_jpeg_lossless;
  const auto size_limit = image_size_limit_from_fields(
      params.size_limit_index, params.max_width_text, params.max_height_text,
      params.max_long_edge_text, params.max_short_edge_text, params.scale_percent_text);
  if (!size_limit) return std::unexpected{size_limit.error()};
  cfg.image_size_limit = *size_limit;
  if (auto valid = awj::finalize_config_defaults(cfg, true, false); !valid) {
    return std::unexpected{valid.error()};
  }
  return cfg;
} catch (const std::bad_alloc&) {
  return std::unexpected{"菜单参数解析内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"菜单参数解析数据超过运行时限制。"};
}

std::expected<void, std::string> validate_menu_params(
    const std::array<MenuFormatParams, 5>& params) {
  constexpr std::array<std::string_view, 5> labels{"AVIF", "WebP", "JXL", "JPGLI", "PNG"};
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (auto cfg = config_from_menu_params(output_format_from_index(static_cast<int>(i)), params[i]); !cfg) {
      return std::unexpected{std::format("{} 菜单参数错误：{}", labels[i], cfg.error())};
    }
  }
  return {};
}

MenuFormatParams default_menu_params_for_index(int index) {
  const auto format = output_format_from_index(index);
  MenuFormatParams params{};
  params.quality_text = text_from_int(awj::default_quality_for(format));
  if (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli) {
    params.bit_depth_text = text_from_int(awj::encoding_defaults::default_webp_bit_depth);
  }
  params.jpegli_progressive_index = awj::encoding_defaults::default_jpegli_progressive_level;
  params.jpegli_optimize_huffman = awj::encoding_defaults::default_jpegli_optimize_huffman;
  params.jpegli_xyb = awj::encoding_defaults::default_jpegli_xyb;
  params.jxl_jpeg_lossless = true;
  params.allow_wic_fallback = awj::encoding_defaults::default_allow_wic_fallback;
  params.alpha_policy_index = 1;
  return params;
}

ParameterFormatParams default_parameter_params_for_index(int index) {
  const auto format = output_format_from_index(index);
  ParameterFormatParams params{};
  params.quality_text = text_from_int(awj::default_quality_for(format));
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    params.speed_text = text_from_int(awj::default_speed_for(format));
  }
  if (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli) {
    params.bit_depth_text =
        text_from_int(awj::encoding_defaults::default_webp_bit_depth);
  }
  params.jpegli_progressive_index =
      awj::encoding_defaults::default_jpegli_progressive_level;
  params.jpegli_optimize_huffman =
      awj::encoding_defaults::default_jpegli_optimize_huffman;
  params.jpegli_xyb = awj::encoding_defaults::default_jpegli_xyb;
  params.jxl_jpeg_lossless = true;
  return params;
}

ParameterFormatParams capture_parameter_params_from_ui(const AwjStudio& app) {
  return ParameterFormatParams{
      .quality_text = shared_to_string(app.get_quality_text()),
      .visual_quality_text = shared_to_string(app.get_visual_quality_text()),
      .bit_depth_text = shared_to_string(app.get_bit_depth_text()),
      .speed_text = shared_to_string(app.get_speed_text()),
      .avif_encoder_index = app.get_avif_encoder_index(),
      .avif_color_representation_index =
          app.get_avif_color_representation_index(),
      .chroma_index = app.get_chroma_index(),
      .alpha_policy_index = app.get_alpha_policy_index(),
      .jpegli_progressive_index = app.get_jpegli_progressive_index(),
      .jpegli_optimize_huffman = app.get_jpegli_optimize_huffman(),
      .jpegli_xyb = app.get_jpegli_xyb(),
      .jxl_jpeg_lossless = app.get_jxl_jpeg_lossless(),
      .threads_text = shared_to_string(app.get_threads_text()),
      .memory_limit_text = shared_to_string(app.get_memory_limit_text()),
      .size_limit_index = app.get_size_limit_index(),
      .max_width_text = shared_to_string(app.get_max_width_text()),
      .max_height_text = shared_to_string(app.get_max_height_text()),
      .max_long_edge_text = shared_to_string(app.get_max_long_edge_text()),
      .max_short_edge_text = shared_to_string(app.get_max_short_edge_text()),
      .scale_percent_text = shared_to_string(app.get_scale_percent_text())};
}

void apply_parameter_params_to_ui(AwjStudio& app,
                                  const ParameterFormatParams& params,
                                  int format_index) {
  const auto format = output_format_from_index(format_index);
  const bool png_lossless = format == awj::OutputFormat::png;
  app.set_quality_text(to_shared(params.quality_text));
  app.set_visual_quality_text(
      to_shared(png_lossless ? std::string{} : params.visual_quality_text));
  app.set_bit_depth_text(to_shared(params.bit_depth_text));
  app.set_speed_text(to_shared(params.speed_text));
  refresh_avif_encoder_options(app);
  app.set_avif_encoder_index(params.avif_encoder_index);
  app.set_avif_color_representation_index(
      params.avif_color_representation_index);
  app.set_chroma_index(params.chroma_index);
  app.set_alpha_policy_index(params.alpha_policy_index);
  app.set_jpegli_progressive_index(params.jpegli_progressive_index);
  app.set_jpegli_optimize_huffman(params.jpegli_progressive_index > 0 || params.jpegli_optimize_huffman);
  app.set_jpegli_xyb(params.jpegli_xyb);
  app.set_jxl_jpeg_lossless(params.jxl_jpeg_lossless);
  app.set_threads_text(to_shared(params.threads_text));
  app.set_memory_limit_text(to_shared(params.memory_limit_text));
  app.set_size_limit_index(params.size_limit_index);
  app.set_max_width_text(to_shared(params.max_width_text));
  app.set_max_height_text(to_shared(params.max_height_text));
  app.set_max_long_edge_text(to_shared(params.max_long_edge_text));
  app.set_max_short_edge_text(to_shared(params.max_short_edge_text));
  app.set_scale_percent_text(to_shared(params.scale_percent_text));
  app.set_quality_follows_format(
      params.quality_text == text_from_int(awj::default_quality_for(format)));
  app.set_bit_depth_follows_format(
      (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli)
          ? params.bit_depth_text == text_from_int(
                                      awj::encoding_defaults::default_webp_bit_depth)
          : params.bit_depth_text.empty());
}

std::array<ParameterFormatParams, 5>& active_parameter_params(UiState& state) {
  return state.parameter_preset_index == 0 ? state.builtin_params
                                           : state.parameter_preset_params;
}

const std::array<ParameterFormatParams, 5>& active_parameter_params(
    const UiState& state) {
  return state.parameter_preset_index == 0 ? state.builtin_params
                                           : state.parameter_preset_params;
}

void store_current_parameter_params(AwjStudio& app, UiState& state) {
  const auto index = parameter_editor_format_index(state.last_format_index);
  auto params = capture_parameter_params_from_ui(app);
  const auto format = output_format_from_index(index);
  if (format == awj::OutputFormat::png) {
    params.visual_quality_text.clear();
  }
  if ((format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
       format == awj::OutputFormat::jxl) &&
      trim_copy(params.speed_text).empty()) {
    params.speed_text = text_from_int(awj::default_speed_for(format));
  }
  active_parameter_params(state)[static_cast<std::size_t>(index)] =
      std::move(params);
}


std::vector<ComboOption> avif_encoder_options() {
  return {combo_option("自动"), combo_option("aom",
      awj::avif_libavif_encoder_available(awj::AvifEncoderMode::aom))};
}

void refresh_avif_encoder_options(AwjStudio& app) {
  const auto options = avif_encoder_options();
  set_combo_options(app, options, &AwjStudio::set_avif_encoder_options);
  const auto selected = app.get_avif_encoder_index();
  if (selected < 0 || static_cast<std::size_t>(selected) >= options.size() ||
      !options[static_cast<std::size_t>(selected)].enabled) {
    app.set_avif_encoder_index(0);
  }
}
}  // namespace awj::studio
