#include <private/slint_tests_helpers.h>
#include <mutex>

// 平台队列函数位于匿名命名空间，直接编译生产实现，避免为测试重写一套合并/回填逻辑。
// 仅本测试目标包含该文件，不与应用的同名实现链接。
#include "../src/ui/studio_run_linux.cpp"

import awj.image;
import awj.codec;
import awj.png_codec;

namespace {
void test_check(bool value, const char* message) {
  if (!value) throw std::runtime_error{message};
}
template<class T> T test_require(std::expected<T, std::string> value) {
  if (!value) throw std::runtime_error{value.error()};
  return std::move(*value);
}
void fixture_png(const fs::path& path) {
  awj::ImageBuffer image{.width = 8, .height = 8, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 8};
  image.planes.push_back({.bytes = std::vector<std::byte>(8 * 8 * 4, std::byte{255}), .stride = 8 * 4});
  const auto encoded = test_require(awj::PngImageEncoder{}.encode(image,
      {.output_format = awj::OutputFormat::png, .quality = 100, .bit_depth = 8}));
  std::ofstream output{path, std::ios::binary};
  output.write(reinterpret_cast<const char*>(encoded.encoded.bytes.data()),
               static_cast<std::streamsize>(encoded.encoded.bytes.size()));
  test_check(static_cast<bool>(output), "PNG 测试输入写入失败");
}

std::uint64_t encode_queue(AwjStudio& app, LinuxUiState& state, const fs::path& output,
                            std::size_t expected_count) {
  auto cfg = awj::default_app_config();
  cfg.input_path = state.queue_files.front().file.path;
  cfg.output_dir = output;
  cfg.output_format = awj::OutputFormat::png;
  cfg.output_template = L"{name}.png";
  cfg.quality = 100;
  auto files = test_require(build_linux_queue_files(cfg, state.queue_files));
  test_check(files.size() == expected_count, "历史完成项错误进入本轮快照");
  for (std::size_t i = 0; i < files.size(); ++i)
    test_check(files[i].index == i, "本轮编码索引不连续");
  const auto manifest = test_require(create_linux_queue_manifest(state.next_queue_run_id++, files, cfg));
  cfg.studio_queue_manifest = manifest;
  const auto generation = test_require(prepare_linux_queue_run(app, state, files));
  app.set_running(true);
  std::mutex events_mutex;
  std::vector<awj::BatchProgress> events;
  const auto summary = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
    std::scoped_lock lock{events_mutex};
    events.push_back(event);
  });
  // 编码回调可以来自工作线程；只在 UI 线程应用拥有型事件快照。
  for (const auto& event : events) {
    if (event.kind == awj::BatchEventKind::item_started)
      mark_linux_task_row_running(app, state.task_rows, state, event.result);
    else if (event.kind == awj::BatchEventKind::item_finished)
      set_linux_task_row_result(app, state.task_rows, state, event.result);
  }
  if (!summary) throw std::runtime_error{summary.error()};
  test_check(summary->ok_count == expected_count && !summary->failed_count && !summary->canceled_count,
             "生产 PNG 编码闭环失败");
  test_check(finish_linux_queue_run(app, state, generation, false), "当前运行被错误拒绝收尾");
  app.set_progress(1.0f);
  std::error_code ec;
  fs::remove(manifest, ec);
  return generation;
}

