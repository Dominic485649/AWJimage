#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "shell_context_menu.hpp"
#include "shell_extension_contract.hpp"
#include "shell_extension_core.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
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

// Real Shell activation crosses the process boundary, so HKCU overrides are not
// sufficient here. Persist exactly the module's AWJ roots before any mutation.
struct RegistryRestore {
  std::vector<std::wstring> paths = awj::shell_context_menu::owned_root_keys();
  std::vector<bool> present;
  std::wstring backup = L"Software\\AWJimage.Tests.Backup\\" + std::to_wstring(GetCurrentProcessId());
  HKEY storage{};
  RegistryRestore() {
    HKEY pending{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\AWJimage.ContextMenu.v4.Transaction",
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
    RegFlushKey(storage);
  }
  ~RegistryRestore() {
    bool restored = true;
    for (std::size_t i = 0; i < paths.size(); ++i) {
      const auto removed = RegDeleteTreeW(HKEY_CURRENT_USER, paths[i].c_str());
      restored &= removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND || removed == ERROR_PATH_NOT_FOUND;
      if (!present[i]) continue;
      HKEY source{}, destination{};
      if (RegOpenKeyExW(storage, std::to_wstring(i).c_str(), 0, KEY_READ, &source) != ERROR_SUCCESS ||
          RegCreateKeyExW(HKEY_CURRENT_USER, paths[i].c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                         nullptr, &destination, nullptr) != ERROR_SUCCESS) {
        restored = false;
      } else restored &= RegCopyTreeW(source, nullptr, destination) == ERROR_SUCCESS;
      if (source) RegCloseKey(source);
      if (destination) RegCloseKey(destination);
    }
    RegCloseKey(storage);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (!restored) {
      std::fwprintf(stderr, L"Registry restoration failed; snapshot retained at HKCU\\%ls\n", backup.c_str());
      std::abort();
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, backup.c_str());
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

HMENU find_awj_submenu(ContextMenu& context_menu,
                       const std::vector<std::wstring>& expected = {}) {
  const int count = GetMenuItemCount(context_menu.menu);
  HMENU first_match = nullptr;
  for (int i = 0; i < count; ++i) {
    if (menu_text(context_menu.menu, i) != L"AWJimage 转换") continue;
    HMENU submenu = GetSubMenu(context_menu.menu, i);
    if (submenu != nullptr) initialize_popup(context_menu, submenu, i);
    if (first_match == nullptr) first_match = submenu;
    if (!expected.empty() && submenu_labels(submenu) == expected) return submenu;
  }
  return first_match;
}

HMENU child_submenu_with_label(HMENU menu, std::wstring_view label) {
  const int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    if (menu_text(menu, i) == label) return GetSubMenu(menu, i);
  }
  return nullptr;
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
  if (FAILED(status)) return std::unexpected{"IContextMenu::InvokeCommand failed for index " +
      std::to_string(command_index) + ": " + std::to_string(status)};
  const std::wstring extension = spec.append_png_suffix ? L".avif.png"
      : spec.format == L"jpgli" ? L".jpg" : L"." + std::wstring{spec.format};
  // JPEGli initialization can be noticeably slower on a cold process; shell
  // invocation is asynchronous, so allow enough time for the child window to
  // finish without weakening the output validation below.
  const auto deadline = GetTickCount64() + 120000;
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
    if (GetTickCount64() >= deadline) return std::unexpected{"Shell output missing for index " +
        std::to_string(command_index) + " or not decodable: " + last_failure};
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

std::expected<bool, std::string> current_process_is_standard_user() {
  SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
  PSID administrators = nullptr;
  if (!AllocateAndInitializeSid(
          &nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
          DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators)) {
    return std::unexpected{"could not allocate Administrators SID"};
  }
  BOOL administrator_enabled = FALSE;
  const BOOL membership =
      CheckTokenMembership(nullptr, administrators, &administrator_enabled);
  FreeSid(administrators);
  if (!membership) {
    return std::unexpected{"could not inspect Administrators membership"};
  }

  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return std::unexpected{"could not open process token for integrity check"};
  }
  DWORD bytes = 0;
  (void)GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
  std::vector<std::byte> buffer(bytes);
  const BOOL queried = bytes != 0 &&
      GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), bytes,
                          &bytes);
  CloseHandle(token);
  if (!queried) {
    return std::unexpected{"could not query process integrity level"};
  }
  const auto* label =
      reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
  const DWORD subauthorities = *GetSidSubAuthorityCount(label->Label.Sid);
  if (subauthorities == 0) {
    return std::unexpected{"process integrity SID is invalid"};
  }
  const DWORD integrity =
      *GetSidSubAuthority(label->Label.Sid, subauthorities - 1);
  return administrator_enabled == FALSE &&
         integrity <= SECURITY_MANDATORY_MEDIUM_RID;
}

