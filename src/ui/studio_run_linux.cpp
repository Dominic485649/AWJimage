// Linux Studio 的完整实现：LinuxUiState、平台工具、更新流程与 run_studio_ui。
// 从 main.cpp 拆出。仅在非 Windows 构建时编译。

#ifndef _WIN32

#include <slint.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <any>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <exception>
#include <expected>
#include <format>
#include <print>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "awj_studio.h"
#include "queue_model.h"
#include "deferred_model.h"
#include "changelog_history.h"

import awj.config;
import awj.core;
import awj.encoding_defaults;
import awj.large_image_plan;
import awj.pipeline;
import awj.preset;
import awj.studio_defaults;
import awj.update_linux;
import awj.update_keyring;
import awj.update_manifest;
import awj.update_manifest_v2;
import awj.update_model;
import awj.update_runtime;

extern char** environ;

namespace {

namespace fs = std::filesystem;

struct LinuxMenuParams {
  std::string quality_text{};
  std::string bit_depth_text{};
  std::string speed_text{};
  int avif_encoder_index{};
  int avif_color_representation_index{};
  int chroma_index{};
  int alpha_policy_index{1};
  int jpegli_progressive_index{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  bool jxl_jpeg_lossless{true};
  bool strip_metadata{};
  bool install_avif_png_command{};
  int size_limit_index{};
  std::string max_width_text{};
  std::string max_height_text{};
  std::string max_long_edge_text{};
  std::string max_short_edge_text{};
  std::string scale_percent_text{};
};

// 参数页按格式保存会话内的编辑值；普通队列只在开始时读取 queue-format
// 指向的这一组，避免“切换编辑格式”悄悄改变输出格式。
struct LinuxParameterParams {
  std::string quality_text{};
  std::string visual_quality_text{};
  std::string bit_depth_text{};
  std::string speed_text{};
  int avif_encoder_index{};
  int avif_color_representation_index{};
  int chroma_index{};
  int alpha_policy_index{1};
  int jpegli_progressive_index{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  bool jxl_jpeg_lossless{true};
  std::string threads_text{};
  std::string memory_limit_text{};
  int size_limit_index{};
  std::string max_width_text{};
  std::string max_height_text{};
  std::string max_long_edge_text{};
  std::string max_short_edge_text{};
  std::string scale_percent_text{};
};

struct LinuxUiState {
  std::jthread worker{};
  std::jthread update_worker{};
  std::shared_ptr<slint::VectorModel<TaskRow>> task_rows{};
  std::shared_ptr<slint::VectorModel<LargeImageRow>> large_image_rows{};
  std::shared_ptr<awj::ui::DeferredModel<UpdateHistoryRow>> update_history_rows{};
  bool ui_font_options_loaded{};
  bool drag_reordered{};
  bool close_requested{};
  slint::Timer close_timer{};
  std::vector<awj::BatchLargeImageItem> large_image_items{};
  std::vector<fs::path> failed_paths{};
  // The visible queue is the source of truth. A manifest is written only
  // immediately before a run so changing UI settings cannot rescan a folder.
  std::vector<awj::ImageFile> queue_files{};
  std::unordered_set<std::wstring> queue_path_keys{};
  std::uint64_t next_queue_run_id{1};
  std::array<LinuxParameterParams, 5> builtin_params{};
  std::array<LinuxParameterParams, 5> parameter_preset_params{};
  std::vector<awj::UserPreset> user_presets{};
  std::vector<std::string> user_preset_errors{};
  int parameter_preset_index{};
  int last_format_index{};
  std::array<LinuxMenuParams, 5> menu_params{};
  int menu_format_index{};
  fs::path config_path{};
  nlohmann::ordered_json config_document{nlohmann::ordered_json::object()};
  bool config_readable{true};
  bool update_check_active{};
  std::string update_channel{"stable"};
  bool show_update_changelog{true};
  bool hide_update_changelog_after_exit{true};
  bool show_update_changelog_after_update{true};
  std::string last_changelog_exit_version{};
  std::int64_t last_successful_update_check_at{};
  // v1 remains only as cached state for 1.0.3 bridge compatibility. Linux
  // Studio itself consumes the independent v2 counter and signed cache.
  std::int64_t last_verified_manifest_sequence{};
  std::int64_t last_verified_manifest_v2_sequence{};
  std::string pending_update_version{};
  std::string pending_update_channel{};
  std::string pending_update_release_url{};
  std::string pending_update_published_at{};
  std::string pending_update_changelog_zh_cn{};
  std::string pending_update_changelog_en{};
  std::string update_manifest_raw{};
  std::string update_manifest_signature{};
  std::string update_manifest_v2_raw{};
  std::string update_manifest_v2_signature{};
  std::string update_keyring_raw{};
  std::string update_keyring_signature{};
  std::string update_status_zh{"尚未检查"};
  std::string update_status_en{"Not checked yet"};
};

nlohmann::ordered_json linux_menu_params_json(const LinuxMenuParams& params) {
  return {{"quality_text", params.quality_text},
          {"bit_depth_text", params.bit_depth_text},
          {"speed_text", params.speed_text},
          {"avif_encoder_index", params.avif_encoder_index},
          {"avif_color_representation_index",
           params.avif_color_representation_index},
          {"chroma_index", params.chroma_index},
          {"alpha_policy_index", params.alpha_policy_index},
          {"jpegli_progressive_index", params.jpegli_progressive_index},
          {"jpegli_optimize_huffman", params.jpegli_optimize_huffman},
          {"jpegli_xyb", params.jpegli_xyb},
          {"jxl_jpeg_lossless", params.jxl_jpeg_lossless},
          {"strip_metadata", params.strip_metadata},
          {"install_avif_png_command", params.install_avif_png_command},
          {"size_limit_index", params.size_limit_index},
          {"max_width_text", params.max_width_text},
          {"max_height_text", params.max_height_text},
          {"max_long_edge_text", params.max_long_edge_text},
          {"max_short_edge_text", params.max_short_edge_text},
          {"scale_percent_text", params.scale_percent_text}};
}

void load_linux_menu_params(const nlohmann::ordered_json& document,
                            std::array<LinuxMenuParams, 5>& output) {
  const auto found = document.find("menu_params");
  if (found == document.end() || !found->is_array() || found->size() != output.size()) {
    return;
  }
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto& value = (*found)[index];
    if (!value.is_object()) continue;
    auto& params = output[index];
    const auto text = [&](std::string_view key, std::string& target) {
      const auto it = value.find(std::string{key});
      if (it != value.end() && it->is_string()) target = it->get<std::string>();
    };
    const auto integer = [&](std::string_view key, int minimum, int maximum,
                             int& target) {
      const auto it = value.find(std::string{key});
      if (it != value.end() && it->is_number_integer()) {
        const auto candidate = it->get<long long>();
        if (candidate >= minimum && candidate <= maximum) {
          target = static_cast<int>(candidate);
        }
      }
    };
    const auto boolean = [&](std::string_view key, bool& target) {
      const auto it = value.find(std::string{key});
      if (it != value.end() && it->is_boolean()) target = it->get<bool>();
    };
    text("quality_text", params.quality_text);
    text("bit_depth_text", params.bit_depth_text);
    text("speed_text", params.speed_text);
    integer("avif_encoder_index", 0, 3, params.avif_encoder_index);
    integer("avif_color_representation_index", 0, 2,
            params.avif_color_representation_index);
    integer("chroma_index", 0, 3, params.chroma_index);
    integer("alpha_policy_index", 0, 2, params.alpha_policy_index);
    integer("jpegli_progressive_index", 0, 2, params.jpegli_progressive_index);
    boolean("jpegli_optimize_huffman", params.jpegli_optimize_huffman);
    boolean("jpegli_xyb", params.jpegli_xyb);
    boolean("jxl_jpeg_lossless", params.jxl_jpeg_lossless);
    boolean("strip_metadata", params.strip_metadata);
    boolean("install_avif_png_command", params.install_avif_png_command);
    integer("size_limit_index", 0, 2, params.size_limit_index);
    text("max_width_text", params.max_width_text);
    text("max_height_text", params.max_height_text);
    text("max_long_edge_text", params.max_long_edge_text);
    text("max_short_edge_text", params.max_short_edge_text);
    text("scale_percent_text", params.scale_percent_text);
  }
}

std::string trim_copy(std::string value) {
  const auto first = std::ranges::find_if_not(value, [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  }).base();
  if (first >= last) return {};
  return std::string(first, last);
}

std::wstring trim_copy(std::wstring value) {
  const auto is_space = [](wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
  };
  const auto first = std::ranges::find_if_not(value, is_space);
  const auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
  if (first >= last) return {};
  return std::wstring(first, last);
}

std::wstring memory_arg_from_ui(std::wstring value) {
  value = trim_copy(std::move(value));
  if (value.empty()) return value;
  const bool has_unit = std::ranges::any_of(value, [](wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
  });
  return has_unit ? value : value + L"GiB";
}

std::string shell_quote(std::string_view value) {
  std::string out{"'"};
  for (const char ch : value) {
    if (ch == '\'') out += "'\\''";
    else out += ch;
  }
  out += "'";
  return out;
}

bool command_exists(std::string_view command) {
  return std::system(std::format("command -v {} >/dev/null 2>&1", command).c_str()) == 0;
}

struct PcloseDeleter {
  void operator()(FILE* file) const noexcept {
    if (file != nullptr) pclose(file);
  }
};

slint::SharedString to_shared(std::string_view text);
std::string shared_to_string(const slint::SharedString& value);

std::optional<std::string> run_capture(const std::string& command) {
  std::unique_ptr<FILE, PcloseDeleter> pipe{popen(command.c_str(), "r")};
  if (!pipe) return std::nullopt;
  std::string output;
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
    output += buffer.data();
  }
  output = trim_copy(std::move(output));
  if (output.empty()) return std::nullopt;
  return output;
}

void load_system_font_options(AwjStudio& app) {
  std::unordered_set<std::string> families;
  if (auto output = run_capture("fc-list -f '%{family}\\n' 2>/dev/null")) {
    std::size_t line_start = 0;
    while (line_start <= output->size()) {
      const auto line_end = output->find('\n', line_start);
      const auto line = output->substr(
          line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
      std::size_t name_start = 0;
      while (name_start <= line.size()) {
        const auto name_end = line.find(',', name_start);
        auto name = trim_copy(line.substr(
            name_start, name_end == std::string::npos ? std::string::npos : name_end - name_start));
        if (!name.empty() && name.front() != '@') families.insert(std::move(name));
        if (name_end == std::string::npos) break;
        name_start = name_end + 1;
      }
      if (line_end == std::string::npos) break;
      line_start = line_end + 1;
    }
  }
  std::vector<std::string> sorted(families.begin(), families.end());
  std::ranges::sort(sorted);
  std::vector<ComboOption> options;
  options.reserve(sorted.size() + 1);
  options.push_back(ComboOption{.text = to_shared("系统默认字体"), .enabled = true});
  for (const auto& family : sorted) {
    options.push_back(ComboOption{.text = to_shared(family), .enabled = true});
  }
  app.set_ui_font_options(std::make_shared<slint::VectorModel<ComboOption>>(std::move(options)));
  const auto selected = shared_to_string(app.get_ui_font_family());
  const auto found = std::ranges::find(sorted, selected);
  if (found == sorted.end()) {
    app.set_ui_font_index(0);
    app.set_ui_font_family({});
  } else {
    app.set_ui_font_index(static_cast<int>(std::distance(sorted.begin(), found)) + 1);
  }
}

std::expected<fs::path, std::string> choose_path(bool directory) {
  std::vector<std::string> commands;
  if (command_exists("zenity")) commands.push_back(directory ? "zenity --file-selection --directory" : "zenity --file-selection");
  if (command_exists("yad")) commands.push_back(directory ? "yad --file-selection --directory" : "yad --file-selection");
  if (command_exists("kdialog")) commands.push_back(directory ? "kdialog --getexistingdirectory" : "kdialog --getopenfilename");
  if (commands.empty()) {
    return std::unexpected{"未找到 Linux 文件选择器；请安装 zenity/yad/kdialog，或手动输入路径。"};
  }
  for (const auto& command : commands) {
    if (auto selected = run_capture(command)) return fs::path{*selected};
  }
  return std::unexpected{"未选择路径。"};
}

std::expected<void, std::string> open_path(fs::path path) {
  if (path.empty()) return std::unexpected{"没有可打开的路径。"};
  std::error_code ec;
  if (fs::is_regular_file(path, ec) && !ec) path = path.parent_path();
  if (!fs::exists(path, ec) || ec) path = path.parent_path();
  if (path.empty()) return std::unexpected{"路径不存在，无法打开。"};
  const auto quoted = shell_quote(awj::path_to_utf8(path));
  if (command_exists("gio") && std::system(std::format("gio open {} >/dev/null 2>&1 &", quoted).c_str()) == 0) return {};
  if (command_exists("xdg-open") && std::system(std::format("xdg-open {} >/dev/null 2>&1 &", quoted).c_str()) == 0) return {};
  if (command_exists("thunar") && std::system(std::format("thunar {} >/dev/null 2>&1 &", quoted).c_str()) == 0) return {};
  return std::unexpected{"未找到可用的目录打开工具（gio/xdg-open/thunar）。"};
}

slint::SharedString to_shared(std::string_view text) {
  return slint::SharedString{std::string{text}.c_str()};
}

std::string shared_to_string(const slint::SharedString& value) {
  return std::string{value.data(), value.size()};
}

std::vector<std::string> native_drop_paths(const slint::SharedString& value) {
  std::vector<std::string> paths;
  const auto text = shared_to_string(value);
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const auto end = text.find('\n', begin);
    auto path = text.substr(begin, end == std::string::npos ? std::string::npos
                                                              : end - begin);
    if (!path.empty() && path.back() == '\r') path.pop_back();
    if (!path.empty()) paths.push_back(std::move(path));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return paths;
}

struct LinuxUpdatePersistentState {
  std::string channel{};
  bool show_changelog{};
  bool hide_changelog_after_exit{};
  bool show_changelog_after_update{};
  std::string last_changelog_exit_version{};
  std::int64_t last_successful_check{};
  std::int64_t last_verified_sequence{};
  std::int64_t last_verified_v2_sequence{};
  std::string version{};
  std::string pending_channel{};
  std::string release_url{};
  std::string published_at{};
  std::string changelog_zh_cn{};
  std::string changelog_en{};
  std::string manifest_raw{};
  std::string manifest_signature{};
  std::string manifest_v2_raw{};
  std::string manifest_v2_signature{};
  std::string keyring_raw{};
  std::string keyring_signature{};
};

LinuxUpdatePersistentState capture_linux_update_state(
    const LinuxUiState& state) {
  return {.channel = state.update_channel,
          .show_changelog = state.show_update_changelog,
          .hide_changelog_after_exit = state.hide_update_changelog_after_exit,
          .show_changelog_after_update = state.show_update_changelog_after_update,
          .last_changelog_exit_version = state.last_changelog_exit_version,
          .last_successful_check = state.last_successful_update_check_at,
          .last_verified_sequence = state.last_verified_manifest_sequence,
          .last_verified_v2_sequence = state.last_verified_manifest_v2_sequence,
          .version = state.pending_update_version,
          .pending_channel = state.pending_update_channel,
          .release_url = state.pending_update_release_url,
          .published_at = state.pending_update_published_at,
          .changelog_zh_cn = state.pending_update_changelog_zh_cn,
          .changelog_en = state.pending_update_changelog_en,
          .manifest_raw = state.update_manifest_raw,
          .manifest_signature = state.update_manifest_signature,
          .manifest_v2_raw = state.update_manifest_v2_raw,
          .manifest_v2_signature = state.update_manifest_v2_signature,
          .keyring_raw = state.update_keyring_raw,
          .keyring_signature = state.update_keyring_signature};
}

void restore_linux_update_state(LinuxUiState& state,
                                LinuxUpdatePersistentState value) {
  state.update_channel = std::move(value.channel);
  state.show_update_changelog = value.show_changelog;
  state.hide_update_changelog_after_exit = value.hide_changelog_after_exit;
  state.show_update_changelog_after_update = value.show_changelog_after_update;
  state.last_changelog_exit_version = std::move(value.last_changelog_exit_version);
  state.last_successful_update_check_at = value.last_successful_check;
  state.last_verified_manifest_sequence = value.last_verified_sequence;
  state.last_verified_manifest_v2_sequence = value.last_verified_v2_sequence;
  state.pending_update_version = std::move(value.version);
  state.pending_update_channel = std::move(value.pending_channel);
  state.pending_update_release_url = std::move(value.release_url);
  state.pending_update_published_at = std::move(value.published_at);
  state.pending_update_changelog_zh_cn = std::move(value.changelog_zh_cn);
  state.pending_update_changelog_en = std::move(value.changelog_en);
  state.update_manifest_raw = std::move(value.manifest_raw);
  state.update_manifest_signature = std::move(value.manifest_signature);
  state.update_manifest_v2_raw = std::move(value.manifest_v2_raw);
  state.update_manifest_v2_signature = std::move(value.manifest_v2_signature);
  state.update_keyring_raw = std::move(value.keyring_raw);
  state.update_keyring_signature = std::move(value.keyring_signature);
}

void clear_linux_pending_update(LinuxUiState& state) {
  state.pending_update_version.clear();
  state.pending_update_channel.clear();
  state.pending_update_release_url.clear();
  state.pending_update_published_at.clear();
  state.pending_update_changelog_zh_cn.clear();
  state.pending_update_changelog_en.clear();
}

std::string linux_update_summary(std::string_view changelog) {
  return std::string{changelog.substr(0, changelog.find_first_of("\r\n"))};
}

std::string linux_update_check_time(std::int64_t unix_seconds) {
  if (unix_seconds <= 0) return {};
  const auto point = std::chrono::system_clock::time_point{
      std::chrono::seconds{unix_seconds}};
  return std::format("{:%Y-%m-%d %H:%M:%S} UTC",
                     std::chrono::floor<std::chrono::seconds>(point));
}

bool linux_changelog_first_start_for_current_version(const LinuxUiState& state) {
  return state.last_changelog_exit_version != AWJ_BUILD_VERSION;
}

bool linux_changelog_visible_for_current_session(const LinuxUiState& state) {
  const bool first_start = linux_changelog_first_start_for_current_version(state);
  if (!state.show_update_changelog) {
    // 总开关关闭时，升级后的首次启动仍临时显示一次；该版本退出后隐藏。
    return first_start;
  }
  return !state.hide_update_changelog_after_exit || first_start;
}

bool linux_changelog_should_open_on_start(const LinuxUiState& state) {
  return linux_changelog_first_start_for_current_version(state) &&
         (!state.show_update_changelog || state.show_update_changelog_after_update);
}

bool linux_pending_update_is_newer(const LinuxUiState& state) {
  const auto current = awj::update::parse_version(AWJ_BUILD_VERSION);
  const auto pending = awj::update::parse_version(state.pending_update_version);
  const auto channel = awj::update::parse_channel(state.pending_update_channel);
  const auto preference = state.update_channel == "prerelease"
                              ? awj::update::ChannelPreference::stable_and_prerelease
                              : awj::update::ChannelPreference::stable_only;
  return current && pending && channel && *pending > *current &&
         awj::update::channel_visible_to(*channel, preference);
}

void sync_linux_update_ui(AwjStudio& app, const LinuxUiState& state) {
  const bool english = app.get_language_index() == 1;
  const bool available = linux_pending_update_is_newer(state);
  app.set_current_version(to_shared(AWJ_BUILD_VERSION));
  app.set_update_channel_index(state.update_channel == "prerelease" ? 1 : 0);
  app.set_show_update_changelog_enabled(state.show_update_changelog);
  app.set_show_update_changelog(linux_changelog_visible_for_current_session(state));
  app.set_hide_update_changelog_after_exit(
      state.hide_update_changelog_after_exit);
  app.set_show_update_changelog_after_update(
      state.show_update_changelog_after_update);
  app.set_update_available(available);
  app.set_update_version(to_shared(available ? state.pending_update_version : ""));
  app.set_update_published_at(
      to_shared(available ? state.pending_update_published_at : ""));
  app.set_update_changelog_zh_cn(
      to_shared(available ? state.pending_update_changelog_zh_cn : ""));
  app.set_update_changelog_en(
      to_shared(available ? state.pending_update_changelog_en : ""));
  app.set_update_summary_zh_cn(to_shared(
      available ? linux_update_summary(state.pending_update_changelog_zh_cn) : ""));
  app.set_update_summary_en(to_shared(
      available ? linux_update_summary(state.pending_update_changelog_en) : ""));
  const auto last = linux_update_check_time(state.last_successful_update_check_at);
  app.set_update_last_successful_check(
      to_shared(last.empty() ? (english ? "Never" : "从未") : last));
  app.set_update_status(
      to_shared(english ? state.update_status_en : state.update_status_zh));
}

void sync_linux_update_history(
    const std::shared_ptr<awj::ui::DeferredModel<UpdateHistoryRow>>& rows,
    const awj::update::Manifest& manifest) {
  if (!rows) return;
  rows->set_loader([manifest] {
  auto merged_history = awj::ui::embedded_changelog_history();
  for (const auto& entry : manifest.entries) {
    const auto version = awj::update::to_string(entry.version);
    const auto signed_entry = awj::ui::ChangelogHistoryEntry{
        .version = version,
        .channel = std::string{awj::update::channel_name(entry.channel)},
        .published_at = entry.published_at,
        .release_url = entry.release_url,
        .changelog_zh_cn = entry.changelog.zh_cn,
        .changelog_en = entry.changelog.en};
    const auto existing = std::ranges::find_if(
        merged_history, [&](const auto& item) { return item.version == version; });
    if (existing == merged_history.end()) {
      merged_history.push_back(signed_entry);
    } else {
      *existing = signed_entry;
    }
  }
  std::ranges::sort(merged_history, [](const auto& lhs, const auto& rhs) {
    const auto left = awj::update::parse_version(lhs.version);
    const auto right = awj::update::parse_version(rhs.version);
    return left && right ? *left > *right : lhs.version > rhs.version;
  });
  std::vector<UpdateHistoryRow> history_rows;
  history_rows.reserve(merged_history.size());
  for (const auto& entry : merged_history) {
    history_rows.push_back(UpdateHistoryRow{
        .version = to_shared(entry.version),
        .channel = to_shared(entry.channel),
        .published_at = to_shared(entry.published_at),
        .release_url = to_shared(entry.release_url),
        .changelog_zh_cn = to_shared(entry.changelog_zh_cn),
        .changelog_en = to_shared(entry.changelog_en)});
  }
  return history_rows;
  });
}

void restore_cached_linux_update_history(LinuxUiState& state) {
  if (state.update_manifest_v2_raw.empty() ||
      state.update_manifest_v2_signature.empty() ||
      state.update_keyring_raw.empty() || state.update_keyring_signature.empty()) {
    return;
  }
  auto keyring = awj::update::verify_and_parse_update_keyring(
      state.update_keyring_raw, state.update_keyring_signature);
  if (!keyring) {
    state.update_manifest_v2_raw.clear();
    state.update_manifest_v2_signature.clear();
    state.update_keyring_raw.clear();
    state.update_keyring_signature.clear();
    return;
  }
  auto manifest = awj::update::verify_and_parse_archive_manifest_v2(
      state.update_manifest_v2_raw, state.update_manifest_v2_signature, *keyring);
  if (!manifest ||
      manifest->sequence <
          static_cast<std::uint64_t>(std::max<std::int64_t>(
              state.last_verified_manifest_v2_sequence, 0)) ||
      manifest->sequence >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    state.update_manifest_v2_raw.clear();
    state.update_manifest_v2_signature.clear();
    state.update_keyring_raw.clear();
    state.update_keyring_signature.clear();
    return;
  }
  state.last_verified_manifest_v2_sequence =
      std::max(state.last_verified_manifest_v2_sequence,
               static_cast<std::int64_t>(manifest->sequence));
  sync_linux_update_history(
      state.update_history_rows,
      awj::update::archive_manifest_v2_for_history(*manifest));
}

std::expected<void, std::string> atomic_write_linux_file(
    const fs::path& path, std::string_view bytes) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto temporary = path;
  temporary += std::format(".tmp-{}-{}", static_cast<long long>(::getpid()),
                           stamp);
  const int descriptor = ::open(temporary.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return std::unexpected{std::format("无法创建配置临时文件：{}",
                                       std::strerror(errno))};
  }
  bool ok = true;
  std::string error{};
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto count = ::write(descriptor, bytes.data() + written,
                               bytes.size() - written);
    if (count < 0) {
      if (errno == EINTR) continue;
      ok = false;
      error = std::format("写入配置失败：{}", std::strerror(errno));
      break;
    }
    if (count == 0) {
      ok = false;
      error = "写入配置失败：未写入任何字节";
      break;
    }
    written += static_cast<std::size_t>(count);
  }
  if (ok && ::fsync(descriptor) != 0) {
    ok = false;
    error = std::format("刷新配置失败：{}", std::strerror(errno));
  }
  if (::close(descriptor) != 0 && ok) {
    ok = false;
    error = std::format("关闭配置失败：{}", std::strerror(errno));
  }
  if (!ok) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return std::unexpected{std::move(error)};
  }
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    const auto message = std::format("原子替换配置失败：{}", std::strerror(errno));
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return std::unexpected{message};
  }
  const int directory = ::open(path.parent_path().c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) {
    return std::unexpected{std::format("无法打开配置目录进行刷新：{}",
                                       std::strerror(errno))};
  }
  const int sync_result = ::fsync(directory);
  const auto sync_error = errno;
  ::close(directory);
  if (sync_result != 0) {
    return std::unexpected{std::format("刷新配置目录失败：{}",
                                       std::strerror(sync_error))};
  }
  return {};
}

