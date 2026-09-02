#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

import awj.update_model;

namespace {

using namespace awj::update;

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

constexpr Version v(std::uint32_t major, std::uint32_t minor,
                    std::uint32_t patch) {
  return Version{.major = major, .minor = minor, .patch = patch};
}

ManifestEntry entry(Version version, Channel channel) {
  return ManifestEntry{.version = version,
                       .channel = channel,
                       .release_url = "https://example.invalid/r",
                       .published_at = "2026-08-09T00:00:00Z"};
}

// 版本比较必须按整数分段，不能退化成字符串比较。
int test_version_ordering() {
  if (!(v(1, 0, 9) < v(1, 0, 10))) {
    return fail("1.0.9 must sort before 1.0.10 (string compare would fail).");
  }
  if (!(v(1, 2, 0) > v(1, 1, 99))) {
    return fail("minor version must outrank patch.");
  }
  if (!(v(2, 0, 0) > v(1, 99, 99))) {
    return fail("major version must outrank minor.");
  }
  if (!(v(1, 0, 0) == v(1, 0, 0))) {
    return fail("equal versions must compare equal.");
  }
  return 0;
}

int test_version_parsing() {
  const auto ok = parse_version("1.0.2");
  if (!ok || *ok != v(1, 0, 2)) {
    return fail("parse_version failed on a well-formed version.");
  }
  const auto big = parse_version("10.20.30");
  if (!big || *big != v(10, 20, 30)) {
    return fail("parse_version failed on multi-digit fields.");
  }
  // 这些必须被拒绝：宽松解析在更新路径上会把畸形输入读成别的版本。
  for (const std::string_view bad : {"1.0", "1.0.2.3", "1.0.x", "v1.0.2",
                                     "1.0.2-rc1", "1.0.2+build", "1.0.02",
                                     " 1.0.2", "1.0.2 ", "", "1..2"}) {
    if (parse_version(bad).has_value()) {
      std::cerr << "parse_version accepted malformed input: " << bad << '\n';
      return 1;
    }
  }
  return 0;
}

// stable 用户绝不能看到 prerelease；prerelease 用户两者都能看到。
int test_channel_filtering() {
  if (channel_visible_to(Channel::prerelease, ChannelPreference::stable_only)) {
    return fail("stable users must never see prerelease builds.");
  }
  if (!channel_visible_to(Channel::stable, ChannelPreference::stable_only)) {
    return fail("stable users must see stable builds.");
  }
  if (!channel_visible_to(Channel::prerelease,
                          ChannelPreference::stable_and_prerelease)) {
    return fail("prerelease users must see prerelease builds.");
  }
  if (!channel_visible_to(Channel::stable,
                          ChannelPreference::stable_and_prerelease)) {
    return fail("prerelease users must still see stable builds.");
  }
  return 0;
}

int test_candidate_selection() {
  Manifest manifest{.schema = supported_manifest_schema, .sequence = 5};
  manifest.entries.push_back(entry(v(1, 0, 1), Channel::stable));
  manifest.entries.push_back(entry(v(1, 0, 3), Channel::prerelease));
  manifest.entries.push_back(entry(v(1, 0, 2), Channel::stable));

  const CandidateRequest stable_req{.current_version = v(1, 0, 0),
                                    .updater_version = v(1, 0, 0),
                                    .preference = ChannelPreference::stable_only};
  const auto stable_pick = select_candidate(manifest, stable_req);
  if (!stable_pick || stable_pick->version != v(1, 0, 2)) {
    return fail("stable channel must pick 1.0.2, not the 1.0.3 prerelease.");
  }

  const CandidateRequest pre_req{
      .current_version = v(1, 0, 0),
      .updater_version = v(1, 0, 0),
      .preference = ChannelPreference::stable_and_prerelease};
  const auto pre_pick = select_candidate(manifest, pre_req);
  if (!pre_pick || pre_pick->version != v(1, 0, 3)) {
    return fail("prerelease channel must pick the highest visible version.");
  }

  // 已是最新：返回 nullopt 而不是错误。
  const CandidateRequest current_req{
      .current_version = v(1, 0, 3),
      .updater_version = v(1, 0, 3),
      .preference = ChannelPreference::stable_and_prerelease};
  if (select_candidate(manifest, current_req).has_value()) {
    return fail("an up-to-date install must yield no candidate.");
  }

  // 绝不降级。
  const CandidateRequest newer_req{
      .current_version = v(2, 0, 0),
      .updater_version = v(2, 0, 0),
      .preference = ChannelPreference::stable_and_prerelease};
  if (select_candidate(manifest, newer_req).has_value()) {
    return fail("a newer local build must never be offered a downgrade.");
  }
  return 0;
}

int test_revocation_and_minimum_updater() {
  Manifest manifest{.schema = supported_manifest_schema, .sequence = 9};
  auto revoked = entry(v(1, 0, 5), Channel::stable);
  revoked.revoked = true;
  manifest.entries.push_back(revoked);
  manifest.entries.push_back(entry(v(1, 0, 4), Channel::stable));

  const CandidateRequest req{.current_version = v(1, 0, 0),
                             .updater_version = v(1, 0, 0),
                             .preference = ChannelPreference::stable_only};
  const auto pick = select_candidate(manifest, req);
  if (!pick || pick->version != v(1, 0, 4)) {
    return fail("a revoked version must never be selected.");
  }

  // updater 太旧时跳过该条目，让用户先装中间版本。
  Manifest gated{.schema = supported_manifest_schema, .sequence = 3};
  auto needs_new_updater = entry(v(2, 0, 0), Channel::stable);
  needs_new_updater.minimum_updater_version = v(1, 5, 0);
  gated.entries.push_back(needs_new_updater);
  gated.entries.push_back(entry(v(1, 5, 0), Channel::stable));
  const auto gated_pick = select_candidate(gated, req);
  if (!gated_pick || gated_pick->version != v(1, 5, 0)) {
    return fail("an entry requiring a newer updater must be skipped.");
  }

  // 撤销是唯一允许清除待更新状态的例外。
  if (!should_clear_pending_for_revocation(manifest, v(1, 0, 5))) {
    return fail("a revoked pending version must be cleared.");
  }
  if (should_clear_pending_for_revocation(manifest, v(1, 0, 4))) {
    return fail("a non-revoked pending version must be retained.");
  }
  return 0;
}

int test_replay_protection() {
  Manifest manifest{.schema = supported_manifest_schema, .sequence = 10};
  if (!is_manifest_fresh(manifest, 10)) {
    return fail("re-fetching the same sequence must stay acceptable.");
  }
  if (!is_manifest_fresh(manifest, 9)) {
    return fail("a newer sequence must be accepted.");
  }
  if (is_manifest_fresh(manifest, 11)) {
    return fail("a rolled-back sequence must be rejected as a replay.");
  }
  return 0;
}

int test_schema_and_duplicates() {
  Manifest wrong_schema{.schema = supported_manifest_schema + 1};
  if (validate_manifest_consistency(wrong_schema).has_value()) {
    return fail("an unknown manifest schema must be rejected outright.");
  }

  // 跨渠道复用同一版本号：必须拒绝，且报错要点明跨渠道，因为这是发布流程里
  // 最容易犯、后果最难排查的一种错。
  Manifest cross_channel{.schema = supported_manifest_schema, .sequence = 1};
  cross_channel.entries.push_back(entry(v(1, 0, 1), Channel::stable));
  cross_channel.entries.push_back(entry(v(1, 0, 1), Channel::prerelease));
  const auto cross_result = validate_manifest_consistency(cross_channel);
  if (cross_result.has_value()) {
    return fail("reusing a version across channels must be rejected.");
  }
  if (cross_result.error().find("跨渠道") == std::string::npos) {
    return fail("the cross-channel error must name the actual problem.");
  }

  // 同渠道重复也必须拒绝。
  Manifest duplicated{.schema = supported_manifest_schema, .sequence = 1};
  duplicated.entries.push_back(entry(v(1, 0, 1), Channel::stable));
  duplicated.entries.push_back(entry(v(1, 0, 1), Channel::stable));
  if (validate_manifest_consistency(duplicated).has_value()) {
    return fail("a reused version number must be rejected.");
  }

  Manifest good{.schema = supported_manifest_schema, .sequence = 1};
  good.entries.push_back(entry(v(1, 0, 1), Channel::stable));
  good.entries.push_back(entry(v(1, 0, 2), Channel::stable));
  if (!validate_manifest_consistency(good)) {
    return fail("a consistent manifest must validate.");
  }
  return 0;
}

int test_check_scheduling() {
  using namespace std::chrono;
  const auto now = system_clock::time_point{hours{24 * 400}};

  // 手动检查与切换渠道不受七天限制。
  if (!should_check_now({.trigger = CheckTrigger::manual,
                         .last_successful_check = now,
                         .now = now})) {
    return fail("a manual check must never be rate limited.");
  }
  if (!should_check_now({.trigger = CheckTrigger::channel_changed,
                         .last_successful_check = now,
                         .now = now})) {
    return fail("switching channels must force a check.");
  }

  // 无有效日期时立即检查。
  if (!should_check_now({.trigger = CheckTrigger::startup, .now = now})) {
    return fail("a missing timestamp must trigger an immediate check.");
  }

  // 未满七天不检查；满七天检查。
  if (should_check_now({.trigger = CheckTrigger::startup,
                        .last_successful_check = now - hours{24 * 6},
                        .now = now})) {
    return fail("a check within the interval must be skipped.");
  }
  if (!should_check_now({.trigger = CheckTrigger::startup,
                         .last_successful_check = now - hours{24 * 7},
                         .now = now})) {
    return fail("a check at exactly the interval must run.");
  }

  // 未来时间戳视为不可信，立即检查，否则会永久禁用更新。
  if (!should_check_now({.trigger = CheckTrigger::startup,
                         .last_successful_check = now + hours{24 * 365},
                         .now = now})) {
    return fail("a future timestamp must not disable updates forever.");
  }
  return 0;
}

// 失败绝不能更新时间戳或清除红点。
int test_persistence_decisions() {
  const auto available = decide_persistence(
      {.outcome = CheckOutcome::update_available});
  if (!available.update_last_successful_check || !available.write_pending) {
    return fail("a successful check with an update must persist both.");
  }

  const auto up_to_date = decide_persistence({.outcome = CheckOutcome::up_to_date});
  if (!up_to_date.update_last_successful_check) {
    return fail("an up-to-date check must still record the timestamp.");
  }
  if (up_to_date.write_pending || up_to_date.clear_pending) {
    return fail("an up-to-date check must not touch pending state.");
  }

  const auto failed = decide_persistence({.outcome = CheckOutcome::failed});
  if (failed.update_last_successful_check) {
    return fail("a failed check must not advance the timestamp.");
  }
  if (failed.clear_pending) {
    return fail("a failed check must retain the pending update and red dot.");
  }
  return 0;
}

int test_update_health_protocol() {
  // 1.0.9 风格的旧 helper 会把已经验签并验证成员哈希的目标版本传给新二进制。
  // UI 的 pending 缓存可以缺失或落后，但不能因此否定新二进制本身已经启动成功。
  const auto matching =
      decide_update_health_handshake("1.0.11", "1.0.11", "1.0.11");
  if (!matching.signal_ready || !matching.clear_matching_pending) {
    return fail("a verified matching target must signal and clear matching pending state.");
  }

  const auto missing =
      decide_update_health_handshake("1.0.11", "1.0.11", "");
  if (!missing.signal_ready || missing.clear_matching_pending) {
    return fail("missing UI pending state must not block a verified target health signal.");
  }

  const auto stale =
      decide_update_health_handshake("1.0.11", "1.0.11", "1.0.10");
  if (!stale.signal_ready || stale.clear_matching_pending) {
    return fail("stale UI pending state must not block or be cleared by a verified target.");
  }

  const auto wrong_target =
      decide_update_health_handshake("1.0.10", "1.0.11", "1.0.11");
  if (wrong_target.signal_ready || wrong_target.clear_matching_pending) {
    return fail("a helper/build target-version mismatch must fail closed.");
  }

  if (classify_update_health_observation(false, false) !=
      UpdateHealthObservation::event_not_signaled) {
    return fail("a missing health signal must remain a rollback outcome.");
  }
  if (classify_update_health_observation(true, false) !=
      UpdateHealthObservation::process_exited_early) {
    return fail("a process that exits during the grace period must roll back.");
  }
  if (classify_update_health_observation(true, true) !=
      UpdateHealthObservation::ready) {
    return fail("only a signaled and still-running health process may commit.");
  }
  return 0;
}

int test_1_0_11_migration_selection() {
  Manifest manifest{.schema = supported_manifest_schema, .sequence = 11};
  auto target = entry(v(1, 0, 11), Channel::prerelease);
  target.minimum_updater_version = v(1, 0, 9);
  manifest.entries.push_back(target);

  for (const auto updater : {v(1, 0, 9), v(1, 0, 10)}) {
    const auto picked = select_candidate(
        manifest,
        {.current_version = updater,
         .updater_version = updater,
         .preference = ChannelPreference::stable_and_prerelease});
    if (!picked || picked->version != v(1, 0, 11)) {
      return fail("deployed 1.0.9/1.0.10 updaters must be able to select 1.0.11.");
    }
  }

  Manifest gated{.schema = supported_manifest_schema, .sequence = 12};
  auto gated_target = entry(v(1, 0, 11), Channel::prerelease);
  gated_target.minimum_updater_version = v(1, 0, 10);
  gated.entries.push_back(gated_target);
  if (select_candidate(
          gated,
          {.current_version = v(1, 0, 9),
           .updater_version = v(1, 0, 9),
           .preference = ChannelPreference::stable_and_prerelease})
          .has_value()) {
    return fail("minimum_updater_version 1.0.10 must fail closed for a 1.0.9 client.");
  }
  return 0;
}

}  // namespace

int main() {
  if (const int rc = test_version_ordering()) return rc;
  if (const int rc = test_version_parsing()) return rc;
  if (const int rc = test_channel_filtering()) return rc;
  if (const int rc = test_candidate_selection()) return rc;
  if (const int rc = test_revocation_and_minimum_updater()) return rc;
  if (const int rc = test_replay_protection()) return rc;
  if (const int rc = test_schema_and_duplicates()) return rc;
  if (const int rc = test_check_scheduling()) return rc;
  if (const int rc = test_persistence_decisions()) return rc;
  if (const int rc = test_update_health_protocol()) return rc;
  if (const int rc = test_1_0_11_migration_selection()) return rc;
  std::cout << "update model tests passed\n";
  return 0;
}