void preset_priority_check(const fs::path& root) {
  auto app = AwjStudio::create();
  LinuxUiState state;
  for (int i = 0; i < 5; ++i) state.builtin_params[i] = default_linux_parameter_params(i);
  apply_linux_parameter_params(*app, state.builtin_params.front(), 0);
  app->set_input_path(to_shared(awj::path_to_utf8(root / "A.png")));
  app->set_output_dir(to_shared(awj::path_to_utf8(root / "preset-output")));
  auto preset = awj::default_user_preset();
  preset.name = "Linux UI CLI contract";
  preset.formats[0].quality = 65;
  preset.cli_args_low_priority = {"--quality", "30", "--timeout-encode", "5", "--log", "--avif-progressive"};
  preset.cli_args_high_priority = {"--quality", "90", "--timeout-encode", "9", "--append-png-suffix"};
  state.user_presets.push_back(preset);
  app->set_queue_preset_index(1);
  auto cfg = test_require(config_from_ui(*app, state));
  test_check(cfg.quality == 90 && cfg.encode_timeout_minutes == 9 && cfg.avif_layers.automatic &&
      cfg.append_png_suffix && !cfg.write_log && cfg.runtime_memory_policy.has_value() &&
      state.user_presets[0].formats[0].quality == 65 && !app->get_write_log(),
      "Linux 生产 UI 分层不一致或污染可视参数");
  auto summary = test_require(awj::run_batch(cfg));
  test_check(summary.ok_count == 1 && !summary.failed_count, "Linux UI 最终预设配置真实转换失败");
  std::size_t outputs = 0;
  for (const auto& entry : fs::directory_iterator(cfg.output_dir)) {
    if (entry.is_regular_file() && entry.path().filename().native().ends_with(".avif.png") && entry.file_size()) ++outputs;
  }
  test_check(outputs == 1, "Linux UI 的高优先级后缀参数没有作用于实际写出");
  state.user_presets[0].cli_args_high_priority = {"--format", "png", "--no-avif-progressive"};
  cfg = test_require(config_from_ui(*app, state));
  test_check(cfg.output_format == awj::OutputFormat::png && cfg.quality == 100 && !cfg.speed &&
      !cfg.avif_layers.enabled(), "Linux UI 跨格式覆盖残留了 AVIF 参数");
  state.user_presets[0].cli_args_high_priority.clear();
  cfg = test_require(config_from_ui(*app, state));
  test_check(cfg.quality == 65 && cfg.encode_timeout_minutes == 5 && cfg.avif_layers.automatic &&
      !cfg.append_png_suffix && !cfg.write_log, "Linux UI 没有覆盖低层可见选项或保留 CLI-only 选项");
  app->set_queue_preset_index(0);
  cfg = test_require(config_from_ui(*app, state));
  test_check(cfg.quality == awj::default_quality_for(awj::OutputFormat::avif) &&
      !cfg.avif_layers.enabled() && !cfg.append_png_suffix, "用户隐藏命令泄漏到 Linux 内置默认");
  std::puts("Linux 生产 UI 预设优先级及真实渐进 AVIF 写出通过");
}

