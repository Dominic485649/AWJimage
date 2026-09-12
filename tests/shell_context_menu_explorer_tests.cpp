#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "shell_context_menu.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import awj.decoder_registry;
import awj.avif_aom_codec;
import awj.image;

namespace {

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

// Real Shell activation crosses the process boundary, so HKCU overrides are not
// sufficient here. Persist exactly the module's AWJ roots before any mutation.
struct RegistryRestore {
  std::vector<std::wstring> paths = awj::shell_context_menu::owned_root_keys();
  std::vector<bool> present;
  std::wstring backup = L"Software\\AWJimage.Tests.Backup\\" + std::to_wstring(GetCurrentProcessId());
  HKEY storage{};
  std::vector<std::wstring> machine_paths =
      awj::shell_context_menu::owned_machine_root_keys();
  std::vector<bool> machine_present;
  HKEY machine_storage{};
  RegistryRestore() {
    try {
      HKEY pending{};
      if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\AWJimage.ContextMenu.v5.Transaction",
                      0, KEY_READ, &pending) == ERROR_SUCCESS ||
          RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\AWJimage.ContextMenu.v4.Transaction",
                      0, KEY_READ, &pending) == ERROR_SUCCESS) {
        RegCloseKey(pending);
        throw std::runtime_error("pending AWJ registry transaction; do not start real Shell test");
      }
      DWORD disposition{};
      if (RegCreateKeyExW(HKEY_CURRENT_USER, backup.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                       nullptr, &storage, &disposition) != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY)
        throw std::runtime_error("could not create unique registry snapshot");
      for (std::size_t i = 0; i < paths.size(); ++i) {
      const auto key = std::to_wstring(i);
      HKEY source{}, destination{};
      const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, paths[i].c_str(), 0, KEY_READ, &source);
      if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
        throw std::runtime_error("cannot read AWJ registry root for snapshot");
      present.push_back(status == ERROR_SUCCESS);
      if (RegCreateKeyExW(storage, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &destination, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("cannot create registry snapshot entry");
      const auto copied = source ? RegCopyTreeW(source, nullptr, destination) : ERROR_SUCCESS;
      if (source) RegCloseKey(source);
      RegCloseKey(destination);
      if (copied != ERROR_SUCCESS) throw std::runtime_error("cannot copy registry snapshot");
      const DWORD existed = present.back() ? 1 : 0;
      if (RegSetValueExW(storage, key.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(paths[i].c_str()),
                        static_cast<DWORD>((paths[i].size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS ||
          RegSetValueExW(storage, (key + L".Present").c_str(), 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&existed), sizeof(existed)) != ERROR_SUCCESS)
        throw std::runtime_error("cannot persist registry snapshot index");
      }
      DWORD machine_disposition{};
      if (RegCreateKeyExW(storage, L"Machine", 0, nullptr, 0, KEY_ALL_ACCESS,
                       nullptr, &machine_storage, &machine_disposition) != ERROR_SUCCESS ||
          machine_disposition != REG_CREATED_NEW_KEY) {
        throw std::runtime_error("could not create machine registry snapshot");
      }
      for (std::size_t i = 0; i < machine_paths.size(); ++i) {
      const auto key = std::to_wstring(i);
      HKEY source{}, destination{};
      const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, machine_paths[i].c_str(), 0,
                                        KEY_READ | KEY_WOW64_64KEY, &source);
      if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND &&
          status != ERROR_PATH_NOT_FOUND) {
        throw std::runtime_error("cannot read machine registry root for snapshot");
      }
      machine_present.push_back(status == ERROR_SUCCESS);
      if (RegCreateKeyExW(machine_storage, key.c_str(), 0, nullptr, 0,
                          KEY_ALL_ACCESS, nullptr, &destination, nullptr) != ERROR_SUCCESS) {
        throw std::runtime_error("cannot create machine registry snapshot entry");
      }
      const auto copied = source ? RegCopyTreeW(source, nullptr, destination) : ERROR_SUCCESS;
      if (source) RegCloseKey(source);
      RegCloseKey(destination);
      if (copied != ERROR_SUCCESS) throw std::runtime_error("cannot copy machine registry snapshot");
      }
      for (const auto& path : machine_paths) {
        if (!delete_machine_tree(path)) {
          throw std::runtime_error("cannot clear machine registry root");
        }
      }
      RegFlushKey(storage);
    } catch (...) {
      const bool restored = restore_machine_roots();
      close_snapshot(restored);
      throw;
    }
  }
  ~RegistryRestore() {
    bool restored = true;
    restored &= restore_machine_roots();
    for (std::size_t i = 0; i < paths.size(); ++i) {
      const auto removed = RegDeleteTreeW(HKEY_CURRENT_USER, paths[i].c_str());
      restored &= removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND ||
                  removed == ERROR_PATH_NOT_FOUND;
      if (!present[i]) continue;
      HKEY source{}, destination{};
      if (RegOpenKeyExW(storage, std::to_wstring(i).c_str(), 0, KEY_READ, &source) != ERROR_SUCCESS ||
          RegCreateKeyExW(HKEY_CURRENT_USER, paths[i].c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                          nullptr, &destination, nullptr) != ERROR_SUCCESS) {
        restored = false;
      } else {
        restored &= RegCopyTreeW(source, nullptr, destination) == ERROR_SUCCESS;
      }
      if (source) RegCloseKey(source);
      if (destination) RegCloseKey(destination);
    }
    close_snapshot(restored);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (!restored) {
      std::fwprintf(stderr, L"Registry restoration failed; snapshot retained at HKCU\\%ls\n", backup.c_str());
      std::abort();
    }
  }

 private:
  bool restore_machine_roots() noexcept {
    bool restored = true;
    for (std::size_t i = 0; i < machine_present.size() && i < machine_paths.size(); ++i) {
      restored &= delete_machine_tree(machine_paths[i]);
      if (!machine_present[i]) continue;
      const auto key = std::to_wstring(i);
      HKEY source{}, destination{}, parent{};
      if (RegOpenKeyExW(machine_storage, key.c_str(), 0, KEY_READ, &source) != ERROR_SUCCESS) {
        restored = false;
        continue;
      }
      const auto slash = machine_paths[i].find_last_of(L'\\');
      const auto parent_path = machine_paths[i].substr(0, slash);
      const auto leaf = machine_paths[i].substr(slash + 1);
      if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, parent_path.c_str(), 0, nullptr, 0,
                          KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &parent, nullptr) != ERROR_SUCCESS ||
          RegCreateKeyExW(parent, leaf.c_str(), 0, nullptr, 0,
                          KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &destination, nullptr) != ERROR_SUCCESS) {
        restored = false;
      } else {
        restored &= RegCopyTreeW(source, nullptr, destination) == ERROR_SUCCESS;
      }
      if (destination) RegCloseKey(destination);
      if (parent) RegCloseKey(parent);
      RegCloseKey(source);
    }
    return restored;
  }

  void close_snapshot(bool remove_backup) noexcept {
    if (machine_storage) RegCloseKey(machine_storage);
    if (storage) RegCloseKey(storage);
    if (remove_backup) RegDeleteTreeW(HKEY_CURRENT_USER, backup.c_str());
  }
};

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  explicit ComPtr(T* value) noexcept : value_(value) {}
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  ~ComPtr() { reset(); }
  T* get() const noexcept { return value_; }
  T* operator->() const noexcept { return value_; }
  void reset(T* value = nullptr) noexcept {
    if (value_ != nullptr) value_->Release();
    value_ = value;
  }

 private:
  T* value_{};
};

