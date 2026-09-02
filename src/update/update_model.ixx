module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module awj.update_model;

// ---------------------------------------------------------------------------
// 更新系统的纯逻辑核心。
//
// 这个模块刻意不碰 I/O、不碰平台 API、不碰 Slint：版本比较、渠道过滤、候选
// 判定和检查调度都是可以离线判断的规则，把它们和 WinHTTP/文件替换分开，才能
// 用普通单元测试覆盖「stable 用户不该看到 prerelease」「离线重启后红点保留」
// 这类安全相关的行为，而不需要真的联网或改写磁盘上的 exe。
//
// signature/下载在 awj.update_manifest 与 awj.update_runtime，Windows 事务替换在
// awj.update_windows；这些模块依赖这里，反过来不成立。
// ---------------------------------------------------------------------------

export namespace awj::update {

// --- 版本 -------------------------------------------------------------------

// 纯三段 MAJOR.MINOR.PATCH。刻意不支持 SemVer 的 -pre/+build 后缀：渠道由
// manifest 的 channel 字段决定，不从版本号字符串里猜，否则 "1.0.2-rc1" 这种
// 写法会让「同一版本号不得先 prerelease 后改为 stable」变得无法校验。
struct Version {
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t patch{};

  // 按整数逐段比较。绝不能用字符串比较：那样 "1.0.10" < "1.0.9"。
  friend constexpr auto operator<=>(const Version&, const Version&) = default;
  friend constexpr bool operator==(const Version&, const Version&) = default;
};

std::string to_string(const Version& version) {
  return std::format("{}.{}.{}", version.major, version.minor, version.patch);
}

// 严格解析：必须恰好三段十进制，无前导 '+'/'-'、无空白、无后缀。
// 宽松解析在更新路径上是危险的——manifest 里一个畸形版本不该被悄悄读成别的值。
std::expected<Version, std::string> parse_version(std::string_view text) {
  Version version{};
  std::uint32_t* const fields[3] = {&version.major, &version.minor,
                                    &version.patch};
  std::size_t pos = 0;
  for (int field = 0; field < 3; ++field) {
    if (field > 0) {
      if (pos >= text.size() || text[pos] != '.') {
        return std::unexpected{
            std::format("版本号需要 MAJOR.MINOR.PATCH 三段：{}", text)};
      }
      ++pos;
    }
    const std::size_t digits_begin = pos;
    std::uint64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      value = value * 10 + static_cast<std::uint64_t>(text[pos] - '0');
      // 允许的上界是 uint32；超出直接失败，不做截断。
      if (value > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{std::format("版本号字段超出范围：{}", text)};
      }
      ++pos;
    }
    if (pos == digits_begin) {
      return std::unexpected{std::format("版本号字段不是数字：{}", text)};
    }
    // 拒绝 "01"：允许前导零会让同一版本有多种写法，破坏单调性校验。
    if (pos - digits_begin > 1 && text[digits_begin] == '0') {
      return std::unexpected{std::format("版本号字段不允许前导零：{}", text)};
    }
    *fields[field] = static_cast<std::uint32_t>(value);
  }
  if (pos != text.size()) {
    return std::unexpected{
        std::format("版本号包含多余字符（不支持 -pre/+build 后缀）：{}", text)};
  }
  return version;
}

// --- 渠道 -------------------------------------------------------------------

enum class Channel {
  stable,
  prerelease,
};

// 用户设置。stable_only 是默认值。
enum class ChannelPreference {
  stable_only,
  stable_and_prerelease,
};

std::string_view channel_name(Channel channel) noexcept {
  return channel == Channel::prerelease ? "prerelease" : "stable";
}

std::expected<Channel, std::string> parse_channel(std::string_view text) {
  if (text == "stable") {
    return Channel::stable;
  }
  if (text == "prerelease") {
    return Channel::prerelease;
  }
  return std::unexpected{std::format("未知更新渠道：{}", text)};
}

// 渠道过滤。stable 用户永远看不到 prerelease；选了 prerelease 的用户两者都能看到
// （否则从 prerelease 升到正式版会没有升级路径）。
constexpr bool channel_visible_to(Channel channel,
                                  ChannelPreference preference) noexcept {
  return channel == Channel::stable ||
         preference == ChannelPreference::stable_and_prerelease;
}

