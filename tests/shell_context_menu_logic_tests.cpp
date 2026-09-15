#include "shell_context_menu.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

bool contains(std::wstring_view text, std::wstring_view needle) {
  return text.find(needle) != std::wstring_view::npos;
}

const awj::shell_context_menu::RegistryValueSpec* find_value(
    const awj::shell_context_menu::RegistrySchema& schema,
    std::wstring_view key, std::wstring_view name) {
  for (const auto& value : schema.values) {
    if (value.key == key && value.name == name) return &value;
  }
  return nullptr;
}

const awj::shell_context_menu::RegistryValueSpec* find_machine_value(
    const awj::shell_context_menu::RegistrySchema& schema,
    std::wstring_view key, std::wstring_view name) {
  for (const auto& value : schema.machine_values) {
    if (value.key == key && value.name == name) return &value;
  }
  return nullptr;
}

awj::shell_context_menu::MenuParams make_params(bool avif_png) {
  awj::shell_context_menu::MenuParams params{};
  for (auto& item : params) {
    item.quality_text = L"73";
    item.bit_depth_text = L"10";
    item.speed_text = L"6";
    item.close_on_finish = true;
    item.allow_wic_fallback = true;
    item.size_limit_index = 2;
    item.max_width_text = L"4096";
    item.max_height_text = L"2160";
  }
  params[0].install_avif_png_command = avif_png;
  params[0].avif_encoder_index = 1;
  params[0].avif_color_representation_index = 1;
  params[0].chroma_index = 3;
  params[0].alpha_policy_index = 0;
  params[3].jpegli_progressive_index = 2;
  params[3].jpegli_optimize_huffman = false;
  params[3].jpegli_xyb = true;
  return params;
}

}  // namespace

