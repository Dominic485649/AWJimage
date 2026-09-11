#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

import awj.preset;
import awj.config;

namespace fs = std::filesystem;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template <class T> T require(std::expected<T, std::string> value) {
  if (!value) throw std::runtime_error(value.error());
  return std::move(*value);
}
void require(std::expected<void, std::string> value) {
  if (!value) throw std::runtime_error(value.error());
}

int main() try {
  const auto directory = require(awj::user_preset_directory());
  check(!fs::exists(directory), "preset test directory must be isolated and absent");
  // The target has its own runtime directory; remove only files made by this run.
  struct Cleanup {
    fs::path directory;
    ~Cleanup() { std::error_code ec; fs::remove_all(directory, ec); }
  } cleanup{directory};
  auto preset = awj::default_user_preset();
  preset.name = "照片 测试";
  preset.description = "keep original resources";
  preset.formats[0].max_jobs = 3;
  preset.formats[0].memory_limit_bytes = 1536ull * 1024 * 1024;
  preset.formats[0].visual_quality = 87;
  preset.shell_menu = true;
  preset.source_path = require(awj::save_user_preset(preset, false));
  check(!awj::save_user_preset(preset, false), "duplicate create accepted");
  const auto original_path = preset.source_path;
  preset.name = "重命名 测试";
  require(awj::save_user_preset(preset, true));
  check(require(awj::find_user_preset(preset.name)).source_path == original_path,
        "rename did not atomically edit the original file");
  check(!awj::find_user_preset("照片 测试"), "old name survived rename");
  auto shell = require(awj::parse_arguments_with_user_preset({L"--shell-convert", L"--preset",
      L"重命名 测试", L"--format", L"avif", L"-i", L"input.png"}));
  check(!shell.config.visual_quality && shell.config.max_jobs == awj::default_max_jobs() &&
      shell.config.memory_limit_bytes == 0, "shell preset retained explicit resource/visual quality controls");
  const auto reread = require(awj::load_user_preset_file(original_path));
  check(reread.formats[0].memory_limit_bytes == preset.formats[0].memory_limit_bytes &&
      reread.formats[0].visual_quality == 87 && reread.formats[0].max_jobs == 3,
      "shell execution rewrote source preset values");
  const auto duplicate = directory / "duplicate.jsonc";
  fs::copy_file(original_path, duplicate);
  auto catalog = require(awj::list_user_presets());
  check(catalog.presets.empty() && catalog.errors.size() == 2, "duplicate disk names were not excluded");
  fs::remove(duplicate);
  int sync_calls = 0;
  const auto sync_failure = [&]() -> std::expected<void, std::string> {
    if (++sync_calls == 1) return std::unexpected{"injected synchronization failure"};
    return {};
  };
  auto renamed = preset;
  renamed.name = "failed rename";
  check(!awj::save_user_preset(renamed, true, sync_failure), "failed menu sync accepted");
  check(sync_calls == 2 && require(awj::find_user_preset(preset.name)).name == preset.name,
        "rename failure did not restore source and synchronize again");
  sync_calls = 0;
  check(!awj::delete_user_preset(preset, sync_failure), "delete synchronization failure accepted");
  check(fs::exists(original_path) && sync_calls == 2, "delete failure did not restore original");
  std::vector<awj::UserPreset> others;
  for (int i = 1; i <= 10; ++i) {
    auto value = awj::default_user_preset();
    value.name = "test preset " + std::to_string(i);
    value.shell_menu = true;
    auto saved = awj::save_user_preset(value, false);
    if (i == 10) check(!saved, "eleventh injected preset accepted");
    else { value.source_path = require(std::move(saved)); others.push_back(value); }
  }
  check(require(awj::injected_user_preset_names()).size() == 10, "injection count incorrect");
  renamed = preset;
  renamed.name = others.front().name;
  check(!awj::save_user_preset(renamed, true), "rename over another preset accepted");
  for (const auto& value : others) require(awj::delete_user_preset(value));
  auto collision = awj::default_user_preset();
  collision.name = "a/b";
  collision.source_path = require(awj::save_user_preset(collision, false));
  auto other = collision;
  other.name = "a?b";
  other.source_path.clear();
  check(!awj::save_user_preset(other, false), "generated filename collision accepted");
  require(awj::delete_user_preset(collision));
  check(!awj::delete_user_preset(awj::default_user_preset()), "built-in deletion accepted");
  require(awj::delete_user_preset(preset));
  check(require(awj::list_user_presets()).presets.empty(), "deleted preset remains selectable");
  std::puts("Preset uniqueness, self-edit, rename/delete rollback, shell resources and injection limits passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
