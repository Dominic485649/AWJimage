#include "../src/ui/studio_config.h"
#include "../src/ui/studio_json.h"
#include "../src/ui/studio_queue_format.h"
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{input}, {}};
}
void memory(const char* phase, std::size_t count, bool legacy) {
  PROCESS_MEMORY_COUNTERS_EX info{};
  check(GetProcessMemoryInfo(GetCurrentProcess(),
      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info)), "memory query failed");
  std::printf("queue=%zu legacy=%d phase=%s working_set=%zu private_bytes=%zu peak_private=%zu\n",
              count, legacy, phase, info.WorkingSetSize, info.PrivateUsage, info.PeakPagefileUsage);
}
void queue_check(std::size_t count, bool legacy) {
  using namespace awj::studio;
  namespace fs = std::filesystem;
  awj::AppConfig cfg;
  cfg.output_dir = fs::temp_directory_path() / "awj-queue-state-output";
  std::vector<QueueImageItem> queue;
  queue.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    queue.push_back(QueueImageItem{.id = i + 1,
      .path = fs::path{L"C:\\queue-fixture\\中文 input"} / std::format(L"image-{}.png", i),
      .source_root = L"C:\\queue-fixture\\中文 input", .relative_dir = L"subfolder",
      .bytes = i + 42, .status = i % 2 ? QueueItemStatus::failed : QueueItemStatus::pending,
      .log_text = "UI-only diagnostic text which must not enter the worker snapshot"});
  }
  memory("idle", count, legacy);
  const auto start = std::chrono::steady_clock::now();
  auto snapshot = legacy ? queue : std::vector<QueueImageItem>{};
  auto files = build_run_files(cfg, legacy ? snapshot : queue, false);
  check(files.has_value() && files->size() == count, "queue input count changed");
  for (std::size_t i = 0; i < count; ++i) {
    check((*files)[i].index == i && (*files)[i].path == queue[i].path &&
          (*files)[i].relative_dir == queue[i].relative_dir && (*files)[i].bytes == queue[i].bytes,
          "queue order or encoder input changed");
  }
  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
  memory("prepared", count, legacy);
  std::printf("queue=%zu legacy=%d preparation_ms=%.3f\n", count, legacy, elapsed);
  files = std::vector<awj::ImageFile>{};
  std::vector<QueueImageItem>{}.swap(snapshot);
  if (count == 10) {
    auto failed = build_run_files(cfg, queue, true);
    check(failed && failed->size() == 5 && failed->front().path == queue[1].path,
          "failed-only selection changed");
    queue[0].status = QueueItemStatus::done;
    queue[1].status = QueueItemStatus::canceled;
    check(build_run_files(cfg, queue, false)->size() == 9, "completed item was requeued");
    cfg.output_template = L"{sha256}.avif";
    check(!build_run_files(cfg, queue, false), "missing source hash did not fail closed");
  }
  std::vector<QueueImageItem>{}.swap(queue);
  check(queue.capacity() == 0 && snapshot.capacity() == 0, "queue storage retained");
  memory("clear", count, legacy);
}
void config_check() {
  using namespace awj::studio;
  namespace fs = std::filesystem;
  const auto path = studio_config_path();
  check(!path.empty() && !fs::exists(path), "test config path must be isolated and empty");
  auto current = StudioConfigSnapshot{};
  const auto defaults = current;
  current.language_index = 1;
  current.last_verified_manifest_v2_sequence = 42;
  current.update_manifest_v2_raw = "signed v2 bytes";
  current.update_manifest_v2_signature = "signature";
  current.update_keyring_raw = "signed keyring bytes";
  current.update_keyring_signature = "keyring signature";
  current.menu_preset_description = "menu preset regression description";
  {
    std::ofstream old(path);
    old << R"({"last_verified_manifest_sequence":17,"update_manifest_raw":"old","update_manifest_signature":"old signature"})";
  }
  check(write_studio_config_file(current, defaults).has_value(), "config write failed");
  const auto first = read(path);
  const auto parsed = awj::studio_json::parse_jsonc_config(first);
  check(parsed.has_value() && parsed->at("language_index").integer == 1 &&
        parsed->at("last_verified_manifest_v2_sequence").integer == 42 &&
        parsed->at("update_manifest_v2_raw").string == current.update_manifest_v2_raw &&
        parsed->at("update_keyring_raw").string == current.update_keyring_raw &&
        parsed->at("menu_preset_description").string == current.menu_preset_description,
        "whitelist lost current config or v2 state");
  for (const auto* key : {"last_verified_manifest_sequence", "update_manifest_raw", "update_manifest_signature"})
    check(!parsed->contains(key), "legacy UI cache was persisted");
  check(write_studio_config_file(current, defaults).has_value() && read(path) == first,
        "config rewrite was not idempotent");
  {
    auto locked = adopt_win32_handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                               nullptr, OPEN_EXISTING, 0, nullptr));
    check(static_cast<bool>(locked), "cannot lock config fixture");
    check(!write_studio_config_file(defaults, defaults), "locked target was replaced");
    check(read(path) == first && !fs::exists(path.wstring() + L".tmp"),
          "failed atomic write damaged original or left temporary file");
  }
  fs::remove(path);
}
}
int main(int argc, char** argv) try {
  if (argc == 4 && std::string_view{argv[1]} == "--queue-benchmark") {
    queue_check(std::stoull(argv[2]), std::string_view{argv[3]} == "legacy");
  } else { queue_check(10, false); config_check(); }
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