int main() {
  using namespace awj::shell_context_menu;

  const auto commands = command_specs();
  if (commands.size() != 6) return fail("shell command count changed unexpectedly");
  for (const auto& command : commands) {
    if (!command.canonical_verb.starts_with(L"AWJimage.Convert.")) {
      return fail("shell subverb is not vendor-qualified");
    }
  }
  if (parent_canonical_verb != L"AWJimage.Convert") {
    return fail("shell parent verb is not vendor-qualified");
  }
  if (machine_command_store_name(L"png") != L"AWJImage.png" ||
      machine_command_store_key(L"png") !=
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\AWJImage.png") {
    return fail("static CommandStore naming/path changed unexpectedly");
  }

  const auto plan = build_install_plan();
  if (plan.extensions.size() != supported_extensions().size()) {
    return fail("not every supported extension is registered");
  }

  const auto exe = std::filesystem::path{L"C:\\Program Files\\AWJimage\\AWJ.exe"};
  const auto params_without_png = make_params(false);
  const auto schema_without_png = build_registry_schema(exe, params_without_png, plan);
  const auto schema_repeat = build_registry_schema(exe, params_without_png, plan);
  if (schema_without_png != schema_repeat) {
    return fail("same install inputs did not produce an idempotent schema");
  }
  std::vector<std::wstring> expected_parents{
      image_parent_key(), directory_parent_key(), ico_parent_key()};
  for (const auto extension : supported_extensions()) {
    expected_parents.push_back(extension_parent_key(extension));
    expected_parents.push_back(class_extension_parent_key(extension));
  }
  std::ranges::sort(expected_parents);
  expected_parents.erase(std::unique(expected_parents.begin(), expected_parents.end()),
                         expected_parents.end());
  auto actual_parents = schema_without_png.parent_roots;
  std::ranges::sort(actual_parents);
  if (actual_parents != expected_parents) {
    return fail("parent roots do not cover image, directory, ico, SFA and class associations");
  }
  for (const auto extension : supported_extensions()) {
    if (std::ranges::find(schema_without_png.parent_roots,
                          extension_parent_key(extension)) ==
            schema_without_png.parent_roots.end() ||
        std::ranges::find(schema_without_png.parent_roots,
                          class_extension_parent_key(extension)) ==
            schema_without_png.parent_roots.end()) {
      return fail("extension parent is missing");
    }
  }

  const auto without_png_subcommands = static_subcommands(false);
  const auto with_png_subcommands = static_subcommands(true);
  for (const auto& parent : schema_without_png.parent_roots) {
    const auto subcommands = find_value(schema_without_png, parent, L"SubCommands");
    if (subcommands == nullptr || subcommands->kind != RegistryValueKind::string ||
        subcommands->string_value != without_png_subcommands) {
      return fail("static SubCommands parent schema is incorrect");
    }
    for (const auto& value : schema_without_png.values) {
      if (value.key == parent && value.name == L"ExtendedSubCommandsKey") {
        return fail("regular parent still uses ExtendedSubCommandsKey");
      }
    }
  }
  for (const auto& command : commands) {
    if (command.append_png_suffix) continue;
    const auto command_name = command.canonical_verb.substr(
        command.canonical_verb.rfind(L'.') + 1);
    const auto key = machine_command_store_key(command_name);
    if (std::ranges::find(schema_without_png.machine_keys, key) ==
            schema_without_png.machine_keys.end() ||
        find_machine_value(schema_without_png, key + L"\\command", L"") == nullptr) {
      return fail("CommandStore command is missing from static schema");
    }
  }
  const auto avif_png_machine_key = machine_command_store_key(L"avif-png");
  if (std::ranges::find(schema_without_png.machine_keys, avif_png_machine_key) !=
          schema_without_png.machine_keys.end() ||
      find_machine_value(schema_without_png, avif_png_machine_key + L"\\command", L"") !=
          nullptr) {
    return fail("disabled AVIF.png command survived static schema");
  }

  const auto params_with_png = make_params(true);
  const auto schema_with_png = build_registry_schema(exe, params_with_png, plan);
  bool found_avif_png = false;
  const auto avif_png_command_key = machine_command_store_key(L"avif-png") + L"\\command";
  for (const auto& value : schema_with_png.machine_values) {
    if (value.key == avif_png_command_key && value.name.empty() &&
        contains(value.string_value, L"--append-png-suffix")) {
      found_avif_png = true;
    }
  }
  if (!found_avif_png) return fail("AVIF.png-on schema is missing its command");

  for (const auto& parent : schema_with_png.parent_roots) {
    const auto subcommands = find_value(schema_with_png, parent, L"SubCommands");
    if (subcommands == nullptr || subcommands->string_value != with_png_subcommands) {
      return fail("AVIF.png-on SubCommands parent schema is incorrect");
    }
  }

  const auto avif_command = build_convert_command_line(exe, L"avif", params_with_png[0]);
  if (!avif_command.starts_with(L"\"C:\\Program Files\\AWJimage\\AWJ.exe\"") ||
      !contains(avif_command, L"--shell-window") ||
      !contains(avif_command, L"--shell-convert") ||
      !contains(avif_command, L"--format avif") ||
      !contains(avif_command, L"--collision number") ||
      !contains(avif_command, L"--avif-encoder aom") ||
      !contains(avif_command, L"--avif-color-representation source") ||
      !contains(avif_command, L"--chroma 420") ||
      !contains(avif_command, L"--alpha force") ||
      !avif_command.ends_with(L"-i \"%1\" %*")) {
    return fail("AVIF shell command generation changed CLI semantics");
  }
  const auto avif_png_command = build_convert_command_line(exe, L"avif", params_with_png[0], true);
  if (!contains(avif_png_command, L"--append-png-suffix")) {
    return fail("AVIF.png command did not include suffix switch");
  }
  const auto png_command = build_convert_command_line(exe, L"png", params_with_png[4]);
  if (!contains(png_command, L"--quality 73") || contains(png_command, L"--speed") ||
      !contains(png_command, L"--bit-depth 10")) {
    return fail("PNG shell command format-specific options changed");
  }
  const auto webp_command = build_convert_command_line(exe, L"webp", params_with_png[1]);
  if (!contains(webp_command, L"--quality 73") ||
      !contains(webp_command, L"--bit-depth 10") ||
      !contains(webp_command, L"--speed 6") ||
      contains(webp_command, L"--chroma")) {
    return fail("WebP shell command format-specific options changed");
  }
  const auto jxl_command = build_convert_command_line(exe, L"jxl", params_with_png[2]);
  if (!contains(jxl_command, L"--quality 73") ||
      !contains(jxl_command, L"--speed 6") ||
      contains(jxl_command, L"--bit-depth")) {
    return fail("JXL shell command format-specific options changed");
  }
  const auto jpgli_command = build_convert_command_line(exe, L"jpgli", params_with_png[3]);
  if (!contains(jpgli_command, L"--quality 73") ||
      !contains(jpgli_command, L"--bit-depth 10") ||
      !contains(jpgli_command, L"--chroma auto") ||
      !contains(jpgli_command, L"--jpegli-progressive-level 2") ||
      contains(jpgli_command, L"--no-jpegli-optimize-huffman") ||
      !contains(jpgli_command, L"--jpegli-xyb") ||
      contains(jpgli_command, L"--speed")) {
    return fail("JPGLI shell command format-specific options changed");
  }

  for (int count : {0, 1, 10}) {
    std::vector<std::wstring> names;
    for (int i = 0; i < count; ++i) names.push_back(L"预设 空格 " + std::to_wstring(i));
    const auto schema = build_registry_schema(exe, params_without_png, plan, names, 1);
    int preset_command_count = 0;
    for (const auto& value : schema.values) {
      if (contains(value.string_value, L"--preset")) {
        ++preset_command_count;
        if (!contains(value.string_value, L"--format") || contains(value.string_value, L"--quality") ||
            contains(value.string_value, L"--append-png-suffix") || contains(value.string_value, L"--jobs")) {
          return fail("preset command embeds format values/resources or AVIF.png");
        }
      }
    }
    if (preset_command_count != count * 5 ||
        (count > 0 && schema == schema_without_png)) {
      return fail("preset subtree/slot schema failed");
    }
    std::size_t preset_pointers = 0;
    for (const auto& value : schema.values) {
      if (value.name == L"ExtendedSubCommandsKey") {
        ++preset_pointers;
        if (!contains(value.string_value, L"AWJimage.ContextMenu")) {
          return fail("preset ExtendedSubCommandsKey target is invalid");
        }
      }
    }
    if (preset_pointers !=
            (count == 0 ? 0u : schema.parent_roots.size() + static_cast<std::size_t>(count))) {
      return fail("preset pointer schema is duplicated or missing");
    }
  }

  const auto legacy = legacy_root_keys();
  if (std::ranges::find(legacy, L"Software\\Classes\\AWJImage.ContextMenu") == legacy.end() ||
      std::ranges::find(legacy, L"Software\\Classes\\SystemFileAssociations\\.jpg\\shell\\AWJImage") == legacy.end() ||
      std::ranges::find(legacy, L"Software\\Classes\\icofile\\shell\\AWJImage") == legacy.end()) {
    return fail("legacy migration roots are incomplete");
  }
  const auto owned = owned_root_keys();
  auto sorted = owned;
  std::ranges::sort(sorted);
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end() ||
      std::ranges::find(owned, shared_tree_key()) == owned.end() ||
      std::ranges::find(owned, legacy_shared_tree_key()) == owned.end()) {
    return fail("owned-root cleanup plan is not stable/unique");
  }
  return 0;
}