std::int64_t linux_config_int64(const nlohmann::ordered_json& document,
                                std::string_view key) {
  const auto it = document.find(std::string{key});
  if (it == document.end()) return 0;
  try {
    if (it->is_number_unsigned()) {
      const auto value = it->get<std::uint64_t>();
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())
                 ? static_cast<std::int64_t>(value)
                 : 0;
    }
    if (it->is_number_integer()) {
      const auto value = it->get<std::int64_t>();
      return value >= 0 ? value : 0;
    }
    return 0;
  } catch (const nlohmann::json::exception&) {
    return 0;
  }
}

std::string linux_config_string(const nlohmann::ordered_json& document,
                                std::string_view key) {
  const auto it = document.find(std::string{key});
  return it != document.end() && it->is_string() ? it->get<std::string>()
                                                 : std::string{};
}

void load_linux_update_config(AwjStudio& app, LinuxUiState& state) {
  auto executable = awj::executable_path();
  if (!executable) {
    state.config_readable = false;
    state.update_status_zh = std::format("无法定位 AWJ.jsonc：{}", executable.error());
    state.update_status_en = "AWJ.jsonc could not be located.";
    return;
  }
  state.config_path = executable->parent_path() / "AWJ.jsonc";
  std::error_code ec;
  if (!fs::exists(state.config_path, ec)) return;
  try {
    std::ifstream input{state.config_path, std::ios::binary};
    if (!input) throw std::runtime_error{"无法读取配置文件"};
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    state.config_document = nlohmann::ordered_json::parse(
        bytes.begin(), bytes.end(), nullptr, true, true);
    if (!state.config_document.is_object()) {
      throw std::runtime_error{"配置根值不是对象"};
    }
  } catch (const std::exception& error) {
    state.config_readable = false;
    state.update_status_zh = std::format("AWJ.jsonc 无法安全读取：{}", error.what());
    state.update_status_en = "AWJ.jsonc could not be read safely.";
    return;
  }
  const auto apply_int = [&](std::string_view key, int minimum, int maximum,
                             auto setter) {
    const auto it = state.config_document.find(std::string{key});
    if (it == state.config_document.end() ||
        !(it->is_number_integer() || it->is_number_unsigned())) {
      return;
    }
    try {
      const auto value = it->get<long long>();
      if (value >= minimum && value <= maximum) {
        (app.*setter)(static_cast<int>(value));
      }
    } catch (const nlohmann::json::exception&) {
    }
  };
  const auto apply_bool = [&](std::string_view key, auto setter) {
    const auto it = state.config_document.find(std::string{key});
    if (it != state.config_document.end() && it->is_boolean()) {
      (app.*setter)(it->get<bool>());
    }
  };
  const auto font = state.config_document.find("ui_font_family");
  if (font != state.config_document.end() && font->is_string()) {
    app.set_ui_font_family(to_shared(font->get<std::string>()));
  }
  apply_int("theme_index", 0, 2, &AwjStudio::set_theme_index);
  apply_int("language_index", 0, 1, &AwjStudio::set_language_index);
  apply_bool("allow_wic_fallback", &AwjStudio::set_allow_wic_fallback);
  apply_bool("visual_quality_gpu", &AwjStudio::set_visual_quality_gpu);
  apply_bool("visual_quality_fallback", &AwjStudio::set_visual_quality_fallback);
  try {
    static_cast<void>(slint::select_bundled_translation(
        app.get_language_index() == 1 ? "en" : "zh-CN"));
  } catch (...) {
  }
  const auto channel = linux_config_string(state.config_document, "update_channel");
  if (channel == "stable" || channel == "prerelease") {
    state.update_channel = channel;
  }
  if (const auto it = state.config_document.find("show_update_changelog");
      it != state.config_document.end() && it->is_boolean()) {
    state.show_update_changelog = it->get<bool>();
  }
  if (const auto it = state.config_document.find("hide_update_changelog_after_exit");
      it != state.config_document.end() && it->is_boolean()) {
    state.hide_update_changelog_after_exit = it->get<bool>();
  }
  if (const auto it = state.config_document.find("show_update_changelog_after_update");
      it != state.config_document.end() && it->is_boolean()) {
    state.show_update_changelog_after_update = it->get<bool>();
  }
  state.last_changelog_exit_version =
      linux_config_string(state.config_document, "last_changelog_exit_version");
  state.last_successful_update_check_at =
      linux_config_int64(state.config_document, "last_successful_update_check_at");
  state.last_verified_manifest_sequence =
      linux_config_int64(state.config_document, "last_verified_manifest_sequence");
  state.last_verified_manifest_v2_sequence = linux_config_int64(
      state.config_document, "last_verified_manifest_v2_sequence");
  state.pending_update_version =
      linux_config_string(state.config_document, "pending_update_version");
  state.pending_update_channel =
      linux_config_string(state.config_document, "pending_update_channel");
  state.pending_update_release_url =
      linux_config_string(state.config_document, "pending_update_release_url");
  state.pending_update_published_at =
      linux_config_string(state.config_document, "pending_update_published_at");
  state.pending_update_changelog_zh_cn =
      linux_config_string(state.config_document, "pending_update_changelog_zh_cn");
  state.pending_update_changelog_en =
      linux_config_string(state.config_document, "pending_update_changelog_en");
  state.update_manifest_raw =
      linux_config_string(state.config_document, "update_manifest_raw");
  state.update_manifest_signature =
      linux_config_string(state.config_document, "update_manifest_signature");
  state.update_manifest_v2_raw =
      linux_config_string(state.config_document, "update_manifest_v2_raw");
  state.update_manifest_v2_signature = linux_config_string(
      state.config_document, "update_manifest_v2_signature");
  state.update_keyring_raw =
      linux_config_string(state.config_document, "update_keyring_raw");
  state.update_keyring_signature =
      linux_config_string(state.config_document, "update_keyring_signature");
  load_linux_menu_params(state.config_document, state.menu_params);
}

std::expected<void, std::string> persist_linux_update_config(
    const AwjStudio& app, LinuxUiState& state) {
  if (!state.config_readable || state.config_path.empty()) {
    return std::unexpected{"AWJ.jsonc 当前不可安全写入。"};
  }
  // 根配置是白名单写入：迁移时故意不复制旧文档，以清理普通队列、路径、
  // 窗口状态以及曾经持久化的参数页数据。
  nlohmann::ordered_json document = nlohmann::ordered_json::object();
  document["theme_index"] = app.get_theme_index();
  document["language_index"] = app.get_language_index();
  document["ui_font_family"] = shared_to_string(app.get_ui_font_family());
  document["allow_wic_fallback"] = app.get_allow_wic_fallback();
  document["visual_quality_gpu"] = app.get_visual_quality_gpu();
  document["visual_quality_fallback"] = app.get_visual_quality_fallback();
  document["update_channel"] = state.update_channel;
  document["show_update_changelog"] = state.show_update_changelog;
  document["hide_update_changelog_after_exit"] =
      state.hide_update_changelog_after_exit;
  document["show_update_changelog_after_update"] =
      state.show_update_changelog_after_update;
  document["last_changelog_exit_version"] = state.last_changelog_exit_version;
  document["last_successful_update_check_at"] =
      state.last_successful_update_check_at;
  document["last_verified_manifest_sequence"] =
      state.last_verified_manifest_sequence;
  document["last_verified_manifest_v2_sequence"] =
      state.last_verified_manifest_v2_sequence;
  document["pending_update_version"] = state.pending_update_version;
  document["pending_update_channel"] = state.pending_update_channel;
  document["pending_update_release_url"] = state.pending_update_release_url;
  document["pending_update_published_at"] = state.pending_update_published_at;
  document["pending_update_changelog_zh_cn"] =
      state.pending_update_changelog_zh_cn;
  document["pending_update_changelog_en"] = state.pending_update_changelog_en;
  document["update_manifest_raw"] = state.update_manifest_raw;
  document["update_manifest_signature"] = state.update_manifest_signature;
  document["update_manifest_v2_raw"] = state.update_manifest_v2_raw;
  document["update_manifest_v2_signature"] = state.update_manifest_v2_signature;
  document["update_keyring_raw"] = state.update_keyring_raw;
  document["update_keyring_signature"] = state.update_keyring_signature;
  document["menu_params"] = nlohmann::ordered_json::array();
  for (const auto& params : state.menu_params) {
    document["menu_params"].push_back(linux_menu_params_json(params));
  }
  auto bytes = document.dump(2);
  bytes.push_back('\n');
  auto saved = atomic_write_linux_file(state.config_path, bytes);
  if (!saved) return saved;
  state.config_document = std::move(document);
  return {};
}

