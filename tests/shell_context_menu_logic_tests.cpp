#include "shell_context_menu.hpp"
#include "shell_extension_contract.hpp"
#include "shell_extension_core.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

bool contains(std::wstring_view text, std::wstring_view needle) {
  return text.find(needle) != std::wstring_view::npos;
}

bool contains(const std::vector<std::wstring>& values,
              std::wstring_view needle) {
  return std::ranges::find(values, needle) != values.end();
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

std::expected<awj::shell_extension::RuntimeConfiguration, std::string>
configuration_from(const awj::shell_context_menu::RegistrySchema& schema) {
  for (const auto& value : schema.values) {
    if (value.key == awj::shell_context_menu::class_root_key() &&
        value.name ==
            awj::shell_extension::contract::configuration_value_name &&
        value.kind ==
            awj::shell_context_menu::RegistryValueKind::multi_string) {
      return awj::shell_extension::decode_configuration(
          value.multi_string_value);
    }
  }
  return std::unexpected{"configuration value is missing"};
}

const awj::shell_extension::MenuCommand* find_command(
    const awj::shell_extension::RuntimeConfiguration& configuration,
    std::wstring_view verb) {
  const auto found = std::ranges::find(configuration.commands, verb,
                                       &awj::shell_extension::MenuCommand::canonical_verb);
  return found == configuration.commands.end() ? nullptr : &*found;
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

  const auto plan = build_install_plan();
  if (plan.extensions.size() != supported_extensions().size() ||
      plan.extensions.size() !=
          awj::shell_extension::supported_extensions().size()) {
    return fail("not every supported extension reaches the shell extension");
  }

  const auto exe = std::filesystem::path{L"C:\\Program Files\\AWJimage\\AWJ.exe"};
  if (shell_extension_path(exe) !=
      exe.parent_path() /
          awj::shell_extension::contract::shell_extension_filename) {
    return fail("shell extension path is not beside AWJ.exe");
  }
  const auto params_without_png = make_params(false);
  const auto schema_without_png =
      build_registry_schema(exe, params_without_png, plan);
  const auto schema_repeat = build_registry_schema(exe, params_without_png, plan);
  if (schema_without_png != schema_repeat) {
    return fail("same install inputs did not produce an idempotent schema");
  }
  const std::vector<std::wstring> expected_roots{
      class_root_key(), file_handler_key(), folder_handler_key()};
  auto actual_roots = schema_without_png.parent_roots;
  auto sorted_expected = expected_roots;
  std::ranges::sort(actual_roots);
  std::ranges::sort(sorted_expected);
  if (actual_roots != sorted_expected) {
    return fail("COM registration roots are incomplete");
  }
  for (const auto& value : schema_without_png.values) {
    if (value.name == L"ExtendedSubCommandsKey" || value.name == L"SubCommands") {
      return fail("static cascade registration survived in the v5 schema");
    }
  }

  const auto configuration_without_png =
      configuration_from(schema_without_png);
  if (!configuration_without_png) return fail(configuration_without_png.error());
  if (configuration_without_png->executable != exe ||
      configuration_without_png->menu_label != L"AWJimage 转换" ||
      configuration_without_png->commands.size() != 5 ||
      find_command(*configuration_without_png,
                   L"AWJimage.Convert.40.avif-png") != nullptr) {
    return fail("AVIF.png-off COM configuration is incorrect");
  }

  const auto params_with_png = make_params(true);
  const auto schema_with_png = build_registry_schema(exe, params_with_png, plan);
  const auto configuration_with_png = configuration_from(schema_with_png);
  if (!configuration_with_png) return fail(configuration_with_png.error());
  const auto* avif_png =
      find_command(*configuration_with_png, L"AWJimage.Convert.40.avif-png");
  if (configuration_with_png->commands.size() != 6 || avif_png == nullptr ||
      !contains(avif_png->arguments, L"--append-png-suffix")) {
    return fail("AVIF.png-on COM configuration is incorrect");
  }

  const auto avif_command =
      build_convert_command_line(exe, L"avif", params_with_png[0]);
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
  const auto png_command =
      build_convert_command_line(exe, L"png", params_with_png[4]);
  if (!contains(png_command, L"--quality 73") ||
      contains(png_command, L"--speed") ||
      !contains(png_command, L"--bit-depth 10")) {
    return fail("PNG shell command format-specific options changed");
  }
  const auto jpgli_command =
      build_convert_command_line(exe, L"jpgli", params_with_png[3]);
  if (!contains(jpgli_command, L"--jpegli-progressive-level 2") ||
      contains(jpgli_command, L"--no-jpegli-optimize-huffman") ||
      !contains(jpgli_command, L"--jpegli-xyb")) {
    return fail("JPGLI shell command format-specific options changed");
  }

  for (int count : {0, 1, 10}) {
    std::vector<std::wstring> names;
    for (int i = 0; i < count; ++i) {
      names.push_back(L"预设 空格 " + std::to_wstring(i));
    }
    const auto schema =
        build_registry_schema(exe, params_without_png, plan, names);
    const auto configuration = configuration_from(schema);
    if (!configuration) return fail(configuration.error());
    std::size_t grouped = 0;
    for (const auto& command : configuration->commands) {
      if (command.group_label.empty()) continue;
      ++grouped;
      if (!contains(command.arguments, L"--preset") ||
          !contains(command.arguments, L"--format") ||
          contains(command.arguments, L"--quality") ||
          contains(command.arguments, L"--append-png-suffix") ||
          contains(command.arguments, L"--jobs")) {
        return fail("preset command embeds format resources or AVIF.png");
      }
    }
    if (grouped != static_cast<std::size_t>(count * 5)) {
      return fail("preset groups did not reach the COM configuration");
    }
  }

  const auto legacy = legacy_root_keys();
  if (std::ranges::find(legacy, L"Software\\Classes\\AWJImage.ContextMenu") ==
          legacy.end() ||
      std::ranges::find(
          legacy,
          L"Software\\Classes\\SystemFileAssociations\\.jpg\\shell\\AWJImage") ==
          legacy.end()) {
    return fail("legacy migration roots are incomplete");
  }
  const auto owned = owned_root_keys();
  auto sorted = owned;
  std::ranges::sort(sorted);
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end() ||
      std::ranges::find(owned, class_root_key()) == owned.end() ||
      std::ranges::find(owned, file_handler_key()) == owned.end() ||
      std::ranges::find(owned, folder_handler_key()) == owned.end() ||
      std::ranges::find(owned, shared_tree_key()) == owned.end() ||
      std::ranges::find(owned, legacy_shared_tree_key()) == owned.end()) {
    return fail("owned-root cleanup plan is not stable or complete");
  }
  return 0;
}
