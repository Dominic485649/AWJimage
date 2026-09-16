#pragma once
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace awj::shell_context_menu {
using RestoreMenuConfig = std::function<std::expected<void, std::string>(
    const std::optional<std::string>&)>;
std::expected<void, std::string> begin_menu_config_journal(
    std::wstring_view id, bool machine, const std::filesystem::path& exe,
    const std::optional<std::string>& previous);
std::expected<void, std::string> recover_menu_config_journal(
    const std::filesystem::path& exe, const RestoreMenuConfig& restore);
std::expected<void, std::string> discard_menu_config_journal();
}
