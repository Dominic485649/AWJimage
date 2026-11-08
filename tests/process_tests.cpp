#include <atomic>
#include <barrier>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

import awj.config;
import awj.core;

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

bool write_file(const std::filesystem::path& path, std::string_view content) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  return static_cast<bool>(output);
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const auto root = fs::path{AWJ_TEST_RESULTS_ROOT} /
      ("process-" + std::to_string(awj::current_process_id()));
  std::error_code ec;
  if (!fs::create_directories(root, ec) || ec)
    return fail("Dedicated process test directory exists or cannot be created.");

  const auto publish_dir = root / "atomic-publish";
  fs::create_directories(publish_dir, ec);
  const auto temporary = publish_dir / "owned.tmp";
  const auto destination = publish_dir / "new.output";
  if (ec || !write_file(temporary, "first generated payload"))
    return fail("Could not seed the no-replace publication fixture.");
  if (const auto published = awj::core_detail::publish_file_noreplace(temporary, destination);
      published || fs::exists(temporary) || read_file(destination) != "first generated payload")
    return fail("No-replace publication did not consume its temporary and publish the new file.");
  if (!write_file(temporary, "second generated payload"))
    return fail("Could not seed the colliding publication fixture.");
  const auto collision = awj::core_detail::publish_file_noreplace(temporary, destination);
  if (collision != std::errc::file_exists || read_file(temporary) != "second generated payload" ||
      read_file(destination) != "first generated payload")
    return fail("EEXIST publication changed the temporary or existing destination.");
  const auto linked_destination = publish_dir / "linked.output";
  fs::create_symlink(destination, linked_destination, ec);
  if (ec) {
#ifdef _WIN32
    std::fputs("No-replace symlink destination regression NOT COVERED: symlink creation unavailable.\n", stdout);
#else
    return fail("Linux no-replace regression requires its owned destination symlink.");
#endif
  } else {
    const auto linked_collision = awj::core_detail::publish_file_noreplace(temporary, linked_destination);
    if (!linked_collision ||
#ifndef _WIN32
        linked_collision != std::errc::file_exists ||
#endif
        !fs::is_symlink(linked_destination) || read_file(temporary) != "second generated payload" ||
        read_file(destination) != "first generated payload")
      return fail("A symlink destination was replaced or its target changed during publication.");
    const auto missing_target = publish_dir / "missing-target";
    const auto dangling_destination = publish_dir / "dangling.output";
    fs::create_symlink(missing_target, dangling_destination, ec);
    if (ec) return fail("Could not create the owned dangling destination symlink.");
    const auto dangling_collision = awj::core_detail::publish_file_noreplace(temporary, dangling_destination);
    if (!dangling_collision ||
#ifndef _WIN32
        dangling_collision != std::errc::file_exists ||
#endif
        !fs::is_symlink(dangling_destination) || fs::exists(missing_target) ||
        read_file(temporary) != "second generated payload")
      return fail("A dangling symlink was treated as an available publication target.");
  }
  ec.clear();

  awj::EncodeResult result{.index = 0, .processed = true, .ok = true};
  result.decode_stage = "heif.gain-map.render";
  result.source_metadata_available = true;
  result.source_has_icc = true;
  result.rendition.selected = "hdr";
  result.rendition.origin = "gain-map-reconstructed";
  result.rendition.reference_chroma = "444";
  result.rendition.reference_bit_depth = 16;
  result.rendition.gain_map_generated = true;
  result.rendition.gain_map_width = 1080;
  result.rendition.gain_map_height = 1440;
  result.rendition.gain_map_bit_depth = 8;
  result.gain_map_compute_seconds = 1.25;
  result.gain_map_compute_workers = 3;
  if (auto written = awj::write_csv(
          root / "csv", std::span<const awj::EncodeResult>{&result, 1});
      !written) {
    return fail(written.error());
  }
  const auto csv = read_file(root / "csv" / "log" / "summery.csv");
  const auto version_cell = "," + std::string{awj::kAwjVersion} + "\n";
  if (!csv.starts_with("\xEF\xBB\xBF" "index,input,") ||
      csv.find("visual_quality_search_trace,decode_stage,source_metadata_available,selected_rendition,rendition_origin,reference_chroma,reference_bit_depth,") ==
          std::string::npos ||
      csv.find("heif_tile_parallelism,heif_codec_thread_budget,") == std::string::npos ||
      csv.find(",awj_version\n") == std::string::npos ||
      csv.find(",heif.gain-map.render,true,hdr,gain-map-reconstructed,444,16,false,false,true,1080,1440,8,") == std::string::npos ||
      csv.find("1.250000") == std::string::npos ||
      !csv.ends_with(version_cell)) {
    return fail("log/summery.csv did not record the CMake AWJ version.");
  }

  const auto log_root = root / "logger";
  const auto log_dir = log_root / "log";
  const auto log_path = log_dir / "awj.log";
  fs::create_directories(log_dir, ec);
  if (ec) {
    return fail("failed to create logger test directory.");
  }

  constexpr std::string_view old_line =
      "[2026-07-13 12:34:56] [INFO] old valid entry\n";
  if (!write_file(log_path, old_line)) {
    return fail("failed to seed valid log.");
  }
  {
    awj::FileLogger logger{log_root};
    logger.info("new valid\nentry");
  }
  auto log = read_file(log_path);
  const auto old_position = log.find(old_line);
  const auto session_position = log.find("===== NEW SESSION START =====");
  const auto new_position = log.find("new valid entry");
  if (old_position != 0 || session_position == std::string::npos ||
      new_position == std::string::npos || session_position > new_position) {
    return fail("valid existing log was not appended in order.");
  }

  if (!write_file(log_path, "broken log line\n")) {
    return fail("failed to seed malformed log.");
  }
  {
    awj::FileLogger logger{log_root};
    logger.info("after malformed log");
  }
  log = read_file(log_path);
  if (log != "broken log line\n") {
    return fail("Unrecognized user log content was overwritten.");
  }

  if (!write_file(log_path,
                  "[2026-02-31 12:34:56] [INFO] invalid calendar date\n")) {
    return fail("failed to seed invalid calendar date log.");
  }
  {
    awj::FileLogger logger{log_root};
    logger.info("after invalid calendar date log");
  }
  log = read_file(log_path);
  if (log != "[2026-02-31 12:34:56] [INFO] invalid calendar date\n") {
    return fail("Unrecognized calendar-date log was overwritten.");
  }

  std::string invalid_utf8 =
      "[2026-07-13 12:34:56] [INFO] invalid byte ";
  invalid_utf8.push_back(static_cast<char>(0xff));
  invalid_utf8.push_back('\n');
  if (!write_file(log_path, invalid_utf8)) {
    return fail("failed to seed invalid UTF-8 log.");
  }
  {
    awj::FileLogger logger{log_root};
    logger.info("after invalid UTF-8 log");
  }
  log = read_file(log_path);
  if (log != invalid_utf8) {
    return fail("Unrecognized UTF-8 user log was overwritten.");
  }

  if (!write_file(log_path,
                  "[2026-07-13 12:34:56] [INFO] truncated")) {
    return fail("failed to seed truncated log.");
  }
  {
    awj::FileLogger logger{log_root};
    logger.info("after truncated log");
  }
  log = read_file(log_path);
  if (log != "[2026-07-13 12:34:56] [INFO] truncated") {
    return fail("Unrecognized truncated user log was overwritten.");
  }

  if (!write_file(log_path, old_line)) {
    return fail("failed to seed concurrent logger test.");
  }
  std::barrier start{3};
  std::jthread first{[&] {
    start.arrive_and_wait();
    awj::FileLogger logger{log_root};
    logger.info("concurrent first");
  }};
  std::jthread second{[&] {
    start.arrive_and_wait();
    awj::FileLogger logger{log_root};
    logger.info("concurrent second");
  }};
  start.arrive_and_wait();
  first.join();
  second.join();
  log = read_file(log_path);
  if (!log.starts_with(old_line) ||
      log.find("concurrent first") == std::string::npos ||
      log.find("concurrent second") == std::string::npos) {
    return fail("concurrent log validation lost a session.");
  }

  // Unknown CSVs, links and lock aliases are user-owned paths, not reports.
  const auto unknown = root / "unknown";
  fs::create_directories(unknown / "log", ec);
  if (ec || !write_file(unknown / "log" / "summery.csv", "user,csv\nkeep,me\n"))
    return fail("Could not seed unrelated CSV.");
  if (awj::write_csv(unknown, std::span<const awj::EncodeResult>{&result, 1}) ||
      read_file(unknown / "log" / "summery.csv") != "user,csv\nkeep,me\n")
    return fail("Unrelated user CSV was overwritten.");
  if (fs::exists(root / "csv" / "summary.csv"))
    return fail("The old root-level summary.csv was still written.");
  result.input_sha256_snapshot = std::string(64, 'a');
  result.input_path = root / "already-recycled-input.webp";
  if (auto written = awj::write_csv(root / "csv", std::span<const awj::EncodeResult>{&result, 1}); !written)
    return fail(written.error());
  if (read_file(root / "csv" / "log" / "summery.csv").find(std::string(64, 'a')) == std::string::npos)
    return fail("CSV lost the pre-recycle source SHA-256 snapshot.");

  const auto stable_csv = read_file(root / "csv" / "log" / "summery.csv");
  std::atomic<bool> csv_ok{true};
  std::barrier csv_start{3};
  std::jthread csv_first{[&] {
    csv_start.arrive_and_wait();
    if (!awj::write_csv(root / "csv", std::span<const awj::EncodeResult>{&result, 1})) csv_ok = false;
  }};
  std::jthread csv_second{[&] {
    csv_start.arrive_and_wait();
    if (!awj::write_csv(root / "csv", std::span<const awj::EncodeResult>{&result, 1})) csv_ok = false;
  }};
  csv_start.arrive_and_wait();
  csv_first.join(); csv_second.join();
  if (!csv_ok || read_file(root / "csv" / "log" / "summery.csv") != stable_csv)
    return fail("Concurrent CSV writes failed.");
  for (const auto& entry : fs::directory_iterator{root / "csv" / "log"})
    if (entry.path().filename().string().starts_with("summery.csv.tmp-"))
      return fail("CSV left an obsolete temporary report.");

  const auto victim = root / "victim";
  fs::create_directories(victim, ec);
  if (ec || !write_file(victim / "summery.csv", "private CSV\n") ||
      !write_file(victim / "awj.log", "private log\n"))
    return fail("Could not seed protected log targets.");
  const auto alias = root / "linked-log";
  fs::create_directories(alias, ec);
  fs::create_directory_symlink(victim, alias / "log", ec);
  if (!ec) {
    awj::FileLogger linked{alias};
    if (linked.enabled() || awj::write_csv(alias, std::span<const awj::EncodeResult>{&result, 1}) ||
        read_file(victim / "summery.csv") != "private CSV\n" ||
        read_file(victim / "awj.log") != "private log\n")
      return fail("A symlinked log directory was followed.");
  } else std::fputs("Log symlink regression not covered: symlink creation unavailable.\n", stdout);
  ec.clear();
  const auto hard = root / "hardlink-log";
  fs::create_directories(hard / "log", ec);
  fs::create_hard_link(victim / "summery.csv", hard / "log" / "summery.csv", ec);
  if (ec) return fail("Could not create dedicated CSV hard-link fixture.");
  if (awj::write_csv(hard, std::span<const awj::EncodeResult>{&result, 1}) ||
      read_file(victim / "summery.csv") != "private CSV\n")
    return fail("Hard-linked user CSV was replaced.");
  const auto lock_alias = root / "linked-lock";
  fs::create_directories(lock_alias / "log", ec);
  fs::create_hard_link(victim / "awj.log", lock_alias / "log" / ".summery.csv.lock", ec);
  if (ec || awj::write_csv(lock_alias, std::span<const awj::EncodeResult>{&result, 1}) ||
      read_file(victim / "awj.log") != "private log\n")
    return fail("A hard-linked user file was accepted as the CSV lock.");

  // Validate raw components before .., and keep the checked absolute spelling
  // for log I/O. All source/decoy/log targets here are this run's own fixtures.
  const auto component_root = root / "raw-log-components";
  const auto p = component_root / "P", q = component_root / "Q";
  fs::create_directories(p / "plain", ec);
  fs::create_directories(q / "sub", ec);
  fs::create_directories(q / "log", ec);
  if (ec) return fail("Could not create raw log-path fixtures.");
  const auto ordinary_output = p / "plain" / "..";
  if (!awj::core_detail::safe_unlinked_path(ordinary_output / "log", true))
    return fail("Ordinary no-link .. log paths were unnecessarily rejected.");
  {
    awj::FileLogger ordinary{ordinary_output};
    if (!ordinary.enabled()) return fail("Ordinary .. log path did not initialize.");
    ordinary.info("safe dotdot log");
  }
  if (!awj::write_csv(ordinary_output, std::span<const awj::EncodeResult>{&result, 1}) ||
      read_file(p / "log" / "awj.log").find("safe dotdot log") == std::string::npos ||
      !fs::exists(p / "log" / "summery.csv"))
    return fail("No-link .. logs did not reach their actual destination.");
  const auto ordinary_log = read_file(p / "log" / "awj.log");
  const auto ordinary_csv = read_file(p / "log" / "summery.csv");
  fs::create_directory_symlink(q / "sub", p / "link", ec);
  if (ec) {
#ifdef _WIN32
    std::fputs("Raw link/.. log regression NOT COVERED: reparse creation unavailable.\n", stdout);
#else
    return fail("Linux raw link/.. log regression requires its owned directory symlink.");
#endif
  } else {
    const auto raw_output = p / "link" / "..";
    // Read-only checks fail first, so testing an old implementation cannot
    // accidentally write through an unsafe chain before its rejection is seen.
    if (awj::core_detail::safe_unlinked_path(raw_output / "log", true) ||
        awj::core_detail::prepare_log_directory(raw_output))
      return fail("Raw link/.. log directory passed safety validation.");
    if (!write_file(q / "log" / "awj.log", "unknown user log\n") ||
        !write_file(q / "log" / "summery.csv", "unknown,user,csv\n"))
      return fail("Could not seed unknown logs behind link/.. .");
    {
      awj::FileLogger rejected{raw_output};
      if (rejected.enabled()) return fail("FileLogger accepted a raw link/.. output directory.");
    }
    if (awj::write_csv(raw_output, std::span<const awj::EncodeResult>{&result, 1}) ||
        read_file(q / "log" / "awj.log") != "unknown user log\n" ||
        read_file(q / "log" / "summery.csv") != "unknown,user,csv\n" ||
        read_file(p / "log" / "awj.log") != ordinary_log ||
        read_file(p / "log" / "summery.csv") != ordinary_csv ||
        fs::exists(q / "log" / ".awj.log.lock") || fs::exists(q / "log" / ".summery.csv.lock"))
      return fail("Raw link/.. log I/O changed an unrelated path.");
    const auto linked_log = component_root / "victim.log";
    const auto linked_csv = component_root / "victim.csv";
    if (!write_file(linked_log, old_line) || !write_file(linked_csv, stable_csv))
      return fail("Could not seed recognizable linked log targets.");
    fs::remove(q / "log" / "awj.log", ec);
    fs::remove(q / "log" / "summery.csv", ec);
    fs::create_symlink(linked_log, q / "log" / "awj.log", ec);
    if (!ec) fs::create_symlink(linked_csv, q / "log" / "summery.csv", ec);
    if (ec) {
#ifdef _WIN32
      std::fputs("Raw link/.. leaf-link regression NOT COVERED: file symlink creation unavailable.\n", stdout);
#else
      return fail("Linux raw link/.. log regression requires its owned leaf symlinks.");
#endif
    } else {
      awj::FileLogger rejected{raw_output};
      if (rejected.enabled() || awj::write_csv(raw_output, std::span<const awj::EncodeResult>{&result, 1}) ||
          read_file(linked_log) != old_line || read_file(linked_csv) != stable_csv ||
          !fs::is_symlink(q / "log" / "awj.log") || !fs::is_symlink(q / "log" / "summery.csv"))
        return fail("A log leaf link hidden behind link/.. was followed or replaced.");
    }
  }
  ec.clear();

  // Numbering reserves all sources up front and keeps the complete extension.
  auto cfg = awj::default_app_config();
  cfg.output_dir = root / "numbered";
  cfg.output_format = awj::OutputFormat::avif;
  cfg.append_png_suffix = true;
  cfg.output_template = L"name";
  fs::create_directories(cfg.output_dir, ec);
  if (ec || !write_file(cfg.output_dir / "name.avif.png", "existing output"))
    return fail("Could not seed collision output.");
  std::vector<awj::ImageFile> files{
      {.index = 0, .path = root / "source.jpg"},
      {.index = 1, .path = root / "source.jpg"},
      {.index = 2, .path = cfg.output_dir / "name(1).avif.png"}};
  if (auto resolved = awj::resolve_batch_output_paths(cfg, files); !resolved)
    return fail(resolved.error());
  for (std::size_t i = 0; i < files.size(); ++i)
    if (!files[i].output_path_resolved || files[i].resolved_output_path.filename() !=
        ("name(" + std::to_string(i + 2) + ").avif.png"))
      return fail("Default numbering overwrote a reserved input or split .avif.png.");
  if (read_file(cfg.output_dir / "name.avif.png") != "existing output")
    return fail("Numbering changed an existing output.");
  if (auto advanced = awj::resolve_collision_output_path(
          cfg.output_dir / "name(1).avif.png", awj::CollisionMode::suffix_number,
          nullptr, L".avif.png"); !advanced || advanced->filename() != "name(1).avif.png")
    return fail("An available pre-numbered filename should remain unchanged.");

  fs::remove_all(root, ec);
  if (ec) return fail("Could not clean dedicated process test files.");
  return 0;
}
