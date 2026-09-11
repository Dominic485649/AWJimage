#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <windows.h>

import awj.config;
import awj.core;
import awj.image;
import awj.large_image_plan;
import awj.pipeline;
import awj.resource_planner;
import awj.webp_codec;
import awj.png_codec;
import awj.avif_aom_codec;

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

awj::ImageBuffer make_test_image(std::byte red = std::byte{255}) {
  awj::ImagePlane plane{.stride = 8};
  plane.bytes = {
      red,          std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0}, std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0}, std::byte{0},   std::byte{255}, std::byte{255},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
  };
  awj::ImageBuffer image{.width = 2,
                         .height = 2,
                         .pixel_format = awj::PixelFormat::rgba,
                         .alpha_mode = awj::AlphaMode::straight,
                         .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

std::expected<void, std::string> write_webp(const std::filesystem::path& path,
                                            std::byte red = std::byte{255}) {
  awj::WebPImageEncoder encoder;
  auto encoded = encoder.encode(
      make_test_image(red),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                .quality = 100,
                                .speed = 10,
                                .resources = awj::ResourcePlan{
                                    .file_parallelism = 1,
                                    .encoder_threads_per_file = 1,
                                    .global_thread_budget = 1}});
  if (!encoded) {
    return std::unexpected{encoded.error()};
  }
  std::ofstream stream{path, std::ios::binary};
  stream.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
               static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  if (!stream) {
    return std::unexpected{"failed to write test webp."};
  }
  return {};
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream}, {}};
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "awjimage-pipeline-security-tests";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return fail("failed to create temp root.");
  }
  {
    auto cfg = awj::default_app_config();
    cfg.output_format = awj::OutputFormat::png;
    const auto bytes = awj::pipeline_detail::estimated_regular_working_set_bytes(
        awj::make_image_dimensions(4096, 4096), cfg);
    const auto plan = awj::plan_resources({.automatic_thread_budget = 8, .file_count = 2,
        .memory_limit_bytes = 1024ull * 1024 * 1024, .estimated_bytes_per_file = bytes});
    if (bytes <= 512ull * 1024 * 1024 || plan.file_parallelism != 1 || plan.encoder_threads_per_file != 8)
      return fail("PNG planning omitted high-depth/output buffers or lost the remaining thread budget");
  }
  {
    auto cfg = awj::default_app_config();
    cfg.output_dir = root;
    awj::FileLogger logger{root, false};
    awj::BatchLargeImageItem oversized{
        .file = {.path = root / "nonexistent-huge-input.png"},
        .dimensions = awj::make_image_dimensions(65537, 4096),
        .decision = {.available_grid = true}};
    const auto result = awj::pipeline_detail::encode_large_mode_item(cfg, logger, oversized,
        {.memory_limit_bytes = 64ull * 1024 * 1024}, {});
    if (result.ok || result.message.find("超过内存预算") == std::string::npos)
      return fail("Grid did not reject its working set before decoding input");
  }
  {
    auto wide = make_test_image();
    wide.width = 65537;
    wide.height = 64;
    wide.planes[0].stride = wide.width * 4;
    wide.planes[0].bytes.assign(wide.planes[0].stride * wide.height, std::byte{255});
    auto png = awj::PngImageEncoder{}.encode(wide, {.quality = 100});
    if (!png) return fail(png.error());
    const auto input = root / "auto-grid.png";
    {
      std::ofstream stream{input, std::ios::binary};
      stream.write(reinterpret_cast<const char*>(png->encoded.bytes.data()), png->encoded.bytes.size());
      if (!stream) return fail("could not write automatic Grid fixture");
    }
    auto cfg = awj::default_app_config();
    cfg.input_path = input;
    cfg.output_dir = root / "auto-grid-output";
    cfg.quality = 50;
    cfg.speed = 8;
    cfg.max_jobs = 2;
    cfg.memory_limit_bytes = 2ull * 1024 * 1024 * 1024;
    cfg.chroma_mode = awj::ChromaMode::yuv444;
    cfg.write_summary = true;
    auto summary = awj::run_batch(cfg);
    if (!summary) return fail(summary.error());
    if (summary->ok_count != 1) return fail("automatic Grid encode failed: " + read_text(cfg.output_dir / "summary.csv"));
    auto decoded = awj::make_avif_image_decoder(1)->decode(cfg.output_dir / "auto-grid.avif");
    if (!summary || summary->ok_count != 1 || !decoded || decoded->image.width != 65537 || decoded->image.height != 64)
      return fail(decoded ? "automatic Grid changed original dimensions" : decoded.error());
    cfg.output_dir = root / "manual-grid-output";
    cfg.image_size_limit.mode = awj::ImageSizeLimitMode::manual;
    cfg.image_size_limit.max_width = 32768;
    summary = awj::run_batch(cfg);
    decoded = awj::make_avif_image_decoder(1)->decode(cfg.output_dir / "auto-grid.avif");
    if (!summary || summary->ok_count != 1 || !decoded || decoded->image.width != 32768 || decoded->image.height != 31)
      return fail(decoded ? "manual size limit left a stale Grid plan" : decoded.error());
  }

  std::vector<awj::ImageFile> manifest_files;
  for (std::size_t index = 0; index < 13; ++index) {
    manifest_files.push_back(awj::ImageFile{
        .index = index,
        .path = root / std::format("input-{}.webp", index),
        .relative_dir = "nested",
        .bytes = 123,
        .date_token = L"20260713",
        .time_token = L"120000",
        .datetime_token = L"20260713-120000",
        .unix_token = L"1783915200",
        .random_token = std::format(L"{:08x}", index),
        .hash_token = L"hash",
        .sha256_token = L"sha256",
        .resolved_output_path = root / std::format("output-{}.avif", index),
        .output_path_resolved = true});
  }
  const auto manifest_path = root / "studio-queue.awjq";
  if (auto written =
          awj::write_studio_queue_manifest(manifest_path, manifest_files);
      !written) {
    return fail(written.error());
  }
  auto manifest = awj::read_studio_queue_manifest(manifest_path);
  if (!manifest || manifest->files.size() != manifest_files.size() ||
      manifest->files.back().resolved_output_path !=
          manifest_files.back().resolved_output_path ||
      manifest->files.back().random_token !=
          manifest_files.back().random_token) {
    return fail(manifest ? "studio queue manifest round-trip changed data."
                         : manifest.error());
  }
  const auto manifest_plan = awj::plan_resources(awj::ResourcePlanRequest{
      .automatic_thread_budget = 8,
      .file_count = static_cast<int>(manifest->files.size()),
      .estimated_bytes_per_file = 1});
  if (manifest_plan.encoder_threads_per_file != 1 ||
      manifest_plan.file_parallelism *
              manifest_plan.encoder_threads_per_file !=
          manifest_plan.global_thread_budget) {
    return fail("13-file Studio worker plan did not keep one encoder thread.");
  }
  {
    std::fstream corrupt{manifest_path,
                         std::ios::binary | std::ios::in | std::ios::out};
    corrupt.seekp(8);
    const std::array<char, 4> invalid_version{};
    corrupt.write(invalid_version.data(), invalid_version.size());
  }
  if (auto invalid = awj::read_studio_queue_manifest(manifest_path); invalid) {
    return fail("studio queue manifest accepted an invalid version.");
  }

  if (auto written =
          awj::write_studio_queue_manifest(manifest_path, manifest_files);
      !written) {
    return fail(written.error());
  }
  {
    std::fstream inflated{manifest_path,
                           std::ios::binary | std::ios::in | std::ios::out};
    inflated.seekp(12);
    constexpr std::uint64_t declared_files = 1'000'000;
    for (int shift = 0; shift < 64; shift += 8) {
      inflated.put(static_cast<char>((declared_files >> shift) & 0xffu));
    }
  }
  std::filesystem::resize_file(manifest_path, 20, ec);
  if (ec) {
    return fail("failed to truncate inflated manifest.");
  }
  if (auto inflated = awj::read_studio_queue_manifest(manifest_path);
      inflated) {
    return fail("studio queue manifest trusted an inflated record count.");
  }

  auto traversal_files = manifest_files;
  traversal_files.front().relative_dir = "../escape";
  if (auto traversal = awj::write_studio_queue_manifest(
          root / "studio-queue-traversal.awjq", traversal_files);
      traversal) {
    return fail("studio queue manifest accepted parent path traversal.");
  }

  auto disambiguator_files = manifest_files;
  disambiguator_files.front().output_path_resolved = false;
  disambiguator_files.front().resolved_output_path.clear();
  disambiguator_files.front().extension_disambiguated = true;
  disambiguator_files.front().source_extension_disambiguator = L"/../escape";
  if (auto disambiguator = awj::write_studio_queue_manifest(
          root / "studio-queue-disambiguator.awjq", disambiguator_files);
      !disambiguator) {
    // The writer may reject malformed records before they reach the worker.
  } else {
    auto disambiguator_cfg = awj::default_app_config();
    disambiguator_cfg.input_path = root;
    disambiguator_cfg.output_dir = root / "allowed-output";
    disambiguator_cfg.studio_queue_manifest =
        root / "studio-queue-disambiguator.awjq";
    disambiguator_cfg.write_log = false;
    disambiguator_cfg.write_summary = false;
    if (auto escaped = awj::run_batch(disambiguator_cfg); escaped) {
      return fail("Studio worker accepted a malicious extension disambiguator.");
    }
  }

  if (auto written =
          awj::write_studio_queue_manifest(manifest_path, manifest_files);
      !written) {
    return fail(written.error());
  }
  auto manifest_cfg = awj::default_app_config();
  manifest_cfg.input_path = root;
  manifest_cfg.output_dir = root / "allowed-output";
  manifest_cfg.studio_queue_manifest = manifest_path;
  manifest_cfg.write_log = false;
  manifest_cfg.write_summary = false;
  auto escaped_output = awj::run_batch(manifest_cfg);
  if (escaped_output ||
      escaped_output.error().find("输出路径越界") == std::string::npos) {
    return fail("Studio worker accepted a resolved output outside output_dir.");
  }

  auto wrong_extension_files = manifest_files;
  wrong_extension_files.front().resolved_output_path =
      manifest_cfg.output_dir / "wrong-extension.txt";
  if (auto written = awj::write_studio_queue_manifest(
          manifest_path, wrong_extension_files);
      !written) {
    return fail(written.error());
  }
  auto wrong_extension = awj::run_batch(manifest_cfg);
  if (wrong_extension ||
      wrong_extension.error().find("扩展名无效") == std::string::npos) {
    return fail("Studio worker accepted an unexpected output extension.");
  }

  auto avif_png_files = manifest_files;
  for (std::size_t index = 0; index < avif_png_files.size(); ++index) {
    avif_png_files[index].resolved_output_path =
        manifest_cfg.output_dir / std::format("output-{}.avif.png", index);
  }
  if (auto written = awj::write_studio_queue_manifest(manifest_path, avif_png_files);
      !written) {
    return fail(written.error());
  }
  auto avif_png_cfg = manifest_cfg;
  avif_png_cfg.append_png_suffix = true;
  if (auto accepted = awj::run_batch(avif_png_cfg);
      !accepted && accepted.error().find("扩展名无效") != std::string::npos) {
    return fail("Studio worker rejected a valid AVIF.png manifest output.");
  }
  if (auto rejected = awj::run_batch(manifest_cfg);
      rejected || rejected.error().find("扩展名无效") == std::string::npos) {
    return fail("Studio worker accepted AVIF.png without the AVIF.png setting.");
  }

  auto input_overwrite_files = manifest_files;
  for (std::size_t index = 0; index < input_overwrite_files.size(); ++index) {
    input_overwrite_files[index].resolved_output_path =
        root / std::format("safe-output-{}.webp", index);
  }
  input_overwrite_files.front().resolved_output_path = manifest_files[1].path;
  if (auto written = awj::write_studio_queue_manifest(
          manifest_path, input_overwrite_files);
      !written) {
    return fail(written.error());
  }
  auto input_overwrite_cfg = manifest_cfg;
  input_overwrite_cfg.output_dir = root;
  input_overwrite_cfg.output_format = awj::OutputFormat::webp;
  auto input_overwrite = awj::run_batch(input_overwrite_cfg);
  if (input_overwrite ||
      input_overwrite.error().find("覆盖输入") == std::string::npos) {
    return fail("Studio worker accepted an output that overwrites another input.");
  }

  auto missing_cfg = awj::default_app_config();
  missing_cfg.input_path = root / "missing.webp";
  auto missing_summary = awj::run_batch(missing_cfg);
  if (missing_summary || missing_summary.error().find("不存在") == std::string::npos) {
    return fail("missing input path was not rejected clearly.");
  }

  const auto input_dir = root / "input";
  const auto output_dir = root / "out";
  std::filesystem::create_directories(input_dir, ec);
  if (ec) {
    return fail("failed to create input dir.");
  }
  if (auto ok = write_webp(input_dir / "same-a.webp", std::byte{255}); !ok) {
    return fail(ok.error());
  }
  if (auto ok = write_webp(input_dir / "same-b.webp", std::byte{128}); !ok) {
    return fail(ok.error());
  }

  const auto drag_input_dir = root / "drag-input";
  const auto drag_nested_dir = drag_input_dir / "nested";
  std::filesystem::create_directories(drag_nested_dir, ec);
  if (ec || !write_webp(drag_input_dir / "b.webp", std::byte{1}) ||
      !write_webp(drag_input_dir / "a.webp", std::byte{2}) ||
      !write_webp(drag_nested_dir / "c.webp", std::byte{3})) {
    return fail("failed to create recursive drag-input fixtures.");
  }
  std::ofstream{drag_input_dir / "ignored.txt", std::ios::binary} << "not an image";
  auto drag_scan_cfg = awj::default_app_config();
  drag_scan_cfg.input_path = drag_input_dir;
  drag_scan_cfg.output_dir = root / "drag-output";
  drag_scan_cfg.output_format = awj::OutputFormat::avif;
  std::vector<awj::ImageFile> drag_scanned;
  if (auto scanned = awj::scan_images(drag_scan_cfg, drag_scanned); !scanned ||
      drag_scanned.size() != 3 ||
      drag_scanned[0].path.filename() != "a.webp" ||
      drag_scanned[1].path.filename() != "b.webp" ||
      drag_scanned[2].path.filename() != "c.webp" ||
      drag_scanned[2].relative_dir != std::filesystem::path{"nested"}) {
    return fail(scanned ? "recursive drag scan lost deterministic order or relative paths."
                        : scanned.error());
  }
  const std::array<std::filesystem::path, 3> drag_targets{
      drag_input_dir, drag_nested_dir, drag_input_dir / "a.webp"};
  std::vector<awj::ImageFile> drag_multi_scanned;
  if (auto scanned = awj::scan_images(drag_scan_cfg, drag_targets,
                                      drag_multi_scanned);
      !scanned || drag_multi_scanned.size() != 3 ||
      drag_multi_scanned[2].relative_dir != std::filesystem::path{"nested"}) {
    return fail(scanned ? "multi-target drag scan did not de-duplicate recursively."
                        : scanned.error());
  }

  const auto invalid_batch_input = root / "invalid-batch-input";
  const auto invalid_batch_output = root / "invalid-batch-output";
  std::filesystem::create_directories(invalid_batch_input, ec);
  if (ec || !write_webp(invalid_batch_input / "valid.webp")) {
    return fail("failed to create valid input for probe failure isolation.");
  }
  std::ofstream{invalid_batch_input / "empty.webp", std::ios::binary};
  auto invalid_batch_cfg = awj::default_app_config();
  invalid_batch_cfg.input_path = invalid_batch_input;
  invalid_batch_cfg.output_dir = invalid_batch_output;
  invalid_batch_cfg.output_format = awj::OutputFormat::avif;
  invalid_batch_cfg.max_jobs = 2;
  invalid_batch_cfg.quality = 90;
  invalid_batch_cfg.write_log = false;
  invalid_batch_cfg.write_summary = true;
  const auto invalid_batch_summary = awj::run_batch(invalid_batch_cfg);
  if (!invalid_batch_summary || invalid_batch_summary->ok_count != 1 ||
      invalid_batch_summary->failed_count != 1 ||
      !std::filesystem::exists(invalid_batch_output / "valid.avif")) {
    return fail(invalid_batch_summary
                    ? "one invalid input stopped the rest of the AVIF batch."
                    : invalid_batch_summary.error());
  }
  const auto invalid_batch_csv =
      read_text(invalid_batch_output / "summary.csv");
  if (invalid_batch_csv.find("empty.webp") == std::string::npos ||
      invalid_batch_csv.find("failed") == std::string::npos) {
    return fail("invalid AVIF batch input was not recorded as one failed item.");
  }

  const auto manifest_input_dir = root / "manifest-input";
  const auto manifest_output_dir = root / "manifest-output";
  std::filesystem::create_directories(manifest_input_dir, ec);
  if (ec) {
    return fail("failed to create manifest input dir.");
  }
  std::vector<awj::ImageFile> runnable_manifest_files;
  for (std::size_t index = 0; index < 13; ++index) {
    const auto input =
        manifest_input_dir / std::format("manifest-input-{}.webp", index);
    if (auto ok = write_webp(input, std::byte{static_cast<unsigned char>(index)});
        !ok) {
      return fail(ok.error());
    }
    runnable_manifest_files.push_back(awj::ImageFile{
        .index = index,
        .path = input,
        .bytes = std::filesystem::file_size(input, ec),
        .resolved_output_path =
            manifest_output_dir / std::format("manifest-output-{}.webp", index),
        .output_path_resolved = true});
  }
  const auto runnable_manifest_path = root / "studio-queue-runnable.awjq";
  if (auto written = awj::write_studio_queue_manifest(
          runnable_manifest_path, runnable_manifest_files);
      !written) {
    return fail(written.error());
  }
  auto runnable_manifest_cfg = awj::default_app_config();
  runnable_manifest_cfg.input_path = manifest_input_dir;
  runnable_manifest_cfg.output_dir = manifest_output_dir;
  runnable_manifest_cfg.output_format = awj::OutputFormat::webp;
  runnable_manifest_cfg.studio_queue_manifest = runnable_manifest_path;
  runnable_manifest_cfg.max_jobs = 8;
  runnable_manifest_cfg.quality = 100;
  runnable_manifest_cfg.speed = 10;
  runnable_manifest_cfg.write_log = false;
  runnable_manifest_cfg.write_summary = false;
  std::vector<int> manifest_encoder_threads(13, -1);
  std::atomic<int> manifest_started_count{0};
  auto runnable_manifest_summary = awj::run_batch(
      runnable_manifest_cfg, [&](const awj::BatchProgress& event) {
        if (event.kind == awj::BatchEventKind::item_started) {
          manifest_started_count.fetch_add(1, std::memory_order_relaxed);
        } else if (event.kind == awj::BatchEventKind::item_finished &&
                   event.result.index < manifest_encoder_threads.size()) {
          manifest_encoder_threads[event.result.index] =
              event.result.encoder_threads;
        }
      });
  if (!runnable_manifest_summary || runnable_manifest_summary->ok_count != 13 ||
      manifest_started_count.load(std::memory_order_relaxed) != 13 ||
      manifest_encoder_threads.size() != 13 ||
      std::ranges::any_of(manifest_encoder_threads,
                          [](int threads) { return threads != 1; })) {
    return fail(runnable_manifest_summary
                    ? "Studio manifest batch did not keep one encoder thread per image."
                    : runnable_manifest_summary.error());
  }

  auto runnable_scan_cfg = runnable_manifest_cfg;
  runnable_scan_cfg.studio_queue_manifest.clear();
  runnable_scan_cfg.output_dir = root / "scan-output";
  std::vector<int> scan_encoder_threads(13, -1);
  auto runnable_scan_summary = awj::run_batch(
      runnable_scan_cfg, [&](const awj::BatchProgress& event) {
        if (event.kind == awj::BatchEventKind::item_finished &&
            event.result.index < scan_encoder_threads.size()) {
          scan_encoder_threads[event.result.index] =
              event.result.encoder_threads;
        }
      });
  if (!runnable_scan_summary || runnable_scan_summary->ok_count != 13 ||
      scan_encoder_threads.size() != 13 ||
      std::ranges::any_of(scan_encoder_threads,
                          [](int threads) { return threads != 1; })) {
    return fail(runnable_scan_summary
                    ? "13-file scanned batch did not keep one encoder thread per image."
                    : runnable_scan_summary.error());
  }

  awj::AppConfig cfg = awj::default_app_config();
  cfg.input_path = input_dir;
  cfg.output_dir = output_dir;
  cfg.output_format = awj::OutputFormat::webp;
  cfg.output_template = L"same";
  cfg.collision_mode = awj::CollisionMode::suffix_random;
  cfg.max_jobs = 2;
  cfg.quality = 100;
  cfg.write_summary = true;
  auto summary = awj::run_batch(cfg);
  if (!summary || summary->ok_count != 2 || summary->failed_count != 0) {
    return fail(summary ? "suffix collision batch did not succeed." : summary.error());
  }

  std::vector<std::filesystem::path> outputs;
  for (const auto& entry : std::filesystem::directory_iterator{output_dir}) {
    if (entry.path().extension() == ".webp") {
      outputs.push_back(entry.path());
    }
  }
  if (outputs.size() != 2) {
    return fail("suffix collision did not create two distinct outputs.");
  }

  const auto numbered_dir = root / "numbered";
  std::filesystem::create_directories(numbered_dir, ec);
  if (ec) {
    return fail("failed to create numbered dir.");
  }
  const auto numbered_input = numbered_dir / "name.webp";
  if (auto ok = write_webp(numbered_input, std::byte{32}); !ok) {
    return fail(ok.error());
  }
  awj::AppConfig number_cfg = awj::default_app_config();
  number_cfg.input_path = numbered_input;
  number_cfg.output_dir = numbered_dir;
  number_cfg.output_format = awj::OutputFormat::webp;
  number_cfg.collision_mode = awj::CollisionMode::suffix_number;
  number_cfg.max_jobs = 1;
  number_cfg.quality = 100;
  auto number_summary = awj::run_batch(number_cfg);
  if (!number_summary || number_summary->ok_count != 1 ||
      !std::filesystem::exists(numbered_dir / "name(1).webp")) {
    return fail(number_summary ? "numbered collision did not create name(1)."
                               : number_summary.error());
  }
  number_cfg.input_path = numbered_dir / "name(1).webp";
  number_summary = awj::run_batch(number_cfg);
  if (!number_summary || number_summary->ok_count != 1 ||
      !std::filesystem::exists(numbered_dir / "name(2).webp") ||
      std::filesystem::exists(numbered_dir / "name(1)(1).webp")) {
    return fail(number_summary ? "numbered collision did not advance existing suffix."
                               : number_summary.error());
  }

  const auto short_dir = root / "short-path";
  std::filesystem::create_directories(short_dir, ec);
  if (ec) {
    return fail("failed to create short-path dir.");
  }
  const auto long_webp = short_dir / "20B07A very long filename that must stay complete.webp";
  if (auto ok = write_webp(long_webp, std::byte{16}); !ok) {
    return fail(ok.error());
  }
  const DWORD needed = GetShortPathNameW(long_webp.c_str(), nullptr, 0);
  if (needed > 0) {
    std::wstring short_buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    const DWORD written = GetShortPathNameW(
        long_webp.c_str(), short_buffer.data(), static_cast<DWORD>(short_buffer.size()));
    if (written > 0 && written < short_buffer.size()) {
      short_buffer.resize(written);
      const std::filesystem::path short_webp{short_buffer};
      if (short_webp.native() != long_webp.native()) {
        const auto short_out = root / "short-out";
        awj::AppConfig short_cfg = awj::default_app_config();
        short_cfg.input_path = short_webp;
        short_cfg.output_dir = short_out;
        short_cfg.output_format = awj::OutputFormat::webp;
        short_cfg.collision_mode = awj::CollisionMode::suffix_number;
        short_cfg.max_jobs = 1;
        short_cfg.quality = 100;
        auto short_summary = awj::run_batch(short_cfg);
        if (!short_summary || short_summary->ok_count != 1 ||
            !std::filesystem::exists(
                short_out / "20B07A very long filename that must stay complete.webp")) {
          return fail(short_summary ? "short WEBP path did not keep long output name."
                                    : short_summary.error());
        }
      }
    }
  }

  const auto csv = read_text(output_dir / "summary.csv");
  if (csv.find("same-a.webp") == std::string::npos ||
      csv.find(root.string()) != std::string::npos) {
    return fail("summary should contain file names but not full temp root paths.");
  }

  const auto formula_input = input_dir / "=cmd.webp";
  if (auto ok = write_webp(formula_input, std::byte{64}); !ok) {
    return fail(ok.error());
  }
  awj::ImageFile image{.index = 0,
                       .path = formula_input,
                       .bytes = std::filesystem::file_size(formula_input, ec)};
  awj::EncodeResult formula_result{.index = 0,
                                   .input_path = image.path,
                                   .output_path = output_dir / "=cmd.webp",
                                   .original_bytes = image.bytes,
                                   .output_bytes = image.bytes,
                                   .processed = true,
                                   .ok = true,
                                   .message = "=FORMULA"};
  if (auto csv_ok = awj::write_csv(output_dir / "csv-formula", std::span<const awj::EncodeResult>{&formula_result, 1}); !csv_ok) {
    return fail(csv_ok.error());
  }
  const auto formula_csv = read_text(output_dir / "csv-formula" / "summary.csv");
  if (formula_csv.find("'=cmd.webp") == std::string::npos ||
      formula_csv.find("'=FORMULA") == std::string::npos) {
    return fail("summary CSV did not escape formula-like cells.");
  }

  std::filesystem::remove_all(root, ec);
  return 0;
}
