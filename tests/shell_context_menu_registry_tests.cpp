#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include "shell_context_menu.hpp"
#include "shell_elevation.hpp"
#include "menu_transaction_state.hpp"
#include "isolated_registry.hpp"
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace menu = awj::shell_context_menu;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void require(const std::expected<void, std::string>& result) {
  if (!result) throw std::runtime_error(result.error());
}
bool exists(const std::wstring& path) {
  HKEY key = nullptr;
  const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
  if (key) RegCloseKey(key);
  return status == ERROR_SUCCESS;
}
void write(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
      KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS, "create test key failed");
  const auto status = RegSetValueExW(key, name, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  check(status == ERROR_SUCCESS, "write test value failed");
}
std::wstring read(const std::wstring& path, const wchar_t* name) {
  wchar_t text[32768]{};
  DWORD bytes = sizeof(text);
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_SZ,
      nullptr, text, &bytes) == ERROR_SUCCESS, "read test value failed");
  return text;
}
std::wstring active_tree() {
  return L"Software\\Classes\\" + read(menu::directory_parent_key(), L"ExtendedSubCommandsKey");
}
void healthy(const std::filesystem::path& exe, const menu::MenuParams& params,
             std::span<const std::wstring> presets = {}) {
  auto warning = menu::warning(exe, params, presets);
  if (!warning) throw std::runtime_error(warning.error());
  if (*warning) throw std::runtime_error(**warning);
}
struct DenyWrites {
  HKEY key{};
  PSECURITY_DESCRIPTOR original{};
  PACL old_dacl{}, denied{};
  explicit DenyWrites(const std::wstring& path) {
    check(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS,
          "open fault-injection key failed");
    check(GetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr,
        &old_dacl, nullptr, &original) == ERROR_SUCCESS, "read test ACL failed");
    BYTE sid[SECURITY_MAX_SID_SIZE]{};
    DWORD bytes = sizeof(sid);
    check(CreateWellKnownSid(WinWorldSid, nullptr, sid, &bytes), "create test SID failed");
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE;
    entry.grfAccessMode = DENY_ACCESS;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    check(SetEntriesInAclW(1, &entry, old_dacl, &denied) == ERROR_SUCCESS, "build test ACL failed");
    check(SetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr,
        denied, nullptr) == ERROR_SUCCESS, "apply test write failure failed");
  }
  ~DenyWrites() {
    if (key) {
      SetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, old_dacl, nullptr);
      RegCloseKey(key);
    }
    if (denied) LocalFree(denied);
    if (original) LocalFree(original);
  }
};
}

