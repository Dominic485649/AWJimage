#include "studio_update_ui.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include "changelog_history.h"
#include "studio_ui_util.h"

import awj.core;
import awj.update_archive;
import awj.update_keyring;
import awj.update_manifest;
import awj.update_manifest_v2;
import awj.update_model;
import awj.update_runtime;
import awj.update_security_state;
import awj.update_windows;

namespace awj::studio {

UpdatePersistentState capture_update_state(const UiState& state) {
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

void restore_update_state(UiState& state, UpdatePersistentState value) {
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

void clear_pending_update(UiState& state) {
  state.pending_update_version.clear();
  state.pending_update_channel.clear();
  state.pending_update_release_url.clear();
  state.pending_update_published_at.clear();
  state.pending_update_changelog_zh_cn.clear();
  state.pending_update_changelog_en.clear();
}

std::string update_summary(std::string_view changelog) {
  const auto line_end = changelog.find_first_of("\r\n");
  return std::string{changelog.substr(0, line_end)};
}

std::string format_update_check_time(std::int64_t unix_seconds) {
  if (unix_seconds <= 0) return {};
  const auto point = std::chrono::system_clock::time_point{
      std::chrono::seconds{unix_seconds}};
  return std::format("{:%Y-%m-%d %H:%M:%S} UTC",
                     std::chrono::floor<std::chrono::seconds>(point));
}

bool pending_update_is_newer(const UiState& state) {
  const auto current = awj::update::parse_version(AWJ_BUILD_VERSION);
  const auto pending = awj::update::parse_version(state.pending_update_version);
  const auto channel = awj::update::parse_channel(state.pending_update_channel);
  const auto preference = state.update_channel == "prerelease"
                              ? awj::update::ChannelPreference::stable_and_prerelease
                              : awj::update::ChannelPreference::stable_only;
  return current && pending && channel && *pending > *current &&
         awj::update::channel_visible_to(*channel, preference);
}

bool changelog_first_start_for_current_version(const UiState& state) {
  return state.last_changelog_exit_version != AWJ_BUILD_VERSION;
}

bool changelog_visible_for_current_session(const UiState& state) {
  const bool first_start = changelog_first_start_for_current_version(state);
  if (!state.show_update_changelog) {
    // 总开关关闭时，升级后的首次启动仍临时显示一次，退出后即恢复隐藏。
    return first_start;
  }
  return !state.hide_update_changelog_after_exit || first_start;
}

bool changelog_should_open_on_start(const UiState& state) {
  return changelog_first_start_for_current_version(state) &&
         (!state.show_update_changelog || state.show_update_changelog_after_update);
}

void sync_update_ui(AwjStudio& app, const UiState& state) {
  const bool english = app.get_language_index() == 1;
  const bool available = pending_update_is_newer(state);
  app.set_current_version(to_shared(AWJ_BUILD_VERSION));
  app.set_update_channel_index(state.update_channel == "prerelease" ? 1 : 0);
  app.set_show_update_changelog_enabled(state.show_update_changelog);
  app.set_show_update_changelog(changelog_visible_for_current_session(state));
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
  app.set_update_summary_zh_cn(
      to_shared(available ? update_summary(state.pending_update_changelog_zh_cn)
                          : ""));
  app.set_update_summary_en(
      to_shared(available ? update_summary(state.pending_update_changelog_en)
                          : ""));
  const auto last = format_update_check_time(state.last_successful_update_check_at);
  app.set_update_last_successful_check(
      to_shared(last.empty() ? (english ? "Never" : "从未") : last));
  app.set_update_status(
      to_shared(english ? state.update_status_en : state.update_status_zh));
}

void sync_update_history(
    const std::shared_ptr<slint::VectorModel<UpdateHistoryRow>>& rows,
    const awj::update::Manifest& manifest) {
  if (!rows) return;
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
  rows->set_vector(std::move(history_rows));
}

void restore_cached_update_history(UiState& state) {
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
    // 缓存只用于展示；验签失败或序号倒退时丢弃，联网检查仍按原状态继续。
    state.update_manifest_v2_raw.clear();
    state.update_manifest_v2_signature.clear();
    state.update_keyring_raw.clear();
    state.update_keyring_signature.clear();
    return;
  }
  state.last_verified_manifest_v2_sequence =
      std::max(state.last_verified_manifest_v2_sequence,
               static_cast<std::int64_t>(manifest->sequence));
  sync_update_history(state.update_history_rows,
                      awj::update::archive_manifest_v2_for_history(*manifest));
}

awj::update::ChannelPreference update_preference(const UiState& state) {
  return state.update_channel == "prerelease"
             ? awj::update::ChannelPreference::stable_and_prerelease
             : awj::update::ChannelPreference::stable_only;
}

}  // namespace awj::studio
