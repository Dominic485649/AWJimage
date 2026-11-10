#include "../src/compat/stdlib_compat.h"
#include "../src/ui/studio_config.h"
#include "../src/ui/studio_json.h"
#include "../src/ui/studio_queue_format.h"
#include "../src/ui/studio_worker_events.h"
#include "../src/ui/studio_queue_import.h"
#include "../src/ui/queue_preset_selection.h"
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{input}, {}};
}
void memory(const char* phase, std::size_t count, bool legacy) {
  PROCESS_MEMORY_COUNTERS_EX info{};
  check(GetProcessMemoryInfo(GetCurrentProcess(),
      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info)), "memory query failed");
  std::printf("queue=%zu legacy=%d phase=%s working_set=%zu private_bytes=%zu peak_private=%zu\n",
              count, legacy, phase, info.WorkingSetSize, info.PrivateUsage, info.PeakPagefileUsage);
}
void queue_check(std::size_t count, bool legacy) {
  using namespace awj::studio;
  namespace fs = std::filesystem;
  awj::AppConfig cfg;
  cfg.output_dir = fs::temp_directory_path() / "awj-queue-state-output";
  std::vector<QueueImageItem> queue;
  queue.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    queue.push_back(QueueImageItem{.id = i + 1,
      .path = fs::path{L"C:\\queue-fixture\\中文 input"} / awj::compat::format(L"image-{}.png", i),
      .source_root = L"C:\\queue-fixture\\中文 input", .relative_dir = L"subfolder",
      .bytes = i + 42, .status = i % 2 ? QueueItemStatus::failed : QueueItemStatus::pending,
      .log_text = "UI-only diagnostic text which must not enter the worker snapshot"});
  }
  memory("idle", count, legacy);
  const auto start = std::chrono::steady_clock::now();
  auto snapshot = legacy ? queue : std::vector<QueueImageItem>{};
  auto files = build_run_files(cfg, legacy ? snapshot : queue, false);
  check(files.has_value() && files->size() == count, "queue input count changed");
  for (std::size_t i = 0; i < count; ++i) {
    check((*files)[i].index == i && (*files)[i].path == queue[i].path &&
          (*files)[i].relative_dir == queue[i].relative_dir && (*files)[i].bytes == queue[i].bytes,
          "queue order or encoder input changed");
  }
  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
  memory("prepared", count, legacy);
  std::printf("queue=%zu legacy=%d preparation_ms=%.3f\n", count, legacy, elapsed);
  files = std::vector<awj::ImageFile>{};
  std::vector<QueueImageItem>{}.swap(snapshot);
  if (count == 10) {
    auto failed = build_run_files(cfg, queue, true);
    check(failed && failed->size() == 5 && failed->front().path == queue[1].path,
          "failed-only selection changed");
    queue[0].status = QueueItemStatus::done;
    queue[1].status = QueueItemStatus::canceled;
    check(build_run_files(cfg, queue, false)->size() == 9, "completed item was requeued");
    cfg.output_template = L"{sha256}.avif";
    check(!build_run_files(cfg, queue, false), "missing source hash did not fail closed");
  }
  std::vector<QueueImageItem>{}.swap(queue);
  check(queue.capacity() == 0 && snapshot.capacity() == 0, "queue storage retained");
  memory("clear", count, legacy);
}
void queue_output_size_check() {
  using namespace awj::studio;
  QueueImageItem item{.path = L"C:\\queue-fixture\\size.png", .bytes = 8192};
  const auto source_size = to_shared(awj::format_size(item.bytes));
  const auto pending = make_queue_task_row(item, 0);
  check(pending.size == source_size && pending.output_size == to_shared("-"),
        "pending output size must be absent without changing source size");
  item.status = QueueItemStatus::done;
  item.output_bytes = 0;
  const auto empty_output = make_queue_task_row(item, 0);
  check(empty_output.size == source_size &&
        empty_output.output_size == to_shared(awj::format_size(0)),
        "zero-byte output was mistaken for an absent output");
  item.output_bytes = 123456;
  const auto completed = make_queue_task_row(item, 0);
  check(completed.size == source_size &&
        completed.output_size == to_shared(awj::format_size(*item.output_bytes)),
        "completed output size was not formatted independently of source size");
  item.output_bytes.reset();
  item.status = QueueItemStatus::pending;
  check(make_queue_task_row(item, 0).output_size == to_shared("-"),
        "reset output size retained a previous completion");
}
void queue_resize_event_guard_check() {
  using namespace awj::studio;
  UiState state;
  state.run_id = 10;
  state.queue_items = {QueueImageItem{.id = 41, .run_index = 0},
                       QueueImageItem{.id = 42, .run_index = 1}};
  state.queue_run_indices = {{0, 0}, {1, 1}};
  state.queue_id_indices = {{41, 0}, {42, 1}};
  state.queue_items[0].encoder_threads = 1;
  const StudioWorkerResizeEvent completed{0, 125000, 8};
  check(apply_queue_worker_resize(state, 10, completed) == 0 &&
        state.queue_items[0].resize_seconds == 0.125 &&
        state.queue_items[0].resize_workers == 8 &&
        state.queue_items[0].encoder_threads == 1,
        "缩放事件未按运行索引合并，或覆盖了编码器线程数");

  // 取消后立即重试：旧事件即使索引相同，也不能污染新一轮。
  ++state.run_id;
  reset_queue_item_execution(state.queue_items[0]);
  state.queue_items[0].run_index = 0;
  check(!apply_queue_worker_resize(state, 10, completed) &&
        state.queue_items[0].resize_seconds == -1.0 &&
        state.queue_items[0].resize_workers == 0,
        "重试后仍接受了旧 run_id 的延迟缩放事件");
  check(!apply_queue_worker_resize(state, 11, {99, 1, 1}),
        "不存在的 worker 索引被接受");
  state.queue_run_indices[99] = state.queue_items.size();
  check(!apply_queue_worker_resize(state, 11, {99, 1, 1}),
        "越界映射访问了队列元素");
  state.queue_run_indices[99] = 0;
  check(!apply_queue_worker_resize(state, 11, {99, 1, 1}),
        "陈旧映射覆盖了不同运行索引的队列项");
  state.queue_items[0].status = QueueItemStatus::canceled;
  check(apply_queue_worker_resize(state, 11, {0, 75000, 0}) == 0 &&
        state.queue_items[0].resize_seconds == 0.075 &&
        state.queue_items[0].resize_workers == 0,
        "同轮取消结果丢失了已发生的缩放时间");

  // 大图子任务绑定稳定 ID，而不是主队列的行位置或 run_index。
  std::swap(state.queue_items[0], state.queue_items[1]);
  state.queue_id_indices = {{42, 0}, {41, 1}};
  check(apply_queue_worker_resize(state, 11, completed, 41) == 1 &&
        state.queue_items[1].resize_seconds == 0.125,
        "重排后大图缩放事件没有更新稳定 ID 对应的条目");
  check(!apply_queue_worker_resize(state, 10, completed, 41) &&
        !apply_queue_worker_resize(state, 11, {1, 500000, 4}, 41) &&
        !apply_queue_worker_resize(state, 11, completed, 999),
        "大图子任务接受了旧运行、非零 worker 索引或未知 ID");
  state.queue_id_indices[41] = 0;
  check(!apply_queue_worker_resize(state, 11, completed, 41) &&
        state.queue_items[0].resize_seconds == -1.0,
        "陈旧稳定 ID 映射污染了其他队列项");
}

