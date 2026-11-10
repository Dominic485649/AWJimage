#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#endif

import awj.animation;
import awj.codec;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.config;
import awj.core;
import awj.image;
import awj.image_memory_plan;
import awj.large_image_plan;
import awj.native_backend;
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

bool valid_stage_sum(const awj::EncodeResult& result) {
  double sum{};
  for (const auto value : {result.decode_seconds, result.resize_seconds, result.prepare_seconds,
                           result.encode_seconds, result.write_seconds}) {
    if (!std::isfinite(value) || (value < 0.0 && value != -1.0)) return false;
    if (value >= 0.0) sum += value;
  }
  return std::isfinite(result.seconds) && sum <= result.seconds + 0.000001;
}

struct AnimationReadObservation {
  double seconds{};
  std::size_t calls{};
};

class ObservedAnimationReader final : public awj::AnimationReader {
 public:
  ObservedAnimationReader(std::size_t frames, AnimationReadObservation& observed, int fault = 0)
      : observed_{observed}, fault_{fault}, image_{make_test_image()} {
    info_.width = image_.width;
    info_.height = image_.height;
    info_.durations.assign(frames, awj::FrameDuration{1, 10});
    info_.decoder_id = "observed-test-reader";
  }
  const awj::AnimationInfo& info() const noexcept override { return info_; }
  const awj::ImageBuffer& frame() const noexcept override { return image_; }
  std::uint64_t memory_usage_bytes() const noexcept override { return image_.planes[0].bytes.capacity(); }
  awj::compat::expected<bool, std::string> next(awj::compat::stop_token stop = {}) override {
    const auto started = std::chrono::steady_clock::now();
    // 短暂等待仅让漏计/重复计入帧解码可被观察，不是性能断言，也不制造编码或系统负载。
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    observed_.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    ++observed_.calls;
    if (stop.stop_requested()) return awj::compat::unexpected{"test reader canceled"};
    if (observed_.calls == 2 && fault_ == 1) return awj::compat::unexpected{"test reader failure"};
    if (observed_.calls == 2 && fault_ == 2) throw std::length_error{"test reader exception"};
    return observed_.calls <= info_.durations.size();
  }
 private:
  AnimationReadObservation& observed_;
  int fault_{};
  awj::ImageBuffer image_;
  awj::AnimationInfo info_;
};

int animation_stage_timing_contract() {
  const awj::ResourcePlan resources{.file_parallelism = 1, .encoder_threads_per_file = 1,
      .global_thread_budget = 1, .memory_limit_bytes = 64 * 1024 * 1024};
  for (const auto frames : {std::size_t{1}, std::size_t{3}}) {
    for (const bool resize : {false, true}) {
      awj::AppConfig cfg;
      cfg.output_format = awj::OutputFormat::webp;
      if (resize) cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::manual, .scale_percent = 50};
      AnimationReadObservation observed;
      awj::EncodeResult timing;
      const auto started = std::chrono::steady_clock::now();
      awj::native_backend_detail::GeometryAnimationReader reader{
          std::make_unique<ObservedAnimationReader>(frames, observed), cfg, resources, timing};
      auto first = reader.next();
      const auto first_decode = timing.decode_seconds;
      const auto dimension = resize ? 1u : 2u;
      if (!first || !*first || reader.frame().width != dimension || reader.frame().height != dimension ||
          first_decode < observed.seconds || timing.resize_seconds < 0 ||
          timing.resize_workers != (resize ? 1 : 0))
        return fail("The first animation frame omitted decode/resize timing or geometry diagnostics.");
      {
        auto timer = reader.time_encode(timing.encode_seconds);
        for (std::size_t i = 1; i < frames; ++i) {
          auto next = reader.next();
          if (!next || !*next || reader.frame().width != dimension || reader.frame().height != dimension)
            return fail("Later animation geometry was not applied to each frame.");
        }
        auto end = reader.next();
        if (!end || *end) return fail("Observed animation reader did not reach EOF.");
      }
      timing.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      if (observed.calls != frames + 1 || timing.decode_seconds < observed.seconds ||
          timing.decode_seconds <= first_decode || !valid_stage_sum(timing) ||
          timing.resize_workers != (resize ? 1 : 0) || (!resize && timing.resize_seconds != 0.0) ||
          reader.read_seconds() + 0.000001 < timing.decode_seconds + timing.resize_seconds)
        return fail("Animation decode/resize was omitted, double-counted as encode, or workers were summed across frames.");
    }
  }
  for (const int fault : {1, 2, 3}) {
    awj::AppConfig cfg;
    cfg.output_format = awj::OutputFormat::webp;
    cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::manual, .scale_percent = 50};
    AnimationReadObservation observed;
    awj::EncodeResult timing;
    const auto started = std::chrono::steady_clock::now();
    awj::native_backend_detail::GeometryAnimationReader reader{
        std::make_unique<ObservedAnimationReader>(3, observed, fault), cfg, resources, timing};
    const auto first = reader.next();
    if (!first || !*first) return fail("Cannot read first frame for partial animation timing.");
    const auto first_decode = timing.decode_seconds;
    const auto first_resize = timing.resize_seconds;
    awj::compat::stop_source stop;
    if (fault == 3) stop.request_stop();
    bool rejected{};
    try {
      auto timer = reader.time_encode(timing.encode_seconds);
      const auto next = reader.next(stop.get_token());
      rejected = !next;
    } catch (const std::length_error&) {
      rejected = fault == 2;
    }
    timing.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!rejected || timing.decode_seconds <= first_decode || timing.decode_seconds < observed.seconds ||
        timing.resize_seconds < first_resize || timing.resize_workers != 0 ||
        timing.encode_seconds < 0 || !valid_stage_sum(timing))
      return fail("Failed/canceled/throwing animation read lost partial timing or left a successful worker count.");
  }
  awj::AppConfig cfg;
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::manual, .scale_percent = 50};
  awj::ImageDecodeResult paired{.image = make_test_image(),
      .gain_map_alternative = awj::GainMapAlternative{.image = make_test_image()}};
  awj::EncodeResult timing;
  if (auto resized = awj::native_backend_detail::apply_image_size_limit(paired, cfg, {}, resources, timing);
      !resized || paired.image.width != 1 || paired.gain_map_alternative->image.width != 1 ||
      timing.resize_seconds < 0 || timing.resize_workers != 1)
    return fail("Paired renditions did not accumulate resize time with a maximum worker count.");
  awj::ImageDecodeResult broken_pair{.image = make_test_image(),
      .gain_map_alternative = awj::GainMapAlternative{.image = make_test_image()}};
  broken_pair.gain_map_alternative->image.planes.clear();
  awj::EncodeResult partial;
  if (awj::native_backend_detail::apply_image_size_limit(broken_pair, cfg, {}, resources, partial) ||
      broken_pair.image.width != 1 || partial.resize_seconds < 0 || partial.resize_workers != 0)
    return fail("A failed second rendition discarded first-layer resize time or left successful workers.");
  auto tight = resources;
  tight.memory_limit_bytes = 1;
  AnimationReadObservation observed;
  awj::EncodeResult failed;
  awj::native_backend_detail::GeometryAnimationReader reader{
      std::make_unique<ObservedAnimationReader>(1, observed), cfg, tight, failed};
  if (reader.next() || failed.decode_seconds < observed.seconds || failed.resize_seconds < 0 || failed.resize_workers != 0)
    return fail("Rejected animation geometry lost measured time or reported successful workers.");
  return 0;
}

int resize_csv_contract(const std::filesystem::path& root) {
  awj::EncodeResult measured;
  measured.resize_seconds = 0.25;
  measured.resize_workers = 3;
  awj::NativeEncodeResult native;
  awj::native_backend_detail::merge_stage_timing(native, measured);
  awj::EncodeResult copied;
  awj::native_backend_detail::copy_native_result(native, copied);
  if (copied.resize_seconds != 0.25 || copied.resize_workers != 3)
    return fail("Native timing merge/copy lost resize diagnostics.");
  awj::native_backend_detail::merge_stage_timing(native, awj::EncodeResult{});
  awj::native_backend_detail::copy_native_result(native, copied);
  if (copied.resize_seconds != -1.0 || copied.resize_workers != 0)
    return fail("An unobserved attempt retained stale native resize diagnostics.");
  std::array<awj::EncodeResult, 3> results;
  if (results[0].resize_seconds != -1.0 || results[0].resize_workers != 0 ||
      awj::EncodeTimingDiagnostics{}.resize_seconds != -1.0 || awj::EncodeTimingDiagnostics{}.resize_workers != 0)
    return fail("Unobserved resize diagnostics do not default to unknown.");
  results[1].resize_seconds = 0.0;
  results[2].resize_seconds = 0.12345678;
  results[2].resize_workers = 2;
  const auto output = root / "resize-csv";
  if (auto written = awj::write_csv(output, results); !written) return fail(written.error());
  const auto path = output / "log" / "summery.csv";
  const auto csv = read_text(path);
  const auto header_end = csv.find('\n');
  if (header_end == std::string::npos || !std::string_view{csv}.substr(0, header_end).ends_with(
          "memory_target_alpha,awj_version,resize_seconds,resize_workers"))
    return fail("Resize CSV fields were not appended after the complete legacy schema.");
  std::size_t start = header_end + 1;
  const auto columns = std::count(csv.begin(), csv.begin() + header_end, ',');
  const std::array<std::string_view, 3> tails{",,0", ",0.000000,0", ",0.123457,2"};
  for (const auto tail : tails) {
    const auto end = csv.find('\n', start);
    if (end == std::string::npos) return fail("Resize CSV row is missing.");
    const auto row = std::string_view{csv}.substr(start, end - start);
    if (!row.ends_with(tail) || std::count(row.begin(), row.end(), ',') != columns)
      return fail("Resize CSV row lost unknown/zero/worker values or shifted old columns.");
    start = end + 1;
  }
  if (start != csv.size()) return fail("Resize CSV wrote unexpected extra rows.");
  if (auto written = awj::write_csv(output, results); !written) return fail("Current resize CSV was not replaceable.");
  const auto legacy = csv.substr(0, header_end - std::string_view{",resize_seconds,resize_workers"}.size()) + '\n';
  { std::ofstream stream{path, std::ios::binary | std::ios::trunc}; stream << legacy; }
  if (auto written = awj::write_csv(output, results); !written)
    return fail("The exact legacy AWJ CSV schema was not recognized after appending resize fields.");
  const auto foreign = legacy.substr(0, legacy.size() - 1) + ",foreign_column\n";
  { std::ofstream stream{path, std::ios::binary | std::ios::trunc}; stream << foreign; }
  if (awj::write_csv(output, results) || read_text(path) != foreign)
    return fail("Appending resize fields weakened CSV ownership checks for an unknown schema.");
  return 0;
}

