#pragma once

#include <string_view>

namespace awj::shell_extension::contract {

inline constexpr std::wstring_view class_id =
    L"{8829EA47-8F26-4670-A910-348D2340DDAA}";
// Keep the packaged Explorer command on a distinct CLSID.  The classic
// HKCU ContextMenuHandlers registration and the sparse package can otherwise
// both contribute the same command tree to Explorer's modern menu.
inline constexpr std::wstring_view modern_class_id =
    L"{63CBBCAE-762F-4224-92C6-B7395BFCD9E2}";
inline constexpr std::wstring_view class_root =
    L"Software\\Classes\\CLSID\\{8829EA47-8F26-4670-A910-348D2340DDAA}";
inline constexpr std::wstring_view file_handler_root =
    L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\AWJimage.Classic";
inline constexpr std::wstring_view folder_handler_root =
    L"Software\\Classes\\Folder\\shellex\\ContextMenuHandlers\\AWJimage.Classic";
inline constexpr std::wstring_view shell_extension_filename =
    L"AWJ.ShellExtension.dll";
inline constexpr std::wstring_view configuration_value_name = L"Configuration";
inline constexpr std::wstring_view modern_configuration_file_name =
    L"AWJimage.ShellExtension.Configuration";
// Lets Shell hosts that request opt-in-only context handlers load this class.
inline constexpr std::wstring_view context_menu_opt_in_value_name =
    L"ContextMenuOptIn";
inline constexpr std::wstring_view friendly_name =
    L"AWJimage Classic Context Menu";

}  // namespace awj::shell_extension::contract
