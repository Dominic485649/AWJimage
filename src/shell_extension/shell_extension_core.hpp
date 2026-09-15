#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace awj::shell_extension {

inline constexpr std::uint32_t configuration_version = 1;
inline constexpr std::wstring_view configuration_magic =
    L"AWJimage.ShellExtension.Configuration";

struct SelectionItem {
  std::filesystem::path path{};
  bool is_directory{};

  bool operator==(const SelectionItem&) const = default;
};

struct MenuCommand {
  std::wstring group_label{};
  std::wstring group_canonical_verb{};
  std::wstring label{};
  std::wstring canonical_verb{};
  std::vector<std::wstring> arguments{};

  bool operator==(const MenuCommand&) const = default;
};

struct RuntimeConfiguration {
  std::filesystem::path executable{};
  std::wstring menu_label{};
  std::vector<MenuCommand> commands{};

  bool operator==(const RuntimeConfiguration&) const = default;
};

std::span<const std::wstring_view> supported_extensions() noexcept;
bool is_supported_extension(std::wstring_view extension) noexcept;
bool is_supported_selection(std::span<const SelectionItem> selection) noexcept;

std::wstring quote_windows_argument(std::wstring_view argument,
                                    bool force_quotes = false);

std::expected<std::wstring, std::string> build_awj_command_line(
    const std::filesystem::path& executable,
    std::span<const std::wstring> arguments,
    std::span<const SelectionItem> selection);

std::expected<std::vector<std::wstring>, std::string> encode_configuration(
    const RuntimeConfiguration& configuration);
std::expected<RuntimeConfiguration, std::string> decode_configuration(
    std::span<const std::wstring> encoded);

std::vector<MenuCommand> default_menu_commands();

}  // namespace awj::shell_extension