// --- Manifest ---------------------------------------------------------------

// 当前能理解的 manifest schema。收到更大的 schema 就明确拒绝，而不是尽力解析：
// 未知字段可能承载安全语义（比如新的撤销机制），忽略它们比失败更危险。
inline constexpr std::uint32_t supported_manifest_schema = 1;

struct AssetInfo {
  std::string url{};
  std::uint64_t size_bytes{};
  std::string sha256{};  // 64 位小写十六进制
};

// v2 归档更新先校验完整 7z，再对其中每一个必需成员逐一校验。成员名属于
// 签名数据的一部分，提取器会拒绝任何未列出的条目而不是“尽量解包”。
struct ArchiveMemberInfo {
  std::string path{};
  AssetInfo asset{};
};

struct ArchiveAssetInfo {
  AssetInfo archive{};
  std::vector<ArchiveMemberInfo> members{};
};

struct Changelog {
  std::string zh_cn{};
  std::string en{};
};

struct ManifestEntry {
  Version version{};
  Channel channel{Channel::stable};
  std::string release_url{};
  std::string published_at{};  // RFC 3339 UTC
  Version minimum_updater_version{};
  // Windows 自更新需要同时替换 GUI 主程序和命令行 shim，两个资产必须成对出现。
  AssetInfo windows_x64_exe{};
  AssetInfo windows_x64_com{};
  AssetInfo linux_x64{};
  ArchiveAssetInfo windows_x64_archive{};
  ArchiveAssetInfo linux_x64_archive{};
  Changelog changelog{};
  // 签名 manifest 可以显式撤销危险版本。这是唯一允许清除待更新状态的安全例外，
  // 见 should_clear_pending_for_revocation。
  bool revoked{};
};

struct Manifest {
  std::uint32_t schema{};
  // 全局递增。用于拒绝重放攻击：签名正确但陈旧的 manifest 不能把用户降级回
  // 一个已被撤销的版本。
  std::uint64_t sequence{};
  // Every 1.0.6+ manifest names the delegated release key and carries a
  // bounded signed validity window.  Legacy clients ignore these extra JSON
  // fields, while new clients fail closed when they are absent or expired.
  std::string key_id{};
  std::string issued_at{};
  std::string expires_at{};
  std::vector<ManifestEntry> entries{};
};

// --- 候选选择 ---------------------------------------------------------------

struct UpdateCandidate {
  Version version{};
  Channel channel{Channel::stable};
  std::string release_url{};
  std::string published_at{};
  Changelog changelog{};
  AssetInfo windows_x64_exe{};
  AssetInfo windows_x64_com{};
  AssetInfo linux_x64{};
  ArchiveAssetInfo windows_x64_archive{};
  ArchiveAssetInfo linux_x64_archive{};
};

struct CandidateRequest {
  Version current_version{};
  // 当前可执行文件充当 updater 的能力版本。manifest 可以要求某个版本必须先
  // 升级到中间版本才能继续（minimum_updater_version）。
  Version updater_version{};
  ChannelPreference preference{ChannelPreference::stable_only};
};

// 选出唯一合格候选：渠道可见、版本严格高于当前、未被撤销、且当前 updater 够新。
// 返回 nullopt 表示「已是最新」——这是正常结果，不是错误。
std::optional<UpdateCandidate> select_candidate(const Manifest& manifest,
                                                const CandidateRequest& request) {
  std::optional<UpdateCandidate> best{};
  // 逐步抬高门槛：只有严格高于「当前已选中的最好版本」的条目才会替换它，
  // 所以 manifest 里条目的顺序不影响结果。
  Version best_version = request.current_version;

  for (const auto& entry : manifest.entries) {
    if (entry.revoked) {
      continue;
    }
    if (!channel_visible_to(entry.channel, request.preference)) {
      continue;
    }
    // 严格大于：等于当前版本不是更新，小于是降级。
    if (!(entry.version > best_version)) {
      continue;
    }
    // 当前 exe 太旧，无法安全执行到该版本的更新流程时跳过，让用户先走中间版本。
    if (request.updater_version < entry.minimum_updater_version) {
      continue;
    }
    best_version = entry.version;
    best = UpdateCandidate{.version = entry.version,
                           .channel = entry.channel,
                           .release_url = entry.release_url,
                           .published_at = entry.published_at,
                           .changelog = entry.changelog,
                           .windows_x64_exe = entry.windows_x64_exe,
                           .windows_x64_com = entry.windows_x64_com,
                           .linux_x64 = entry.linux_x64,
                           .windows_x64_archive = entry.windows_x64_archive,
                           .linux_x64_archive = entry.linux_x64_archive};
  }

  return best;
}

