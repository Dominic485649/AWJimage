#include <cstdio>
#include <iterator>
#include <nlohmann/json.hpp>
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
  preset.formats[0].image_size_limit.mode = awj::ImageSizeLimitMode::manual;
  preset.formats[0].image_size_limit.scale_percent = 80;
  preset.formats[0].menu_strip_metadata = true;
  preset.formats[0].menu_allow_wic_fallback = false;
  preset.formats[0].menu_close_on_finish = false;
  preset.formats[0].menu_install_avif_png_command = true;
  preset.formats[2].jxl_jpeg_lossless = false;
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
#ifdef _WIN32
  check(shell.config.visual_quality == 87 && shell.config.max_jobs == 3 &&
      shell.config.memory_limit_bytes == 1536ull * 1024 * 1024 &&
      shell.config.strip_metadata && !shell.config.allow_wic_fallback &&
      !shell.config.shell_close_on_finish,
      "shell preset lost format resources or menu options");
#else
  check(!shell.config.visual_quality && shell.config.max_jobs == awj::default_max_jobs() &&
      shell.config.memory_limit_bytes == 0, "Linux shell resource behavior changed");
#endif
  const auto reread = require(awj::load_user_preset_file(original_path));
  check(reread.formats[0].memory_limit_bytes == preset.formats[0].memory_limit_bytes &&
      reread.formats[0].visual_quality == 87 && reread.formats[0].max_jobs == 3 &&
      reread.formats[0].image_size_limit.scale_percent.value_or(0) == 80 &&
      reread.formats[2].jxl_jpeg_lossless == false &&
      reread.formats[0].menu_install_avif_png_command &&
      reread.formats[0].menu_strip_metadata &&
      !reread.formats[0].menu_allow_wic_fallback &&
      !reread.formats[0].menu_close_on_finish,
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
  auto unmarked = preset;
  unmarked.shell_menu = false;
  require(awj::save_user_preset(unmarked, true));
  auto selected_for_install = unmarked;
  selected_for_install.shell_menu = true;
  sync_calls = 0;
  check(!awj::save_user_preset(selected_for_install, true, sync_failure),
        "failed install accepted selected preset injection");
  check(sync_calls == 2 && !require(awj::find_user_preset(preset.name)).shell_menu,
        "failed install did not roll back selected preset injection");
  require(awj::save_user_preset(selected_for_install, true));
  const auto installed_preset = require(awj::find_user_preset(preset.name));
  check(installed_preset.shell_menu &&
        installed_preset.formats[0].visual_quality == preset.formats[0].visual_quality &&
        installed_preset.formats[0].memory_limit_bytes == preset.formats[0].memory_limit_bytes,
        "install changed saved preset parameters while marking injection");
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
  const auto legacy_path = directory / "legacy.jsonc";
  const auto read = [](const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, {}};
  };
  {
    std::ofstream legacy(legacy_path);
    legacy << R"({"schema":1,"name":"legacy optional","description":"", "unknown":{"future":true},
      "formats":{"avif":{"speed":[6],"bit_depth":[10],"visual_quality":[null]}}})";
  }
  auto migrated = require(awj::load_user_preset_file(legacy_path));
  check(migrated.formats[0].speed == 6 && migrated.formats[0].bit_depth == 10 &&
        !migrated.formats[0].visual_quality, "legacy optional fields did not load");
  require(awj::save_user_preset(migrated, true));
  const auto normalized = read(legacy_path);
  const auto document = nlohmann::json::parse(normalized, nullptr, true, true);
  check(!document.contains("unknown") && document["formats"]["avif"]["speed"] == 6 &&
        document["formats"]["avif"]["bit_depth"] == 10 &&
        document["formats"]["avif"]["visual_quality"].is_null(),
        "legacy save did not normalize optional values or remove extra fields");
  require(awj::save_user_preset(require(awj::load_user_preset_file(legacy_path)), true));
  check(read(legacy_path) == normalized, "preset migration is not idempotent");
  require(awj::delete_user_preset(migrated));
  for (const auto* invalid : {"[]", "[6,7]", "[true]", "true"}) {
    std::ofstream bad(legacy_path);
    bad << "{\"schema\":1,\"name\":\"invalid\",\"description\":\"\",\"formats\":{\"avif\":{\"speed\":"
        << invalid << "}}}";
    bad.close();
    const auto before = read(legacy_path);
    check(!awj::load_user_preset_file(legacy_path) && read(legacy_path) == before,
          "invalid migration changed the source file or was accepted");
    fs::remove(legacy_path);
  }
  check(require(awj::list_user_presets()).presets.empty(), "deleted preset remains selectable");
  std::puts("Preset uniqueness, self-edit, rename/delete rollback, shell resources and injection limits passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
