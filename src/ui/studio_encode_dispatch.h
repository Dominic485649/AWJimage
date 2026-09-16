#pragma once

// 编码任务的启动与队列交互调度：运行快照构建、queue manifest、
// begin_*_conversion_run、队列右键菜单与拖拽重排。
// 从 main.cpp 拆出。guarded_worker 是模板，定义在头文件里。

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "awj_studio.h"
#include "studio_state.h"
#include "studio_ui_util.h"

namespace awj::studio {

// 把一个 worker 线程体包进 catch-all。
//
// 这不是多余的保险：从线程函数逃出的异常会直接走 std::terminate -> abort，进程
// 静默消失，只在 Windows 错误报告里留下 0xC0000409 / FAST_FAIL_FATAL_APP_EXIT(7)。
// 注意包在 std::jthread 构造外面的 try 只能接住“创建线程失败”，接不到线程体内部。
//
// 出错后不能在这里直接写 Slint 属性——那是跨线程改 UI。状态清理走带锁的
// clear_run_if_callback_not_posted 终止仍在运行的子进程并清理状态，界面提示走
// post_to_ui 回到 UI 线程。
template <class Body>
auto guarded_worker(slint::ComponentWeakHandle<AwjStudio> weak,
                    std::shared_ptr<UiState> state, std::uint64_t run_id,
                    std::string_view what, Body body) {
  return [weak, state = std::move(state), run_id, what,
          body = std::move(body)](std::stop_token token) mutable {
    try {
      body(std::move(token));
    } catch (...) {
      clear_run_if_callback_not_posted(*state, run_id);
      post_to_ui(weak, [what](AwjStudio& app) {
        app.set_running(false);
        set_status_text_noexcept(
            app, std::format("{}发生未预期异常，任务已停止。", what));
      });
    }
  };
}

LargeImageRow make_large_image_row(const awj::BatchLargeImageItem& item,
                                   std::string_view status);
std::expected<void, std::string> open_path(std::filesystem::path path,
                                           bool create_if_missing);
std::expected<void, std::string> open_file_with_default_app(
    const std::filesystem::path& path);
std::expected<void, std::string> copy_text_to_clipboard(
    std::wstring_view text);
bool output_template_contains(std::wstring_view text,
                              std::wstring_view token);
bool large_image_grid_available(const awj::BatchLargeImageItem& item) noexcept;
bool large_image_action_available(const awj::BatchLargeImageItem& item,
                                  std::string_view action) noexcept;
std::string large_image_actions_summary(const awj::BatchLargeImageItem& item);
void add_large_image_task_row(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::BatchLargeImageItem& item) noexcept;
bool push_large_image_row(UiState& state, awj::BatchLargeImageItem item) noexcept;
void select_first_large_image_from_state(AwjStudio& app,
                                         const UiState& state) noexcept;
std::string large_image_action_status(const awj::BatchLargeImageItem& item,
                                      std::string_view action);
void append_log_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                    std::string_view text) noexcept;
std::string result_status_text(const awj::EncodeResult& result);
std::string result_log_text(const awj::EncodeResult& result);
bool push_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                   TaskRow row) noexcept;
TaskRow task_row_from_result(const awj::EncodeResult& result);
TaskRow pending_shell_task_row(const awj::AppConfig& cfg,
                               const awj::ImageFile& image);
void mark_task_row_running(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::EncodeResult& result) noexcept;
void add_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                  const awj::EncodeResult& result) noexcept;
void append_pending_event(UiState& state, std::uint64_t run_id,
                          awj::BatchProgress event);
void set_large_image_status(UiState& state, int index,
                            std::string_view status) noexcept;

std::expected<std::vector<awj::ImageFile>, std::string> build_run_files(
    const awj::AppConfig& cfg, const std::vector<QueueImageItem>& queue,
    bool failed_only);
std::expected<std::filesystem::path, std::string> create_studio_queue_manifest(
    std::uint64_t run_id, std::span<const awj::ImageFile> files);

void begin_queue_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                const std::shared_ptr<UiState>& state,
                                awj::AppConfig cfg, bool failed_only = false);
void begin_child_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                const std::shared_ptr<UiState>& state,
                                awj::AppConfig cfg,
                                std::optional<int> large_index = std::nullopt);

void handle_queue_menu_action(AwjStudio& app,
                              const std::shared_ptr<UiState>& state, int index,
                              std::string action);
slint::DataTransfer make_queue_drag_data(
    const std::shared_ptr<UiState>& state, int index);
std::optional<std::size_t> queue_drop_target_index(
    const UiState& state, std::size_t current, int target_slot) noexcept;
slint::language::DragAction handle_queue_drag_can_drop(
    const std::shared_ptr<UiState>& state, slint::language::DropEvent event,
    int target_slot);
slint::language::DragAction handle_queue_drag_dropped(
    AwjStudio& app, const std::shared_ptr<UiState>& state,
    slint::language::DropEvent event, int target_slot);
void handle_queue_pointer_event(AwjStudio& app,
                                const std::shared_ptr<UiState>& state,
                                int index, int button, int kind,
                                float local_y);

}  // namespace awj::studio
