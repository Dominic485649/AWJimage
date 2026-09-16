#include "menu_transaction_state.hpp"
#include <sddl.h>
#include <aclapi.h>
#include <vector>

namespace awj::shell_context_menu {
namespace {
std::unexpected<std::string> error() { return std::unexpected<std::string>{"菜单事务权限或状态校验失败。"}; }
struct Key { HKEY value{}; ~Key() { if (value) RegCloseKey(value); } };
constexpr wchar_t receipt_root[] = L"SOFTWARE\\AWJimage.MenuCommit";
bool protected_tree(HKEY key, bool root, unsigned depth, unsigned& remaining) {
  if (!remaining-- || depth > 16) return false;
  PSID owner{};
  PACL dacl{};
  PSECURITY_DESCRIPTOR actual{};
  if (GetSecurityInfo(key, SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                      &owner, nullptr, &dacl, nullptr, &actual) != ERROR_SUCCESS) return false;
  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision{};
  BYTE admin[SECURITY_MAX_SID_SIZE]{}, system[SECURITY_MAX_SID_SIZE]{};
  DWORD size = sizeof(admin);
  bool safe = CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin, &size);
  size = sizeof(system);
  safe = safe && CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size) &&
      owner && (EqualSid(owner, admin) || EqualSid(owner, system)) && dacl &&
      GetSecurityDescriptorControl(actual, &control, &revision) &&
      (!root || (control & SE_DACL_PROTECTED));
  for (DWORD i = 0; safe && i < dacl->AceCount; ++i) {
    void* raw{};
    if (!GetAce(dacl, i, &raw)) { safe = false; break; }
    const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) { safe = false; break; }
    if ((ace->Mask & ~KEY_READ) && !EqualSid(&ace->SidStart, admin) &&
        !EqualSid(&ace->SidStart, system)) safe = false;
  }
  LocalFree(actual);
  if (!safe) return false;
  // Never follow registry symbolic links while validating a recovery snapshot.
  DWORD type{};
  if (RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS &&
      type == REG_LINK) return false;
  for (DWORD i = 0;; ++i) {
    wchar_t name[256]{};
    DWORD length = 256;
    const auto status = RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) return true;
    if (status != ERROR_SUCCESS) return false;
    Key child;
    if (RegOpenKeyExW(key, name, REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY,
                     &child.value) != ERROR_SUCCESS ||
        !protected_tree(child.value, false, depth + 1, remaining)) return false;
  }
}
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
  const auto sddl = public_read ? L"O:BAG:BAD:P(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;KR;;;BU)"
                                : L"O:BAG:BAD:P(A;CI;KA;;;SY)(A;CI;KA;;;BA)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) return error();
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  HKEY key{};
  auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, REG_OPTION_OPEN_LINK,
      KEY_ALL_ACCESS | KEY_WOW64_64KEY, &key);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr, 0,
        KEY_ALL_ACCESS | KEY_WOW64_64KEY, &attributes, &key, nullptr);
  LocalFree(descriptor);
  if (status != ERROR_SUCCESS) return error();
  unsigned remaining = 512;
  const bool safe = protected_tree(key, true, 0, remaining);
  if (!safe) { RegCloseKey(key); return error(); }
  return key;
}

MenuOperationLock::MenuOperationLock() {
  auto sid = process_user_sid(GetCurrentProcess());
  if (!sid) return;
  const auto name = L"Global\\AWJimage.MenuOperation." + *sid;
  handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
  if (handle_) {
    const auto result = WaitForSingleObject(handle_, 0);
    held_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
  }
}
MenuOperationLock::~MenuOperationLock() {
  if (held_) ReleaseMutex(handle_);
  if (handle_) CloseHandle(handle_);
}

std::expected<void, std::string> validate_machine_tree(const wchar_t* path) {
  Key key;
  const auto opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, REG_OPTION_OPEN_LINK,
      KEY_READ | KEY_WOW64_64KEY, &key.value);
  unsigned remaining = 512;
  if (opened != ERROR_SUCCESS || !protected_tree(key.value, false, 0, remaining)) return error();
  return {};
}

std::expected<bool, std::string> menu_commit_recorded(std::wstring_view id, bool machine,
                                                    std::wstring_view sid) {
  auto caller = sid.empty() ? process_user_sid(GetCurrentProcess()) : std::expected<std::wstring, std::string>{sid};
  if (!caller) return std::unexpected{caller.error()};
  const auto path = std::wstring{receipt_root} + L"\\" + *caller;
  if (machine) {
    Key root;
    const auto opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, receipt_root, REG_OPTION_OPEN_LINK,
        KEY_READ | KEY_WOW64_64KEY, &root.value);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return false;
    unsigned remaining = 512;
    if (opened != ERROR_SUCCESS || !protected_tree(root.value, true, 0, remaining)) return error();
  }
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
  if (machine) {
    auto secured = protected_machine_key(path.c_str(), true);
    if (!secured) return std::unexpected{secured.error()};
    key.value = *secured;
  } else if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
      nullptr, 0, KEY_ALL_ACCESS, nullptr, &key.value, nullptr) != ERROR_SUCCESS) return error();
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