awj::update::ChannelPreference linux_update_preference(
    const LinuxUiState& state) {
  return state.update_channel == "prerelease"
             ? awj::update::ChannelPreference::stable_and_prerelease
             : awj::update::ChannelPreference::stable_only;
}

std::expected<void, std::string> open_linux_url(std::string url) {
  if (!awj::update::parse_allowed_https_url(url)) {
    return std::unexpected{"拒绝打开不受信任的更新 URL。"};
  }
  const auto spawn = [&](const char* command, std::vector<char*> arguments)
      -> std::optional<pid_t> {
    pid_t child = 0;
    arguments.push_back(nullptr);
    if (::posix_spawnp(&child, command, nullptr, nullptr, arguments.data(),
                       environ) != 0) {
      return std::nullopt;
    }
    return child;
  };
  std::string xdg{"xdg-open"};
  if (auto child = spawn(xdg.c_str(), {xdg.data(), url.data()})) {
    std::thread{[pid = *child] { ::waitpid(pid, nullptr, 0); }}.detach();
    return {};
  }
  std::string gio{"gio"};
  std::string open{"open"};
  if (auto child = spawn(gio.c_str(), {gio.data(), open.data(), url.data()})) {
    std::thread{[pid = *child] { ::waitpid(pid, nullptr, 0); }}.detach();
    return {};
  }
  return std::unexpected{"未找到可用的 URL 打开工具（xdg-open/gio）。"};
}

void start_linux_update_check(slint::ComponentWeakHandle<AwjStudio> weak,
                              const std::shared_ptr<LinuxUiState>& state) {
  if (state->update_check_active) return;
  if (state->update_worker.joinable()) state->update_worker.join();
  state->update_check_active = true;
  if (auto app = weak.lock()) {
    (*app)->set_update_checking(true);
    state->update_status_zh = "正在检查更新…";
    state->update_status_en = "Checking for updates...";
    sync_linux_update_ui(**app, *state);
  }
  const auto last_sequence = state->last_verified_manifest_v2_sequence < 0
                                 ? std::uint64_t{0}
                                 : static_cast<std::uint64_t>(
                                       state->last_verified_manifest_v2_sequence);
  const auto preference = linux_update_preference(*state);
  state->update_worker = std::jthread(
      [weak, state, last_sequence, preference](std::stop_token token) {
        auto fetched = awj::update::fetch_verified_archive_manifest_v2(
            last_sequence, token);
        static_cast<void>(slint::invoke_from_event_loop(
            [weak, state, preference, fetched = std::move(fetched)]() mutable {
              auto app = weak.lock();
              if (!app) return;
              state->update_check_active = false;
              (*app)->set_update_checking(false);
              if (!fetched) {
                state->update_status_zh =
                    std::format("检查失败：{}", fetched.error());
                state->update_status_en = "Update check failed.";
                sync_linux_update_ui(**app, *state);
                return;
              }
              if (fetched->manifest.sequence >
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
                state->update_status_zh =
                    "检查失败：manifest sequence 超出本机范围。";
                state->update_status_en =
                    "Update check failed: sequence is out of range.";
                sync_linux_update_ui(**app, *state);
                return;
              }
              const auto before = capture_linux_update_state(*state);
              state->update_manifest_v2_raw = fetched->raw_bytes;
              state->update_manifest_v2_signature = fetched->signature_base64;
              state->update_keyring_raw = fetched->keyring_raw_bytes;
              state->update_keyring_signature =
                  fetched->keyring_signature_envelope;
              if (const auto pending =
                      awj::update::parse_version(state->pending_update_version);
                  pending && awj::update::should_clear_pending_for_revocation(
                                 awj::update::archive_manifest_v2_for_history(
                                     fetched->manifest),
                                 *pending)) {
                clear_linux_pending_update(*state);
              }
              const auto current = awj::update::parse_version(AWJ_BUILD_VERSION);
              if (!current) {
                restore_linux_update_state(*state, before);
                state->update_status_zh = "检查失败：当前构建版本号非法。";
                state->update_status_en =
                    "Update check failed: invalid build version.";
                sync_linux_update_ui(**app, *state);
                return;
              }
              const auto candidate = awj::update::select_archive_candidate_v2(
                  fetched->manifest,
                  {.current_version = *current,
                   .updater_version = *current,
                   .preference = preference});
              if (candidate) {
                state->pending_update_version =
                    awj::update::to_string(candidate->version);
                state->pending_update_channel =
                    std::string{awj::update::channel_name(candidate->channel)};
                state->pending_update_release_url = candidate->release_url;
                state->pending_update_published_at = candidate->published_at;
                state->pending_update_changelog_zh_cn = candidate->changelog.zh_cn;
                state->pending_update_changelog_en = candidate->changelog.en;
              }
              state->last_successful_update_check_at =
                  std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
              state->last_verified_manifest_v2_sequence =
                  static_cast<std::int64_t>(fetched->manifest.sequence);
              const bool retained =
                  !candidate && linux_pending_update_is_newer(*state);
              state->update_status_zh =
                  candidate ? "发现可用更新。"
                            : retained ? "检查成功；保留之前发现的更新。"
                                       : "已是最新版。";
              state->update_status_en =
                  candidate ? "An update is available."
                            : retained
                                  ? "Check succeeded; the previously found update remains available."
                                  : "Up to date.";
              if (auto saved = persist_linux_update_config(**app, *state); !saved) {
                restore_linux_update_state(*state, before);
                state->update_status_zh =
                    std::format("检查失败：无法持久化状态：{}", saved.error());
                state->update_status_en =
                    "Update check failed: state could not be saved.";
              } else {
                sync_linux_update_history(
                    state->update_history_rows,
                    awj::update::archive_manifest_v2_for_history(
                        fetched->manifest));
              }
              sync_linux_update_ui(**app, *state);
            }));
      });
}

std::wstring wide_from_shared(const slint::SharedString& value) {
  return awj::wide_from_utf8(shared_to_string(value));
}

LinuxMenuParams capture_linux_menu_params(const AwjStudio& app) {
  return LinuxMenuParams{
      .quality_text = shared_to_string(app.get_menu_quality_text()),
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
      .install_avif_png_command = app.get_menu_install_avif_png_command(),
      .size_limit_index = app.get_menu_size_limit_index(),
      .max_width_text = shared_to_string(app.get_menu_max_width_text()),
      .max_height_text = shared_to_string(app.get_menu_max_height_text()),
      .max_long_edge_text = shared_to_string(app.get_menu_max_long_edge_text()),
      .max_short_edge_text = shared_to_string(app.get_menu_max_short_edge_text()),
      .scale_percent_text = shared_to_string(app.get_menu_scale_percent_text())};
}

void apply_linux_menu_params(AwjStudio& app, const LinuxMenuParams& params) {
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
  app.set_menu_install_avif_png_command(params.install_avif_png_command);
  app.set_menu_size_limit_index(params.size_limit_index);
  app.set_menu_max_width_text(to_shared(params.max_width_text));
  app.set_menu_max_height_text(to_shared(params.max_height_text));
  app.set_menu_max_long_edge_text(to_shared(params.max_long_edge_text));
  app.set_menu_max_short_edge_text(to_shared(params.max_short_edge_text));
  app.set_menu_scale_percent_text(to_shared(params.scale_percent_text));
}

void store_linux_menu_params(AwjStudio& app, LinuxUiState& state) {
  state.menu_params[static_cast<std::size_t>(std::clamp(state.menu_format_index, 0, 4))] =
      capture_linux_menu_params(app);
}

LinuxMenuParams default_linux_menu_params(int format_index) {
  LinuxMenuParams params{};
  const auto format = [format_index] {
    switch (format_index) {
      case 1: return awj::OutputFormat::webp;
      case 2: return awj::OutputFormat::jxl;
      case 3: return awj::OutputFormat::jpgli;
      case 4: return awj::OutputFormat::png;
      default: return awj::OutputFormat::avif;
    }
  }();
  params.quality_text = std::format("{}", awj::default_quality_for(format));
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    params.speed_text = std::format("{}", awj::default_speed_for(format));
  }
  if (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli) {
    params.bit_depth_text = std::format("{}", awj::encoding_defaults::default_webp_bit_depth);
  }
  params.jpegli_progressive_index = awj::encoding_defaults::default_jpegli_progressive_level;
  params.jpegli_optimize_huffman = awj::encoding_defaults::default_jpegli_optimize_huffman;
  params.jpegli_xyb = awj::encoding_defaults::default_jpegli_xyb;
  params.jxl_jpeg_lossless = true;
  return params;
}


std::string stage_seconds_text(double seconds) {
  return seconds < 0.0 ? std::string{"-"} : std::format("{:.3f}s", seconds);
}

std::string stage_timings_text(double decode_seconds, double prepare_seconds,
                               double encode_seconds, double write_seconds) {
  if (decode_seconds < 0.0 && prepare_seconds < 0.0 &&
      encode_seconds < 0.0 && write_seconds < 0.0) {
    return {};
  }
  return std::format("decode {} · prepare {} · encode {} · write {}",
                     stage_seconds_text(decode_seconds),
                     stage_seconds_text(prepare_seconds),
                     stage_seconds_text(encode_seconds),
                     stage_seconds_text(write_seconds));
}

std::string linux_result_status_text(const awj::EncodeResult& result) {
  if (result.ok) {
    return result.skipped ? "已跳过" : "完成";
  }
  if (result.canceled) {
    return "已取消";
  }
  return result.message.empty() ? "失败"
                                : std::format("失败 · {}", result.message);
}

TaskRow task_row_from_result(const awj::EncodeResult& result) {
  return TaskRow{.order = to_shared(std::format("{}", result.index + 1)),
                 .filename = to_shared(awj::path_to_utf8(result.input_path.filename())),
                 .folder = to_shared(awj::path_to_utf8(result.input_path.parent_path())),
                 .size = to_shared(awj::format_size(result.original_bytes)),
                 .status = to_shared(linux_result_status_text(result)),
                 .output = to_shared(awj::path_to_utf8(result.output_path.filename())),
                 .log = to_shared(result.ok ? std::string{} : result.message),
                 .warning = !result.ok,
                 .locked = true,
                 .state = result.ok ? 2 : (result.canceled ? 4 : 3),
                 .input_path = to_shared(awj::path_to_utf8(result.input_path)),
                 .output_path = to_shared(awj::path_to_utf8(result.output_path)),
                 .encoder = to_shared(result.encoder_id),
                 .threads = result.encoder_threads > 0
                                ? to_shared(std::format("{}", result.encoder_threads))
                                : slint::SharedString{},
                 .stage_timings = to_shared(stage_timings_text(
                     result.decode_seconds, result.prepare_seconds,
                     result.encode_seconds, result.write_seconds))};
}

std::optional<std::size_t> linux_task_row_index_for_path(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const fs::path& path, std::optional<std::size_t> hint = {}) {
  if (!rows) {
    return std::nullopt;
  }
  const auto expected = awj::path_to_utf8(path);
  if (hint && *hint < rows->row_count()) {
    if (auto row = rows->row_data(*hint); row && shared_to_string(row->input_path) == expected) return hint;
  }
  for (std::size_t index = 0; index < rows->row_count(); ++index) {
    if (auto row = rows->row_data(index);
        row && shared_to_string(row->input_path) == expected) {
      return index;
    }
  }
  return std::nullopt;
}

void refresh_linux_queue_counts(
    AwjStudio& app,
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows) {
  int pending = 0;
  int running = 0;
  int success = 0;
  int failed = 0;
  if (rows) {
    for (std::size_t index = 0; index < rows->row_count(); ++index) {
      const auto row = rows->row_data(index);
      if (!row) {
        continue;
      }
      switch (row->state) {
        case 1:
          ++running;
          break;
        case 2:
          ++success;
          break;
        case 3:
          ++failed;
          break;
        case 0:
        case 4:
        default:
          ++pending;
          break;
      }
    }
  }
  app.set_queue_pending_count(pending);
  app.set_queue_running_count(running);
  app.set_queue_success_count(success);
  app.set_queue_failed_count(failed);
  if (!rows || app.get_selected_queue_index() >=
                   static_cast<int>(rows->row_count())) {
    app.set_selected_queue_index(-1);
  }
}

void push_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                   TaskRow row) noexcept {
  if (!rows) return;
  try {
    rows->push_back(std::move(row));
  } catch (...) {
  }
}

void mark_linux_task_row_running(AwjStudio& app,
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::EncodeResult& result) noexcept {
  if (!rows) return;
  try {
    if (const auto index = linux_task_row_index_for_path(rows, result.input_path, result.index)) {
      auto row = rows->row_data(*index);
      if (row) {
        row->status = to_shared("正在转码");
        row->locked = true;
        row->state = 1;
        awj::ui::replace_queue_row(app, rows, *index, *row);
        return;
      }
    }
    const auto previous_count = rows->row_count();
    push_task_row(rows,
                  TaskRow{.order = to_shared(std::format("{}", result.index + 1)),
                          .filename = to_shared(awj::path_to_utf8(
                              result.input_path.filename())),
                          .folder = to_shared(awj::path_to_utf8(
                              result.input_path.parent_path())),
                          .size = to_shared(awj::format_size(result.original_bytes)),
                          .status = to_shared("正在转码"),
                           .output = to_shared(awj::path_to_utf8(
                               result.output_path.filename())),
                           .locked = true,
                           .state = 1,
                           .input_path = to_shared(awj::path_to_utf8(result.input_path)),
                           .output_path = to_shared(awj::path_to_utf8(result.output_path))});
    if (rows->row_count() > previous_count) awj::ui::adjust_queue_count(app, 1, 1);
  } catch (...) {
  }
}

void set_linux_task_row_result(
    AwjStudio& app,
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::EncodeResult& result) {
  auto row = task_row_from_result(result);
  if (const auto index = linux_task_row_index_for_path(rows, result.input_path, result.index)) {
    awj::ui::replace_queue_row(app, rows, *index, row);
  } else {
    const auto previous_count = rows->row_count();
    const int status = row.state;
    push_task_row(rows, std::move(row));
    if (rows->row_count() > previous_count) awj::ui::adjust_queue_count(app, status, 1);
  }
}

void add_large_image_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                              const awj::BatchLargeImageItem& item) noexcept {
  push_task_row(rows, TaskRow{.order = to_shared(std::format("{}", item.file.index + 1)),
                              .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
                              .folder = to_shared(awj::path_to_utf8(item.file.path.parent_path())),
                              .size = to_shared(awj::format_size(item.file.bytes)),
                              .status = to_shared("大图模式"),
                              .output = {},
                              .log = to_shared(std::format("{}；{}", awj::large_image_reason_name(item.decision.reason), item.decision.reason_text)),
                              .warning = false,
                              .locked = true});
}


bool linux_large_image_grid_available(const awj::BatchLargeImageItem& item) noexcept {
  if (!item.decision.available_grid) return false;
  // 与 Windows 分支同因：clamped 边缘 cell 是 pipeline 已支持的默认路径。
  const auto plan = awj::plan_grid(awj::GridPlanRequest{
      .width = item.dimensions.width,
      .height = item.dimensions.height,
      .mode = awj::GridMode::auto_grid,
      .clamped_padding_enabled =
          awj::encoding_defaults::default_experimental_clamped_grid_padding});
  return plan.has_value();
}



LargeImageRow make_linux_large_image_row(const awj::BatchLargeImageItem& item,
                                         std::string_view status) {
  const auto grid = linux_large_image_grid_available(item) ? "grid 可用" : "grid 不可用";
  return LargeImageRow{
      .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
      .dimensions = to_shared(std::format("{} x {}", item.dimensions.width, item.dimensions.height)),
      .reason = to_shared(std::format("{} · {}", awj::large_image_reason_name(item.decision.reason),
                                      item.decision.reason_text)),
      .actions = to_shared(grid),
      .status = to_shared(status),
      .grid_available = linux_large_image_grid_available(item)};
}

void set_linux_large_image_status(LinuxUiState& state, int index, std::string_view status) {
  if (!state.large_image_rows || index < 0 ||
      static_cast<std::size_t>(index) >= state.large_image_items.size()) {
    return;
  }
  try {
    state.large_image_rows->set_row_data(
        static_cast<std::size_t>(index),
        make_linux_large_image_row(state.large_image_items[static_cast<std::size_t>(index)], status));
  } catch (...) {
  }
}

void push_linux_large_image(LinuxUiState& state, awj::BatchLargeImageItem item) {
  if (!state.large_image_rows) return;
  try {
    auto row = make_linux_large_image_row(item, "等待选择");
    state.large_image_items.push_back(std::move(item));
    state.large_image_rows->push_back(std::move(row));
  } catch (...) {
  }
}
void set_input_path_preserving_output(AwjStudio& app, const fs::path& path) {
  app.set_input_path(to_shared(awj::path_to_utf8(path)));
  if (shared_to_string(app.get_output_dir()).empty()) {
    app.set_output_dir(to_shared(awj::path_to_utf8(awj::default_output_dir_for(path))));
  }
}

std::wstring linux_queue_path_key(const fs::path& path) {
  std::error_code ec;
  const auto absolute = fs::absolute(path, ec);
  return awj::wide_from_utf8((ec ? path : absolute).lexically_normal().native());
}

