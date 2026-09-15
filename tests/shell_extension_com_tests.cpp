#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include "isolated_registry.hpp"
#include "modern_configuration_restore.hpp"
#include "shell_extension_contract.hpp"

#include <cstdio>
#include <array>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

const std::wstring kClassIdText{awj::shell_extension::contract::class_id};
const std::wstring kModernClassIdText{
    awj::shell_extension::contract::modern_class_id};
const std::wstring kClassRoot{awj::shell_extension::contract::class_root};
const std::wstring kFileHandler{
    awj::shell_extension::contract::file_handler_root};
const std::wstring kFolderHandler{
    awj::shell_extension::contract::folder_handler_root};

using DllStatusFunction = HRESULT(__stdcall*)();
using DllGetClassObjectFunction = HRESULT(__stdcall*)(REFCLSID, REFIID,
                                                       void**);

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error{std::string{message}};
}

bool key_exists(const std::wstring& path) {
  HKEY key = nullptr;
  const LSTATUS status =
      RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
  if (key != nullptr) RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

std::wstring read_string(const std::wstring& path, const wchar_t* name) {
  wchar_t value[32768]{};
  DWORD bytes = sizeof(value);
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_SZ,
                     nullptr, value, &bytes) == ERROR_SUCCESS,
        "could not read registered string");
  return value;
}

bool string_value_is_absent(const std::wstring& path, const wchar_t* name) {
  wchar_t value[2]{};
  DWORD bytes = sizeof(value);
  const auto status = RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name,
                                   RRF_RT_REG_SZ, nullptr, value, &bytes);
  return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

std::vector<std::wstring> read_multi_string(const std::wstring& path,
                                            const wchar_t* name) {
  DWORD bytes = 0;
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name,
                     RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) ==
            ERROR_SUCCESS &&
            bytes >= 2 * sizeof(wchar_t) && bytes % sizeof(wchar_t) == 0,
        "could not read registered configuration");
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name,
                     RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &bytes) ==
            ERROR_SUCCESS,
        "could not read registered configuration data");
  std::vector<std::wstring> values;
  for (std::size_t cursor = 0; cursor + 1 < buffer.size();) {
    const auto* current = buffer.data() + cursor;
    const std::size_t length = wcslen(current);
    if (length == 0) break;
    values.emplace_back(current, length);
    cursor += length + 1;
  }
  check(!values.empty(), "registered configuration is empty");
  return values;
}

void write_multi_string(const std::wstring& path, const wchar_t* name,
                        const std::vector<std::wstring>& values) {
  std::size_t characters = 1;
  for (const auto& value : values) characters += value.size() + 1;
  std::vector<wchar_t> buffer;
  buffer.reserve(characters);
  for (const auto& value : values) {
    buffer.insert(buffer.end(), value.begin(), value.end());
    buffer.push_back(L'\0');
  }
  buffer.push_back(L'\0');
  HKEY raw = nullptr;
  check(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE,
                      &raw) == ERROR_SUCCESS,
        "could not open registered configuration");
  const auto status = RegSetValueExW(
      raw, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(buffer.data()),
      static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
  RegCloseKey(raw);
  check(status == ERROR_SUCCESS, "could not update registered configuration");
}

void write_modern_configuration_file(const std::vector<std::wstring>& values) {
  const auto path = awj::test::modern_configuration_path();
  check(path.has_value(), "could not locate modern configuration file");
  std::error_code error;
  std::filesystem::create_directories(path->parent_path(), error);
  check(!error, "could not create modern configuration directory");

  std::vector<wchar_t> buffer;
  std::size_t characters = 1;
  for (const auto& value : values) characters += value.size() + 1;
  buffer.reserve(characters);
  for (const auto& value : values) {
    buffer.insert(buffer.end(), value.begin(), value.end());
    buffer.push_back(L'\0');
  }
  buffer.push_back(L'\0');
  std::ofstream output(*path, std::ios::binary | std::ios::trunc);
  check(static_cast<bool>(output), "could not create modern configuration file");
  output.write(reinterpret_cast<const char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size() * sizeof(wchar_t)));
  check(output.good(), "could not write modern configuration file");
}

