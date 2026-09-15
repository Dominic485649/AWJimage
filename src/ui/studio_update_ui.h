#pragma once

// 更新状态的捕获/恢复、UI 同步与后台检查。从 main.cpp 拆出。

#include <cstdint>
#include <string>
#include <string_view>

#include "awj_studio.h"
#include "studio_state.h"

import awj.update_model;

namespace awj::studio {

struct UpdatePersistentState {
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

UpdatePersistentState capture_update_state(const UiState& state);
void restore_update_state(UiState& state, UpdatePersistentState value);
void clear_pending_update(UiState& state);
std::string update_summary(std::string_view changelog);
std::string format_update_check_time(std::int64_t unix_seconds);
bool pending_update_is_newer(const UiState& state);
bool changelog_first_start_for_current_version(const UiState& state);
bool changelog_visible_for_current_session(const UiState& state);
bool changelog_should_open_on_start(const UiState& state);
void restore_cached_update_history(UiState& state);
awj::update::ChannelPreference update_preference(const UiState& state);
void sync_update_ui(AwjStudio& app, const UiState& state);
void start_update_check(slint::ComponentWeakHandle<AwjStudio> weak,
                        const std::shared_ptr<UiState>& state);
void sync_update_history(
    const std::shared_ptr<slint::VectorModel<UpdateHistoryRow>>& rows,
    const awj::update::Manifest& manifest);

}  // namespace awj::studio