void queue_session_config_check() {
  const auto executable = test_require(awj::executable_path());
  const auto path = executable.parent_path() / "AWJ.jsonc";
  test_check(executable.parent_path().parent_path().filename() == "linux-queue-contract" &&
                 !fs::exists(path), "配置测试必须使用空的专属运行目录");
  auto app = AwjStudio::create();
  const auto switches = [&] {
    return std::array{app->get_strip_metadata(), app->get_write_log(), app->get_delete_source(),
        app->get_preserve_creation_time(), app->get_preserve_modification_time(), app->get_preserve_access_time()};
  };
  const std::array<bool, 6> off{};
  test_check(switches() == off, "新会话六项开关未全部关闭");
  nlohmann::ordered_json legacy{{"theme_index", 2}, {"language_index", 1}, {"collision_index", 3},
      {"future_extension", "保留"}, {"last_verified_manifest_v2_sequence", 42},
      {"update_manifest_v2_raw", "signed bytes"}, {"update_manifest_v2_signature", "signature"}};
  // 生产配置的菜单数组固定为五种格式，不能用单项数组冒充有效输入。
  LinuxUiState fixture;
  fixture.menu_params[0].strip_metadata = true;
  legacy["menu_params"] = nlohmann::ordered_json::array();
  for (std::size_t i = 0; i < fixture.menu_params.size(); ++i)
    legacy["menu_params"].push_back(linux_menu_params_json(fixture.menu_params[i], static_cast<int>(i)));
  for (const auto key : awj::studio::queue_session_config_keys) legacy[std::string{key}] = true;
  const auto write_fixture = [&](const nlohmann::ordered_json& document) {
    test_check(atomic_write_linux_file(path, document.dump(2)).has_value(), "配置输入写入失败");
  };
  write_fixture(legacy);
  LinuxUiState state;
  load_linux_update_config(*app, state);
  test_check(state.config_readable && switches() == off && app->get_theme_index() == 2 &&
      app->get_language_index() == 1 && app->get_collision_index() == 3 && state.menu_params[0].strip_metadata,
      "旧配置恢复了会话开关或丢失独立持久参数");
  app->set_strip_metadata(true);
  app->set_write_log(true);
  app->set_delete_source(true);
  app->set_preserve_creation_time(true);
  app->set_preserve_modification_time(true);
  app->set_preserve_access_time(true);
  const auto selected = switches();
  test_check(selected == std::array{true, true, true, true, true, true}, "会话开关不可选");
  for (const auto key : awj::studio::queue_session_config_keys) legacy[std::string{key}] = "旧值";
  write_fixture(legacy);
  load_linux_update_config(*app, state);
  test_check(state.config_readable && switches() == selected, "重载其他设置清空了会话开关");
  test_check(persist_linux_update_config(*app, state).has_value() && switches() == selected,
      "保存配置失败或修改了当前会话开关");
  const auto read_config = [&] {
    std::ifstream input{path, std::ios::binary};
    test_check(static_cast<bool>(input), "配置输出不可读");
    return std::string{std::istreambuf_iterator<char>{input}, {}};
  };
  const auto first = read_config();
  const auto saved = nlohmann::ordered_json::parse(first);
  for (const auto key : awj::studio::queue_session_config_keys)
    test_check(!saved.contains(std::string{key}), "会话根键被回写配置");
  test_check(saved.at("future_extension") == "保留" && saved.at("collision_index") == 3 &&
      saved.at("menu_params")[0].at("strip_metadata") == true &&
      saved.at("last_verified_manifest_v2_sequence") == 42 &&
      saved.at("update_manifest_v2_raw") == "signed bytes" && saved.at("update_manifest_v2_signature") == "signature",
      "保存丢失了未知扩展、菜单参数或更新器状态");
  test_check(persist_linux_update_config(*app, state).has_value() && read_config() == first,
      "配置保存不幂等");
  auto restarted = AwjStudio::create();
  LinuxUiState restarted_state;
  load_linux_update_config(*restarted, restarted_state);
  test_check(restarted_state.config_readable && !restarted->get_strip_metadata() && !restarted->get_write_log() &&
      !restarted->get_delete_source() && !restarted->get_preserve_creation_time() &&
      !restarted->get_preserve_modification_time() && !restarted->get_preserve_access_time() &&
      restarted->get_collision_index() == 3 && restarted_state.menu_params[0].strip_metadata,
      "新会话恢复了临时开关或丢失持久参数");
  test_check(fs::remove(path), "测试专属配置未清理");
  std::puts("Linux 生产配置六项会话开关加载/保存/重载/新会话回归通过");
}
}