void add_test_preset_to_configuration() {
  auto values = read_multi_string(
      kClassRoot,
      awj::shell_extension::contract::configuration_value_name.data());
  check(values.size() >= 5 && values[4] == L"5",
        "unexpected default shell configuration command count");
  values[4] = L"10";
  constexpr std::array<std::wstring_view, 5> labels = {
      L"Convert to PNG", L"Convert to WebP", L"Convert to AVIF",
      L"Convert to JXL", L"Convert to JPGLI"};
  constexpr std::array<std::wstring_view, 5> formats = {
      L"png", L"webp", L"avif", L"jxl", L"jpgli"};
  constexpr std::array<std::wstring_view, 5> verbs = {
      L"png", L"webp", L"avif", L"jxl", L"jpgli"};
  for (std::size_t index = 0; index < labels.size(); ++index) {
    values.emplace_back(L"1");
    values.emplace_back(L"Test preset");
    values.emplace_back(L"AWJimage.Preset.00");
    values.emplace_back(labels[index]);
    values.emplace_back(std::wstring{L"AWJimage.Preset.00."} +
                        std::wstring{verbs[index]});
    values.emplace_back(L"8");
    values.emplace_back(L"--shell-window");
    values.emplace_back(L"--shell-convert");
    values.emplace_back(L"--preset");
    values.emplace_back(L"Test preset");
    values.emplace_back(L"--format");
    values.emplace_back(formats[index]);
    values.emplace_back(L"--collision");
    values.emplace_back(L"number");
  }
  write_multi_string(
      kClassRoot,
      awj::shell_extension::contract::configuration_value_name.data(),
      values);
}

template <typename Function>
Function export_from(HMODULE module, const char* name) {
  const auto address = GetProcAddress(module, name);
  check(address != nullptr, "required DLL export is missing");
  return reinterpret_cast<Function>(address);
}

}  // namespace

