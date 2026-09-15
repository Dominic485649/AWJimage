#include "studio_presets.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "studio_fields.h"
#include "studio_menu_params.h"
#include "studio_parameter_page.h"
#include "studio_queue_rows.h"
#include "studio_ui_util.h"

import awj.core;
import awj.encoding_defaults;
import awj.preset;
import awj.studio_defaults;

namespace awj::studio {

awj::CollisionMode collision_from_index(int index) {
  switch (index) {
    case 1:
      return awj::CollisionMode::skip;
    case 2:
      return awj::CollisionMode::suffix_time;
    case 3:
      return awj::CollisionMode::suffix_random;
    case 0:
    default:
      return awj::CollisionMode::overwrite;
  }
}


void apply_format_defaults_to_ui(AwjStudio& app, int format_index, UiState& state) {
  store_current_parameter_params(app, state);
  format_index = parameter_editor_format_index(format_index);
  if (app.get_format_index() != format_index) {
    app.set_format_index(format_index);
  }
  state.last_format_index = format_index;
  apply_parameter_params_to_ui(
      app, active_parameter_params(state)[static_cast<std::size_t>(format_index)],
      format_index);
}

void initialize_ui_defaults(AwjStudio& app, UiState& state) {
  const auto defaults = awj::default_app_config();
  app.set_input_path({});
  app.set_input_mode_index(0);
  app.set_template_text(to_shared(text_from_wide(defaults.output_template)));
  const auto avif_default = awj::default_quality_for(awj::OutputFormat::avif);
  state.last_format_index = 0;
  app.set_avif_quality_default(to_shared(text_from_int(avif_default)));
  app.set_webp_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::webp))));
  app.set_jxl_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::jxl))));
  app.set_jpegli_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::jpgli))));
  app.set_png_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::png))));
  app.set_webp_bit_depth_default(
      to_shared(text_from_int(awj::encoding_defaults::default_webp_bit_depth)));
  for (int i = 0; i < static_cast<int>(state.builtin_params.size()); ++i) {
    state.builtin_params[static_cast<std::size_t>(i)] =
        default_parameter_params_for_index(i);
  }
  apply_parameter_params_to_ui(app, state.builtin_params[0], 0);
  app.set_unlock_max_input_file_bytes(false);
  app.set_size_limit_index(0);
  app.set_max_width_text({});
  app.set_max_height_text({});
  app.set_max_long_edge_text({});
  app.set_max_short_edge_text({});
  app.set_format_index(0);
  app.set_visual_quality_gpu(defaults.visual_quality_gpu);
  app.set_visual_quality_fallback(defaults.visual_quality_fallback);
  app.set_allow_wic_fallback(defaults.allow_wic_fallback);
  refresh_avif_encoder_options(app);
  app.set_avif_encoder_index(0);
  app.set_collision_index(0);
  app.set_chroma_index(0);
  app.set_jpegli_progressive_index(
      awj::encoding_defaults::default_jpegli_progressive_level);
  app.set_jpegli_optimize_huffman(
      awj::encoding_defaults::default_jpegli_optimize_huffman);
  app.set_jpegli_xyb(awj::encoding_defaults::default_jpegli_xyb);
  app.set_alpha_policy_index(1);
  app.set_quality_follows_format(true);
  app.set_bit_depth_follows_format(true);
  app.set_queue_format_index(0);
  app.set_queue_preset_index(0);
  app.set_strip_metadata(false);
  app.set_preserve_creation_time(false);
  app.set_preserve_modification_time(false);
  app.set_preserve_access_time(false);
  for (int i = 0; i < static_cast<int>(state.menu_params.size()); ++i) {
    state.menu_params[static_cast<std::size_t>(i)] = default_menu_params_for_index(i);
  }
  state.last_menu_format_index = 0;
  app.set_menu_format_index(0);
  load_menu_params_for_index(app, state, 0);
}