template <typename PidlType>
class PidlOwner {
 public:
  PidlOwner() = default;
  explicit PidlOwner(PidlType value) noexcept : value_(value) {}
  PidlOwner(const PidlOwner&) = delete;
  PidlOwner& operator=(const PidlOwner&) = delete;
  PidlOwner(PidlOwner&& other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  PidlOwner& operator=(PidlOwner&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  ~PidlOwner() { reset(); }
  PidlType get() const noexcept { return value_; }
  void reset() noexcept {
    if (value_ != nullptr) CoTaskMemFree(value_);
    value_ = nullptr;
  }

 private:
  PidlType value_{};
};

using AbsolutePidl = PidlOwner<PIDLIST_ABSOLUTE>;
using ChildPidl = PidlOwner<PITEMID_CHILD>;

struct ContextMenu {
  ComPtr<IContextMenu> context{};
  HMENU menu{};
  UINT id_first{1};

  ContextMenu() = default;
  ContextMenu(const ContextMenu&) = delete;
  ContextMenu& operator=(const ContextMenu&) = delete;
  ContextMenu(ContextMenu&& other) noexcept
      : context(std::move(other.context)),
        menu(std::exchange(other.menu, nullptr)),
        id_first(other.id_first) {}
  ContextMenu& operator=(ContextMenu&& other) noexcept {
    if (this != &other) {
      if (menu != nullptr) DestroyMenu(menu);
      context = std::move(other.context);
      menu = std::exchange(other.menu, nullptr);
      id_first = other.id_first;
    }
    return *this;
  }
  ~ContextMenu() {
    if (menu != nullptr) DestroyMenu(menu);
  }
};

struct ComApartment {
  HRESULT status{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
  ~ComApartment() {
    if (SUCCEEDED(status)) CoUninitialize();
  }
};

bool write_one_pixel_bmp(const std::filesystem::path& path) {
  constexpr std::array<unsigned char, 58> bmp = {
      0x42, 0x4d, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x36, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x01, 0x00,
      0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x13, 0x0b,
      0x00, 0x00, 0x13, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00};
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bmp.data()),
             static_cast<std::streamsize>(bmp.size()));
  return file.good();
}

awj::shell_context_menu::MenuParams make_params(bool avif_png) {
  awj::shell_context_menu::MenuParams params{};
  for (auto& item : params) {
    item.close_on_finish = true;
    item.allow_wic_fallback = true;
    item.size_limit_index = 0;
  }
  params[0].quality_text = L"75";
  params[0].speed_text = L"6";
  params[0].install_avif_png_command = avif_png;
  params[1].quality_text = L"80";
  params[2].quality_text = L"80";
  params[2].speed_text = L"6";
  params[3].quality_text = L"80";
  return params;
}

std::expected<ContextMenu, std::string> create_context_menu(
    const std::vector<std::filesystem::path>& paths) {
  if (paths.empty()) return std::unexpected{"no shell items"};
  const auto parent = paths.front().parent_path();
  for (const auto& path : paths) {
    if (path.parent_path() != parent) {
      return std::unexpected{"shell items do not share a parent"};
    }
  }

  PIDLIST_ABSOLUTE raw_parent = nullptr;
  const auto parent_path = parent.wstring();
  HRESULT hr = SHParseDisplayName(parent_path.c_str(), nullptr, &raw_parent, 0, nullptr);
  if (FAILED(hr)) return std::unexpected{"SHParseDisplayName(parent) failed"};
  AbsolutePidl parent_pidl{raw_parent};

  IShellFolder* raw_desktop = nullptr;
  hr = SHGetDesktopFolder(&raw_desktop);
  if (FAILED(hr)) return std::unexpected{"SHGetDesktopFolder failed"};
  ComPtr<IShellFolder> desktop{raw_desktop};

  IShellFolder* raw_folder = nullptr;
  hr = desktop->BindToObject(parent_pidl.get(), nullptr, IID_PPV_ARGS(&raw_folder));
  if (FAILED(hr)) return std::unexpected{"BindToObject(parent) failed"};
  ComPtr<IShellFolder> folder{raw_folder};

  std::vector<ChildPidl> child_storage;
  std::vector<PCUITEMID_CHILD> children;
  child_storage.reserve(paths.size());
  children.reserve(paths.size());
  for (const auto& path : paths) {
    auto name = path.filename().wstring();
    name.push_back(L'\0');
    ULONG eaten = 0;
    PITEMID_CHILD raw_child = nullptr;
    SFGAOF attributes = 0;
    hr = folder->ParseDisplayName(nullptr, nullptr, name.data(), &eaten,
                                  &raw_child, &attributes);
    if (FAILED(hr)) return std::unexpected{"ParseDisplayName(child) failed"};
    child_storage.emplace_back(raw_child);
    children.push_back(reinterpret_cast<PCUITEMID_CHILD>(child_storage.back().get()));
  }

  IContextMenu* raw_context = nullptr;
  hr = folder->GetUIObjectOf(nullptr, static_cast<UINT>(children.size()),
                             children.data(), IID_IContextMenu, nullptr,
                             reinterpret_cast<void**>(&raw_context));
  if (FAILED(hr)) return std::unexpected{"GetUIObjectOf(IContextMenu) failed"};

  ContextMenu result;
  result.context.reset(raw_context);
  result.menu = CreatePopupMenu();
  if (result.menu == nullptr) return std::unexpected{"CreatePopupMenu failed"};
  hr = result.context->QueryContextMenu(result.menu, 0, result.id_first, 0x7fff,
                                        CMF_NORMAL);
  if (FAILED(hr)) return std::unexpected{"IContextMenu::QueryContextMenu failed"};
  return result;
}

std::wstring menu_text(HMENU menu, int position) {
  std::array<wchar_t, 512> buffer{};
  const int copied = GetMenuStringW(menu, static_cast<UINT>(position), buffer.data(),
                                    static_cast<int>(buffer.size()), MF_BYPOSITION);
  return copied > 0 ? std::wstring{buffer.data(), static_cast<std::size_t>(copied)}
                    : std::wstring{};
}

void initialize_popup(ContextMenu& context_menu, HMENU submenu, int position) {
  IContextMenu3* raw_context3 = nullptr;
  if (SUCCEEDED(context_menu.context->QueryInterface(IID_PPV_ARGS(&raw_context3)))) {
    ComPtr<IContextMenu3> context3{raw_context3};
    LRESULT result = 0;
    (void)context3->HandleMenuMsg2(
        WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
        MAKELPARAM(static_cast<WORD>(position), FALSE), &result);
    return;
  }
  IContextMenu2* raw_context2 = nullptr;
  if (SUCCEEDED(context_menu.context->QueryInterface(IID_PPV_ARGS(&raw_context2)))) {
    ComPtr<IContextMenu2> context2{raw_context2};
    (void)context2->HandleMenuMsg(
        WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
        MAKELPARAM(static_cast<WORD>(position), FALSE));
  }
}

HMENU find_awj_submenu(ContextMenu& context_menu) {
  const int count = GetMenuItemCount(context_menu.menu);
  for (int i = 0; i < count; ++i) {
    if (menu_text(context_menu.menu, i) != L"AWJimage 转换") continue;
    HMENU submenu = GetSubMenu(context_menu.menu, i);
    if (submenu != nullptr) initialize_popup(context_menu, submenu, i);
    return submenu;
  }
  return nullptr;
}

std::vector<std::wstring> submenu_labels(HMENU submenu) {
  std::vector<std::wstring> labels;
  const int count = GetMenuItemCount(submenu);
  for (int i = 0; i < count; ++i) {
    if ((GetMenuState(submenu, static_cast<UINT>(i), MF_BYPOSITION) & MF_SEPARATOR) != 0) {
      continue;
    }
    labels.push_back(menu_text(submenu, i));
  }
  return labels;
}

void dump_menu(HMENU menu, int depth = 0) {
  const int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    const auto text = menu_text(menu, i);
    const auto state = GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION);
    std::fwprintf(stderr, L"%*ls[%d] id=%u state=0x%08x text='%ls'\n",
                  depth * 2, L"", i, GetMenuItemID(menu, i), state, text.c_str());
    if (HMENU child = GetSubMenu(menu, i); child != nullptr) dump_menu(child, depth + 1);
  }
}

