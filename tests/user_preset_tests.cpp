#include <algorithm>
#include <cstdio>
#include <iterator>
#include <nlohmann/json.hpp>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../src/ui/queue_preset_selection.h"

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

void queue_preset_identity_check() {
  using awj::studio::queue_preset_file_for_index;
  using awj::studio::queue_preset_index_for_file;
  auto first = awj::default_user_preset();
  first.source_path = fs::path{u8"预设目录"} / fs::path{u8"中文预设.jsonc"};
  auto second = awj::default_user_preset();
  second.source_path = "other.jsonc";
  std::vector<awj::UserPreset> presets{first, second};
  const std::string filename = "中文预设.jsonc";
  check(queue_preset_index_for_file(presets, filename) == 1 &&
        queue_preset_file_for_index(presets, 1) == filename,
        "UTF-8 basename identity did not roundtrip");
  check(queue_preset_file_for_index(presets, 0).empty() &&
        queue_preset_file_for_index(presets, -1).empty() &&
        queue_preset_file_for_index(presets, 3).empty(),
        "built-in or invalid indices must not produce a persisted filename");
  for (const auto* invalid : {"", "missing.jsonc", ".", "..", "../中文预设.jsonc",
                              "folder/中文预设.jsonc", "folder\\中文预设.jsonc",
                              "C:\\中文预设.jsonc"}) {
    check(queue_preset_index_for_file(presets, invalid) == 0,
          "absent or path-shaped identity did not fall back to built-in");
  }
  auto embedded_nul = filename;
  embedded_nul.push_back('\0');
  check(queue_preset_index_for_file(presets, embedded_nul) == 0,
        "embedded NUL was accepted as a preset identity");
  check(queue_preset_index_for_file({}, filename) == 0,
        "empty catalog retained a selected preset");
#ifndef _WIN32
  auto native = awj::default_user_preset();
  native.source_path = fs::path{"catalog"} / "name:with\\separator.jsonc";
  const std::vector<awj::UserPreset> native_catalog{native};
  check(queue_preset_file_for_index(native_catalog, 1) == "name:with\\separator.jsonc" &&
        queue_preset_index_for_file(native_catalog, "name:with\\separator.jsonc") == 1,
        "a valid POSIX basename was reinterpreted as a path");
#endif
  second.source_path = fs::path{u8"另一个目录"} / first.source_path.filename();
  presets.push_back(second);
  check(queue_preset_index_for_file(presets, filename) == 0 &&
        queue_preset_file_for_index(presets, 1).empty(),
        "ambiguous basenames must not choose an arbitrary catalog entry");
}

int main() try {
  queue_preset_identity_check();
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
  preset.formats[0].chroma_mode = awj::ChromaMode::yuv400;
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
  const auto queue_preset_file = awj::studio::user_preset_file_name(original_path);
  const auto selected_path = [&](const std::vector<awj::UserPreset>& presets) {
    const int index = awj::studio::queue_preset_index_for_file(presets, queue_preset_file);
    return index == 0 ? fs::path{} : presets[static_cast<std::size_t>(index - 1)].source_path;
  };
  check(selected_path(require(awj::list_user_presets()).presets) == original_path,
        "saved preset identity was not found in the real catalog");
  preset.name = "重命名 测试";
  require(awj::save_user_preset(preset, true));
  check(require(awj::find_user_preset(preset.name)).source_path == original_path,
        "rename did not atomically edit the original file");
  check(!awj::find_user_preset("照片 测试"), "old name survived rename");
  check(selected_path(require(awj::list_user_presets()).presets) == original_path,
        "display-name edit changed the persisted queue preset identity");
  auto peer = awj::default_user_preset();
  peer.name = "AAA queue ordering";
  peer.source_path = require(awj::save_user_preset(peer, false));
  auto reordered = require(awj::list_user_presets()).presets;
  const int previous_index = awj::studio::queue_preset_index_for_file(reordered, queue_preset_file);
  check(reordered.size() == 2 && selected_path(reordered) == original_path,
        "inserting another preset changed the selected file");
  std::reverse(reordered.begin(), reordered.end());
  check(selected_path(reordered) == original_path &&
        awj::studio::queue_preset_index_for_file(reordered, queue_preset_file) != previous_index,
        "queue selection followed a stale catalog index after reordering");
  for (auto& value : reordered) value.name = "translated " + value.name;
  check(selected_path(reordered) == original_path,
        "display language or label changed the selected file");
  require(awj::delete_user_preset(peer));
  check(selected_path(require(awj::list_user_presets()).presets) == original_path,
        "deleting another preset changed the selected file");
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
      shell.config.memory_limit_bytes == 1536ull * 1024 * 1024 && !shell.config.unlock_memory_limit, "Linux shell lost the preset memory budget");
