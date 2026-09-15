#include "studio_encode_dispatch.h"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cwctype>
#include <format>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "studio_fields.h"
#include "studio_queue_rows.h"
#include "studio_queue_format.h"
#include "studio_shell_cli.h"
#include "studio_ui_util.h"
#include "studio_worker_control.h"
#include "studio_worker_events.h"

import awj.core;
import awj.encoding_defaults;
import awj.pipeline;
import awj.studio_defaults;

namespace awj::studio {

void append_log_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                    std::string_view text) noexcept {
  try {
    push_task_row(rows, TaskRow{.order = {},
                                .filename = {},
                                .folder = {},
                                .size = {},
                                .status = {},
                                .output = {},
                                .log = to_shared(text),
                                .warning = false,
                                .locked = true});
  } catch (...) {
  }
}


void set_large_image_status(UiState& state, int index,
                            std::string_view status) noexcept {
  if (state.large_image_rows == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= state.large_image_items.size()) {
    return;
  }
  try {
    state.large_image_rows->set_row_data(
        static_cast<std::size_t>(index),
        make_large_image_row(
            state.large_image_items[static_cast<std::size_t>(index)], status));
  } catch (...) {
  }
}


std::expected<std::vector<awj::ImageFile>, std::string> build_run_files(
    const awj::AppConfig& cfg, const std::vector<QueueImageItem>& queue,
    bool failed_only) {
  try {
    std::vector<awj::ImageFile> files;
    files.reserve(std::ranges::count_if(
        queue, [failed_only](const QueueImageItem& item) {
          return queue_item_selected_for_run(item, failed_only);
        }));
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    const bool needs_hash =
        output_template_contains(cfg.output_template, L"{hash}") ||
        output_template_contains(cfg.output_template, L"{hash8}");
    const bool needs_sha256 =
        output_template_contains(cfg.output_template, L"{sha256}") ||
        output_template_contains(cfg.output_template, L"{sha2568}") ||
        output_template_contains(cfg.output_template, L"{sha256_8}");
    for (const auto& item : queue) {
      if (!queue_item_selected_for_run(item, failed_only)) {
        continue;
      }
      std::wstring hash;
      std::wstring sha256;
      if (needs_hash) {
        if (auto ok = awj::file_hash_token(item.path, hash); !ok) {
          return std::unexpected{ok.error()};
        }
      }
      if (needs_sha256) {
        if (auto ok = awj::file_sha256_token(item.path, sha256); !ok) {
          return std::unexpected{ok.error()};
        }
      }
      files.push_back(awj::make_image_file(files.size(), item.path,
                                           item.relative_dir,
                                           item.bytes, rng,
                                           std::move(hash),
                                           std::move(sha256)));
    }
    if (auto disambiguated = awj::apply_source_extension_disambiguation(cfg, files);
        !disambiguated) {
      return std::unexpected{disambiguated.error()};
    }
    if (auto resolved = awj::resolve_batch_output_paths(cfg, files); !resolved) {
      return std::unexpected{resolved.error()};
    }
    return files;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"构建队列运行快照时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"构建队列运行快照时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"构建队列运行快照时文件系统访问失败。"};
  }
}

std::expected<std::filesystem::path, std::string>
create_studio_queue_manifest(std::uint64_t run_id,
                             std::span<const awj::ImageFile> files) {
  try {
    std::error_code ec;
    const auto temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
      return std::unexpected{std::format(
          "无法获取 Studio worker 临时目录：{}", ec.message())};
    }
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    for (int attempt = 0; attempt < 16; ++attempt) {
      const auto path = temp_dir / std::format(
                                       L"AWJStudioQueue-{}-{}-{:016x}.awjq",
                                       GetCurrentProcessId(), run_id, rng());
      if (std::filesystem::exists(path, ec)) {
        ec.clear();
        continue;
      }
      auto written = awj::write_studio_queue_manifest(path, files);
      if (written) {
        return path;
      }
      std::filesystem::remove(path, ec);
      return std::unexpected{written.error()};
    }
    return std::unexpected{"无法创建唯一的 Studio 队列 manifest。"};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"创建 Studio 队列 manifest 时内存不足。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"创建 Studio 队列 manifest 时文件系统访问失败。"};
  }
}