int queue_path_component_contract(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  const auto base = root / "raw-path-components";
  const auto p = base / "P", q = base / "Q";
  const auto decoy = p / "image.webp", selected = q / "image.webp";
  const auto output = base / "out" / "result.png";
  std::error_code ec;
  fs::create_directories(p / "plain", ec);
  fs::create_directories(q / "sub", ec);
  fs::create_directories(output.parent_path(), ec);
  if (ec) return fail("Could not create raw-path component fixtures.");
  const auto seed = [](const fs::path& path, std::string_view text) {
    std::ofstream stream{path, std::ios::binary};
    stream << text;
    return static_cast<bool>(stream);
  };
  if (!seed(decoy, "P-decoy!") || !seed(selected, "Q-source") || !seed(output, "output!!"))
    return fail("Could not seed equal-size source/decoy fixtures.");
  const auto plain = p / "plain" / ".." / "image.webp";
  const auto decoy_identity = awj::pipeline_detail::recycle_file_identity(decoy);
  const auto plain_identity = awj::pipeline_detail::recycle_file_identity(plain);
  if (!decoy_identity || !plain_identity || *decoy_identity != *plain_identity ||
      !awj::core_detail::safe_unlinked_path(plain.parent_path(), true))
    return fail("Ordinary no-link .. paths must remain usable.");
  const auto relative_base = fs::relative(p, fs::current_path(), ec);
  if (ec) return fail("Could not locate the relative path fixture.");
  if (!relative_base.empty()) {
    const auto relative = relative_base / "plain" / ".." / "image.webp";
    const auto relative_identity = awj::pipeline_detail::recycle_file_identity(relative);
    if (!relative_identity || *relative_identity != *decoy_identity)
      return fail("Relative no-link .. paths must resolve to the actual file.");
  } else std::fputs("Relative path subcase NOT COVERED: fixture and CWD use different roots.\n", stdout);
  auto cfg = awj::default_app_config();
  cfg.input_path = plain;
  cfg.output_dir = output.parent_path();
  cfg.output_format = awj::OutputFormat::png;
  cfg.delete_source = true;
  std::vector<awj::ImageFile> files{{.index = 0, .path = plain, .bytes = 8,
      .resolved_output_path = output, .output_path_resolved = true}};
  auto ordinary = awj::pipeline_detail::make_source_recycle_plan(cfg, files);
  if (!ordinary || !ordinary->entries[0].identity || !ordinary->entries[0].retain_reason.empty() ||
      ordinary->entries[0].source != plain)
    return fail("A safe source spelling was normalized away or unnecessarily rejected.");
  fs::create_directory_symlink(q / "sub", p / "link", ec);
  if (ec) {
#ifdef _WIN32
    std::fputs("Raw link/.. source regression NOT COVERED: reparse creation unavailable.\n", stdout);
    return 0;
#else
    return fail("Linux raw link/.. regression requires its owned directory symlink.");
#endif
  }
  const auto raw = p / "link" / ".." / "image.webp";
#ifndef _WIN32
  if (!fs::equivalent(raw, selected, ec) || ec || fs::file_size(raw) != fs::file_size(decoy))
    return fail("link/.. fixture did not select Q/image with an equal-size P/image decoy.");
#endif
  // These tests never call the recycler with a fabricated successful result.
  // Fail using read-only planning/validation first, even against an old binary.
  if (awj::core_detail::safe_unlinked_path(raw.parent_path(), true) ||
      awj::pipeline_detail::recycle_file_identity(raw))
    return fail("An unsafe raw link/.. source/output chain passed identity validation.");
  const auto preserved = awj::core_detail::absolute_path_preserving_components(raw, ec);
  if (ec || preserved != raw)
    return fail("The original link/.. output spelling was changed before validation.");
  files[0].path = raw;
  auto unsafe = awj::pipeline_detail::make_source_recycle_plan(cfg, files);
  if (!unsafe || unsafe->entries[0].identity || unsafe->entries[0].retain_reason.empty() ||
      unsafe->entries[0].source != raw)
    return fail("An unsafe source chain acquired a recyclable decoy identity.");
  // Windows may apply native dot processing differently; protect the target
  // of this platform's canonical dependency observation without assuming Q.
  const auto dependency_target = fs::weakly_canonical(raw, ec);
  if (ec || (dependency_target != selected && dependency_target != decoy))
    return fail("Unexpected canonical dependency target in the owned alias fixture.");
  const auto dependency_payload = read_text(dependency_target);
  const auto alias = q / "shared-input.webp";
  fs::create_hard_link(dependency_target, alias, ec);
  if (ec) return fail("Could not create owned input-dependency alias.");
  auto shared = files;
  shared.push_back(awj::ImageFile{.index = 1, .path = alias, .bytes = 8,
      .resolved_output_path = output.parent_path() / "other.png", .output_path_resolved = true});
  auto dependencies = awj::pipeline_detail::make_source_recycle_plan(cfg, shared);
  if (!dependencies || dependencies->entries[0].identity ||
      dependencies->entries[1].retain_reason.find("重复源") == std::string::npos)
    return fail("A non-eligible alias lost protection for another input sharing its actual file.");
  const auto output_alias = output.parent_path() / "input-alias.png";
  fs::create_hard_link(dependency_target, output_alias, ec);
  if (ec) return fail("Could not create owned output-dependency alias.");
  files[0].resolved_output_path = output_alias;
  if (awj::pipeline_detail::make_source_recycle_plan(cfg, files))
    return fail("A non-eligible alias lost hard-link input/output protection.");
  if (read_text(decoy) != "P-decoy!" || read_text(selected) != "Q-source" ||
      read_text(alias) != dependency_payload || read_text(output) != "output!!")
    return fail("Raw-path planning changed a source, decoy, or committed output fixture.");
  return 0;
}

