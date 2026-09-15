#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <expected>
#include <string>
#include <string_view>

namespace awj::shell_context_menu {
std::expected<std::wstring, std::string> process_user_sid(HANDLE process);
std::expected<bool, std::string> menu_commit_recorded(std::wstring_view id, bool machine,
                                                    std::wstring_view sid = {});
std::expected<void, std::string> record_menu_commit(std::wstring_view id, bool machine,
                                                   std::wstring_view sid = {});
std::expected<HKEY, std::string> protected_machine_key(const wchar_t* path, bool public_read);
bool menu_owner_active(DWORD pid, FILETIME created) noexcept;
}