void begin_queue_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                 const std::shared_ptr<UiState>& state,
                                 awj::AppConfig cfg,
                                 bool failed_only) {
  auto app = weak.lock();
  if (!app) {
    return;
  }

  std::vector<QueueImageItem> queue_snapshot;
  {
    std::scoped_lock lock{state->mutex};
    if (state->worker_active) {
      (*app)->set_status_text(to_shared("当前任务正在运行，请先停止任务或强制终止"));
      return;
    }
    if (state->queue_items.empty()) {
      (*app)->set_status_text(to_shared("队列为空，请先输入或选择图片。"));
      return;
    }
    queue_snapshot = state->queue_items;
  }

  auto files = build_run_files(cfg, queue_snapshot, failed_only);
  if (!files) {
    (*app)->set_status_text(
        to_shared(std::format("队列准备失败：{}", files.error())));
    return;
  }
  if (files->empty()) {
    (*app)->set_status_text(
        to_shared(failed_only
                      ? "队列中没有失败项。"
                      : "队列中没有待编码图片；请清空队列或添加新图片。"));
    return;
  }

  std::vector<std::filesystem::path> temp_directories;
  try {
    temp_directories.push_back(awj::output_dir_for(cfg));
    for (const auto& file : *files) {
      const auto directory = awj::output_path_for(cfg, file).parent_path();
      if (std::ranges::find(temp_directories, directory) ==
          temp_directories.end()) {
        temp_directories.push_back(directory);
      }
    }
  } catch (...) {
    (*app)->set_status_text(to_shared("队列准备失败：无法记录临时输出目录。"));
    return;
  }

  std::uint64_t run_id{};
  {
    std::scoped_lock lock{state->mutex};
    run_id = ++state->run_id;
    state->worker_active = true;
    state->active_child.reset();
    state->pending_events.clear();
    std::size_t run_index = 0;
    for (auto& item : state->queue_items) {
      item.run_index = std::numeric_limits<std::size_t>::max();
      if (!queue_item_selected_for_run(item, failed_only)) {
        continue;
      }
      item.status = QueueItemStatus::pending;
      item.status_text = "等待编码";
      item.log_text.clear();
      item.encoder_id.clear();
      item.encoder_threads = 0;
      item.decode_seconds = -1.0;
      item.prepare_seconds = -1.0;
      item.encode_seconds = -1.0;
      item.write_seconds = -1.0;
      item.warning = false;
      item.run_index = run_index;
      item.locked_output_path = awj::output_path_for(cfg, (*files)[run_index]);
      ++run_index;
    }
    refresh_queue_rows(**app, *state);
  }
  (*app)->set_running(true);
  (*app)->set_progress(0.0f);
  (*app)->set_status_text(to_shared("正在启动队列 worker…"));

  auto manifest_path = create_studio_queue_manifest(run_id, *files);
  if (!manifest_path) {
    reset_failed_run(**app, *state, run_id,
                     std::format("队列 worker 准备失败：{}",
                                 manifest_path.error()));
    return;
  }
  cfg.studio_queue_manifest = *manifest_path;

  auto child = start_studio_cli_worker(cfg, run_id);
  if (!child) {
    std::error_code ec;
    std::filesystem::remove(*manifest_path, ec);
    reset_failed_run(**app, *state, run_id,
                     std::format("启动队列 worker 失败：{}", child.error()));
    return;
  }
  (*child)->temp_directories = std::move(temp_directories);
  {
    std::scoped_lock lock{state->mutex};
    state->active_child = *child;
  }
  (*app)->set_status_text(to_shared(std::format(
      "队列 worker 已启动，PID {}，共 {} 张图片。", (*child)->process_id,
      files->size())));

  std::optional<std::jthread> monitor;
  try {
    monitor.emplace(guarded_worker(
        weak, state, run_id, "队列 worker 监控",
        [weak, state, run_id, child = *child,
                     file_count = files->size()](std::stop_token token) mutable {
      std::string pending;
      std::optional<StudioWorkerItemEvent> pending_item;

      auto publish_item = [&](StudioWorkerItemEvent event) {
        post_to_ui(weak, [state, run_id, event](AwjStudio& app) {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          if (auto queue_index =
                  queue_index_for_run_index(*state, event.index)) {
            auto& item = state->queue_items[*queue_index];
            item.warning = item.warning || event.status == 'F';
            switch (event.status) {
              case 'R':
                item.status = QueueItemStatus::running;
                item.status_text = "正在转码";
                break;
              case 'D':
                item.status = QueueItemStatus::done;
                item.status_text = "完成";
                break;
              case 'S':
                item.status = QueueItemStatus::skipped;
                item.status_text = "已跳过";
                break;
              case 'C':
                item.status = QueueItemStatus::canceled;
                item.status_text = "已取消";
                break;
              case 'F':
              default:
                item.status = QueueItemStatus::failed;
                item.status_text = "失败";
                break;
            }
          }
          refresh_queue_rows(app, *state);
          app.set_progress(static_cast<float>(event.completed) /
                           static_cast<float>(event.total));
          app.set_status_text(to_shared(
              event.status == 'R'
                  ? std::format("正在转码第 {} 项；已完成 {}/{}。",
                                event.index + 1, event.completed, event.total)
                  : std::format("已完成 {}/{}。", event.completed,
                                event.total)));
        });
      };

      auto publish_line = [&](std::string line) {
        line = trim_copy(std::move(line));
        if (line.empty()) {
          return;
        }
        if (auto event = parse_studio_worker_item_event(line)) {
          if (event->status != 'R') {
            pending_item = *event;
          }
          publish_item(*event);
          return;
        }
        if (auto detail = parse_studio_worker_detail_event(line)) {
          post_to_ui(weak, [state, run_id, detail = std::move(*detail)](
                               AwjStudio& app) {
            std::scoped_lock lock{state->mutex};
            if (state->run_id != run_id) {
              return;
            }
            if (auto queue_index =
                    queue_index_for_run_index(*state, detail.index)) {
              auto& item = state->queue_items[*queue_index];
              const auto seconds = [](std::int64_t value) {
                return value < 0 ? -1.0
                                 : static_cast<double>(value) /
                                       1'000'000.0;
              };
              item.encoder_id = std::move(detail.encoder_id);
              item.encoder_threads = detail.encoder_threads;
              item.decode_seconds = seconds(detail.decode_microseconds);
              item.prepare_seconds = seconds(detail.prepare_microseconds);
              item.encode_seconds = seconds(detail.encode_microseconds);
              item.write_seconds = seconds(detail.write_microseconds);
            }
            refresh_queue_rows(app, *state);
          });
          return;
        }
        if (pending_item) {
          const auto event = *pending_item;
          pending_item.reset();
          post_to_ui(weak, [state, run_id, event,
                             line = std::move(line)](AwjStudio& app) {
            std::scoped_lock lock{state->mutex};
            if (state->run_id != run_id) {
              return;
            }
            if (auto queue_index =
                    queue_index_for_run_index(*state, event.index)) {
              auto& item = state->queue_items[*queue_index];
              item.log_text = line;
              // 跨进程约定：这里嗅探的是 AWJ CLI 子进程 stdout 里的中文子串，
              // 生产方在 pipeline.ixx:461（", 未达标兜底"）。子进程的输出与日志
              // 固定为中文、不跟随界面语言，本判断才成立——1.0.0 的双语只覆盖
              // 界面标签。如果以后要翻译 worker 输出，必须先把这里换成不随语言
              // 变化的机器可读信号（稳定 ASCII 标记，或把
              // visual_quality_target_met 走 awj::BatchProgress 结构化通道传上来），
              // 否则视觉质量未达标的行会静默不再标警告，且不会有编译错误。
              item.warning = item.warning ||
                             line.find("未达标") != std::string::npos;
            }
            refresh_queue_rows(app, *state);
          });
          return;
        }
        post_to_ui(weak, [state, run_id,
                          line = std::move(line)](AwjStudio& app) {
          std::scoped_lock lock{state->mutex};
          if (state->run_id == run_id) {
            app.set_status_text(to_shared(line));
          }
        });
      };

      auto consume_output = [&] {
        if (child->output_read == nullptr) {
          return;
        }
        while (true) {
          DWORD available = 0;
          if (!PeekNamedPipe(child->output_read.get(), nullptr, 0, nullptr,
                             &available, nullptr) ||
              available == 0) {
            break;
          }
          std::array<char, 4096> buffer{};
          DWORD read_bytes = 0;
          if (!ReadFile(child->output_read.get(), buffer.data(),
                        static_cast<DWORD>(
                            std::min<std::size_t>(buffer.size(), available)),
                        &read_bytes, nullptr) ||
              read_bytes == 0) {
            break;
          }
          pending.append(buffer.data(), buffer.data() + read_bytes);
          std::size_t pos = 0;
          while ((pos = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
            pending.erase(0, pos + 1);
            publish_line(std::move(line));
          }
        }
      };

      DWORD exit_code = 1;
      while (!token.stop_requested()) {
        consume_output();
        const DWORD wait = WaitForSingleObject(child->process.get(), 80);
        if (wait == WAIT_OBJECT_0) {
          break;
        }
        if (wait != WAIT_TIMEOUT) {
          break;
        }
      }
      if (token.stop_requested() && child->process != nullptr) {
        child->terminate();
      }
      WaitForSingleObject(child->process.get(), INFINITE);
      consume_output();
      if (!pending.empty()) {
        publish_line(std::move(pending));
      }
      GetExitCodeProcess(child->process.get(), &exit_code);
      const bool forced = child->was_force_terminated();
      const bool canceled =
          child->cancel_requested.load(std::memory_order_acquire) &&
          !forced &&
          exit_code == awj::studio_defaults::worker_force_stop_exit_code;
      cleanup_studio_queue_manifest(child);
      if (forced) {
        cleanup_forced_worker_temp_files(child);
      }

      post_to_ui(weak, [state, run_id, exit_code, forced, canceled,
                        file_count](AwjStudio& app) {
        std::size_t ok_count = 0;
        std::size_t failed_count = 0;
        std::size_t canceled_count = 0;
        {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          state->pending_events.clear();
          state->worker_active = false;
          state->active_child.reset();
          for (auto& item : state->queue_items) {
            if (item.run_index == std::numeric_limits<std::size_t>::max()) {
              continue;
            }
            if (item.status == QueueItemStatus::pending ||
                item.status == QueueItemStatus::running) {
              if (forced || canceled) {
                item.status = QueueItemStatus::canceled;
                item.status_text =
                    forced ? "已强制终止" : "已取消";
                item.warning = false;
              } else {
                item.status = QueueItemStatus::failed;
                item.status_text = "未处理";
                item.warning = true;
              }
            }
            if (item.status == QueueItemStatus::done ||
                item.status == QueueItemStatus::skipped) {
              ++ok_count;
            } else if (item.status == QueueItemStatus::canceled) {
              ++canceled_count;
            } else if (item.status == QueueItemStatus::failed) {
              ++failed_count;
            }
          }
          refresh_queue_rows(app, *state);
        }
        app.set_running(false);
        app.set_progress(exit_code == 0 ? 1.0f : 0.0f);
        if (forced) {
          app.set_status_text(
              to_shared("编码已强制终止，Studio 仍可继续使用"));
        } else if (canceled) {
          app.set_status_text(to_shared(std::format(
              "已取消：成功 {}，失败 {}，取消 {}。", ok_count,
              failed_count, canceled_count)));
        } else if (exit_code == 0) {
          app.set_status_text(to_shared(std::format(
              "完成：成功 {}，失败 {}，取消 {}，共 {}。", ok_count,
              failed_count, canceled_count, file_count)));
        } else {
          app.set_status_text(to_shared(std::format(
              "队列 worker 失败，退出码 {}：成功 {}，失败 {}。",
              exit_code, ok_count, failed_count)));
        }
      });
    }));
  } catch (...) {
    (*child)->terminate();
    WaitForSingleObject((*child)->process.get(), INFINITE);
    cleanup_studio_queue_manifest(*child);
    cleanup_forced_worker_temp_files(*child);
    {
      std::scoped_lock lock{state->mutex};
      if (state->run_id == run_id) {
        state->active_child.reset();
      }
    }
    reset_failed_run(**app, *state, run_id, "队列 worker 监控启动失败。");
    return;
  }

  {
    std::scoped_lock lock{state->mutex};
    state->worker = std::move(*monitor);
  }
}
void handle_queue_menu_action(AwjStudio& app,
                              const std::shared_ptr<UiState>& state,
                              int index, std::string action) {
  std::optional<std::filesystem::path> path_to_open;
  std::optional<std::filesystem::path> image_to_open;
  std::optional<std::wstring> text_to_copy;
  std::string status;
  {
    std::scoped_lock lock{state->mutex};
    if (index < 0 ||
        static_cast<std::size_t>(index) >= state->queue_items.size()) {
      app.set_status_text(to_shared("队列项不存在。"));
      return;
    }
    auto& item = state->queue_items[static_cast<std::size_t>(index)];
    if (action == "open-folder") {
      path_to_open = item.path.parent_path();
    } else if (action == "open-image") {
      image_to_open = item.path;
    } else if (action == "copy-path") {
      text_to_copy = item.path.native();
    } else if (action == "remove") {
      if (state->worker_active) {
        app.set_status_text(to_shared("运行中不能移除队列项。"));
        return;
      }
      state->queue_path_keys.erase(queue_path_key(state->queue_items[index].path));
      state->queue_items.erase(state->queue_items.begin() + index);
      app.set_selected_queue_index(-1);
      status = "已从队列移除。";
    } else if (state->worker_active) {
      app.set_status_text(to_shared("运行中不能调整队列顺序。"));
      return;
    } else if (!queue_item_editable(item)) {
      app.set_status_text(to_shared("该图片已开始编码，不能重新排序。"));
      return;
    } else if (action == "priority") {
      const auto target = first_pending_index(*state);
      if (target < state->queue_items.size()) {
        move_queue_item(*state, static_cast<std::size_t>(index), target);
      }
      status = "已移到未编码队列最前。";
    } else if (action == "last") {
      const auto target = last_pending_index(*state);
      if (target < state->queue_items.size()) {
        move_queue_item(*state, static_cast<std::size_t>(index), target);
      }
      status = "已移到未编码队列最后。";
    } else if (action == "move-up") {
      if (index > 0) {
        move_queue_item(*state, static_cast<std::size_t>(index),
                        static_cast<std::size_t>(index - 1));
      }
      status = "已上移。";
    } else if (action == "move-down") {
      move_queue_item(*state, static_cast<std::size_t>(index),
                      static_cast<std::size_t>(index + 1));
      status = "已下移。";
    }
    refresh_queue_rows(app, *state);
  }

  if (path_to_open) {
    if (auto opened = open_path(*path_to_open, false); !opened) {
      app.set_status_text(
          to_shared(std::format("打开所在位置失败：{}", opened.error())));
    }
    return;
  }
  if (image_to_open) {
    if (auto opened = open_file_with_default_app(*image_to_open); !opened) {
      app.set_status_text(
          to_shared(std::format("打开图片失败：{}", opened.error())));
    }
    return;
  }
  if (text_to_copy) {
    if (auto copied = copy_text_to_clipboard(*text_to_copy); !copied) {
      app.set_status_text(
          to_shared(std::format("复制路径失败：{}", copied.error())));
    } else {
      app.set_status_text(to_shared("已复制完整路径。"));
    }
    return;
  }
  if (!status.empty()) {
    app.set_status_text(to_shared(status));
  }
}


struct QueueDragPayload {
  std::uint64_t id{};
};

slint::DataTransfer make_queue_drag_data(
    const std::shared_ptr<UiState>& state, int index) {
  slint::DataTransfer transfer;
  std::scoped_lock lock{state->mutex};
  if (index < 0 ||
      static_cast<std::size_t>(index) >= state->queue_items.size()) {
    return transfer;
  }
  const auto& item = state->queue_items[static_cast<std::size_t>(index)];
  if (!state->worker_active && queue_item_editable(item)) {
    transfer.set_user_data(QueueDragPayload{.id = item.id});
  }
  return transfer;
}

std::optional<std::size_t> queue_drop_target_index(
    const UiState& state, std::size_t current, int target_slot) noexcept {
  if (target_slot < 0) {
    return std::nullopt;
  }
  auto target = static_cast<std::size_t>(target_slot);
  if (target > state.queue_items.size()) {
    return std::nullopt;
  }
  if (target > current) {
    --target;
  }
  if (target >= state.queue_items.size() || target == current ||
      !queue_item_editable(state.queue_items[target])) {
    return std::nullopt;
  }
  return target;
}

slint::language::DragAction handle_queue_drag_can_drop(
    const std::shared_ptr<UiState>& state, slint::language::DropEvent event,
    int target_slot) {
  const auto user_data = event.data.user_data();
  const auto* payload = std::any_cast<QueueDragPayload>(&user_data);
  // Windows Explorer 外部文件由原生 OLE IDropTarget 处理；这里仅处理 AWJ 队列内部
  // DataTransfer user_data，避免再次依赖 Slint 的 plain_text 路径序列化。
  if (payload == nullptr) return slint::language::DragAction::None;
  std::scoped_lock lock{state->mutex};
  if (state->worker_active) {
    return slint::language::DragAction::None;
  }
  const auto current = queue_index_for_id(*state, payload->id);
  if (!current || !queue_item_editable(state->queue_items[*current]) ||
      !queue_drop_target_index(*state, *current, target_slot)) {
    return slint::language::DragAction::None;
  }
  return slint::language::DragAction::Move;
}

slint::language::DragAction handle_queue_drag_dropped(
    AwjStudio& app, const std::shared_ptr<UiState>& state,
    slint::language::DropEvent event, int target_slot) {
  bool refresh = false;
  {
    const auto user_data = event.data.user_data();
    const auto* payload = std::any_cast<QueueDragPayload>(&user_data);
    if (payload == nullptr) return slint::language::DragAction::None;
    std::scoped_lock lock{state->mutex};
    if (state->worker_active) {
      return slint::language::DragAction::None;
    }
    const auto current = queue_index_for_id(*state, payload->id);
    if (!current || !queue_item_editable(state->queue_items[*current])) {
      return slint::language::DragAction::None;
    }
    const auto target = queue_drop_target_index(*state, *current, target_slot);
    if (!target) {
      return slint::language::DragAction::None;
    }
    refresh = move_queue_item(*state, *current, *target);
    state->drag_reordered = true;
    if (refresh) {
      refresh_queue_rows(app, *state);
    }
  }
  if (refresh) {
    app.set_status_text(to_shared("已调整未编码队列顺序。"));
  }
  return slint::language::DragAction::Move;
}

void handle_queue_pointer_event(AwjStudio& app,
                                const std::shared_ptr<UiState>& state,
                                int index, int button, int kind,
                                float /*local_y*/) {
  if (button != 0) {
    return;
  }
  std::optional<std::filesystem::path> double_click_folder;
  bool select_row = false;
  {
    std::scoped_lock lock{state->mutex};
    if (index < 0 ||
        static_cast<std::size_t>(index) >= state->queue_items.size()) {
      return;
    }
    auto& item = state->queue_items[static_cast<std::size_t>(index)];
    const auto now = std::chrono::steady_clock::now();
    if (kind == 0) {
      state->drag_reordered = false;
      return;
    }
    if (kind == 1) {
      const bool was_drag = state->drag_reordered;
      state->drag_reordered = false;
      if (!was_drag) {
        // 详情面板只在未发生拖动的抬起时打开，避免拖动排序后的释放误开详情。
        select_row = true;
        if (state->last_click_id == item.id &&
            now - state->last_click_time <=
                awj::studio_defaults::queue_double_click_delay) {
          double_click_folder = item.path.parent_path();
          state->last_click_id = 0;
        } else {
          state->last_click_id = item.id;
          state->last_click_time = now;
        }
      }
    }
  }
  if (select_row) {
    app.set_selected_queue_index(index);
  }
  if (double_click_folder) {
    if (auto opened = open_path(*double_click_folder, false); !opened) {
      app.set_status_text(
          to_shared(std::format("打开所在位置失败：{}", opened.error())));
    }
  }
}

void begin_child_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                const std::shared_ptr<UiState>& state,
                                awj::AppConfig cfg,
                                std::optional<int> large_index) {
  auto app = weak.lock();
  if (!app) {
    return;
  }

  std::uint64_t run_id{};
  std::shared_ptr<slint::VectorModel<TaskRow>> rows;
  std::shared_ptr<slint::VectorModel<LargeImageRow>> large_rows;
  {
    std::scoped_lock lock{state->mutex};
    if (state->worker_active) {
      (*app)->set_status_text(to_shared("当前任务正在运行，请先停止任务或强制终止"));
      return;
    }
    run_id = ++state->run_id;
    state->pending_events.clear();
    state->worker_active = true;
    state->active_child.reset();
    if (!large_index) {
      state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
      state->large_image_rows = std::make_shared<slint::VectorModel<LargeImageRow>>();
      state->large_image_items.clear();
      rows = state->task_rows;
      large_rows = state->large_image_rows;
    } else {
      set_large_image_status(*state, *large_index, "正在编码…");
      rows = state->task_rows;
      large_rows = state->large_image_rows;
    }
  }

  try {
    (*app)->set_running(true);
    (*app)->set_progress(0.0f);
    if (!large_index) {
      (*app)->set_task_rows(rows);
      (*app)->set_large_image_rows(large_rows);
      (*app)->set_selected_large_image_index(-1);
    }
    (*app)->set_status_text(to_shared("正在启动编码 worker…"));
  } catch (...) {
    reset_failed_run(**app, *state, run_id, "转换启动失败。");
    return;
  }

  auto child = start_studio_cli_worker(cfg, run_id);
  if (!child) {
    reset_failed_run(**app, *state, run_id,
                     std::format("启动编码 worker 失败：{}", child.error()));
    if (large_index) {
      set_large_image_status(*state, *large_index, "启动失败");
    }
    return;
  }
  try {
    (*app)->set_status_text(
        to_shared(std::format("编码 worker 已启动，PID {}", (*child)->process_id)));
  } catch (...) {
  }

  std::optional<std::jthread> worker;
  try {
    {
      std::scoped_lock lock{state->mutex};
      state->active_child = *child;
    }
    worker.emplace(guarded_worker(
        weak, state, run_id, "编码 worker 监控",
        [weak, state, run_id, child = *child,
                    large_index](std::stop_token token) mutable {
      std::string pending;
      auto publish_line = [&](std::string line) {
        line = trim_copy(std::move(line));
        if (line.empty()) {
          return;
        }
        post_to_ui(weak, [state, run_id, line = std::move(line)](AwjStudio&) {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          append_log_row(state->task_rows, line);
        });
      };
      auto consume_output = [&] {
        if (child->output_read == nullptr) {
          return;
        }
        while (true) {
          DWORD available = 0;
          if (!PeekNamedPipe(child->output_read.get(), nullptr, 0, nullptr,
                             &available, nullptr) ||
              available == 0) {
            break;
          }
          std::array<char, 4096> buffer{};
          DWORD read = 0;
          if (!ReadFile(child->output_read.get(), buffer.data(),
                        static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available)),
                        &read, nullptr) ||
              read == 0) {
            break;
          }
          pending.append(buffer.data(), buffer.data() + read);
          std::size_t pos = 0;
          while ((pos = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
            pending.erase(0, pos + 1);
            publish_line(std::move(line));
          }
        }
      };

      DWORD exit_code = 1;
      while (!token.stop_requested()) {
        consume_output();
        const DWORD wait = WaitForSingleObject(child->process.get(), 80);
        if (wait == WAIT_OBJECT_0) {
          break;
        }
        if (wait != WAIT_TIMEOUT) {
          break;
        }
      }
      if (token.stop_requested() && child->process != nullptr) {
        child->terminate();
      }
      WaitForSingleObject(child->process.get(), INFINITE);
      consume_output();
      if (!pending.empty()) {
        publish_line(std::move(pending));
      }
      GetExitCodeProcess(child->process.get(), &exit_code);
      const bool forced = child->was_force_terminated();
      const bool canceled =
          child->cancel_requested.load(std::memory_order_acquire) && !forced &&
          exit_code == awj::studio_defaults::worker_force_stop_exit_code;
      if (forced) {
        cleanup_forced_worker_temp_files(child);
      }
      post_to_ui(weak, [state, run_id, large_index, exit_code, forced,
                        canceled](AwjStudio& app) {
              {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          state->update_timer.stop();
          state->pending_events.clear();
          state->worker_active = false;
          state->active_child.reset();
                if (large_index) {
            set_large_image_status(
                *state, *large_index,
                exit_code == 0 ? "完成" : (forced ? "已强制终止"
                                                   : (canceled ? "已取消" : "失败")));
          }
              }
              try {
          app.set_running(false);
          app.set_progress(exit_code == 0 ? 1.0f : 0.0f);
          if (exit_code == 0) {
            app.set_status_text(to_shared(large_index ? "大图处理完成" : "完成"));
          } else if (forced) {
            app.set_status_text(to_shared("编码已强制终止，Studio 仍可继续使用"));
          } else if (canceled) {
            app.set_status_text(to_shared("已取消"));
          } else {
            app.set_status_text(
                to_shared(std::format("编码 worker 失败，退出码 {}", exit_code)));
          }
        } catch (...) {
        }
      });
    }));
  } catch (const std::exception&) {
    (*child)->terminate();
    WaitForSingleObject((*child)->process.get(), INFINITE);
    cleanup_forced_worker_temp_files(*child);
    {
      std::scoped_lock lock{state->mutex};
      if (state->run_id == run_id) {
        state->active_child.reset();
      }
    }
    reset_failed_run(**app, *state, run_id, "转换启动失败。");
    return;
  } catch (...) {
    (*child)->terminate();
    WaitForSingleObject((*child)->process.get(), INFINITE);
    cleanup_forced_worker_temp_files(*child);
    {
      std::scoped_lock lock{state->mutex};
      if (state->run_id == run_id) {
        state->active_child.reset();
      }
    }
    reset_failed_run(**app, *state, run_id, "转换启动失败。");
    return;
  }
  {
    std::scoped_lock lock{state->mutex};
    state->worker = std::move(*worker);
  }
}