int queue_core_contract(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  if (const auto checked = queue_path_component_contract(root)) return checked;
  const auto input_dir = root / "queue-input";
  const auto source = input_dir / "queue-contract.webp";
  std::error_code ec;
  fs::create_directories(input_dir, ec);
  if (ec) return fail("Could not create dedicated queue inputs.");
  if (auto written = write_webp(source); !written) return fail(written.error());
  const auto source_hash = awj::file_sha256_hex(source);
  if (!source_hash) return fail(source_hash.error());
  auto cfg = awj::default_app_config();
  cfg.input_path = source;
  cfg.output_format = awj::OutputFormat::png;
  cfg.quality = 100;
  cfg.max_jobs = 1;
  for (const bool log : {false, true}) for (const bool summary : {false, true}) {
    cfg.write_log = log;
    cfg.write_summary = summary;
    cfg.output_dir = root / std::format("queue-log-{}-{}", log, summary);
    const auto converted = awj::run_batch(cfg);
    if (!converted || converted->ok_count != 1 || !fs::exists(source) ||
        fs::exists(cfg.output_dir / "log" / "awj.log") != (log || summary) ||
        fs::exists(cfg.output_dir / "log" / "summery.csv") != (log || summary) ||
        fs::exists(cfg.output_dir / "summary.csv"))
      return fail(converted ? "Direct AppConfig legacy log bools did not control both outputs." : converted.error());
  }
  cfg.write_log = cfg.write_summary = false;
  {
    auto numbered = cfg;
    numbered.input_path = root / "queue-number-input";
    numbered.output_dir = root / "queue-number-output";
    numbered.output_template = L"same";
    fs::create_directories(numbered.input_path, ec);
    fs::create_directories(numbered.output_dir, ec);
    if (ec) return fail("Could not create numbered queue fixture.");
    if (auto a = write_webp(numbered.input_path / "a.webp"); !a) return fail(a.error());
    if (auto b = write_webp(numbered.input_path / "b.webp", std::byte{23}); !b) return fail(b.error());
    { std::ofstream existing{numbered.output_dir / "same.png", std::ios::binary}; existing << "user output"; }
    const auto converted = awj::run_batch(numbered);
    if (!converted || converted->ok_count != 2 ||
        !fs::exists(numbered.output_dir / "same(1).png") ||
        !fs::exists(numbered.output_dir / "same(2).png") ||
        read_text(numbered.output_dir / "same.png") != "user output" ||
        !fs::exists(numbered.input_path / "a.webp") || !fs::exists(numbered.input_path / "b.webp"))
      return fail("Default queue numbering overwrote an existing/batch output or its input.");
  }
  cfg.delete_source = true;
  cfg.output_dir = root / "queue-guards";
  fs::create_directories(cfg.output_dir, ec);
  const auto output = cfg.output_dir / "queue-contract.png";
  fs::copy_file(root / "queue-log-false-false" / "queue-contract.png", output, ec);
  if (ec) return fail("Could not seed a committed queue output.");
  std::vector<awj::ImageFile> files{{.index = 0, .path = source, .bytes = fs::file_size(source),
      .resolved_output_path = output, .output_path_resolved = true}};
  auto plan = awj::pipeline_detail::make_source_recycle_plan(cfg, files);
  if (!plan) return fail(plan.error());
#ifdef _WIN32
  {
    const auto output_identity = awj::pipeline_detail::recycle_file_identity(output);
    if (!output_identity || !plan->entries.front().identity) return fail("Missing recycle identity fixture.");
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    struct ComScope { HRESULT value; ~ComScope() { if (SUCCEEDED(value)) CoUninitialize(); } } com{initialized};
    if (FAILED(initialized)) return fail("Could not initialize the read-only Shell parsing probe.");
    auto shell_source = source;
    shell_source.make_preferred();
    auto slash_source = shell_source.native();
    std::replace(slash_source.begin(), slash_source.end(), L'\\', L'/');
    Microsoft::WRL::ComPtr<IShellItem> raw_item, item;
    const HRESULT raw_hr = SHCreateItemFromParsingName(slash_source.c_str(), nullptr, IID_PPV_ARGS(&raw_item));
    const HRESULT shell_hr = SHCreateItemFromParsingName(shell_source.c_str(), nullptr, IID_PPV_ARGS(&item));
    std::fprintf(stdout, "Read-only Shell path probe: slash=0x%08X, preferred=0x%08X.\n",
        static_cast<unsigned int>(raw_hr), static_cast<unsigned int>(shell_hr));
    if (FAILED(shell_hr)) return fail("The Shell could not parse the preferred-separator fixture path.");
    awj::pipeline_detail::RecycleOnlySink sink{source, *plan->entries.front().identity,
        output, *output_identity, {}};
    if (sink.PreDeleteItem(TSF_NORMAL, item.Get()) != E_ABORT || sink.permitted ||
        sink.rejection != "permanent-delete notification")
      return fail("The Shell sink did not veto/diagnose a permanent-delete notification.");
    awj::pipeline_detail::RecycleOnlySink valid{source, *plan->entries.front().identity,
        output, *output_identity, {}};
    if (valid.PreDeleteItem(TSF_DELETE_RECYCLE_IF_POSSIBLE, item.Get()) != S_OK || !valid.permitted)
      return fail("Separator adaptation changed the original validated source identity.");
    auto different = *plan->entries.front().identity;
    ++different.changed;
    awj::pipeline_detail::RecycleOnlySink changed{source, different, output, *output_identity, {}};
    if (changed.PreDeleteItem(TSF_DELETE_RECYCLE_IF_POSSIBLE, item.Get()) != E_ABORT || changed.permitted ||
        changed.rejection != "source identity changed")
      return fail("Shell path adaptation weakened/obscured stable source identity validation.");
  }
#endif
  const awj::EncodeResult committed{.index = 0, .input_path = source, .output_path = output,
      .original_bytes = fs::file_size(source), .output_bytes = fs::file_size(output),
      .processed = true, .ok = true};
  for (int state = 0; state < 6; ++state) {
    auto result = committed;
    auto effective = cfg;
    awj::compat::stop_source stop;
    switch (state) {
      case 0: result.ok = false; break;
      case 1: result.skipped = true; break;
      case 2: result.canceled = true; break;
      case 3: result.processed = false; break;
      case 4: effective.delete_source = false; break;
      case 5: stop.request_stop(); break;
    }
    const auto recycled = awj::pipeline_detail::recycle_successful_source(effective, *plan, result, stop.get_token());
    const auto retained_hash = awj::file_sha256_hex(source);
    if (!recycled || !fs::exists(source) || !retained_hash || *retained_hash != *source_hash)
      return fail("A failed/canceled/skipped/uncommitted/default-preserve result recycled its source.");
  }
  {
    auto missing = committed;
    missing.output_path = cfg.output_dir / "not-written.png";
    if (awj::pipeline_detail::recycle_successful_source(cfg, *plan, missing) || !fs::exists(source))
      return fail("A missing output was treated as committed.");
    auto same = committed;
    same.output_path = source;
    same.output_bytes = same.original_bytes;
    if (awj::pipeline_detail::recycle_successful_source(cfg, *plan, same) || !fs::exists(source))
      return fail("Same-source output was recycled.");
    auto mismatched = committed;
    mismatched.input_path = input_dir / "another.webp";
    if (awj::pipeline_detail::recycle_successful_source(cfg, *plan, mismatched) || !fs::exists(source))
      return fail("A result not bound to the captured source was recycled.");
  }
  {
    auto duplicate_files = files;
    duplicate_files.push_back(files.front());
    duplicate_files.back().index = 1;
    duplicate_files.back().resolved_output_path = cfg.output_dir / "duplicate.png";
    auto duplicates = awj::pipeline_detail::make_source_recycle_plan(cfg, duplicate_files);
    auto result = committed;
    if (!duplicates || awj::pipeline_detail::recycle_successful_source(cfg, *duplicates, result) ||
        !fs::exists(source)) return fail("Duplicate source dependency was not retained.");
  }
  {
    const auto alias = input_dir / "hardlink.webp";
    fs::create_hard_link(source, alias, ec);
    if (ec) return fail("Could not create dedicated hard-link input.");
    auto hard = awj::pipeline_detail::make_source_recycle_plan(cfg, files);
    auto result = committed;
    if (!hard || awj::pipeline_detail::recycle_successful_source(cfg, *hard, result) ||
        !fs::exists(source) || !fs::exists(alias)) return fail("A hard-linked source was recycled.");
    auto aliased_output_files = files;
    aliased_output_files.front().resolved_output_path = alias;
    if (awj::pipeline_detail::make_source_recycle_plan(cfg, aliased_output_files))
      return fail("An output aliasing an input hard link was accepted.");
    fs::remove(alias, ec);
    if (ec) return fail("Could not remove dedicated hard-link fixture.");
  }
  {
    auto captured = awj::pipeline_detail::make_source_recycle_plan(cfg, files);
    if (!captured) return fail(captured.error());
    const auto saved = input_dir / "captured-source.webp";
    fs::rename(source, saved, ec);
    if (ec) return fail("Could not preserve replacement-test source.");
    if (auto replacement = write_webp(source, std::byte{17}); !replacement) return fail(replacement.error());
    const auto replacement_hash = awj::file_sha256_hex(source);
    auto result = committed;
    const auto recycled = awj::pipeline_detail::recycle_successful_source(cfg, *captured, result);
    const auto retained_hash = awj::file_sha256_hex(source);
    if (recycled || !replacement_hash || !retained_hash ||
        *retained_hash != *replacement_hash || !fs::exists(saved))
      return fail("A replaced source pathname was recycled.");
    fs::remove(source, ec);
    fs::rename(saved, source, ec);
    if (ec) return fail("Could not restore replacement-test fixture.");
  }
  cfg.output_dir = root / "queue-cancel";
  awj::compat::stop_source canceled;
  bool started = false;
  const auto canceled_batch = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
    if (event.kind == awj::BatchEventKind::item_started) { started = true; canceled.request_stop(); }
  }, canceled.get_token());
  if (!canceled_batch || !started || !canceled_batch->canceled || !fs::exists(source))
    return fail("Cancellation at item start recycled its source.");

  cfg.output_dir = root / "queue-late-collision";
  bool seeded = false;
  const auto collision = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
    if (event.kind == awj::BatchEventKind::item_started) {
      fs::create_directories(event.result.output_path.parent_path(), ec);
      std::ofstream sentinel{event.result.output_path, std::ios::binary};
      sentinel << "new user output";
      seeded = static_cast<bool>(sentinel);
    }
  });
  if (!collision || !seeded || collision->ok_count != 0 || !fs::exists(source) ||
      read_text(cfg.output_dir / "queue-contract.png") != "new user output")
    return fail("Late output collision overwrote the new path or recycled the source.");

  cfg.collision_mode = awj::CollisionMode::skip;
  const auto skipped = awj::run_batch(cfg);
  if (!skipped || skipped->ok_count != 1 || !fs::exists(source))
    return fail("Skipped existing output recycled its source.");
  const auto invalid = input_dir / "invalid-empty.webp";
  { std::ofstream empty{invalid, std::ios::binary}; }
  cfg.input_path = invalid;
  cfg.output_dir = root / "queue-failure";
  const auto failed = awj::run_batch(cfg);
  if (!failed || failed->ok_count || failed->failed_count != 1 || !fs::exists(invalid))
    return fail("Failed input recycled its source.");
  return 0;
}

