#include "studio_shell_cli.h"
#include "shell_elevation.hpp"
#include "menu_config_journal.hpp"
#include "menu_transaction_state.hpp"
#include "studio_config.h"
#include "studio_config_io.h"
#include "studio_menu_params.h"
#include "studio_parameter_page.h"
#include "studio_ui_util.h"
#include <fstream>

namespace awj::studio {
namespace {
using MenuResult = std::expected<void, std::string>;
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
MenuResult restore_config(const std::optional<std::string>& content) {
  if (content) return write_file_atomically(studio_config_path(), *content);
  std::error_code ec;
  std::filesystem::remove(studio_config_path(), ec);
  if (ec) return std::unexpected{"恢复右键菜单配置失败：" + ec.message()};
  return {};
}

MenuResult apply_menu_change(const StudioConfigSnapshot& desired,
                             const StudioConfigSnapshot& defaults, bool install, bool remove) {
  shell_context_menu::MenuOperationLock operation;
  if (!operation.held()) return std::unexpected{"另一进程正在修改右键菜单。"};
  if (auto recovered = recover_shell_menu_config(); !recovered) return recovered;
  auto installed = shell_context_menu::is_installed();
  if (!installed) return std::unexpected{installed.error()};
  if (!*installed && !install && !remove) return write_studio_config_file(desired, defaults);
  auto previous = read_previous_config();
  if (!previous) return std::unexpected{previous.error()};
  auto exe = awj_exe_path_for_shell_menu();
  auto names = awj::injected_user_preset_names();
  if (!exe) return std::unexpected{exe.error()};
  if (!names) return std::unexpected{names.error()};
  auto prepared = shell_context_menu::prepare_menu_change(*exe,
      shell_menu_params(desired.menu_params), *names, desired.shell_menu_compatibility, remove);
  if (!prepared) return std::unexpected{prepared.error()};
  auto& transaction = **prepared;
  auto journal = shell_context_menu::begin_menu_config_journal(
      transaction.id(), transaction.machine(), *exe, *previous);
  if (!journal) return journal;
  auto result = write_studio_config_file(desired, defaults);
  if (result) result = transaction.commit();
  if (!result) {
    auto rollback = transaction.rollback();
    auto committed = shell_context_menu::menu_commit_recorded(transaction.id(), transaction.machine());
    if (!committed) return std::unexpected{result.error() + " " + committed.error()};
    if (!*committed) {
      auto restored = restore_config(*previous);
      if (!restored) return std::unexpected{result.error() + " " + restored.error()};
      if (!rollback) result = std::unexpected{result.error() + " " + rollback.error()};
      (void)shell_context_menu::discard_menu_config_journal();
      return result;
    }
  }
  (void)shell_context_menu::discard_menu_config_journal();
  return {};
}
}

MenuResult recover_shell_menu_config() {
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
        if (!*result) {
          (*app)->set_shell_menu_compatibility(previous.shell_menu_compatibility);
          (*app)->set_context_menu_status(to_shared(result->error()));
          (*app)->set_status_text(to_shared(result->error()));
        } else {
          state->last_config_snapshot = desired;
          (*app)->set_context_menu_warning({});
          const auto message = remove ? "右键菜单已移除。" : "右键菜单设置已保存。";
          (*app)->set_context_menu_status(to_shared(message));
          (*app)->set_status_text(to_shared(message));
        }
      });
      state->menu_operation_active = false;
      (*app)->set_menu_operation_active(false);
    });
    state->menu_worker = std::jthread([completion, desired, defaults = *state->config_defaults, install, remove] {
      MenuResult result;
      try { result = apply_menu_change(desired, defaults, install, remove); }
      catch (const std::exception& error) { result = std::unexpected{error.what()}; }
      catch (...) { result = std::unexpected{"右键菜单操作异常，未提交修改。"}; }
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