void reload_user_preset_options(AwjStudio& app, UiState& state) {
  auto catalog = awj::list_user_presets();
  if (!catalog) {
    state.user_presets.clear();
    state.user_preset_errors = {catalog.error()};
    const std::vector<ComboOption> builtin_only{
        {.text = to_shared("内置默认"), .enabled = true}};
    app.set_queue_preset_options(
        std::make_shared<slint::VectorModel<ComboOption>>(builtin_only));
    app.set_parameter_preset_options(
        std::make_shared<slint::VectorModel<ComboOption>>(builtin_only));
    app.set_queue_preset_index(0);
    app.set_queue_preset_description({});
    state.parameter_preset_index = 0;
    app.set_parameter_preset_index(0);
    app.set_parameter_preset_description({});
    return;
  }
  state.user_presets = std::move(catalog->presets);
  state.user_preset_errors = std::move(catalog->errors);
  std::vector<ComboOption> options;
  options.reserve(state.user_presets.size() + 1);
  options.push_back(ComboOption{.text = to_shared("内置默认"), .enabled = true});
  for (const auto& preset : state.user_presets) {
    options.push_back(ComboOption{.text = to_shared(preset.name), .enabled = true});
  }
  app.set_queue_preset_options(
      std::make_shared<slint::VectorModel<ComboOption>>(options));
  app.set_parameter_preset_options(
      std::make_shared<slint::VectorModel<ComboOption>>(std::move(options)));
  if (app.get_queue_preset_index() >
      static_cast<int>(state.user_presets.size())) {
    app.set_queue_preset_index(0);
  }
  if (state.parameter_preset_index >
      static_cast<int>(state.user_presets.size())) {
    state.parameter_preset_index = 0;
  }
  app.set_parameter_preset_index(state.parameter_preset_index);
  app.set_parameter_preset_description(
      state.parameter_preset_index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(
                                         state.parameter_preset_index - 1)]
                          .description));
  const auto queue_index = std::clamp(
      app.get_queue_preset_index(), 0,
      static_cast<int>(state.user_presets.size()));
  app.set_queue_preset_index(queue_index);
  app.set_queue_preset_description(
      queue_index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(
                          queue_index - 1)]
                          .description));
}

std::expected<awj::AppConfig, std::string> config_from_parameter_params(
    awj::OutputFormat format, const ParameterFormatParams& params) {
  const bool png_lossless = format == awj::OutputFormat::png;
  MenuFormatParams menu{
      .quality_text = params.quality_text,
      .bit_depth_text = params.bit_depth_text,
      .speed_text = params.speed_text,
      .avif_encoder_index = params.avif_encoder_index,
      .avif_color_representation_index =
          params.avif_color_representation_index,
      .chroma_index = params.chroma_index,
      .alpha_policy_index = params.alpha_policy_index,
      .jpegli_progressive_index = params.jpegli_progressive_index,
      .jpegli_optimize_huffman = params.jpegli_optimize_huffman,
      .jpegli_xyb = params.jpegli_xyb,
      .size_limit_index = params.size_limit_index,
      .max_width_text = params.max_width_text,
      .max_height_text = params.max_height_text,
      .max_long_edge_text = params.max_long_edge_text,
      .max_short_edge_text = params.max_short_edge_text};
  auto config = config_from_menu_params(format, menu);
  if (!config) return std::unexpected{config.error()};
  if (png_lossless) {
    config->visual_quality.reset();
  } else {
    const auto visual_quality =
        parse_visual_quality_field(params.visual_quality_text);
    if (!visual_quality) return std::unexpected{visual_quality.error()};
    config->visual_quality = *visual_quality;
  }
  const auto jobs = parse_jobs_field(params.threads_text);
  if (!jobs) return std::unexpected{jobs.error()};
  config->max_jobs = *jobs;
  const auto memory = parse_memory_limit_field(params.memory_limit_text);
  if (!memory) return std::unexpected{memory.error()};
  config->memory_limit_bytes = *memory;
  config->output_policy = awj::OutputPolicy::normal;
  return config;
}

int avif_encoder_index_from_mode(awj::AvifEncoderMode mode) noexcept {
  return mode == awj::AvifEncoderMode::aom ? 1 : 0;
}

int chroma_index_from_mode(awj::ChromaMode mode) noexcept {
  switch (mode) {
    case awj::ChromaMode::yuv444:
      return 1;
    case awj::ChromaMode::yuv422:
      return 2;
    case awj::ChromaMode::yuv420:
      return 3;
    case awj::ChromaMode::auto_keep:
    default:
      return 0;
  }
}

int avif_color_representation_index_from_mode(
    awj::AvifColorRepresentation mode) noexcept {
  switch (mode) {
    case awj::AvifColorRepresentation::source:
      return 1;
    case awj::AvifColorRepresentation::rgb_identity:
      return 2;
    case awj::AvifColorRepresentation::yuv:
    default:
      return 0;
  }
}

