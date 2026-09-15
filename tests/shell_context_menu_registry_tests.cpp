#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include "isolated_registry.hpp"
#include "modern_configuration_restore.hpp"
#include "shell_context_menu.hpp"
#include "shell_extension_contract.hpp"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace menu = awj::shell_context_menu;

namespace {

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

void require(const std::expected<void, std::string>& result) {
  if (!result) throw std::runtime_error(result.error());
}

bool exists(const std::wstring& path) {
  HKEY key = nullptr;
  const auto status =
      RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
  if (key) RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

void write(const std::wstring& path, const wchar_t* name,
           const std::wstring& value) {
  HKEY key = nullptr;
  check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                        KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS,
        "create test key failed");
  const auto status = RegSetValueExW(
      key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  check(status == ERROR_SUCCESS, "write test value failed");
}

void write_dword(const std::wstring& path, const wchar_t* name,
                 DWORD value) {
  HKEY key = nullptr;
  check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                        KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS,
        "create test dword key failed");
  const auto status = RegSetValueExW(
      key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
      static_cast<DWORD>(sizeof(value)));
  RegCloseKey(key);
  check(status == ERROR_SUCCESS, "write test dword failed");
}

std::wstring read(const std::wstring& path, const wchar_t* name) {
  wchar_t text[32768]{};
  DWORD bytes = sizeof(text);
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_SZ,
                     nullptr, text, &bytes) == ERROR_SUCCESS,
        "read test value failed");
  return text;
}

std::vector<unsigned char> read_configuration() {
  const std::wstring name{
      awj::shell_extension::contract::configuration_value_name};
  DWORD bytes = 0;
  check(RegGetValueW(HKEY_CURRENT_USER, menu::class_root_key().c_str(),
                     name.c_str(), RRF_RT_REG_MULTI_SZ, nullptr, nullptr,
                     &bytes) == ERROR_SUCCESS,
        "read configuration size failed");
  std::vector<unsigned char> value(bytes);
  check(RegGetValueW(HKEY_CURRENT_USER, menu::class_root_key().c_str(),
                     name.c_str(), RRF_RT_REG_MULTI_SZ, nullptr, value.data(),
                     &bytes) == ERROR_SUCCESS,
        "read configuration failed");
  value.resize(bytes);
  return value;
}

void healthy(const std::filesystem::path& exe,
             const menu::MenuParams& params,
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
    check(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_ALL_ACCESS,
                        &key) == ERROR_SUCCESS,
          "open fault-injection key failed");
    check(GetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                          nullptr, nullptr, &old_dacl, nullptr,
                          &original) == ERROR_SUCCESS,
          "read test ACL failed");
    BYTE sid[SECURITY_MAX_SID_SIZE]{};
    DWORD bytes = sizeof(sid);
    check(CreateWellKnownSid(WinWorldSid, nullptr, sid, &bytes),
          "create test SID failed");
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE;
    entry.grfAccessMode = DENY_ACCESS;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    check(SetEntriesInAclW(1, &entry, old_dacl, &denied) == ERROR_SUCCESS,
          "build test ACL failed");
    check(SetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                          nullptr, nullptr, denied, nullptr) == ERROR_SUCCESS,
          "apply test write failure failed");
  }

  ~DenyWrites() {
    if (key) {
      SetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr,
                      nullptr, old_dacl, nullptr);
      RegCloseKey(key);
    }
    if (denied) LocalFree(denied);
    if (original) LocalFree(original);
  }
};

struct TempDirectory {
  std::filesystem::path path;
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

}  // namespace

