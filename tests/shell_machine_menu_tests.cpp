#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <objbase.h>
#include "shell_context_menu.hpp"
#include "menu_transaction_state.hpp"
#include "isolated_registry.hpp"
#include <cstdio>
#include <stdexcept>

namespace menu = awj::shell_context_menu;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void require(const std::expected<void, std::string>& value) {
  if (!value) throw std::runtime_error(value.error());
}
struct MachineOverride {
  explicit MachineOverride(HKEY key) {
    check(RegOverridePredefKey(HKEY_LOCAL_MACHINE, key) == ERROR_SUCCESS, "override HKLM failed");
  }
  ~MachineOverride() { RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr); }
};
struct MachineSandbox {
  std::wstring path = L"SOFTWARE\\AWJimage.Tests." + std::to_wstring(GetCurrentProcessId()) +
                      L"." + std::to_wstring(GetTickCount64());
  HKEY key{};
  MachineSandbox() {
    auto created = menu::protected_machine_key(path.c_str(), false);
    if (!created) throw std::runtime_error(created.error());
    key = *created;
  }
  ~MachineSandbox() {
    if (key) {
      RegDeleteTreeW(key, nullptr);
      RegCloseKey(key);
      RegDeleteKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), KEY_WOW64_64KEY, 0);
    }
  }
};
void insecure_snapshot_rejected() {
  IsolatedRegistry user;
  MachineOverride machine{user.key};
  HKEY forged{};
  check(RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AWJimage.MenuTransaction", 0,
      nullptr, 0, KEY_ALL_ACCESS, nullptr, &forged, nullptr) == ERROR_SUCCESS, "create forged snapshot failed");
  const DWORD state = 1;
  RegSetValueExW(forged, L"State", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&state), sizeof(state));
  RegCloseKey(forged);
  check(!menu::recover_machine_menu(), "ordinary-user-owned machine snapshot accepted");
}
}

int wmain(int argc, wchar_t** argv) try {
  if (argc == 1) {
    insecure_snapshot_rejected();
    std::puts("Unprivileged machine snapshot rejected in isolated registry.");
    return 0;
  }
  check(argc == 4 && std::wstring_view{argv[1]} == L"--elevated-isolated", "invalid arguments");
  FILE* output{};
  check(_wfreopen_s(&output, argv[3], L"w", stdout) == 0, "open evidence log failed");
  IsolatedRegistry user;
  MachineSandbox sandbox;
  MachineOverride machine{sandbox.key};
  const std::filesystem::path exe{argv[2]};
  const auto sid = menu::process_user_sid(GetCurrentProcess());
  check(sid.has_value(), "read SID failed");
  menu::MenuParams params{};
  for (auto& p : params) p.quality_text = L"73";
  const std::wstring first = L"{F2B1DC4C-FAF4-46E9-A5E0-FE7DBD677211}";
  const std::wstring second = L"{F2B1DC4C-FAF4-46E9-A5E0-FE7DBD677212}";
  require(menu::stage_machine_menu(exe, params, false, first, *sid));
  check(*menu::machine_menu_matches(exe, params), "staged schema mismatch");
  require(menu::recover_machine_menu());
  check(!*menu::machine_menu_matches(exe, params), "rollback left registrations");
  require(menu::stage_machine_menu(exe, params, false, first, *sid));
  require(menu::commit_machine_menu(first, *sid, exe, params, false));
  check(*menu::menu_commit_recorded(first, true, *sid), "commit receipt missing");
  // Standard CommandStore ACLs contain inherit-only generic-read and creator
  // owner entries. They do not grant ordinary users write access to the key.
  {
    const wchar_t* path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\AWJImage.png";
    HKEY key{};
    check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                       &key) == ERROR_SUCCESS, "open inherited ACL fixture failed");
    PSECURITY_DESCRIPTOR descriptor{};
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;CI;KA;;;BA)(A;CI;KA;;;SY)(A;;KR;;;BU)(A;CIIO;GR;;;BU)(A;CIIO;GA;;;CO)",
        SDDL_REVISION_1, &descriptor, nullptr), "build inherited ACL fixture failed");
    PACL dacl{}; BOOL present{}, defaulted{};
    GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
    check(SetSecurityInfo(key, SE_REGISTRY_KEY,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS, "apply inherited ACL fixture failed");
    LocalFree(descriptor);
    RegCloseKey(key);
    require(menu::validate_machine_tree(path));
  }
  require(menu::stage_machine_menu(exe, params, true, second, *sid));
  check(*menu::machine_menu_matches(exe, params), "old menu removed before replacement commit");
  require(menu::recover_machine_menu());
  check(*menu::machine_menu_matches(exe, params), "remove rollback did not restore menu");
  // Exercise machine-only residue, both modes and repeated transitions without
  // touching either real registry hive. Each transaction needs a fresh receipt.
  for (bool compatibility : {false, true, false, true}) {
    check(*menu::is_installed(true), "machine/user installation was missed");
    GUID guid{};
    check(SUCCEEDED(CoCreateGuid(&guid)), "create transaction ID failed");
    wchar_t id[40]{};
    StringFromGUID2(guid, id, 40);
    require(menu::stage_machine_menu(exe, params, !compatibility, id, *sid));
    require(menu::stage_user_menu(id, true, exe, params, {}, compatibility, false));
    require(menu::commit_machine_menu(id, *sid, exe, params, !compatibility));
    require(menu::finish_user_menu(id, true));
    check(*menu::compatibility_installed() == compatibility, "user mode did not switch");
    check(menu::legacy_machine_commands()->empty() == !compatibility, "machine registrations survived mode switch");
    if (!compatibility) {
      auto warning = menu::warning(exe, params);
      check(warning && !*warning, "normal menu was not healthy after machine cleanup");
    }
  }
  // A compromised snapshot child must fail before any fixed machine entry is changed.
  auto changed = params;
  changed[0].quality_text = L"74";
  require(menu::stage_machine_menu(exe, changed, false, second, *sid));
  HKEY child{};
  check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AWJimage.MenuTransaction\\000\\Data", 0,
      KEY_ALL_ACCESS | KEY_WOW64_64KEY, &child) == ERROR_SUCCESS, "open snapshot child failed");
  PSECURITY_DESCRIPTOR descriptor{};
  check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"O:BAG:BAD:P(A;;KA;;;BA)(A;;KA;;;SY)(A;;KW;;;BU)", SDDL_REVISION_1, &descriptor, nullptr),
      "build insecure ACL failed");
  PACL dacl{}; BOOL present{}, defaulted{};
  GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
  check(SetSecurityInfo(child, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS, "inject insecure child ACL failed");
  LocalFree(descriptor);
  RegCloseKey(child);
  check(!menu::recover_machine_menu(), "writable snapshot child accepted");
  check(!*menu::machine_menu_matches(exe, params), "rejected snapshot still restored machine menu");
  std::puts("Isolated HKLM install, commit, remove, rollback, receipt and child ACL rejection passed.");
  return 0;
} catch (const std::exception& error) {
  std::printf("FAIL: %s\n", error.what());
  return 1;
}
