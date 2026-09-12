#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include "shell_context_menu.hpp"
#include "isolated_registry.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
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

bool machine_exists(const std::wstring& path) {
  HKEY key = nullptr;
  const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                                    KEY_READ | KEY_WOW64_64KEY, &key);
  if (key) RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool process_is_elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD bytes = 0;
  const bool elevated = GetTokenInformation(token, TokenElevation, &elevation,
                                            sizeof(elevation), &bytes) != FALSE &&
                        elevation.TokenIsElevated != 0;
  CloseHandle(token);
  return elevated;
}

bool machine_tests_enabled() {
  wchar_t value[8]{};
  const DWORD length = GetEnvironmentVariableW(
      L"AWJ_RUN_MACHINE_REGISTRY_TESTS", value,
      static_cast<DWORD>(std::size(value)));
  return length > 0 && length < std::size(value) &&
         (value[0] == L'1' || value[0] == L'y' || value[0] == L'Y');
}

bool delete_machine_tree(const std::wstring& path) {
  const auto slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) return false;
  const auto parent_path = path.substr(0, slash);
  const auto leaf = path.substr(slash + 1);
  HKEY parent = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, parent_path.c_str(), 0,
                   KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &parent) != ERROR_SUCCESS) {
    HKEY probe = nullptr;
    const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                                      KEY_READ | KEY_WOW64_64KEY, &probe);
    if (probe) RegCloseKey(probe);
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
  }
  const auto status = RegDeleteTreeW(parent, leaf.c_str());
  RegCloseKey(parent);
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
         status == ERROR_PATH_NOT_FOUND;
}

// HKCU isolation cannot virtualize HKLM. The opt-in integration test therefore
// snapshots only AWJ-owned CommandStore roots and restores them on every exit.
struct MachineRegistryRestore {
  std::vector<std::wstring> paths = menu::owned_machine_root_keys();
  std::vector<bool> present;
  std::wstring backup = L"MachineBackup-" + std::to_wstring(GetCurrentProcessId()) +
      L"-" + std::to_wstring(GetTickCount64());
  HKEY storage{};

  MachineRegistryRestore() {
    try {
      DWORD disposition{};
      check(RegCreateKeyExW(HKEY_CURRENT_USER, backup.c_str(), 0, nullptr, 0,
                            KEY_ALL_ACCESS, nullptr, &storage, &disposition) ==
                ERROR_SUCCESS && disposition == REG_CREATED_NEW_KEY,
            "could not create machine registry snapshot");
      for (std::size_t i = 0; i < paths.size(); ++i) {
        HKEY source{}, destination{};
        const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, paths[i].c_str(), 0,
                                          KEY_READ | KEY_WOW64_64KEY, &source);
        check(status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
                  status == ERROR_PATH_NOT_FOUND,
              "cannot read machine registry root for snapshot");
        const bool was_present = status == ERROR_SUCCESS;
        present.push_back(was_present);
        const auto slot = std::to_wstring(i);
        check(RegCreateKeyExW(storage, slot.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                              nullptr, &destination, nullptr) == ERROR_SUCCESS,
              "cannot create machine registry snapshot entry");
        if (was_present) {
          check(RegCopyTreeW(source, nullptr, destination) == ERROR_SUCCESS,
                "cannot copy machine registry snapshot");
        }
        if (source) RegCloseKey(source);
        RegCloseKey(destination);
      }
      for (const auto& path : paths) {
        check(delete_machine_tree(path), "cannot clear machine registry root");
      }
      RegFlushKey(storage);
    } catch (...) {
      const bool restored = restore_machine_roots();
      close_snapshot(restored);
      throw;
    }
  }

  ~MachineRegistryRestore() {
    const bool restored = restore_machine_roots();
    close_snapshot(restored);
  }

 private:
  bool restore_machine_roots() noexcept {
    bool restored = true;
    for (std::size_t i = 0; i < present.size() && i < paths.size(); ++i) {
      restored &= delete_machine_tree(paths[i]);
      if (!present[i]) continue;
      const auto slot = std::to_wstring(i);
      HKEY source{}, destination{};
      if (RegOpenKeyExW(storage, slot.c_str(), 0, KEY_READ, &source) != ERROR_SUCCESS) {
        restored = false;
        continue;
      }
      const auto slash = paths[i].find_last_of(L'\\');
      const auto parent_path = paths[i].substr(0, slash);
      const auto leaf = paths[i].substr(slash + 1);
      HKEY parent{};
      if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, parent_path.c_str(), 0, nullptr, 0,
                          KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &parent,
                          nullptr) != ERROR_SUCCESS ||
          RegCreateKeyExW(parent, leaf.c_str(), 0, nullptr, 0,
                          KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &destination,
                          nullptr) != ERROR_SUCCESS) {
        restored = false;
      } else {
        restored &= RegCopyTreeW(source, nullptr, destination) == ERROR_SUCCESS;
      }
      if (destination) RegCloseKey(destination);
      if (parent) RegCloseKey(parent);
      RegCloseKey(source);
    }
    if (!restored) {
      std::fwprintf(stderr, L"Machine registry restoration failed; snapshot retained at HKCU\\%ls\n",
                    backup.c_str());
    }
    return restored;
  }

  void close_snapshot(bool remove_backup) noexcept {
    if (storage) RegCloseKey(storage);
    if (remove_backup) RegDeleteTreeW(HKEY_CURRENT_USER, backup.c_str());
  }
};
void write(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
      KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS, "create test key failed");
  const auto status = RegSetValueExW(key, name, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  check(status == ERROR_SUCCESS, "write test value failed");
}

