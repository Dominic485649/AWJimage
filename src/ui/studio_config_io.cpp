#include "studio_config_io.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <string>
#include <utility>

#include "studio_config.h"
#include "studio_json.h"
#include "studio_parameter_page.h"
#include "studio_menu_params.h"
#include "studio_shell_cli.h"
#include "menu_transaction_state.hpp"
#include "studio_ui_util.h"

import awj.core;
import awj.encoding_defaults;
import awj.studio_defaults;

namespace awj::studio {

std::pair<int, int> current_studio_window_size(const AwjStudio& app) noexcept {
  try {
    const auto physical_size = app.window().size();
    const auto scale = std::max(app.window().scale_factor(), 1.0f);
    const auto width = physical_size.width == 0
                           ? awj::studio_defaults::default_window_width
                           : std::lround(static_cast<double>(physical_size.width) /
                                         scale);
    const auto height = physical_size.height == 0
                            ? awj::studio_defaults::default_window_height
                            : std::lround(
                                  static_cast<double>(physical_size.height) / scale);
    return {std::clamp(static_cast<int>(width),
                       awj::studio_defaults::min_window_width,
                       awj::studio_defaults::max_window_width),
            std::clamp(static_cast<int>(height),
                       awj::studio_defaults::min_window_height,
                       awj::studio_defaults::max_window_height)};
  } catch (...) {
    return {static_cast<int>(awj::studio_defaults::default_window_width),
            static_cast<int>(awj::studio_defaults::default_window_height)};
  }
}



StudioConfigSnapshot capture_studio_config(const AwjStudio& app,
                                           const UiState* state) {
  StudioConfigSnapshot snapshot{
      .theme_index = app.get_theme_index(),
      .language_index = app.get_language_index(),
      .ui_font_family = shared_to_string(app.get_ui_font_family()),
      .allow_wic_fallback = app.get_allow_wic_fallback(),
      .shell_menu_compatibility = app.get_shell_menu_compatibility(),
      .visual_quality_gpu = app.get_visual_quality_gpu(),
      .visual_quality_fallback = app.get_visual_quality_fallback(),
      .menu_params = menu_params_snapshot(app, state)};
  // 后台状态只在 UiState 中维护；所有写入仍由 UI 线程走统一的原子提交。
  if (state != nullptr) {
    snapshot.update_channel = state->update_channel;
    snapshot.show_update_changelog = state->show_update_changelog;
    snapshot.hide_update_changelog_after_exit =
        state->hide_update_changelog_after_exit;
    snapshot.show_update_changelog_after_update =
        state->show_update_changelog_after_update;
    snapshot.last_changelog_exit_version = state->last_changelog_exit_version;
    snapshot.last_successful_update_check_at =
        state->last_successful_update_check_at;
    snapshot.last_verified_manifest_sequence =
        state->last_verified_manifest_sequence;
    snapshot.last_verified_manifest_v2_sequence =
        state->last_verified_manifest_v2_sequence;
    snapshot.pending_update_version = state->pending_update_version;
    snapshot.pending_update_channel = state->pending_update_channel;
    snapshot.pending_update_release_url = state->pending_update_release_url;
    snapshot.pending_update_published_at = state->pending_update_published_at;
    snapshot.pending_update_changelog_zh_cn =
        state->pending_update_changelog_zh_cn;
    snapshot.pending_update_changelog_en = state->pending_update_changelog_en;
    snapshot.update_manifest_raw = state->update_manifest_raw;
    snapshot.update_manifest_signature = state->update_manifest_signature;
    snapshot.update_manifest_v2_raw = state->update_manifest_v2_raw;
    snapshot.update_manifest_v2_signature = state->update_manifest_v2_signature;
    snapshot.update_keyring_raw = state->update_keyring_raw;
    snapshot.update_keyring_signature = state->update_keyring_signature;
  }
  return snapshot;
}


template <class Setter>
std::expected<void, std::string> apply_config_int(
    AwjStudio& app,
    const std::unordered_map<std::string, awj::studio_json::JsonConfigValue>& values,
    std::string_view key, int minimum, int maximum, Setter setter) {
  auto value = awj::studio_json::config_int(values, key, minimum, maximum);
  if (!value) {
    return value.error().empty() ? std::expected<void, std::string>{}
                                 : std::unexpected{value.error()};
  }
  (app.*setter)(*value);
  return {};
}

template <class Setter>
std::expected<void, std::string> apply_config_bool(
    AwjStudio& app,
    const std::unordered_map<std::string, awj::studio_json::JsonConfigValue>& values,
    std::string_view key, Setter setter) {
  auto value = awj::studio_json::config_bool(values, key);
  if (!value) {
    return value.error().empty() ? std::expected<void, std::string>{}
                                 : std::unexpected{value.error()};
  }
  (app.*setter)(*value);
  return {};
}

template <class Setter>
std::expected<void, std::string> apply_config_string(
    AwjStudio& app,
    const std::unordered_map<std::string, awj::studio_json::JsonConfigValue>& values,
    std::string_view key, Setter setter) {
  auto value = awj::studio_json::config_string(values, key);
  if (!value) {
    return value.error().empty() ? std::expected<void, std::string>{}
                                 : std::unexpected{value.error()};
  }
  (app.*setter)(to_shared(*value));
  return {};
}

std::expected<void, std::string> apply_config_window_size(
    AwjStudio& app,
    const std::unordered_map<std::string, awj::studio_json::JsonConfigValue>& values) {
  const bool has_width = awj::studio_json::config_has_key(values, "window_width");
  const bool has_height = awj::studio_json::config_has_key(values, "window_height");
  if (!has_width && !has_height) {
    return {};
  }
  if (has_width != has_height) {
    return std::unexpected{"window_width 与 window_height 必须同时设置。"};
  }
  auto width = awj::studio_json::config_int(values, "window_width",
                          awj::studio_defaults::min_window_width,
                          awj::studio_defaults::max_window_width);
  if (!width) {
    return std::unexpected{width.error()};
  }
  auto height = awj::studio_json::config_int(values, "window_height",
                           awj::studio_defaults::min_window_height,
                           awj::studio_defaults::max_window_height);
  if (!height) {
    return std::unexpected{height.error()};
  }
  app.window().set_size(slint::LogicalSize{
      {static_cast<float>(*width), static_cast<float>(*height)}});
  return {};
}


std::expected<void, std::string> apply_menu_config_values(
    const std::unordered_map<std::string, awj::studio_json::JsonConfigValue>& values,
    std::array<MenuFormatParams, 5>& params) {
  const auto apply_int = [&](std::string_view key, int minimum, int maximum,
                             int& target) -> std::expected<void, std::string> {
    auto value = awj::studio_json::config_int(values, key, minimum, maximum);
    if (!value) {
      return value.error().empty() ? std::expected<void, std::string>{}
                                   : std::unexpected{value.error()};
    }
    target = *value;
    return {};
  };
  const auto apply_bool = [&](std::string_view key,
                              bool& target) -> std::expected<void, std::string> {
    auto value = awj::studio_json::config_bool(values, key);
    if (!value) {
      return value.error().empty() ? std::expected<void, std::string>{}
                                   : std::unexpected{value.error()};
    }
    target = *value;
    return {};
  };
  const auto apply_string = [&](std::string_view key, std::string& target)
      -> std::expected<void, std::string> {
    auto value = awj::studio_json::config_string(values, key);
    if (!value) {
      return value.error().empty() ? std::expected<void, std::string>{}
                                   : std::unexpected{value.error()};
    }
    target = std::move(*value);
    return {};
  };

  for (std::size_t i = 0; i < params.size(); ++i) {
    const auto prefix = menu_config_prefixes[i];
    auto& param = params[i];
    const auto one = [&](auto result) -> std::expected<void, std::string> {
      if (!result) return std::unexpected{result.error()};
      return {};
    };
    // menu_*_preset_index 在 1.0.0 随右键预设下拉一并移除；旧配置里的残留键会被忽略。
    if (auto r = one(apply_string(menu_config_key(prefix, "quality_text"), param.quality_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "bit_depth_text"), param.bit_depth_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "speed_text"), param.speed_text)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "avif_encoder_index"), 0, 3, param.avif_encoder_index)); !r) return r;
    if (param.avif_encoder_index == 1 || param.avif_encoder_index == 3) {
      return std::unexpected{std::format("{} 对应的编码器已移除，请将该字段修正为 0（auto）或 2（AOM）。",
          menu_config_key(prefix, "avif_encoder_index"))};
    }
    if (param.avif_encoder_index == 2) param.avif_encoder_index = 1;
    if (auto r = one(apply_int(menu_config_key(prefix, "avif_color_representation_index"), 0, 2, param.avif_color_representation_index)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "chroma_index"), 0, 3, param.chroma_index)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "alpha_policy_index"), 0, 2, param.alpha_policy_index)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "jpegli_progressive_index"), 0, 2, param.jpegli_progressive_index)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "jpegli_optimize_huffman"), param.jpegli_optimize_huffman)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "jpegli_xyb"), param.jpegli_xyb)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "jxl_jpeg_lossless"), param.jxl_jpeg_lossless)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "strip_metadata"), param.strip_metadata)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "allow_wic_fallback"), param.allow_wic_fallback)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "close_on_finish"), param.close_on_finish)); !r) return r;
    if (auto r = one(apply_bool(menu_config_key(prefix, "install_avif_png_command"), param.install_avif_png_command)); !r) return r;
    if (auto r = one(apply_int(menu_config_key(prefix, "size_limit_index"), 0, 2, param.size_limit_index)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "max_width_text"), param.max_width_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "max_height_text"), param.max_height_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "max_long_edge_text"), param.max_long_edge_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "max_short_edge_text"), param.max_short_edge_text)); !r) return r;
    if (auto r = one(apply_string(menu_config_key(prefix, "scale_percent_text"), param.scale_percent_text)); !r) return r;
  }
  return {};
}

