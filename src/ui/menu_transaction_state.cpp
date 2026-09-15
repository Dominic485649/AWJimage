#include "menu_transaction_state.hpp"
#include <sddl.h>
#include <aclapi.h>
#include <vector>

namespace awj::shell_context_menu {
namespace {
std::unexpected<std::string> error() { return std::unexpected<std::string>{"菜单事务权限或状态校验失败。"}; }
struct Key { HKEY value{}; ~Key() { if (value) RegCloseKey(value); } };
constexpr wchar_t receipt_root[] = L"SOFTWARE\\AWJimage.MenuCommit";
}
std::expected<std::wstring, std::string> process_user_sid(HANDLE process) {
  HANDLE token{};
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return error();
  DWORD bytes{};
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  std::vector<BYTE> storage(bytes);
  const auto read = GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes);
  CloseHandle(token);
  if (!read) return error();
  wchar_t* text{};
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &text)) return error();
  std::wstring result{text};
  LocalFree(text);
  return result;
}

std::expected<HKEY, std::string> protected_machine_key(const wchar_t* path, bool public_read) {
  PSECURITY_DESCRIPTOR descriptor{};
  const auto sddl = public_read ? L"D:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)"
                                : L"D:P(A;;KA;;;SY)(A;;KA;;;BA)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) return error();
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  HKEY key{};
  const auto status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr, 0,
      KEY_ALL_ACCESS | KEY_WOW64_64KEY, &attributes, &key, nullptr);
  LocalFree(descriptor);
  if (status != ERROR_SUCCESS) return error();
  PACL dacl{};
  PSECURITY_DESCRIPTOR actual{};
  if (GetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                       nullptr, nullptr, &dacl, nullptr, &actual) != ERROR_SUCCESS) {
    RegCloseKey(key);
    return error();
  }
  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision{};
  bool safe = dacl && GetSecurityDescriptorControl(actual, &control, &revision) &&
              (control & SE_DACL_PROTECTED);
  BYTE admin[SECURITY_MAX_SID_SIZE]{}, system[SECURITY_MAX_SID_SIZE]{};
  DWORD size = sizeof(admin);
  safe = safe && CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin, &size);
  size = sizeof(system);
  safe = safe && CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size);
  for (DWORD i = 0; safe && i < dacl->AceCount; ++i) {
    void* raw{};
    if (!GetAce(dacl, i, &raw)) { safe = false; break; }
    const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) { safe = false; break; }
    if ((ace->Mask & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL)) &&
        !EqualSid(&ace->SidStart, admin) && !EqualSid(&ace->SidStart, system)) safe = false;
  }
  LocalFree(actual);
  if (!safe) { RegCloseKey(key); return error(); }
  return key;
}

std::expected<bool, std::string> menu_commit_recorded(std::wstring_view id, bool machine,
                                                    std::wstring_view sid) {
  auto caller = sid.empty() ? process_user_sid(GetCurrentProcess()) : std::expected<std::wstring, std::string>{sid};
  if (!caller) return std::unexpected{caller.error()};
  const auto path = std::wstring{receipt_root} + L"\\" + *caller;
  wchar_t value[40]{};
  DWORD bytes = sizeof(value);
  const auto status = RegGetValueW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
      path.c_str(), L"Id", RRF_RT_REG_SZ | (machine ? RRF_SUBKEY_WOW6464KEY : 0), nullptr, value, &bytes);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return false;
  if (status != ERROR_SUCCESS) return error();
  return id == value;
}

std::expected<void, std::string> record_menu_commit(std::wstring_view id, bool machine,
                                                   std::wstring_view sid) {
  auto caller = sid.empty() ? process_user_sid(GetCurrentProcess()) : std::expected<std::wstring, std::string>{sid};
  if (!caller) return std::unexpected{caller.error()};
  const auto path = std::wstring{receipt_root} + L"\\" + *caller;
  Key root;
  if (machine) {
    auto secured = protected_machine_key(receipt_root, true);
    if (!secured) return std::unexpected{secured.error()};
    root.value = *secured;
  }
  Key key;
  if (RegCreateKeyExW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, path.c_str(), 0,
      nullptr, 0, KEY_ALL_ACCESS | (machine ? KEY_WOW64_64KEY : 0), nullptr, &key.value, nullptr) != ERROR_SUCCESS) return error();
  const std::wstring value{id};
  if (RegSetValueExW(key.value, L"Id", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS || RegFlushKey(key.value) != ERROR_SUCCESS) return error();
  return {};
}

bool menu_owner_active(DWORD pid, FILETIME created) noexcept {
  const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
  FILETIME actual{}, exit{}, kernel{}, user{};
  const bool active = !GetProcessTimes(process, &actual, &exit, &kernel, &user) ||
      (CompareFileTime(&actual, &created) == 0 && WaitForSingleObject(process, 0) == WAIT_TIMEOUT);
  CloseHandle(process);
  return active;
}
}