// 重放保护：签名正确但陈旧的 manifest 必须被拒绝，否则攻击者可以重放一份旧的
// 已签名 manifest，把用户导向一个后来被撤销的版本。
//
// 判定用 >=（不是 >）：发布者不发新版时，同一份 manifest 会被反复取到，此时
// sequence 等于本机已验证值，这属于正常情况，必须接受，否则第二次检查起就永远
// 失败。真正要挡的是 sequence 变小——那只可能是重放或回滚攻击。
//
// 「同一 sequence 内容不得改变」由发布流程保证（确定性生成 + 签名覆盖原始字节），
// 客户端无法也不需要在这里校验。
bool is_manifest_fresh(const Manifest& manifest,
                       std::uint64_t last_verified_sequence) noexcept {
  return manifest.sequence >= last_verified_sequence;
}

// 撤销是唯一允许清除已保存待更新状态的理由。悬停、打开日志页、切换页面、重启
// 或检查失败都不清除红点——那些都不代表「这个版本不该再装」。
bool should_clear_pending_for_revocation(const Manifest& manifest,
                                         const Version& pending_version) {
  return std::ranges::any_of(manifest.entries, [&](const ManifestEntry& entry) {
    return entry.revoked && entry.version == pending_version;
  });
}

// --- 单调性校验 -------------------------------------------------------------

// 纯三段版本号必须全局单调递增，且同一版本号不得先 prerelease 后改为 stable。
// 发布脚本和客户端都调用它：客户端侧可以挡下被篡改成「重用旧版本号」的
// manifest，即使签名有效（例如私钥泄露后的降级攻击）。
std::expected<void, std::string> validate_manifest_consistency(
    const Manifest& manifest) {
  if (manifest.schema != supported_manifest_schema) {
    return std::unexpected{
        std::format("不支持的 manifest schema {}，当前只支持 {}。",
                    manifest.schema, supported_manifest_schema)};
  }
  // 按版本号排序后只需比较相邻项即可查重，同时得到稳定的报错顺序。
  // 排序的是索引，manifest 本身保持不变（签名覆盖的是原始字节）。
  std::vector<const ManifestEntry*> sorted{};
  sorted.reserve(manifest.entries.size());
  for (const auto& entry : manifest.entries) {
    sorted.push_back(&entry);
  }
  std::ranges::sort(sorted, [](const ManifestEntry* a, const ManifestEntry* b) {
    return a->version < b->version;
  });
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i - 1]->version != sorted[i]->version) {
      continue;
    }
    // 同一版本号出现两次即为错误，无论渠道是否相同。特别地，先发 prerelease
    // 再用同一号码发 stable 会让「用户装的到底是哪个构建」无法判定。
    if (sorted[i - 1]->channel != sorted[i]->channel) {
      return std::unexpected{std::format(
          "manifest 中版本 {} 同时出现在 stable 与 prerelease；"
          "同一版本号不得跨渠道复用。",
          to_string(sorted[i]->version))};
    }
    return std::unexpected{
        std::format("manifest 中版本 {} 重复；同一版本号不得出现多次。",
                    to_string(sorted[i]->version))};
  }
  return {};
}

// --- 检查调度 ---------------------------------------------------------------

inline constexpr auto automatic_check_interval = std::chrono::hours{24 * 7};

enum class CheckTrigger {
  startup,            // 启动时的自动判定
  manual,             // 用户点「立即检查更新」
  channel_changed,    // 用户切换渠道
};

