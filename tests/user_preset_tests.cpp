#include "../src/compat/stdlib_compat.h"
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
template <class T> T require(awj::compat::expected<T, std::string> value) {
  if (!value) throw std::runtime_error(value.error());
  return std::move(*value);
}
void require(awj::compat::expected<void, std::string> value) {
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

std::string read_preset(const fs::path& path) {
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{input}, {}};
}
void write_preset(const fs::path& path, std::string_view text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  check(static_cast<bool>(output), "无法写入测试预设");
}

void priority_and_migration_checks(const fs::path& directory) {
  auto preset = awj::default_user_preset();
  preset.name = "priority contract";
  preset.formats[0].quality = 65;
  preset.formats[1].quality = 55;
  preset.source_path = require(awj::save_user_preset(preset, false));
  const auto path = preset.source_path;
  const auto locked_catalog_path = directory / "locked-catalog.jsonc";
  const std::string locked_catalog = R"({"name":"legacy injected under lock","shell_menu":true,"formats":{}})";
  write_preset(locked_catalog_path, locked_catalog);
  require(awj::save_user_preset(preset, true, [&]() -> awj::compat::expected<void, std::string> {
    auto names = awj::injected_user_preset_names();
    if (!names) return awj::compat::unexpected{names.error()};
    check(names->size() == 1 && names->front() == L"legacy injected under lock" &&
          read_preset(locked_catalog_path) == locked_catalog,
          "锁内菜单同步递归迁移预设，导致旧预设被排除或被改写");
    return {};
  }));
  auto locked_preset = require(awj::load_user_preset_file(locked_catalog_path));
  check(read_preset(locked_catalog_path) != locked_catalog, "锁外正常加载没有继续迁移旧预设");
  require(awj::delete_user_preset(locked_preset));
  const auto fallback_duplicate = directory / "fallback-duplicate.jsonc";
  const auto declared_duplicate = directory / "declared-duplicate.jsonc";
  write_preset(fallback_duplicate, "{}");
  write_preset(declared_duplicate, R"({"name":"fallback-duplicate","formats":{}})");
  const auto duplicate_catalog = require(awj::list_user_presets());
  check(duplicate_catalog.presets.size() == 1 && duplicate_catalog.errors.size() == 2 &&
        !awj::find_user_preset("fallback-duplicate"), "自动补齐的名称绕过了重复预设排除规则");
  fs::remove(fallback_duplicate);
  fs::remove(declared_duplicate);
  const auto original = read_preset(path);
  check(original.find("\"cli_args_high_priority\"") < original.find("\"cli_args_low_priority\"") &&
        original.find("\"cli_args_low_priority\"") < original.find("\"formats\""),
        "新建预设 CLI 键没有固定排在 formats 前面");
  auto document = nlohmann::ordered_json::parse(original, nullptr, true, true);
  check(document["cli_args_high_priority"] == nlohmann::ordered_json::array() &&
        document["cli_args_low_priority"] == nlohmann::ordered_json::array(),
        "新建预设没有写出两个空数组");
  const auto cli = [&](std::vector<std::wstring> args = {}) {
    args.insert(args.begin(), {L"--preset-file", path.wstring()});
    return awj::parse_arguments_with_user_preset(args);
  };
  preset.cli_args_low_priority = {"--quality", "30", "--timeout-encode", "7", "--strip"};
  preset.cli_args_high_priority = {"--quality", "90", "--timeout-encode", "11",
      "--avif-progressive", "--avif-animation-tune", "psnr", "--avif-animation-keyframe", "12"};
  require(awj::save_user_preset(preset, true));
  auto resolved = require(cli({L"-q", L"80", L"--timeout-encode", L"9"}));
  check(resolved.config.quality == 90 && resolved.config.encode_timeout_minutes == 11 &&
        resolved.config.strip_metadata && resolved.config.avif_layers.automatic &&
        resolved.config.avif_animation_tune == awj::AvifAnimationTune::psnr &&
        resolved.config.avif_animation_keyframe == 12, "高优先级没有覆盖显式 CLI 或丢失 CLI-only 参数");
  auto ui = awj::config_from_user_preset(preset, awj::OutputFormat::avif);
  ui.quality = 80;
  ui.input_path = fs::path{u8"中文 输入.png"};
  ui.output_template = L"中文 {stem}";
  auto effective = require(awj::resolve_user_preset_ui_config(preset, ui));
  check(effective.quality == 90 && effective.encode_timeout_minutes == 11 &&
        !effective.strip_metadata && effective.avif_layers.automatic &&
        ui.quality == 80 && !ui.strip_metadata, "UI 分层不一致或覆盖结果污染了 UI 参数");
  effective.color_primaries = 9;
  effective.transfer_characteristics = 16;
  effective.matrix_coefficients = 9;
  effective.color_range = 1;
  auto worker = require(awj::parse_arguments(awj::conversion_arguments_from_config(effective)));
  check(worker.config.quality == effective.quality && worker.config.avif_layers.automatic &&
        worker.config.input_path == effective.input_path && worker.config.output_template == ui.output_template &&
        worker.config.encode_timeout_minutes == 11 && worker.config.avif_animation_keyframe == 12 &&
        worker.config.avif_animation_tune == awj::AvifAnimationTune::psnr &&
        worker.config.color_primaries == 9 && worker.config.transfer_characteristics == 16 &&
        worker.config.matrix_coefficients == 9 && worker.config.color_range == 1,
        "最终配置经 worker 参数序列化后丢失 CLI-only 选项或 Unicode");
  preset.cli_args_high_priority.clear();
  require(awj::save_user_preset(preset, true));
  resolved = require(cli());
  check(resolved.config.quality == 65 && resolved.config.encode_timeout_minutes == 7,
        "formats 没有覆盖低层，或低层 CLI-only 参数未生效");
  resolved = require(cli({L"--quality", L"80", L"--timeout-encode", L"9", L"--keep-metadata"}));
  check(resolved.config.quality == 80 && resolved.config.encode_timeout_minutes == 9 &&
        !resolved.config.strip_metadata, "显式 CLI 没有覆盖 formats 或低层参数");
  effective = require(awj::resolve_user_preset_ui_config(preset, ui));
  check(effective.quality == 80 && !effective.strip_metadata && effective.encode_timeout_minutes == 7,
        "低层抢占 UI 可见值或 CLI-only 默认没有传入 UI 执行配置");
  preset.cli_args_high_priority = {"--format", "webp"};
  require(awj::save_user_preset(preset, true));
  resolved = require(cli({L"--format", L"avif"}));
  check(resolved.config.output_format == awj::OutputFormat::webp && resolved.config.quality == 55,
        "没有按最终高优先级格式选择 formats");
  effective = require(awj::resolve_user_preset_ui_config(preset, ui));
  check(effective.output_format == awj::OutputFormat::webp && effective.quality == 55 &&
        effective.bit_depth == 8, "UI 跨格式覆盖残留了原格式参数");
  preset.cli_args_low_priority = {"--avif-layers", "2", "--avif-layer-scale", "1:1/2",
      "--image-size-limit", "fixed", "--max-width", "512", "--cpu-affinity", "0-3"};
  preset.cli_args_high_priority = {"--avif-layer-scale", "1:1/3", "--image-size-limit", "fixed",
      "--scale-percent", "75.125", "--cpu-affinity", "0-1"};
  require(awj::save_user_preset(preset, true));
  resolved = require(cli({L"--avif-layer-quality", L"1:15", L"--threads", L"auto"}));
  check(resolved.config.avif_layers.count == 2 && resolved.config.avif_layers.layers[0].scale == std::pair{1u, 3u} &&
        resolved.config.avif_layers.layers[0].quality == 15 &&
        resolved.config.image_size_limit.scale_percent == 75.125 && !resolved.config.image_size_limit.max_width &&
        resolved.config.cpu_affinity == std::vector<unsigned>{0, 1} && resolved.config.max_jobs == 1,
        "跨层尺寸/渐进层/affinity 覆盖被误判为重复，或线程 auto 丢失");
  auto layer_roundtrip = require(awj::parse_arguments(awj::conversion_arguments_from_config(resolved.config)));
  check(layer_roundtrip.config.avif_layers.layers[0].scale == std::pair{1u, 3u} &&
        layer_roundtrip.config.image_size_limit.scale_percent == 75.125,
        "手动渐进层或小数缩放经 worker 传递发生变化");
  preset.cli_args_low_priority = {"--avif-progressive"};
  preset.cli_args_high_priority = {"--no-avif-progressive"};
  require(awj::save_user_preset(preset, true));
  check(!require(cli()).config.avif_layers.enabled(), "高层无法关闭低层渐进设置");
  for (const auto& invalid : std::vector<std::vector<std::string>>{
           {"--max-width", "100", "--width", "200"}, {"--quality"}, {"--unknown-option"},
           {"--preset", "recursive"}, {"--preset-file", "other.jsonc"}, {"--help"}, {"--version"},
           {"--shell-convert"}, {"--studio-queue-manifest", "untrusted"},
           {"--avif-layers", "2", "--avif-layer-scale", "1:1/2", "--avif-layer-size", "1:32x32"},
           {"--cpu-affinity", "0", "--cpu-affinity", "1"}, {std::string(4097, 'x')},
           {std::string{"--quality\0ignored", 17}}, std::vector<std::string>(257, "--strip")}) {
    preset.cli_args_high_priority = invalid;
    const auto before = read_preset(path);
    check(!awj::save_user_preset(preset, true) && read_preset(path) == before,
          "非法、超限或控制参数被保存，或失败时损坏原文件");
  }
  preset.cli_args_high_priority = {"--input", "other.png"};
  check(!awj::resolve_user_preset_ui_config(preset, ui), "预设输入替换了已经建立的 UI 队列");
  preset.cli_args_high_priority = {"--jpeg-hdr", "sdr"};
  preset.cli_args_low_priority.clear();
  require(awj::save_user_preset(preset, true));
  auto jpeg = require(cli({L"--format", L"jpg"}));
  auto jpeg_worker = require(awj::parse_arguments(awj::conversion_arguments_from_config(jpeg.config)));
  check(jpeg.config.jpeg_hdr == awj::JpegHdrMode::sdr && jpeg_worker.config.jpeg_hdr == jpeg.config.jpeg_hdr,
        "JPEG HDR CLI-only 参数未传递到 worker");
  require(awj::delete_user_preset(preset));

  const auto legacy = directory / "automatic migration.jsonc";
  write_preset(legacy, R"jsonc(/* 保留原块注释 */
{
  // "cli_args_high_priority": ["--delete-source"]
  "description": "https://example.test/a/*literal*/?x=//b",
  "obsolete": {"future": [1, {"nested": true}], "cli_args_high_priority": ["--delete-source"]},
  "formats": {
    "avif": {"quality": 71, "future_option": {"x": "*/"}, "size_limit": {"mode": "none", "old": 42}},
    "future_format": {"quality": 2}
  }
})jsonc");
  auto migrated = require(awj::load_user_preset_file(legacy));
  auto normalized = read_preset(legacy);
  document = nlohmann::ordered_json::parse(normalized, nullptr, true, true);
  check(migrated.name == "automatic migration" && migrated.formats[0].quality == 71 &&
        migrated.cli_args_high_priority.empty() && !require(awj::resolve_user_preset_ui_config(migrated, ui)).delete_source &&
        document["schema"] == 1 && document.contains("shell_menu") && document["formats"].size() == 5 &&
        !document.contains("obsolete") && !document["formats"].contains("future_format") &&
        !document["formats"]["avif"].contains("future_option") &&
        !document["formats"]["avif"]["size_limit"].contains("old") &&
        normalized.find("保留原块注释") != std::string::npos &&
        normalized.find("兼容保留（不读取）：$[\"formats\"][\"avif\"][\"size_limit\"][\"old\"]") != std::string::npos &&
        normalized.find("future_format") != std::string::npos &&
        migrated.description == "https://example.test/a/*literal*/?x=//b",
        "自动迁移未补齐缺项、注释多余字段，或误读取了注释/字符串");
  (void)require(awj::load_user_preset_file(legacy));
  check(read_preset(legacy) == normalized, "重复加载预设不幂等");
  migrated.description = "UI 编辑后仍保留原注释";
  require(awj::save_user_preset(migrated, true));
  normalized = read_preset(legacy);
  require(awj::save_user_preset(require(awj::load_user_preset_file(legacy)), true));
  check(read_preset(legacy) == normalized && normalized.find("future_option") != std::string::npos &&
        normalized.find("保留原块注释") != std::string::npos, "UI 保存丢失注释或重复包裹了注释");
  require(awj::delete_user_preset(migrated));
  for (const auto& invalid : std::vector<std::string>{
           "{} /* 未闭合", "{\"formats\":[]}", "{\"schema\":2}", "{\"name\":null}",
           "{\"cli_args_high_priority\":\"--strip\"}", "{\"cli_args_low_priority\":[1]}",
           "{\"name\":\"one\",\"name\":\"two\"}",
           "{\"formats\":{\"avif\":{\"quality\":1/**/2}}}",
           "{} /* 外层 /* 内层 */ 外层 */",
           "{\"unknown\":" + std::string(70, '[') + "0" + std::string(70, ']') + "}"}) {
    write_preset(legacy, invalid);
    check(!awj::load_user_preset_file(legacy) && read_preset(legacy) == invalid,
          "无效 JSONC 在加载时被接受或覆盖");
    fs::remove(legacy);
  }
  write_preset(legacy, "{}");
  const auto obstructed = fs::path{legacy.string() + ".tmp"};
  fs::create_directory(obstructed);
  write_preset(obstructed / "keep", "仅本次测试的写回障碍");
  check(!awj::load_user_preset_file(legacy) && read_preset(legacy) == "{}",
        "迁移写回失败没有报告或损坏了原文");
  fs::remove(obstructed / "keep");
  fs::remove(obstructed);
  require(awj::recover_user_preset_change());
  check(read_preset(legacy) == "{}", "迁移失败恢复记录没有恢复原文");
  fs::remove(legacy);
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
  preset.formats[1].image_size_limit = {.mode = awj::ImageSizeLimitMode::fixed, .scale_percent = 200.5};
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
  check(reread.formats[1].image_size_limit.mode == awj::ImageSizeLimitMode::fixed &&
      reread.formats[1].image_size_limit.scale_percent == 200.5 &&
      !reread.formats[1].image_size_limit.max_width && !reread.formats[1].image_size_limit.max_height,
      "固定模式单独比例没有经过预设保存与加载");
  const auto ratio_cli = require(awj::parse_arguments_with_user_preset(
      {L"--preset", L"重命名 测试", L"--format", L"webp"}));
  check(ratio_cli.config.image_size_limit.mode == awj::ImageSizeLimitMode::fixed &&
      ratio_cli.config.image_size_limit.scale_percent == 200.5,
      "固定模式比例预设没有传入 CLI");
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
  const auto sync_failure = [&]() -> awj::compat::expected<void, std::string> {
    if (++sync_calls == 1) return awj::compat::unexpected{"injected synchronization failure"};
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
  check(!document.contains("unknown") && normalized.find("\"future\": true") != std::string::npos &&
        document["formats"]["avif"]["speed"] == 6 && document["formats"]["avif"]["bit_depth"] == 10 &&
        document["formats"]["avif"]["visual_quality"].is_null(),
        "旧预设迁移没有归一可选字段或将未知字段保留为注释");
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
  priority_and_migration_checks(directory);
  check(require(awj::list_user_presets()).presets.empty(), "deleted preset remains selectable");
  std::puts("预设唯一性、回滚、CLI 优先级、worker 往返、JSONC 自动迁移与注释保留契约通过。");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