int run_shell_scenarios(const std::filesystem::path& awj_exe,
                        const std::filesystem::path& root) {
  using namespace awj::shell_context_menu;
  auto standard_user = current_process_is_standard_user();
  if (!standard_user) return fail(standard_user.error());
  if (!*standard_user) {
    return fail("Explorer scenarios did not run as a standard user");
  }
  ComApartment apartment;
  if (FAILED(apartment.status)) return fail("CoInitializeEx failed");
  CLSID class_id{};
  const std::wstring class_id_text{
      awj::shell_extension::contract::class_id};
  if (FAILED(CLSIDFromString(class_id_text.c_str(), &class_id))) {
    return fail("formal shell extension CLSID is invalid");
  }
  IContextMenu* activated_raw = nullptr;
  const HRESULT activation = CoCreateInstance(
      class_id, nullptr, CLSCTX_INPROC_SERVER, IID_IContextMenu,
      reinterpret_cast<void**>(&activated_raw));
  if (FAILED(activation) || activated_raw == nullptr) {
    return fail("registered shell extension CoCreateInstance failed: " +
                std::to_string(static_cast<unsigned long>(activation)));
  }
  ComPtr<IContextMenu> activated{activated_raw};
  std::error_code ec;
  auto params = make_params(true);

  const auto single_dir = root / L"single 菜单";
  std::filesystem::create_directories(single_dir, ec);
  const auto single_input = single_dir / L"输入 单图 Ω.bmp";
  if (ec || !write_one_pixel_bmp(single_input)) return fail("failed to create single input");
  auto single_menu = create_context_menu({single_input});
  if (!single_menu) return fail(single_menu.error());
  HMENU single_submenu = find_awj_submenu(*single_menu, expected_labels(true));
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
  HMENU multi_submenu = find_awj_submenu(*multi_menu, expected_labels(true));
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
  HMENU folder_submenu = find_awj_submenu(*folder_menu, expected_labels(true));
  if (folder_submenu == nullptr || submenu_labels(folder_submenu) != expected_labels(true)) {
    return fail("folder Explorer/Shell menu order is incorrect");
  }
  // Directory shell output is beside the selected directory.
  for (std::size_t i = 0; i < command_specs().size(); ++i) {
    if (auto result = invoke_and_decode(*folder_menu, folder_submenu, {folder_bmp}, root, i, true); !result)
      return fail(result.error());
  }

  params[0].install_avif_png_command = false;
  auto installed = install(awj_exe, params);
  if (!installed) return fail(installed.error());
  auto no_png_menu = create_context_menu({single_input});
  if (!no_png_menu) return fail(no_png_menu.error());
  HMENU no_png_submenu = find_awj_submenu(*no_png_menu, expected_labels(false));
  if (no_png_submenu == nullptr || submenu_labels(no_png_submenu) != expected_labels(false)) {
    return fail("AVIF.png-off Explorer/Shell menu order is incorrect");
  }

  params[0].install_avif_png_command = true;
  installed = install(awj_exe, params);
  if (!installed) return fail(installed.error());
  auto reenabled_menu = create_context_menu({single_input});
  if (!reenabled_menu) return fail(reenabled_menu.error());
  HMENU reenabled_submenu = find_awj_submenu(*reenabled_menu, expected_labels(true));
  if (reenabled_submenu == nullptr ||
      submenu_labels(reenabled_submenu) != expected_labels(true)) {
    return fail("AVIF.png-on Explorer/Shell menu did not restore exact order");
  }

  const auto unsupported_input = single_dir / L"unsupported.txt";
  {
    std::ofstream unsupported{unsupported_input};
    unsupported << "not an image";
  }
  auto unsupported_menu = create_context_menu({unsupported_input});
  if (!unsupported_menu) return fail(unsupported_menu.error());
  if (find_awj_submenu(*unsupported_menu) != nullptr) {
    return fail("unsupported file received an AWJ shell menu");
  }
  auto mixed_menu = create_context_menu({single_input, unsupported_input});
  if (!mixed_menu) return fail(mixed_menu.error());
  if (find_awj_submenu(*mixed_menu) != nullptr) {
    return fail("mixed supported/unsupported selection received an AWJ shell menu");
  }

  const std::vector<std::wstring> presets{L"网页 预设 Ω"};
  installed = install(awj_exe, params, presets);
  if (!installed) return fail(installed.error());
  auto preset_menu = create_context_menu({single_input});
  if (!preset_menu) return fail(preset_menu.error());
  auto top_labels = expected_labels(true);
  top_labels.push_back(presets.front());
  HMENU preset_parent = find_awj_submenu(*preset_menu, top_labels);
  if (preset_parent == nullptr || submenu_labels(preset_parent) != top_labels) {
    return fail("preset group is missing from the COM shell menu");
  }
  HMENU preset_submenu = child_submenu_with_label(preset_parent, presets.front());
  if (preset_submenu == nullptr ||
      submenu_labels(preset_submenu) != expected_labels(false)) {
    return fail("preset COM submenu order is incorrect");
  }

  auto removed = remove();
  if (!removed) return fail(removed.error());
  auto still_installed = is_installed();
  if (!still_installed) return fail(still_installed.error());
  if (*still_installed) {
    return fail("normal-user shell extension removal was incomplete");
  }

  return 0;
}