std::string result_status_text(const awj::EncodeResult& result) {
  if (result.ok) {
    if (result.skipped) {
      return "已跳过";
    }
    const double ratio = result.original_bytes == 0
                             ? 0.0
                             : static_cast<double>(result.output_bytes) /
                                   static_cast<double>(result.original_bytes) *
                                   100.0;
    if (result.requested_visual_quality) {
      if (result.lossless) {
        return std::format("完成 · {} · {:.1f}% · VQ lossless · q{} · {:.2f}s",
                           awj::format_size(result.output_bytes), ratio,
                           result.final_encoder_quality, result.seconds);
      }
      const char* target_state =
          result.visual_quality_target_met ? "" : " 未达标兜底";
      return std::format("完成 · {} · {:.1f}% · VQ {:.2f}{} · q{} · {:.2f}s",
                         awj::format_size(result.output_bytes), ratio,
                         result.visual_score, target_state,
                         result.final_encoder_quality, result.seconds);
    }
    return std::format("完成 · {} · {:.1f}% · {:.2f}s",
                       awj::format_size(result.output_bytes), ratio,
                       result.seconds);
  }
  if (result.canceled) {
    return "已取消";
  }
  return result.message.empty() ? "失败"
                                : std::format("失败 · {}", result.message);
}

std::string result_log_text(const awj::EncodeResult& result) {
  if (!result.requested_visual_quality || !result.ok || result.skipped) {
    return {};
  }
  if (result.lossless) {
    return std::format("requested={} · lossless=true · final q{} · {}",
                       *result.requested_visual_quality,
                       result.final_encoder_quality,
                       awj::format_size(result.output_bytes));
  }
  const char* target_state =
      result.visual_quality_target_met ? "" : " · 未达标兜底";
  return std::format(
      "score {:.2f}{} · GMSD {:.6f} · MS-SSIM {:.6f} · Qg {:.2f} · Qm {:.2f} · "
      "q{} · {} 次 · {}",
      result.visual_score, target_state, result.raw_gmsd, result.raw_ms_ssim,
      result.gmsd_quality_score, result.msssim_quality_score,
      result.final_encoder_quality, result.search_attempt_count,
      awj::format_size(result.output_bytes));
}

