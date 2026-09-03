#include "shell_context_menu.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shlwapi.h>

#include <algorithm>
#include <format>
#include <set>
#include <utility>

namespace awj::shell_context_menu {
namespace {

constexpr std::wstring_view kImageParent =
    L"Software\\Classes\\SystemFileAssociations\\image\\shell\\AWJimage.Convert";
constexpr std::wstring_view kDirectoryParent =
    L"Software\\Classes\\Directory\\shell\\AWJimage.Convert";
constexpr std::wstring_view kSharedTree =
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
    {.canonical_verb = L"AWJimage.Convert.png", .label = L"转换为 PNG", .format = L"png", .params_index = 4},
    {.canonical_verb = L"AWJimage.Convert.webp", .label = L"转换为 WebP", .format = L"webp", .params_index = 1},
    {.canonical_verb = L"AWJimage.Convert.avif", .label = L"转换为 AVIF", .format = L"avif", .params_index = 0},
    {.canonical_verb = L"AWJimage.Convert.avif-png", .label = L"转换为 AVIF.png", .format = L"avif", .params_index = 0, .append_png_suffix = true},
    {.canonical_verb = L"AWJimage.Convert.jxl", .label = L"转换为 JXL", .format = L"jxl", .params_index = 2},
    {.canonical_verb = L"AWJimage.Convert.jpgli", .label = L"转换为 JPGLI", .format = L"jpgli", .params_index = 3},
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

std::expected<RegistryKey, std::string> create_key(std::wstring_view subkey) {
  HKEY raw = nullptr;
  const std::wstring path{subkey};
  const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                                      REG_OPTION_NON_VOLATILE,
                                      KEY_READ | KEY_WRITE, nullptr, &raw, nullptr);
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
  switch (index) {
    case 1: return L"svt";
    case 2: return L"aom";
    case 3: return L"zenrav1e";
    default: return L"auto";
  }
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
  roots.reserve(std::size(kSupportedExtensions) + 2);
  roots.push_back(image_parent_key());
  roots.push_back(directory_parent_key());
  for (const auto extension : kSupportedExtensions) {
    roots.push_back(extension_parent_key(extension));
  }
  return roots;
}

std::expected<void, std::string> cleanup_owned_roots() {
  std::string errors;
  for (const auto& root : owned_root_keys()) {
    if (auto removed = delete_tree(root); !removed) {
      if (!errors.empty()) errors += " ";
      errors += removed.error();
    }
  }
  if (!errors.empty()) return std::unexpected{std::move(errors)};
  return {};
}

std::expected<void, std::string> apply_schema(const RegistrySchema& schema) {
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

std::vector<std::wstring> expected_command_keys(const MenuParams& menu_params) {
  std::vector<std::wstring> names;
  for (const auto& command : kCommands) {
    if (command.append_png_suffix && !menu_params[0].install_avif_png_command) continue;
    names.emplace_back(command.canonical_verb);
  }
  std::ranges::sort(names);
  return names;
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
std::wstring shared_tree_key() { return std::wstring{kSharedTree}; }

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
  roots.push_back(shared_tree_key());
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

InstallPlan build_install_plan(std::span<const ExtensionPerception> perceptions) {
  InstallPlan plan;
  for (const auto& perception : perceptions) {
    if (!perception.is_image) plan.fallback_extensions.push_back(perception.extension);
  }
  std::ranges::sort(plan.fallback_extensions);
  plan.fallback_extensions.erase(
      std::unique(plan.fallback_extensions.begin(), plan.fallback_extensions.end()),
      plan.fallback_extensions.end());
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
  if (!is_png && !params.quality_text.empty()) append_option(command, L"--quality", params.quality_text);
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
  } else if (is_jpgli) {
    append_option(command, L"--chroma", chroma_arg(params.chroma_index));
    append_option(command, L"--jpegli-progressive-level",
                  std::to_wstring(std::clamp(params.jpegli_progressive_index, 0, 2)));
    append_arg(command, params.jpegli_optimize_huffman
                            ? L"--jpegli-optimize-huffman"
                            : L"--no-jpegli-optimize-huffman");
    if (params.jpegli_xyb) append_arg(command, L"--jpegli-xyb");
  }
  command += L" -i \"%1\" %*";
  return command;
}

RegistrySchema build_registry_schema(const std::filesystem::path& awj_exe,
                                     const MenuParams& menu_params,
                                     const InstallPlan& plan) {
  RegistrySchema schema{.plan = plan};
  schema.parent_roots.push_back(image_parent_key());
  for (const auto& extension : plan.fallback_extensions) {
    schema.parent_roots.push_back(extension_parent_key(extension));
  }
  schema.parent_roots.push_back(directory_parent_key());
  std::ranges::sort(schema.parent_roots);
  schema.parent_roots.erase(std::unique(schema.parent_roots.begin(), schema.parent_roots.end()),
                            schema.parent_roots.end());

  const auto icon = icon_value(awj_exe);
  const auto shared = shared_tree_key();
  append_owned_markers(schema.values, shared);
  for (const auto& command : kCommands) {
    if (command.append_png_suffix && !menu_params[0].install_avif_png_command) continue;
    const auto verb_key = std::format(L"{}\\shell\\{}", shared, command.canonical_verb);
    append_string_spec(schema.values, verb_key, L"MUIVerb", std::wstring{command.label});
    append_string_spec(schema.values, verb_key, L"Icon", icon);
    append_string_spec(schema.values, verb_key, L"MultiSelectModel", std::wstring{kMultiSelectModel});
    const auto command_key = verb_key + L"\\command";
    append_string_spec(
        schema.values, command_key, L"",
        build_convert_command_line(awj_exe, command.format,
                                   menu_params[command.params_index],
                                   command.append_png_suffix));
  }

  for (const auto& parent : schema.parent_roots) {
    append_string_spec(schema.values, parent, L"MUIVerb", std::wstring{kMenuLabel});
    append_string_spec(schema.values, parent, L"Icon", icon);
    append_string_spec(schema.values, parent, std::wstring{kExtendedSubCommandsKey},
                       std::wstring{shared_tree_reference});
    append_string_spec(schema.values, parent, L"MultiSelectModel", std::wstring{kMultiSelectModel});
    append_owned_markers(schema.values, parent);
  }
  return schema;
}

std::expected<InstallPlan, std::string> detect_install_plan() {
  std::vector<ExtensionPerception> perceptions;
  perceptions.reserve(std::size(kSupportedExtensions));
  for (const auto extension : kSupportedExtensions) {
    PERCEIVED perceived = PERCEIVED_TYPE_UNSPECIFIED;
    PERCEIVEDFLAG flags = PERCEIVEDFLAG_UNDEFINED;
    const std::wstring extension_storage{extension};
    const HRESULT status = AssocGetPerceivedType(extension_storage.c_str(), &perceived,
                                                 &flags, nullptr);
    perceptions.push_back(ExtensionPerception{
        .extension = extension_storage,
        .is_image = SUCCEEDED(status) && perceived == PERCEIVED_TYPE_IMAGE});
  }
  return build_install_plan(perceptions);
}

std::expected<void, std::string> install(const std::filesystem::path& awj_exe,
                                         const MenuParams& menu_params) {
  auto plan = detect_install_plan();
  if (!plan) return std::unexpected{plan.error()};
  if (auto cleaned = cleanup_owned_roots(); !cleaned) return cleaned;
  const auto schema = build_registry_schema(awj_exe, menu_params, *plan);
  if (auto applied = apply_schema(schema); !applied) {
    auto rollback = cleanup_owned_roots();
    if (!rollback) {
      return std::unexpected{applied.error() + " 回滚右键菜单注册失败：" + rollback.error()};
    }
    return applied;
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

std::expected<void, std::string> remove() {
  auto removed = cleanup_owned_roots();
  if (!removed) return removed;
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

std::expected<std::optional<std::string>, std::string> warning(
    const std::filesystem::path& awj_exe,
    const MenuParams& menu_params) {
  bool any_owned = false;
  for (const auto& root : owned_root_keys()) {
    auto exists = key_exists(root);
    if (!exists) return std::unexpected{exists.error()};
    any_owned = any_owned || *exists;
  }
  if (!any_owned) return std::optional<std::string>{};

  for (const auto& root : legacy_root_keys()) {
    auto exists = key_exists(root);
    if (!exists) return std::unexpected{exists.error()};
    if (*exists) {
      return std::optional<std::string>{"检测到旧版右键菜单，点击移除后重新安装。"};
    }
  }

  auto plan = detect_install_plan();
  if (!plan) return std::unexpected{plan.error()};
  const auto schema = build_registry_schema(awj_exe, menu_params, *plan);
  for (const auto& spec : schema.values) {
    auto verified = verify_spec(spec);
    if (!verified) return std::unexpected{verified.error()};
    if (!*verified) {
      return std::optional<std::string>{
          "右键菜单与当前版本、程序路径或菜单参数不一致，请重新安装右键菜单。"};
    }
  }

  const std::set<std::wstring> expected_parents(schema.parent_roots.begin(),
                                                schema.parent_roots.end());
  for (const auto& root : current_parent_roots_for_all_extensions()) {
    auto exists = key_exists(root);
    if (!exists) return std::unexpected{exists.error()};
    if (*exists != expected_parents.contains(root)) {
      return std::optional<std::string>{
          "右键菜单 fallback 与当前系统文件分类不一致，请重新安装右键菜单。"};
    }
  }
  const auto current_icofile =
      std::format(L"Software\\Classes\\icofile\\shell\\{}", parent_canonical_verb);
  auto icofile_exists = key_exists(current_icofile);
  if (!icofile_exists) return std::unexpected{icofile_exists.error()};
  if (*icofile_exists) {
    return std::optional<std::string>{
        "右键菜单仍含不需要的 icofile 专用注册，请重新安装右键菜单。"};
  }

  const auto shell_key = shared_tree_key() + L"\\shell";
  auto shell_exists = key_exists(shell_key);
  if (!shell_exists) return std::unexpected{shell_exists.error()};
  if (!*shell_exists) {
    return std::optional<std::string>{
        "右键菜单共享命令树不完整，请重新安装右键菜单。"};
  }
  auto actual_commands = child_keys(shell_key);
  if (!actual_commands) return std::unexpected{actual_commands.error()};
  if (*actual_commands != expected_command_keys(menu_params)) {
    return std::optional<std::string>{
        "右键菜单共享命令树与当前 AVIF.png 设置不一致，请重新安装右键菜单。"};
  }
  return std::optional<std::string>{};
}

}  // namespace awj::shell_context_menu
