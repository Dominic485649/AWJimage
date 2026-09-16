#include "shell_context_menu.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "menu_transaction_state.hpp"
#include <objbase.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shlwapi.h>

#include <algorithm>
#include <format>
#include <cwctype>
#include <map>
#include <set>
#include <utility>

namespace awj::shell_context_menu {
namespace {

thread_local HKEY native_hive{HKEY_CURRENT_USER};
thread_local REGSAM native_view{};
thread_local std::wstring staged_id;
thread_local bool staged_machine{};
thread_local bool defer_commit{};

struct StageScope {
  bool previous{defer_commit};
  StageScope() { defer_commit = true; }
  ~StageScope() { defer_commit = previous; staged_id.clear(); staged_machine = false; }
};

struct NativeRegistryScope {
  HKEY hive{native_hive};
  REGSAM view{native_view};
  explicit NativeRegistryScope(bool machine) {
    native_hive = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    native_view = machine ? KEY_WOW64_64KEY : 0;
  }
  ~NativeRegistryScope() {
    native_hive = hive;
    native_view = view;
  }
};

LSTATUS open_native_key(const wchar_t* path, REGSAM access, HKEY* key) {
  return RegOpenKeyExW(native_hive, path, 0, access | native_view, key);
}

LSTATUS get_native_value(const wchar_t* path, const wchar_t* name, DWORD flags,
                        DWORD* type, void* data, DWORD* bytes) {
  HKEY key{};
  const auto opened = open_native_key(path, KEY_QUERY_VALUE, &key);
  if (opened != ERROR_SUCCESS) return opened;
  const auto result = RegGetValueW(key, nullptr, name, flags, type, data, bytes);
  RegCloseKey(key);
  return result;
}

LSTATUS delete_native_tree(const std::wstring& path) {
  HKEY key{};
  auto status = open_native_key(path.c_str(), KEY_READ | KEY_WRITE | DELETE, &key);
  if (status != ERROR_SUCCESS) return status;
  status = RegDeleteTreeW(key, nullptr);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS) return status;
  return RegDeleteKeyExW(native_hive, path.c_str(), native_view, 0);
}

constexpr std::wstring_view kImageParent =
    L"Software\\Classes\\SystemFileAssociations\\image\\shell\\AWJimage.Convert";
constexpr std::wstring_view kDirectoryParent =
    L"Software\\Classes\\Directory\\shell\\AWJimage.Convert";
constexpr std::wstring_view kIcoParent =
    L"Software\\Classes\\icofile\\shell\\AWJimage.Convert";
// Windows 10.0.26200 Shell was empirically verified to materialize a real
// cascade when ExtendedSubCommandsKey is a REG_SZ pointer to this shared tree.
// Current Microsoft Learn documents the child-key form instead, so this is a
// Windows compatibility contract, not a claim about documented behavior.
constexpr std::wstring_view kSharedTree =
    L"Software\\Classes\\AWJimage.ContextMenu.v4.A";
constexpr std::wstring_view kSharedTreeV3 = L"Software\\Classes\\AWJimage.ContextMenu.v3";
constexpr std::wstring_view kTransaction = L"Software\\Classes\\AWJimage.ContextMenu.v5.Transaction";
constexpr std::wstring_view kLegacyTransactionV4 =
    L"Software\\Classes\\AWJimage.ContextMenu.v4.Transaction";
constexpr std::wstring_view kLegacySharedTreeV2 =
    L"Software\\Classes\\AWJimage.ContextMenu.v2";
constexpr std::wstring_view kLegacyImageParent =
    L"Software\\Classes\\SystemFileAssociations\\image\\shell\\AWJImage";
constexpr std::wstring_view kLegacyIcoFileParent =
    L"Software\\Classes\\icofile\\shell\\AWJImage";
constexpr std::wstring_view kLegacyDirectoryParent =
    L"Software\\Classes\\Directory\\shell\\AWJImage";
constexpr std::wstring_view kLegacySharedTree =
    L"Software\\Classes\\AWJImage.ContextMenu";
constexpr std::wstring_view kMenuLabel = L"AWJimage 转换";
constexpr std::wstring_view kMultiSelectModel = L"Player";
constexpr std::wstring_view kExtendedSubCommandsKey = L"ExtendedSubCommandsKey";

constexpr std::wstring_view kSupportedExtensions[] = {
    L".jpg",    L".jpeg", L".jpe", L".jfif", L".png",  L".webp",
    L".bmp",    L".dib",  L".rle", L".ico",  L".tif",  L".tiff",
    L".gif",    L".jxl",  L".avif", L".awsraw", L".dng", L".cr2",
    L".cr3",    L".nef",  L".arw", L".rw2",  L".orf",  L".raf",
    L".pef",    L".srw",  L".x3f", L".3fr",  L".erf",  L".kdc",
    L".mrw",    L".raw",  L".heic", L".heif", L".jxr",  L".wdp",
    L".hdp"};

constexpr CommandSpec kCommands[] = {
    {.canonical_verb = L"AWJimage.Convert.10.png", .label = L"转换为 PNG", .format = L"png", .params_index = 4},
    {.canonical_verb = L"AWJimage.Convert.20.webp", .label = L"转换为 WebP", .format = L"webp", .params_index = 1},
    {.canonical_verb = L"AWJimage.Convert.30.avif", .label = L"转换为 AVIF", .format = L"avif", .params_index = 0},
    {.canonical_verb = L"AWJimage.Convert.40.avif-png", .label = L"转换为 AVIF.png", .format = L"avif", .params_index = 0, .append_png_suffix = true},
    {.canonical_verb = L"AWJimage.Convert.50.jxl", .label = L"转换为 JXL", .format = L"jxl", .params_index = 2},
    {.canonical_verb = L"AWJimage.Convert.60.jpgli", .label = L"转换为 JPGLI", .format = L"jpgli", .params_index = 3},
};

bool missing_registry_status(LSTATUS status) noexcept {
  return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

std::string narrow_ascii(std::wstring_view value) {
  std::string result;
  result.reserve(value.size());
  for (const wchar_t ch : value) {
    result.push_back(ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
  }
  return result;
}

std::string registry_error(std::string_view operation, std::wstring_view key,
                           LSTATUS status) {
  return std::format("{}失败：{}，错误码 {}。", operation, narrow_ascii(key), status);
}

class RegistryKey {
 public:
  RegistryKey() = default;
  explicit RegistryKey(HKEY key) noexcept : key_(key) {}
  RegistryKey(const RegistryKey&) = delete;
  RegistryKey& operator=(const RegistryKey&) = delete;
  RegistryKey(RegistryKey&& other) noexcept : key_(std::exchange(other.key_, nullptr)) {}
  RegistryKey& operator=(RegistryKey&& other) noexcept {
    if (this != &other) {
      reset();
      key_ = std::exchange(other.key_, nullptr);
    }
    return *this;
  }
  ~RegistryKey() { reset(); }
  HKEY get() const noexcept { return key_; }
  void reset() noexcept {
    if (key_ != nullptr) {
      RegCloseKey(key_);
      key_ = nullptr;
    }
  }

 private:
  HKEY key_{};
};

std::expected<RegistryKey, std::string> create_key(std::wstring_view subkey,
                                                  REGSAM access = KEY_READ | KEY_WRITE) {
  HKEY raw = nullptr;
  const std::wstring path{subkey};
  const auto status = RegCreateKeyExW(native_hive, path.c_str(), 0, nullptr,
          REG_OPTION_NON_VOLATILE, access | native_view, nullptr, &raw, nullptr);
  if (status != ERROR_SUCCESS) {
    return std::unexpected{registry_error("创建右键菜单注册表项", subkey, status)};
  }
  return RegistryKey{raw};
}

std::expected<RegistryKey, std::string> open_key(std::wstring_view subkey,
                                                 REGSAM access) {
  HKEY raw = nullptr;
  const std::wstring path{subkey};
  const auto status = open_native_key(path.c_str(), access, &raw);
  if (status != ERROR_SUCCESS) {
    return std::unexpected{registry_error("打开右键菜单注册表项", subkey, status)};
  }
  return RegistryKey{raw};
}

std::expected<void, std::string> set_string(std::wstring_view subkey,
                                            std::wstring_view name,
                                            std::wstring_view value) {
  auto key = create_key(subkey);
  if (!key) return std::unexpected{key.error()};
  const std::wstring name_storage{name};
  const std::wstring value_storage{value};
  const auto bytes = static_cast<DWORD>((value_storage.size() + 1) * sizeof(wchar_t));
  const auto status = RegSetValueExW(
      key->get(), name.empty() ? nullptr : name_storage.c_str(), 0, REG_SZ,
      reinterpret_cast<const BYTE*>(value_storage.c_str()), bytes);
  if (status != ERROR_SUCCESS) {
    return std::unexpected{registry_error("写入右键菜单字符串值", subkey, status)};
  }
  return {};
}

std::expected<void, std::string> set_dword(std::wstring_view subkey,
                                           std::wstring_view name,
                                           std::uint32_t value) {
  auto key = create_key(subkey);
  if (!key) return std::unexpected{key.error()};
  const std::wstring name_storage{name};
  const DWORD data = value;
  const auto status = RegSetValueExW(
      key->get(), name_storage.c_str(), 0, REG_DWORD,
      reinterpret_cast<const BYTE*>(&data), sizeof(data));
  if (status != ERROR_SUCCESS) {
    return std::unexpected{registry_error("写入右键菜单 DWORD 值", subkey, status)};
  }
  return {};
}

std::expected<bool, std::string> key_exists(std::wstring_view subkey) {
  HKEY raw = nullptr;
  const std::wstring path{subkey};
  const auto status = open_native_key(path.c_str(), KEY_READ, &raw);
  if (status == ERROR_SUCCESS) {
    RegCloseKey(raw);
    return true;
  }
  if (missing_registry_status(status)) return false;
  return std::unexpected{registry_error("检查右键菜单注册表项", subkey, status)};
}

std::expected<void, std::string> delete_tree(std::wstring_view subkey) {
  const std::wstring path{subkey};
  const auto status = delete_native_tree(path);
  if (status == ERROR_SUCCESS || missing_registry_status(status)) return {};
  return std::unexpected{registry_error("删除右键菜单注册表项", subkey, status)};
}

std::expected<std::optional<std::wstring>, std::string> read_string(
    std::wstring_view subkey, std::wstring_view name) {
  const std::wstring path{subkey};
  const std::wstring name_storage{name};
  DWORD bytes = 0;
  DWORD type = 0;
  const auto first = get_native_value(path.c_str(),
                                  name.empty() ? nullptr : name_storage.c_str(),
                                  RRF_RT_REG_SZ, &type, nullptr, &bytes);
  if (missing_registry_status(first)) return std::optional<std::wstring>{};
  if (first != ERROR_SUCCESS) {
    return std::unexpected{registry_error("读取右键菜单字符串值", subkey, first)};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  const auto second = get_native_value(path.c_str(),
                                   name.empty() ? nullptr : name_storage.c_str(),
                                   RRF_RT_REG_SZ, &type, value.data(), &bytes);
  if (second != ERROR_SUCCESS) {
    return std::unexpected{registry_error("读取右键菜单字符串值", subkey, second)};
  }
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  return std::optional<std::wstring>{std::move(value)};
}

std::expected<std::optional<std::uint32_t>, std::string> read_dword(
    std::wstring_view subkey, std::wstring_view name) {
  const std::wstring path{subkey};
  const std::wstring name_storage{name};
  DWORD value = 0;
  DWORD bytes = sizeof(value);
  DWORD type = 0;
  const auto status = get_native_value(path.c_str(), name_storage.c_str(),
                                   RRF_RT_REG_DWORD, &type, &value, &bytes);
  if (missing_registry_status(status)) return std::optional<std::uint32_t>{};
  if (status != ERROR_SUCCESS) {
    return std::unexpected{registry_error("读取右键菜单 DWORD 值", subkey, status)};
  }
  return std::optional<std::uint32_t>{value};
}

std::expected<std::vector<std::wstring>, std::string> child_keys(
    std::wstring_view subkey) {
  auto key = open_key(subkey, KEY_READ | KEY_ENUMERATE_SUB_KEYS);
  if (!key) return std::unexpected{key.error()};
  DWORD max_name = 0;
  DWORD count = 0;
  const auto info = RegQueryInfoKeyW(key->get(), nullptr, nullptr, nullptr, &count,
                                     &max_name, nullptr, nullptr, nullptr, nullptr,
                                     nullptr, nullptr);
  if (info != ERROR_SUCCESS) {
    return std::unexpected{registry_error("枚举右键菜单子项", subkey, info)};
  }
  std::vector<std::wstring> names;
  names.reserve(count);
  std::vector<wchar_t> buffer(static_cast<std::size_t>(max_name) + 1, L'\0');
  for (DWORD index = 0; index < count; ++index) {
    DWORD length = max_name + 1;
    const auto status = RegEnumKeyExW(key->get(), index, buffer.data(), &length,
                                      nullptr, nullptr, nullptr, nullptr);
    if (status != ERROR_SUCCESS) {
      return std::unexpected{registry_error("枚举右键菜单子项", subkey, status)};
    }
    names.emplace_back(buffer.data(), length);
  }
  std::ranges::sort(names);
  return names;
}

std::expected<std::vector<std::wstring>, std::string> value_names(
    std::wstring_view subkey) {
  auto key = open_key(subkey, KEY_READ | KEY_QUERY_VALUE);
  if (!key) return std::unexpected{key.error()};
  DWORD max_name = 0;
  DWORD count = 0;
  const auto info = RegQueryInfoKeyW(key->get(), nullptr, nullptr, nullptr, nullptr,
                                     nullptr, nullptr, &count, &max_name, nullptr,
                                     nullptr, nullptr);
  if (info != ERROR_SUCCESS) {
    return std::unexpected{registry_error("枚举右键菜单值", subkey, info)};
  }
  std::vector<std::wstring> names;
  names.reserve(count);
  std::vector<wchar_t> buffer(static_cast<std::size_t>(max_name) + 1, L'\0');
  for (DWORD index = 0; index < count; ++index) {
    DWORD length = max_name + 1;
    const auto status = RegEnumValueW(key->get(), index, buffer.data(), &length,
                                      nullptr, nullptr, nullptr, nullptr);
    if (status != ERROR_SUCCESS) {
      return std::unexpected{registry_error("枚举右键菜单值", subkey, status)};
    }
    names.emplace_back(buffer.data(), length);
  }
  std::ranges::sort(names);
  return names;
}

std::wstring parent_key_of(std::wstring_view key) {
  const auto slash = key.find_last_of(L'\\');
  return slash == std::wstring_view::npos ? std::wstring{}
                                           : std::wstring{key.substr(0, slash)};
}

std::wstring leaf_key_name(std::wstring_view key) {
  const auto slash = key.find_last_of(L'\\');
  return std::wstring{slash == std::wstring_view::npos ? key : key.substr(slash + 1)};
}

std::vector<std::wstring> expected_children(const RegistrySchema& schema,
                                            std::wstring_view key) {
  std::vector<std::wstring> result;
  for (const auto& candidate : schema.keys) {
    if (parent_key_of(candidate) == key) result.push_back(leaf_key_name(candidate));
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<std::wstring> expected_value_names(const RegistrySchema& schema,
                                               std::wstring_view key) {
  std::vector<std::wstring> result;
  for (const auto& value : schema.values) {
    if (value.key == key) result.push_back(value.name);
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::wstring quote_windows_arg(std::wstring_view arg, bool always_quote = false) {
  if (!always_quote && !arg.empty() &&
      arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring{arg};
  }
  std::wstring quoted{L"\""};
  std::size_t backslashes = 0;
  for (const wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

void append_arg(std::wstring& command, std::wstring_view arg) {
  command.push_back(L' ');
  command += quote_windows_arg(arg);
}

void append_option(std::wstring& command, std::wstring_view option,
                   std::wstring_view value) {
  append_arg(command, option);
  append_arg(command, value);
}

std::wstring chroma_arg(int index) {
  switch (index) {
    case 1: return L"444";
    case 2: return L"422";
    case 3: return L"420";
    default: return L"auto";
  }
}

std::wstring alpha_arg(int index) {
  switch (index) {
    case 0: return L"force";
    case 2: return L"off";
    default: return L"auto";
  }
}

std::wstring avif_encoder_arg(int index) {
  return index == 0 ? L"auto" : index == 1 ? L"aom" : L"invalid";
}

std::wstring avif_color_representation_arg(int index) {
  switch (index) {
    case 1: return L"source";
    case 2: return L"rgb";
    default: return L"yuv";
  }
}

std::wstring icon_value(const std::filesystem::path& awj_exe) {
  return quote_windows_arg(awj_exe.wstring(), true) + L",0";
}

void append_string_spec(std::vector<RegistryValueSpec>& values,
                        std::wstring key, std::wstring name,
                        std::wstring value) {
  values.push_back(RegistryValueSpec{.key = std::move(key),
                                     .name = std::move(name),
                                     .kind = RegistryValueKind::string,
                                     .string_value = std::move(value)});
}

void append_dword_spec(std::vector<RegistryValueSpec>& values,
                       std::wstring key, std::wstring name,
                       std::uint32_t value) {
  values.push_back(RegistryValueSpec{.key = std::move(key),
                                     .name = std::move(name),
                                     .kind = RegistryValueKind::dword,
                                     .dword_value = value});
}

void append_owned_markers(std::vector<RegistryValueSpec>& values,
                          const std::wstring& key) {
  append_string_spec(values, key, std::wstring{owner_value_name},
                     std::wstring{owner_value});
  append_dword_spec(values, key, std::wstring{schema_value_name}, schema_version);
}

std::vector<std::wstring> current_parent_roots_for_all_extensions() {
  std::vector<std::wstring> roots;
  roots.reserve(std::size(kSupportedExtensions) * 2 + 3);
  roots.push_back(image_parent_key());
  roots.push_back(ico_parent_key());
  roots.push_back(directory_parent_key());
  for (const auto extension : kSupportedExtensions) {
    roots.push_back(extension_parent_key(extension));
    roots.push_back(class_extension_parent_key(extension));
  }
  std::ranges::sort(roots);
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  return roots;
}

std::expected<void, std::string> apply_schema(const RegistrySchema& schema) {
  for (const auto& key_path : schema.keys) {
    auto key = create_key(key_path);
    if (!key) return std::unexpected{key.error()};
  }
  for (const auto& value : schema.values) {
    if (value.kind == RegistryValueKind::string) {
      if (auto written = set_string(value.key, value.name, value.string_value); !written) {
        return written;
      }
    } else {
      if (auto written = set_dword(value.key, value.name, value.dword_value); !written) {
        return written;
      }
    }
  }
  return {};
}

std::expected<bool, std::string> verify_spec(const RegistryValueSpec& spec) {
  if (spec.kind == RegistryValueKind::string) {
    auto value = read_string(spec.key, spec.name);
    if (!value) return std::unexpected{value.error()};
    return *value && **value == spec.string_value;
  }
  auto value = read_dword(spec.key, spec.name);
  if (!value) return std::unexpected{value.error()};
  return *value && **value == spec.dword_value;
}

}  // namespace

std::span<const std::wstring_view> supported_extensions() noexcept {
  return kSupportedExtensions;
}

std::span<const CommandSpec> command_specs() noexcept {
  return kCommands;
}

std::wstring image_parent_key() { return std::wstring{kImageParent}; }
std::wstring directory_parent_key() { return std::wstring{kDirectoryParent}; }
std::wstring ico_parent_key() { return std::wstring{kIcoParent}; }
std::wstring shared_tree_key(int slot) {
  auto key = std::wstring{kSharedTree};
  if (slot == 1) key.back() = L'B';
  return key;
}
std::wstring legacy_shared_tree_key() { return std::wstring{kLegacySharedTreeV2}; }

std::wstring extension_parent_key(std::wstring_view extension) {
  return std::format(L"Software\\Classes\\SystemFileAssociations\\{}\\shell\\{}",
                     extension, parent_canonical_verb);
}

std::wstring class_extension_parent_key(std::wstring_view extension) {
  return std::format(L"Software\\Classes\\{}\\shell\\{}", extension,
                     parent_canonical_verb);
}

std::vector<std::wstring> legacy_root_keys() {
  std::vector<std::wstring> roots;
  roots.reserve(std::size(kSupportedExtensions) + 4);
  roots.emplace_back(kLegacyImageParent);
  roots.emplace_back(kLegacyIcoFileParent);
  roots.emplace_back(kLegacyDirectoryParent);
  roots.emplace_back(kLegacySharedTree);
  roots.emplace_back(kLegacySharedTreeV2);
  roots.emplace_back(kSharedTreeV3);
  roots.emplace_back(kImageParent);
  for (const auto extension : kSupportedExtensions) {
    roots.push_back(std::format(
        L"Software\\Classes\\SystemFileAssociations\\{}\\shell\\AWJImage",
        extension));
  }
  std::ranges::sort(roots);
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  return roots;
}

std::vector<std::wstring> owned_root_keys() {
  auto roots = legacy_root_keys();
  roots.push_back(shared_tree_key(0));
  roots.push_back(shared_tree_key(1));
  auto current = current_parent_roots_for_all_extensions();
  roots.insert(roots.end(), current.begin(), current.end());
  // No current icofile root is installed, but remove this owned name if an interrupted
  // development build ever created it.
  roots.push_back(std::format(L"Software\\Classes\\icofile\\shell\\{}",
                              parent_canonical_verb));
  std::ranges::sort(roots);
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  return roots;
}

InstallPlan build_install_plan() {
  InstallPlan plan;
  for (const auto extension : supported_extensions()) plan.extensions.emplace_back(extension);
  std::ranges::sort(plan.extensions);
  return plan;
}

std::wstring build_convert_command_line(const std::filesystem::path& awj_exe,
                                        std::wstring_view format,
                                        const FormatParams& params,
                                        bool append_png_suffix) {
  auto command = quote_windows_arg(awj_exe.wstring(), true);
  append_arg(command, L"--shell-window");
  append_arg(command, L"--shell-convert");
  append_option(command, L"--format", format);
  append_option(command, L"--collision", L"number");

  const bool is_avif = format == L"avif";
  const bool is_webp = format == L"webp";
  const bool is_jxl = format == L"jxl";
  const bool is_jpgli = format == L"jpgli";
  const bool is_png = format == L"png";
  if (!params.quality_text.empty()) append_option(command, L"--quality", params.quality_text);
  if ((is_avif || is_webp || is_jpgli || is_png) && !params.bit_depth_text.empty()) {
    append_option(command, L"--bit-depth", params.bit_depth_text);
  }
  if ((is_avif || is_webp || is_jxl) && !params.speed_text.empty()) {
    append_option(command, L"--speed", params.speed_text);
  }
  append_arg(command, params.strip_metadata ? L"--strip" : L"--keep-metadata");
  append_arg(command, params.allow_wic_fallback ? L"--allow-wic-fallback" : L"--no-wic-fallback");
  append_arg(command, params.close_on_finish ? L"--close-on-finish" : L"--no-close-on-finish");
  switch (params.size_limit_index) {
    case 1:
      append_option(command, L"--image-size-limit", L"none");
      break;
    case 2:
      append_option(command, L"--image-size-limit", L"manual");
      if (!params.max_width_text.empty()) append_option(command, L"--max-width", params.max_width_text);
      if (!params.max_height_text.empty()) append_option(command, L"--max-height", params.max_height_text);
      if (!params.max_long_edge_text.empty()) append_option(command, L"--max-long-edge", params.max_long_edge_text);
      if (!params.max_short_edge_text.empty()) append_option(command, L"--max-short-edge", params.max_short_edge_text);
      if (!params.scale_percent_text.empty()) append_option(command, L"--scale-percent", params.scale_percent_text);
      break;
    default:
      append_option(command, L"--image-size-limit", L"auto");
      break;
  }
  if (is_avif) {
    append_option(command, L"--avif-encoder", avif_encoder_arg(params.avif_encoder_index));
    append_option(command, L"--avif-color-representation",
                  avif_color_representation_arg(params.avif_color_representation_index));
    append_option(command, L"--chroma", chroma_arg(params.chroma_index));
    append_option(command, L"--alpha", alpha_arg(params.alpha_policy_index));
    if (append_png_suffix) append_arg(command, L"--append-png-suffix");
  } else if (is_jxl) {
    if (!params.jxl_jpeg_lossless) append_arg(command, L"--no-jxl-jpeg-lossless");
  } else if (is_jpgli) {
    append_option(command, L"--chroma", chroma_arg(params.chroma_index));
    append_option(command, L"--jpegli-progressive-level",
                  std::to_wstring(std::clamp(params.jpegli_progressive_index, 0, 2)));
    append_arg(command, params.jpegli_progressive_index > 0 || params.jpegli_optimize_huffman
                            ? L"--jpegli-optimize-huffman"
                            : L"--no-jpegli-optimize-huffman");
    if (params.jpegli_xyb) append_arg(command, L"--jpegli-xyb");
  }
  command += L" -i \"%1\" %*";
  return command;
}

RegistrySchema build_registry_schema(const std::filesystem::path& awj_exe,
                                     const MenuParams& menu_params,
                                     const InstallPlan& plan,
                                     std::span<const std::wstring> preset_names,
                                     int slot, bool compatibility) {
  RegistrySchema schema{.plan = plan};
  schema.parent_roots.push_back(directory_parent_key());
  if (compatibility) {
    schema.parent_roots.push_back(image_parent_key());
    schema.parent_roots.push_back(ico_parent_key());
    for (const auto& extension : plan.extensions)
      schema.parent_roots.push_back(class_extension_parent_key(extension));
  }
  for (const auto& extension : plan.extensions) {
    schema.parent_roots.push_back(extension_parent_key(extension));
  }
  std::ranges::sort(schema.parent_roots);
  schema.parent_roots.erase(std::unique(schema.parent_roots.begin(), schema.parent_roots.end()),
                            schema.parent_roots.end());

  const auto icon = icon_value(awj_exe);
  const auto shared = shared_tree_key(slot);
  const auto shared_shell = shared + L"\\shell";
  schema.keys.push_back(shared);
  schema.keys.push_back(shared_shell);
  append_owned_markers(schema.values, shared);

  for (const auto& command : kCommands) {
    if (compatibility) break;
    if (command.append_png_suffix && !menu_params[0].install_avif_png_command) continue;
    const auto verb_key = std::format(L"{}\\{}", shared_shell, command.canonical_verb);
    const auto command_key = verb_key + L"\\command";
    schema.keys.push_back(verb_key);
    schema.keys.push_back(command_key);
    append_string_spec(schema.values, verb_key, L"MUIVerb", std::wstring{command.label});
    append_string_spec(schema.values, verb_key, L"Icon", icon);
    append_string_spec(schema.values, verb_key, L"MultiSelectModel",
                       std::wstring{kMultiSelectModel});
    append_string_spec(
        schema.values, command_key, L"",
        build_convert_command_line(awj_exe, command.format,
                                   menu_params[command.params_index],
                                   command.append_png_suffix));
  }

  for (std::size_t index = 0; index < preset_names.size(); ++index) {
    const auto parent = std::format(L"{}\\AWJimage.Preset.{:02}", shared_shell, index);
    const auto subtree = parent + L"\\shell";
    schema.keys.push_back(parent);
    schema.keys.push_back(subtree);
    append_string_spec(schema.values, parent, L"MUIVerb", preset_names[index]);
    append_string_spec(schema.values, parent, L"Icon", icon);
    append_string_spec(schema.values, parent, L"MultiSelectModel", std::wstring{kMultiSelectModel});
    append_string_spec(schema.values, parent, std::wstring{kExtendedSubCommandsKey},
                       parent.substr(std::wstring_view{L"Software\\Classes\\"}.size()));
    for (const auto& spec : kCommands) {
      if (spec.append_png_suffix) continue;
      const auto verb = subtree + L"\\" + std::wstring{spec.canonical_verb};
      const auto command_key = verb + L"\\command";
      schema.keys.push_back(verb);
      schema.keys.push_back(command_key);
      append_string_spec(schema.values, verb, L"MUIVerb", std::wstring{spec.label});
      append_string_spec(schema.values, verb, L"Icon", icon);
      append_string_spec(schema.values, verb, L"MultiSelectModel", std::wstring{kMultiSelectModel});
      auto command = quote_windows_arg(awj_exe.wstring(), true);
      append_arg(command, L"--shell-window");
      append_arg(command, L"--shell-convert");
      append_option(command, L"--preset", preset_names[index]);
      append_option(command, L"--format", spec.format);
      append_option(command, L"--collision", L"number");
      command += L" -i \"%1\" %*";
      append_string_spec(schema.values, command_key, L"", std::move(command));
    }
  }

  for (const auto& parent : schema.parent_roots) {
    schema.keys.push_back(parent);
    append_string_spec(schema.values, parent, L"MUIVerb", std::wstring{kMenuLabel});
    append_string_spec(schema.values, parent, L"Icon", icon);
    append_string_spec(schema.values, parent, L"MultiSelectModel", std::wstring{kMultiSelectModel});
    if (compatibility) {
      std::wstring commands;
      for (const auto& spec : kCommands) {
        if (spec.append_png_suffix && !menu_params[0].install_avif_png_command) continue;
        if (!commands.empty()) commands += L';';
        commands += L"AWJImage.";
        commands += spec.append_png_suffix ? L"avif-png" : spec.format;
      }
      if (!preset_names.empty()) commands += L";AWJImage.presets";
      append_string_spec(schema.values, parent, L"SubCommands", std::move(commands));
    }
    if (!compatibility)
      append_string_spec(schema.values, parent, std::wstring{kExtendedSubCommandsKey},
                         shared.substr(std::wstring_view{L"Software\\Classes\\"}.size()));
    append_owned_markers(schema.values, parent);
  }
  return schema;
}

std::expected<InstallPlan, std::string> detect_install_plan() {
  return build_install_plan();
}

namespace {

struct RegistrationLock {
  MenuOperationLock operation;
  HANDLE handle{CreateMutexW(nullptr, FALSE, L"Local\\AWJimage.ContextMenu.v4")};
  bool held{};
  RegistrationLock() {
    if (operation.held() && handle) {
      const DWORD result = WaitForSingleObject(handle, 15000);
      held = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
  }
  ~RegistrationLock() {
    if (held) ReleaseMutex(handle);
    if (handle) CloseHandle(handle);
  }
};

bool is_awj_command(std::wstring_view command) {
  int argc = 0;
  const std::wstring text{command};
  auto argv = CommandLineToArgvW(text.c_str(), &argc);
  if (!argv) return false;
  bool awj = argc > 0 &&
      _wcsicmp(std::filesystem::path{argv[0]}.filename().c_str(), L"AWJ.exe") == 0;
  bool shell_convert = false;
  for (int i = 1; i < argc; ++i) {
    if (std::wstring_view{argv[i]} == L"--shell-convert") shell_convert = true;
  }
  LocalFree(argv);
  return awj && shell_convert;
}

// Only the exact historical roots are candidates. An unmarked root must contain
// an actual AWJ command, and may not contain commands belonging to another app.
std::expected<bool, std::string> historical_commands(std::wstring_view root,
                                                     bool& foreign_command,
                                                     int depth = 0) {
  if (depth > 16) return std::unexpected{"历史菜单嵌套过深，未进行清理。"};
  bool found = false;
  if (_wcsicmp(leaf_key_name(root).c_str(), L"command") == 0) {
    auto command = read_string(root, L"");
    if (!command) return std::unexpected{command.error()};
    if (!*command || !is_awj_command(**command)) {
      foreign_command = true;
      return false;
    }
    return true;
  }
  auto children = child_keys(root);
  if (!children) return std::unexpected{children.error()};
  if (children->size() > 128) return std::unexpected{"历史菜单子项过多，未进行清理。"};
  for (const auto& child : *children) {
    auto value = historical_commands(std::wstring{root} + L"\\" + child, foreign_command, depth + 1);
    if (!value) return std::unexpected{value.error()};
    if (_wcsicmp(child.c_str(), L"command") == 0 && !*value) return false;
    found = found || *value;
  }
  return found;
}

std::expected<bool, std::string> owned(std::wstring_view root) {
  auto marker = read_string(root, owner_value_name);
  if (!marker) return std::unexpected{marker.error()};
  if (*marker) return **marker == owner_value;
  const auto legacy = legacy_root_keys();
  if (std::ranges::find(legacy, root) == legacy.end()) return false;
  bool foreign_command = false;
  auto commands = historical_commands(root, foreign_command);
  if (!commands) return std::unexpected{commands.error()};
  return *commands && !foreign_command;
}

std::expected<void, std::string> verify_schema(const RegistrySchema& schema) {
  for (const auto& key : schema.keys) {
    auto values = value_names(key);
    if (!values) return std::unexpected{values.error()};
    auto children = child_keys(key);
    if (!children) return std::unexpected{children.error()};
    if (*values != expected_value_names(schema, key) ||
        *children != expected_children(schema, key)) {
      return std::unexpected{"右键菜单 key/value 集合与预期不一致：" + narrow_ascii(key)};
    }
  }
  for (const auto& spec : schema.values) {
    auto valid = verify_spec(spec);
    if (!valid) return std::unexpected{valid.error()};
    if (!*valid) return std::unexpected{"右键菜单值或类型与预期不一致：" + narrow_ascii(spec.key)};
  }
  return {};
}

std::expected<void, std::string> copy_tree(std::wstring_view source,
                                         std::wstring_view destination) {
  auto from = open_key(source, KEY_READ);
  if (!from) return std::unexpected{from.error()};
  auto to = create_key(destination, KEY_ALL_ACCESS);
  if (!to) return std::unexpected{to.error()};
  const auto status = RegCopyTreeW(from->get(), nullptr, to->get());
  if (status != ERROR_SUCCESS) return std::unexpected{registry_error("复制注册快照", source, status)};
  return {};
}

std::expected<void, std::string> flush_journal() {
  auto key = open_key(kTransaction, KEY_READ);
  if (!key) return std::unexpected{key.error()};
  const auto status = RegFlushKey(key->get());
  if (status != ERROR_SUCCESS) return std::unexpected{registry_error("保存注册事务", kTransaction, status)};
  return {};
}

struct SnapshotRoot {
  std::wstring path;
  bool present{};
  bool managed{};
};

std::expected<std::vector<SnapshotRoot>, std::string> snapshot_roots(
    const RegistrySchema& schema) {
  std::vector<SnapshotRoot> roots;
  for (const auto& path : owned_root_keys()) {
    auto exists = key_exists(path);
    if (!exists) return std::unexpected{exists.error()};
    bool managed = true;
    if (*exists) {
      auto ours = owned(path);
      if (!ours) return std::unexpected{ours.error()};
      managed = *ours;
    }
    if (!managed && std::ranges::find(schema.keys, path) != schema.keys.end()) {
      return std::unexpected{"注册位置被非 AWJ 项占用，未修改：" + narrow_ascii(path)};
    }
    roots.push_back({path, *exists, managed});
  }
  return roots;
}

std::expected<void, std::string> begin_journal(const std::vector<SnapshotRoot>& roots) {
  if (auto r = set_string(kTransaction, owner_value_name, owner_value); !r) return r;
  if (auto r = set_dword(kTransaction, schema_value_name, schema_version); !r) return r;
  if (auto r = set_dword(kTransaction, L"State", 0); !r) return r;
  if (!staged_id.empty()) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
      return std::unexpected{"无法记录菜单事务进程身份。"};
    if (auto r = set_string(kTransaction, L"Id", staged_id); !r) return r;
    if (auto r = set_dword(kTransaction, L"Machine", staged_machine); !r) return r;
    if (auto r = set_dword(kTransaction, L"Pid", GetCurrentProcessId()); !r) return r;
    if (auto r = set_dword(kTransaction, L"CreatedLow", created.dwLowDateTime); !r) return r;
    if (auto r = set_dword(kTransaction, L"CreatedHigh", created.dwHighDateTime); !r) return r;
  }
  for (std::size_t index = 0; index < roots.size(); ++index) {
    const auto& root = roots[index];
    const auto key = std::format(L"{}\\{:03}", kTransaction, index);
    if (auto r = set_string(key, L"Path", root.path); !r) return r;
    if (auto r = set_dword(key, L"Present", root.present); !r) return r;
    if (auto r = set_dword(key, L"Managed", root.managed); !r) return r;
    if (root.present && root.managed) {
      if (auto r = copy_tree(root.path, key + L"\\Data"); !r) return r;
    }
  }
  if (auto r = set_dword(kTransaction, L"Count", static_cast<DWORD>(roots.size())); !r) return r;
  if (auto r = set_dword(kTransaction, L"State", 1); !r) return r;
  return flush_journal();
}

std::expected<void, std::string> recover_locked() {
  auto exists = key_exists(kTransaction);
  if (!exists) return std::unexpected{exists.error()};
  if (!*exists) return {};
  auto marker = read_string(kTransaction, owner_value_name);
  if (!marker || !*marker || **marker != owner_value) {
    return std::unexpected{"注册事务缺少 AWJ ownership，未修改注册表。"};
  }
  auto state = read_dword(kTransaction, L"State");
  if (!state) return std::unexpected{state.error()};
  // State 0 never changes live registrations; State 2 has fully committed.
  if (!*state || **state == 0 || **state == 2) return delete_tree(kTransaction);
  if (**state != 1) return std::unexpected{"注册事务状态无效。"};
  auto id = read_string(kTransaction, L"Id");
  if (!id) return std::unexpected{id.error()};
  if (*id) {
    auto machine = read_dword(kTransaction, L"Machine");
    auto pid = read_dword(kTransaction, L"Pid");
    auto low = read_dword(kTransaction, L"CreatedLow");
    auto high = read_dword(kTransaction, L"CreatedHigh");
    if (!machine || !*machine || **machine > 1 || !pid || !*pid || !low || !*low || !high || !*high)
      return std::unexpected{"菜单事务进程身份无效。"};
    if (menu_owner_active(**pid, FILETIME{**low, **high}) &&
        !(defer_commit && **pid == GetCurrentProcessId() && staged_id == **id))
      return std::unexpected{"另一进程正在修改右键菜单，请稍后重试。"};
    auto committed = menu_commit_recorded(**id, **machine != 0);
    if (!committed) return std::unexpected{committed.error()};
    if (*committed) return delete_tree(kTransaction);
  }
  auto count = read_dword(kTransaction, L"Count");
  const auto allowed = owned_root_keys();
  if (!count || !*count || **count != allowed.size()) {
    return std::unexpected{"注册快照不完整，未继续修改。"};
  }
  std::vector<SnapshotRoot> roots;
  for (std::size_t i = 0; i < allowed.size(); ++i) {
    const auto key = std::format(L"{}\\{:03}", kTransaction, i);
    auto path = read_string(key, L"Path");
    auto present = read_dword(key, L"Present");
    auto managed = read_dword(key, L"Managed");
    if (!path || !*path || **path != allowed[i] || !present || !*present ||
        **present > 1 || !managed || !*managed || **managed > 1) {
      return std::unexpected{"注册快照字段无效，未继续修改。"};
    }
    if (**present && **managed) {
      auto data = key_exists(key + L"\\Data");
      if (!data || !*data) return std::unexpected{"注册快照数据缺失。"};
    }
    roots.push_back({**path, **present != 0, **managed != 0});
  }
  std::string errors;
  // Validate every snapshot before restoring any root. Keep the journal if a
  // restore fails so the next launch can retry with the same original data.
  for (std::size_t i = 0; i < roots.size(); ++i) {
    const auto& root = roots[i];
    if (!root.managed) continue;
    auto restored = delete_tree(root.path);
    if (restored && root.present) {
      restored = copy_tree(std::format(L"{}\\{:03}\\Data", kTransaction, i), root.path);
    }
    if (!restored) errors += restored.error() + " ";
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  if (!errors.empty()) return std::unexpected{std::move(errors)};
  return delete_tree(kTransaction);
}

std::expected<int, std::string> active_slot() {
  auto reference = read_string(directory_parent_key(), kExtendedSubCommandsKey);
  if (!reference) return std::unexpected{reference.error()};
  return *reference && **reference == L"AWJimage.ContextMenu.v4.B" ? 1 : 0;
}

std::expected<void, std::string> validate_request(const std::filesystem::path& exe,
                                                const RegistrySchema& schema,
                                                std::span<const std::wstring> names) {
  std::error_code ec;
  if (!exe.is_absolute() || !std::filesystem::is_regular_file(exe, ec) || ec) {
    return std::unexpected{"右键菜单程序必须是存在的绝对路径普通文件。"};
  }
  if (names.size() > 10) return std::unexpected{"最多同时注入 10 个预设。"};
  std::set<std::wstring> unique;
  for (const auto& name : names) {
    auto normalized = name;
    std::ranges::transform(normalized, normalized.begin(), [](wchar_t c) { return std::towlower(c); });
    if (name.empty() || name.size() > 240 ||
        std::ranges::any_of(name, [](wchar_t c) { return c < 32; }) ||
        !unique.insert(normalized).second) {
      return std::unexpected{"注入预设名称无效或重复。"};
    }
  }
  for (const auto& value : schema.values) {
    if (value.kind != RegistryValueKind::string) continue;
    if (value.string_value.size() >= 30000 ||
        value.string_value.find(L'\0') != std::wstring::npos ||
        value.string_value.find_first_of(L"\r\n") != std::wstring::npos) {
      return std::unexpected{"菜单参数含控制字符或超过 Windows 命令长度限制。"};
    }
  }
  return {};
}

std::expected<void, std::string> verify_no_obsolete_roots(const RegistrySchema& schema) {
  for (const auto& root : owned_root_keys()) {
    if (std::ranges::find(schema.keys, root) != schema.keys.end()) continue;
    auto exists = key_exists(root);
    if (!exists) return std::unexpected{exists.error()};
    if (!*exists) continue;
    auto ours = owned(root);
    if (!ours) return std::unexpected{ours.error()};
    if (*ours) return std::unexpected{"旧菜单或非活动树尚未清除：" + narrow_ascii(root)};
  }
  return {};
}

std::expected<void, std::string> commit_journal() {
  if (defer_commit) return {};
  if (auto result = set_dword(kTransaction, L"State", 2); !result) return result;
  if (auto result = flush_journal(); !result) return result;
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return delete_tree(kTransaction);
}

}  // namespace

std::expected<void, std::string> recover() {
  RegistrationLock lock;
  if (!lock.held) return std::unexpected{"另一进程正在修改右键菜单，请稍后重试。"};
  return recover_locked();
}

std::expected<bool, std::string> is_installed() {
  for (const auto& root : owned_root_keys()) {
    auto exists = key_exists(root);
    if (!exists) return std::unexpected{exists.error()};
    if (!*exists) continue;
    auto ours = owned(root);
    if (!ours) return std::unexpected{ours.error()};
    if (*ours) return true;
  }
  return false;
}

std::expected<void, std::string> reconcile(const std::filesystem::path& awj_exe,
                                         const MenuParams& menu_params,
                                         std::span<const std::wstring> preset_names,
                                         bool force_install, bool compatibility) {
  RegistrationLock lock;
  if (!lock.held) return std::unexpected{"另一进程正在修改右键菜单，请稍后重试。"};
  if (auto recovered = recover_locked(); !recovered) return recovered;
  auto installed = is_installed();
  if (!installed) return std::unexpected{installed.error()};
  if (!*installed && !force_install) return {};
  std::error_code ec;
  const auto exe = std::filesystem::absolute(awj_exe, ec).lexically_normal();
  if (ec) return std::unexpected{"无法确定程序的绝对路径。"};
  auto active = active_slot();
  if (!active) return std::unexpected{active.error()};
  const auto plan = build_install_plan();
  auto current = build_registry_schema(exe, menu_params, plan, preset_names, *active, compatibility);
  if (auto valid = validate_request(exe, current, preset_names); !valid) return valid;
  if (verify_schema(current) && verify_no_obsolete_roots(current)) return {};
  const int next = compatibility ? 0 : (*installed ? 1 - *active : 0);
  const auto schema = build_registry_schema(exe, menu_params, plan, preset_names, next, compatibility);
  auto roots = snapshot_roots(schema);
  if (!roots) return std::unexpected{roots.error()};
  if (auto saved = begin_journal(*roots); !saved) {
    auto recovered = recover_locked();
    return std::unexpected{saved.error() + (recovered ? "" : " " + recovered.error())};
  }
  auto apply = [&]() -> std::expected<void, std::string> {
    if (compatibility) {
      auto preview = build_registry_schema(exe, menu_params, plan, preset_names, 1, true);
      const auto preview_root = shared_tree_key(1);
      std::erase_if(preview.keys, [&](const auto& key) { return !key.starts_with(preview_root); });
      std::erase_if(preview.values, [&](const auto& value) { return !value.key.starts_with(preview_root); });
      if (auto r = delete_tree(preview_root); !r) return r;
      if (auto r = apply_schema(preview); !r) return r;
      if (auto r = verify_schema(preview); !r) return r;
    }
    const auto shared = shared_tree_key(next);
    if (auto r = delete_tree(shared); !r) return r;
    RegistrySchema tree = schema;
    std::erase_if(tree.keys, [&](const auto& key) {
      return key != shared && !key.starts_with(shared + L"\\");
    });
    std::erase_if(tree.values, [&](const auto& value) {
      return std::ranges::find(tree.keys, value.key) == tree.keys.end();
    });
    if (auto r = apply_schema(tree); !r) return r;
    if (auto r = verify_schema(tree); !r) return r;
    // Parents switch only after the complete inactive tree is readable.
    for (const auto& parent : schema.parent_roots) {
      if (auto r = delete_tree(parent); !r) return r;
      for (const auto& value : schema.values) {
        if (value.key != parent) continue;
        auto written = value.kind == RegistryValueKind::string
            ? set_string(value.key, value.name, value.string_value)
            : set_dword(value.key, value.name, value.dword_value);
        if (!written) return written;
      }
    }
    if (auto r = verify_schema(schema); !r) return r;
    for (const auto& root : *roots) {
      if (!root.managed || std::ranges::find(schema.keys, root.path) != schema.keys.end()) continue;
      if (auto r = delete_tree(root.path); !r) return r;
    }
    if (auto r = verify_no_obsolete_roots(schema); !r) return r;
    return verify_schema(schema);
  };
  if (auto applied = apply(); !applied) {
    auto rolled_back = recover_locked();
    return std::unexpected{applied.error() + (rolled_back ? " 已恢复原注册。" : " 回滚失败：" + rolled_back.error())};
  }
  return commit_journal();
}

std::expected<void, std::string> install(const std::filesystem::path& awj_exe,
                                       const MenuParams& menu_params,
                                       std::span<const std::wstring> preset_names) {
  return reconcile(awj_exe, menu_params, preset_names, true);
}

std::expected<void, std::string> remove() {
  RegistrationLock lock;
  if (!lock.held) return std::unexpected{"另一进程正在修改右键菜单，请稍后重试。"};
  if (auto recovered = recover_locked(); !recovered) return recovered;
  const RegistrySchema empty;
  auto roots = snapshot_roots(empty);
  if (!roots) return std::unexpected{roots.error()};
  if (auto saved = begin_journal(*roots); !saved) return saved;
  for (const auto& root : *roots) {
    if (!root.managed) continue;
    if (auto removed = delete_tree(root.path); !removed) {
      auto restored = recover_locked();
      return std::unexpected{removed.error() + (restored ? " 已恢复原注册。" : " " + restored.error())};
    }
  }
  if (auto verified = verify_no_obsolete_roots(empty); !verified) {
    auto restored = recover_locked();
    return std::unexpected{verified.error() + (restored ? " 已恢复原注册。" : " " + restored.error())};
  }
  return commit_journal();
}

std::expected<std::optional<std::string>, std::string> warning(
    const std::filesystem::path& awj_exe, const MenuParams& menu_params,
    std::span<const std::wstring> preset_names, bool compatibility) {
  auto installed = is_installed();
  if (!installed) return std::unexpected{installed.error()};
  if (!*installed) return std::optional<std::string>{};
  auto slot = active_slot();
  if (!slot) return std::unexpected{slot.error()};
  const auto schema = build_registry_schema(awj_exe, menu_params, build_install_plan(), preset_names, *slot, compatibility);
  if (auto valid = verify_schema(schema); !valid) return std::optional<std::string>{valid.error()};
  if (auto valid = verify_no_obsolete_roots(schema); !valid) return std::optional<std::string>{valid.error()};
  return std::optional<std::string>{};
}

namespace {
constexpr std::wstring_view kMachineCommandPrefix =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\";
// PR #3 used exactly these six names. Never accept registry paths from argv.
constexpr std::wstring_view kMachineCommands[] = {
    L"AWJImage.png", L"AWJImage.webp", L"AWJImage.avif",
    L"AWJImage.avif-png", L"AWJImage.jxl", L"AWJImage.jpgli", L"AWJImage.presets"};
}

std::expected<std::vector<std::wstring>, std::string> legacy_machine_commands() {
  std::vector<std::wstring> found;
  for (const auto name : kMachineCommands) {
    const auto path = std::wstring{kMachineCommandPrefix} + std::wstring{name} + L"\\command";
    wchar_t command[32768]{};
    DWORD bytes = sizeof(command);
    const auto status = RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), nullptr,
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, command, &bytes);
    if (missing_registry_status(status)) continue;
    if (status != ERROR_SUCCESS) return std::unexpected{registry_error("检查历史系统菜单", path, status)};
    if (is_awj_command(command)) found.emplace_back(name);
  }
  return found;
}

std::expected<bool, std::string> compatibility_installed() {
  for (const auto& root : current_parent_roots_for_all_extensions()) {
    auto ours = owned(root);
    if (!ours) return std::unexpected{ours.error()};
    if (!*ours) continue;
    auto commands = read_string(root, L"SubCommands");
    if (!commands) return std::unexpected{commands.error()};
    if (*commands && (**commands).starts_with(L"AWJImage.")) return true;
  }
  return false;
}

namespace {
RegistrySchema machine_schema(const std::filesystem::path& exe,
                              const MenuParams& params) {
  RegistrySchema schema;
  const auto presets = std::wstring{kMachineCommandPrefix} + L"AWJImage.presets";
  schema.keys.push_back(presets);
  append_owned_markers(schema.values, presets);
  append_string_spec(schema.values, presets, L"MUIVerb", L"用户预设 / User presets");
  append_string_spec(schema.values, presets, L"Icon", icon_value(exe));
  append_string_spec(schema.values, presets, std::wstring{kExtendedSubCommandsKey},
                     std::wstring{shared_tree_reference});
  for (const auto& command : kCommands) {
    const auto suffix = command.append_png_suffix ? L"avif-png" : command.format;
    const auto root = std::wstring{kMachineCommandPrefix} + L"AWJImage." + std::wstring{suffix};
    schema.keys.push_back(root);
    schema.keys.push_back(root + L"\\command");
    append_owned_markers(schema.values, root);
    append_string_spec(schema.values, root, L"MUIVerb", std::wstring{command.label});
    append_string_spec(schema.values, root, L"Icon", icon_value(exe));
    append_string_spec(schema.values, root, L"MultiSelectModel", std::wstring{kMultiSelectModel});
    append_string_spec(schema.values, root + L"\\command", L"",
        build_convert_command_line(exe, command.format, params[command.params_index],
                                   command.append_png_suffix));
  }
  return schema;
}
}

std::expected<void, std::string> stage_user_menu(
    std::wstring_view id, bool machine, const std::filesystem::path& exe, const MenuParams& params,
    std::span<const std::wstring> names, bool compatibility, bool remove_menu) {
  if (auto restored = recover(); !restored) return restored;
  StageScope scope;
  staged_id = id;
  staged_machine = machine;
  return remove_menu ? remove() : reconcile(exe, params, names, true, compatibility);
}

std::expected<void, std::string> finish_user_menu(std::wstring_view id, bool commit) {
  RegistrationLock lock;
  if (!lock.held) return std::unexpected{"无法锁定菜单事务。"};
  auto stored = read_string(kTransaction, L"Id");
  if (!stored) return std::unexpected{stored.error()};
  if (!*stored) return {};
  if (**stored != id) return std::unexpected{"菜单事务身份不匹配。"};
  if (commit) return commit_journal();
  StageScope scope;
  staged_id = id;
  return recover_locked();
}

namespace {
constexpr wchar_t machine_journal[] = L"SOFTWARE\\AWJimage.MenuTransaction";
std::expected<void, std::string> apply_machine_menu(
    const std::filesystem::path& exe, const MenuParams& params,
    bool remove_menu, bool validate_only = false) {
  const auto schema = machine_schema(exe, params);
  if (auto valid = validate_request(exe, schema, {}); !valid) return valid;
  for (const auto name : kMachineCommands) {
    const auto root = std::wstring{kMachineCommandPrefix} + std::wstring{name};
    auto exists = key_exists(root);
    if (!exists) return std::unexpected{exists.error()};
    if (!*exists) continue;
    if (name == L"AWJImage.presets") {
      auto marker = read_string(root, owner_value_name);
      auto icon = read_string(root, L"Icon");
      if (!marker || !icon || !*marker || !*icon || **marker != owner_value || **icon != icon_value(exe))
        return std::unexpected{"机器预设菜单已被其他程序位置占用，未修改。"};
      continue;
    }
    auto command = read_string(root + L"\\command", L"");
    if (!command) return std::unexpected{command.error()};
    const auto prefix = quote_windows_arg(exe.wstring(), true) + L" --shell-window --shell-convert ";
    if (!*command || !(**command).starts_with(prefix))
      return std::unexpected{"机器菜单已被其他程序位置占用，未修改。"};
  }
  if (validate_only) return {};
  for (const auto name : kMachineCommands) {
    const auto root = std::wstring{kMachineCommandPrefix} + std::wstring{name};
    if (auto removed = delete_tree(root); !removed) return removed;
  }
  if (remove_menu) return {};
  if (auto applied = apply_schema(schema); !applied) return applied;
  return verify_schema(schema);
}
}

std::expected<void, std::string> recover_machine_menu() {
  NativeRegistryScope scope{true};
  auto exists = key_exists(machine_journal);
  if (!exists) return std::unexpected{exists.error()};
  if (!*exists) return {};
  auto protected_key = protected_machine_key(machine_journal, false);
  if (!protected_key) return std::unexpected{protected_key.error()};
  RegistryKey secured{*protected_key};
  auto state = read_dword(machine_journal, L"State");
  if (!state) return std::unexpected{state.error()};
  if (!*state || **state == 0) return delete_tree(machine_journal);
  if (**state != 1) return std::unexpected{"机器菜单事务状态无效。"};
  auto id = read_string(machine_journal, L"Id");
  auto sid = read_string(machine_journal, L"Sid");
  if (!id || !*id || !sid || !*sid) return std::unexpected{"机器菜单事务身份缺失。"};
  auto committed = menu_commit_recorded(**id, true, **sid);
  if (!committed) return std::unexpected{committed.error()};
  if (*committed) return delete_tree(machine_journal);
  std::array<bool, std::size(kMachineCommands)> present{};
  for (std::size_t i = 0; i < present.size(); ++i) {
    const auto backup = std::format(L"{}\\{:03}", machine_journal, i);
    auto value = read_dword(backup, L"Present");
    if (!value || !*value || **value > 1) return std::unexpected{"受保护的机器菜单快照无效。"};
    present[i] = **value != 0;
    if (present[i]) {
      auto data = key_exists(backup + L"\\Data");
      if (!data || !*data) return std::unexpected{"受保护的机器菜单快照缺失。"};
    }
  }
  for (std::size_t i = 0; i < present.size(); ++i) {
    const auto target = std::wstring{kMachineCommandPrefix} + std::wstring{kMachineCommands[i]};
    if (auto removed = delete_tree(target); !removed) return removed;
    if (present[i]) {
      if (auto copied = copy_tree(std::format(L"{}\\{:03}\\Data", machine_journal, i), target); !copied) return copied;
    }
  }
  return delete_tree(machine_journal);
}

std::expected<void, std::string> stage_machine_menu(
    const std::filesystem::path& exe, const MenuParams& params, bool remove_menu,
    std::wstring_view id, std::wstring_view sid) {
  if (auto restored = recover_machine_menu(); !restored) return restored;
  NativeRegistryScope scope{true};
  const auto schema = machine_schema(exe, params);
  if (auto valid = validate_request(exe, schema, {}); !valid) return valid;
  if (auto valid = apply_machine_menu(exe, params, remove_menu, true); !valid) return valid;
  auto protected_key = protected_machine_key(machine_journal, false);
  if (!protected_key) return std::unexpected{protected_key.error()};
  RegistryKey secured{*protected_key};
  if (auto r = set_dword(machine_journal, L"State", 0); !r) return r;
  if (auto r = set_string(machine_journal, L"Id", id); !r) return r;
  if (auto r = set_string(machine_journal, L"Sid", sid); !r) return r;
  for (std::size_t i = 0; i < std::size(kMachineCommands); ++i) {
    const auto source = std::wstring{kMachineCommandPrefix} + std::wstring{kMachineCommands[i]};
    const auto backup = std::format(L"{}\\{:03}", machine_journal, i);
    auto present = key_exists(source);
    if (!present) return std::unexpected{present.error()};
    if (auto r = set_dword(backup, L"Present", *present); !r) return r;
    if (*present) {
      if (auto r = copy_tree(source, backup + L"\\Data"); !r) return r;
    }
  }
  // Validate every copied child before the journal may be used for recovery.
  auto verified = protected_machine_key(machine_journal, false);
  if (!verified) return std::unexpected{verified.error()};
  RegCloseKey(*verified);
  if (auto r = set_dword(machine_journal, L"State", 1); !r) return r;
  if (RegFlushKey(secured.get()) != ERROR_SUCCESS) return std::unexpected{"无法持久化机器菜单快照。"};
  // Keep the old compatibility menu usable until the replacement HKCU schema
  // has been verified and the original process requests commit.
  return remove_menu ? std::expected<void, std::string>{} : apply_machine_menu(exe, params, false);
}

std::expected<void, std::string> commit_machine_menu(std::wstring_view id, std::wstring_view sid,
    const std::filesystem::path& exe, const MenuParams& params, bool remove_menu) {
  NativeRegistryScope scope{true};
  if (remove_menu) {
    if (auto removed = apply_machine_menu(exe, params, true); !removed) return removed;
  }
  if (auto recorded = record_menu_commit(id, true, sid); !recorded) return recorded;
  // The receipt is the durable commit point; leftover backup cleanup is retryable.
  (void)delete_tree(machine_journal);
  return {};
}

std::expected<bool, std::string> machine_menu_matches(
    const std::filesystem::path& exe, const MenuParams& params) {
  NativeRegistryScope scope{true};
  return verify_schema(machine_schema(exe, params)).has_value();
}

}  // namespace awj::shell_context_menu