std::optional<std::filesystem::path> current_executable_path() {
  std::vector<wchar_t> buffer(512);
  for (;;) {
    const DWORD copied = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) return std::nullopt;
    if (copied < buffer.size()) {
      return std::filesystem::path{
          std::wstring_view{buffer.data(), static_cast<std::size_t>(copied)}};
    }
    if (buffer.size() >= 32768) return std::nullopt;
    buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
  }
}

int launch_child(std::wstring_view mode,
                 const std::filesystem::path& awj_exe,
                 const std::filesystem::path& root) {
  const auto launcher = current_executable_path();
  if (!launcher) return fail("could not locate Explorer test executable");
  std::wstring command_line;
  for (const auto& argument :
       std::array<std::wstring, 4>{launcher->native(), std::wstring{mode},
                                   awj_exe.native(), root.native()}) {
    if (!command_line.empty()) command_line.push_back(L' ');
    command_line += awj::shell_extension::quote_windows_argument(argument, true);
  }
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  auto standard_user = current_process_is_standard_user();
  if (!standard_user) return fail(standard_user.error());
  HANDLE token = nullptr;
  BOOL created = FALSE;
  if (*standard_user) {
    created = CreateProcessW(launcher->c_str(), command_line.data(), nullptr,
                             nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                             nullptr, nullptr, &startup, &process);
  } else {
    HANDLE process_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY |
                              TOKEN_ADJUST_DEFAULT,
                          &process_token)) {
      return fail("could not open process token for normal-user test");
    }
    const BOOL restricted = CreateRestrictedToken(
        process_token, LUA_TOKEN, 0, nullptr, 0, nullptr, 0, nullptr, &token);
    CloseHandle(process_token);
    if (!restricted || token == nullptr) {
      if (token != nullptr) CloseHandle(token);
      return fail("could not create LUA test token");
    }
    SID_IDENTIFIER_AUTHORITY mandatory_authority =
        SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID medium_integrity = nullptr;
    if (!AllocateAndInitializeSid(
            &mandatory_authority, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0,
            0, 0, 0, 0, &medium_integrity)) {
      CloseHandle(token);
      return fail("could not allocate medium-integrity SID");
    }
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = medium_integrity;
    const BOOL integrity_set = SetTokenInformation(
        token, TokenIntegrityLevel, &label,
        static_cast<DWORD>(sizeof(label) + GetLengthSid(medium_integrity)));
    FreeSid(medium_integrity);
    if (!integrity_set) {
      const DWORD integrity_error = GetLastError();
      CloseHandle(token);
      return fail("could not set medium integrity on LUA test token: " +
                  std::to_string(integrity_error));
    }
    created = CreateProcessAsUserW(
        token, launcher->c_str(), command_line.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process);
  }
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
  if (token != nullptr) CloseHandle(token);
  if (!created) {
    return fail("could not launch fresh normal-user test process: " +
                std::to_string(create_error));
  }
  CloseHandle(process.hThread);
  const DWORD waited = WaitForSingleObject(process.hProcess, 15 * 60 * 1000);
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    return fail("fresh normal-user test process timed out");
  }
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process.hProcess, &exit_code)) exit_code = 1;
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
}

}  // namespace