void write_dword(const std::wstring& path, const wchar_t* name, DWORD value) {
  HKEY key = nullptr;
  check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                        KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS,
        "create DWORD test key failed");
  const auto status = RegSetValueExW(
      key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
      sizeof(value));
  RegCloseKey(key);
  check(status == ERROR_SUCCESS, "write DWORD test value failed");
}

DWORD read_dword(const std::wstring& path, const wchar_t* name) {
  DWORD value = 0;
  DWORD bytes = sizeof(value);
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_DWORD,
                     nullptr, &value, &bytes) == ERROR_SUCCESS,
        "read DWORD test value failed");
  return value;
}

bool string_value_exists(const std::wstring& path, const wchar_t* name) {
  DWORD bytes = 0;
  return RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_SZ,
                      nullptr, nullptr, &bytes) == ERROR_SUCCESS;
}

void write_machine(const std::wstring& path, const wchar_t* name,
                   const std::wstring& value) {
  HKEY key = nullptr;
  check(RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0,
                        KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &key,
                        nullptr) == ERROR_SUCCESS,
        "create machine test key failed");
  const auto status = RegSetValueExW(
      key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  check(status == ERROR_SUCCESS, "write machine test value failed");
}
std::wstring read(const std::wstring& path, const wchar_t* name) {
  wchar_t text[32768]{};
  DWORD bytes = sizeof(text);
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_SZ,
      nullptr, text, &bytes) == ERROR_SUCCESS, "read test value failed");
  return text;
}
std::wstring active_tree() {
  wchar_t text[32768]{};
  DWORD bytes = sizeof(text);
  const auto status = RegGetValueW(HKEY_CURRENT_USER,
      menu::directory_parent_key().c_str(), L"ExtendedSubCommandsKey",
      RRF_RT_REG_SZ, nullptr, text, &bytes);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return {};
  check(status == ERROR_SUCCESS, "read active preset tree failed");
  return L"Software\\Classes\\" + std::wstring{text};
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
  if (!machine_tests_enabled() || !process_is_elevated()) {
    std::fputs("SKIP: shell_context_menu_registry requires an elevated process and "
               "AWJ_RUN_MACHINE_REGISTRY_TESTS=1; HKLM was not touched.\n", stdout);
    return 0;
  }
  IsolatedRegistry sandbox;
  MachineRegistryRestore machine_restore;
  const auto exe = std::filesystem::absolute(argv[1]);
  menu::MenuParams params{};
  for (auto& value : params) value.quality_text = L"73";
  params[0].install_avif_png_command = true;
  require(menu::reconcile(exe, params));
  check(!*menu::is_installed(), "parameter save unexpectedly installed a menu");

  // A v5 launch must finish an interrupted v4 transaction before starting its
  // own migration. Recreate the exact v4 journal shape with one present,
  // managed root whose live value has drifted from the saved snapshot.
  const std::wstring v4_transaction =
      L"Software\\Classes\\AWJimage.ContextMenu.v4.Transaction";
  const auto v4_roots = menu::legacy_v4_transaction_roots();
  const auto v4_target = menu::image_parent_key();
  check(std::ranges::find(v4_roots, v4_target) != v4_roots.end(),
        "v4 recovery allow-list is missing the image parent");
  write(v4_transaction, menu::owner_value_name.data(),
        std::wstring{menu::owner_value});
  write_dword(v4_transaction, menu::schema_value_name.data(), 4);
  write_dword(v4_transaction, L"State", 1);
  for (std::size_t index = 0; index < v4_roots.size(); ++index) {
    const auto key = std::format(L"{}\\{:03}", v4_transaction, index);
    const bool present = v4_roots[index] == v4_target;
    write(key, L"Path", v4_roots[index]);
    write_dword(key, L"Present", present ? 1 : 0);
    write_dword(key, L"Managed", 1);
    if (!present) continue;
    const auto data = key + L"\\Data";
    write(data, menu::owner_value_name.data(), std::wstring{menu::owner_value});
    write_dword(data, menu::schema_value_name.data(), 4);
    write(data, L"MUIVerb", L"before v4 rollback");
  }
  write_dword(v4_transaction, L"Count",
              static_cast<DWORD>(v4_roots.size()));
  write(v4_target, menu::owner_value_name.data(),
        std::wstring{menu::owner_value});
  write_dword(v4_target, menu::schema_value_name.data(), 4);
  write(v4_target, L"MUIVerb", L"partial v4 update");
  write(v4_target, L"UnexpectedValue", L"must be removed");
  require(menu::recover());
  check(!exists(v4_transaction), "recovered v4 journal remains");
  check(read(v4_target, L"MUIVerb") == L"before v4 rollback" &&
            read_dword(v4_target, menu::schema_value_name.data()) == 4,
        "v4 snapshot was not restored");
  check(!string_value_exists(v4_target, L"UnexpectedValue"),
        "v4 recovery merged instead of replacing the live root");
  require(menu::remove());
  check(!exists(v4_target), "restored v4 root survived cleanup");

  const std::wstring legacy = L"Software\\Classes\\SystemFileAssociations\\image\\shell\\AWJImage";
  write(legacy + L"\\shell\\png\\command", nullptr,
        L"\"C:\\Old App\\AWJ.exe\" --shell-convert --format png -i \"%1\" %*");
  require(menu::install(exe, params));
  check(!exists(legacy), "recognized unmarked legacy menu survived migration");
  check(exists(menu::image_parent_key()), "generic image menu is missing");
  check(exists(menu::ico_parent_key()), "ico menu is missing");
  for (auto extension : menu::supported_extensions()) {
    check(exists(menu::extension_parent_key(extension)), "SFA extension is missing");
    check(exists(menu::class_extension_parent_key(extension)), "class extension is missing");
  }
  healthy(exe, params);
  const auto initial_tree = active_tree();
  require(menu::install(exe, params));
  check(active_tree() == initial_tree, "unchanged registration was rewritten");
  auto next = params;
  next[0].quality_text = L"74";
  require(menu::reconcile(exe, next));
  if (!initial_tree.empty()) {
    check(active_tree() != initial_tree && !exists(initial_tree), "slot switch/cleanup failed");
  } else {
    check(active_tree().empty(), "unexpected preset tree was created");
  }
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
  if (!active_tree().empty()) {
    drift(active_tree(), L"UnexpectedValue", L"unexpected");
    drift(active_tree() + L"\\shell\\unknown", L"MUIVerb", L"unexpected");
    drift(active_tree() + L"\\shell\\AWJimage.Convert.10.png\\command", nullptr, L"unexpected");
  }
  drift(menu::extension_parent_key(L".png"), L"AWJimage.SchemaVersion",
        std::to_wstring(menu::schema_version + 1));
  drift(menu::extension_parent_key(L".png"), L"SubCommands", L"unexpected");
  write_machine(menu::machine_command_store_key(L"png") + L"\\command", nullptr,
                L"unexpected");
  auto machine_warning = menu::warning(exe, next);
  check(!machine_warning || machine_warning->has_value(),
        "machine CommandStore drift was not detected");
  require(menu::reconcile(exe, next));
  healthy(exe, next);
  // Fail after inactive-tree creation and partial parent switching. Preserve the
  // journal when rollback is also denied; recover from its original snapshot.
  {
    DenyWrites failure{L"Software\\Classes\\SystemFileAssociations\\.webp\\shell"};
    check(!menu::install(exe, params), "registry write failure was reported as success");
  }
  require(menu::recover());
  healthy(exe, next);
  check(!exists(L"Software\\Classes\\AWJimage.ContextMenu.v5.Transaction"), "recovered journal remains");
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
  for (const auto& machine_root : menu::owned_machine_root_keys()) {
    check(!machine_exists(machine_root), "owned machine CommandStore residue remains");
  }
  std::puts("Shell v5 isolation, migration, drift, switching, rollback, recovery and preset limits passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
