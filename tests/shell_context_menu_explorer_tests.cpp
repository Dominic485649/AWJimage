#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "shell_context_menu.hpp"

#include <array>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

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

}  // namespace

int wmain(int argc, wchar_t** argv) {
  using namespace awj::shell_context_menu;
  if (argc != 2) return fail("expected AWJ executable path argument");
  const std::filesystem::path awj_exe{argv[1]};
  if (!std::filesystem::is_regular_file(awj_exe)) return fail("AWJ executable is missing");

  ComApartment apartment;
  if (FAILED(apartment.status)) return fail("CoInitializeEx failed");

  struct RegistryCleanup {
    ~RegistryCleanup() { (void)awj::shell_context_menu::remove(); }
  } registry_cleanup;

  const auto root = std::filesystem::temp_directory_path() / L"AWJ Explorer API 验证 空格";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) return fail("failed to create Explorer API test root");
  struct TempCleanup {
    std::filesystem::path path;
    ~TempCleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } temp_cleanup{root};

  auto params = make_params(true);
  auto installed = install(awj_exe, params);
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

  params[0].install_avif_png_command = false;
  installed = install(awj_exe, params);
  if (!installed) return fail(installed.error());
  auto no_png_menu = create_context_menu({single_input});
  if (!no_png_menu) return fail(no_png_menu.error());
  HMENU no_png_submenu = find_awj_submenu(*no_png_menu);
  if (no_png_submenu == nullptr || submenu_labels(no_png_submenu) != expected_labels(false)) {
    return fail("AVIF.png-off Explorer/Shell menu order is incorrect");
  }

  params[0].install_avif_png_command = true;
  installed = install(awj_exe, params);
  if (!installed) return fail(installed.error());
  auto reenabled_menu = create_context_menu({single_input});
  if (!reenabled_menu) return fail(reenabled_menu.error());
  HMENU reenabled_submenu = find_awj_submenu(*reenabled_menu);
  if (reenabled_submenu == nullptr ||
      submenu_labels(reenabled_submenu) != expected_labels(true)) {
    return fail("AVIF.png-on Explorer/Shell menu did not restore exact order");
  }

  return 0;
}
