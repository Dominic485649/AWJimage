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
#include "shell_extension_contract.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

const std::wstring kClassIdText{awj::shell_extension::contract::class_id};
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
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_SZ, nullptr,
                     value, &bytes) == ERROR_SUCCESS,
        "could not read registered string");
  return value;
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
  check(read_string(
            kClassRoot,
            awj::shell_extension::contract::context_menu_opt_in_value_name.data()) ==
            L"",
        "CLSID ContextMenuOptIn value is missing");
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
  check(can_unload() == S_FALSE,
        "DLL ignored a live class factory reference");

  IContextMenu* context_menu = nullptr;
  check(SUCCEEDED(factory->CreateInstance(
            nullptr, IID_IContextMenu,
            reinterpret_cast<void**>(&context_menu))) &&
            context_menu != nullptr,
        "class factory did not create IContextMenu");

  IExplorerCommand* explorer_command = nullptr;
  check(SUCCEEDED(factory->CreateInstance(
            nullptr, IID_IExplorerCommand,
            reinterpret_cast<void**>(&explorer_command))) &&
            explorer_command != nullptr,
        "class factory did not create IExplorerCommand");
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
