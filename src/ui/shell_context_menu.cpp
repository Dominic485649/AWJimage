#include "shell_context_menu.hpp"

#include "shell_extension_contract.hpp"
#include "shell_extension_core.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

constexpr std::wstring_view kImageParent =
    L"Software\\Classes\\SystemFileAssociations\\image\\shell\\AWJimage.Convert";
constexpr std::wstring_view kDirectoryParent =
    L"Software\\Classes\\Directory\\shell\\AWJimage.Convert";
// v4 static-cascade roots remain listed for migration and rollback only.
constexpr std::wstring_view kSharedTree =
    L"Software\\Classes\\AWJimage.ContextMenu.v4.A";
constexpr std::wstring_view kSharedTreeV3 = L"Software\\Classes\\AWJimage.ContextMenu.v3";
constexpr std::wstring_view kTransaction = L"Software\\Classes\\AWJimage.ContextMenu.v4.Transaction";
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
  const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                                      REG_OPTION_NON_VOLATILE,
                                      access, nullptr, &raw, nullptr);
  if (status != ERROR_SUCCESS) {
    return std::unexpected{registry_error("创建右键菜单注册表项", subkey, status)};
  }
  return RegistryKey{raw};
}

std::expected<RegistryKey, std::string> open_key(std::wstring_view subkey,
                                                 REGSAM access) {
  HKEY raw = nullptr;
  const std::wstring path{subkey};
  const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, access, &raw);
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

std::expected<void, std::string> set_multi_string(
    std::wstring_view subkey, std::wstring_view name,
    std::span<const std::wstring> values) {
  auto key = create_key(subkey);
  if (!key) return std::unexpected{key.error()};
  std::size_t characters = 1;
  for (const auto& value : values) characters += value.size() + 1;
  if (characters > 256 * 1024) {
    return std::unexpected{"右键菜单配置超过注册表值大小限制。"};
  }
  std::vector<wchar_t> buffer;
  buffer.reserve(characters);
  for (const auto& value : values) {
    buffer.insert(buffer.end(), value.begin(), value.end());
    buffer.push_back(L'\0');
  }
  buffer.push_back(L'\0');
  const std::wstring name_storage{name};
  const auto status = RegSetValueExW(
      key->get(), name_storage.c_str(), 0, REG_MULTI_SZ,
      reinterpret_cast<const BYTE*>(buffer.data()),
      static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
  if (status != ERROR_SUCCESS) {
    return std::unexpected{
        registry_error("写入右键菜单多字符串值", subkey, status)};
  }
  return {};
}

std::expected<bool, std::string> key_exists(std::wstring_view subkey) {
  HKEY raw = nullptr;
  const std::wstring path{subkey};
  const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &raw);
  if (status == ERROR_SUCCESS) {
    RegCloseKey(raw);
    return true;
  }
  if (missing_registry_status(status)) return false;
  return std::unexpected{registry_error("检查右键菜单注册表项", subkey, status)};
}

std::expected<void, std::string> delete_tree(std::wstring_view subkey) {
  const std::wstring path{subkey};
  const auto status = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  if (status == ERROR_SUCCESS || missing_registry_status(status)) return {};
  return std::unexpected{registry_error("删除右键菜单注册表项", subkey, status)};
}