#ifdef _WIN32
bool restore_generated_recycle_item(const std::wstring& recycled,
    const std::filesystem::path& original) {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  struct ComScope { HRESULT value; ~ComScope() { if (SUCCEEDED(value)) CoUninitialize(); } } com{initialized};
  if (FAILED(initialized) || recycled.empty() || std::filesystem::exists(original)) return false;
  Microsoft::WRL::ComPtr<IFileOperation> operation;
  Microsoft::WRL::ComPtr<IShellItem> item, parent;
  auto directory = original.parent_path();
  directory.make_preferred();
  const auto filename = original.filename().native();
  HRESULT hr = CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
  if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(
      FOF_NOERRORUI | FOFX_EARLYFAILURE | FOF_SILENT | FOF_NO_CONNECTED_ELEMENTS | FOFX_NOCOPYHOOKS);
  if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(recycled.c_str(), nullptr, IID_PPV_ARGS(&item));
  if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&parent));
  if (SUCCEEDED(hr)) hr = operation->MoveItem(item.Get(), parent.Get(), filename.c_str(), nullptr);
  if (SUCCEEDED(hr)) hr = operation->PerformOperations();
  BOOL aborted = TRUE;
  if (SUCCEEDED(hr)) hr = operation->GetAnyOperationsAborted(&aborted);
  return SUCCEEDED(hr) && !aborted && std::filesystem::exists(original);
}