int alpha_policy_index_from_mode(awj::AlphaModePolicy mode) noexcept {
  switch (mode) {
    case awj::AlphaModePolicy::force:
      return 0;
    case awj::AlphaModePolicy::off:
      return 2;
    case awj::AlphaModePolicy::automatic:
    default:
      return 1;
  }
}

ParameterFormatParams parameter_params_from_config(const awj::AppConfig& config) {
  const auto format = config.output_format;
  ParameterFormatParams params = default_parameter_params_for_index(
      parameter_index_from_output_format(format));
  const auto optional_text = [](const std::optional<int>& value) {
    return value ? text_from_int(*value) : std::string{};
  };
  params.quality_text = text_from_int(config.quality);
  params.visual_quality_text = optional_text(config.visual_quality);
  params.bit_depth_text = optional_text(config.bit_depth);
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    params.speed_text = text_from_int(
        config.speed.value_or(awj::default_speed_for(format)));
  }
  params.avif_encoder_index = avif_encoder_index_from_mode(config.avif_encoder);
  params.avif_color_representation_index =
      avif_color_representation_index_from_mode(
          config.avif_color_representation);
  params.chroma_index = chroma_index_from_mode(config.chroma_mode);
  params.alpha_policy_index = alpha_policy_index_from_mode(config.alpha_policy);
  params.jpegli_progressive_index = config.jpegli_progressive_level;
  params.jpegli_optimize_huffman = config.jpegli_optimize_huffman;
  params.jpegli_xyb = config.jpegli_xyb;
  params.threads_text = config.max_jobs == awj::default_max_jobs()
                            ? std::string{}
                            : text_from_int(config.max_jobs);
  if (config.memory_limit_bytes != 0) {
    params.memory_limit_text = text_from_int(static_cast<int>(
        (config.memory_limit_bytes + awj::studio_defaults::bytes_per_gib - 1) /
        awj::studio_defaults::bytes_per_gib));
  }
  switch (config.image_size_limit.mode) {
    case awj::ImageSizeLimitMode::none:
      params.size_limit_index = 1;
      break;
    case awj::ImageSizeLimitMode::manual:
      params.size_limit_index = 2;
      break;
    case awj::ImageSizeLimitMode::automatic:
    default:
      params.size_limit_index = 0;
      break;
  }
  params.max_width_text = optional_text(config.image_size_limit.max_width);
  params.max_height_text = optional_text(config.image_size_limit.max_height);
  params.max_long_edge_text = optional_text(config.image_size_limit.max_long_edge);
  params.max_short_edge_text = optional_text(config.image_size_limit.max_short_edge);
  return params;
}

std::array<ParameterFormatParams, 5> parameter_params_from_user_preset(
    const awj::UserPreset& preset) {
  std::array<ParameterFormatParams, 5> params{};
  for (int index = 0; index < static_cast<int>(params.size()); ++index) {
    const auto format = output_format_from_index(index);
    params[static_cast<std::size_t>(index)] = parameter_params_from_config(
        awj::config_from_user_preset(preset, format));
  }
  return params;
}

std::expected<awj::UserPreset, std::string> user_preset_from_parameter_params(
    std::string name, std::string description,
    const std::array<ParameterFormatParams, 5>& params) {
  awj::UserPreset preset = awj::default_user_preset();
  preset.name = std::move(name);
  preset.description = std::move(description);
  for (int index = 0; index < static_cast<int>(params.size()); ++index) {
    const auto format = output_format_from_index(index);
    auto config = config_from_parameter_params(
        format, params[static_cast<std::size_t>(index)]);
    if (!config) {
      return std::unexpected{std::format("{} 预设参数错误：{}",
                                         awj::output_format_name(format),
                                         config.error())};
    }
    preset.formats[static_cast<std::size_t>(index)] =
        awj::preset_format_from_config(*config);
  }
  return preset;
}