bool push_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                   TaskRow row) noexcept {
  if (rows == nullptr) {
    return false;
  }
  try {
    rows->push_back(std::move(row));
    while (rows->row_count() > awj::studio_defaults::max_task_rows) {
      rows->erase(0);
    }
  } catch (...) {
    return false;
  }
  return true;
}

TaskRow task_row_from_result(const awj::EncodeResult& result) {
  std::string output_format = result.output_format;
  if (output_format.empty()) {
    auto inferred = awj::OutputFormat::avif;
    auto ext = result.output_path.extension().wstring();
    std::ranges::transform(ext, ext.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    if (ext == L".png") {
      inferred = awj::OutputFormat::png;
    } else if (ext == L".webp") {
      inferred = awj::OutputFormat::webp;
    } else if (ext == L".jxl") {
      inferred = awj::OutputFormat::jxl;
    } else if (result.encoder_id == "jpegli") {
      inferred = awj::OutputFormat::jpgli;
    }
    output_format = awj::output_format_name(inferred);
  }
  const auto log_text = result.ok ? result_log_text(result) : result.message;
  return TaskRow{.order = to_shared(std::format("{}", result.index + 1)),
                 .filename = to_shared(awj::path_to_utf8(result.input_path.filename())),
                 .folder = to_shared(awj::path_to_utf8(result.input_path.parent_path())),
                 .size = to_shared(awj::format_size(result.original_bytes)),
                 .status = to_shared(result_status_text(result)),
                 .output = to_shared(awj::path_to_utf8(result.output_path.filename())),
                 .log = to_shared(log_text),
                 .warning = result.ok && result.requested_visual_quality.has_value() &&
                             !result.visual_quality_target_met,
                 .locked = result.processed,
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

TaskRow pending_shell_task_row(const awj::AppConfig& cfg, const awj::ImageFile& image) {
  return TaskRow{.order = to_shared(std::format("{}", image.index + 1)),
                 .filename = to_shared(awj::path_to_utf8(image.path.filename())),
                 .folder = to_shared(awj::path_to_utf8(image.path.parent_path())),
                 .size = to_shared(awj::format_size(image.bytes)),
                 .status = to_shared("等待转码"),
                 .output = to_shared(awj::path_to_utf8(awj::output_path_for(cfg, image).filename())),
                 .log = {},
                 .warning = false,
                 .locked = true,
                 .state = 0,
                 .input_path = to_shared(awj::path_to_utf8(image.path)),
                 .output_path = to_shared(awj::path_to_utf8(awj::output_path_for(cfg, image)))};
}

void mark_task_row_running(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::EncodeResult& result) noexcept {
  if (rows == nullptr) {
    return;
  }
  try {
    if (result.index < rows->row_count()) {
      auto row = rows->row_data(result.index);
      if (row) {
        row->status = to_shared("正在转码");
        row->locked = true;
        row->state = 1;
        rows->set_row_data(result.index, *row);
        return;
      }
    }
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
  } catch (...) {
  }
}

void add_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                  const awj::EncodeResult& result) noexcept {
  try {
    push_task_row(rows, task_row_from_result(result));
  } catch (...) {
  }
}

bool large_image_grid_available(const awj::BatchLargeImageItem& item) noexcept {
  if (!item.decision.available_grid) {
    return false;
  }
  // 可用性判断必须和真正执行的 CLI 一致：手动 grid 会带上
  // --experimental-clamped-grid-padding（Studio 不提供关闭入口，始终是默认值），
  // 而 pipeline 的 try_grid 接受 clamped 计划，编码器也按右列/底行的真实剩余
  // 尺寸切 tile。这里再拒绝 uses_padding 只会把 CLI 能做的事灰掉。
  const auto plan = awj::plan_grid(awj::GridPlanRequest{
      .width = item.dimensions.width,
      .height = item.dimensions.height,
      .mode = awj::GridMode::auto_grid,
      .clamped_padding_enabled =
          awj::encoding_defaults::default_experimental_clamped_grid_padding});
  return plan.has_value();
}



bool large_image_action_available(const awj::BatchLargeImageItem& item,
                                  std::string_view action) noexcept {
  if (action == "grid") {
    return large_image_grid_available(item);
  }

  return false;
}



std::string large_image_actions_summary(const awj::BatchLargeImageItem& item) {
  const auto grid = large_image_grid_available(item)
                        ? std::string{"grid 可用"}
                        : std::string{"grid 不可用"};
  return grid;
}

void add_large_image_task_row(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::BatchLargeImageItem& item) noexcept {
  try {
    push_task_row(
        rows,
        TaskRow{
            .order = to_shared(std::format("{}", item.file.index + 1)),
            .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
            .folder =
                to_shared(awj::path_to_utf8(item.file.path.parent_path())),
            .size = to_shared(awj::format_size(item.file.bytes)),
            .status = to_shared("大图模式"),
            .output = {},
            .log = to_shared(std::format(
                "原因：{}；{}；可用处理方式：{}。",
                awj::large_image_reason_name(item.decision.reason),
                item.decision.reason_text, large_image_actions_summary(item))),
            .warning = false,
            .locked = true});
  } catch (...) {
  }
}

LargeImageRow make_large_image_row(const awj::BatchLargeImageItem& item,
                                   std::string_view status) {
  const auto actions = large_image_actions_summary(item);

  return LargeImageRow{
      .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
      .dimensions = to_shared(std::format("{} x {}", item.dimensions.width,
                                          item.dimensions.height)),
      .reason = to_shared(std::format(
          "{} · {}", awj::large_image_reason_name(item.decision.reason),
          item.decision.reason_text)),
      .actions = to_shared(actions),
      .status = to_shared(status),
      .grid_available = large_image_grid_available(item)};
}

bool push_large_image_row(UiState& state,
                          awj::BatchLargeImageItem item) noexcept {
  if (state.large_image_rows == nullptr) {
    return false;
  }
  try {
    const bool was_empty = state.large_image_items.empty();
    auto row = make_large_image_row(item, "等待选择");
    bool item_added = false;
    try {
      state.large_image_items.push_back(item);
      item_added = true;
      state.large_image_rows->push_back(std::move(row));
    } catch (...) {
      if (item_added) {
        state.large_image_items.pop_back();
      }
      return false;
    }
    return was_empty;
  } catch (...) {
    return false;
  }
}

void select_first_large_image_from_state(AwjStudio& app,
                                         const UiState& state) noexcept {
  try {
    if (state.large_image_items.empty()) {
      return;
    }
    app.set_selected_large_image_index(0);
    app.set_selected_page(0);
  } catch (...) {
  }
}

bool path_is_directory(const std::filesystem::path& path) noexcept {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) && !ec;
}

std::string large_image_action_status(const awj::BatchLargeImageItem& item,
                                      std::string_view action) {
  if (!large_image_action_available(item, action)) {
    return std::format("{} 不可用", action);
  }
  if (action == "grid") {
    const auto plan =
        awj::plan_grid(awj::GridPlanRequest{.width = item.dimensions.width,
                                            .height = item.dimensions.height,
                                            .mode = awj::GridMode::auto_grid});
    if (!plan) {
      return std::format("grid 规划失败：{}", plan.error());
    }
    return std::format("已选择 grid · {}x{} 分块 · tile {}x{}", plan->cols,
                       plan->rows, plan->tile_width, plan->tile_height);
  }

  return "未知处理方式";
}

void append_pending_event(UiState& state, std::uint64_t run_id,
                          const awj::BatchProgress& event) noexcept {
  try {
    std::scoped_lock lock{state.mutex};
    if (state.run_id != run_id) {
      return;
    }
    if (state.pending_events.size() >=
        awj::studio_defaults::max_pending_events) {
      const auto keep = [](const awj::BatchProgress& pending) {
        return pending.kind == awj::BatchEventKind::item_finished ||
               pending.kind == awj::BatchEventKind::warning ||
               pending.kind == awj::BatchEventKind::large_image_queued;
      };
      const auto retained = std::ranges::remove_if(
          state.pending_events,
          [&](const awj::BatchProgress& pending) { return !keep(pending); });
      state.pending_events.erase(retained.begin(), retained.end());
      if (state.pending_events.size() >=
          awj::studio_defaults::max_pending_events) {
        state.pending_events.erase(
            state.pending_events.begin(),
            state.pending_events.begin() +
                static_cast<std::ptrdiff_t>(state.pending_events.size() -
                                            awj::studio_defaults::
                                                max_pending_events +
                                            1));
      }
    }
    state.pending_events.push_back(event);
  } catch (...) {
  }
}



bool output_template_contains(std::wstring_view text,
                              std::wstring_view token) {
  return text.find(token) != std::wstring_view::npos;
}

std::expected<void, std::string> open_path(std::filesystem::path path,
                                           bool create_if_missing) try {
  if (path.empty()) {
    return std::unexpected{"路径为空。"};
  }

  std::error_code ec;
  const bool regular_file = std::filesystem::is_regular_file(path, ec);
  if (ec) {
    return std::unexpected{std::format("无法判断路径类型: {}；系统错误：{}。",
                                       awj::display_path_for_user(path),
                                       ec.message())};
  }
  if (regular_file) {
    path = path.parent_path();
  }
  if (path.empty()) {
    auto current = std::filesystem::current_path(ec);
    if (ec) {
      return std::unexpected{
          std::format("无法定位当前目录；系统错误：{}。", ec.message())};
    }
    path = std::move(current);
  }
  if (create_if_missing) {
    std::filesystem::create_directories(path, ec);
    if (ec) {
      return std::unexpected{std::format("无法创建目录: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
  }

  bool exists = std::filesystem::exists(path, ec);
  if (ec) {
    return std::unexpected{std::format("无法检查路径: {}；系统错误：{}。",
                                       awj::display_path_for_user(path),
                                       ec.message())};
  }
  if (!exists) {
    const auto parent = path.parent_path();
    if (create_if_missing || parent.empty() || parent == path) {
      return std::unexpected{
          std::format("路径不存在: {}。", awj::display_path_for_user(path))};
    }
    path = parent;
    exists = std::filesystem::exists(path, ec);
    if (ec) {
      return std::unexpected{std::format("无法检查父目录: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
    if (!exists) {
      return std::unexpected{
          std::format("父目录不存在: {}。", awj::display_path_for_user(path))};
    }
  }
  const bool directory = std::filesystem::is_directory(path, ec);
  if (ec) {
    return std::unexpected{std::format("无法判断目录类型: {}；系统错误：{}。",
                                       awj::display_path_for_user(path),
                                       ec.message())};
  }
  if (!directory) {
    return std::unexpected{
        std::format("路径不是目录: {}。", awj::display_path_for_user(path))};
  }

  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    return std::unexpected{
        std::format("无法打开目录: {}；ShellExecuteW 返回码：{}。",
                    awj::display_path_for_user(path), result)};
  }
  return {};
} catch (const std::bad_alloc&) {
  return std::unexpected{"打开路径内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"打开路径数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"打开路径文件系统访问失败。"};
}

std::expected<void, std::string> open_file_with_default_app(
    const std::filesystem::path& path) try {
  if (path.empty()) {
    return std::unexpected{"路径为空。"};
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::unexpected{std::format("图片文件不存在: {}。",
                                       awj::display_path_for_user(path))};
  }
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    return std::unexpected{
        std::format("无法打开图片: {}；ShellExecuteW 返回码：{}。",
                    awj::display_path_for_user(path), result)};
  }
  return {};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"打开图片文件系统访问失败。"};
}

std::expected<void, std::string> copy_text_to_clipboard(
    std::wstring_view text) {
  if (text.empty()) {
    return std::unexpected{"复制内容为空。"};
  }
  if (!OpenClipboard(nullptr)) {
    return std::unexpected{std::format("打开剪贴板失败: {}。",
                                       awj::win32_error_message(GetLastError()))};
  }
  struct ClipboardGuard {
    ~ClipboardGuard() { CloseClipboard(); }
  } guard;
  if (!EmptyClipboard()) {
    return std::unexpected{std::format("清空剪贴板失败: {}。",
                                       awj::win32_error_message(GetLastError()))};
  }
  const auto bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    return std::unexpected{"分配剪贴板内存失败。"};
  }
  void* locked = GlobalLock(memory);
  if (locked == nullptr) {
    GlobalFree(memory);
    return std::unexpected{"锁定剪贴板内存失败。"};
  }
  std::memcpy(locked, text.data(), text.size() * sizeof(wchar_t));
  static_cast<wchar_t*>(locked)[text.size()] = L'\0';
  GlobalUnlock(memory);
  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    return std::unexpected{std::format("写入剪贴板失败: {}。",
                                       awj::win32_error_message(GetLastError()))};
  }
  return {};
}

}  // namespace awj::studio