void queue_stage_timings_check() {
  using namespace awj::studio;
  QueueImageItem item{.path = L"C:\\queue-fixture\\timing.png"};
  check(item.resize_seconds == -1.0 && item.resize_workers == 0 &&
        make_queue_task_row(item, 0).stage_timings.empty(),
        "new queue item must not claim that resize has run");
  check(stage_timings_text(-1, -1, -1, -1).empty(),
        "all-unknown stage timings should stay absent");
  check(stage_timings_text(0.010, 0.002, 1.250, 0.004) ==
            "decode 0.010s · resize - · prepare 0.002s · encode 1.250s · write 0.004s",
        "legacy formatter call synthesized a zero resize or changed stage order");
  check(stage_timings_text(-1, -1, -1, -1, 0.0, 0) ==
            "decode - · resize 0.000s (workers 0) · prepare - · encode - · write -",
        "confirmed no-transform was displayed as unknown");
  check(stage_timings_text(-1, -1, -1, -1, 0.125, 0) ==
            "decode - · resize 0.125s (workers 0) · prepare - · encode - · write -",
        "failed/canceled transform lost its partial timing");
  item.encoder_id = "aom";
  item.encoder_threads = 2;
  item.decode_seconds = 0.010;
  item.prepare_seconds = 0.002;
  item.encode_seconds = 1.250;
  item.write_seconds = 0.004;
  item.resize_seconds = 0.125;
  item.resize_workers = 8;
  item.status = QueueItemStatus::failed;
  item.run_index = 3;
  const auto completed = make_queue_task_row(item, 0);
  check(completed.threads == to_shared("2") && completed.stage_timings == to_shared(
            "decode 0.010s · resize 0.125s (workers 8) · prepare 0.002s · encode 1.250s · write 0.004s"),
        "resize workers replaced encoder threads or were not independently displayed");
  reset_queue_item_execution(item);
  const auto retried = make_queue_task_row(item, 0);
  check(item.resize_seconds == -1.0 && item.resize_workers == 0 &&
        item.decode_seconds == -1.0 && item.prepare_seconds == -1.0 &&
        item.encode_seconds == -1.0 && item.write_seconds == -1.0 &&
        item.run_index == std::numeric_limits<std::size_t>::max() &&
        item.encoder_threads == 0 && retried.threads.empty() && retried.stage_timings.empty(),
        "retry retained a previous run's resize timing or workers");
}
void queue_open_target_check() {
  using namespace awj::studio;
  QueueImageItem item{.path = L"C:\\queue-fixture\\source.png",
      .locked_output_path = L"C:\\queue-fixture\\source(7).avif.png"};
  for (const auto status : {QueueItemStatus::pending, QueueItemStatus::running,
                           QueueItemStatus::failed, QueueItemStatus::canceled,
                           QueueItemStatus::skipped}) {
    item.status = status;
    check(queue_image_open_target(item) == item.path,
          "unfinished or skipped item opened a predicted output");
  }
  item.status = QueueItemStatus::done;
  check(queue_image_open_target(item) == item.locked_output_path,
        "successful item did not open its final numbered output");
  item.locked_output_path.clear();
  check(queue_image_open_target(item).empty(),
        "success without an output path did not fail closed");
  reset_queue_item_execution(item);
  check(queue_image_open_target(item) == item.path,
        "reset item retained an old output target");
}
void queue_reimport_check() {
  using namespace awj::studio;
  namespace fs = std::filesystem;
  const fs::path root{L"C:\\queue-fixture\\reimport"};
  UiState state;
  state.next_queue_id = 3;
  state.run_id = 7;
  for (std::uint64_t id : {1u, 2u}) {
    QueueImageItem item{.id = id, .path = root / (id == 1 ? L"A.png" : L"B.png"),
        .bytes = 100, .output_bytes = 80, .status = QueueItemStatus::done,
        .run_index = static_cast<std::size_t>(id - 1), .locked_output_path = root / L"old.avif",
        .status_text = "完成", .log_text = "上轮日志", .encoder_id = "old-encoder",
        .encoder_threads = 4, .decode_seconds = 1, .prepare_seconds = 2,
        .encode_seconds = 3, .write_seconds = 4, .resize_seconds = 0.5,
        .resize_workers = 3, .warning = true};
    state.queue_items.push_back(std::move(item));
  }
  std::vector<awj::ui_import::File> incoming{
      {.path = root / L"C.png", .source_root = root, .bytes = 10},
      {.path = root / L"A.png", .bytes = 200},
      {.path = root / L".\\C.png", .bytes = 30}};
  const auto result = merge_queue_import(state, incoming);
  check(result && result->added == 1 && result->requeued == 1 && result->folded == 1,
        "重新导入数量或批内折叠错误");
  check(state.queue_items.size() == 3 && state.queue_items[0].id == 2 &&
        state.queue_items[1].id == 1 && state.queue_items[2].id == 3,
        "未保留完成项或末次导入顺序/稳定 ID 错误");
  const auto& old = state.queue_items[0];
  check(old.status == QueueItemStatus::done && old.output_bytes == 80 &&
        old.resize_seconds == 0.5 && old.resize_workers == 3 &&
        old.log_text == to_shared("上轮日志") && old.warning &&
        queue_image_open_target(old) == old.locked_output_path,
        "未重导入的完成项被重置");
  const auto& again = state.queue_items[1];
  check(again.status == QueueItemStatus::pending && again.status_text == "等待编码" &&
        again.bytes == 200 && !again.output_bytes && again.locked_output_path.empty() &&
        again.log_text.empty() && again.encoder_id.empty() && !again.encoder_threads &&
        again.decode_seconds == -1 && again.prepare_seconds == -1 &&
        again.encode_seconds == -1 && again.write_seconds == -1 &&
        again.resize_seconds == -1 && again.resize_workers == 0 && !again.warning &&
        again.run_index == std::numeric_limits<std::size_t>::max() &&
        queue_image_open_target(again) == again.path,
        "重导入未清除上轮执行字段");
  check(state.queue_items[2].bytes == 30 && state.queue_items[2].source_root.empty() &&
        state.queue_items[2].relative_dir.empty() && state.next_queue_id == 4 &&
        state.run_id == 8 && state.queue_path_keys.size() == 3 &&
        state.queue_id_indices.at(1) == 1 && !state.queue_run_indices.contains(0),
        "末次元数据、next-ID、generation 或旧 run 映射错误");
  awj::AppConfig cfg;
  cfg.output_dir = fs::temp_directory_path() / "awj-reimport-output";
  const auto files = build_run_files(cfg, state.queue_items, false);
  check(files && files->size() == 2 && (*files)[0].index == 0 && (*files)[1].index == 1 &&
        (*files)[0].path == root / L"A.png", "下一轮错误包含历史完成项或索引不连续");
  state.worker_active = true;
  check(!merge_queue_import(state, incoming) && state.queue_items[0].id == 2 &&
        state.next_queue_id == 4 && state.run_id == 8, "运行/收尾期间仍允许修改队列");
  state.worker_active = false;
  state.next_queue_id = UINT64_MAX;
  const awj::ui_import::File extra{.path = root / L"D.png", .bytes = 1};
  check(!merge_queue_import(state, std::span{&extra, 1}) && state.queue_items.size() == 3 &&
        state.run_id == 8, "准备失败破坏了原队列");
}

