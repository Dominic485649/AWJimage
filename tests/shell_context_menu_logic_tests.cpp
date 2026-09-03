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
  params[0].avif_encoder_index = 2;
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

  const std::array perceptions{
      ExtensionPerception{L".jpg", true},
      ExtensionPerception{L".ico", true},
      ExtensionPerception{L".awsraw", false},
      ExtensionPerception{L".hdp", false}};
  const auto plan = build_install_plan(perceptions);
  if (plan.fallback_extensions != std::vector<std::wstring>{L".awsraw", L".hdp"}) {
    return fail("perceived-type fallback planning is incorrect");
  }

  const auto exe = std::filesystem::path{L"C:\\Program Files\\AWJimage\\AWJ.exe"};
  const auto params_without_png = make_params(false);
  const auto schema_without_png = build_registry_schema(exe, params_without_png, plan);
  const auto schema_repeat = build_registry_schema(exe, params_without_png, plan);
  if (schema_without_png != schema_repeat) {
    return fail("same install inputs did not produce an idempotent schema");
  }
  if (schema_without_png.parent_roots.size() != 4 ||
      std::ranges::find(schema_without_png.parent_roots, image_parent_key()) == schema_without_png.parent_roots.end() ||
      std::ranges::find(schema_without_png.parent_roots, directory_parent_key()) == schema_without_png.parent_roots.end() ||
      std::ranges::find(schema_without_png.parent_roots, extension_parent_key(L".jpg")) != schema_without_png.parent_roots.end() ||
      std::ranges::find(schema_without_png.parent_roots, extension_parent_key(L".ico")) != schema_without_png.parent_roots.end() ||
      std::ranges::find(schema_without_png.parent_roots, extension_parent_key(L".awsraw")) == schema_without_png.parent_roots.end()) {
    return fail("install parent roots do not match perceived-type plan");
  }

  std::size_t pointer_values = 0;
  std::size_t avif_png_commands = 0;
  for (const auto& value : schema_without_png.values) {
    if (value.name == L"ExtendedSubCommandsKey") {
      ++pointer_values;
      if (value.kind != RegistryValueKind::string ||
          value.string_value != shared_tree_reference ||
          contains(value.key, L"ExtendedSubCommandsKey\\")) {
        return fail("ExtendedSubCommandsKey was not modeled as a shared-tree pointer value");
      }
    }
    if (contains(value.key, L"AWJimage.Convert.avif-png\\command")) ++avif_png_commands;
  }
  if (pointer_values != schema_without_png.parent_roots.size() || avif_png_commands != 0) {
    return fail("shared-tree pointer or AVIF.png-off schema is incorrect");
  }

  const auto params_with_png = make_params(true);
  const auto schema_with_png = build_registry_schema(exe, params_with_png, plan);
  bool found_avif_png = false;
  for (const auto& value : schema_with_png.values) {
    if (contains(value.key, L"AWJimage.Convert.avif-png\\command") &&
        value.name.empty() && contains(value.string_value, L"--append-png-suffix")) {
      found_avif_png = true;
    }
  }
  if (!found_avif_png) return fail("AVIF.png-on schema is missing its command");

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
  if (contains(png_command, L"--quality") || contains(png_command, L"--speed") ||
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
      !contains(jpgli_command, L"--no-jpegli-optimize-huffman") ||
      !contains(jpgli_command, L"--jpegli-xyb") ||
      contains(jpgli_command, L"--speed")) {
    return fail("JPGLI shell command format-specific options changed");
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
      std::ranges::find(owned, shared_tree_key()) == owned.end()) {
    return fail("owned-root cleanup plan is not stable/unique");
  }
  return 0;
}