std::vector<std::wstring> expected_labels(bool avif_png) {
  std::vector<std::wstring> labels;
  for (const auto& spec : awj::shell_context_menu::command_specs()) {
    if (spec.append_png_suffix && !avif_png) continue;
    labels.emplace_back(spec.label);
  }
  return labels;
}

std::expected<void, std::string> invoke_and_decode(ContextMenu& menu, HMENU submenu,
    const std::vector<std::filesystem::path>& inputs, const std::filesystem::path& working_directory,
    std::size_t command_index, bool directory_selection = false) {
  const auto& spec = awj::shell_context_menu::command_specs()[command_index];
  const UINT command = GetMenuItemID(submenu, static_cast<int>(command_index));
  if (command < menu.id_first || command == UINT(-1)) return std::unexpected{"invalid Shell command ID"};
  CMINVOKECOMMANDINFOEX invoke{};
  invoke.cbSize = sizeof(invoke);
  invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_NOASYNC;
  invoke.lpVerb = MAKEINTRESOURCEA(command - menu.id_first);
  invoke.lpVerbW = MAKEINTRESOURCEW(command - menu.id_first);
  const auto cwd = working_directory.wstring();
  invoke.lpDirectoryW = cwd.c_str();
  invoke.nShow = SW_SHOWNORMAL;
  const auto status = menu.context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&invoke));
  if (FAILED(status)) return std::unexpected{"IContextMenu::InvokeCommand failed: " + std::to_string(status)};
  const std::wstring extension = spec.append_png_suffix ? L".avif.png"
      : spec.format == L"jpgli" ? L".jpg" : L"." + std::wstring{spec.format};
  const auto deadline = GetTickCount64() + 45000;
  std::string last_failure = "output file not found";
  for (;;) {
    bool complete = true;
    for (const auto& input : inputs) {
      const auto output_dir = directory_selection ? input.parent_path().parent_path() / L"AWJOutput" : input.parent_path();
      const auto output = output_dir / (input.stem().wstring() + extension);
      std::error_code ec;
      if (!std::filesystem::is_regular_file(output, ec) || ec) { complete = false; break; }
      // AVIF.png deliberately carries an AVIF stream despite its final suffix.
      const auto decoded = spec.format == L"avif" ? awj::make_avif_image_decoder(1)->decode(output)
          : awj::decode_image_for_path(output, {.allow_wic_fallback = false, .decode_threads = 1});
      if (!decoded || decoded->image.width != 1 || decoded->image.height != 1) {
        last_failure = decoded ? "unexpected decoded dimensions" : decoded.error();
        complete = false;
        break;
      }
    }
    if (complete) break;
    if (GetTickCount64() >= deadline) return std::unexpected{"Shell output missing or not decodable: " + last_failure};
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(50);
  }
  std::fwprintf(stdout, L"InvokeCommand decoded %zu item(s), format %ls\n", inputs.size(), extension.c_str());
  return {};
}

}  // namespace