bool linux_queue_contains_path(const LinuxUiState& state, const fs::path& path) {
  return state.queue_path_keys.contains(linux_queue_path_key(path));
}

TaskRow pending_linux_queue_row(const awj::ImageFile& file, std::size_t index,
                                bool locked = false) {
  return TaskRow{
      .order = to_shared(std::format("{}", index + 1)),
      .filename = to_shared(awj::path_to_utf8(file.path.filename())),
      .folder = to_shared(awj::path_to_utf8(file.path.parent_path())),
      .size = to_shared(awj::format_size(file.bytes)),
      .status = to_shared("等待编码"),
      .output = file.resolved_output_path.empty()
                    ? slint::SharedString{}
                    : to_shared(awj::path_to_utf8(
                          file.resolved_output_path.filename())),
      .locked = locked,
      .state = 0,
      .input_path = to_shared(awj::path_to_utf8(file.path)),
      .output_path = to_shared(awj::path_to_utf8(file.resolved_output_path))};
}

void refresh_linux_pending_queue(AwjStudio& app, LinuxUiState& state,
                                 bool locked = false) {
  std::vector<TaskRow> rows;
  rows.reserve(state.queue_files.size());
  for (std::size_t index = 0; index < state.queue_files.size(); ++index) {
    rows.push_back(pending_linux_queue_row(state.queue_files[index], index,
                                           locked));
  }
  state.task_rows->set_vector(std::move(rows));
  refresh_linux_queue_counts(app, state.task_rows);
}

std::expected<bool, std::string> add_linux_queue_from_path(
    AwjStudio& app, LinuxUiState& state, const fs::path& path,
    bool update_input_path) {
  auto scan_cfg = awj::default_app_config();
  scan_cfg.input_path = path;
  scan_cfg.output_dir = fs::path{shared_to_string(app.get_output_dir())};
  scan_cfg.output_policy = awj::OutputPolicy::normal;
  std::vector<awj::ImageFile> scanned;
  if (auto result = awj::scan_images(scan_cfg, scanned); !result) {
    return std::unexpected{result.error()};
  }
  if (scanned.empty()) {
    return false;
  }

  std::size_t added = 0;
  for (auto& file : scanned) {
    std::error_code ec;
    const auto absolute = fs::absolute(file.path, ec);
    if (!ec) {
      file.path = absolute;
    }
    const auto key = linux_queue_path_key(file.path);
    if (state.queue_path_keys.contains(key)) {
      continue;
    }
    file.index = state.queue_files.size();
    file.source_extension_disambiguator.clear();
    file.extension_disambiguated = false;
    file.resolved_output_path.clear();
    file.output_path_resolved = false;
    const auto [position, inserted] = state.queue_path_keys.insert(key);
    if (!inserted) continue;
    try {
      state.queue_files.push_back(std::move(file));
    } catch (...) {
      state.queue_path_keys.erase(position);
      throw;
    }
    ++added;
  }
  if (update_input_path) {
    set_input_path_preserving_output(app, path);
  }
  refresh_linux_pending_queue(app, state);
  app.set_status_text(to_shared(
      added == 0 ? "没有新图片加入队列，重复项已跳过。"
                 : std::format("已加入 {} 张图片。", added)));
  return true;
}

std::expected<std::vector<awj::ImageFile>, std::string>
build_linux_queue_files(const awj::AppConfig& cfg,
                        const std::vector<awj::ImageFile>& queue,
                        const std::vector<fs::path>* only_paths = nullptr) {
  try {
    std::unordered_set<std::wstring> selected;
    if (only_paths != nullptr) {
      selected.reserve(only_paths->size());
      for (const auto& path : *only_paths) {
        selected.insert(linux_queue_path_key(path));
      }
    }
    std::vector<awj::ImageFile> files;
    files.reserve(queue.size());
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    const auto template_text = std::wstring_view{cfg.output_template};
    const bool needs_hash = template_text.find(L"{hash}") != std::wstring_view::npos ||
                            template_text.find(L"{hash8}") != std::wstring_view::npos;
    const bool needs_sha256 = template_text.find(L"{sha256}") != std::wstring_view::npos ||
                              template_text.find(L"{sha2568}") != std::wstring_view::npos ||
                              template_text.find(L"{sha256_8}") != std::wstring_view::npos;
    for (const auto& item : queue) {
      if (only_paths != nullptr &&
          !selected.contains(linux_queue_path_key(item.path))) {
        continue;
      }
      std::wstring hash;
      if (needs_hash) {
        if (auto result = awj::file_hash_token(item.path, hash); !result) {
          return std::unexpected{result.error()};
        }
      }
      std::wstring sha256;
      if (needs_sha256) {
        if (auto result = awj::file_sha256_token(item.path, sha256); !result) {
          return std::unexpected{result.error()};
        }
      }
      files.push_back(awj::make_image_file(files.size(), item.path,
                                           item.relative_dir, item.bytes, rng,
                                           std::move(hash), std::move(sha256)));
    }
    if (files.empty()) {
      return std::unexpected{"队列为空，或没有可重试的项目。"};
    }
    if (auto result = awj::apply_source_extension_disambiguation(cfg, files);
        !result) {
      return std::unexpected{result.error()};
    }
    if (auto result = awj::resolve_batch_output_paths(cfg, files); !result) {
      return std::unexpected{result.error()};
    }
    return files;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"构建队列运行快照时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"构建队列运行快照时数据超过运行时限制。"};
  } catch (const fs::filesystem_error&) {
    return std::unexpected{"构建队列运行快照时文件系统访问失败。"};
  }
}

std::expected<fs::path, std::string> create_linux_queue_manifest(
    std::uint64_t run_id, const std::vector<awj::ImageFile>& files) {
  try {
    std::error_code ec;
    const auto temp_dir = fs::temp_directory_path(ec);
    if (ec) {
      return std::unexpected{std::format("无法获取 Studio worker 临时目录：{}",
                                         ec.message())};
    }
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    for (int attempt = 0; attempt < 16; ++attempt) {
      const auto path = temp_dir / std::format(
          "AWJStudioQueue-{}-{}-{:016x}.awjq", static_cast<long long>(getpid()),
          run_id, rng());
      if (fs::exists(path, ec)) {
        ec.clear();
        continue;
      }
      if (auto written = awj::write_studio_queue_manifest(path, files);
          written) {
        return path;
      } else {
        fs::remove(path, ec);
        return std::unexpected{written.error()};
      }
    }
    return std::unexpected{"无法创建唯一的 Studio 队列 manifest。"};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"创建 Studio 队列 manifest 时内存不足。"};
  } catch (const fs::filesystem_error&) {
    return std::unexpected{"创建 Studio 队列 manifest 时文件系统访问失败。"};
  }
}

struct LinuxQueueDragPayload {
  std::wstring path_key{};
};

slint::DataTransfer make_linux_queue_drag_data(const LinuxUiState& state,
                                               int index) {
  slint::DataTransfer transfer;
  if (index < 0 || static_cast<std::size_t>(index) >= state.queue_files.size()) {
    return transfer;
  }
  transfer.set_user_data(
      LinuxQueueDragPayload{linux_queue_path_key(
          state.queue_files[static_cast<std::size_t>(index)].path)});
  return transfer;
}

std::optional<std::size_t> linux_queue_index_for_key(
    const LinuxUiState& state, std::wstring_view key) {
  for (std::size_t index = 0; index < state.queue_files.size(); ++index) {
    if (linux_queue_path_key(state.queue_files[index].path) == key) {
      return index;
    }
  }
  return std::nullopt;
}

slint::language::DragAction linux_queue_drag_can_drop(
    const LinuxUiState& state, slint::language::DropEvent event,
    int target_slot, bool running) {
  if (running) {
    return slint::language::DragAction::None;
  }
  const auto data = event.data.user_data();
  const auto* payload = std::any_cast<LinuxQueueDragPayload>(&data);
  if (payload == nullptr) {
    const auto text = event.data.plain_text();
    return text && !native_drop_paths(*text).empty()
               ? slint::language::DragAction::Copy
               : slint::language::DragAction::None;
  }
  const auto current = linux_queue_index_for_key(state, payload->path_key);
  if (!current || target_slot < 0 ||
      static_cast<std::size_t>(target_slot) > state.queue_files.size()) {
    return slint::language::DragAction::None;
  }
  auto target = static_cast<std::size_t>(target_slot);
  if (target > *current) {
    --target;
  }
  return target == *current ? slint::language::DragAction::None
                            : slint::language::DragAction::Move;
}

slint::language::DragAction linux_queue_drag_dropped(
    AwjStudio& app, LinuxUiState& state, slint::language::DropEvent event,
    int target_slot) {
  if (app.get_running()) {
    return slint::language::DragAction::None;
  }
  const auto data = event.data.user_data();
  const auto* payload = std::any_cast<LinuxQueueDragPayload>(&data);
  if (payload == nullptr) {
    const auto text = event.data.plain_text();
    if (!text) {
      return slint::language::DragAction::None;
    }
    const auto paths = native_drop_paths(*text);
    if (paths.empty()) {
      app.set_status_text(to_shared("拖入内容不包含本地文件或文件夹路径。"));
      return slint::language::DragAction::None;
    }
    std::string first_error;
    for (const auto& raw : paths) {
      const auto path = awj::normalize_path_argument(
          awj::wide_from_utf8(raw), "拖入队列");
      if (!path) {
        if (first_error.empty()) first_error = path.error();
        continue;
      }
      if (auto added = add_linux_queue_from_path(app, state, *path, false);
          !added && first_error.empty()) {
        first_error = added.error();
      }
    }
    if (!first_error.empty()) {
      app.set_status_text(to_shared(first_error));
    }
    return slint::language::DragAction::Copy;
  }
  const auto current = linux_queue_index_for_key(state, payload->path_key);
  if (!current || target_slot < 0 ||
      static_cast<std::size_t>(target_slot) > state.queue_files.size()) {
    return slint::language::DragAction::None;
  }
  auto target = static_cast<std::size_t>(target_slot);
  if (target > *current) {
    --target;
  }
  if (target == *current) {
    return slint::language::DragAction::None;
  }
  auto file = std::move(state.queue_files[*current]);
  state.drag_reordered = true;
  state.queue_files.erase(state.queue_files.begin() +
                          static_cast<std::ptrdiff_t>(*current));
  state.queue_files.insert(state.queue_files.begin() +
                               static_cast<std::ptrdiff_t>(target),
                           std::move(file));
  refresh_linux_pending_queue(app, state);
  return slint::language::DragAction::Move;
}
std::wstring format_arg(int index) {
  switch (index) {
    case 1:
      return L"webp";
    case 2:
      return L"jxl";
    case 3:
      return L"jpgli";
    case 4:
      return L"png";
    case 0:
    default:
      return L"avif";
  }
}

std::wstring collision_arg(int index) {
  switch (index) {
    case 1:
      return L"skip";
    case 2:
      return L"time";
    case 3:
      return L"random";
    case 0:
    default:
      return L"overwrite";
  }
}
std::wstring avif_encoder_arg(int index) {
  return index == 0 ? L"auto" : index == 1 ? L"aom" : L"invalid";
}

std::wstring avif_color_representation_arg(int index) {
  switch (index) {
    case 1: return L"source";
    case 2: return L"rgb";
    default: return L"yuv";
  }
}

std::wstring chroma_arg(int index) {
  switch (index) {
    case 1: return L"444";
    case 2: return L"422";
    case 3: return L"420";
    default: return L"auto";
  }
}

std::wstring alpha_arg(int index) {
  switch (index) {
    case 0: return L"force";
    case 2: return L"off";
    default: return L"auto";
  }
}

std::wstring size_limit_arg(int index) {
  switch (index) {
    case 1: return L"none";
    case 2: return L"manual";
    default: return L"auto";
  }
}

awj::OutputFormat linux_output_format_from_index(int index) noexcept {
  switch (index) {
    case 1: return awj::OutputFormat::webp;
    case 2: return awj::OutputFormat::jxl;
    case 3: return awj::OutputFormat::jpgli;
    case 4: return awj::OutputFormat::png;
    default: return awj::OutputFormat::avif;
  }
}

struct LinuxQueueFormatChoice {
  int format_index{};
  bool append_png_suffix{};
};

constexpr LinuxQueueFormatChoice linux_queue_format_choice_from_index(
    int index) noexcept {
  const int choice = std::clamp(index, 0, 5);
  if (choice == 1) {
    return {.format_index = 0, .append_png_suffix = true};
  }
  return {.format_index = choice == 0 ? 0 : choice - 1,
          .append_png_suffix = false};
}

static_assert(linux_queue_format_choice_from_index(0).format_index == 0);
static_assert(linux_queue_format_choice_from_index(1).format_index == 0 &&
              linux_queue_format_choice_from_index(1).append_png_suffix);
static_assert(linux_queue_format_choice_from_index(2).format_index == 1);
static_assert(linux_queue_format_choice_from_index(3).format_index == 2);
static_assert(linux_queue_format_choice_from_index(4).format_index == 3);
static_assert(linux_queue_format_choice_from_index(5).format_index == 4);

int linux_parameter_editor_format_index(int index) noexcept {
  return index >= 0 && index < 5 ? index : 0;
}

int linux_avif_color_representation_index(
    awj::AvifColorRepresentation value) noexcept {
  switch (value) {
    case awj::AvifColorRepresentation::source: return 1;
    case awj::AvifColorRepresentation::rgb_identity: return 2;
    default: return 0;
  }
}

LinuxParameterParams default_linux_parameter_params(int index) {
  const auto format = linux_output_format_from_index(index);
  LinuxParameterParams params{};
  params.quality_text = std::format("{}", awj::default_quality_for(format));
  if (format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jxl) {
    params.speed_text = std::format("{}", awj::default_speed_for(format));
  }
  if (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli) {
    params.bit_depth_text = std::format("{}", awj::encoding_defaults::default_webp_bit_depth);
  }
  params.jpegli_progressive_index =
      awj::encoding_defaults::default_jpegli_progressive_level;
  params.jpegli_optimize_huffman =
      awj::encoding_defaults::default_jpegli_optimize_huffman;
  params.jpegli_xyb = awj::encoding_defaults::default_jpegli_xyb;
  params.jxl_jpeg_lossless = true;
  return params;
}

