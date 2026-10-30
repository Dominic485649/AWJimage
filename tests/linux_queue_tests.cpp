#include <private/slint_tests_helpers.h>
#include <mutex>

// 平台队列函数位于匿名命名空间，直接编译生产实现，避免为测试重写一套合并/回填逻辑。
// 仅本测试目标包含该文件，不与应用的同名实现链接。
#include "../src/ui/studio_run_linux.cpp"

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
  const auto manifest = test_require(create_linux_queue_manifest(state.next_queue_run_id++, files));
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
}

int main(int argc, char** argv) try {
  test_check(argc == 2, "需要项目内的测试工作目录");
  const fs::path root = fs::path{argv[1]} / std::to_string(::getpid());
  test_check(!fs::exists(root), "拒绝覆盖已有队列测试输入");
  fs::create_directories(root);
  for (const auto* name : {"A.png", "B.png", "C.png"}) fixture_png(root / name);
  slint::testing::init();
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