void select_parameter_preset(AwjStudio& app, UiState& state, int index) {
  store_current_parameter_params(app, state);
  index = std::clamp(index, 0, static_cast<int>(state.user_presets.size()));
  state.parameter_preset_index = index;
  if (index > 0) {
    state.parameter_preset_params = parameter_params_from_user_preset(
        state.user_presets[static_cast<std::size_t>(index - 1)]);
  }
  app.set_parameter_preset_index(index);
  app.set_parameter_preset_description(
      index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(index - 1)]
                          .description));
  const auto format_index = parameter_editor_format_index(app.get_format_index());
  state.last_format_index = format_index;
  apply_parameter_params_to_ui(
      app, active_parameter_params(state)[static_cast<std::size_t>(format_index)],
      format_index);
}

void select_queue_preset(AwjStudio& app, UiState& state, int index) {
  index = std::clamp(index, 0, static_cast<int>(state.user_presets.size()));
  app.set_queue_preset_index(index);
  app.set_queue_preset_description(
      index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(index - 1)]
                          .description));
}

std::expected<awj::AppConfig, std::string> config_from_ui(
    AwjStudio& app, UiState& state) try {
  store_current_parameter_params(app, state);
  const auto queue_choice =
      queue_format_choice_from_index(app.get_queue_format_index());
  const int queue_format_index = queue_choice.format_index;
  const auto format = output_format_from_index(queue_format_index);
  const int preset_index = app.get_queue_preset_index();
  awj::AppConfig cfg;
  if (preset_index == 0) {
    auto parsed = config_from_parameter_params(
        format, state.builtin_params[static_cast<std::size_t>(queue_format_index)]);
    if (!parsed) return std::unexpected{parsed.error()};
    cfg = std::move(*parsed);
  } else {
    const auto user_index = preset_index - 1;
    if (user_index < 0 || static_cast<std::size_t>(user_index) >=
                              state.user_presets.size()) {
      return std::unexpected{"选中的用户预设已不存在，请重新选择。"};
    }
    cfg = awj::config_from_user_preset(
        state.user_presets[static_cast<std::size_t>(user_index)], format);
  }
  const auto input_path = awj::normalize_path_argument(
      awj::wide_from_utf8(shared_to_string(app.get_input_path())), "输入路径");
  if (!input_path) return std::unexpected{input_path.error()};
  cfg.input_path = *input_path;
  const auto output_text = shared_to_string(app.get_output_dir());
  if (!output_text.empty()) {
    const auto output_path = awj::normalize_path_argument(
        awj::wide_from_utf8(output_text), "输出目录");
    if (!output_path) return std::unexpected{output_path.error()};
    cfg.output_dir = *output_path;
  }
  cfg.output_template =
      awj::wide_from_utf8(shared_to_string(app.get_template_text()));
  if (cfg.output_template.empty()) {
    cfg.output_template = awj::encoding_defaults::default_output_template;
  }
  cfg.allow_wic_fallback = app.get_allow_wic_fallback();
  cfg.output_format = format;
  cfg.append_png_suffix = queue_choice.append_png_suffix;

  cfg.collision_mode = collision_from_index(app.get_collision_index());
  cfg.visual_quality_gpu = app.get_visual_quality_gpu();
  cfg.visual_quality_fallback = app.get_visual_quality_fallback();
  cfg.unlock_max_input_file_bytes = app.get_unlock_max_input_file_bytes();
  awj::encoding_defaults::unlock_max_input_file_bytes.store(cfg.unlock_max_input_file_bytes, std::memory_order_relaxed);
  cfg.strip_metadata = app.get_strip_metadata();
  cfg.write_summary = app.get_write_summary();
  cfg.write_log = app.get_write_log();
  cfg.preserve_creation_time = app.get_preserve_creation_time();
  cfg.preserve_modification_time = app.get_preserve_modification_time();
  cfg.preserve_access_time = app.get_preserve_access_time();

  if (auto valid = awj::finalize_config_defaults(cfg, true, true); !valid) {
    return std::unexpected{valid.error()};
  }
  return cfg;
} catch (const std::bad_alloc&) {
  return std::unexpected{"Studio 配置解析内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"Studio 配置解析数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"Studio 配置解析文件系统访问失败。"};
}

std::filesystem::path effective_output_dir(const AwjStudio& app) {
  auto output = std::filesystem::path{
      awj::wide_from_utf8(shared_to_string(app.get_output_dir()))};
  if (!output.empty()) {
    return output;
  }
  const auto input = std::filesystem::path{
      awj::wide_from_utf8(shared_to_string(app.get_input_path()))};
  return awj::default_output_dir_for(input);
}


}  // namespace awj::studio