LinuxParameterParams capture_linux_parameter_params(const AwjStudio& app) {
  return {.quality_text = shared_to_string(app.get_quality_text()),
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

void apply_linux_parameter_params(AwjStudio& app,
                                  const LinuxParameterParams& params,
                                  int format_index) {
  const auto format = linux_output_format_from_index(format_index);
  const bool png_lossless = format == awj::OutputFormat::png;
  app.set_quality_text(to_shared(params.quality_text));
  app.set_visual_quality_text(
      to_shared(png_lossless ? std::string{} : params.visual_quality_text));
  app.set_bit_depth_text(to_shared(params.bit_depth_text));
  app.set_speed_text(to_shared(params.speed_text));
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
      params.quality_text == std::format("{}", awj::default_quality_for(format)));
  app.set_bit_depth_follows_format(
      (format == awj::OutputFormat::webp || format == awj::OutputFormat::jpgli)
          ? params.bit_depth_text == std::format(
                "{}", awj::encoding_defaults::default_webp_bit_depth)
          : params.bit_depth_text.empty());
}

std::array<LinuxParameterParams, 5>& active_linux_parameter_params(
    LinuxUiState& state) {
  return state.parameter_preset_index == 0 ? state.builtin_params
                                           : state.parameter_preset_params;
}

const std::array<LinuxParameterParams, 5>& active_linux_parameter_params(
    const LinuxUiState& state) {
  return state.parameter_preset_index == 0 ? state.builtin_params
                                           : state.parameter_preset_params;
}

void store_current_linux_parameter_params(AwjStudio& app, LinuxUiState& state) {
  const int index = linux_parameter_editor_format_index(state.last_format_index);
  auto params = capture_linux_parameter_params(app);
  const auto format = linux_output_format_from_index(index);
  if (format == awj::OutputFormat::png) {
    params.visual_quality_text.clear();
  }
  if ((format == awj::OutputFormat::avif || format == awj::OutputFormat::webp ||
       format == awj::OutputFormat::jxl) && trim_copy(params.speed_text).empty()) {
    params.speed_text = std::format("{}", awj::default_speed_for(format));
  }
  active_linux_parameter_params(state)[static_cast<std::size_t>(index)] =
      std::move(params);
}

void apply_linux_format_parameters(AwjStudio& app, LinuxUiState& state,
                                   int index) {
  store_current_linux_parameter_params(app, state);
  state.last_format_index = linux_parameter_editor_format_index(index);
  if (app.get_format_index() != state.last_format_index) {
    app.set_format_index(state.last_format_index);
  }
  apply_linux_parameter_params(
      app, active_linux_parameter_params(state)[static_cast<std::size_t>(
               state.last_format_index)],
      state.last_format_index);
}

void push_flag(std::vector<std::wstring>& args, bool enabled,
               std::wstring enabled_arg, std::wstring disabled_arg) {
  args.push_back(enabled ? std::move(enabled_arg) : std::move(disabled_arg));
}

void push_option(std::vector<std::wstring>& args, std::wstring option,
                 std::wstring value) {
  if (!value.empty()) {
    args.push_back(std::move(option));
    args.push_back(std::move(value));
  }
}

std::expected<awj::AppConfig, std::string> linux_config_from_parameter_params(
    const AwjStudio* app, int format_index, const LinuxParameterParams& params) {
  std::vector<std::wstring> args;
  push_option(args, L"--format", format_arg(format_index));
  args.push_back(L"--no-wic-fallback");
  const auto format = linux_output_format_from_index(format_index);
  const bool png_lossless = format == awj::OutputFormat::png;
  const auto visual_quality = png_lossless
                                  ? std::wstring{}
                                  : awj::wide_from_utf8(params.visual_quality_text);
  if (!trim_copy(visual_quality).empty()) {
    push_option(args, L"--visual-quality", visual_quality);
  } else {
    push_option(args, L"--quality",
                awj::wide_from_utf8(params.quality_text));
  }
  push_option(args, L"--threads", awj::wide_from_utf8(params.threads_text));
  push_option(args, L"--memory-limit",
              memory_arg_from_ui(awj::wide_from_utf8(params.memory_limit_text)));
  push_option(args, L"--bit-depth", awj::wide_from_utf8(params.bit_depth_text));
  push_option(args, L"--speed", awj::wide_from_utf8(params.speed_text));
  push_option(args, L"--image-size-limit", size_limit_arg(params.size_limit_index));
  if (params.size_limit_index == 2) {
    push_option(args, L"--max-width", awj::wide_from_utf8(params.max_width_text));
    push_option(args, L"--max-height", awj::wide_from_utf8(params.max_height_text));
    push_option(args, L"--max-long-edge", awj::wide_from_utf8(params.max_long_edge_text));
    push_option(args, L"--max-short-edge", awj::wide_from_utf8(params.max_short_edge_text));
    push_option(args, L"--scale-percent", awj::wide_from_utf8(params.scale_percent_text));
  }
  if (format_index == 0) {
    push_option(args, L"--avif-encoder", avif_encoder_arg(params.avif_encoder_index));
    push_option(args, L"--avif-color-representation",
                avif_color_representation_arg(
                    params.avif_color_representation_index));
    push_option(args, L"--chroma", chroma_arg(params.chroma_index));
    push_option(args, L"--alpha", alpha_arg(params.alpha_policy_index));
  } else if (format_index == 2) {
    if (!params.jxl_jpeg_lossless) {
      args.push_back(L"--no-jxl-jpeg-lossless");
    }
  } else if (format_index == 3) {
    push_option(args, L"--chroma", chroma_arg(params.chroma_index));
    push_option(args, L"--jpegli-progressive-level",
                std::to_wstring(params.jpegli_progressive_index));
    push_flag(args, params.jpegli_optimize_huffman,
              L"--jpegli-optimize-huffman", L"--no-jpegli-optimize-huffman");
    if (params.jpegli_xyb) {
      args.push_back(L"--jpegli-xyb");
    }
  }
  if (app != nullptr) {
    push_option(args, L"--input", wide_from_shared(app->get_input_path()));
    push_option(args, L"--output", wide_from_shared(app->get_output_dir()));
    push_option(args, L"--template", wide_from_shared(app->get_template_text()));
    push_option(args, L"--collision", collision_arg(app->get_collision_index()));
    if (!trim_copy(visual_quality).empty()) {
      push_flag(args, app->get_visual_quality_gpu(), L"--visual-quality-gpu",
                L"--no-visual-quality-gpu");
      push_flag(args, app->get_visual_quality_fallback(),
                L"--visual-quality-fallback", L"--no-visual-quality-fallback");
    }
    if (app->get_unlock_max_input_file_bytes()) {
      args.push_back(L"--unlock-max-input-file-bytes");
    }
    push_flag(args, app->get_strip_metadata(), L"--strip", L"--keep-metadata");
    push_flag(args, app->get_write_summary(), L"--summary", L"--no-summary");
    push_flag(args, app->get_write_log(), L"--log", L"--no-log");
  }

  auto parsed = awj::parse_arguments(args);
  if (!parsed) {
    return std::unexpected{parsed.error()};
  }
  return parsed->config;
}

int linux_avif_encoder_index(awj::AvifEncoderMode value) noexcept {
  return value == awj::AvifEncoderMode::aom ? 1 : 0;
}

int linux_chroma_index(awj::ChromaMode value) noexcept {
  switch (value) {
    case awj::ChromaMode::yuv444: return 1;
    case awj::ChromaMode::yuv422: return 2;
    case awj::ChromaMode::yuv420: return 3;
    default: return 0;
  }
}

int linux_alpha_index(awj::AlphaModePolicy value) noexcept {
  switch (value) {
    case awj::AlphaModePolicy::force: return 0;
    case awj::AlphaModePolicy::off: return 2;
    default: return 1;
  }
}

LinuxParameterParams linux_parameter_params_from_config(const awj::AppConfig& config) {
  const int index = [&] {
    switch (config.output_format) {
      case awj::OutputFormat::avif: return 0;
      case awj::OutputFormat::webp: return 1;
      case awj::OutputFormat::jxl: return 2;
      case awj::OutputFormat::jpgli: return 3;
      case awj::OutputFormat::png:
      default: return 4;
    }
  }();
  auto params = default_linux_parameter_params(index);
  params.quality_text = std::format("{}", config.quality);
  params.visual_quality_text = config.visual_quality
                                   ? std::format("{}", *config.visual_quality)
                                   : std::string{};
  params.bit_depth_text = config.bit_depth ? std::format("{}", *config.bit_depth)
                                            : std::string{};
  if (config.output_format == awj::OutputFormat::avif ||
      config.output_format == awj::OutputFormat::webp ||
      config.output_format == awj::OutputFormat::jxl) {
    params.speed_text = std::format(
        "{}", config.speed.value_or(awj::default_speed_for(config.output_format)));
  }
  params.avif_encoder_index = linux_avif_encoder_index(config.avif_encoder);
  params.avif_color_representation_index =
      linux_avif_color_representation_index(config.avif_color_representation);
  params.chroma_index = linux_chroma_index(config.chroma_mode);
  params.alpha_policy_index = linux_alpha_index(config.alpha_policy);
  params.jpegli_progressive_index = config.jpegli_progressive_level;
  params.jpegli_optimize_huffman = config.jpegli_optimize_huffman;
  params.jpegli_xyb = config.jpegli_xyb;
  params.jxl_jpeg_lossless = config.jxl_jpeg_lossless;
  params.threads_text = config.max_jobs == awj::default_max_jobs()
                            ? std::string{}
                            : std::format("{}", config.max_jobs);
  if (config.memory_limit_bytes != 0) {
    params.memory_limit_text = std::format(
        "{}", (config.memory_limit_bytes + awj::studio_defaults::bytes_per_gib - 1) /
                  awj::studio_defaults::bytes_per_gib);
  }
  switch (config.image_size_limit.mode) {
    case awj::ImageSizeLimitMode::none: params.size_limit_index = 1; break;
    case awj::ImageSizeLimitMode::manual: params.size_limit_index = 2; break;
    default: params.size_limit_index = 0; break;
  }
  const auto optional_text = [](const std::optional<int>& value) {
    return value ? std::format("{}", *value) : std::string{};
  };
  params.max_width_text = optional_text(config.image_size_limit.max_width);
  params.max_height_text = optional_text(config.image_size_limit.max_height);
  params.max_long_edge_text = optional_text(config.image_size_limit.max_long_edge);
  params.max_short_edge_text = optional_text(config.image_size_limit.max_short_edge);
  params.scale_percent_text = optional_text(config.image_size_limit.scale_percent);
  return params;
}

std::array<LinuxParameterParams, 5> linux_parameter_params_from_user_preset(
    const awj::UserPreset& preset) {
  std::array<LinuxParameterParams, 5> params{};
  for (int index = 0; index < static_cast<int>(params.size()); ++index) {
    params[static_cast<std::size_t>(index)] = linux_parameter_params_from_config(
        awj::config_from_user_preset(preset, linux_output_format_from_index(index)));
  }
  return params;
}

void reload_linux_user_preset_options(AwjStudio& app, LinuxUiState& state) {
  auto catalog = awj::list_user_presets();
  if (!catalog) {
    state.user_presets.clear();
    state.user_preset_errors = {catalog.error()};
  } else {
    state.user_presets = std::move(catalog->presets);
    state.user_preset_errors = std::move(catalog->errors);
  }
  std::vector<ComboOption> options;
  options.push_back({.text = to_shared("内置默认"), .enabled = true});
  for (const auto& preset : state.user_presets) {
    options.push_back({.text = to_shared(preset.name), .enabled = true});
  }
  app.set_queue_preset_options(
      std::make_shared<slint::VectorModel<ComboOption>>(options));
  app.set_parameter_preset_options(
      std::make_shared<slint::VectorModel<ComboOption>>(std::move(options)));
  if (app.get_queue_preset_index() > static_cast<int>(state.user_presets.size())) {
    app.set_queue_preset_index(0);
  }
  if (state.parameter_preset_index > static_cast<int>(state.user_presets.size())) {
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
          : to_shared(state.user_presets[static_cast<std::size_t>(queue_index - 1)]
                          .description));
  if (!state.user_preset_errors.empty()) {
    app.set_status_text(to_shared(
        std::format("发现无效用户预设：{}", state.user_preset_errors.front())));
  }
}

void select_linux_parameter_preset(AwjStudio& app, LinuxUiState& state, int index) {
  store_current_linux_parameter_params(app, state);
  index = std::clamp(index, 0, static_cast<int>(state.user_presets.size()));
  state.parameter_preset_index = index;
  if (index > 0) {
    state.parameter_preset_params = linux_parameter_params_from_user_preset(
        state.user_presets[static_cast<std::size_t>(index - 1)]);
  }
  app.set_parameter_preset_index(index);
  app.set_parameter_preset_description(
      index == 0 ? slint::SharedString{}
                 : to_shared(state.user_presets[static_cast<std::size_t>(index - 1)]
                                 .description));
  apply_linux_format_parameters(app, state, app.get_format_index());
}

void select_linux_queue_preset(AwjStudio& app, LinuxUiState& state, int index) {
  index = std::clamp(index, 0, static_cast<int>(state.user_presets.size()));
  app.set_queue_preset_index(index);
  app.set_queue_preset_description(
      index == 0
          ? slint::SharedString{}
          : to_shared(state.user_presets[static_cast<std::size_t>(index - 1)]
                          .description));
}

std::expected<awj::AppConfig, std::string> config_from_ui(
    AwjStudio& app, LinuxUiState& state) {
  store_current_linux_parameter_params(app, state);
  const auto queue_choice =
      linux_queue_format_choice_from_index(app.get_queue_format_index());
  const int format_index = queue_choice.format_index;
  const int preset_index = app.get_queue_preset_index();
  LinuxParameterParams params{};
  if (preset_index == 0) {
    params = state.builtin_params[static_cast<std::size_t>(format_index)];
  } else {
    const int user_index = preset_index - 1;
    if (user_index < 0 || static_cast<std::size_t>(user_index) >=
                              state.user_presets.size()) {
      return std::unexpected{"选中的用户预设已不存在，请重新选择。"};
    }
    params = linux_parameter_params_from_config(awj::config_from_user_preset(
        state.user_presets[static_cast<std::size_t>(user_index)],
        linux_output_format_from_index(format_index)));
  }
  auto config = linux_config_from_parameter_params(&app, format_index, params);
  if (config) {
    config->append_png_suffix = queue_choice.append_png_suffix;
  }
  return config;
}

std::expected<awj::UserPreset, std::string> linux_user_preset_from_parameters(
    std::string name, std::string description,
    const std::array<LinuxParameterParams, 5>& params) {
  auto preset = awj::default_user_preset();
  preset.name = std::move(name);
  preset.description = std::move(description);
  for (int index = 0; index < static_cast<int>(params.size()); ++index) {
    auto config = linux_config_from_parameter_params(
        nullptr, index, params[static_cast<std::size_t>(index)]);
    if (!config) {
      return std::unexpected{std::format("{} 预设参数错误：{}",
                                         awj::output_format_name(
                                             linux_output_format_from_index(index)),
                                         config.error())};
    }
    preset.formats[static_cast<std::size_t>(index)] =
        awj::preset_format_from_config(*config);
  }
  return preset;
}

std::string xml_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

std::string remove_awj_thunar_actions(std::string xml) {
  if (xml.empty()) return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<actions>\n</actions>\n";
  std::string out;
  std::size_t pos = 0;
  while (true) {
    auto start = xml.find("<action", pos);
    while (start != std::string::npos) {
      const auto after_name = start + std::string_view{"<action"}.size();
      if (after_name >= xml.size() || xml[after_name] == '>' ||
          xml[after_name] == ' ' || xml[after_name] == '\t' ||
          xml[after_name] == '\r' || xml[after_name] == '\n') {
        break;
      }
      // Do not treat the <actions> container as an individual action.
      start = xml.find("<action", after_name);
    }
    if (start == std::string::npos) {
      out += xml.substr(pos);
      break;
    }
    const auto end = xml.find("</action>", start);
    if (end == std::string::npos) {
      out += xml.substr(pos);
      break;
    }
    const auto block_end = end + std::string_view{"</action>"}.size();
    const auto block = xml.substr(start, block_end - start);
    out += xml.substr(pos, start - pos);
    if (block.find("<unique-id>awjimage-") == std::string::npos) out += block;
    pos = block_end;
  }
  return out;
}

void append_linux_shell_arg(std::string& command, std::string_view value) {
  command.push_back(' ');
  command += shell_quote(value);
}

void append_linux_shell_option(std::string& command, std::string_view option,
                               std::string_view value) {
  append_linux_shell_arg(command, option);
  append_linux_shell_arg(command, value);
}

std::string awj_cli_command(const fs::path& exe, int format_index,
                            const LinuxMenuParams& params,
                            bool append_png_suffix = false) {
  auto command = shell_quote(awj::path_to_utf8(exe));
  const auto format = awj::utf8_from_wide(format_arg(format_index));
  append_linux_shell_option(command, "--output-policy", "shell");
  append_linux_shell_option(command, "--format", format);
  append_linux_shell_option(command, "--collision", "number");
  append_linux_shell_arg(command, "--no-wic-fallback");

  const bool is_avif = format_index == 0;
  const bool is_webp = format_index == 1;
  const bool is_jxl = format_index == 2;
  const bool is_jpgli = format_index == 3;
  const bool is_png = format_index == 4;
  if (!trim_copy(params.quality_text).empty()) {
    append_linux_shell_option(command, "--quality", trim_copy(params.quality_text));
  }
  if ((is_avif || is_webp || is_jpgli || is_png) &&
      !trim_copy(params.bit_depth_text).empty()) {
    append_linux_shell_option(command, "--bit-depth", trim_copy(params.bit_depth_text));
  }
  if ((is_avif || is_webp || is_jxl) && !trim_copy(params.speed_text).empty()) {
    append_linux_shell_option(command, "--speed", trim_copy(params.speed_text));
  }
  append_linux_shell_arg(command, params.strip_metadata ? "--strip" : "--keep-metadata");
  append_linux_shell_option(command, "--image-size-limit",
                            awj::utf8_from_wide(size_limit_arg(params.size_limit_index)));
  if (params.size_limit_index == 2) {
    if (!trim_copy(params.max_width_text).empty()) append_linux_shell_option(command, "--max-width", trim_copy(params.max_width_text));
    if (!trim_copy(params.max_height_text).empty()) append_linux_shell_option(command, "--max-height", trim_copy(params.max_height_text));
    if (!trim_copy(params.max_long_edge_text).empty()) append_linux_shell_option(command, "--max-long-edge", trim_copy(params.max_long_edge_text));
    if (!trim_copy(params.max_short_edge_text).empty()) append_linux_shell_option(command, "--max-short-edge", trim_copy(params.max_short_edge_text));
    if (!trim_copy(params.scale_percent_text).empty()) append_linux_shell_option(command, "--scale-percent", trim_copy(params.scale_percent_text));
  }
  if (is_avif) {
    append_linux_shell_option(command, "--avif-encoder",
                              awj::utf8_from_wide(avif_encoder_arg(params.avif_encoder_index)));
    append_linux_shell_option(
        command, "--avif-color-representation",
        awj::utf8_from_wide(avif_color_representation_arg(
            params.avif_color_representation_index)));
    append_linux_shell_option(command, "--chroma",
                              awj::utf8_from_wide(chroma_arg(params.chroma_index)));
    append_linux_shell_option(command, "--alpha",
                              awj::utf8_from_wide(alpha_arg(params.alpha_policy_index)));
    if (append_png_suffix) {
      append_linux_shell_arg(command, "--append-png-suffix");
    }
  } else if (is_jxl) {
    if (!params.jxl_jpeg_lossless) {
      append_linux_shell_arg(command, "--no-jxl-jpeg-lossless");
    }
  } else if (is_jpgli) {
    append_linux_shell_option(command, "--chroma",
                              awj::utf8_from_wide(chroma_arg(params.chroma_index)));
    append_linux_shell_option(command, "--jpegli-progressive-level",
                              std::to_string(std::clamp(params.jpegli_progressive_index, 0, 2)));
    append_linux_shell_arg(command, params.jpegli_optimize_huffman
                                       ? "--jpegli-optimize-huffman"
                                       : "--no-jpegli-optimize-huffman");
    if (params.jpegli_xyb) append_linux_shell_arg(command, "--jpegli-xyb");
  }
  return command;
}

std::expected<void, std::string> validate_linux_menu_params(
    const std::array<LinuxMenuParams, 5>& menu_params) {
  constexpr std::array<std::string_view, 5> labels{"AVIF", "WebP", "JXL", "JPGLI", "PNG"};
  for (int format_index = 0; format_index < 5; ++format_index) {
    const auto& params = menu_params[static_cast<std::size_t>(format_index)];
    std::vector<std::wstring> args{L"--format", format_arg(format_index),
                                   L"--no-wic-fallback"};
    const bool is_avif = format_index == 0;
    const bool is_webp = format_index == 1;
    const bool is_jxl = format_index == 2;
    const bool is_jpgli = format_index == 3;
    const bool is_png = format_index == 4;
    push_option(args, L"--quality", awj::wide_from_utf8(trim_copy(params.quality_text)));
    if (is_avif || is_webp || is_jpgli || is_png) {
      push_option(args, L"--bit-depth", awj::wide_from_utf8(trim_copy(params.bit_depth_text)));
    }
    if (is_avif || is_webp || is_jxl) {
      push_option(args, L"--speed", awj::wide_from_utf8(trim_copy(params.speed_text)));
    }
    push_flag(args, params.strip_metadata, L"--strip", L"--keep-metadata");
    push_option(args, L"--image-size-limit", size_limit_arg(params.size_limit_index));
    if (params.size_limit_index == 2) {
      push_option(args, L"--max-width", awj::wide_from_utf8(trim_copy(params.max_width_text)));
      push_option(args, L"--max-height", awj::wide_from_utf8(trim_copy(params.max_height_text)));
      push_option(args, L"--max-long-edge", awj::wide_from_utf8(trim_copy(params.max_long_edge_text)));
      push_option(args, L"--max-short-edge", awj::wide_from_utf8(trim_copy(params.max_short_edge_text)));
      push_option(args, L"--scale-percent", awj::wide_from_utf8(trim_copy(params.scale_percent_text)));
    }
    if (is_avif) {
      push_option(args, L"--avif-encoder", avif_encoder_arg(params.avif_encoder_index));
      push_option(args, L"--avif-color-representation",
                  avif_color_representation_arg(
                      params.avif_color_representation_index));
      push_option(args, L"--chroma", chroma_arg(params.chroma_index));
      push_option(args, L"--alpha", alpha_arg(params.alpha_policy_index));
    } else if (is_jxl) {
      if (!params.jxl_jpeg_lossless) {
        args.push_back(L"--no-jxl-jpeg-lossless");
      }
    } else if (is_jpgli) {
      push_option(args, L"--chroma", chroma_arg(params.chroma_index));
      push_option(args, L"--jpegli-progressive-level",
                  std::to_wstring(std::clamp(params.jpegli_progressive_index, 0, 2)));
      push_flag(args, params.jpegli_optimize_huffman, L"--jpegli-optimize-huffman",
                L"--no-jpegli-optimize-huffman");
      if (params.jpegli_xyb) args.push_back(L"--jpegli-xyb");
    }
    auto parsed = awj::parse_arguments(args);
    if (!parsed) {
      return std::unexpected{std::format("{} 菜单参数错误：{}",
                                         labels[static_cast<std::size_t>(format_index)],
                                         parsed.error())};
    }
    if (auto valid = awj::validate_execution_config(parsed->config); !valid) {
      return std::unexpected{std::format("{} 菜单参数错误：{}",
                                         labels[static_cast<std::size_t>(format_index)],
                                         valid.error())};
    }
  }
  return {};
}

std::string nautilus_awj_script(const fs::path& exe, int format_index,
                                const LinuxMenuParams& params,
                                bool append_png_suffix = false) {
  std::string script = "#!/bin/sh\nset -eu\nfiles=${NAUTILUS_SCRIPT_SELECTED_FILE_PATHS:-}\n[ -n \"$files\" ] || exit 0\n";
  script += "printf '%s\n' \"$files\" | while IFS= read -r file; do\n";
  script += "  [ -n \"$file\" ] || continue\n";
  script += "  ";
  script += awj_cli_command(exe, format_index, params, append_png_suffix);
  script += " \"$file\"\n";
  script += "done\n";
  return script;
}

std::expected<void, std::string> write_nautilus_scripts(
    const fs::path& exe, const std::array<LinuxMenuParams, 5>& menu_params) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) return std::unexpected{"无法定位 HOME，不能写入 Nautilus 脚本。"};
  const auto scripts_dir = fs::path{home} / ".local" / "share" / "nautilus" / "scripts";
  std::error_code ec;
  fs::create_directories(scripts_dir, ec);
  if (ec) return std::unexpected{std::format("创建 Nautilus 脚本目录失败: {}", ec.message())};
  constexpr std::array<std::string_view, 5> labels{"AVIF", "WebP", "JXL", "JPGLI", "PNG"};
  for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
    const auto script_path = scripts_dir / std::format("AWJ 转换为 {}", labels[static_cast<std::size_t>(i)]);
    std::ofstream out{script_path, std::ios::binary | std::ios::trunc};
    if (!out) return std::unexpected{std::format("写入 Nautilus {} 脚本失败。", labels[static_cast<std::size_t>(i)])};
    out << nautilus_awj_script(exe, i, menu_params[static_cast<std::size_t>(i)]);
    out.close();
    fs::permissions(script_path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    if (ec) return std::unexpected{std::format("设置 Nautilus 脚本权限失败: {}", ec.message())};
  }
  if (menu_params.front().install_avif_png_command) {
    const auto script_path = scripts_dir / "AWJ 转换为 AVIF.png";
    std::ofstream out{script_path, std::ios::binary | std::ios::trunc};
    if (!out) return std::unexpected{"写入 Nautilus AVIF.png 脚本失败。"};
    out << nautilus_awj_script(exe, 0, menu_params.front(), true);
    out.close();
    fs::permissions(script_path,
                    fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    fs::perm_options::add, ec);
    if (ec) return std::unexpected{std::format("设置 Nautilus 脚本权限失败: {}", ec.message())};
  } else {
    fs::remove(scripts_dir / "AWJ 转换为 AVIF.png", ec);
  }
  return {};
}

