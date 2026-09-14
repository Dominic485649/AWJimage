#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <shlwapi.h>

#include "shell_extension_contract.hpp"
#include "shell_extension_core.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr CLSID kClassId = {
    0x8829ea47,
    0x8f26,
    0x4670,
    {0xa9, 0x10, 0x34, 0x8d, 0x23, 0x40, 0xdd, 0xaa}};
constexpr wchar_t kOwnerValueName[] = L"AWJimage.Owner";
constexpr wchar_t kSchemaValueName[] = L"AWJimage.SchemaVersion";
constexpr wchar_t kOwnerValue[] = L"AWJimage";
constexpr DWORD kSchemaVersion = 5;
constexpr std::size_t kMaximumSelectionItems = 4096;
constexpr std::size_t kMaximumConfigurationBytes = 512 * 1024;

HINSTANCE g_module = nullptr;
std::atomic_ulong g_live_objects{0};
std::atomic_ulong g_server_locks{0};

class RegistryKey {
 public:
  RegistryKey() = default;
  explicit RegistryKey(HKEY key) noexcept : key_(key) {}
  RegistryKey(const RegistryKey&) = delete;
  RegistryKey& operator=(const RegistryKey&) = delete;
  RegistryKey(RegistryKey&& other) noexcept
      : key_(std::exchange(other.key_, nullptr)) {}
  RegistryKey& operator=(RegistryKey&& other) noexcept {
    if (this != &other) reset(std::exchange(other.key_, nullptr));
    return *this;
  }
  ~RegistryKey() { reset(); }
  HKEY get() const noexcept { return key_; }
  explicit operator bool() const noexcept { return key_ != nullptr; }
  void reset(HKEY key = nullptr) noexcept {
    if (key_ != nullptr) RegCloseKey(key_);
    key_ = key;
  }

 private:
  HKEY key_{};
};

class StorageMedium {
 public:
  StorageMedium() = default;
  StorageMedium(const StorageMedium&) = delete;
  StorageMedium& operator=(const StorageMedium&) = delete;
  ~StorageMedium() {
    if (valid_) ReleaseStgMedium(&value_);
  }
  STGMEDIUM* out() noexcept { return &value_; }
  STGMEDIUM& get() noexcept { return value_; }
  void mark_valid() noexcept { valid_ = true; }

 private:
  STGMEDIUM value_{};
  bool valid_{};
};

std::optional<RegistryKey> open_user_key(std::wstring_view path,
                                         REGSAM access = KEY_READ) {
  HKEY key = nullptr;
  const std::wstring path_storage{path};
  if (RegOpenKeyExW(HKEY_CURRENT_USER, path_storage.c_str(), 0, access, &key) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  return RegistryKey{key};
}

std::optional<std::vector<std::wstring>> read_configuration_value() {
  auto root = open_user_key(awj::shell_extension::contract::class_root);
  if (!root) return std::nullopt;
  const std::wstring value_name{
      awj::shell_extension::contract::configuration_value_name};
  DWORD bytes = 0;
  LSTATUS status = RegGetValueW(root->get(), nullptr, value_name.c_str(),
                                RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes);
  if (status != ERROR_SUCCESS || bytes < 2 * sizeof(wchar_t) ||
      bytes > kMaximumConfigurationBytes || bytes % sizeof(wchar_t) != 0) {
    return std::nullopt;
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
  status = RegGetValueW(root->get(), nullptr, value_name.c_str(),
                        RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &bytes);
  if (status != ERROR_SUCCESS || buffer.size() < 2 ||
      buffer[buffer.size() - 1] != L'\0' ||
      buffer[buffer.size() - 2] != L'\0') {
    return std::nullopt;
  }

  std::vector<std::wstring> values;
  const wchar_t* cursor = buffer.data();
  const wchar_t* const final_terminator = buffer.data() + buffer.size() - 1;
  while (cursor < final_terminator && *cursor != L'\0') {
    const wchar_t* terminator = std::find(cursor, final_terminator, L'\0');
    if (terminator == final_terminator) return std::nullopt;
    values.emplace_back(cursor, terminator);
    cursor = terminator + 1;
  }
  if (values.empty()) return std::nullopt;
  return values;
}

std::optional<awj::shell_extension::RuntimeConfiguration>
load_configuration() {
  auto encoded = read_configuration_value();
  if (!encoded) return std::nullopt;
  auto decoded = awj::shell_extension::decode_configuration(*encoded);
  if (!decoded) return std::nullopt;
  const DWORD attributes = GetFileAttributesW(decoded->executable.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return std::nullopt;
  }
  return std::move(*decoded);
}

HRESULT copy_wide_string(std::wstring_view value, char* destination,
                         UINT capacity) noexcept {
  if (destination == nullptr || capacity == 0) return E_INVALIDARG;
  if (value.size() + 1 > capacity) {
    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  }
  auto* output = reinterpret_cast<wchar_t*>(destination);
  std::memcpy(output, value.data(), value.size() * sizeof(wchar_t));
  output[value.size()] = L'\0';
  return S_OK;
}

HRESULT copy_ansi_string(std::wstring_view value, char* destination,
                         UINT capacity) noexcept {
  if (destination == nullptr || capacity == 0) return E_INVALIDARG;
  if (value.empty()) {
    destination[0] = '\0';
    return S_OK;
  }
  const int required = WideCharToMultiByte(
      CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
      nullptr, nullptr);
  if (required <= 0) {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ?
                                  ERROR_NO_UNICODE_TRANSLATION : error);
  }
  if (static_cast<UINT>(required) + 1u > capacity) {
    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  }
  const int written = WideCharToMultiByte(
      CP_ACP, 0, value.data(), static_cast<int>(value.size()), destination,
      required, nullptr, nullptr);
  if (written != required) {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ?
                                  ERROR_NO_UNICODE_TRANSLATION : error);
  }
  destination[written] = '\0';
  return S_OK;
}

