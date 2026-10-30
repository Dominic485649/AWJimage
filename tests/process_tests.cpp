#include <barrier>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <thread>

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
  const auto root = fs::temp_directory_path() / "awjimage-process-tests";
  std::error_code ec;
  fs::remove_all(root, ec);

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
  const auto csv = read_file(root / "csv" / "summary.csv");
  const auto version_cell = "," + std::string{awj::kAwjVersion} + "\n";
  if (!csv.starts_with("\xEF\xBB\xBF" "index,input,") ||
      csv.find("visual_quality_search_trace,decode_stage,source_metadata_available,selected_rendition,rendition_origin,reference_chroma,reference_bit_depth,") ==
          std::string::npos ||
      csv.find("heif_tile_parallelism,heif_codec_thread_budget,awj_version\n") == std::string::npos ||
      csv.find(",heif.gain-map.render,true,hdr,gain-map-reconstructed,444,16,false,false,true,1080,1440,8,") == std::string::npos ||
      csv.find("1.250000") == std::string::npos ||
      !csv.ends_with(version_cell)) {
    return fail("summary.csv did not record the CMake AWJ version.");
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
  if (log.find("broken log line") != std::string::npos ||
      log.find("after malformed log") == std::string::npos) {
    return fail("malformed existing log was not replaced.");
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
  if (log.find("2026-02-31") != std::string::npos ||
      log.find("after invalid calendar date log") == std::string::npos) {
    return fail("invalid calendar date log was not replaced.");
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
  if (log.find(static_cast<char>(0xff)) != std::string::npos ||
      log.find("after invalid UTF-8 log") == std::string::npos) {
    return fail("invalid UTF-8 existing log was not replaced.");
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
  if (log.find("12:34:56] [INFO] truncated") != std::string::npos ||
      log.find("after truncated log") == std::string::npos) {
    return fail("truncated existing log was not replaced.");
  }

  if (!write_file(log_path, "broken concurrent log\n")) {
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
  if (log.find("broken concurrent log") != std::string::npos ||
      log.find("concurrent first") == std::string::npos ||
      log.find("concurrent second") == std::string::npos) {
    return fail("concurrent log validation lost a session.");
  }

  fs::remove_all(root, ec);
  return 0;
}