int wmain(int argc, wchar_t** argv) try {
  check(argc == 2, "expected executable path");
  IsolatedRegistry sandbox;
  awj::test::ModernConfigurationRestore modern_configuration_restore;
  const auto exe = std::filesystem::absolute(argv[1]);
  check(std::filesystem::is_regular_file(menu::shell_extension_path(exe)),
        "shell extension DLL is not beside AWJ.exe");

  menu::MenuParams params{};
  for (auto& value : params) value.quality_text = L"73";
  params[0].install_avif_png_command = true;
  require(menu::reconcile(exe, params));
  check(!*menu::is_installed(),
        "parameter save unexpectedly installed a menu");

  const std::wstring legacy =
      L"Software\\Classes\\SystemFileAssociations\\image\\shell\\AWJImage";
  write(legacy + L"\\shell\\png\\command", nullptr,
        L"\"C:\\Old App\\AWJ.exe\" --shell-convert --format png -i \"%1\" %*");
  require(menu::install(exe, params));
  check(!exists(legacy), "recognized unmarked legacy menu survived migration");
  check(exists(menu::class_root_key()), "per-user CLSID registration is missing");
  check(exists(menu::file_handler_key()), "per-user file handler is missing");
  check(exists(menu::folder_handler_key()), "per-user folder handler is missing");
  check(!exists(menu::directory_parent_key()),
        "legacy static directory verb survived migration");
  for (auto extension : menu::supported_extensions()) {
    check(!exists(menu::extension_parent_key(extension)),
          "legacy static extension verb survived migration");
  }
  healthy(exe, params);

  const auto initial_configuration = read_configuration();
  require(menu::install(exe, params));
  check(read_configuration() == initial_configuration,
        "unchanged registration changed its atomic configuration");

  auto next = params;
  next[0].quality_text = L"74";
  require(menu::reconcile(exe, next));
  check(read_configuration() != initial_configuration,
        "updated menu parameters did not change the atomic configuration");
  healthy(exe, next);

  const auto drift = [&](const std::wstring& key, const wchar_t* name,
                         const std::wstring& value) {
    write(key, name, value);
    auto warning = menu::warning(exe, next);
    check(!warning || warning->has_value(), "registry drift was not detected");
    require(menu::reconcile(exe, next));
    healthy(exe, next);
  };
  drift(menu::class_root_key(), nullptr, L"unexpected default");
  drift(menu::class_root_key() + L"\\InprocServer32", nullptr,
        L"C:\\unexpected.dll");
  drift(menu::file_handler_key(), nullptr, L"{00000000-0000-0000-0000-000000000000}");
  drift(menu::class_root_key(),
        awj::shell_extension::contract::configuration_value_name.data(),
        L"wrong registry type");
  drift(menu::class_root_key(), L"UnexpectedValue", L"unexpected");
  drift(menu::class_root_key() + L"\\Unknown", L"UnexpectedValue",
        L"unexpected");

  {
    DenyWrites failure{
        L"Software\\Classes\\Folder\\shellex\\ContextMenuHandlers"};
    check(!menu::install(exe, params),
          "registry write failure was reported as success");
  }
  require(menu::recover());
  healthy(exe, next);
  check(!exists(L"Software\\Classes\\AWJimage.ContextMenu.v4.Transaction"),
        "recovered journal remains");

  check(!menu::install(exe.parent_path() / L"missing-AWJ.exe", params),
        "missing executable accepted");
  healthy(exe, next);

  const auto no_dll_root = std::filesystem::temp_directory_path() /
      (L"AWJ no shell DLL " + std::to_wstring(GetCurrentProcessId()));
  TempDirectory no_dll{no_dll_root};
  std::filesystem::create_directories(no_dll_root);
  const auto no_dll_exe = no_dll_root / L"AWJ.exe";
  std::filesystem::copy_file(exe, no_dll_exe,
                             std::filesystem::copy_options::overwrite_existing);
  check(!menu::install(no_dll_exe, params),
        "installation without the shell extension DLL succeeded");
  healthy(exe, next);

  std::vector<std::wstring> names;
  for (int count : {0, 1, 10, 11}) {
    names.clear();
    for (int i = 0; i < count; ++i) {
      names.push_back(L"预设 空格 " + std::to_wstring(i));
    }
    const auto result = menu::install(exe, next, names);
    if (count == 11) {
      check(!result, "eleventh injected preset accepted");
    } else {
      require(result);
      healthy(exe, next, names);
    }
  }
  names.pop_back();
  healthy(exe, next, names);
  names.back() = names.front();
  check(!menu::install(exe, next, names),
        "duplicate injected name accepted");

  const auto alternate_root = std::filesystem::temp_directory_path() /
      (L"AWJ shell path change " + std::to_wstring(GetCurrentProcessId()));
  TempDirectory alternate{alternate_root};
  std::filesystem::create_directories(alternate_root);
  const auto alternate_exe = alternate_root / L"AWJ.exe";
  const auto alternate_dll = menu::shell_extension_path(alternate_exe);
  std::filesystem::copy_file(exe, alternate_exe,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(menu::shell_extension_path(exe), alternate_dll,
                             std::filesystem::copy_options::overwrite_existing);
  require(menu::install(exe, next));
  require(menu::reconcile(alternate_exe, next));
  healthy(alternate_exe, next);
  require(menu::reconcile(exe, next));

  write(legacy + L"\\shell\\foreign\\command", nullptr,
        L"C:\\Other.exe --shell-convert");
  require(menu::remove());
  require(menu::remove());
  check(exists(legacy), "foreign legacy-looking root was deleted");
  check(!*menu::is_installed(), "owned registration residue remains");

  write(menu::file_handler_key(), nullptr,
        L"{11111111-1111-1111-1111-111111111111}");
  check(!menu::install(exe, params),
        "foreign COM handler root was overwritten");
  check(read(menu::file_handler_key(), nullptr) ==
            L"{11111111-1111-1111-1111-111111111111}",
         "foreign COM handler root changed");

  // Recovery snapshots legitimately contain absent roots (Present=0) and
  // foreign roots that must be preserved (Managed=0). Both records must be
  // accepted so a failed HKCU transaction can be discarded or rolled back.
  const std::wstring transaction =
      L"Software\\Classes\\AWJimage.ContextMenu.v4.Transaction";
  write(transaction, L"AWJimage.Owner", L"AWJimage");
  write_dword(transaction, L"State", 1);
  write_dword(transaction, L"Count", 2);
  const auto first = transaction + L"\\000";
  write(first, L"Path", menu::class_root_key());
  write_dword(first, L"Present", 0);
  write_dword(first, L"Managed", 1);
  const auto second = transaction + L"\\001";
  write(second, L"Path", menu::file_handler_key());
  write_dword(second, L"Present", 1);
  write_dword(second, L"Managed", 0);
  require(menu::recover());
  check(!exists(transaction), "valid absent/foreign recovery snapshot was rejected");

  std::puts("Shell v5 COM registration, migration, drift, rollback, recovery and preset limits passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