std::expected<std::optional<std::wstring>, std::string> read_string(
    std::wstring_view subkey, std::wstring_view name) {
  const std::wstring path{subkey};
  const std::wstring name_storage{name};
  DWORD bytes = 0;
  DWORD type = 0;
  const auto first = RegGetValueW(HKEY_CURRENT_USER, path.c_str(),
                                  name.empty() ? nullptr : name_storage.c_str(),
                                  RRF_RT_REG_SZ, &type, nullptr, &bytes);
  if (missing_registry_status(first)) return std::optional<std::wstring>{};
  if (first != ERROR_SUCCESS) {
    return std::unexpected{registry_error("读取右键菜单字符串值", subkey, first)};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  const auto second = RegGetValueW(HKEY_CURRENT_USER, path.c_str(),
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
  const auto status = RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name_storage.c_str(),
                                   RRF_RT_REG_DWORD, &type, &value, &bytes);
  if (missing_registry_status(status)) return std::optional<std::uint32_t>{};
  if (status != ERROR_SUCCESS) {
    return std::unexpected{registry_error("读取右键菜单 DWORD 值", subkey, status)};
  }
  return std::optional<std::uint32_t>{value};
}

std::expected<std::optional<std::vector<std::wstring>>, std::string>
read_multi_string(std::wstring_view subkey, std::wstring_view name) {
  const std::wstring path{subkey};
  const std::wstring name_storage{name};
  DWORD bytes = 0;
  DWORD type = 0;
  const auto first = RegGetValueW(HKEY_CURRENT_USER, path.c_str(),
                                  name_storage.c_str(), RRF_RT_REG_MULTI_SZ,
                                  &type, nullptr, &bytes);
  if (missing_registry_status(first)) {
    return std::optional<std::vector<std::wstring>>{};
  }
  if (first != ERROR_SUCCESS || bytes < 2 * sizeof(wchar_t) ||
      bytes > 512 * 1024 || bytes % sizeof(wchar_t) != 0) {
    return std::unexpected{
        registry_error("读取右键菜单多字符串值", subkey, first)};
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
  const auto second = RegGetValueW(
      HKEY_CURRENT_USER, path.c_str(), name_storage.c_str(),
      RRF_RT_REG_MULTI_SZ, &type, buffer.data(), &bytes);
  if (second != ERROR_SUCCESS || buffer.size() < 2 ||
      buffer[buffer.size() - 1] != L'\0' ||
      buffer[buffer.size() - 2] != L'\0') {
    return std::unexpected{
        registry_error("读取右键菜单多字符串值", subkey, second)};
  }
  std::vector<std::wstring> values;
  const wchar_t* cursor = buffer.data();
  const wchar_t* const final_terminator = buffer.data() + buffer.size() - 1;
  while (cursor < final_terminator && *cursor != L'\0') {
    const wchar_t* terminator = std::find(cursor, final_terminator, L'\0');
    if (terminator == final_terminator) {
      return std::unexpected{"右键菜单多字符串值终止符无效。"};
    }
    values.emplace_back(cursor, terminator);
    cursor = terminator + 1;
  }
  return std::optional<std::vector<std::wstring>>{std::move(values)};
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

void append_multi_string_spec(std::vector<RegistryValueSpec>& values,
                              std::wstring key, std::wstring name,
                              std::vector<std::wstring> value) {
  values.push_back(RegistryValueSpec{
      .key = std::move(key),
      .name = std::move(name),
      .kind = RegistryValueKind::multi_string,
      .multi_string_value = std::move(value)});
}

void append_owned_markers(std::vector<RegistryValueSpec>& values,
                          const std::wstring& key) {
  append_string_spec(values, key, std::wstring{owner_value_name},
                     std::wstring{owner_value});
  append_dword_spec(values, key, std::wstring{schema_value_name}, schema_version);
}

std::vector<std::wstring> current_parent_roots_for_all_extensions() {
  std::vector<std::wstring> roots;
  roots.reserve(std::size(kSupportedExtensions) + 2);
  roots.push_back(image_parent_key());
  roots.push_back(directory_parent_key());
  for (const auto extension : kSupportedExtensions) {
    roots.push_back(extension_parent_key(extension));
  }
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
    } else if (value.kind == RegistryValueKind::dword) {
      if (auto written = set_dword(value.key, value.name, value.dword_value); !written) {
        return written;
      }
    } else {
      if (auto written = set_multi_string(value.key, value.name,
                                          value.multi_string_value);
          !written) {
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
  if (spec.kind == RegistryValueKind::dword) {
    auto value = read_dword(spec.key, spec.name);
    if (!value) return std::unexpected{value.error()};
    return *value && **value == spec.dword_value;
  }
  auto value = read_multi_string(spec.key, spec.name);
  if (!value) return std::unexpected{value.error()};
  return *value && **value == spec.multi_string_value;
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
std::wstring class_root_key() {
  return std::wstring{awj::shell_extension::contract::class_root};
}
std::wstring file_handler_key() {
  return std::wstring{awj::shell_extension::contract::file_handler_root};
}
std::wstring folder_handler_key() {
  return std::wstring{awj::shell_extension::contract::folder_handler_root};
}
std::filesystem::path shell_extension_path(
    const std::filesystem::path& awj_exe) {
  return awj_exe.parent_path() /
         awj::shell_extension::contract::shell_extension_filename;
}
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
  roots.push_back(class_root_key());
  roots.push_back(file_handler_key());
  roots.push_back(folder_handler_key());
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

namespace {

std::vector<std::wstring> build_convert_arguments(
    std::wstring_view format, const FormatParams& params,
    bool append_png_suffix) {
  std::vector<std::wstring> arguments{
      L"--shell-window", L"--shell-convert", L"--format",
      std::wstring{format}, L"--collision", L"number"};
  const auto append_option = [&](std::wstring_view option,
                                 std::wstring_view value) {
    arguments.emplace_back(option);
    arguments.emplace_back(value);
  };
  const bool is_avif = format == L"avif";
  const bool is_webp = format == L"webp";
  const bool is_jxl = format == L"jxl";
  const bool is_jpgli = format == L"jpgli";
  const bool is_png = format == L"png";
  if (!params.quality_text.empty()) append_option(L"--quality", params.quality_text);
  if ((is_avif || is_webp || is_jpgli || is_png) && !params.bit_depth_text.empty()) {
    append_option(L"--bit-depth", params.bit_depth_text);
  }
  if ((is_avif || is_webp || is_jxl) && !params.speed_text.empty()) {
    append_option(L"--speed", params.speed_text);
  }
  arguments.emplace_back(params.strip_metadata ? L"--strip" : L"--keep-metadata");
  arguments.emplace_back(params.allow_wic_fallback ? L"--allow-wic-fallback"
                                                    : L"--no-wic-fallback");
  arguments.emplace_back(params.close_on_finish ? L"--close-on-finish"
                                                 : L"--no-close-on-finish");
  switch (params.size_limit_index) {
    case 1:
      append_option(L"--image-size-limit", L"none");
      break;
    case 2:
      append_option(L"--image-size-limit", L"manual");
      if (!params.max_width_text.empty()) append_option(L"--max-width", params.max_width_text);
      if (!params.max_height_text.empty()) append_option(L"--max-height", params.max_height_text);
      if (!params.max_long_edge_text.empty()) append_option(L"--max-long-edge", params.max_long_edge_text);
      if (!params.max_short_edge_text.empty()) append_option(L"--max-short-edge", params.max_short_edge_text);
      break;
    default:
      append_option(L"--image-size-limit", L"auto");
      break;
  }
  if (is_avif) {
    append_option(L"--avif-encoder", avif_encoder_arg(params.avif_encoder_index));
    append_option(L"--avif-color-representation",
                  avif_color_representation_arg(params.avif_color_representation_index));
    append_option(L"--chroma", chroma_arg(params.chroma_index));
    append_option(L"--alpha", alpha_arg(params.alpha_policy_index));
    if (append_png_suffix) arguments.emplace_back(L"--append-png-suffix");
  } else if (is_jpgli) {
    append_option(L"--chroma", chroma_arg(params.chroma_index));
    append_option(L"--jpegli-progressive-level",
                  std::to_wstring(std::clamp(params.jpegli_progressive_index, 0, 2)));
    arguments.emplace_back(params.jpegli_progressive_index > 0 ||
                                   params.jpegli_optimize_huffman
                               ? L"--jpegli-optimize-huffman"
                               : L"--no-jpegli-optimize-huffman");
    if (params.jpegli_xyb) arguments.emplace_back(L"--jpegli-xyb");
  }
  return arguments;
}

std::vector<std::wstring> build_preset_arguments(
    std::wstring_view preset_name, std::wstring_view format) {
  return {L"--shell-window", L"--shell-convert", L"--preset",
          std::wstring{preset_name}, L"--format", std::wstring{format},
          L"--collision", L"number"};
}

}  // namespace

std::wstring build_convert_command_line(const std::filesystem::path& awj_exe,
                                        std::wstring_view format,
                                        const FormatParams& params,
                                        bool append_png_suffix) {
  auto command = quote_windows_arg(awj_exe.wstring(), true);
  for (const auto& argument :
       build_convert_arguments(format, params, append_png_suffix)) {
    append_arg(command, argument);
  }
  append_arg(command, L"-i");
  command += L" \"%1\" %*";
  return command;
}

RegistrySchema build_registry_schema(const std::filesystem::path& awj_exe,
                                     const MenuParams& menu_params,
                                     const InstallPlan& plan,
                                     std::span<const std::wstring> preset_names) {
  RegistrySchema schema{.plan = plan};
  const auto class_root = class_root_key();
  const auto inproc = class_root + L"\\InprocServer32";
  const auto file_handler = file_handler_key();
  const auto folder_handler = folder_handler_key();
  schema.parent_roots = {class_root, file_handler, folder_handler};
  std::ranges::sort(schema.parent_roots);
  schema.keys = {class_root, inproc, file_handler, folder_handler};

  awj::shell_extension::RuntimeConfiguration configuration{
      .executable = awj_exe,
      .menu_label = std::wstring{kMenuLabel}};

  for (const auto& command : kCommands) {
    if (command.append_png_suffix && !menu_params[0].install_avif_png_command) continue;
    configuration.commands.push_back({
        .label = std::wstring{command.label},
        .canonical_verb = std::wstring{command.canonical_verb},
        .arguments = build_convert_arguments(
            command.format, menu_params[command.params_index],
            command.append_png_suffix)});
  }

  for (std::size_t index = 0; index < preset_names.size(); ++index) {
    const auto group_verb = std::format(L"AWJimage.Preset.{:02}", index);
    for (const auto& spec : kCommands) {
      if (spec.append_png_suffix) continue;
      configuration.commands.push_back({
          .group_label = preset_names[index],
          .group_canonical_verb = group_verb,
          .label = std::wstring{spec.label},
          .canonical_verb = std::format(L"{}.{}", group_verb, spec.format),
          .arguments = build_preset_arguments(preset_names[index], spec.format)});
    }
  }

  auto encoded = awj::shell_extension::encode_configuration(configuration);
  if (!encoded) {
    schema.validation_error = encoded.error();
  } else {
    append_multi_string_spec(
        schema.values, class_root,
        std::wstring{awj::shell_extension::contract::configuration_value_name},
        std::move(*encoded));
  }
  append_string_spec(schema.values, class_root, L"",
                     std::wstring{awj::shell_extension::contract::friendly_name});
  append_owned_markers(schema.values, class_root);
  append_string_spec(schema.values, inproc, L"",
                     shell_extension_path(awj_exe).wstring());
  append_string_spec(schema.values, inproc, L"ThreadingModel", L"Apartment");
  for (const auto& handler : {file_handler, folder_handler}) {
    append_string_spec(schema.values, handler, L"",
                       std::wstring{awj::shell_extension::contract::class_id});
    append_owned_markers(schema.values, handler);
  }
  return schema;
}

std::expected<InstallPlan, std::string> detect_install_plan() {
  return build_install_plan();
}

namespace {

std::optional<std::filesystem::path> modern_configuration_path() {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,
                                  KF_FLAG_NO_PACKAGE_REDIRECTION,
                                  nullptr, &raw)) || raw == nullptr) {
    if (raw != nullptr) CoTaskMemFree(raw);
    return std::nullopt;
  }
  std::filesystem::path path{raw};
  CoTaskMemFree(raw);
  path /= L"AWJimage";
  path /= std::wstring{
      awj::shell_extension::contract::modern_configuration_file_name};
  return path;
}

std::expected<void, std::string> write_modern_configuration_file(
    const RegistrySchema& schema) {
  const auto spec = std::ranges::find_if(
      schema.values, [](const RegistryValueSpec& value) {
        return value.kind == RegistryValueKind::multi_string &&
               value.name == awj::shell_extension::contract::configuration_value_name;
      });
  if (spec == schema.values.end()) {
    return std::unexpected{"右键菜单现代配置缺少序列化数据。"};
  }
  auto configuration =
      awj::shell_extension::decode_configuration(spec->multi_string_value);
  if (!configuration) return std::unexpected{configuration.error()};
  std::erase_if(configuration->commands, [](const auto& command) {
    return !command.group_label.empty();
  });
  if (configuration->commands.empty()) {
    return std::unexpected{"右键菜单现代配置没有可用命令。"};
  }
  auto encoded = awj::shell_extension::encode_configuration(*configuration);
  if (!encoded) return std::unexpected{encoded.error()};

  auto path = modern_configuration_path();
  if (!path) return std::unexpected{"无法定位现代右键菜单配置路径。"};
  std::error_code error;
  std::filesystem::create_directories(path->parent_path(), error);
  if (error) {
    return std::unexpected{"无法创建现代右键菜单配置目录：" + error.message()};
  }
  auto temporary = *path;
  temporary += L".tmp";
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::unexpected{"无法创建现代右键菜单配置文件。"};
  }
  std::vector<wchar_t> buffer;
  std::size_t characters = 1;
  for (const auto& value : *encoded) characters += value.size() + 1;
  buffer.reserve(characters);
  for (const auto& value : *encoded) {
    buffer.insert(buffer.end(), value.begin(), value.end());
    buffer.push_back(L'\0');
  }
  buffer.push_back(L'\0');
  const DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  DWORD written = 0;
  const BOOL write_ok = WriteFile(file, buffer.data(), bytes, &written, nullptr);
  const BOOL flush_ok = write_ok && written == bytes && FlushFileBuffers(file);
  CloseHandle(file);
  if (!flush_ok || MoveFileExW(temporary.c_str(), path->c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
                       FALSE) {
    std::filesystem::remove(temporary, error);
    return std::unexpected{"写入现代右键菜单配置文件失败。"};
  }
  return {};
}

std::expected<void, std::string> remove_modern_configuration_file() {
  auto path = modern_configuration_path();
  if (!path) return {};
  if (DeleteFileW(path->c_str()) == FALSE && GetLastError() != ERROR_FILE_NOT_FOUND) {
    return std::unexpected{"删除现代右键菜单配置文件失败。"};
  }
  return {};
}

void notify_shell_configuration_changed() {
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

struct RegistrationLock {
  HANDLE handle{CreateMutexW(nullptr, FALSE, L"Local\\AWJimage.ContextMenu.v4")};
  bool held{};
  RegistrationLock() {
    if (handle) {
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
  auto count = read_dword(kTransaction, L"Count");
  const auto allowed = owned_root_keys();
  if (!count || !*count || **count == 0 || **count > allowed.size()) {
    return std::unexpected{"注册快照不完整，未继续修改。"};
  }
  std::vector<SnapshotRoot> roots;
  std::set<std::wstring> seen;
  for (std::size_t i = 0; i < **count; ++i) {
    const auto key = std::format(L"{}\\{:03}", kTransaction, i);
    auto path = read_string(key, L"Path");
    auto present = read_dword(key, L"Present");
    auto managed = read_dword(key, L"Managed");
    if (!path || !*path ||
        std::ranges::find(allowed, **path) == allowed.end() ||
        !seen.insert(**path).second || !present || **present > 1 ||
        !managed || **managed > 1) {
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

std::expected<void, std::string> validate_request(const std::filesystem::path& exe,
                                                const RegistrySchema& schema,
                                                std::span<const std::wstring> names) {
  if (!schema.validation_error.empty()) {
    return std::unexpected{"右键菜单配置无效：" + schema.validation_error};
  }
  std::error_code ec;
  if (!exe.is_absolute() || !std::filesystem::is_regular_file(exe, ec) || ec) {
    return std::unexpected{"右键菜单程序必须是存在的绝对路径普通文件。"};
  }
  const auto extension = shell_extension_path(exe);
  ec.clear();
  if (!std::filesystem::is_regular_file(extension, ec) || ec) {
    return std::unexpected{
        "缺少 AWJ.ShellExtension.dll，无法安装 Windows 右键菜单。"};
  }
  if (schema.plan != build_install_plan()) {
    return std::unexpected{"右键菜单支持扩展名集合无效。"};
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
    if (value.kind == RegistryValueKind::string) {
      if (value.string_value.size() >= 30000 ||
          value.string_value.find(L'\0') != std::wstring::npos ||
          value.string_value.find_first_of(L"\r\n") != std::wstring::npos) {
        return std::unexpected{"菜单参数含控制字符或超过 Windows 命令长度限制。"};
      }
    } else if (value.kind == RegistryValueKind::multi_string) {
      for (const auto& item : value.multi_string_value) {
        if (item.empty() || item.find(L'\0') != std::wstring::npos ||
            item.find_first_of(L"\r\n") != std::wstring::npos) {
          return std::unexpected{"右键菜单原子配置包含无效字符串。"};
        }
      }
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
                                         bool force_install) {
  RegistrationLock lock;
  if (!lock.held) return std::unexpected{"另一进程正在修改右键菜单，请稍后重试。"};
  if (auto recovered = recover_locked(); !recovered) return recovered;
  auto installed = is_installed();
  if (!installed) return std::unexpected{installed.error()};
  if (!*installed && !force_install) {
    auto removed = remove_modern_configuration_file();
    if (removed) notify_shell_configuration_changed();
    return removed;
  }
  std::error_code ec;
  const auto exe = std::filesystem::absolute(awj_exe, ec).lexically_normal();
  if (ec) return std::unexpected{"无法确定程序的绝对路径。"};
  const auto plan = build_install_plan();
  const auto schema = build_registry_schema(exe, menu_params, plan, preset_names);
  if (auto valid = validate_request(exe, schema, preset_names); !valid) return valid;
  if (verify_schema(schema) && verify_no_obsolete_roots(schema)) {
    auto published = write_modern_configuration_file(schema);
    if (published) {
      notify_shell_configuration_changed();
      return {};
    }
    auto removed = remove_modern_configuration_file();
    if (!removed) {
      return std::unexpected{published.error() + " " + removed.error()};
    }
    // The modern COM path falls back to the committed HKCU configuration
    // when the file cannot be published.  Clearing any stale file keeps that
    // fallback deterministic, so this is still a successful registration.
    notify_shell_configuration_changed();
    return {};
  }
  auto roots = snapshot_roots(schema);
  if (!roots) return std::unexpected{roots.error()};
  if (auto saved = begin_journal(*roots); !saved) {
    auto recovered = recover_locked();
    return std::unexpected{saved.error() + (recovered ? "" : " " + recovered.error())};
  }
  auto apply = [&]() -> std::expected<void, std::string> {
    for (const auto& parent : schema.parent_roots) {
      if (auto r = delete_tree(parent); !r) return r;
    }
    if (auto r = apply_schema(schema); !r) return r;
    if (auto r = verify_schema(schema); !r) return r;
    for (const auto& root : *roots) {
      if (!root.managed || std::ranges::find(schema.keys, root.path) != schema.keys.end()) continue;
      if (auto r = delete_tree(root.path); !r) return r;
    }
    if (auto r = verify_no_obsolete_roots(schema); !r) return r;
    if (auto r = verify_schema(schema); !r) return r;
    return {};
  };
  if (auto applied = apply(); !applied) {
    auto rolled_back = recover_locked();
    return std::unexpected{applied.error() + (rolled_back ? " 已恢复原注册。" : " 回滚失败：" + rolled_back.error())};
  }
  // The registry journal is the rollback boundary.  Publish the file-backed
  // modern configuration only after the registry transaction is committed;
  // otherwise a late commit failure could leave the file describing a
  // registration that was subsequently restored from the journal.
  if (auto committed = commit_journal(); !committed) return committed;
  auto published = write_modern_configuration_file(schema);
  if (published) {
    notify_shell_configuration_changed();
    return {};
  }
  auto removed = remove_modern_configuration_file();
  if (!removed) {
    return std::unexpected{published.error() + " " + removed.error()};
  }
  // Let the modern COM path fall back to the just-committed registry schema
  // instead of keeping a stale file-backed menu after a publish failure.
  notify_shell_configuration_changed();
  return {};
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
  if (auto committed = commit_journal(); !committed) return committed;
  auto removed = remove_modern_configuration_file();
  if (removed) notify_shell_configuration_changed();
  return removed;
}

std::expected<std::optional<std::string>, std::string> warning(
    const std::filesystem::path& awj_exe, const MenuParams& menu_params,
    std::span<const std::wstring> preset_names) {
  auto installed = is_installed();
  if (!installed) return std::unexpected{installed.error()};
  if (!*installed) return std::optional<std::string>{};
  std::error_code ec;
  const auto exe = std::filesystem::absolute(awj_exe, ec).lexically_normal();
  if (ec) return std::unexpected{"无法确定程序的绝对路径。"};
  const auto schema =
      build_registry_schema(exe, menu_params, build_install_plan(), preset_names);
  if (auto valid = validate_request(exe, schema, preset_names); !valid) {
    return std::optional<std::string>{valid.error()};
  }
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
    L"AWJImage.avif-png", L"AWJImage.jxl", L"AWJImage.jpgli"};
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

std::expected<void, std::string> remove_legacy_machine_commands() {
  auto found = legacy_machine_commands();
  if (!found) return std::unexpected{found.error()};
  HKEY raw = nullptr;
  const auto opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell",
      0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &raw);
  if (missing_registry_status(opened)) return {};
  if (opened != ERROR_SUCCESS) return std::unexpected{registry_error("打开历史系统菜单", kMachineCommandPrefix, opened)};
  RegistryKey parent{raw};
  std::string errors;
  for (const auto& name : *found) {
    const auto status = RegDeleteTreeW(parent.get(), name.c_str());
    if (status != ERROR_SUCCESS && !missing_registry_status(status)) {
      errors += registry_error("移除历史系统菜单", name, status) + " ";
    }
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  if (!errors.empty()) return std::unexpected{std::move(errors)};
  auto remaining = legacy_machine_commands();
  if (!remaining) return std::unexpected{remaining.error()};
  if (!remaining->empty()) return std::unexpected{"历史系统菜单尚未完全清除。"};
  return {};
}

}  // namespace awj::shell_context_menu