int wmain(int argc, wchar_t** argv) try {
  check(argc == 2, "expected executable path");
  IsolatedRegistry sandbox;
  const auto exe = std::filesystem::absolute(argv[1]);
  menu::MenuParams params{};
  for (auto& value : params) value.quality_text = L"73";
  params[0].install_avif_png_command = true;
  const std::wstring rollback_id = L"{62B1DC4C-FAF4-46E9-A5E0-FE7DBD677210}";
  require(menu::stage_user_menu(rollback_id, false, exe, params, {}, false, false));
  check(!menu::reconcile(exe, params), "another operation recovered a live staged menu");
  require(menu::finish_user_menu(rollback_id, false));
  check(!*menu::is_installed(), "abandoned transaction left a menu");
  const std::wstring commit_id = L"{62B1DC4C-FAF4-46E9-A5E0-FE7DBD677211}";
  require(menu::stage_user_menu(commit_id, false, exe, params, {}, false, false));
  require(menu::record_menu_commit(commit_id, false));
  require(menu::finish_user_menu(commit_id, true));
  healthy(exe, params);
  const std::vector<std::wstring> compatibility_presets{L"测试预设"};
  require(menu::reconcile(exe, params, compatibility_presets, false, true));
  check(*menu::compatibility_installed(), "compatibility registration not detected");
  check(read(menu::directory_parent_key(), L"SubCommands").ends_with(L";AWJImage.presets"),
        "compatibility preset bridge missing");
  auto compatibility_warning = menu::warning(exe, params, compatibility_presets, true);
  check(compatibility_warning && !*compatibility_warning, "compatibility schema is unhealthy");
  require(menu::reconcile(exe, params, {}, false, true));
  require(menu::reconcile(exe, params, {}, false, false));
  healthy(exe, params);
  require(menu::remove());
  require(menu::reconcile(exe, params));
  check(!*menu::is_installed(), "parameter save unexpectedly installed a menu");
  const std::wstring legacy = L"Software\\Classes\\SystemFileAssociations\\image\\shell\\AWJImage";
  write(legacy + L"\\shell\\png\\command", nullptr,
        L"\"C:\\Old App\\AWJ.exe\" --shell-convert --format png -i \"%1\" %*");
  require(menu::install(exe, params));
  check(!exists(legacy), "recognized unmarked legacy menu survived migration");
  check(!exists(menu::image_parent_key()), "generic image menu was installed");
  for (auto extension : menu::supported_extensions())
    check(exists(menu::extension_parent_key(extension)), "supported extension is missing");
  healthy(exe, params);
  const auto initial_tree = active_tree();
  require(menu::install(exe, params));
  check(active_tree() == initial_tree, "unchanged registration was rewritten");
  auto next = params;
  next[0].quality_text = L"74";
  require(menu::reconcile(exe, next));
  check(active_tree() != initial_tree && !exists(initial_tree), "slot switch/cleanup failed");
  healthy(exe, next);
  const auto drift = [&](const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    write(key, name, value);
    auto warning = menu::warning(exe, next);
    check(!warning || warning->has_value(), "registry drift was not detected");
    require(menu::reconcile(exe, next));
    healthy(exe, next);
  };
  drift(menu::directory_parent_key(), nullptr, L"unexpected default");
  drift(menu::directory_parent_key(), L"SubCommands", L"unexpected");
  drift(menu::directory_parent_key(), L"MultiSelectModel", L"Single");
  drift(menu::directory_parent_key() + L"\\command", nullptr, L"unexpected");
  drift(active_tree(), L"UnexpectedValue", L"unexpected");
  drift(active_tree() + L"\\shell\\unknown", L"MUIVerb", L"unexpected");
  drift(active_tree() + L"\\shell\\AWJimage.Convert.10.png\\command", nullptr, L"unexpected");
  drift(menu::extension_parent_key(L".png"), L"AWJimage.SchemaVersion", L"4");
  // Fail after inactive-tree creation and partial parent switching. Preserve the
  // journal when rollback is also denied; recover from its original snapshot.
  {
    DenyWrites failure{L"Software\\Classes\\SystemFileAssociations\\.webp\\shell"};
    check(!menu::install(exe, params), "registry write failure was reported as success");
  }
  require(menu::recover());
  healthy(exe, next);
  check(!exists(L"Software\\Classes\\AWJimage.ContextMenu.v4.Transaction"), "recovered journal remains");
  check(!menu::install(exe.parent_path() / L"missing-AWJ.exe", params), "missing executable accepted");
  healthy(exe, next);
  std::vector<std::wstring> names;
  for (int count : {0, 1, 10, 11}) {
    names.clear();
    for (int i = 0; i < count; ++i) names.push_back(L"预设 空格 " + std::to_wstring(i));
    const auto result = menu::install(exe, next, names);
    if (count == 11) check(!result, "eleventh injected preset accepted");
    else { require(result); healthy(exe, next, names); }
  }
  names.pop_back();
  healthy(exe, next, names);
  names.back() = names.front();
  check(!menu::install(exe, next, names), "duplicate injected name accepted");
  require(menu::install(exe, next));
  wchar_t own_exe[32768]{};
  GetModuleFileNameW(nullptr, own_exe, 32768);
  require(menu::reconcile(own_exe, next));
  healthy(own_exe, next);
  require(menu::reconcile(exe, next));
  write(legacy + L"\\shell\\foreign\\command", nullptr, L"C:\\Other.exe --shell-convert");
  require(menu::remove());
  require(menu::remove());
  check(exists(legacy), "foreign legacy-looking root was deleted");
  check(!*menu::is_installed(), "owned registration residue remains");
  const auto occupied = menu::extension_parent_key(L".png");
  write(occupied, L"MUIVerb", L"Someone else");
  check(!menu::install(exe, params), "foreign current root was overwritten");
  check(read(occupied, L"MUIVerb") == L"Someone else", "foreign root changed");
  std::puts("Shell v4 isolation, migration, drift, switching, rollback, recovery and preset limits passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