int wmain(int argc, wchar_t** argv) try {
  check(argc == 2, "expected shell extension DLL path");
  IsolatedRegistry registry;
  awj::test::ModernConfigurationRestore modern_configuration_restore;
  HMODULE module = LoadLibraryW(argv[1]);
  check(module != nullptr, "could not load shell extension DLL");
  struct ModuleCleanup {
    HMODULE module{};
    ~ModuleCleanup() {
      if (module != nullptr) FreeLibrary(module);
    }
  } module_cleanup{module};

  const auto can_unload =
      export_from<DllStatusFunction>(module, "DllCanUnloadNow");
  const auto register_server =
      export_from<DllStatusFunction>(module, "DllRegisterServer");
  const auto unregister_server =
      export_from<DllStatusFunction>(module, "DllUnregisterServer");
  const auto get_class_object = export_from<DllGetClassObjectFunction>(
      module, "DllGetClassObject");

  check(can_unload() == S_OK, "new DLL reported live COM objects");
  check(register_server() == S_OK, "per-user self-registration failed");
  check(read_string(kClassRoot, L"AWJimage.Owner") == L"AWJimage",
        "CLSID ownership marker is missing");
  check(string_value_is_absent(
            kClassRoot,
            awj::shell_extension::contract::context_menu_opt_in_value_name.data()),
        "CLSID ContextMenuOptIn value should not opt the classic handler into the modern menu");
  check(read_string(kFileHandler, nullptr) == kClassIdText,
        "file handler registration is incorrect");
  check(read_string(kFolderHandler, nullptr) == kClassIdText,
        "folder handler registration is incorrect");

  CLSID class_id{};
  check(SUCCEEDED(CLSIDFromString(kClassIdText.c_str(), &class_id)),
        "test CLSID is invalid");
  IClassFactory* factory = nullptr;
  check(SUCCEEDED(get_class_object(class_id, IID_IClassFactory,
                                   reinterpret_cast<void**>(&factory))) &&
            factory != nullptr,
        "DllGetClassObject did not return a class factory");

  CLSID modern_class_id{};
  check(SUCCEEDED(CLSIDFromString(kModernClassIdText.c_str(),
                                  &modern_class_id)),
        "modern test CLSID is invalid");
  IClassFactory* modern_factory = nullptr;
  check(SUCCEEDED(get_class_object(modern_class_id, IID_IClassFactory,
                                   reinterpret_cast<void**>(&modern_factory))) &&
            modern_factory != nullptr,
        "DllGetClassObject did not return the modern class factory");
  IExplorerCommand* modern_command = nullptr;
  check(SUCCEEDED(modern_factory->CreateInstance(
            nullptr, IID_IExplorerCommand,
            reinterpret_cast<void**>(&modern_command))) &&
            modern_command != nullptr,
        "modern class factory did not create IExplorerCommand");
  IContextMenu* modern_context_menu = nullptr;
  check(modern_factory->CreateInstance(
            nullptr, IID_IContextMenu,
            reinterpret_cast<void**>(&modern_context_menu)) == E_NOINTERFACE &&
            modern_context_menu == nullptr,
        "modern class factory unexpectedly exposed IContextMenu");
  IShellExtInit* modern_initializer = nullptr;
  check(modern_factory->CreateInstance(
            nullptr, IID_IShellExtInit,
            reinterpret_cast<void**>(&modern_initializer)) == E_NOINTERFACE &&
            modern_initializer == nullptr,
        "modern class factory unexpectedly exposed IShellExtInit");
  check(can_unload() == S_FALSE,
        "DLL ignored a live class factory reference");

  IContextMenu* context_menu = nullptr;
  check(SUCCEEDED(factory->CreateInstance(
            nullptr, IID_IContextMenu,
            reinterpret_cast<void**>(&context_menu))) &&
            context_menu != nullptr,
        "class factory did not create IContextMenu");

  IExplorerCommand* explorer_command = nullptr;
  check(factory->CreateInstance(
            nullptr, IID_IExplorerCommand,
            reinterpret_cast<void**>(&explorer_command)) == E_NOINTERFACE &&
            explorer_command == nullptr,
        "classic class factory unexpectedly exposed IExplorerCommand");
  explorer_command = modern_command;
  modern_command = nullptr;
  check(explorer_command != nullptr,
        "modern class factory did not create IExplorerCommand");
  EXPCMDFLAGS explorer_flags{};
  check(SUCCEEDED(explorer_command->GetFlags(&explorer_flags)) &&
            explorer_flags == ECF_HASSUBCOMMANDS,
        "root Explorer command does not advertise subcommands");
  PWSTR explorer_title = nullptr;
  check(SUCCEEDED(explorer_command->GetTitle(nullptr, &explorer_title)) &&
            explorer_title != nullptr && *explorer_title != L'\0',
        "root Explorer command has no title");
  CoTaskMemFree(explorer_title);
  EXPCMDSTATE fast_state = ECS_HIDDEN;
  check(SUCCEEDED(explorer_command->GetState(nullptr, FALSE, &fast_state)) &&
            fast_state == ECS_ENABLED,
        "root Explorer command is hidden during the fast state probe");

  IEnumExplorerCommand* subcommands = nullptr;
  check(SUCCEEDED(explorer_command->EnumSubCommands(&subcommands)) &&
            subcommands != nullptr,
        "root Explorer command did not enumerate subcommands");
  for (int index = 0; index < 5; ++index) {
    IExplorerCommand* child = nullptr;
    ULONG fetched = 0;
    check(subcommands->Next(1, &child, &fetched) == S_OK && fetched == 1 &&
              child != nullptr,
          "Explorer command enumeration did not return five children");
    EXPCMDFLAGS child_flags{};
    check(SUCCEEDED(child->GetFlags(&child_flags)) &&
              child_flags == ECF_DEFAULT,
          "Explorer child command has unexpected flags");
    PWSTR child_title = nullptr;
    check(SUCCEEDED(child->GetTitle(nullptr, &child_title)) &&
              child_title != nullptr && *child_title != L'\0',
          "Explorer child command has no title");
    CoTaskMemFree(child_title);
    child->Release();
  }
  IExplorerCommand* exhausted_child = nullptr;
  ULONG exhausted_fetched = 0;
  check(subcommands->Next(1, &exhausted_child, &exhausted_fetched) == S_FALSE &&
            exhausted_child == nullptr && exhausted_fetched == 0,
        "Explorer command enumeration returned an unexpected sixth child");
  subcommands->Release();
  explorer_command->Release();
  modern_factory->Release();

  // A preset adds five grouped commands to the serialized runtime
  // configuration.  Modern Explorer deliberately receives only the five
  // direct format commands so the classic preset branch cannot be merged into
  // the packaged command tree as a second copy of those commands.
  auto modern_file_values = read_multi_string(
      kClassRoot,
      awj::shell_extension::contract::configuration_value_name.data());
  check(modern_file_values.size() > 3,
        "serialized configuration is missing its menu label");
  modern_file_values[3] = L"Modern file configuration";
  add_test_preset_to_configuration();
  write_modern_configuration_file(modern_file_values);
  IClassFactory* grouped_factory = nullptr;
  check(SUCCEEDED(get_class_object(
            modern_class_id, IID_IClassFactory,
            reinterpret_cast<void**>(&grouped_factory))) &&
            grouped_factory != nullptr,
        "could not create modern factory for grouped configuration");
  IExplorerCommand* grouped_root = nullptr;
  check(SUCCEEDED(grouped_factory->CreateInstance(
            nullptr, IID_IExplorerCommand,
            reinterpret_cast<void**>(&grouped_root))) &&
            grouped_root != nullptr,
        "could not create grouped modern root command");
  PWSTR grouped_title = nullptr;
  check(SUCCEEDED(grouped_root->GetTitle(nullptr, &grouped_title)) &&
            grouped_title != nullptr &&
            std::wstring_view{grouped_title} == L"Modern file configuration",
        "modern COM did not prefer its file-backed configuration");
  CoTaskMemFree(grouped_title);
  IEnumExplorerCommand* grouped_root_children = nullptr;
  check(SUCCEEDED(grouped_root->EnumSubCommands(&grouped_root_children)) &&
            grouped_root_children != nullptr,
        "grouped modern root did not enumerate children");
  for (int index = 0; index < 5; ++index) {
    IExplorerCommand* child = nullptr;
    ULONG fetched = 0;
    check(grouped_root_children->Next(1, &child, &fetched) == S_OK &&
              fetched == 1 && child != nullptr,
          "modern root lost a direct format command after adding a preset");
    EXPCMDFLAGS child_flags{};
    check(SUCCEEDED(child->GetFlags(&child_flags)) &&
              child_flags == ECF_DEFAULT,
          "modern direct command is not a leaf after adding a preset");
    child->Release();
  }
  IExplorerCommand* unexpected_root_child = nullptr;
  ULONG unexpected_root_fetched = 0;
  check(grouped_root_children->Next(1, &unexpected_root_child,
                                    &unexpected_root_fetched) == S_FALSE &&
            unexpected_root_child == nullptr && unexpected_root_fetched == 0,
        "modern root exposed duplicate preset commands");
  grouped_root_children->Release();
  grouped_root->Release();
  grouped_factory->Release();

  IShellExtInit* initializer = nullptr;
  check(SUCCEEDED(context_menu->QueryInterface(
            IID_IShellExtInit, reinterpret_cast<void**>(&initializer))) &&
            initializer != nullptr,
        "context menu does not expose IShellExtInit");
  initializer->Release();
  context_menu->Release();
  factory->Release();
  check(can_unload() == S_OK,
        "DLL retained COM objects after final Release");

  check(unregister_server() == S_OK, "per-user unregistration failed");
  if (const auto modern_path = awj::test::modern_configuration_path();
      modern_path) {
    check(!std::filesystem::exists(*modern_path),
          "per-user unregistration left the modern configuration file");
  }
  check(!key_exists(kClassRoot),
        "owned CLSID key remains after unregistration");
  check(!key_exists(kFileHandler),
        "owned file handler remains after unregistration");
  check(!key_exists(kFolderHandler),
        "owned folder handler remains after unregistration");

  std::puts("Shell extension COM exports, lifetime and isolated HKCU registration passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