std::expected<void, std::string> apply_studio_config_file(AwjStudio& app, UiState& state) {
  const auto path = studio_config_path();
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return {};
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{"无法读取 Studio 配置文件。"};
  }
  std::string source{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
  auto values = awj::studio_json::parse_jsonc_config(source);
  if (!values) {
    return std::unexpected{values.error()};
  }

  const auto apply = [&](auto result) -> std::expected<void, std::string> {
    if (!result) {
      return std::unexpected{result.error()};
    }
    return {};
  };

  if (auto result = apply(apply_config_int(app, *values, "theme_index", 0, 2,
                                           &AwjStudio::set_theme_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(app, *values, "language_index", 0, 1,
                                           &AwjStudio::set_language_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_string(
          app, *values, "ui_font_family", &AwjStudio::set_ui_font_family));
      !result) {
    return result;
  }

  if (auto result = apply(apply_config_bool(
          app, *values, "allow_wic_fallback",
          &AwjStudio::set_allow_wic_fallback));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "shell_menu_compatibility",
          &AwjStudio::set_shell_menu_compatibility)); !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "visual_quality_gpu",
          &AwjStudio::set_visual_quality_gpu));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "visual_quality_fallback",
          &AwjStudio::set_visual_quality_fallback));
      !result) {
    return result;
  }
  if (auto result = apply_menu_config_values(*values, state.menu_params); !result) {
    return result;
  }

  // 更新状态：读进 UiState 而不是 Slint 属性。缺失的键保持默认值——首次运行、
  // 或旧版本写出的配置里没有这些键，都属于正常情况，不是错误。
  {
    const auto load_int64 =
        [&](std::string_view key, std::int64_t minimum, std::int64_t maximum,
            std::int64_t& target) -> std::expected<void, std::string> {
      auto parsed = awj::studio_json::config_int64(*values, key, minimum, maximum);
      if (parsed) {
        target = *parsed;
        return {};
      }
      return parsed.error().empty() ? std::expected<void, std::string>{}
                                    : std::unexpected{parsed.error()};
    };
    const auto load_bool = [&](std::string_view key,
                               bool& target) -> std::expected<void, std::string> {
      auto parsed = awj::studio_json::config_bool(*values, key);
      if (parsed) {
        target = *parsed;
        return {};
      }
      return parsed.error().empty() ? std::expected<void, std::string>{}
                                    : std::unexpected{parsed.error()};
    };
    const auto load_string =
        [&](std::string_view key,
            std::string& target) -> std::expected<void, std::string> {
      auto parsed = awj::studio_json::config_string(*values, key);
      if (parsed) {
        target = *parsed;
        return {};
      }
      return parsed.error().empty() ? std::expected<void, std::string>{}
                                    : std::unexpected{parsed.error()};
    };

    constexpr std::int64_t max_unix_seconds = 4102444800;  // 2100-01-01Z
    if (auto r = load_string("update_channel", state.update_channel); !r) return r;
    if (state.update_channel != "stable" && state.update_channel != "prerelease") {
      return std::unexpected{"配置 update_channel 只能是 stable 或 prerelease。"};
    }
    if (auto r = load_bool("show_update_changelog", state.show_update_changelog); !r) return r;
    if (auto r = load_bool("hide_update_changelog_after_exit",
                           state.hide_update_changelog_after_exit); !r) return r;
    if (auto r = load_bool("show_update_changelog_after_update",
                           state.show_update_changelog_after_update); !r) return r;
    if (auto r = load_string("last_changelog_exit_version",
                             state.last_changelog_exit_version); !r) return r;
    if (auto r = load_int64("last_successful_update_check_at", 0,
                            max_unix_seconds,
                            state.last_successful_update_check_at); !r) return r;
    if (auto r = load_int64("last_verified_manifest_sequence", 0,
                            std::numeric_limits<std::int64_t>::max(),
                            state.last_verified_manifest_sequence); !r) return r;
    if (auto r = load_int64("last_verified_manifest_v2_sequence", 0,
                            std::numeric_limits<std::int64_t>::max(),
                            state.last_verified_manifest_v2_sequence); !r) return r;
    if (auto r = load_string("pending_update_version", state.pending_update_version); !r) return r;
    if (auto r = load_string("pending_update_channel", state.pending_update_channel); !r) return r;
    if (auto r = load_string("pending_update_release_url", state.pending_update_release_url); !r) return r;
    if (auto r = load_string("pending_update_published_at", state.pending_update_published_at); !r) return r;
    if (auto r = load_string("pending_update_changelog_zh_cn",
                             state.pending_update_changelog_zh_cn); !r) return r;
    if (auto r = load_string("pending_update_changelog_en",
                             state.pending_update_changelog_en); !r) return r;
    if (auto r = load_string("update_manifest_raw", state.update_manifest_raw);
        !r) return r;
    if (auto r = load_string("update_manifest_signature",
                             state.update_manifest_signature); !r) return r;
    if (auto r = load_string("update_manifest_v2_raw",
                             state.update_manifest_v2_raw); !r) return r;
    if (auto r = load_string("update_manifest_v2_signature",
                             state.update_manifest_v2_signature); !r) return r;
    if (auto r = load_string("update_keyring_raw", state.update_keyring_raw);
        !r) return r;
    if (auto r = load_string("update_keyring_signature",
                             state.update_keyring_signature); !r) return r;
  }

  app.set_update_channel_index(state.update_channel == "prerelease" ? 1 : 0);
  app.set_hide_update_changelog_after_exit(
      state.hide_update_changelog_after_exit);
  app.set_show_update_changelog_after_update(
      state.show_update_changelog_after_update);
  load_menu_params_for_index(app, state, app.get_menu_format_index());
  return {};
}