int wmain(int argc, wchar_t** argv) try {
  using namespace awj::shell_context_menu;
  if (argc != 2) return fail("expected AWJ executable path argument");
  if (!machine_tests_enabled() || !process_is_elevated()) {
    std::fputs("SKIP: shell_context_menu_explorer requires an elevated process and "
               "AWJ_RUN_MACHINE_REGISTRY_TESTS=1; HKLM was not touched.\n", stdout);
    return 0;
  }
  const std::filesystem::path awj_exe{argv[1]};
  if (!std::filesystem::is_regular_file(awj_exe)) return fail("AWJ executable is missing");

  ComApartment apartment;
  if (FAILED(apartment.status)) return fail("CoInitializeEx failed");

  RegistryRestore registry_cleanup;

  const auto root = std::filesystem::temp_directory_path() /
      (L"AWJ Explorer API 验证 空格 " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  std::error_code ec;
  if (!std::filesystem::create_directory(root, ec) || ec) return fail("failed to create isolated Explorer API test root");
  struct TempCleanup {
    std::filesystem::path path;
    ~TempCleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } temp_cleanup{root};

  const auto executable_dir = root / L"程序 空格";
  std::filesystem::create_directory(executable_dir, ec);
  const auto test_exe = executable_dir / L"AWJ.exe";
  if (ec || !std::filesystem::copy_file(awj_exe, test_exe, ec) || ec)
    return fail("failed to copy the test executable into its isolated profile");
  auto params = make_params(true);
  auto installed = install(test_exe, params);
  if (!installed) return fail(installed.error());

  const auto single_dir = root / L"single 菜单";
  std::filesystem::create_directories(single_dir, ec);
  const auto single_input = single_dir / L"输入 单图 Ω.bmp";
  if (ec || !write_one_pixel_bmp(single_input)) return fail("failed to create single input");
  auto single_menu = create_context_menu({single_input});
  if (!single_menu) return fail(single_menu.error());
  HMENU single_submenu = find_awj_submenu(*single_menu);
  if (single_submenu == nullptr || submenu_labels(single_submenu) != expected_labels(true)) {
    std::fputs("Explorer/Shell single-file menu dump follows:\n", stderr);
    dump_menu(single_menu->menu);
    return fail("single-file Explorer/Shell menu order is incorrect");
  }
  for (std::size_t i = 0; i < command_specs().size(); ++i) {
    if (auto result = invoke_and_decode(*single_menu, single_submenu, {single_input}, root, i); !result)
      return fail(result.error());
  }

  const auto multi_dir = root / L"multi 菜单";
  std::filesystem::create_directories(multi_dir, ec);
  const auto multi_a = multi_dir / L"多选 一.bmp";
  const auto multi_b = multi_dir / L"多选 二.bmp";
  if (ec || !write_one_pixel_bmp(multi_a) || !write_one_pixel_bmp(multi_b)) {
    return fail("failed to create multi-select inputs");
  }
  auto multi_menu = create_context_menu({multi_a, multi_b});
  if (!multi_menu) return fail(multi_menu.error());
  HMENU multi_submenu = find_awj_submenu(*multi_menu);
  if (multi_submenu == nullptr || submenu_labels(multi_submenu) != expected_labels(true)) {
    return fail("multi-select Explorer/Shell menu order is incorrect");
  }
  for (std::size_t i = 0; i < command_specs().size(); ++i) {
    if (auto result = invoke_and_decode(*multi_menu, multi_submenu, {multi_a, multi_b}, root, i); !result)
      return fail(result.error());
  }

  const auto folder_input = root / L"folder 菜单输入";
  std::filesystem::create_directories(folder_input, ec);
  const auto folder_bmp = folder_input / L"目录图像.bmp";
  if (ec || !write_one_pixel_bmp(folder_bmp)) return fail("failed to create folder input");
  auto folder_menu = create_context_menu({folder_input});
  if (!folder_menu) return fail(folder_menu.error());
  HMENU folder_submenu = find_awj_submenu(*folder_menu);
  if (folder_submenu == nullptr || submenu_labels(folder_submenu) != expected_labels(true)) {
    return fail("folder Explorer/Shell menu order is incorrect");
  }
  // Directory shell output is beside the selected directory.
  for (std::size_t i = 0; i < command_specs().size(); ++i) {
    if (auto result = invoke_and_decode(*folder_menu, folder_submenu, {folder_bmp}, root, i, true); !result)
      return fail(result.error());
  }

  params[0].install_avif_png_command = false;
  installed = install(test_exe, params);
  if (!installed) return fail(installed.error());
  auto no_png_menu = create_context_menu({single_input});
  if (!no_png_menu) return fail(no_png_menu.error());
  HMENU no_png_submenu = find_awj_submenu(*no_png_menu);
  if (no_png_submenu == nullptr || submenu_labels(no_png_submenu) != expected_labels(false)) {
    return fail("AVIF.png-off Explorer/Shell menu order is incorrect");
  }

  params[0].install_avif_png_command = true;
  installed = install(test_exe, params);
  if (!installed) return fail(installed.error());
  auto reenabled_menu = create_context_menu({single_input});
  if (!reenabled_menu) return fail(reenabled_menu.error());
  HMENU reenabled_submenu = find_awj_submenu(*reenabled_menu);
  if (reenabled_submenu == nullptr ||
      submenu_labels(reenabled_submenu) != expected_labels(true)) {
    return fail("AVIF.png-on Explorer/Shell menu did not restore exact order");
  }

  return 0;
} catch (const std::exception& error) {
  return fail(error.what());
}