int wmain(int argc, wchar_t** argv) try {
  using namespace awj::shell_context_menu;
  if (argc == 4 &&
      (std::wstring_view{argv[1]} == L"--install-child" ||
       std::wstring_view{argv[1]} == L"--shell-child" ||
       std::wstring_view{argv[1]} == L"--persist-install-child" ||
       std::wstring_view{argv[1]} == L"--persist-remove-child")) {
    const std::filesystem::path awj_exe{argv[2]};
    const std::filesystem::path root{argv[3]};
    if (!std::filesystem::is_regular_file(awj_exe)) {
      return fail("fresh-process AWJ executable is missing");
    }
    auto standard_user = current_process_is_standard_user();
    if (!standard_user) return fail(standard_user.error());
    if (!*standard_user) {
      return fail("shell integration child retained administrator access");
    }
    if (std::wstring_view{argv[1]} == L"--persist-remove-child") {
      auto removed = remove();
      if (!removed) return fail(removed.error());
      return 0;
    }
    if (std::wstring_view{argv[1]} == L"--install-child" ||
        std::wstring_view{argv[1]} == L"--persist-install-child") {
      auto installed = install(awj_exe, make_params(true));
      if (!installed) return fail(installed.error());
      if (std::wstring_view{argv[1]} == L"--persist-install-child") return 0;
      return launch_child(L"--shell-child", awj_exe, root);
    }
    return run_shell_scenarios(awj_exe, root);
  }
  if (argc == 3 &&
      (std::wstring_view{argv[1]} == L"--persist-install" ||
       std::wstring_view{argv[1]} == L"--persist-remove")) {
    const std::filesystem::path awj_exe{argv[2]};
    if (!std::filesystem::is_regular_file(awj_exe)) {
      return fail("persistent integration AWJ executable is missing");
    }
    const auto child_mode = std::wstring_view{argv[1]} == L"--persist-install"
                                ? L"--persist-install-child"
                                : L"--persist-remove-child";
    return launch_child(child_mode, awj_exe, awj_exe.parent_path());
  }
  if (argc != 2) return fail("expected AWJ executable path argument");
  const std::filesystem::path awj_exe{argv[1]};
  if (!std::filesystem::is_regular_file(awj_exe)) {
    return fail("AWJ executable is missing");
  }

  RegistryRestore registry_cleanup;
  const auto root = std::filesystem::temp_directory_path() /
      (L"AWJ Explorer API 验证 空格 " +
       std::to_wstring(GetCurrentProcessId()) + L"-" +
       std::to_wstring(GetTickCount64()));
  std::error_code ec;
  if (!std::filesystem::create_directory(root, ec) || ec) {
    return fail("failed to create isolated Explorer API test root");
  }
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
  if (ec || !std::filesystem::copy_file(awj_exe, test_exe, ec) || ec) {
    return fail("failed to copy the test executable into its isolated profile");
  }
  const auto source_extension = shell_extension_path(awj_exe);
  const auto test_extension = shell_extension_path(test_exe);
  ec.clear();
  if (!std::filesystem::copy_file(source_extension, test_extension, ec) || ec) {
    return fail("failed to copy the shell extension into its isolated profile");
  }
  return launch_child(L"--install-child", test_exe, root);
} catch (const std::exception& error) {
  return fail(error.what());
}