int main(int argc, char** argv) try {
  test_check(argc == 2, "需要项目内的测试工作目录");
  const fs::path root = fs::path{argv[1]} / std::to_string(::getpid());
  test_check(!fs::exists(root), "拒绝覆盖已有队列测试输入");
  fs::create_directories(root);
  for (const auto* name : {"A.png", "B.png", "C.png"}) fixture_png(root / name);
  slint::testing::init();
  preset_priority_check(root);
  queue_session_config_check();
  auto app = AwjStudio::create();
  LinuxUiState state;
  state.task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
  state.large_image_rows = std::make_shared<slint::VectorModel<LargeImageRow>>();
  awj::ui::bind_queue_model(*app, state.task_rows);
  const std::array first{root / "A.png", root / "B.png"};
  test_check(test_require(add_linux_queue_from_paths(*app, state, first, true)), "首次导入失败");
  const auto a_id = state.queue_files[0].id;
  const auto b_id = state.queue_files[1].id;
  const auto old_generation = encode_queue(*app, state, root / "first-output", 2);
  test_check(app->get_queue_success_count() == 2 && app->get_queue_pending_count() == 0,
             "首轮完成状态/计数错误");
  const auto b_result = state.queue_files[1].row;
  app->set_selected_queue_index(0);
  const std::array again{root / "C.png", root / "A.png", root / "." / "C.png"};
  test_check(test_require(add_linux_queue_from_paths(*app, state, again, false)), "混合重导入失败");
  test_check(state.queue_files.size() == 3 && state.queue_files[0].id == b_id &&
      state.queue_files[1].id == a_id && state.queue_files[0].row.state == 2 &&
      state.queue_files[1].row.state == 0 && state.queue_files[2].row.state == 0,
      "Linux 未保留完成项、稳定身份或末次导入位置");
  test_check(state.queue_files[0].row.output_path == b_result.output_path &&
      state.queue_files[0].row.output_size == b_result.output_size &&
      app->get_queue_success_count() == 1 && app->get_queue_pending_count() == 2 &&
      app->get_selected_queue_index() == 1 && app->get_progress() == 0,
      "重导入破坏旧结果、计数、选择或进度");
  app->set_queue_failed_only(true);
  test_check(test_require(add_linux_queue_from_path(*app, state, root / "A.png", false)), "再次重导入失败");
  test_check(app->get_selected_queue_index() == -1, "失败过滤仍选中了已转为 pending 的隐藏行");
  app->set_queue_failed_only(false);
  auto cfg = awj::default_app_config();
  cfg.output_dir = root / "canceled-output";
  const auto files = test_require(build_linux_queue_files(cfg, state.queue_files));
  const auto current = test_require(prepare_linux_queue_run(*app, state, files));
  app->set_running(true);
  awj::EncodeResult started{};
  started.index = 0;
  started.input_path = files.front().path;
  mark_linux_task_row_running(*app, state.task_rows, state, started);
  apply_linux_frame_progress(*app, state, state.task_rows, current, 0, 1, 2);
  test_check(state.queue_files[0].row.state == 2 && state.queue_files[1].row.state == 1 &&
      state.queue_files[1].row.status == to_shared("编码帧 1/2"), "FRAME 使用了显示位置而非本轮稳定映射");
  app->set_status_text(to_shared("当前运行"));
  const auto row_status = state.queue_files[1].row.status;
  apply_linux_frame_progress(*app, state, state.task_rows, old_generation, 0, 2, 2);
  test_check(!finish_linux_queue_run(*app, state, old_generation, false) && app->get_running() &&
      state.queue_files[1].row.status == row_status && app->get_status_text() == to_shared("当前运行"),
      "旧运行的 FRAME/终结回调污染了新状态");
  test_check(!add_linux_queue_from_path(*app, state, root / "A.png", false), "运行/收尾期间仍允许导入");
  test_check(finish_linux_queue_run(*app, state, current, true), "取消收尾失败");
  test_check(state.queue_files[0].row.state == 2 && state.queue_files[1].row.state == 4 &&
      state.queue_files[2].row.state == 4, "取消修改了未参加本轮的历史完成项");
  encode_queue(*app, state, root / "second-output", 2);
  test_check(app->get_queue_success_count() == 3 && app->get_queue_pending_count() == 0 &&
      state.queue_files[0].row.output_path == b_result.output_path, "第二轮错误重跑了未重导入的 B");
  const auto generation = state.run_generation;
  const auto invalid = add_linux_queue_from_path(*app, state, root / "missing.png", false);
  test_check(invalid && !*invalid && state.run_generation == generation &&
      app->get_queue_success_count() == 3 && app->get_progress() == 1, "无效输入修改了完成队列或进度");
  std::printf("Linux 生产队列导入/PNG 编码/重导入/取消/迟到回调闭环通过：%s\n", root.c_str());
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
