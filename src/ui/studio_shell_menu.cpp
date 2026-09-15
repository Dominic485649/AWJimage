#include "studio_shell_cli.h"

#include "shell_elevation.hpp"
#include "menu_config_journal.hpp"
#include "studio_config.h"
#include "studio_config_io.h"
#include "studio_menu_params.h"
#include "studio_parameter_page.h"
#include "studio_ui_util.h"
#include <fstream>

namespace awj::studio {
namespace {
using MenuResult = std::expected<std::shared_ptr<shell_context_menu::MenuTransaction>, std::string>;
struct Completion {
  std::mutex mutex;
  std::optional<MenuResult> result;
};
std::expected<std::optional<std::string>, std::string> read_previous_config() {
  const auto path = studio_config_path();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) && !ec) return std::nullopt;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > 4 * 1024 * 1024) return std::unexpected{"无法备份右键菜单配置。"};
  std::string text(static_cast<std::size_t>(size), '\0');
  std::ifstream input{path, std::ios::binary};
  if (!input.read(text.data(), static_cast<std::streamsize>(size)))
    return std::unexpected{"读取右键菜单配置备份失败。"};
  return std::optional{std::move(text)};
}
std::expected<void, std::string> restore_config(const std::optional<std::string>& content) {
  if (content) return write_file_atomically(studio_config_path(), *content);
  std::error_code ec;
  std::filesystem::remove(studio_config_path(), ec);
  if (ec) return std::unexpected{"恢复右键菜单配置失败：" + ec.message()};
  return {};
}
}

std::expected<void, std::string> recover_shell_menu_config() {
  auto exe = awj_exe_path_for_shell_menu();
  if (!exe) return std::unexpected{exe.error()};
  return shell_context_menu::recover_menu_config_journal(*exe, restore_config);
}

void request_shell_menu_change(slint::ComponentWeakHandle<AwjStudio> weak,
                               const std::shared_ptr<UiState>& state,
                               bool install, bool remove) {
  auto app = weak.lock();
  if (!app || state->menu_operation_active) return;
  if (reject_when_worker_active(**app, state, "当前任务正在运行，无法修改右键菜单。")) return;
  store_current_menu_params(**app, *state);
  const auto previous = state->last_config_snapshot.value_or(capture_studio_config(**app, state.get()));
  const auto desired = capture_studio_config(**app, state.get());
  auto validated = validate_menu_params(desired.menu_params);
  if (!validated && !remove) {
    (*app)->set_shell_menu_compatibility(previous.shell_menu_compatibility);
    (*app)->set_context_menu_status(to_shared(validated.error()));
    (*app)->set_status_text(to_shared(validated.error()));
    return;
  }
  auto completion = std::make_shared<Completion>();
  std::weak_ptr<UiState> weak_state = state;
  state->menu_operation_active = true;
  (*app)->set_menu_operation_active(true);
  (*app)->set_context_menu_status(to_shared("正在应用右键菜单设置；修改机器菜单时需要管理员权限。"));
  try {
    state->menu_timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds{50},
        [weak, weak_state, completion, previous, desired, remove] {
      auto state = weak_state.lock();
      auto app = weak.lock();
      if (!state || !app) return;
      std::optional<MenuResult> result;
      {
        std::scoped_lock lock{completion->mutex};
        if (!completion->result) return;
        result = std::move(completion->result);
      }
      state->menu_timer.stop();
      run_ui_callback(weak, "应用右键菜单失败", [&] {
        std::string error;
        bool config_written = false;
        bool journal_written = false;
        auto before_file = read_previous_config();
        auto current = capture_studio_config(**app, state.get());
        current.menu_params = desired.menu_params;
        current.shell_menu_compatibility = desired.shell_menu_compatibility;
        if (!*result) error = result->error();
        else if (!before_file) error = before_file.error();
        else {
          if (**result) {
            auto exe = awj_exe_path_for_shell_menu();
            auto journal = exe ? shell_context_menu::begin_menu_config_journal(
                (**result)->handle(), *exe, *before_file) : std::expected<void, std::string>{std::unexpected{exe.error()}};
            if (!journal) error = journal.error();
            else journal_written = true;
          }
          if (error.empty()) {
            if (auto saved = write_studio_config_file(current, *state->config_defaults); !saved)
              error = saved.error();
            else {
            config_written = true;
            if (**result) {
              if (auto committed = (**result)->commit(); !committed) error = committed.error();
            }
            }
          }
        }
        if (!error.empty()) {
          if (*result) (**result).reset();
          bool restored = true;
          if (config_written) {
            if (auto recovery = restore_config(*before_file); !recovery) {
              error += " 配置恢复失败：" + recovery.error();
              restored = false;
            }
          }
          if (journal_written && restored) shell_context_menu::discard_menu_config_journal();
          (*app)->set_shell_menu_compatibility(previous.shell_menu_compatibility);
          (*app)->set_context_menu_status(to_shared(error));
          (*app)->set_status_text(to_shared(error));
        } else {
          state->last_config_snapshot = current;
          (*app)->set_context_menu_warning({});
          const auto message = remove ? "右键菜单已移除。" : "右键菜单设置已保存。";
          (*app)->set_context_menu_status(to_shared(message));
          (*app)->set_status_text(to_shared(message));
        }
      });
      state->menu_operation_active = false;
      (*app)->set_menu_operation_active(false);
    });
    state->menu_worker = std::jthread([completion, desired, install, remove] {
      MenuResult result;
      try {
        auto installed = shell_context_menu::is_installed();
        if (!installed) result = std::unexpected{installed.error()};
        else if (*installed || install || remove) {
          auto exe = awj_exe_path_for_shell_menu();
          auto names = awj::injected_user_preset_names();
          if (!exe) result = std::unexpected{exe.error()};
          else if (!names) result = std::unexpected{names.error()};
          else result = shell_context_menu::prepare_menu_change(*exe,
              shell_menu_params(desired.menu_params), *names, desired.shell_menu_compatibility, remove);
        }
      } catch (const std::exception& error) {
        result = std::unexpected{error.what()};
      } catch (...) {
        result = std::unexpected{"右键菜单操作异常，未提交修改。"};
      }
      std::scoped_lock lock{completion->mutex};
      completion->result = std::move(result);
    });
  } catch (...) {
    state->menu_timer.stop();
    state->menu_operation_active = false;
    (*app)->set_menu_operation_active(false);
    (*app)->set_shell_menu_compatibility(previous.shell_menu_compatibility);
    throw;
  }
}
}  // namespace awj::studio
