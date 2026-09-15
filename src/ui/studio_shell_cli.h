#pragma once

// CLI 参数构造、Shell 右键菜单参数与 worker 启动，以及窗口约束。
// 从 main.cpp 拆出。这块函数彼此耦合较紧，整体搬移。

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "awj_studio.h"
#include "shell_context_menu.hpp"
#include "studio_state.h"

namespace awj::studio {

std::wstring queue_path_key(const std::filesystem::path& path);

std::vector<std::wstring> cli_arguments_from_config(const awj::AppConfig& cfg);
std::wstring command_line_from_args(std::span<const std::wstring> args);

std::expected<std::filesystem::path, std::string> awj_exe_path_for_shell_menu();
std::expected<bool, std::string> collect_shell_launch_inputs(
    awj::OutputFormat format, bool append_png_suffix,
    std::vector<std::filesystem::path>& inputs);

std::expected<void, std::string> synchronize_shell_context_menu(
    const std::array<MenuFormatParams, 5>& menu_params,
    bool force_install = false);
std::expected<void, std::string> remove_shell_context_menu();
std::optional<std::string> shell_context_menu_warning(
    const std::array<MenuFormatParams, 5>& menu_params);

std::expected<std::shared_ptr<StudioChildProcess>, std::string>
start_studio_cli_worker(const awj::AppConfig& cfg, std::uint64_t run_id);

void cleanup_studio_queue_manifest(
    const std::shared_ptr<StudioChildProcess>& child) noexcept;
void cleanup_forced_worker_temp_files(
    const std::shared_ptr<StudioChildProcess>& child) noexcept;
bool reject_when_worker_active(AwjStudio& app,
                               const std::shared_ptr<UiState>& state,
                               const char* message);

void trim_process_working_set();
void constrain_window_to_work_area(slint::Window& window);

}  // namespace awj::studio