std::expected<void, std::string> remove_nautilus_script() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) return {};
  std::error_code ec;
  constexpr std::array<std::string_view, 5> labels{"AVIF", "WebP", "JXL", "JPGLI", "PNG"};
  const auto scripts_dir = fs::path{home} / ".local" / "share" / "nautilus" / "scripts";
  for (const auto label : labels) fs::remove(scripts_dir / std::format("AWJ 转换为 {}", label), ec);
  fs::remove(scripts_dir / "AWJ 转换为 AVIF.png", ec);
  return {};
}
std::string thunar_action_xml(const fs::path& exe, int format_index,
                              const LinuxMenuParams& params,
                              bool append_png_suffix = false) {
  const auto format = append_png_suffix ? std::string{"AVIF.png"}
                                        : awj::utf8_from_wide(format_arg(format_index));
  auto command = awj_cli_command(exe, format_index, params, append_png_suffix);
  command += " %F";
  return std::format(
      "  <action>\n"
      "    <icon>image-x-generic</icon>\n"
      "    <name>{}</name>\n"
      "    <submenu>AWJimage 转换</submenu>\n"
      "    <unique-id>awjimage-{}</unique-id>\n"
      "    <command>{}</command>\n"
      "    <description>使用 AWJimage 转换选中的图片</description>\n"
      "    <patterns>*</patterns>\n"
      "    <startup-notify/>\n"
      "    <image-files/>\n"
      "    <directories/>\n"
      "  </action>\n",
      xml_escape(std::format("AWJ 转换为 {}", format)),
      xml_escape(append_png_suffix ? "avif-png" : format),
      xml_escape(command));
}

std::expected<void, std::string> write_thunar_actions(
    const std::array<LinuxMenuParams, 5>& menu_params) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) {
    return std::unexpected{"无法定位 HOME，不能写入 Thunar 右键菜单。"};
  }
  auto exe = awj::executable_path();
  if (!exe) return std::unexpected{exe.error()};
  const auto config_dir = fs::path{home} / ".config" / "Thunar";
  std::error_code ec;
  fs::create_directories(config_dir, ec);
  if (ec) return std::unexpected{std::format("创建 Thunar 配置目录失败: {}", ec.message())};
  const auto uca = config_dir / "uca.xml";
  std::string xml;
  if (std::ifstream in{uca}; in) {
    xml.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
  }
  xml = remove_awj_thunar_actions(std::move(xml));
  if (xml.find("</actions>") == std::string::npos) {
    xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<actions>\n</actions>\n";
  }
  std::string actions;
  for (int i = 0; i < 5; ++i) {
    actions += thunar_action_xml(*exe, i, menu_params[static_cast<std::size_t>(i)]);
  }
  if (menu_params.front().install_avif_png_command) {
    actions += thunar_action_xml(*exe, 0, menu_params.front(), true);
  }
  xml.insert(xml.rfind("</actions>"), actions);
  std::ofstream out{uca, std::ios::binary | std::ios::trunc};
  if (!out) return std::unexpected{"写入 Thunar 右键菜单失败。"};
  out << xml;
  return {};
}

std::expected<void, std::string> remove_thunar_actions() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) return {};
  const auto uca = fs::path{home} / ".config" / "Thunar" / "uca.xml";
  std::ifstream in{uca};
  if (!in) return {};
  std::string xml{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  xml = remove_awj_thunar_actions(std::move(xml));
  std::ofstream out{uca, std::ios::binary | std::ios::trunc};
  if (!out) return std::unexpected{"写入 Thunar 右键菜单失败。"};
  out << xml;
  return {};
}
std::expected<void, std::string> write_linux_file_manager_actions(
    const std::array<LinuxMenuParams, 5>& menu_params) {
  auto exe = awj::executable_path();
  if (!exe) return std::unexpected{exe.error()};
  auto nautilus = write_nautilus_scripts(*exe, menu_params);
  auto thunar = write_thunar_actions(menu_params);
  if (!nautilus && !thunar) {
    return std::unexpected{std::format("Nautilus: {}; Thunar: {}", nautilus.error(), thunar.error())};
  }
  return {};
}

std::expected<void, std::string> remove_linux_file_manager_actions() {
  auto nautilus = remove_nautilus_script();
  auto thunar = remove_thunar_actions();
  if (!nautilus && !thunar) {
    return std::unexpected{std::format("Nautilus: {}; Thunar: {}", nautilus.error(), thunar.error())};
  }
  return {};
}

std::optional<std::string> linux_context_menu_warning(
    const std::array<LinuxMenuParams, 5>& menu_params) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string_view{home}.empty()) {
    return std::nullopt;
  }
  const auto scripts_dir = fs::path{home} / ".local" / "share" /
                           "nautilus" / "scripts";
  const auto avif_script = scripts_dir / "AWJ 转换为 AVIF";
  const auto avif_png_script = scripts_dir / "AWJ 转换为 AVIF.png";
  std::error_code ec;
  const bool avif_installed = fs::exists(avif_script, ec) && !ec;
  ec.clear();
  const bool avif_png_installed = fs::exists(avif_png_script, ec) && !ec;
  if (!avif_installed && !avif_png_installed) {
    return std::nullopt;
  }
  if (!avif_installed ||
      avif_png_installed != menu_params.front().install_avif_png_command) {
    return "右键菜单与当前 AVIF.png 设置不一致，请重新安装右键菜单。";
  }
  auto executable = awj::executable_path();
  if (!executable) {
    return "无法检查右键菜单程序路径，请重新安装右键菜单。";
  }
  std::ifstream input{avif_script, std::ios::binary};
  const std::string actual{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
  if (!input || actual != nautilus_awj_script(*executable, 0,
                                                menu_params.front())) {
    return "右键菜单与当前菜单参数不一致，请重新安装右键菜单。";
  }
  if (menu_params.front().install_avif_png_command) {
    std::ifstream png_input{avif_png_script, std::ios::binary};
    const std::string png_actual{std::istreambuf_iterator<char>{png_input},
                                 std::istreambuf_iterator<char>{}};
    if (!png_input ||
        png_actual != nautilus_awj_script(*executable, 0,
                                          menu_params.front(), true)) {
      return "右键菜单与当前 AVIF.png 设置不一致，请重新安装右键菜单。";
    }
  }
  return std::nullopt;
}
void set_format_quality(AwjStudio& app, int index) {
  const auto quality = [index] {
    switch (index) {
      case 1:
        return awj::default_quality_for(awj::OutputFormat::webp);
      case 2:
        return awj::default_quality_for(awj::OutputFormat::jxl);
      case 3:
        return awj::default_quality_for(awj::OutputFormat::jpgli);
      case 4:
        return awj::default_quality_for(awj::OutputFormat::png);
      case 0:
      default:
        return awj::default_quality_for(awj::OutputFormat::avif);
    }
  }();
  app.set_quality_text(to_shared(std::format("{}", quality)));
}

void initialize_ui(AwjStudio& app) {
  awj::ui::bind_queue_model(app, std::make_shared<slint::VectorModel<TaskRow>>());
  app.set_large_image_rows(std::make_shared<slint::VectorModel<LargeImageRow>>());
  app.set_wic_ui_visible(false);
  app.set_allow_wic_fallback(false);
  app.set_force_terminate_supported(false);
  app.set_menu_allow_wic_fallback(false);
  app.set_timestamp_ui_visible(false);
  app.set_visual_quality_fallback(true);
  app.set_visual_quality_gpu_help_text(to_shared("默认启用 Vulkan 加速 visual_quality 的 luma、GMSD 与 MS-SSIM 指标；codec 编码/解码仍使用 native CPU 库，失败或小图会自动回退 CPU。"));
  app.set_avif_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::avif))));
  app.set_webp_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::webp))));
  app.set_jxl_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::jxl))));
  app.set_jpegli_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::jpgli))));
  app.set_png_quality_default(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::png))));
  app.set_webp_bit_depth_default(to_shared(std::format("{}", awj::encoding_defaults::default_webp_bit_depth)));
  app.set_quality_text(to_shared(std::format("{}", awj::default_quality_for(awj::OutputFormat::avif))));
  app.set_template_text(to_shared(std::string{awj::encoding_defaults::default_output_template_text}));
  app.set_memory_limit_text(to_shared(std::string{awj::encoding_defaults::default_memory_limit_text}));
  app.set_unlock_max_input_file_bytes(false);
  app.set_status_text(to_shared("就绪"));
}

}  // namespace

bool finish_linux_close(AwjStudio& app, LinuxUiState& state) noexcept {
  try {
    state.update_worker.request_stop();
    const auto before = capture_linux_update_state(state);
    state.last_changelog_exit_version = AWJ_BUILD_VERSION;
    if (auto saved = persist_linux_update_config(app, state); !saved) {
      restore_linux_update_state(state, before);
      app.set_status_text(to_shared(saved.error()));
      state.close_requested = false;
      return false;
    }
    return true;
  } catch (...) {
    state.close_requested = false;
    return false;
  }
}