bool cleanup_generated_recycle_metadata(const std::filesystem::path& recycled,
    const std::filesystem::path& original, std::uint64_t original_bytes) {
  namespace fs = std::filesystem;
  auto info_name = recycled.filename().native();
  if (!info_name.starts_with(L"$R")) return false;
  std::error_code ec;
  if (fs::exists(recycled, ec) || ec) return false;
  info_name[1] = L'I';
  const auto metadata = recycled.parent_path() / info_name;
  if (!fs::exists(metadata, ec)) return !ec;
  // Parsing a returned $R path can restore the data as a filesystem item while
  // leaving $I bookkeeping. Remove only that exact orphan, after the restored
  // source hash is verified and the bounded record names this generated input.
  const auto identity = awj::pipeline_detail::recycle_file_identity(metadata);
  if (!identity || identity->links != 1 || identity->bytes < 24 || identity->bytes > 65564) return false;
  std::string record(static_cast<std::size_t>(identity->bytes), '\0');
  {
    std::ifstream stream{metadata, std::ios::binary};
    stream.read(record.data(), static_cast<std::streamsize>(record.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof()) return false;
  }
  std::uint64_t version{}, bytes{};
  std::memcpy(&version, record.data(), sizeof(version));
  std::memcpy(&bytes, record.data() + 8, sizeof(bytes));
  std::size_t offset = 24;
  std::uint32_t chars = 260;
  if (version == 2) {
    if (record.size() < 28) return false;
    std::memcpy(&chars, record.data() + 24, sizeof(chars));
    offset = 28;
  } else if (version != 1) return false;
  if (!chars || chars > 32768 || bytes != original_bytes ||
      record.size() != offset + chars * sizeof(wchar_t)) return false;
  std::wstring recorded_path(chars, L'\0');
  std::memcpy(recorded_path.data(), record.data() + offset, chars * sizeof(wchar_t));
  const auto terminator = recorded_path.find(L'\0');
  if (terminator == std::wstring::npos) return false;
  recorded_path.resize(terminator);
  if (awj::normalized_lower_path_key(fs::path{recorded_path}) != awj::normalized_lower_path_key(original)) return false;
  const auto unchanged = awj::pipeline_detail::recycle_file_identity(metadata);
  if (!unchanged || *unchanged != *identity) return false;
  return fs::remove(metadata, ec) && !ec;
}
#endif

int queue_recycle_system_contract(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  const auto unique = std::to_string(awj::current_process_id()) + "-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto inputs = root / "system-recycle-input";
  const auto source = inputs / ("awj-queue-recycle-" + unique + ".webp");
  const auto failed_input = inputs / ("z-failed-" + unique + ".webp");
  std::error_code ec;
  fs::create_directories(inputs, ec);
  if (ec) return fail("Could not create uniquely owned system-recycle fixture.");
  if (auto written = write_webp(source); !written) return fail(written.error());
  { std::ofstream invalid{failed_input, std::ios::binary}; }
  const auto original_hash = awj::file_sha256_hex(source);
  if (!original_hash) return fail(original_hash.error());
#ifndef _WIN32
  // A fresh test process isolates GIO's home trash to this owned build/results
  // directory. Never enumerate or empty the user's real desktop trash.
  const char* previous = std::getenv("XDG_DATA_HOME");
  const std::optional<std::string> old_xdg = previous ? std::optional<std::string>{previous} : std::nullopt;
  struct RestoreEnvironment {
    const std::optional<std::string>& old;
    ~RestoreEnvironment() { if (old) ::setenv("XDG_DATA_HOME", old->c_str(), 1); else ::unsetenv("XDG_DATA_HOME"); }
  } restore_environment{old_xdg};
  const auto xdg = root / "isolated-desktop-data";
  fs::create_directories(xdg, ec);
  if (ec || ::setenv("XDG_DATA_HOME", xdg.c_str(), 1) != 0)
    return fail("Could not isolate the test desktop trash.");
#endif
  auto cfg = awj::default_app_config();
  cfg.input_path = inputs;
  cfg.output_dir = root / "system-recycle-output";
  cfg.output_format = awj::OutputFormat::png;
  cfg.quality = 100;
  cfg.max_jobs = 1;
  // Share one output group so its scan order is serialized. Separate groups
  // are scheduled by memory admission, not filename/weight order: the invalid
  // input's whole-budget reservation may otherwise run before the valid one.
  cfg.output_template = L"serial";
  cfg.collision_mode = awj::CollisionMode::overwrite;
  cfg.delete_source = true;
  cfg.write_log = true;
  awj::EncodeResult successful;
  bool immediate = false, next_item_started = false, before_next_item = false;
  const auto converted = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
    if (event.kind == awj::BatchEventKind::item_finished && event.result.input_path == source) {
      successful = event.result;
      immediate = !fs::exists(source);
    }
    if (event.kind == awj::BatchEventKind::item_started && event.result.input_path == failed_input) {
      next_item_started = true;
      before_next_item = !fs::exists(source);
    }
  });
  const bool queue_valid = converted && converted->ok_count == 1 && converted->failed_count == 1 &&
      successful.ok && !successful.skipped && fs::exists(failed_input) && fs::exists(successful.output_path);
  const bool source_recycled = !fs::exists(source);
  const bool snapshot_valid = successful.input_sha256_snapshot &&
      *successful.input_sha256_snapshot == *original_hash;
  const bool csv_valid = read_text(cfg.output_dir / "log" / "summery.csv").find(*original_hash) != std::string::npos;
  std::fprintf(stdout,
      "Recycle observations: immediate=%d, next_item_started=%d, before_next_item=%d, source_sha=%d, csv_sha=%d; message=%s\n",
      immediate, next_item_started, before_next_item, snapshot_valid, csv_valid, successful.message.c_str());
  // Restore the exact generated entry before reporting any queue/timing/SHA
  // failure. A failed assertion must not strand a file in the system trash.
  if (source_recycled) {
#ifdef _WIN32
    // Use only the exact item returned by PostDeleteItem; never enumerate or
    // empty the Recycle Bin. Check its corresponding bookkeeping separately.
    if (!restore_generated_recycle_item(successful.recycled_source_shell_item, source))
      return fail("UNRESTORED generated Shell recycle item; original=" + awj::path_to_utf8(source) +
          "; item=" + awj::utf8_from_wide(successful.recycled_source_shell_item));
#else
    const auto recycled = xdg / "Trash" / "files" / source.filename();
    const auto metadata = xdg / "Trash" / "info" / (source.filename().native() + ".trashinfo");
    const auto trashed_hash = awj::file_sha256_hex(recycled);
    if (!trashed_hash || *trashed_hash != *original_hash ||
        !read_text(metadata).starts_with("[Trash Info]\n"))
      return fail("UNRESTORED generated GIO entry failed recovery validation: " + awj::path_to_utf8(recycled));
    fs::rename(recycled, source, ec);
    if (ec || !fs::remove(metadata, ec) || ec)
      return fail("Could not restore/clean the generated isolated trash entry: " + awj::path_to_utf8(metadata));
#endif
    const auto restored_hash = awj::file_sha256_hex(source);
    if (!restored_hash || *restored_hash != *original_hash)
      return fail("Restoring the generated recycle item changed its content.");
#ifdef _WIN32
    if (!cleanup_generated_recycle_metadata(successful.recycled_source_path, source, successful.original_bytes))
      return fail("Generated Shell entry bookkeeping could not be safely cleaned: " +
          awj::path_to_utf8(successful.recycled_source_path));
#endif
    std::fputs("Generated recycle entry restored and cleaned before assertion reporting.\n", stdout);
  }
  if (!queue_valid)
    return fail(converted ? "System-recycle queue did not retain its failed input or commit the successful output." : converted.error());
  if (!source_recycled) {
    const auto retained_hash = awj::file_sha256_hex(source);
    if (!retained_hash || *retained_hash != *original_hash || successful.message.find("回收") == std::string::npos)
      return fail("Unavailable recycling did not preserve the source and produce a warning.");
    std::fprintf(stdout, "System recycle success NOT COVERED; source safely retained: %s\n", successful.message.c_str());
    return 77;
  }
  if (!immediate) return fail("Source was not recycled before its item_finished event.");
  if (!next_item_started) return fail("The serialized follow-up item did not emit item_started.");
  if (!before_next_item) return fail("Source was not recycled before the serialized next item started.");
  if (!snapshot_valid) return fail("The pre-recycle source SHA-256 snapshot is missing or incorrect.");
  if (!csv_valid) return fail("The CSV lost the pre-recycle source SHA-256.");
  std::fputs("Real system recycling verified before the next item; source SHA-256 and CSV verified.\n", stdout);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  {
    constexpr std::uint64_t mib = 1024ull * 1024;
    constexpr std::uint64_t n = 2048ull * 1536;
    auto dimensions = awj::make_image_dimensions(2048, 1536);
    dimensions.opaque_sdr8_yuv420 = true;
    dimensions.source_bit_depth = 8;
    dimensions.source_dynamic_range = awj::ImageDynamicRangeHint::sdr;
    dimensions.decoded_storage_bytes = n * 3 / 2;
    auto defaults = awj::default_app_config();
    defaults.max_jobs = 1;
    const auto baseline = awj::estimated_regular_memory(dimensions, defaults);
    if (baseline.profile != "v2-avif-planes" || baseline.target_chroma != awj::ChromaMode::yuv420 ||
        baseline.bytes >= awj::avif_encode_working_set_bytes_for_dimensions(dimensions))
      return fail("Confirmed ordinary SDR420 did not use its own memory profile");
    for (int mode = 0; mode < 10; ++mode) {
      auto cfg = defaults;
      switch (mode) {
        case 0: cfg.quality = 100; break;
        case 1: cfg.visual_quality = 90; break;
        case 2: cfg.alpha_policy = awj::AlphaModePolicy::force; break;
        case 3: cfg.transfer_characteristics = 16; break;
        case 4: cfg.color_primaries = 9; break;
        case 5: cfg.matrix_coefficients = 9; break;
        case 6: cfg.color_range = 1; break;
        case 7: cfg.bit_depth = 12; break;
        case 8: cfg.chroma_mode = awj::ChromaMode::yuv444; break;
        case 9: cfg.avif_color_representation = awj::AvifColorRepresentation::rgb_identity; break;
      }
      const auto guarded = awj::estimated_regular_memory(dimensions, cfg);
      if (guarded.bytes < baseline.bytes || (mode == 2 && !guarded.target_alpha) ||
          (mode == 7 && guarded.target_bit_depth != 12) ||
          ((mode == 8 || mode == 9) && guarded.target_chroma != awj::ChromaMode::yuv444))
        return fail("HDR, alpha or overridden output used an ordinary SDR memory profile");
    }
    auto unknown = dimensions;
    unknown.source_bit_depth = 0;
    unknown.source_dynamic_range = awj::ImageDynamicRangeHint::unknown;
    unknown.decoded_storage_bytes = 0;
    const auto fallback = awj::estimated_regular_memory(unknown, defaults);
    if (fallback.source_resident_bytes < n * 16 || !fallback.target_alpha || fallback.bytes < baseline.bytes)
      return fail("An old native420 flag or missing hints silently treated unknown input as SDR");

    auto rgb = dimensions;
    rgb.opaque_sdr8_yuv420 = false;
    rgb.decoded_storage_bytes = n * 4;
    const auto ordinary = awj::estimated_regular_memory(rgb, defaults);
    if (ordinary.profile != "v2-avif-planes" || ordinary.bytes >=
        awj::avif_encode_working_set_bytes_for_dimensions(rgb))
      return fail("Known SDR packed input still paid the complete unknown/HDR AVIF cost");
    auto thumbnail_cfg = defaults;
    thumbnail_cfg.image_size_limit.mode = awj::ImageSizeLimitMode::manual;
    thumbnail_cfg.image_size_limit.max_long_edge = 512;
    const auto thumbnail = awj::estimated_regular_memory(rgb, thumbnail_cfg);
    if (thumbnail.target_width != 512 || thumbnail.target_height != 384 ||
        thumbnail.source_width != 2048 || thumbnail.source_height != 1536 ||
        thumbnail.source_resident_bytes != ordinary.source_resident_bytes ||
        thumbnail.decode_bytes != ordinary.decode_bytes ||
        thumbnail.encode_bytes >= ordinary.encode_bytes ||
        thumbnail.bytes != std::max({thumbnail.decode_bytes, thumbnail.prepare_bytes,
            thumbnail.encode_bytes, thumbnail.metric_bytes, thumbnail.output_bytes}))
      return fail("Thumbnail planning dropped source residency or used source-sized encoding/summed phases");

    auto gain = dimensions; // An accidental native420 flag must not beat Gain Map semantics.
    gain.source_has_gain_map = true;
    gain.source_bit_depth = 10;
    gain.source_dynamic_range = awj::ImageDynamicRangeHint::hdr;
    gain.decoded_storage_bytes = n * 8;
    gain.gain_map_width = 512;
    gain.gain_map_height = 384;
    const auto dual = awj::estimated_regular_memory(gain, defaults);
    auto missing_map_geometry = gain;
    missing_map_geometry.gain_map_width = missing_map_geometry.gain_map_height = 0;
    const auto full_map = awj::estimated_regular_memory(missing_map_geometry, defaults);
    if (dual.profile != "v2-avif-gain-map" || dual.source_resident_bytes < n * 24 ||
        dual.prepare_bytes < dual.source_resident_bytes + n * 12 ||
        full_map.bytes < dual.bytes || full_map.decode_bytes <= dual.decode_bytes ||
        full_map.source_resident_bytes != dual.source_resident_bytes)
      return fail("Gain Map planning lost an endpoint, decode-only auxiliary lifetime or conservative geometry");

    auto jpeg = defaults;
    jpeg.output_format = awj::OutputFormat::jpgli;
    const auto sdr_jpeg = awj::estimated_regular_memory(rgb, jpeg);
    const auto unknown_jpeg = awj::estimated_regular_memory(unknown, jpeg);
    if (sdr_jpeg.profile != "v2-jpeg-sdr" || sdr_jpeg.bytes >= unknown_jpeg.bytes)
      return fail("Ordinary SDR JPEGli still reserved the complete HDR pipeline");
    jpeg.jpeg_hdr = awj::JpegHdrMode::hdr;
    const auto forced_sdr = awj::estimated_regular_memory(rgb, jpeg);
    if (forced_sdr.profile != "v2-jpeg-sdr" || forced_sdr.bytes != sdr_jpeg.bytes ||
        awj::estimated_regular_memory(unknown, jpeg).profile != "v2-jpeg-hdr" ||
        awj::estimated_regular_memory(gain, jpeg).profile != "v2-jpeg-hdr")
      return fail("JPG hdr reserved HDR for confirmed SDR or under-reserved real/unknown HDR");
    auto converted_hdr = jpeg;
    converted_hdr.transfer_characteristics = 16;
    if (awj::estimated_regular_memory(rgb, converted_hdr).profile != "v2-jpeg-hdr")
      return fail("Explicit SDR-to-PQ conversion lost its HDR JPEG working-set budget");
    jpeg.jpeg_hdr = awj::JpegHdrMode::sdr;
    if (awj::estimated_regular_memory(gain, jpeg).profile != "v2-jpeg-source-sdr-presentation")
      return fail("Forced SDR Gain Map output did not plan the saved source SDR presentation");

    auto png = defaults;
    png.output_format = awj::OutputFormat::png;
    const auto rgba8 = awj::estimated_regular_memory(rgb, png);
    auto high_depth = rgb;
    high_depth.source_bit_depth = 16;
    high_depth.source_has_alpha = true;
    high_depth.decoded_storage_bytes = n * 8;
    const auto rgba16 = awj::estimated_regular_memory(high_depth, png);
    auto explicit_png16 = png;
    explicit_png16.bit_depth = 16;
    const auto requested16 = awj::estimated_regular_memory(high_depth, explicit_png16);
    if (rgba8.profile != "v2-png-rgba8" || rgba8.bytes >= 128 * mib ||
        rgba16.profile != "v2-png-rgba8" || rgba16.bytes <= rgba8.bytes || rgba16.bytes < 73 * mib ||
        requested16.profile != "v2-png-rgba16" || requested16.encode_bytes <= rgba16.encode_bytes)
      return fail("PNG planning ignored SDR auto8, retained 16-bit source storage or explicit16 output");
    const auto hdr_single = awj::estimated_regular_memory(gain, png);
    if (hdr_single.source_resident_bytes >= dual.source_resident_bytes ||
        hdr_single.source_resident_bytes < n * 16)
      return fail("HDR-only PNG retained an unused alternate endpoint or dropped its selected HDR storage");

    auto alpha = rgb;
    alpha.source_has_alpha = true;
    auto webp = defaults;
    webp.output_format = awj::OutputFormat::webp;
    auto jxl = defaults;
    jxl.output_format = awj::OutputFormat::jxl;
    if (awj::estimated_regular_memory(alpha, webp).bytes < 115 * mib ||
        awj::estimated_regular_memory(rgb, jxl).bytes < 188 * mib ||
        awj::estimated_regular_memory(alpha, jxl).bytes < 261 * mib ||
        awj::estimated_regular_memory(gain, jxl).profile != "v2-jxl-gain-map")
      return fail("WebP/JXL planning is below the recorded 3MP workload or lost dual presentation");
    auto threaded = jxl;
    threaded.max_jobs = 8;
    if (awj::estimated_regular_memory(alpha, threaded).bytes <=
        awj::estimated_regular_memory(alpha, jxl).bytes)
      return fail("Codec thread headroom did not participate in memory admission");
    auto metrics = defaults;
    metrics.visual_quality = 90;
    const auto measured_target = awj::estimated_regular_memory(rgb, metrics);
    if (!measured_target.metric_bytes || measured_target.encode_bytes <= ordinary.encode_bytes)
      return fail("Cached metric reference/best-candidate storage was omitted from candidate encoding");
    auto metric_thumbnail_cfg = metrics;
    metric_thumbnail_cfg.image_size_limit = thumbnail_cfg.image_size_limit;
    const auto metric_thumbnail = awj::estimated_regular_memory(rgb, metric_thumbnail_cfg);
    auto padded_source = rgb;
    padded_source.decoded_storage_bytes = n * 8;
    const auto padded_thumbnail = awj::estimated_regular_memory(padded_source, metric_thumbnail_cfg);
    if (metric_thumbnail.metric_bytes >= measured_target.metric_bytes ||
        padded_thumbnail.decode_bytes <= metric_thumbnail.decode_bytes ||
        padded_thumbnail.encode_bytes != metric_thumbnail.encode_bytes)
      return fail("Physical metric resize retained already released source padding or source-sized metric workspaces");
    auto icc_dimensions = rgb;
    icc_dimensions.source_icc_bytes = 660;
    const auto ordinary_icc_hint = awj::estimated_regular_memory(icc_dimensions, defaults);
    if (ordinary_icc_hint.decode_bytes != ordinary.decode_bytes ||
        ordinary_icc_hint.prepare_bytes != ordinary.prepare_bytes)
      return fail("非 Gain Map 输入错误地支付了新增 ICC 校验暂存。");
    icc_dimensions.source_has_gain_map = true;
    const auto icc_estimate = awj::estimated_regular_memory(icc_dimensions, defaults);
    icc_dimensions.source_icc_bytes = 0;
    const auto no_icc_estimate = awj::estimated_regular_memory(icc_dimensions, defaults);
    if (icc_estimate.decode_bytes != no_icc_estimate.decode_bytes + 660 ||
        icc_estimate.prepare_bytes != no_icc_estimate.prepare_bytes + 660 ||
        icc_estimate.encode_bytes != no_icc_estimate.encode_bytes)
      return fail("Gain Map ICC 校验没有按实际单 profile 字节计入对应阶段。");
    icc_dimensions.source_icc_bytes = UINT64_MAX;
    if (awj::estimated_regular_memory(icc_dimensions, defaults).bytes != UINT64_MAX)
      return fail("ICC profile 字节计费溢出后错误准入。");
    auto overflow = awj::make_image_dimensions(UINT32_MAX, UINT32_MAX);
    if (awj::estimated_regular_memory(overflow, defaults).bytes != UINT64_MAX ||
        awj::estimated_regular_memory(rgb, png, UINT64_MAX).bytes != UINT64_MAX ||
        awj::estimated_regular_memory({}, defaults).bytes != UINT64_MAX)
      return fail("Image working-set dimensions/input arithmetic overflowed or admitted empty geometry");
  }
  const auto root = std::filesystem::path{AWJ_TEST_RESULTS_ROOT} /
      ("pipeline-security-" + std::to_string(awj::current_process_id()));
  std::error_code ec;
  if (!std::filesystem::create_directories(root, ec) || ec) {
    return fail("Dedicated pipeline test directory exists or cannot be created.");
  }
  if (argc == 2 && std::string_view{argv[1]} == "--queue-recycle-only") {
    const auto checked = queue_recycle_system_contract(root);
    if (checked == 0 || checked == 77) {
      std::filesystem::remove_all(root, ec);
      if (ec) return fail("Could not clean generated system-recycle test files.");
    }
    return checked;
  }
  const bool stage_timing_only = argc == 2 && std::string_view{argv[1]} == "--stage-timing-only";
  if (argc == 1 || stage_timing_only) {
    if (const auto checked = animation_stage_timing_contract()) return checked;
    if (const auto checked = resize_csv_contract(root)) return checked;
    if (stage_timing_only) {
      std::filesystem::remove_all(root, ec);
      return ec ? fail("Could not clean dedicated stage timing test files.") : 0;
    }
  }
  if (!(argc == 2 && std::string_view{argv[1]} == "--memory-policy-only")) {
    if (const auto checked = queue_core_contract(root)) return checked;
  }
  if (argc == 2 && std::string_view{argv[1]} == "--queue-contract-only") {
    std::filesystem::remove_all(root, ec);
    return ec ? fail("Could not clean dedicated queue test files.") : 0;
  }
  {
    const auto input = root / "memory-unlock.webp";
    if (auto written = write_webp(input); !written) return fail(written.error());
    const awj::encoding_defaults::MemoryPolicy outer_policy{1, false, false};
    const awj::encoding_defaults::ScopedMemoryPolicy outer_scope{outer_policy};
    for (const auto configured : {0ull, 1ull, 64ull * 1024 * 1024}) {
      auto cfg = awj::default_app_config();
      cfg.input_path = input;
      cfg.output_dir = root / std::format("memory-unlock-{}", configured);
      cfg.output_format = awj::OutputFormat::png;
      cfg.quality = 100;
      cfg.max_jobs = 1;
      cfg.memory_limit_bytes = configured;
      cfg.unlock_memory_limit = true;
      cfg.write_summary = true;
      bool traced = false;
      auto summary = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
        if (event.kind == awj::BatchEventKind::item_finished) {
          const auto& result = event.result;
          traced = awj::encoding_defaults::effective_max_input_file_bytes() == UINT64_MAX &&
              awj::encoding_defaults::memory_limits_unlocked() &&
              result.ok && result.memory_budget_bytes == 0 &&
              result.memory_budget_mode == "unlocked" && result.memory_budget_source == "session_unlock" &&
              result.estimated_working_set_bytes > 0 && !result.memory_estimate_profile.empty() &&
              result.memory_source_width == 2 && result.memory_source_height == 2 &&
              result.memory_target_width == 2 && result.memory_target_height == 2;
        }
      });
      const auto csv = read_text(cfg.output_dir / "log" / "summery.csv");
      if (!summary || summary->ok_count != 1 || !traced ||
          awj::encoding_defaults::current_memory_policy() != outer_policy ||
          csv.find("memory_budget_bytes,memory_budget_mode,memory_budget_source,estimated_working_set_bytes") == std::string::npos ||
          csv.find(",0,unlocked,session_unlock,") == std::string::npos)
        return fail("Successful session-unlocked work lost zero budget, mode/source or estimate CSV diagnostics.");
      cfg.output_format = awj::OutputFormat::avif;
      cfg.studio_large_action = L"grid";
      cfg.output_dir = root / std::format("manual-memory-unlock-{}", configured);
      std::stop_source cancel;
      cancel.request_stop();
      bool manual_traced = false;
      auto manual = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
        if (event.kind == awj::BatchEventKind::item_finished) {
          const auto& result = event.result;
          manual_traced = result.canceled && result.memory_budget_bytes == 0 &&
              result.memory_budget_mode == "unlocked" && result.memory_budget_source == "session_unlock" &&
              result.estimated_working_set_bytes > 0 && result.encoder_threads == 0 &&
              result.seconds == 0 && result.decode_seconds == 0 && result.encode_seconds == 0 &&
              result.resize_seconds == -1.0 && result.resize_workers == 0;
        }
      }, cancel.get_token());
      if (!manual || !manual->canceled || !manual_traced ||
          awj::encoding_defaults::current_memory_policy() != outer_policy ||
          read_text(cfg.output_dir / "log" / "summery.csv").find(",0,unlocked,session_unlock,") == std::string::npos)
        return fail("Manual Grid worker reinterpreted unlocked zero as automatic or fabricated unstarted measurements.");
    }
    auto cfg = awj::default_app_config();
    cfg.output_format = awj::OutputFormat::png;
    cfg.quality = 100;
    cfg.memory_limit_bytes = 1;
    cfg.unlock_memory_limit = true;
    cfg.unlock_max_input_file_bytes = false;
    awj::FileLogger logger{root, false};
    bool reserved = false;
    const auto reject_reservation = [&](std::uint64_t) { reserved = true; return false; };
    const awj::ResourcePlan stale{.memory_limit_bytes = 1};
    awj::NativeBackend direct{cfg, logger, stale, reject_reservation};
    auto encoded = direct.encode_memory(awj::ImageDecodeResult{.image = make_test_image()});
    if (!encoded || encoded->encoded.bytes.empty() || encoded->diagnostics.memory_budget_bytes != 0 || reserved ||
        awj::encoding_defaults::current_memory_policy() != outer_policy || cfg.memory_limit_bytes != 1)
      return fail(encoded ? "Direct backend retained a stale budget/callback or leaked its scope." : encoded.error());
    std::stop_source stopped;
    stopped.request_stop();
    if (direct.encode_memory(awj::ImageDecodeResult{.image = make_test_image()}, stopped.get_token()))
      return fail("Direct backend full unlock disabled cancellation.");
    for (const bool legacy_input_only : {false, true}) {
      auto limited_cfg = cfg;
      limited_cfg.unlock_memory_limit = false;
      limited_cfg.unlock_max_input_file_bytes = legacy_input_only;
      awj::NativeBackend limited{limited_cfg, logger, stale};
      if (limited.encode_memory(awj::ImageDecodeResult{.image = make_test_image()}))
        return fail("Direct finite/input-only backend discarded its working-set budget.");
      // A zero ResourcePlan is not permission to discard an explicit manual
      // budget. Carry a finite input snapshot so this check needs no OS query.
      limited_cfg.runtime_memory_policy = awj::encoding_defaults::MemoryPolicy{
          16u * 1024 * 1024, legacy_input_only, false};
      awj::NativeBackend unresolved_manual{limited_cfg, logger, awj::ResourcePlan{}};
      if (unresolved_manual.encode_memory(awj::ImageDecodeResult{.image = make_test_image()}))
        return fail("An unresolved direct-backend plan silently discarded a manual working-set budget.");
    }
    constexpr auto declared_size = 513ull * 1024 * 1024 * 1024;
    const std::vector<awj::ImageFile> records{{.index = 0, .path = input, .bytes = declared_size}};
    const auto manifest_path = root / "unlocked-numeric-size.awjq";
    if (awj::WebPImageDecoder{}.probe_dimensions(input) ||
        awj::probe_image_dimensions_for_path(input, {.allow_wic_fallback = false}) ||
        awj::check_scanned_input_file_size(input, declared_size))
      return fail("Finite probe/scan ignored the submitting operation's input cap.");
    {
      const awj::encoding_defaults::ScopedMemoryPolicy full{
          awj::encoding_defaults::MemoryPolicy{UINT64_MAX, true, true}};
      if (!awj::WebPImageDecoder{}.probe_dimensions(input) ||
          !awj::probe_image_dimensions_for_path(input, {.allow_wic_fallback = false}) ||
          !awj::check_scanned_input_file_size(input, declared_size))
        return fail("Unlocked probe/scan retained an ambient or legacy input cap.");
      if (awj::probe_image_dimensions_for_path(input,
          {.allow_wic_fallback = false, .stop_token = stopped.get_token()}))
        return fail("Unlocked registry probe ignored explicit cancellation.");
      if (auto written = awj::write_studio_queue_manifest(manifest_path, records); !written) return fail(written.error());
      const auto read = awj::read_studio_queue_manifest(manifest_path);
      if (!read || read->files.size() != 1 || read->files.front().bytes != declared_size ||
          std::filesystem::file_size(manifest_path) >= 4096)
        return fail("A tiny manifest could not preserve an unlocked numeric size above 512 GiB.");
    }
    if (awj::read_studio_queue_manifest(manifest_path) ||
        awj::write_studio_queue_manifest(root / "finite-numeric-size.awjq", records))
      return fail("Finite manifest I/O ignored its restored input policy.");
  }
  {
    // 约 16 MiB 的长条图触发真实 AVIF Grid，无需耗尽物理内存。
    auto wide = make_test_image();
    wide.width = 65537; wide.height = 64;
    wide.planes[0].stride = wide.width * 4;
    wide.planes[0].bytes.assign(wide.planes[0].stride * wide.height, std::byte{255});
    auto png = awj::PngImageEncoder{}.encode(wide, {.quality = 100});
    if (!png) return fail(png.error());
    const auto input = root / "memory-unlocked-grid.png";
    {
      std::ofstream stream{input, std::ios::binary};
      stream.write(reinterpret_cast<const char*>(png->encoded.bytes.data()), png->encoded.bytes.size());
      if (!stream) return fail("Could not write the bounded unlocked Grid fixture.");
    }
    const awj::encoding_defaults::MemoryPolicy outer_policy{1, false, false};
    const awj::encoding_defaults::ScopedMemoryPolicy outer_scope{outer_policy};
    for (const bool manual_worker : {false, true}) {
      auto cfg = awj::default_app_config();
      cfg.input_path = input;
      cfg.output_dir = root / (manual_worker ? "unlocked-manual-grid" : "unlocked-auto-grid");
      cfg.output_format = awj::OutputFormat::avif;
      cfg.image_size_limit.mode = awj::ImageSizeLimitMode::none;
      cfg.chroma_mode = awj::ChromaMode::yuv444;
      cfg.quality = 50; cfg.speed = 10; cfg.max_jobs = 2;
      cfg.memory_limit_bytes = 1; cfg.unlock_memory_limit = true;
      cfg.unlock_max_input_file_bytes = false;
      cfg.studio_large_action = manual_worker ? L"grid" : L"";
      cfg.write_summary = true;
      bool traced = false;
      const auto summary = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
        if (event.kind == awj::BatchEventKind::item_finished)
          traced = event.result.ok && event.result.memory_budget_bytes == 0 &&
              event.result.memory_budget_mode == "unlocked" &&
              event.result.memory_budget_source == "session_unlock" &&
              event.result.memory_estimate_profile.starts_with("v2-avif-grid");
      });
      if (!summary || summary->ok_count != 1 || !traced ||
          awj::encoding_defaults::current_memory_policy() != outer_policy ||
          read_text(cfg.output_dir / "log" / "summery.csv").find(",0,unlocked,session_unlock,") == std::string::npos)
        return fail(summary ? "Actual automatic/manual Grid retained a budget or leaked its policy." : summary.error());
      const awj::encoding_defaults::ScopedMemoryPolicy decode_scope{
          awj::encoding_defaults::MemoryPolicy{UINT64_MAX, true, true}};
      const auto decoded = awj::make_avif_image_decoder(1)->decode(cfg.output_dir / "memory-unlocked-grid.avif");
      if (!decoded || decoded->image.width != wide.width || decoded->image.height != wide.height)
        return fail(decoded ? "Actual unlocked Grid changed original dimensions." : decoded.error());
    }
  }
  if (argc == 2 && std::string_view{argv[1]} == "--memory-policy-only") {
    std::filesystem::remove_all(root, ec);
    return ec ? fail("Could not clean dedicated memory-policy test files.") : 0;
  }
  {
    const auto source = make_test_image();
    const awj::ResourcePlan resources{.file_parallelism = 1,
                                      .encoder_threads_per_file = 1,
                                      .global_thread_budget = 1};
    auto encoder = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
    auto decoder = awj::make_avif_image_decoder(1);
    if (!encoder || !decoder) return fail("AVIF depth fixture codecs are unavailable");
    awj::FileLogger logger{root, false};
    for (const int depth : {8, 10, 12}) {
      const auto context = std::format("{}-bit AVIF to auto PNG", depth);
      auto encoded = encoder->encode(source, awj::NativeEncodeSettings{
          .output_format = awj::OutputFormat::avif,
          .quality = 80,
          .speed = 10,
          .bit_depth = depth,
          .bit_depth_explicit = true,
          .chroma_mode = awj::ChromaMode::yuv420,
          .avif_encoder = awj::AvifEncoderMode::aom,
          .resources = resources});
      if (!encoded) return fail(context + ": " + encoded.error());
      const auto input = root / std::format("backend-png-depth-{}.avif", depth);
      {
        std::ofstream stream{input, std::ios::binary};
        stream.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                     static_cast<std::streamsize>(encoded->encoded.bytes.size()));
        if (!stream) return fail(context + ": could not write AVIF fixture");
      }
      auto fixture = decoder->decode(input, awj::DecodeOptions{.native_pixels = true});
      if (!fixture) return fail(context + ": " + fixture.error());
      if (fixture->image.width != source.width || fixture->image.height != source.height ||
          fixture->image.bit_depth != depth || !fixture->image.source_info ||
          fixture->image.source_info->bit_depth != depth)
        return fail(context + ": AVIF fixture did not retain its explicit depth");

      auto cfg = awj::default_app_config();
      cfg.input_path = input;
      cfg.output_dir = root / std::format("backend-png-depth-{}-out", depth);
      cfg.output_format = awj::OutputFormat::png;
      cfg.quality = 100;
      cfg.max_jobs = 1;
      // 保持 PNG 自动位深；这里强制 16 会掩盖后端位深回归。
      cfg.bit_depth.reset();
      std::filesystem::create_directories(cfg.output_dir, ec);
      if (ec) return fail(context + ": could not create PNG output directory");
      awj::NativeBackend backend{cfg, logger, resources};
      const auto result = backend.encode(awj::ImageFile{
          .index = 0, .path = input,
          .bytes = static_cast<std::uint64_t>(encoded->encoded.bytes.size())});
      if (!result.ok || result.skipped || result.canceled)
        return fail(context + ": backend conversion failed: " + result.message);
      auto restored = awj::PngImageDecoder{}.decode(result.output_path);
      if (!restored) return fail(context + ": " + restored.error());
      const int storage_depth = 8; // 所选呈现为 SDR，PNG 自动位深固定为兼容的 8-bit。
      const auto png_bytes = read_text(result.output_path);
      if (restored->image.width != source.width || restored->image.height != source.height ||
          restored->image.bit_depth != storage_depth || png_bytes.size() < 25 ||
          static_cast<unsigned char>(png_bytes[24]) != storage_depth)
        return fail(context + ": PNG dimensions or IHDR storage depth changed");
    }
  }
  {
    auto cfg = awj::default_app_config();
    const std::vector<awj::ImageFile> unknown{{.path = root / "missing.jpg", .bytes = 7}};
    constexpr auto budget = 512ull * 1024 * 1024;
    for (auto format : {awj::OutputFormat::avif, awj::OutputFormat::png}) {
      cfg.output_format = format;
      auto work = awj::pipeline_detail::classify_work_for_avif(cfg, unknown, budget);
      if (!work || work->ordinary.size() != 1 || work->ordinary.front().estimated_bytes != budget)
        return fail("Unknown dimensions used compressed size instead of exclusive admission");
    }
  }
  {
    auto cfg = awj::default_app_config();
    cfg.output_format = awj::OutputFormat::png;
    const auto bytes = awj::estimated_regular_working_set_bytes(
        awj::make_image_dimensions(4096, 4096), cfg);
    const auto plan = awj::plan_resources({.automatic_thread_budget = 8, .file_count = 2,
        .memory_limit_bytes = 1024ull * 1024 * 1024, .estimated_bytes_per_file = bytes});
    if (bytes <= 512ull * 1024 * 1024 || plan.file_parallelism != 1 || plan.encoder_threads_per_file != 8)
      return fail("PNG planning omitted high-depth/output buffers or lost the remaining thread budget");
  }
  {
    auto cfg = awj::default_app_config();
    cfg.output_dir = root / "grid-memory-preflight";
    cfg.memory_limit_bytes = 64ull * 1024 * 1024;
    awj::FileLogger logger{root, false};
    awj::BatchLargeImageItem oversized{
        .file = {.path = root / "nonexistent-huge-input.png"},
        .dimensions = awj::make_image_dimensions(65537, 4096),
        .decision = {.available_grid = true}};
    const auto result = awj::pipeline_detail::encode_large_mode_item(cfg, logger, oversized,
        {.memory_limit_bytes = cfg.memory_limit_bytes}, {});
    if (result.ok || result.message.find("超过内存预算") == std::string::npos ||
        result.memory_budget_bytes != cfg.memory_limit_bytes || result.memory_budget_mode != "manual" ||
        result.memory_budget_source != "configuration" ||
        result.estimated_working_set_bytes <= result.memory_budget_bytes ||
        result.memory_source_width != 65537 || result.memory_source_height != 4096 ||
        result.encoder_threads != 0 || result.seconds != 0 || result.decode_seconds != 0 ||
        result.prepare_seconds != 0 || result.encode_seconds != 0 || result.write_seconds != 0 ||
        result.resize_seconds != -1.0 || result.resize_workers != 0)
      return fail("Grid preflight failed to record its actual budget/estimate or fabricated encoder work");
    if (auto csv = awj::write_csv(cfg.output_dir, std::span<const awj::EncodeResult>{&result, 1}); !csv)
      return fail(csv.error());
    if (read_text(cfg.output_dir / "log" / "summery.csv").find(",67108864,manual,configuration,") == std::string::npos)
      return fail("Budget rejection CSV silently emitted zero instead of the actual manual budget");
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
    auto rejected_cfg = cfg;
    rejected_cfg.output_dir = root / "auto-grid-memory-reject";
    rejected_cfg.memory_limit_bytes = 64ull * 1024 * 1024;
    bool rejected_before_start = false;
    int rejected_started = 0;
    auto rejected_summary = awj::run_batch(rejected_cfg, [&](const awj::BatchProgress& event) {
      if (event.kind == awj::BatchEventKind::item_started) ++rejected_started;
      if (event.kind == awj::BatchEventKind::item_finished) {
        const auto& result = event.result;
        rejected_before_start = !result.ok && result.memory_budget_bytes == rejected_cfg.memory_limit_bytes &&
            result.memory_budget_mode == "manual" && result.memory_budget_source == "configuration" &&
            result.estimated_working_set_bytes > result.memory_budget_bytes &&
            result.memory_estimate_profile.starts_with("v2-avif-grid") &&
            result.memory_source_width == 65537 && result.memory_source_height == 64 &&
            result.encoder_threads == 0 && result.decode_seconds == 0 && result.encode_seconds == 0 &&
            result.resize_seconds == -1.0 && result.resize_workers == 0;
      }
    });
    if (!rejected_summary || rejected_summary->failed_count != 1 || !rejected_before_start || rejected_started ||
        read_text(rejected_cfg.output_dir / "log" / "summery.csv").find(",67108864,manual,configuration,") == std::string::npos)
      return fail("Batch admission rejection lost its Grid estimate/budget or reported encoder work before start");
    bool successful_trace = false;
    auto summary = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
      if (event.kind == awj::BatchEventKind::item_finished)
        successful_trace = event.result.ok && event.result.memory_budget_bytes == cfg.memory_limit_bytes &&
            event.result.memory_budget_mode == "manual" && event.result.memory_budget_source == "configuration" &&
            event.result.memory_estimate_profile.starts_with("v2-avif-grid") &&
            event.result.estimated_working_set_bytes > 0;
    });
    if (!summary) return fail(summary.error());
    if (summary->ok_count != 1 || !successful_trace) return fail("automatic Grid encode failed or lost memory diagnostics: " + read_text(cfg.output_dir / "log" / "summery.csv"));
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
  invalid_batch_cfg.write_log = true;
  invalid_batch_cfg.write_summary = true;
  awj::EncodeResult invalid_decode_result;
  bool saw_invalid_decode = false;
  const auto invalid_batch_summary = awj::run_batch(invalid_batch_cfg, [&](const awj::BatchProgress& event) {
    if (event.kind == awj::BatchEventKind::item_finished &&
        event.result.input_path.filename() == "empty.webp") {
      invalid_decode_result = event.result;
      saw_invalid_decode = true;
    }
  });
  if (!invalid_batch_summary || invalid_batch_summary->ok_count != 1 ||
      invalid_batch_summary->failed_count != 1 ||
      !std::filesystem::exists(invalid_batch_output / "valid.avif")) {
    return fail(invalid_batch_summary
                    ? "one invalid input stopped the rest of the AVIF batch."
                    : invalid_batch_summary.error());
  }
  const auto invalid_batch_csv =
      read_text(invalid_batch_output / "log" / "summery.csv");
  if (invalid_batch_csv.find("empty.webp") == std::string::npos ||
      invalid_batch_csv.find("failed") == std::string::npos ||
      invalid_batch_csv.find("decode_stage,source_metadata_available,selected_rendition,rendition_origin") == std::string::npos ||
      invalid_batch_csv.find("heif_tile_parallelism,heif_codec_thread_budget,avif_layer_count,avif_layer_trace,") == std::string::npos ||
      invalid_batch_csv.find("memory_target_bit_depth,memory_target_chroma,memory_target_alpha,awj_version,resize_seconds,resize_workers\n") == std::string::npos) {
    return fail("invalid AVIF batch input was not recorded with its diagnostic columns.");
  }
  if (!saw_invalid_decode || invalid_decode_result.ok || !invalid_decode_result.processed ||
      invalid_decode_result.decode_stage.empty() || invalid_decode_result.source_metadata_available ||
      invalid_decode_result.seconds <= 0 || invalid_decode_result.decode_seconds < 0 ||
      invalid_decode_result.encoder_threads != 0 || invalid_decode_result.output_bytes != 0 ||
      invalid_decode_result.encode_seconds >= 0 || invalid_decode_result.resize_seconds != -1.0 ||
      invalid_decode_result.resize_workers != 0 || !valid_stage_sum(invalid_decode_result)) {
    return fail("解码失败丢失执行阶段/真实耗时，或冒填尚未开始的编码与源元数据。");
  }
  const auto invalid_batch_log = read_text(invalid_batch_output / "log" / "awj.log");
  const auto failure_position = invalid_batch_log.find("[FAIL]");
  if (failure_position == std::string::npos ||
      invalid_batch_log.find("[FAIL]", failure_position + 1) != std::string::npos ||
      invalid_batch_log.find("empty.webp", failure_position) == std::string::npos ||
      invalid_batch_log.find("decode_stage=" + invalid_decode_result.decode_stage, failure_position) == std::string::npos ||
      invalid_batch_log.find("source_icc=unknown", failure_position) == std::string::npos) {
    return fail("普通解码失败没有恰好写入一条带真实阶段的文件日志，或把未知 ICC 误记为不存在。");
  }