void queue_config_migration_check() {
  using namespace awj::studio;
  const auto parse = [](std::string_view text) {
    const auto values = awj::studio_json::parse_jsonc_config(text);
    check(values.has_value(), "queue config fixture is not valid JSONC");
    return awj::studio_json::config_queue_options(*values);
  };
  const auto defaults = parse("{}");
  check(defaults && defaults->collision_index == 4 && !defaults->write_log &&
        !defaults->delete_source, "queue defaults are not safe");
  for (const bool log : {false, true}) {
    for (const bool summary : {false, true}) {
      const auto options = parse(awj::compat::format(
          R"({{"write_log":{},"write_summary":{}}})", log, summary));
      check(options && options->write_log == (log || summary) && !options->delete_source,
            "legacy log flags changed recycling or lost OR migration");
    }
  }
  for (int index = 0; index <= 4; ++index) {
    const auto options = parse(awj::compat::format(R"({{"collision_index":{}}})", index));
    check(options && options->collision_index == index, "old collision index was reinterpreted");
  }
  for (const auto invalid : {R"({"collision_index":5})", R"({"write_log":1})",
                             R"({"write_summary":"true"})", R"({"delete_source":1})"})
    check(!parse(invalid), "invalid queue config was accepted");
}

void config_check() {
  using namespace awj::studio;
  namespace fs = std::filesystem;
  const auto path = studio_config_path();
  check(!path.empty() && !fs::exists(path), "test config path must be isolated and empty");
  auto current = StudioConfigSnapshot{};
  const auto defaults = current;
  check(current.only_show_changelog_after_update, "new changelog option must default on");
  check(current.queue_preset_file.empty(), "queue preset must default to built-in");
  current.queue_preset_file = "照片 测试.jsonc";
  check(current != defaults, "snapshot equality ignored the queue preset identity");
  current.queue_preset_file.clear();
  check(current == defaults, "cleared queue preset did not restore default snapshot equality");
  check(write_studio_config_file(current, defaults).has_value(), "default config write failed");
  const auto default_config = awj::studio_json::parse_jsonc_config(read(path));
  check(default_config && !default_config->contains("collision_index") &&
        !default_config->contains("write_log") && !default_config->contains("delete_source") &&
        awj::studio_json::config_queue_options(*default_config) ==
            std::expected<QueueRunOptions, std::string>{defaults.queue_options},
        "default queue options were not safely omitted and restored");
  check(default_config && !default_config->contains("queue_preset_file"),
        "built-in queue preset should be omitted from the config file");
  current.queue_preset_file = "照片 测试.jsonc";
  current.queue_options = {.collision_index = 0, .write_log = true, .delete_source = true};
  current.show_update_changelog = false;
  current.only_show_changelog_after_update = false;
  current.language_index = 1;
  current.last_verified_manifest_v2_sequence = 42;
  current.update_manifest_v2_raw = "signed v2 bytes";
  current.update_manifest_v2_signature = "signature";
  current.update_keyring_raw = "signed keyring bytes";
  current.update_keyring_signature = "keyring signature";
  current.menu_preset_description = "menu preset regression description";
  current.menu_params[0].chroma_index = 4;
  current.menu_params[0].size_limit_index = 2;
  current.menu_params[0].scale_percent_text = "200.5";
  current.menu_params[3].chroma_index = 4; // Removed JPG 400 from an old UI cache.
  {
    std::ofstream old(path);
    old << R"({"last_verified_manifest_sequence":17,"update_manifest_raw":"old","update_manifest_signature":"old signature","hide_update_changelog_after_exit":false,"show_update_changelog_after_update":false,"queue_preset_index":7,"queue_preset_name":"old display name","queue_format_index":3,"format_index":2,"quality_text":"88","input_path":"old-input","output_dir":"old-output","visual_quality_gpu":true,"write_log":false,"write_summary":true,"delete_source":false,"collision_index":3,"future_extension":"保留"})";
  }
  const auto legacy = awj::studio_json::parse_jsonc_config(read(path));
  check(legacy && awj::studio_json::config_string(*legacy, "queue_preset_file")
                      .value_or(std::string{}).empty(),
        "legacy queue index or name must not restore a user preset");
  check(write_studio_config_file(current, defaults).has_value(), "config write failed");
  const auto first = read(path);
  const auto parsed = awj::studio_json::parse_jsonc_config(first);
  check(parsed && !parsed->contains("visual_quality_gpu"),
        "旧 GPU 配置键不应在保存时重新写入");
  const auto queue_options = awj::studio_json::config_queue_options(*parsed);
  check(queue_options && *queue_options == current.queue_options &&
        !parsed->contains("write_summary"),
        "queue options did not round-trip canonically");
  check(parsed.has_value() && parsed->at("language_index").integer == 1 &&
        parsed->at("last_verified_manifest_v2_sequence").integer == 42 &&
        parsed->at("update_manifest_v2_raw").string == current.update_manifest_v2_raw &&
        parsed->at("update_keyring_raw").string == current.update_keyring_raw &&
        parsed->at("menu_preset_description").string == current.menu_preset_description &&
        parsed->at("queue_preset_file").string == current.queue_preset_file &&
        parsed->at("menu_avif_chroma_index").integer == 4,
        "whitelist lost current config or v2 state");
  check(parsed->at("menu_avif_size_limit_mode").string == "upscale" &&
      parsed->at("menu_avif_scale_percent_text").string == "200.5" &&
      !parsed->contains("menu_avif_size_limit_index") && parsed->at("future_extension").string == "保留",
      "新尺寸模式没有稳定序列化或未知配置被删除");
  auto selected_preset = awj::default_user_preset();
  selected_preset.source_path = fs::path{u8"照片 测试.jsonc"};
  selected_preset.name = "display name is not a persisted identity";
  const std::vector<awj::UserPreset> catalog{selected_preset};
  const auto restored_file = awj::studio_json::config_string(*parsed, "queue_preset_file");
  check(restored_file && queue_preset_index_for_file(catalog, *restored_file) == 1,
        "UTF-8 preset identity did not restore after a real config write and parse");
  for (const auto* invalid : {R"({"queue_preset_file":7})", R"({"queue_preset_file":false})"}) {
    const auto invalid_config = awj::studio_json::parse_jsonc_config(invalid);
    check(invalid_config && queue_preset_index_for_file(catalog,
              awj::studio_json::config_string(*invalid_config, "queue_preset_file")
                  .value_or(std::string{})) == 0,
          "invalid optional identity must restore the built-in preset");
  }
  check(!parsed->contains("menu_jpgli_chroma_index") ||
        parsed->at("menu_jpgli_chroma_index").integer == 0,
        "old JPG 400 UI cache was persisted instead of normalizing to auto");
  check(!parsed->at("show_update_changelog").boolean &&
        !parsed->at("only_show_changelog_after_update").boolean, "changelog preferences were not preserved");
  for (const auto* key : {"last_verified_manifest_sequence", "update_manifest_raw", "update_manifest_signature", "hide_update_changelog_after_exit", "show_update_changelog_after_update", "queue_preset_index", "queue_preset_name", "queue_format_index", "format_index", "quality_text", "input_path", "output_dir"})
    check(!parsed->contains(key), "legacy UI cache or transient queue state was persisted");
  check(!parsed->contains("menu_avif_avif_animation_tune_index") &&
        !parsed->contains("menu_avif_avif_animation_keyframe"), "CLI-only animation options persisted in UI config");
  for (const int index : {0, 1, 2, 3}) {
    auto value = current;
    value.menu_params[3].chroma_index = index;
    check(write_studio_config_file(value, defaults).has_value(), "legal JPG chroma write failed");
    const auto reloaded = awj::studio_json::parse_jsonc_config(read(path));
    check(reloaded && (index == 0 ? !reloaded->contains("menu_jpgli_chroma_index")
                                 : reloaded->at("menu_jpgli_chroma_index").integer == index) &&
          reloaded->at("menu_avif_chroma_index").integer == 4,
          "format caches reinterpreted valid chroma values or leaked across formats");
  }
  for (const int representation : {1, 2}) {
    auto value = current;
    value.menu_params[0].avif_color_representation_index = representation;
    check(write_studio_config_file(value, defaults).has_value(), "AVIF representation write failed");
    const auto reloaded = awj::studio_json::parse_jsonc_config(read(path));
    check(reloaded && (representation == 1
              ? reloaded->at("menu_avif_chroma_index").integer == 4
              : !reloaded->contains("menu_avif_chroma_index")),
          "source-follow AVIF hid valid 400 or forced Identity retained invalid 400");
  }
  auto builtin = current;
  builtin.queue_preset_file.clear();
  check(write_studio_config_file(builtin, defaults).has_value(),
        "switching the queue back to built-in could not be saved");
  const auto cleared = awj::studio_json::parse_jsonc_config(read(path));
  check(cleared && !cleared->contains("queue_preset_file") &&
        cleared->at("language_index").integer == current.language_index,
        "returning to built-in retained its old identity or lost other config");
  check(write_studio_config_file(current, defaults).has_value() && read(path) == first,
        "config rewrite was not idempotent");
  auto reports_off = current;
  reports_off.queue_options.write_log = false;
  reports_off.queue_options.delete_source = false;
  check(write_studio_config_file(reports_off, defaults).has_value(),
        "cannot disable logging and recycling");
  const auto disabled = awj::studio_json::parse_jsonc_config(read(path));
  const auto disabled_options = disabled ? awj::studio_json::config_queue_options(*disabled)
      : std::expected<QueueRunOptions, std::string>{std::unexpected{"invalid saved config"}};
  check(disabled_options && !disabled_options->write_log && !disabled_options->delete_source &&
        !disabled->contains("write_summary"),
        "old CSV flag resurrected disabled logging or recycling");
  check(write_studio_config_file(current, defaults).has_value() && read(path) == first,
        "restoring queue options changed unrelated configuration");
  {
    auto locked = adopt_win32_handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                               nullptr, OPEN_EXISTING, 0, nullptr));
    check(static_cast<bool>(locked), "cannot lock config fixture");
    check(!write_studio_config_file(defaults, defaults), "locked target was replaced");
    check(read(path) == first && !fs::exists(path.wstring() + L".tmp"),
          "failed atomic write damaged original or left temporary file");
  }
  fs::remove(path);
}
}
int main(int argc, char** argv) try {
  if (argc == 4 && std::string_view{argv[1]} == "--queue-benchmark") {
    queue_check(std::stoull(argv[2]), std::string_view{argv[3]} == "legacy");
  } else { queue_check(10, false); queue_output_size_check(); queue_stage_timings_check();
    queue_resize_event_guard_check();
    queue_open_target_check(); queue_reimport_check(); queue_config_migration_check(); config_check(); }
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