#endif
  const auto reread = require(awj::load_user_preset_file(original_path));
  check(reread.formats[0].chroma_mode == awj::ChromaMode::yuv400 &&
      shell.config.chroma_mode == awj::ChromaMode::yuv400 &&
      reread.formats[0].memory_limit_bytes == preset.formats[0].memory_limit_bytes &&
      reread.formats[0].visual_quality == 87 && reread.formats[0].max_jobs == 3 &&
      reread.formats[0].image_size_limit.scale_percent.value_or(0) == 80 &&
      reread.formats[2].jxl_jpeg_lossless == false &&
      reread.formats[0].menu_install_avif_png_command &&
      reread.formats[0].menu_strip_metadata &&
      !reread.formats[0].menu_allow_wic_fallback &&
      !reread.formats[0].menu_close_on_finish,
      "shell execution rewrote source preset values");
  auto invalid_chroma_preset = preset;
  invalid_chroma_preset.formats[3].chroma_mode = awj::ChromaMode::yuv400;
  check(!awj::validate_user_preset(invalid_chroma_preset) &&
        !awj::save_user_preset(invalid_chroma_preset, true),
        "explicit JPG 400 preset was silently normalized instead of rejected");
  check(!awj::parse_arguments_with_user_preset({L"--preset", L"重命名 测试",
            L"--format", L"jpgli", L"--chroma", L"400"}),
        "explicit JPG 400 override on a saved preset was accepted");
  const auto gray_cli = require(awj::parse_arguments_with_user_preset({L"--preset",
      L"重命名 测试", L"--format", L"avif"}));
  check(gray_cli.config.chroma_mode == awj::ChromaMode::yuv400,
        "AVIF 400 preset CLI roundtrip changed its sampling");

  const auto duplicate = directory / "duplicate.jsonc";
  fs::copy_file(original_path, duplicate);
  auto catalog = require(awj::list_user_presets());
  check(catalog.presets.empty() && catalog.errors.size() == 2, "duplicate disk names were not excluded");
  check(selected_path(catalog.presets).empty(),
        "duplicate-name exclusion left the selected queue preset active");
  fs::remove(duplicate);
  // 真实目录里文件被外部改名或内容失效时，旧身份不能误选其他项。
  const auto external_rename = directory / "external-rename.jsonc";
  fs::rename(original_path, external_rename);
  check(selected_path(require(awj::list_user_presets()).presets).empty(),
        "external file rename must invalidate the old persisted basename");
  fs::rename(external_rename, original_path);
  auto backup = original_path;
  backup += ".backup";
  fs::rename(original_path, backup);
  {
    std::ofstream invalid(original_path);
    invalid << "[]";
  }
  check(selected_path(require(awj::list_user_presets()).presets).empty(),
        "invalid selected preset remained selectable");
  fs::remove(original_path);
  fs::rename(backup, original_path);
  check(selected_path(require(awj::list_user_presets()).presets) == original_path,
        "valid catalog restoration lost the stable preset identity");
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
  auto survivor = awj::default_user_preset();
  survivor.name = "remaining after selected deletion";
  survivor.source_path = require(awj::save_user_preset(survivor, false));
  require(awj::delete_user_preset(preset));
  const auto after_delete = require(awj::list_user_presets());
  check(after_delete.presets.size() == 1 && selected_path(after_delete.presets).empty(),
        "deleting the selected preset switched to an unrelated survivor");
  require(awj::delete_user_preset(survivor));
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
  for (const auto* format : {"avif", "jpgli"}) {
    std::ofstream file(legacy_path);
    file << "{\"schema\":1,\"name\":\"explicit chroma\",\"description\":\"\",\"formats\":{\""
         << format << "\":{\"chroma\":\"4:0:0\"}}}";
    file.close();
    const auto loaded = awj::load_user_preset_file(legacy_path);
    if (std::string_view{format} == "avif")
      check(loaded && loaded->formats[0].chroma_mode == awj::ChromaMode::yuv400,
            "preset parser did not reuse configuration chroma aliases for AVIF 400");
    else check(!loaded, "explicit JPG 400 on disk was silently normalized instead of rejected");
    fs::remove(legacy_path);
  }
  check(require(awj::list_user_presets()).presets.empty(), "deleted preset remains selectable");
  std::puts("Preset uniqueness, self-edit, rename/delete rollback, shell resources and injection limits passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