struct CheckScheduleRequest {
  CheckTrigger trigger{CheckTrigger::startup};
  // 上次成功检查的 UTC 时间。nullopt 表示从未成功过（或配置里没有有效日期）。
  std::optional<std::chrono::system_clock::time_point> last_successful_check{};
  std::chrono::system_clock::time_point now{};
};

// 手动检查和切换渠道不受七天限制；启动时没有有效日期就立即检查；否则等满七天。
//
// 未来时间戳（用户改过系统时钟，或配置被手工编辑）同样触发检查：把它当成
// 「不可信」比信任它更安全，否则一个被写进 2099 年的时间戳会永久禁用更新。
bool should_check_now(const CheckScheduleRequest& request) noexcept {
  if (request.trigger != CheckTrigger::startup) {
    return true;
  }
  if (!request.last_successful_check) {
    return true;
  }
  const auto last = *request.last_successful_check;
  if (last > request.now) {
    return true;
  }
  return request.now - last >= automatic_check_interval;
}

// --- 持久化状态 -------------------------------------------------------------

// 写进 AWJ.jsonc 的待更新状态。检查失败时整块保留不动：网络、签名、解析、
// 持久化任一失败都不更新时间、不清除已有待更新状态。
struct PendingUpdate {
  Version version{};
  Channel channel{Channel::stable};
  std::string release_url{};
  std::string published_at{};
  Changelog changelog{};  // 缓存仅用于展示，执行更新前必须重新获取并验签
};

enum class CheckOutcome {
  update_available,
  up_to_date,
  failed,
};

struct CheckResult {
  CheckOutcome outcome{CheckOutcome::failed};
  std::optional<PendingUpdate> pending{};
  std::uint64_t verified_sequence{};
  std::string message{};
};

// 检查结果如何影响持久化状态。分成独立函数是因为「什么时候更新时间戳」是这套
// 设计里最容易写错的地方：只有成功才更新时间，失败必须保持原样。
struct PersistenceDecision {
  bool update_last_successful_check{};
  bool write_pending{};
  bool clear_pending{};
};

PersistenceDecision decide_persistence(const CheckResult& result) noexcept {
  switch (result.outcome) {
    case CheckOutcome::update_available:
      return {.update_last_successful_check = true, .write_pending = true};
    case CheckOutcome::up_to_date:
      // 成功但无新版本：只更新时间，不显示红点。不主动清除已有 pending——
      // 那只可能因为撤销而清除，由 should_clear_pending_for_revocation 决定。
      return {.update_last_successful_check = true};
    case CheckOutcome::failed:
    default:
      // 失败：不更新时间，不清除已有待更新状态，红点保留。
      return {};
  }
}

// --- 更新健康检查 -----------------------------------------------------------

// helper 在启动新二进制之前已经重新验证了签名 keyring / manifest、目标版本和
// AWJ.exe/AWJ.com 成员哈希。新进程的健康握手因此只需要确认 helper 传入的已验证
// 目标版本确实就是当前 build；AWJ.jsonc 里的 pending_update_version 只是 UI 缓存，
// 不能反过来否定已经通过认证并成功启动的新二进制。
struct UpdateHealthHandshakeDecision {
  bool signal_ready{};
  bool clear_matching_pending{};
};

UpdateHealthHandshakeDecision decide_update_health_handshake(
    std::string_view verified_target_version, std::string_view build_version,
    std::string_view pending_version) noexcept {
  if (verified_target_version.empty() ||
      verified_target_version != build_version) {
    return {};
  }
  return {.signal_ready = true,
          .clear_matching_pending = pending_version == build_version};
}

enum class UpdateHealthObservation {
  event_not_signaled,
  process_exited_early,
  ready,
};

UpdateHealthObservation classify_update_health_observation(
    bool event_signaled, bool process_alive_after_grace) noexcept {
  if (!event_signaled) return UpdateHealthObservation::event_not_signaled;
  if (!process_alive_after_grace) {
    return UpdateHealthObservation::process_exited_early;
  }
  return UpdateHealthObservation::ready;
}

}  // namespace awj::update