bool insert_leaf(HMENU menu, UINT position, UINT id,
                 const std::wstring& label) {
  MENUITEMINFOW item{sizeof(item)};
  item.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE;
  item.fType = MFT_STRING;
  item.wID = id;
  item.dwTypeData = const_cast<wchar_t*>(label.c_str());
  return InsertMenuItemW(menu, position, TRUE, &item) != FALSE;
}

bool insert_submenu(HMENU menu, UINT position, HMENU submenu,
                    const std::wstring& label) {
  MENUITEMINFOW item{sizeof(item)};
  item.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_FTYPE;
  item.fType = MFT_STRING;
  item.hSubMenu = submenu;
  item.dwTypeData = const_cast<wchar_t*>(label.c_str());
  return InsertMenuItemW(menu, position, TRUE, &item) != FALSE;
}

std::optional<std::vector<awj::shell_extension::SelectionItem>>
read_shell_item_array(IShellItemArray* item_array) {
  if (item_array == nullptr) return std::nullopt;
  DWORD count = 0;
  if (FAILED(item_array->GetCount(&count)) || count == 0 ||
      count > kMaximumSelectionItems) {
    return std::nullopt;
  }

  std::vector<awj::shell_extension::SelectionItem> selection;
  selection.reserve(count);
  for (DWORD index = 0; index < count; ++index) {
    IShellItem* item = nullptr;
    if (FAILED(item_array->GetItemAt(index, &item)) || item == nullptr) {
      return std::nullopt;
    }
    PWSTR display_name = nullptr;
    const HRESULT name_result =
        item->GetDisplayName(SIGDN_FILESYSPATH, &display_name);
    item->Release();
    if (FAILED(name_result) || display_name == nullptr) {
      if (display_name != nullptr) CoTaskMemFree(display_name);
      return std::nullopt;
    }
    std::wstring path{display_name};
    CoTaskMemFree(display_name);
    if (path.empty() || path.size() >= 32767) return std::nullopt;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return std::nullopt;
    selection.push_back(
        {std::filesystem::path{std::move(path)},
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
  }
  if (!awj::shell_extension::is_supported_selection(selection)) {
    return std::nullopt;
  }
  return selection;
}

void release_explorer_commands(
    std::vector<IExplorerCommand*>& commands) noexcept {
  for (auto* command : commands) {
    if (command != nullptr) command->Release();
  }
  commands.clear();
}

class ExplorerCommandEnumerator final : public IEnumExplorerCommand {
 public:
  explicit ExplorerCommandEnumerator(
      std::vector<IExplorerCommand*> commands)
      : commands_(std::move(commands)) {
    ++g_live_objects;
  }
  ExplorerCommandEnumerator(const ExplorerCommandEnumerator&) = delete;
  ExplorerCommandEnumerator& operator=(const ExplorerCommandEnumerator&) =
      delete;

  STDMETHODIMP QueryInterface(REFIID interface_id, void** result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(interface_id, IID_IUnknown) ||
        IsEqualIID(interface_id, IID_IEnumExplorerCommand)) {
      *result = static_cast<IEnumExplorerCommand*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }

  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }

  STDMETHODIMP Next(ULONG count, IExplorerCommand** commands,
                    ULONG* fetched) override {
    if (commands == nullptr || count == 0 || (count != 1 && fetched == nullptr)) {
      return E_INVALIDARG;
    }
    if (fetched != nullptr) *fetched = 0;
    ULONG copied = 0;
    while (copied < count && cursor_ < commands_.size()) {
      commands[copied] = commands_[cursor_++];
      commands[copied]->AddRef();
      ++copied;
    }
    if (fetched != nullptr) *fetched = copied;
    return copied == count ? S_OK : S_FALSE;
  }

  STDMETHODIMP Skip(ULONG count) override {
    const auto remaining = commands_.size() - cursor_;
    if (count > remaining) {
      cursor_ = commands_.size();
      return S_FALSE;
    }
    cursor_ += count;
    return S_OK;
  }

  STDMETHODIMP Reset() override {
    cursor_ = 0;
    return S_OK;
  }

  STDMETHODIMP Clone(IEnumExplorerCommand** result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    std::vector<IExplorerCommand*> commands;
    try {
      commands.reserve(commands_.size());
      for (auto* command : commands_) {
        commands.push_back(command);
        command->AddRef();
      }
      auto* clone = new (std::nothrow) ExplorerCommandEnumerator(
          std::move(commands));
      if (clone == nullptr) {
        release_explorer_commands(commands);
        return E_OUTOFMEMORY;
      }
      clone->cursor_ = cursor_;
      *result = clone;
      return S_OK;
    } catch (const std::bad_alloc&) {
      release_explorer_commands(commands);
      return E_OUTOFMEMORY;
    } catch (...) {
      release_explorer_commands(commands);
      return E_FAIL;
    }
  }

 private:
  ~ExplorerCommandEnumerator() {
    release_explorer_commands(commands_);
    --g_live_objects;
  }

  std::atomic_ulong references_{1};
  std::vector<IExplorerCommand*> commands_{};
  std::size_t cursor_{};
};

class ContextMenu final : public IShellExtInit,
                          public IContextMenu,
                          public IExplorerCommand {
 public:
  ContextMenu() { ++g_live_objects; }
  explicit ContextMenu(awj::shell_extension::MenuCommand command,
                       std::filesystem::path executable)
      : explorer_command_(std::move(command)),
        explorer_executable_(std::move(executable)) {
    ++g_live_objects;
  }
  ContextMenu(const ContextMenu&) = delete;
  ContextMenu& operator=(const ContextMenu&) = delete;

  STDMETHODIMP QueryInterface(REFIID interface_id, void** result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(interface_id, IID_IUnknown) ||
        IsEqualIID(interface_id, IID_IContextMenu)) {
      *result = static_cast<IContextMenu*>(this);
    } else if (IsEqualIID(interface_id, IID_IShellExtInit)) {
      *result = static_cast<IShellExtInit*>(this);
    } else if (IsEqualIID(interface_id, IID_IExplorerCommand)) {
      *result = static_cast<IExplorerCommand*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }

  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }

  STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject* data_object,
                          HKEY) override {
    try {
      selection_.clear();
      commands_.clear();
      executable_.clear();
      command_id_first_ = 0;
      if (data_object == nullptr) return E_INVALIDARG;
      FORMATETC format{static_cast<CLIPFORMAT>(CF_HDROP), nullptr,
                      DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      StorageMedium medium;
      const HRESULT result = data_object->GetData(&format, medium.out());
      if (FAILED(result)) return result;
      medium.mark_valid();
      if (medium.get().tymed != TYMED_HGLOBAL ||
          medium.get().hGlobal == nullptr) {
        return DV_E_TYMED;
      }
      const auto drop = reinterpret_cast<HDROP>(medium.get().hGlobal);
      if (drop == nullptr) return E_INVALIDARG;

      const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
      if (count == 0 || count > kMaximumSelectionItems) return S_OK;
      selection_.reserve(count);
      for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0 || length >= 32767) {
          selection_.clear();
          return S_OK;
        }
        std::wstring path(length + 1, L'\0');
        if (DragQueryFileW(drop, index, path.data(), length + 1) != length) {
          selection_.clear();
          return S_OK;
        }
        path.resize(length);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
          selection_.clear();
          return S_OK;
        }
        selection_.push_back({std::filesystem::path{std::move(path)},
                              (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
      }
      if (!awj::shell_extension::is_supported_selection(selection_)) {
        selection_.clear();
      }
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  STDMETHODIMP QueryContextMenu(HMENU menu, UINT index_menu, UINT id_first,
                                UINT id_last, UINT flags) override {
    try {
      if ((flags & (CMF_DEFAULTONLY | CMF_NOVERBS)) != 0 ||
          selection_.empty()) {
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
      }
      auto configuration = load_configuration();
      if (!configuration || configuration->commands.empty()) {
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
      }
      if (id_last < id_first) return E_FAIL;
      const auto available =
          static_cast<std::uint64_t>(id_last) - id_first + 1;
      if (configuration->commands.size() > available) return E_FAIL;

      HMENU submenu = CreatePopupMenu();
      if (submenu == nullptr) return HRESULT_FROM_WIN32(GetLastError());
      UINT parent_position = 0;
      UINT leaf_index = 0;
      bool separator_inserted = false;
      std::wstring current_group;
      HMENU group_menu = nullptr;
      UINT group_position = 0;
      for (const auto& command : configuration->commands) {
        const bool grouped = !command.group_label.empty();
        HMENU target = submenu;
        UINT position = parent_position;
        if (grouped) {
          if (!separator_inserted && parent_position != 0) {
            if (!InsertMenuW(submenu, parent_position, MF_BYPOSITION | MF_SEPARATOR,
                             0, nullptr)) {
              const DWORD error = GetLastError();
              DestroyMenu(submenu);
              return HRESULT_FROM_WIN32(error);
            }
            ++parent_position;
            separator_inserted = true;
          }
          if (command.group_canonical_verb != current_group) {
            group_menu = CreatePopupMenu();
            if (group_menu == nullptr) {
              const DWORD error = GetLastError();
              DestroyMenu(submenu);
              return HRESULT_FROM_WIN32(error);
            }
            if (!insert_submenu(submenu, parent_position, group_menu,
                                command.group_label)) {
              const DWORD error = GetLastError();
              DestroyMenu(group_menu);
              DestroyMenu(submenu);
              return HRESULT_FROM_WIN32(error);
            }
            ++parent_position;
            group_position = 0;
            current_group = command.group_canonical_verb;
          }
          target = group_menu;
          position = group_position;
        }
        if (!insert_leaf(target, position, id_first + leaf_index,
                         command.label)) {
          const DWORD error = GetLastError();
          DestroyMenu(submenu);
          return HRESULT_FROM_WIN32(error);
        }
        if (grouped) {
          ++group_position;
        } else {
          ++parent_position;
        }
        ++leaf_index;
      }

      if (!insert_submenu(menu, index_menu, submenu,
                          configuration->menu_label)) {
        const DWORD error = GetLastError();
        DestroyMenu(submenu);
        return HRESULT_FROM_WIN32(error);
      }
      commands_ = std::move(configuration->commands);
      executable_ = std::move(configuration->executable);
      command_id_first_ = id_first;
      return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL,
                          static_cast<USHORT>(commands_.size()));
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  STDMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* command_info) override {
    try {
      if (command_info == nullptr ||
          command_info->cbSize < sizeof(*command_info)) {
        return E_INVALIDARG;
      }
      const auto command_index = resolve_command_index(command_info);
      if (!command_index || *command_index >= commands_.size()) {
        return E_INVALIDARG;
      }
      const auto command_line = awj::shell_extension::build_awj_command_line(
          executable_, commands_[*command_index].arguments, selection_);
      if (!command_line) return E_INVALIDARG;

      std::wstring mutable_command_line = *command_line;
      STARTUPINFOW startup{sizeof(startup)};
      startup.dwFlags = STARTF_USESHOWWINDOW;
      startup.wShowWindow = static_cast<WORD>(command_info->nShow);
      PROCESS_INFORMATION process{};
      const auto working_directory = executable_.parent_path().native();
      if (!CreateProcessW(executable_.c_str(), mutable_command_line.data(),
                          nullptr, nullptr, FALSE,
                          CREATE_DEFAULT_ERROR_MODE | CREATE_UNICODE_ENVIRONMENT,
                          nullptr,
                          working_directory.empty() ? nullptr
                                                    : working_directory.c_str(),
                          &startup, &process)) {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  STDMETHODIMP GetCommandString(UINT_PTR command_index, UINT flags, UINT*,
                                char* name, UINT maximum) override {
    if (command_index >= commands_.size()) return E_INVALIDARG;
    const auto& command = commands_[command_index];
    switch (flags) {
      case GCS_VERBW:
        return copy_wide_string(command.canonical_verb, name, maximum);
      case GCS_HELPTEXTW:
        return copy_wide_string(command.label, name, maximum);
      case GCS_VERBA:
        return copy_ansi_string(command.canonical_verb, name, maximum);
      case GCS_HELPTEXTA:
        return copy_ansi_string(command.label, name, maximum);
      case GCS_VALIDATEA:
      case GCS_VALIDATEW:
        return S_OK;
      default:
      return E_NOTIMPL;
    }
  }

  // Modern Windows 11 context menus use IExplorerCommand.  The same COM
  // object continues to expose IContextMenu for classic hosts such as
  // Directory Opus and Explorer's "Show more options" menu.
  STDMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override {
    if (name == nullptr) return E_POINTER;
    *name = nullptr;
    try {
      if (explorer_command_) {
        return SHStrDupW(explorer_command_->label.c_str(), name);
      }
      auto configuration = load_configuration();
      const std::wstring_view title =
          configuration ? std::wstring_view{configuration->menu_label}
                        : std::wstring_view{L"AWJimage 转换"};
      return SHStrDupW(std::wstring{title}.c_str(), name);
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  STDMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
    if (icon == nullptr) return E_POINTER;
    *icon = nullptr;
    try {
      std::filesystem::path executable = explorer_executable_;
      if (executable.empty()) {
        auto configuration = load_configuration();
        if (configuration) executable = configuration->executable;
      }
      if (executable.empty()) return S_FALSE;
      const std::wstring icon_path = executable.native() + L",0";
      const HRESULT result = SHStrDupW(icon_path.c_str(), icon);
      return SUCCEEDED(result) ? S_OK : result;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  STDMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tooltip) override {
    if (tooltip == nullptr) return E_POINTER;
    *tooltip = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetCanonicalName(GUID* name) override {
    if (name == nullptr) return E_POINTER;
    *name = GUID_NULL;
    return S_OK;
  }

  STDMETHODIMP GetState(IShellItemArray* item_array, BOOL ok_to_be_slow,
                        EXPCMDSTATE* state) override {
    if (state == nullptr) return E_POINTER;
    *state = ECS_HIDDEN;
    // Explorer uses a fast probe while constructing the modern menu.  Return
    // an enabled state for that probe; a later slow call performs the full
    // configuration and selection validation.
    if (!ok_to_be_slow) {
      *state = ECS_ENABLED;
      return S_OK;
    }

    try {
      auto configuration = load_configuration();
      if (!configuration || configuration->commands.empty()) return S_OK;
      if (explorer_command_ && explorer_executable_.empty()) return S_OK;
      if (explorer_command_) {
        const auto command = std::ranges::find_if(
            configuration->commands, [this](const auto& value) {
              return value.canonical_verb == explorer_command_->canonical_verb;
            });
        if (command == configuration->commands.end()) return S_OK;
      }
      // Explorer may probe the command before it has a concrete selection
      // (notably while constructing a folder/background menu).  A valid
      // command must remain visible during that probe; Invoke still validates
      // the actual selection before launching AWJ.
      if (item_array == nullptr) {
        *state = ECS_ENABLED;
        return S_OK;
      }
      auto selection = read_shell_item_array(item_array);
      if (!selection) return S_OK;
      *state = ECS_ENABLED;
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  STDMETHODIMP Invoke(IShellItemArray* item_array, IBindCtx*) override {
    if (!explorer_command_) return E_NOTIMPL;
    try {
      auto configuration = load_configuration();
      if (!configuration || configuration->commands.empty()) return E_FAIL;
      if (explorer_executable_.empty()) return E_FAIL;
      const auto command_it = std::ranges::find_if(
          configuration->commands, [this](const auto& command) {
            return command.canonical_verb == explorer_command_->canonical_verb;
          });
      if (command_it == configuration->commands.end()) return E_INVALIDARG;
      auto selection = read_shell_item_array(item_array);
      if (!selection) return E_INVALIDARG;
      const auto command_line = awj::shell_extension::build_awj_command_line(
          explorer_executable_, command_it->arguments, *selection);
      if (!command_line) return E_INVALIDARG;

      std::wstring mutable_command_line = *command_line;
      STARTUPINFOW startup{sizeof(startup)};
      startup.dwFlags = STARTF_USESHOWWINDOW;
      startup.wShowWindow = SW_SHOWNORMAL;
      PROCESS_INFORMATION process{};
      const auto working_directory = explorer_executable_.parent_path().native();
      if (!CreateProcessW(explorer_executable_.c_str(),
                          mutable_command_line.data(), nullptr, nullptr, FALSE,
                          CREATE_DEFAULT_ERROR_MODE | CREATE_UNICODE_ENVIRONMENT,
                          nullptr,
                          working_directory.empty() ? nullptr
                                                    : working_directory.c_str(),
                          &startup, &process)) {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  STDMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
    if (flags == nullptr) return E_POINTER;
    *flags = explorer_command_ ? ECF_DEFAULT : ECF_HASSUBCOMMANDS;
    return S_OK;
  }

  STDMETHODIMP EnumSubCommands(IEnumExplorerCommand** result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (explorer_command_) return E_NOTIMPL;
    std::vector<IExplorerCommand*> children;
    try {
      auto configuration = load_configuration();
      if (!configuration || configuration->commands.empty()) return E_NOTIMPL;

      children.reserve(configuration->commands.size());
      for (const auto& command : configuration->commands) {
        auto* child = new (std::nothrow)
            ContextMenu{command, configuration->executable};
        if (child == nullptr) {
          release_explorer_commands(children);
          return E_OUTOFMEMORY;
        }
        IExplorerCommand* interface_pointer = nullptr;
        const HRESULT query = child->QueryInterface(
            IID_IExplorerCommand,
            reinterpret_cast<void**>(&interface_pointer));
        child->Release();
        if (FAILED(query)) {
          release_explorer_commands(children);
          return query;
        }
        try {
          children.push_back(interface_pointer);
        } catch (...) {
          interface_pointer->Release();
          release_explorer_commands(children);
          throw;
        }
      }
      auto* enumerator = new (std::nothrow)
          ExplorerCommandEnumerator{std::move(children)};
      if (enumerator == nullptr) {
        release_explorer_commands(children);
        return E_OUTOFMEMORY;
      }
      *result = enumerator;
      return S_OK;
    } catch (const std::bad_alloc&) {
      release_explorer_commands(children);
      return E_OUTOFMEMORY;
    } catch (...) {
      release_explorer_commands(children);
      return E_FAIL;
    }
  }

 private:
  ~ContextMenu() { --g_live_objects; }

  std::optional<std::size_t> resolve_command_index(
      const CMINVOKECOMMANDINFO* command_info) const {
    if (command_info == nullptr) return std::nullopt;
    const bool unicode =
        command_info->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) &&
        (command_info->fMask & CMIC_MASK_UNICODE) != 0;
    std::wstring verb;
    const auto numeric_index = [this](ULONG_PTR value)
        -> std::optional<std::size_t> {
      if (value < commands_.size()) {
        return static_cast<std::size_t>(value);
      }
      if (command_id_first_ != 0 && value >= command_id_first_ &&
          value - command_id_first_ < commands_.size()) {
        return static_cast<std::size_t>(value - command_id_first_);
      }
      // Some Explorer-compatible aggregators pass the absolute menu id minus
      // their own one-based sentinel. Accept that form as well; the normal
      // direct-handler and absolute-id forms above remain unambiguous.
      if (command_id_first_ != 0 && value + 1 >= command_id_first_ &&
          value + 1 - command_id_first_ < commands_.size()) {
        return static_cast<std::size_t>(value + 1 - command_id_first_);
      }
      return std::nullopt;
    };
    if (unicode) {
      const auto* extended =
          reinterpret_cast<const CMINVOKECOMMANDINFOEX*>(command_info);
      const auto base_value =
          reinterpret_cast<ULONG_PTR>(command_info->lpVerb);
      if (base_value <= 0xFFFFu) {
        if (auto index = numeric_index(base_value)) return index;
      }
      const auto wide_value =
          reinterpret_cast<ULONG_PTR>(extended->lpVerbW);
      if (wide_value <= 0xFFFFu) {
        return numeric_index(wide_value);
      }
      verb = extended->lpVerbW;
    } else {
      const auto verb_value =
          reinterpret_cast<ULONG_PTR>(command_info->lpVerb);
      if (verb_value <= 0xFFFFu) {
        return numeric_index(verb_value);
      }
      const int length = MultiByteToWideChar(CP_ACP, 0, command_info->lpVerb,
                                             -1, nullptr, 0);
      if (length <= 1) return std::nullopt;
      verb.resize(static_cast<std::size_t>(length));
      if (MultiByteToWideChar(CP_ACP, 0, command_info->lpVerb, -1,
                              verb.data(), length) != length) {
        return std::nullopt;
      }
      verb.resize(static_cast<std::size_t>(length - 1));
    }
    for (std::size_t index = 0; index < commands_.size(); ++index) {
      if (CompareStringOrdinal(verb.c_str(), -1,
                               commands_[index].canonical_verb.c_str(), -1,
                               TRUE) == CSTR_EQUAL) {
        return index;
      }
    }
    return std::nullopt;
  }

  std::atomic_ulong references_{1};
  std::vector<awj::shell_extension::SelectionItem> selection_{};
  std::vector<awj::shell_extension::MenuCommand> commands_{};
  std::filesystem::path executable_{};
  UINT command_id_first_{};
  std::optional<awj::shell_extension::MenuCommand> explorer_command_{};
  std::filesystem::path explorer_executable_{};
};

class ClassFactory final : public IClassFactory {
 public:
  ClassFactory() { ++g_live_objects; }

  STDMETHODIMP QueryInterface(REFIID interface_id, void** result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (!IsEqualIID(interface_id, IID_IUnknown) &&
        !IsEqualIID(interface_id, IID_IClassFactory)) {
      return E_NOINTERFACE;
    }
    *result = static_cast<IClassFactory*>(this);
    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }

  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID interface_id,
                              void** result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (outer != nullptr) return CLASS_E_NOAGGREGATION;
    auto* instance = new (std::nothrow) ContextMenu;
    if (instance == nullptr) return E_OUTOFMEMORY;
    const HRESULT queried = instance->QueryInterface(interface_id, result);
    instance->Release();
    return queried;
  }

  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      ++g_server_locks;
    } else if (g_server_locks.load() != 0) {
      --g_server_locks;
    }
    return S_OK;
  }

 private:
  ~ClassFactory() { --g_live_objects; }
  std::atomic_ulong references_{1};
};

LSTATUS set_user_string(std::wstring_view path, const wchar_t* name,
                        std::wstring_view value) {
  HKEY raw = nullptr;
  const std::wstring path_storage{path};
  LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path_storage.c_str(), 0,
                                   nullptr, 0, KEY_READ | KEY_WRITE, nullptr,
                                   &raw, nullptr);
  if (status != ERROR_SUCCESS) return status;
  RegistryKey key{raw};
  const std::wstring value_storage{value};
  return RegSetValueExW(
      key.get(), name, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(value_storage.c_str()),
      static_cast<DWORD>((value_storage.size() + 1) * sizeof(wchar_t)));
}

LSTATUS set_user_dword(std::wstring_view path, const wchar_t* name,
                       DWORD value) {
  HKEY raw = nullptr;
  const std::wstring path_storage{path};
  LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path_storage.c_str(), 0,
                                   nullptr, 0, KEY_READ | KEY_WRITE, nullptr,
                                   &raw, nullptr);
  if (status != ERROR_SUCCESS) return status;
  RegistryKey key{raw};
  return RegSetValueExW(key.get(), name, 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

bool string_value_equals(std::wstring_view path, const wchar_t* name,
                         std::wstring_view expected) {
  const std::wstring path_storage{path};
  std::vector<wchar_t> buffer(expected.size() + 2, L'\0');
  DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  const LSTATUS status = RegGetValueW(
      HKEY_CURRENT_USER, path_storage.c_str(), name, RRF_RT_REG_SZ, nullptr,
      buffer.data(), &bytes);
  if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t) ||
      bytes % sizeof(wchar_t) != 0) {
    return false;
  }
  const std::size_t characters = bytes / sizeof(wchar_t);
  return characters == expected.size() + 1 &&
         buffer[characters - 1] == L'\0' &&
         std::wstring_view{buffer.data(), characters - 1} == expected;
}

LSTATUS delete_user_tree(std::wstring_view path) {
  const std::wstring path_storage{path};
  const LSTATUS tree = RegDeleteTreeW(HKEY_CURRENT_USER, path_storage.c_str());
  if (tree != ERROR_SUCCESS && tree != ERROR_FILE_NOT_FOUND &&
      tree != ERROR_PATH_NOT_FOUND) {
    return tree;
  }
  const LSTATUS key =
      RegDeleteKeyExW(HKEY_CURRENT_USER, path_storage.c_str(), 0, 0);
  if (key != ERROR_SUCCESS && key != ERROR_FILE_NOT_FOUND &&
      key != ERROR_PATH_NOT_FOUND) {
    return key;
  }
  return ERROR_SUCCESS;
}

LSTATUS delete_handler_if_owned(std::wstring_view path) {
  if (string_value_equals(path, nullptr,
                           awj::shell_extension::contract::class_id) &&
      string_value_equals(path, kOwnerValueName, kOwnerValue)) {
    return delete_user_tree(path);
  }
  return ERROR_SUCCESS;
}

bool handler_is_owned(std::wstring_view path) {
  return string_value_equals(path, nullptr,
                             awj::shell_extension::contract::class_id) &&
         string_value_equals(path, kOwnerValueName, kOwnerValue);
}

LSTATUS delete_class_if_owned() {
  if (string_value_equals(awj::shell_extension::contract::class_root,
                          kOwnerValueName, kOwnerValue)) {
    return delete_user_tree(awj::shell_extension::contract::class_root);
  }
  return ERROR_SUCCESS;
}

LSTATUS set_user_multi_string(std::wstring_view path, const wchar_t* name,
                              const std::vector<std::wstring>& values) {
  HKEY raw = nullptr;
  const std::wstring path_storage{path};
  LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path_storage.c_str(), 0,
                                   nullptr, 0, KEY_READ | KEY_WRITE, nullptr,
                                   &raw, nullptr);
  if (status != ERROR_SUCCESS) return status;
  RegistryKey key{raw};
  std::size_t characters = 1;
  for (const auto& value : values) {
    if (value.find(L'\0') != std::wstring::npos) return ERROR_INVALID_DATA;
    characters += value.size() + 1;
  }
  if (characters > (512u * 1024u) / sizeof(wchar_t)) {
    return ERROR_BUFFER_OVERFLOW;
  }
  std::vector<wchar_t> buffer;
  buffer.reserve(characters);
  for (const auto& value : values) {
    buffer.insert(buffer.end(), value.begin(), value.end());
    buffer.push_back(L'\0');
  }
  buffer.push_back(L'\0');
  return RegSetValueExW(
      key.get(), name, 0, REG_MULTI_SZ,
      reinterpret_cast<const BYTE*>(buffer.data()),
      static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
}

std::optional<std::filesystem::path> module_path() {
  std::vector<wchar_t> buffer(512);
  for (;;) {
    const DWORD copied = GetModuleFileNameW(g_module, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (copied == 0) return std::nullopt;
    if (copied < buffer.size()) {
      return std::filesystem::path{
          std::wstring_view{buffer.data(), static_cast<std::size_t>(copied)}};
    }
    if (buffer.size() >= 32768) return std::nullopt;
    buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
  }
}

HRESULT register_server() {
  try {
    const auto path = module_path();
    if (!path) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    const bool class_existed_owned =
        open_user_key(awj::shell_extension::contract::class_root).has_value() &&
        string_value_equals(awj::shell_extension::contract::class_root,
                            kOwnerValueName, kOwnerValue);
    if (open_user_key(awj::shell_extension::contract::class_root).has_value() &&
        !class_existed_owned) {
      return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }
    const bool file_handler_existed_owned =
        open_user_key(awj::shell_extension::contract::file_handler_root)
            .has_value() &&
        handler_is_owned(awj::shell_extension::contract::file_handler_root);
    const bool folder_handler_existed_owned =
        open_user_key(awj::shell_extension::contract::folder_handler_root)
            .has_value() &&
        handler_is_owned(awj::shell_extension::contract::folder_handler_root);
    for (const auto handler : {
             awj::shell_extension::contract::file_handler_root,
             awj::shell_extension::contract::folder_handler_root}) {
      if (open_user_key(handler).has_value() && !handler_is_owned(handler)) {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
      }
    }

    const std::wstring inproc =
        std::wstring{awj::shell_extension::contract::class_root} +
        L"\\InprocServer32";
    const auto write = [](std::wstring_view key, const wchar_t* name,
                          std::wstring_view value) -> HRESULT {
      const auto status = set_user_string(key, name, value);
      return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
    };
    HRESULT result = write(awj::shell_extension::contract::class_root,
                           kOwnerValueName, kOwnerValue);
    if (SUCCEEDED(result)) {
      const auto status = set_user_dword(
          awj::shell_extension::contract::class_root, kSchemaValueName,
          kSchemaVersion);
      result = status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
    }
    if (SUCCEEDED(result)) {
      result = write(awj::shell_extension::contract::class_root, nullptr,
                     awj::shell_extension::contract::friendly_name);
    }
    if (SUCCEEDED(result)) {
      result = write(
          awj::shell_extension::contract::class_root,
          awj::shell_extension::contract::context_menu_opt_in_value_name.data(),
          L"");
    }
    if (SUCCEEDED(result)) result = write(inproc, nullptr, path->native());
    if (SUCCEEDED(result)) result = write(inproc, L"ThreadingModel", L"Apartment");
    if (SUCCEEDED(result)) {
      const auto executable = path->parent_path() / L"AWJ.exe";
      const DWORD attributes = GetFileAttributesW(executable.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        auto configuration = awj::shell_extension::RuntimeConfiguration{
            .executable = executable,
            .menu_label = L"AWJimage 转换",
            .commands = awj::shell_extension::default_menu_commands()};
        auto encoded = awj::shell_extension::encode_configuration(configuration);
        if (!encoded) {
          result = E_FAIL;
        } else {
          const auto status = set_user_multi_string(
              awj::shell_extension::contract::class_root,
              awj::shell_extension::contract::configuration_value_name.data(),
              *encoded);
          result = status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
        }
      }
    }
    for (const auto handler : {
             awj::shell_extension::contract::file_handler_root,
             awj::shell_extension::contract::folder_handler_root}) {
      if (SUCCEEDED(result)) {
        result = write(handler, kOwnerValueName, kOwnerValue);
      }
      if (SUCCEEDED(result)) {
        const auto status = set_user_dword(handler, kSchemaValueName,
                                           kSchemaVersion);
        result = status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
      }
      if (SUCCEEDED(result)) {
        result = write(handler, nullptr,
                       awj::shell_extension::contract::class_id);
      }
    }
    if (FAILED(result)) {
      if (!file_handler_existed_owned) {
        delete_user_tree(awj::shell_extension::contract::file_handler_root);
      }
      if (!folder_handler_existed_owned) {
        delete_user_tree(awj::shell_extension::contract::folder_handler_root);
      }
      if (!class_existed_owned) {
        delete_user_tree(awj::shell_extension::contract::class_root);
      }
      return result;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

HRESULT unregister_server() {
  LSTATUS status = delete_handler_if_owned(
      awj::shell_extension::contract::file_handler_root);
  if (status == ERROR_SUCCESS) {
    status = delete_handler_if_owned(
        awj::shell_extension::contract::folder_handler_root);
  }
  if (status == ERROR_SUCCESS) status = delete_class_if_owned();
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
  return g_live_objects.load() == 0 && g_server_locks.load() == 0 ? S_OK
                                                                  : S_FALSE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID class_id,
                                                 REFIID interface_id,
                                                 void** result) {
  if (result == nullptr) return E_POINTER;
  *result = nullptr;
  if (!IsEqualCLSID(class_id, kClassId)) return CLASS_E_CLASSNOTAVAILABLE;
  auto* factory = new (std::nothrow) ClassFactory;
  if (factory == nullptr) return E_OUTOFMEMORY;
  const HRESULT queried = factory->QueryInterface(interface_id, result);
  factory->Release();
  return queried;
}

extern "C" HRESULT __stdcall DllRegisterServer() { return register_server(); }

extern "C" HRESULT __stdcall DllUnregisterServer() {
  return unregister_server();
}