#ifdef _WIN32
  const auto manifest_input_dir = root / "manifest-input";
  const auto manifest_output_dir = root / "manifest-output";
#else
  const auto manifest_input_dir = root / (std::string{"中文 input\n"} + char(0xff));
  const auto manifest_output_dir = root / (std::string{"中文 output\n"} + char(0xfe));
#endif
  std::filesystem::create_directories(manifest_input_dir, ec);
  if (ec) {
    return fail("failed to create manifest input dir.");
  }
  std::vector<awj::ImageFile> runnable_manifest_files;
  for (std::size_t index = 0; index < 13; ++index) {
    auto input_name = std::format("manifest-input-{}.webp", index);
#ifndef _WIN32
    // Same displayed replacement character, distinct POSIX file names.
    if (index < 2) input_name = std::string{"image-"} + char(0xff - index) + ".webp";
#endif
    const auto input = manifest_input_dir / input_name;
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
  const auto native_roundtrip = awj::read_studio_queue_manifest(runnable_manifest_path);
  if (!native_roundtrip || native_roundtrip->files.size() != runnable_manifest_files.size())
    return fail("native manifest roundtrip failed");
  for (std::size_t i = 0; i < runnable_manifest_files.size(); ++i) {
    if (native_roundtrip->files[i].path != runnable_manifest_files[i].path ||
        native_roundtrip->files[i].resolved_output_path != runnable_manifest_files[i].resolved_output_path)
      return fail("native manifest path bytes changed");
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

#ifdef _WIN32
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

#endif

  const auto csv = read_text(output_dir / "log" / "summery.csv");
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
  const auto formula_csv = read_text(output_dir / "csv-formula" / "log" / "summery.csv");
  if (formula_csv.find("'=cmd.webp") == std::string::npos ||
      formula_csv.find("'=FORMULA") == std::string::npos) {
    return fail("summary CSV did not escape formula-like cells.");
  }

  std::filesystem::remove_all(root, ec);
  return 0;
}