std::expected<void, std::string> persist_studio_config_if_changed(
    AwjStudio& app, UiState& state) {
  if (!state.config_defaults || state.menu_operation_active) {
    return {};
  }
  shell_context_menu::MenuOperationLock operation;
  if (!operation.held()) return std::unexpected{"另一进程正在修改右键菜单配置。"};
  auto current = capture_studio_config(app, &state);
  // Machine parameters are applied only by an explicit save/repair action.
  if (current.shell_menu_compatibility && state.last_config_snapshot)
    current.menu_params = state.last_config_snapshot->menu_params;
  if (state.last_config_snapshot &&
      current == *state.last_config_snapshot) {
    return {};
  }
  if (auto saved = write_studio_config_file(current, *state.config_defaults);
      !saved) {
    return std::unexpected{saved.error()};
  }
  if (!state.last_config_snapshot || current.menu_params != state.last_config_snapshot->menu_params) {
    auto valid = validate_menu_params(current.menu_params);
    auto synchronized = valid ? synchronize_shell_context_menu(current.menu_params) : valid;
    if (!synchronized) {
      if (state.last_config_snapshot) {
        auto restored = write_studio_config_file(*state.last_config_snapshot, *state.config_defaults);
        if (!restored) return std::unexpected{synchronized.error() + " 配置恢复失败：" + restored.error()};
      }
      return synchronized;
    }
    app.set_context_menu_warning({});
  }
  state.last_config_snapshot = std::move(current);
  return {};
}



}  // namespace awj::studio