int run_studio_ui() {
  try {
    auto app = AwjStudio::create();
    app->set_shell_menu_injection_supported(false);
    if (auto recovered = awj::recover_user_preset_change(); !recovered)
      app->set_status_text(to_shared(recovered.error()));
    auto state = std::make_shared<LinuxUiState>();
    state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
    state->large_image_rows = std::make_shared<slint::VectorModel<LargeImageRow>>();
    state->update_history_rows =
        std::make_shared<awj::ui::DeferredModel<UpdateHistoryRow>>();
    sync_linux_update_history(state->update_history_rows,
                              awj::update::Manifest{.schema = 1});
    auto weak = slint::ComponentWeakHandle(app);
    app->on_settings_page_opened([weak, state] {
      if (auto app = weak.lock(); app && !state->ui_font_options_loaded) {
        load_system_font_options(**app);
        state->ui_font_options_loaded = true;
      }
    });
    initialize_ui(*app);
    app->set_force_close_supported(false);
    app->on_close_confirm_dismissed([weak] {
      if (auto app = weak.lock()) (*app)->set_close_confirm_open(false);
    });
    app->on_close_confirm_force_quit([weak, state] {
      if (auto app = weak.lock()) {
        (*app)->set_close_confirm_open(false);
        (*app)->set_status_text(to_shared("正在停止编码，完成清理后退出。"));
        state->close_requested = true;
        state->worker.request_stop();
        state->close_timer.start(slint::TimerMode::Repeated,
            std::chrono::milliseconds{50},
            [weak, pending = std::weak_ptr<LinuxUiState>{state}] {
          auto state = pending.lock();
          auto app = weak.lock();
          if (!state || !app || (*app)->get_running()) return;
          state->close_timer.stop();
          if (finish_linux_close(**app, *state)) (*app)->window().hide();
        });
      }
    });
    app->on_queue_row_pointer_event([weak, state](int index, int button,
                                                 int kind, float) {
      if (button != 0) return;
      if (kind == 0) {
        state->drag_reordered = false;
      } else if (kind == 1) {
        const bool dragged = std::exchange(state->drag_reordered, false);
        if (auto app = weak.lock(); app && !dragged && index >= 0 &&
            static_cast<std::size_t>(index) < state->task_rows->row_count()) {
          (*app)->set_selected_queue_index(index);
        }
      }
    });
    for (int i = 0; i < static_cast<int>(state->builtin_params.size()); ++i) {
      state->builtin_params[static_cast<std::size_t>(i)] =
          default_linux_parameter_params(i);
      state->parameter_preset_params[static_cast<std::size_t>(i)] =
          state->builtin_params[static_cast<std::size_t>(i)];
    }
    state->last_format_index = 0;
    apply_linux_parameter_params(*app, state->builtin_params.front(), 0);
    app->set_queue_format_index(0);
    app->set_queue_preset_index(0);
    reload_linux_user_preset_options(*app, *state);
    for (int i = 0; i < 5; ++i) {
      state->menu_params[static_cast<std::size_t>(i)] =
          default_linux_menu_params(i);
    }
    load_linux_update_config(*app, *state);
    restore_cached_linux_update_history(*state);
    sync_linux_update_ui(*app, *state);
    state->menu_format_index = 0;
    apply_linux_menu_params(*app, state->menu_params.front());
    if (auto warning = linux_context_menu_warning(state->menu_params)) {
      app->set_context_menu_warning(to_shared(*warning));
    }
    awj::ui::bind_queue_model(*app, state->task_rows);
    app->set_large_image_rows(state->large_image_rows);
    app->set_update_history(state->update_history_rows);
    app->set_selected_large_image_index(-1);
    if (linux_changelog_should_open_on_start(*state) &&
        linux_changelog_visible_for_current_session(*state)) {
      app->set_selected_page(4);
    }

    // 与 Windows 分支同一套机制：0 = bundled 默认中文 msgid，1 = bundled 英文翻译。
    app->on_language_selection_requested([weak, state](int index) {
      if (auto app = weak.lock()) {
        (*app)->set_language_index(index);
        try {
          static_cast<void>(
              slint::select_bundled_translation(index == 1 ? "en" : ""));
        } catch (...) {
        }
        sync_linux_update_ui(**app, *state);
      }
    });
    app->on_update_channel_selection_requested([weak, state](int index) {
      if (auto app = weak.lock()) {
        const auto before = capture_linux_update_state(*state);
        state->update_channel = index == 1 ? "prerelease" : "stable";
        if (auto saved = persist_linux_update_config(**app, *state); !saved) {
          restore_linux_update_state(*state, before);
          state->update_status_zh =
              std::format("更新渠道保存失败：{}", saved.error());
          state->update_status_en = "The update channel could not be saved.";
          sync_linux_update_ui(**app, *state);
          return;
        }
        state->update_status_zh = "更新渠道已保存，正在检查…";
        state->update_status_en = "Update channel saved; checking...";
        sync_linux_update_ui(**app, *state);
        start_linux_update_check(weak, state);
      }
    });
    app->on_show_update_changelog_requested([weak, state](bool visible) {
      if (auto app = weak.lock()) {
        const auto before = capture_linux_update_state(*state);
        state->show_update_changelog = visible;
        if (!linux_changelog_visible_for_current_session(*state) &&
            (*app)->get_selected_page() == 4) {
          (*app)->set_selected_page(2);
        }
        if (auto saved = persist_linux_update_config(**app, *state); !saved) {
          restore_linux_update_state(*state, before);
          state->update_status_zh =
              std::format("更新日志设置保存失败：{}", saved.error());
          state->update_status_en =
              "The changelog setting could not be saved.";
        }
        sync_linux_update_ui(**app, *state);
      }
    });
    app->on_hide_update_changelog_after_exit_requested(
        [weak, state](bool enabled) {
          if (auto app = weak.lock()) {
            const auto before = capture_linux_update_state(*state);
            state->hide_update_changelog_after_exit = enabled;
            if (auto saved = persist_linux_update_config(**app, *state); !saved) {
              restore_linux_update_state(*state, before);
              state->update_status_zh =
                  std::format("更新日志设置保存失败：{}", saved.error());
              state->update_status_en =
                  "The changelog setting could not be saved.";
            }
            sync_linux_update_ui(**app, *state);
          }
        });
    app->on_show_update_changelog_after_update_requested(
        [weak, state](bool enabled) {
          if (auto app = weak.lock()) {
            const auto before = capture_linux_update_state(*state);
            state->show_update_changelog_after_update = enabled;
            if (auto saved = persist_linux_update_config(**app, *state); !saved) {
              restore_linux_update_state(*state, before);
              state->update_status_zh =
                  std::format("更新日志设置保存失败：{}", saved.error());
              state->update_status_en =
                  "The changelog setting could not be saved.";
            }
            sync_linux_update_ui(**app, *state);
          }
        });
    app->on_version_clicked([weak, state] {
      if (auto app = weak.lock()) {
        if (!linux_pending_update_is_newer(*state)) {
          const auto url = std::format(
              "https://github.com/Dominic485649/AWJimage/releases/tag/{}",
              AWJ_BUILD_VERSION);
          if (auto opened = open_linux_url(url); !opened) {
            state->update_status_zh =
                std::format("无法打开更新页面：{}", opened.error());
            state->update_status_en = "The release page could not be opened.";
            sync_linux_update_ui(**app, *state);
          }
          return;
        }
        if ((*app)->get_running()) {
          state->update_status_zh = "编码任务运行时禁止更新；请先完成或取消任务。";
          state->update_status_en =
              "Finish or cancel the current encoding task before updating.";
          sync_linux_update_ui(**app, *state);
          return;
        }
        if (state->update_check_active) {
          state->update_status_zh = "更新检查或下载正在进行中。";
          state->update_status_en = "An update operation is already running.";
          sync_linux_update_ui(**app, *state);
          return;
        }
        if (state->update_worker.joinable()) state->update_worker.join();
        const auto requested_version = state->pending_update_version;
        const auto release_url = state->pending_update_release_url;
        const auto preference = linux_update_preference(*state);
        const auto sequence = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, state->last_verified_manifest_v2_sequence));
        state->update_check_active = true;
        state->update_status_zh = "正在重新验签并下载更新…";
        state->update_status_en = "Verifying and downloading the update...";
        (*app)->set_update_checking(true);
        sync_linux_update_ui(**app, *state);
        state->update_worker = std::jthread(
            [weak, state, requested_version, release_url, preference,
             sequence](std::stop_token token) {
              auto staged = awj::update::stage_and_launch_linux_update(
                  requested_version, preference, sequence, token);
              static_cast<void>(slint::invoke_from_event_loop(
                  [weak, state, release_url, staged = std::move(staged)]() mutable {
                    auto app = weak.lock();
                    if (!app) return;
                    state->update_check_active = false;
                    (*app)->set_update_checking(false);
                    if (!staged) {
                      state->update_status_zh =
                          std::format("更新失败：{}", staged.error());
                      state->update_status_en = "The update could not be installed.";
                      sync_linux_update_ui(**app, *state);
                      if (staged.error().starts_with("INSTALL_DIR_NOT_WRITABLE:")) {
                        (void)open_linux_url(release_url);
                      }
                      return;
                    }
                    state->update_status_zh = "更新 helper 已启动，正在关闭当前版本…";
                    state->update_status_en =
                        "The update helper is ready; closing this version...";
                    sync_linux_update_ui(**app, *state);
                    (*app)->window().hide();
                  }));
            });
      }
    });
    app->on_format_defaults_requested([weak, state](int index) {
      if (auto app = weak.lock()) {
        if (!(*app)->get_running()) {
          apply_linux_format_parameters(**app, *state, index);
        }
      }
    });
    app->on_parameter_preset_selected([weak, state](int index) {
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        select_linux_parameter_preset(**app, *state, index);
      }
    });
    app->on_queue_preset_selected([weak, state](int index) {
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        select_linux_queue_preset(**app, *state, index);
      }
    });
    app->on_queue_drag_data([weak, state](int index) {
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        return make_linux_queue_drag_data(*state, index);
      }
      return slint::DataTransfer{};
    });
    app->on_queue_drag_can_drop(
        [weak, state](slint::language::DropEvent event, int target_slot) {
          if (auto app = weak.lock()) {
            return linux_queue_drag_can_drop(*state, std::move(event),
                                             target_slot, (*app)->get_running());
          }
          return slint::language::DragAction::None;
        });
    app->on_queue_drag_dropped(
        [weak, state](slint::language::DropEvent event, int target_slot) {
          if (auto app = weak.lock()) {
            return linux_queue_drag_dropped(**app, *state, std::move(event),
                                            target_slot);
          }
          return slint::language::DragAction::None;
        });
    app->on_open_preset_editor([weak, state] {
      if (auto app = weak.lock(); app && !(*app)->get_running()) {
        store_current_linux_parameter_params(**app, *state);
        const auto index = state->parameter_preset_index;
        (*app)->set_preset_editor_name(
            index == 0 ? slint::SharedString{}
                       : to_shared(state->user_presets[static_cast<std::size_t>(index - 1)]
                                       .name));
        (*app)->set_preset_editor_description(
            index == 0 ? slint::SharedString{}
                       : to_shared(state->user_presets[static_cast<std::size_t>(index - 1)]
                                       .description));
        (*app)->set_preset_editor_existing(index > 0);
        (*app)->set_preset_editor_shell_menu(index > 0 && state->user_presets[static_cast<std::size_t>(index - 1)].shell_menu);
        (*app)->set_preset_editor_error({});
        (*app)->set_preset_editor_open(true);
      }
    });
    app->on_cancel_preset_editor([weak] {
      if (auto app = weak.lock()) {
        (*app)->set_preset_editor_open(false);
        (*app)->set_preset_editor_error({});
      }
    });
    app->on_delete_parameter_preset([weak, state] {
      auto app = weak.lock();
      if (!app || (*app)->get_running()) return;
      const auto index = state->parameter_preset_index;
      if (index <= 0 || index > static_cast<int>(state->user_presets.size())) return;
      const auto queue_index = (*app)->get_queue_preset_index();
      auto removed = awj::delete_user_preset(state->user_presets[static_cast<std::size_t>(index - 1)]);
      if (!removed) { (*app)->set_preset_editor_error(to_shared(removed.error())); return; }
      state->parameter_preset_index = 0;
      (*app)->set_queue_preset_index(
          queue_index == index ? 0 : queue_index > index ? queue_index - 1 : queue_index);
      reload_linux_user_preset_options(**app, *state);
      select_linux_parameter_preset(**app, *state, 0);
      (*app)->set_preset_editor_open(false);
      (*app)->set_status_text(to_shared("用户预设已删除。"));
    });

    app->on_save_parameter_preset(
        [weak, state](slint::SharedString name, slint::SharedString description) {
          auto app = weak.lock();
          if (!app || (*app)->get_running()) return;
          store_current_linux_parameter_params(**app, *state);
          auto preset = linux_user_preset_from_parameters(
              shared_to_string(name), shared_to_string(description),
              active_linux_parameter_params(*state));
          if (!preset) {
            (*app)->set_preset_editor_error(to_shared(preset.error()));
            return;
          }
          preset->shell_menu = (*app)->get_preset_editor_shell_menu();
          const auto edit_index = state->parameter_preset_index;
          if (edit_index > 0 && edit_index <= static_cast<int>(state->user_presets.size())) {
            const auto& original = state->user_presets[static_cast<std::size_t>(edit_index - 1)];
            preset->source_path = original.source_path;
            const auto original_ui = linux_parameter_params_from_user_preset(original);
            for (std::size_t i = 0; i < preset->formats.size(); ++i) {
              if (active_linux_parameter_params(*state)[i].memory_limit_text == original_ui[i].memory_limit_text)
                preset->formats[i].memory_limit_bytes = original.formats[i].memory_limit_bytes;
              if (active_linux_parameter_params(*state)[i].speed_text == original_ui[i].speed_text)
                preset->formats[i].speed = original.formats[i].speed;
            }
          }
          auto saved = awj::save_user_preset(*preset, edit_index > 0);
          if (!saved) {
            (*app)->set_preset_editor_error(to_shared(saved.error()));
            return;
          }
          reload_linux_user_preset_options(**app, *state);
          const auto found = std::ranges::find(
              state->user_presets, preset->name, &awj::UserPreset::name);
          if (found != state->user_presets.end()) {
            select_linux_parameter_preset(
                **app, *state,
                static_cast<int>(std::distance(state->user_presets.begin(), found)) +
                    1);
          }
          (*app)->set_preset_editor_open(false);
          (*app)->set_preset_editor_error({});
          (*app)->set_status_text(to_shared("用户预设已保存。"));
        });
    app->on_clear_tasks([weak, state] {
      if (auto app = weak.lock()) {
        if ((*app)->get_running()) {
          (*app)->set_status_text(to_shared("当前任务正在运行，无法清空状态。"));
          return;
        }
        state->queue_files.clear();
        state->queue_path_keys.clear();
        state->task_rows->set_vector({});
        state->failed_paths.clear();
        refresh_linux_queue_counts(**app, state->task_rows);
        (*app)->set_selected_queue_index(-1);
        (*app)->set_progress(0.0f);
        (*app)->set_status_text(to_shared("已清空状态。"));
      }
    });
    app->on_browse_input([weak, state] {
      if (auto app = weak.lock()) {
        auto selected = choose_path((*app)->get_input_mode_index() == 1);
        if (!selected) {
          (*app)->set_status_text(to_shared(selected.error()));
          return;
        }
        if (auto added = add_linux_queue_from_path(**app, *state, *selected,
                                                    true);
            !added) {
          (*app)->set_status_text(to_shared(added.error()));
        }
      }
    });
    app->on_input_path_accepted([weak, state](slint::SharedString text) {
      if (auto app = weak.lock()) {
        const auto path = awj::normalize_path_argument(
            awj::wide_from_utf8(shared_to_string(text)), "输入路径");
        if (!path) {
          (*app)->set_status_text(to_shared(path.error()));
          return;
        }
        if (auto added = add_linux_queue_from_path(**app, *state, *path, true);
            !added) {
          (*app)->set_status_text(to_shared(added.error()));
        }
      }
    });
    app->on_input_path_dropped(
        [weak, state](slint::language::DropEvent event) {
          if (auto app = weak.lock()) {
            if ((*app)->get_running()) {
              (*app)->set_status_text(to_shared("当前任务正在运行，无法添加队列。"));
              return;
            }
            const auto text = event.data.plain_text();
            if (!text) {
              (*app)->set_status_text(to_shared("拖入内容不是本地文件或文件夹路径。"));
              return;
            }
            const auto paths = native_drop_paths(*text);
            if (paths.empty()) {
              (*app)->set_status_text(to_shared("拖入内容不包含本地文件或文件夹路径。"));
              return;
            }
            bool update_input_path = true;
            std::string first_error;
            for (const auto& raw : paths) {
              const auto path = awj::normalize_path_argument(
                  awj::wide_from_utf8(raw), "拖入输入路径");
              if (!path) {
                if (first_error.empty()) first_error = path.error();
                continue;
              }
              if (auto added = add_linux_queue_from_path(
                      **app, *state, *path, update_input_path);
                  added && *added) {
                update_input_path = false;
              } else if (!added && first_error.empty()) {
                first_error = added.error();
              }
            }
            if (!first_error.empty()) {
              (*app)->set_status_text(to_shared(first_error));
            }
          }
        });

    app->on_browse_output([weak] {
      if (auto app = weak.lock()) {
        auto selected = choose_path(true);
        if (!selected) {
          (*app)->set_status_text(to_shared(selected.error()));
          return;
        }
        (*app)->set_output_dir(to_shared(awj::path_to_utf8(*selected)));
        (*app)->set_status_text(to_shared("已选择输出目录。"));
      }
    });
    app->on_open_output([weak] {
      if (auto app = weak.lock()) {
        fs::path path{shared_to_string((*app)->get_output_dir())};
        if (path.empty()) {
          path = awj::default_output_dir_for(fs::path{shared_to_string((*app)->get_input_path())});
        }
        if (auto opened = open_path(path); !opened) {
          (*app)->set_status_text(to_shared(opened.error()));
        }
      }
    });
    app->on_output_path_accepted([weak](slint::SharedString text) {
      if (auto app = weak.lock()) {
        const auto raw = shared_to_string(text);
        if (trim_copy(raw).empty()) {
          (*app)->set_output_dir({});
          return;
        }
        const auto path = awj::normalize_path_argument(
            awj::wide_from_utf8(raw), "输出目录");
        if (!path) {
          (*app)->set_status_text(to_shared(path.error()));
          return;
        }
        (*app)->set_output_dir(to_shared(awj::path_to_utf8(*path)));
      }
    });
    app->on_output_path_dropped(
        [weak](slint::language::DropEvent event) {
          if (auto app = weak.lock()) {
            if ((*app)->get_running()) {
              (*app)->set_status_text(to_shared("当前任务正在运行，无法修改输出目录。"));
              return;
            }
            const auto text = event.data.plain_text();
            if (!text) {
              (*app)->set_status_text(to_shared("拖入内容不是本地文件或文件夹路径。"));
              return;
            }
            const auto paths = native_drop_paths(*text);
            if (paths.size() != 1) {
              (*app)->set_status_text(to_shared("输出目录一次只能拖入一个文件或文件夹。"));
              return;
            }
            const auto path = awj::normalize_path_argument(
                awj::wide_from_utf8(paths.front()), "拖入输出目录");
            if (!path) {
              (*app)->set_status_text(to_shared(path.error()));
              return;
            }
            std::error_code ec;
            if (!std::filesystem::exists(*path, ec) || ec) {
              (*app)->set_status_text(to_shared("拖入的输出目标不存在或无法访问。"));
              return;
            }
            const auto output = std::filesystem::is_directory(*path, ec) && !ec
                                    ? *path
                                    : path->parent_path();
            if (output.empty()) {
              (*app)->set_status_text(to_shared("无法从拖入目标确定输出目录。"));
              return;
            }
            (*app)->set_output_dir(to_shared(awj::path_to_utf8(output)));
            (*app)->set_status_text(to_shared("已更新输出目录。"));
          }
        });
    app->on_menu_format_selected([weak, state](int index) {
      if (auto app = weak.lock()) {
        store_linux_menu_params(**app, *state);
        state->menu_format_index = std::clamp(index, 0, 4);
        apply_linux_menu_params(
            **app, state->menu_params[static_cast<std::size_t>(state->menu_format_index)]);
      }
    });
    app->on_save_menu_params_requested([weak, state] {
      if (auto app = weak.lock()) {
        store_linux_menu_params(**app, *state);
        auto valid = validate_linux_menu_params(state->menu_params);
        if (valid) valid = persist_linux_update_config(**app, *state);
        if (valid) {
          if (auto warning = linux_context_menu_warning(state->menu_params)) {
            (*app)->set_context_menu_warning(to_shared(*warning));
          } else {
            (*app)->set_context_menu_warning({});
          }
        }
        (*app)->set_context_menu_status(to_shared(
            valid ? "菜单参数已保存；重新安装菜单后生效。" : valid.error()));
        (*app)->set_status_text((*app)->get_context_menu_status());
      }
    });
    app->on_install_context_menu_requested([weak, state] {
      if (auto app = weak.lock()) {
        store_linux_menu_params(**app, *state);
        if (auto valid = validate_linux_menu_params(state->menu_params); !valid) {
          (*app)->set_context_menu_status(to_shared(valid.error()));
          (*app)->set_status_text(to_shared(valid.error()));
          return;
        }
        if (auto saved = persist_linux_update_config(**app, *state); !saved) {
          (*app)->set_context_menu_status(to_shared(saved.error()));
          (*app)->set_status_text(to_shared(saved.error()));
          return;
        }
        auto result = write_linux_file_manager_actions(state->menu_params);
        if (result) {
          (*app)->set_context_menu_warning({});
        }
        (*app)->set_context_menu_status(to_shared(
            result ? "已写入 Nautilus 脚本和 Thunar 右键菜单；重开文件管理器后生效。"
                   : result.error()));
        (*app)->set_status_text((*app)->get_context_menu_status());
      }
    });
    app->on_remove_context_menu_requested([weak] {
      if (auto app = weak.lock()) {
        auto result = remove_linux_file_manager_actions();
        if (result) {
          (*app)->set_context_menu_warning({});
        }
        (*app)->set_context_menu_status(to_shared(
            result ? "已移除 Nautilus 脚本和 Thunar AWJ 右键菜单。" : result.error()));
        (*app)->set_status_text((*app)->get_context_menu_status());
      }
    });
    app->on_context_menu_warning_clicked([weak] {
      if (auto app = weak.lock()) {
        auto result = remove_linux_file_manager_actions();
        (*app)->set_context_menu_status(to_shared(
            result ? "旧右键菜单已移除，请重新安装。" : result.error()));
        (*app)->set_status_text((*app)->get_context_menu_status());
        if (result) {
          (*app)->set_context_menu_warning({});
        }
      }
    });
    app->on_cancel_conversion([weak, state] {
      if (auto app = weak.lock()) {
        state->worker.request_stop();
        (*app)->set_status_text(to_shared("正在停止当前任务…"));
      }
    });
    app->on_retry_failed([weak, state] {
      auto app = weak.lock();
      if (!app) {
        return;
      }
      if ((*app)->get_running()) {
        (*app)->set_status_text(to_shared("当前任务正在运行，无法重试失败项。"));
        return;
      }
      if (state->failed_paths.empty()) {
        (*app)->set_status_text(to_shared("队列中没有失败项。"));
        return;
      }
      auto cfg = config_from_ui(**app, *state);
      if (!cfg) {
        (*app)->set_status_text(
            to_shared(std::format("配置错误：{}", cfg.error())));
        return;
      }
      auto retry_paths = state->failed_paths;
      auto files = build_linux_queue_files(*cfg, state->queue_files, &retry_paths);
      if (!files) {
        (*app)->set_status_text(
            to_shared(std::format("重试准备失败：{}", files.error())));
        return;
      }
      auto manifest = create_linux_queue_manifest(state->next_queue_run_id++, *files);
      if (!manifest) {
        (*app)->set_status_text(
            to_shared(std::format("重试准备失败：{}", manifest.error())));
        return;
      }
      cfg->studio_queue_manifest = *manifest;
      for (std::size_t index = 0; index < state->task_rows->row_count();
           ++index) {
        auto row = state->task_rows->row_data(index);
        if (row && row->state == 3) {
          row->state = 0;
          row->status = to_shared("等待重试");
          row->warning = false;
          state->task_rows->set_row_data(index, *row);
        }
      }
      refresh_linux_queue_counts(**app, state->task_rows);
      (*app)->set_running(true);
      (*app)->set_progress(0.0f);
      (*app)->set_status_text(to_shared(
          std::format("正在重试 {} 个失败项…", retry_paths.size())));
      auto rows = state->task_rows;
      state->worker = std::jthread(
          [weak, state, rows, cfg = std::move(*cfg),
           retry_paths = std::move(retry_paths), manifest_path = *manifest](
              std::stop_token token) mutable {
            auto progress = [weak, state, rows](
                                const awj::BatchProgress& event) {
              slint::invoke_from_event_loop([weak, state, rows, event] {
                if (auto app = weak.lock()) {
                  if (event.kind == awj::BatchEventKind::item_started) {
                    mark_linux_task_row_running(**app, rows, event.result);
                  } else if (event.kind == awj::BatchEventKind::item_finished) {
                    set_linux_task_row_result(**app, rows, event.result);
                    std::erase(state->failed_paths, event.result.input_path);
                    if (!event.result.ok && !event.result.canceled) {
                      state->failed_paths.push_back(event.result.input_path);
                    }
                  } else if (event.kind ==
                             awj::BatchEventKind::large_image_queued) {
                    add_large_image_task_row(rows, event.large_image);
                    push_linux_large_image(*state, event.large_image);
                    refresh_linux_queue_counts(**app, rows);
                  }
                  if (event.total > 0) {
                    (*app)->set_progress(
                        static_cast<float>(event.completed) /
                        static_cast<float>(event.total));
                  }
                  if (!event.text.empty()) {
                    (*app)->set_status_text(to_shared(event.text));
                  }
                }
              });
            };
            auto summary = awj::run_batch(cfg, progress, token);
            std::error_code cleanup_ec;
            fs::remove(manifest_path, cleanup_ec);
            slint::invoke_from_event_loop(
                [weak, state, rows, summary = std::move(summary),
                 retry_paths = std::move(retry_paths)]() mutable {
                  if (auto app = weak.lock()) {
                    (*app)->set_running(false);
                    if (!summary) {
                      state->failed_paths = retry_paths;
                      for (const auto& path : retry_paths) {
                        if (const auto index =
                                linux_task_row_index_for_path(rows, path)) {
                          auto row = rows->row_data(*index);
                          if (row) {
                            row->state = 3;
                            row->status = to_shared("失败");
                            row->log = to_shared(summary.error());
                            row->warning = true;
                            rows->set_row_data(*index, *row);
                          }
                        }
                      }
                      refresh_linux_queue_counts(**app, rows);
                      (*app)->set_status_text(to_shared(
                          std::format("重试失败：{}", summary.error())));
                      return;
                    }
                    (*app)->set_progress(1.0f);
                    (*app)->set_status_text(to_shared(std::format(
                        "重试完成：成功 {}，失败 {}，取消 {}。",
                        summary->ok_count, summary->failed_count,
                        summary->canceled_count)));
                  }
                });
          });
    });
    app->on_toggle_template_token([weak](slint::SharedString token) {
      if (auto app = weak.lock()) {
        auto text = shared_to_string((*app)->get_template_text());
        const auto value = shared_to_string(token);
        if (const auto pos = text.find(value); pos != std::string::npos) {
          text.erase(pos, value.size());
        } else {
          if (!text.empty() && text.back() != '_') text += '_';
          text += value;
        }
        (*app)->set_template_text(to_shared(text));
      }
    });
    app->on_title_bar_theme_requested([](bool) {});
    app->on_combo_popup_requested([] {});
    app->on_combo_popup_finished([] {});
    app->on_large_image_action_requested([weak, state](int index, slint::SharedString action_text) {
      auto app = weak.lock();
      if (!app) return;
      if ((*app)->get_running()) {
        (*app)->set_status_text(to_shared("当前任务正在运行，请先停止任务"));
        return;
      }
      const auto action = shared_to_string(action_text);
      if (index < 0 || static_cast<std::size_t>(index) >= state->large_image_items.size()) {
        (*app)->set_status_text(to_shared("未选择大图任务"));
        return;
      }
      const auto item = state->large_image_items[static_cast<std::size_t>(index)];
      const bool available = action == "grid" && linux_large_image_grid_available(item);
      if (!available) {
        (*app)->set_status_text(to_shared(std::format("{} 不可用", action)));
        return;
      }
      auto cfg = config_from_ui(**app, *state);
      if (!cfg) {
        (*app)->set_status_text(to_shared(std::format("配置错误：{}", cfg.error())));
        return;
      }
      (*cfg).input_path = item.file.path;
      (*cfg).output_format = awj::OutputFormat::avif;
      (*cfg).studio_large_action = awj::wide_from_utf8(action);
      (*cfg).visual_quality.reset();
      set_linux_large_image_status(*state, index, std::format("已选择 {} · 正在编码…", action));
      (*app)->set_running(true);
      (*app)->set_progress(0.0f);
      (*app)->set_status_text(to_shared(std::format("大图处理：{}", action)));
      state->worker = std::jthread([weak, state, cfg = std::move(*cfg), index, action](std::stop_token token) mutable {
        auto progress = [weak](const awj::BatchProgress& event) {
          slint::invoke_from_event_loop([weak, event] {
            if (auto app = weak.lock()) {
              if (event.total > 0) {
                (*app)->set_progress(static_cast<float>(event.completed) / static_cast<float>(event.total));
              }
              if (!event.text.empty()) {
                (*app)->set_status_text(to_shared(event.text));
              } else if (event.kind == awj::BatchEventKind::item_finished) {
                (*app)->set_status_text(to_shared(event.result.message));
              }
            }
          });
        };
        auto summary = awj::run_batch(cfg, progress, token);
        slint::invoke_from_event_loop([weak, state, summary = std::move(summary), index, action] {
          if (auto app = weak.lock()) {
            (*app)->set_running(false);
            if (!summary) {
              set_linux_large_image_status(*state, index, "失败");
              (*app)->set_status_text(to_shared(std::format("大图失败：{}", summary.error())));
              return;
            }
            const bool ok = summary->ok_count > 0 && summary->failed_count == 0;
            set_linux_large_image_status(*state, index, ok ? std::format("{} 完成", action) : std::format("{} 失败", action));
            (*app)->set_progress(1.0f);
            (*app)->set_status_text(to_shared(std::format("大图完成：成功 {}，失败 {}，取消 {}。",
                                                         summary->ok_count, summary->failed_count, summary->canceled_count)));
          }
        });
      });
    });

    app->on_start_conversion([weak, state] {
      auto app = weak.lock();
      if (!app) {
        return;
      }
      if ((*app)->get_running()) {
        state->worker.request_stop();
        (*app)->set_status_text(to_shared("正在停止当前任务…"));
        return;
      }
      auto cfg = config_from_ui(**app, *state);
      if (!cfg) {
        (*app)->set_status_text(to_shared(std::format("配置错误：{}", cfg.error())));
        return;
      }
      if (state->queue_files.empty()) {
        (*app)->set_status_text(to_shared("队列为空，请先输入、选择或拖入图片。"));
        return;
      }
      auto files = build_linux_queue_files(*cfg, state->queue_files);
      if (!files) {
        (*app)->set_status_text(
            to_shared(std::format("队列准备失败：{}", files.error())));
        return;
      }
      auto manifest = create_linux_queue_manifest(state->next_queue_run_id++, *files);
      if (!manifest) {
        (*app)->set_status_text(
            to_shared(std::format("队列准备失败：{}", manifest.error())));
        return;
      }
      cfg->studio_queue_manifest = *manifest;
      state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
      state->large_image_rows = std::make_shared<slint::VectorModel<LargeImageRow>>();
      state->large_image_items.clear();
      state->failed_paths.clear();
      std::vector<TaskRow> pending_rows;
      pending_rows.reserve(files->size());
      for (std::size_t index = 0; index < files->size(); ++index) {
        pending_rows.push_back(pending_linux_queue_row((*files)[index], index,
                                                       true));
      }
      state->task_rows->set_vector(std::move(pending_rows));
      awj::ui::bind_queue_model(**app, state->task_rows);
      (*app)->set_large_image_rows(state->large_image_rows);
      (*app)->set_selected_large_image_index(-1);
      refresh_linux_queue_counts(**app, state->task_rows);
      (*app)->set_running(true);
      (*app)->set_progress(0.0f);
      (*app)->set_status_text(to_shared("正在转换…"));
      auto rows = state->task_rows;
      state->worker = std::jthread([weak, state, rows, cfg = std::move(*cfg),
                                    manifest_path = *manifest](std::stop_token token) mutable {
        auto progress = [weak, state, rows](const awj::BatchProgress& event) {
          slint::invoke_from_event_loop([weak, state, rows, event] {
            if (auto app = weak.lock()) {
              if (event.kind == awj::BatchEventKind::item_started) {
                mark_linux_task_row_running(**app, rows, event.result);
              } else if (event.kind == awj::BatchEventKind::item_finished) {
                set_linux_task_row_result(**app, rows, event.result);
                std::erase(state->failed_paths, event.result.input_path);
                if (!event.result.ok && !event.result.canceled) {
                  state->failed_paths.push_back(event.result.input_path);
                }
              } else if (event.kind == awj::BatchEventKind::large_image_queued) {
                add_large_image_task_row(rows, event.large_image);
                push_linux_large_image(*state, event.large_image);
                refresh_linux_queue_counts(**app, rows);
                if ((*app)->get_selected_large_image_index() < 0 && !state->large_image_items.empty()) {
                  (*app)->set_selected_large_image_index(0);
                }
              }
              if (event.total > 0) {
                (*app)->set_progress(static_cast<float>(event.completed) / static_cast<float>(event.total));
              }
              if (!event.text.empty()) {
                (*app)->set_status_text(to_shared(event.text));
              } else if (event.kind == awj::BatchEventKind::item_finished) {
                (*app)->set_status_text(to_shared(event.result.message));
              }
            }
          });
        };
        auto summary = awj::run_batch(cfg, progress, token);
        std::error_code cleanup_ec;
        fs::remove(manifest_path, cleanup_ec);
        slint::invoke_from_event_loop([weak, summary = std::move(summary)] {
          if (auto app = weak.lock()) {
            (*app)->set_running(false);
            if (!summary) {
              (*app)->set_status_text(to_shared(std::format("转换失败：{}", summary.error())));
              return;
            }
            (*app)->set_progress(1.0f);
            (*app)->set_status_text(to_shared(std::format("完成：成功 {}，失败 {}，取消 {}。",
                                                         summary->ok_count,
                                                         summary->failed_count,
                                                         summary->canceled_count)));
          }
        });
      });
    });

    std::optional<std::chrono::system_clock::time_point> last_check{};
    if (state->last_successful_update_check_at > 0) {
      last_check = std::chrono::system_clock::time_point{
          std::chrono::seconds{state->last_successful_update_check_at}};
    }
    if (awj::update::should_check_now(
            {.trigger = awj::update::CheckTrigger::startup,
             .last_successful_check = last_check,
             .now = std::chrono::system_clock::now()})) {
      start_linux_update_check(weak, state);
    }
    app->window().on_close_requested([weak, state] {
      if (auto app = weak.lock()) {
        if ((*app)->get_running()) {
          if (!state->close_requested) (*app)->set_close_confirm_open(true);
          return slint::CloseRequestResponse::KeepWindowShown;
        }
        if (!finish_linux_close(**app, *state))
          return slint::CloseRequestResponse::KeepWindowShown;
      }
      return slint::CloseRequestResponse::HideWindow;
    });
    app->run();
    state->worker.request_stop();
    state->update_worker.request_stop();
    if (state->update_worker.joinable()) state->update_worker.join();
    return 0;
  } catch (const std::exception& ex) {
    std::println(stderr, "[FAIL] 启动 Slint UI 失败: {}", ex.what());
    return 1;
  } catch (...) {
    std::println(stderr, "[FAIL] 启动 Slint UI 失败。");
    return 1;
  }
}

#endif  // !_WIN32
