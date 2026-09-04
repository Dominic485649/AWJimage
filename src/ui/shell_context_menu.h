#pragma once

#ifdef _WIN32

#include <array>
#include <string>
#include <string_view>

namespace awj::ui {

struct ShellContextMenuCommand {
  std::wstring_view key{};
  std::wstring_view label{};
  std::wstring_view format{};
  bool append_png_suffix{};
};

inline constexpr std::wstring_view shell_image_menu_key =
    L"Software\\Classes\\SystemFileAssociations\\image\\shell\\AWJImage";
inline constexpr std::wstring_view shell_icofile_menu_key =
    L"Software\\Classes\\icofile\\shell\\AWJImage";
inline constexpr std::wstring_view shell_directory_menu_key =
    L"Software\\Classes\\Directory\\shell\\AWJImage";
inline constexpr std::wstring_view shell_legacy_subcommands_key =
    L"Software\\Classes\\AWJImage.ContextMenu";
inline constexpr std::wstring_view shell_command_store_shell_key =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell";
inline constexpr std::wstring_view shell_command_store_prefix = L"AWJImage.";

inline constexpr std::array<ShellContextMenuCommand, 6>
    shell_context_menu_commands = {{
        {.key = L"png", .label = L"转换为 PNG", .format = L"png"},
        {.key = L"webp", .label = L"转换为 WebP", .format = L"webp"},
        {.key = L"avif", .label = L"转换为 AVIF", .format = L"avif"},
        {.key = L"avif-png", .label = L"转换为 AVIF.png", .format = L"avif", .append_png_suffix = true},
        {.key = L"jxl", .label = L"转换为 JXL", .format = L"jxl"},
        {.key = L"jpgli", .label = L"转换为 JPGLI", .format = L"jpgli"},
    }};

inline std::wstring shell_command_store_verb_name(
    std::wstring_view command_key) {
  return std::wstring{shell_command_store_prefix} +
         std::wstring{command_key};
}

inline std::wstring shell_command_store_verb_key(
    std::wstring_view command_key) {
  return std::wstring{shell_command_store_shell_key} + L"\\" +
         shell_command_store_verb_name(command_key);
}

inline std::wstring shell_context_menu_subcommands(
    bool install_avif_png_command) {
  std::wstring value;
  bool first = true;
  for (const auto& command : shell_context_menu_commands) {
    if (command.append_png_suffix && !install_avif_png_command) continue;
    if (!first) value.push_back(L';');
    value += shell_command_store_verb_name(command.key);
    first = false;
  }
  return value;
}

}  // namespace awj::ui

#endif  // _WIN32
