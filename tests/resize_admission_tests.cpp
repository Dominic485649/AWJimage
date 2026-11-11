#include "../src/compat/stdlib_compat.h"
#include "../src/core/hdr_cost.h"
#include "../src/core/work_memory_admission.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

import awj.animation;
import awj.avif_aom_codec;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.image;
import awj.image_memory_plan;
import awj.native_backend;
import awj.pipeline;
import awj.pixel_conversion;
import awj.png_codec;
import awj.resource_planner;

namespace {
constexpr std::uint64_t mib = 1024ull * 1024;

int fail(const std::string& message) {
  std::fprintf(stderr, "%s\n", message.c_str());
  return 1;
}

awj::ImageBuffer constant_image(std::size_t width, std::size_t height) {
  awj::ImageBuffer image{.width = width, .height = height, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 8};
  awj::ImagePlane plane{.stride = width * 4};
  plane.bytes.assign(plane.stride * height, std::byte{90});
  for (std::size_t p = 3; p < plane.bytes.size(); p += 4) plane.bytes[p] = std::byte{255};
  image.planes.push_back(std::move(plane));
  image.source_info = awj::ImageSourceInfo{};
  image.source_info->pixel_format = awj::PixelFormat::rgba;
  image.source_info->bit_depth = 8;
  image.source_info->color_primaries = 1;
  image.source_info->transfer_characteristics = 13;
  image.source_info->matrix_coefficients = 0;
  image.source_info->color_range = 1;
  return image;
}

awj::compat::expected<std::vector<std::byte>, std::string> png_bytes(std::size_t width, std::size_t height) {
  awj::NativeEncodeSettings settings;
  settings.output_format = awj::OutputFormat::png;
  settings.quality = 100;
  settings.bit_depth = 8;
  settings.applied_alpha = "kept";
  settings.applied_color_primaries = 1;
  settings.applied_transfer_characteristics = 13;
  settings.applied_matrix_coefficients = 0;
  settings.applied_color_range = 1;
  auto encoded = awj::PngImageEncoder{}.encode(constant_image(width, height), settings);
  if (!encoded) return awj::compat::unexpected{encoded.error()};
  return std::move(encoded->encoded.bytes);
}

bool write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::ofstream out{path, std::ios::binary};
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

awj::AppConfig png_config(const std::filesystem::path& input, const std::filesystem::path& output) {
  auto cfg = awj::default_app_config();
  cfg.input_path = input;
  cfg.output_dir = output;
  cfg.output_format = awj::OutputFormat::png;
  cfg.quality = 100;
  cfg.visual_quality.reset();
  cfg.bit_depth = 8;
  cfg.max_jobs = 8;
  cfg.memory_limit_bytes = 64 * mib; // 合法的显式 CLI 预算，不使用旧的 48 MiB 示例。
  cfg.write_log = false;
  cfg.write_summary = false;
  cfg.allow_wic_fallback = false;
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::manual, .scale_percent = 50};
  return cfg;
}

int batch_admission(const std::filesystem::path& root) {
  // 约 2 MP / 8 MiB 像素；宽矮尺寸使每 worker 可选缓存较大，
  // 但解码与串行几何确实能容纳在合法的 64 MiB 预算内。
  const auto input = root / "wide-short.png";
  auto bytes = png_bytes(32768, 64);
  if (!bytes || !write_bytes(input, *bytes)) return fail(bytes ? "Cannot write resize admission PNG." : bytes.error());
  const auto dimensions = awj::probe_image_dimensions_for_path(input, {.allow_wic_fallback = false});
  if (!dimensions) return fail(dimensions.error());
  std::vector<std::byte> reference;
  for (const int workers : {1, 8}) {
    auto cfg = png_config(input, root / ("bounded-" + std::to_string(workers)));
    cfg.max_jobs = workers;
    const auto preferred = awj::estimated_regular_memory(*dimensions, cfg, bytes->size());
    const auto selected = awj::estimated_regular_memory(*dimensions, cfg, bytes->size(), false, nullptr, cfg.memory_limit_bytes);
    if (selected.bytes > cfg.memory_limit_bytes || selected.decode_bytes > cfg.memory_limit_bytes ||
        (workers == 8 && (preferred.prepare_bytes <= cfg.memory_limit_bytes ||
         selected.geometry_scratch_bytes >= selected.geometry_preferred_scratch_bytes)))
      return fail("Wide-short fixture does not isolate optional-cache preflight rejection.");

    // 覆盖真实分类/过滤、ticket 与公开批处理入口，不只调用 resize_native_pixels；
    // 批处理必须传递取得的这个 ticket。
    const std::vector<awj::ImageFile> files{{.index = 0, .path = input, .bytes = bytes->size()}};
    auto classified = awj::pipeline_detail::classify_work_for_avif(cfg, files, cfg.memory_limit_bytes);
    if (!classified) return fail(classified.error());
    auto filtered = awj::pipeline_detail::reject_over_memory_budget(*classified, cfg.memory_limit_bytes, cfg);
    if (!filtered || classified->ordinary.size() != 1 || !classified->memory_rejected.empty())
      return fail("Optional cache still rejects a tight-budget batch before geometry fallback.");
    auto groups = awj::pipeline_detail::build_work_groups(cfg, classified->ordinary);
    if (!groups) return fail(groups.error());
    awj::WorkMemoryAdmission admission{cfg.memory_limit_bytes, *groups};
    const auto ticket = admission.acquire({});
    if (!ticket || ticket->bytes != selected.bytes) return fail("Preflight selection did not become the acquired ticket.");
    admission.release(ticket->bytes);

    std::optional<awj::EncodeResult> result;
    auto batch = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
      if (event.kind == awj::BatchEventKind::item_finished) result = event.result;
    });
    if (!batch || batch->ok_count != 1 || !result || !result->ok)
      return fail(result ? result->message : batch ? "Batch omitted its result." : batch.error());
    if (result->memory_plan.initial_bytes != selected.bytes || !result->memory_plan.refined ||
        result->memory_plan.reserved_peak_bytes < selected.bytes || result->memory_plan.reserved_peak_bytes > cfg.memory_limit_bytes ||
        result->resize_workers < 1 || result->resize_workers > workers || (workers == 8 && result->resize_workers == 8))
      return fail("Geometry escaped the finite ticket or lost the bounded worker/initial reservation diagnostics.");
    auto decoded = awj::PngImageDecoder{}.decode(result->output_path);
    if (!decoded || decoded->image.width != 16384 || decoded->image.height != 32)
      return fail(decoded ? "Batch output geometry is incorrect." : decoded.error());
    if (reference.empty()) reference = decoded->image.planes.front().bytes;
    else if (reference != decoded->image.planes.front().bytes)
      return fail("Budget 1/8 fallback changed PNG pixels.");
  }

  for (const bool input_only : {false, true}) {
    auto cfg = png_config(input, root / (input_only ? "insufficient-input-only" : "insufficient"));
    cfg.unlock_max_input_file_bytes = input_only;
    cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::fixed, .scale_percent = 400}; // 仅输出就需 128 MiB，应在实际分配前拒绝。
    std::optional<awj::EncodeResult> result;
    auto batch = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
      if (event.kind == awj::BatchEventKind::item_finished) result = event.result;
    });
    if (!batch || batch->ok_count || !result || result->ok || !result->processed || result->resize_workers ||
        result->decode_seconds > 0 || std::filesystem::exists(result->output_path))
      return fail("A genuinely insufficient/input-only batch was not rejected before decode/output.");
  }
  return 0;
}

int batch_cpu_quota(const std::filesystem::path& root) {
  const auto input = root / "cpu-quota-input";
  std::error_code ec;
  if (!std::filesystem::create_directory(input, ec) || ec) return fail("Cannot create CPU-quota fixture directory.");
  auto bytes = png_bytes(512, 256);
  if (!bytes || !write_bytes(input / "left.png", *bytes) || !write_bytes(input / "right.png", *bytes))
    return fail(bytes ? "Cannot write CPU-quota fixtures." : bytes.error());
  auto cfg = png_config(input, root / "cpu-quota-output");
  cfg.max_jobs = 5; // 动态份额分配余数，尾部可借用空闲额度；总额守恒由 CPU admission 定向测试证明。
  std::atomic_size_t finished{}, invalid{};
  auto batch = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
    if (event.kind != awj::BatchEventKind::item_finished) return;
    const auto& result = event.result;
    if (!result.ok || result.resize_workers < 1 || result.resize_workers > cfg.max_jobs ||
        result.memory_plan.reserved_peak_bytes > cfg.memory_limit_bytes)
      invalid.fetch_add(1);
    finished.fetch_add(1);
  });
  if (!batch || batch->ok_count != 2 || finished.load() != 2 || invalid.load())
    return fail(batch ? "Dynamic batch geometry escaped the selected CPU/memory budgets or lost an item." : batch.error());
  return 0;
}

int held_ticket_and_pair() {
  const awj::encoding_defaults::ScopedMemoryPolicy finite{{256 * mib, false, false}};
  auto image = constant_image(512, 256);
  image.metadata.push_back({awj::MetadataKind::xmp, std::vector<std::byte>(4096)});
  image.metadata.front().bytes.reserve(8192);
  auto cfg = awj::default_app_config();
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::manual, .scale_percent = 50};
  const auto geometry = awj::plan_image_geometry(image.width, image.height, cfg);
  if (!geometry) return fail(geometry.error());
  const auto resident = awj::pixel_detail::geometry_resident_bytes(image, 256 * 128 * 4);
  const auto scratch = awj::geometry_scratch_requirements(512, 256, 256, 128, 2);
  const auto other = awj::resident_image_bytes(image);
  awj::ResourcePlan resources{.file_parallelism = 2, .encoder_threads_per_file = 8, .global_thread_budget = 5,
      .memory_limit_bytes = 256 * mib, .memory_reservation_bytes = resident + other + scratch.preferred_bytes};
  auto layer = resources;
  layer.geometry_retained_bytes = other;
  auto admitted = awj::admit_image_geometry(image, *geometry, layer);
  if (!admitted || admitted->encoder_threads_per_file != 2 || admitted->file_parallelism != 1 ||
      admitted->memory_limit_bytes != resources.memory_reservation_bytes)
    return fail("Normalizing the ticket changed the original global/files CPU quota.");
  awj::GeometryTransformDiagnostics diagnostics;
  auto resized = awj::apply_image_geometry(image, *geometry, true, {}, *admitted, &diagnostics);
  if (!resized || diagnostics.workers != 2 || diagnostics.working_set_bytes > resources.memory_reservation_bytes)
    return fail("Actual geometry expanded into the unused global budget beyond its ticket.");

  awj::ImageDecodeResult pair{.image = image, .gain_map_alternative = awj::GainMapAlternative{.image = image}};
  awj::EncodeResult timing;
  auto transformed = awj::native_backend_detail::apply_image_size_limit(pair, cfg, {}, resources, timing);
  if (!transformed || pair.image.width != 256 || pair.gain_map_alternative->image.width != 256 || timing.resize_workers != 2)
    return fail(transformed ? "Sequential renditions did not retain the shared CPU cap/worker maximum." : transformed.error());
  resources.memory_reservation_bytes = resident + scratch.minimum_bytes; // 可容纳单图，但放不下仍存活的另一呈现。
  for (const bool input_only : {false, true}) {
    const awj::encoding_defaults::ScopedMemoryPolicy policy{{256 * mib, input_only, false}};
    awj::ImageDecodeResult rejected{.image = image, .gain_map_alternative = awj::GainMapAlternative{.image = image}};
    awj::EncodeResult failed;
    if (awj::native_backend_detail::apply_image_size_limit(rejected, cfg, {}, resources, failed) ||
        failed.resize_workers || rejected.image.width != 512)
      return fail("A retained alternative was omitted, or input-only unlock discarded the working-set ticket.");
  }
  {
    const awj::encoding_defaults::ScopedMemoryPolicy unlocked{{UINT64_MAX, true, true}};
    resources.memory_limit_bytes = 1;
    resources.memory_reservation_bytes = 1;
    bool reserved{};
    awj::ImageDecodeResult pair_unlocked{.image = image, .gain_map_alternative = awj::GainMapAlternative{.image = image}};
    awj::EncodeResult result;
    auto ok = awj::native_backend_detail::apply_image_size_limit(pair_unlocked, cfg, {}, resources, result,
        [&](std::uint64_t) { reserved = true; return false; });
    if (!ok || reserved || result.resize_workers != 2)
      return fail("Full unlock revived a stale finite ticket/callback or dropped the CPU quota.");
  }
  layer.geometry_retained_bytes = UINT64_MAX;
  if (awj::admit_image_geometry(image, *geometry, layer)) return fail("Retained-storage arithmetic overflow was admitted.");
  return 0;
}

class RetainedAnimationReader final : public awj::AnimationReader {
 public:
  RetainedAnimationReader() : image_{constant_image(512, 256)}, other_(128 * 1024) {
    info_.width = image_.width;
    info_.height = image_.height;
    info_.durations.assign(2, awj::FrameDuration{1, 10});
    info_.decoder_id = "retained-storage-test";
  }
  const awj::AnimationInfo& info() const noexcept override { return info_; }
  const awj::ImageBuffer& frame() const noexcept override { return image_; }
  std::uint64_t memory_usage_bytes() const noexcept override {
    return awj::resident_image_bytes(image_) + other_.capacity();
  }
  awj::compat::expected<bool, std::string> next(awj::compat::stop_token stop = {}) override {
    if (stop.stop_requested()) return awj::compat::unexpected{"canceled"};
    return ++reads_ <= 2;
  }
 private:
  awj::ImageBuffer image_;
  std::vector<std::byte> other_;
  awj::AnimationInfo info_;
  int reads_{};
};

int animation_reservation() {
  const awj::encoding_defaults::ScopedMemoryPolicy finite{{256 * mib, false, false}};
  auto cfg = awj::default_app_config();
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::manual, .scale_percent = 50};
  for (const bool deny_growth : {false, true}) {
    awj::ResourcePlan resources{.file_parallelism = 2, .encoder_threads_per_file = 8, .global_thread_budget = 5,
        .memory_limit_bytes = 8 * mib, .memory_reservation_bytes = 2 * mib};
    std::uint64_t held = resources.memory_reservation_bytes;
    std::size_t requests{};
    const auto reserve = [&](std::uint64_t bytes) {
      ++requests;
      if (bytes > resources.memory_limit_bytes || (deny_growth && bytes > held)) return false;
      held = bytes;
      return true;
    };
    awj::EncodeResult timing;
    awj::native_backend_detail::GeometryAnimationReader reader{
        std::make_unique<RetainedAnimationReader>(), cfg, resources, timing, reserve};
    auto first = reader.next();
    if (!first || !*first || timing.resize_workers != 2 || reader.memory_usage_bytes() <= 128 * 1024)
      return fail("Animation first-frame storage or original CPU quota was lost.");
    awj::NativeEncodeSettings settings;
    settings.memory_reservation = reserve;
    settings.memory_base_reservation = reader.memory_usage_bytes() + 384 * 1024;
    // 模拟编码器已经接受的 reader 与持久编码状态合并预约。
    held = settings.memory_base_reservation;
    reader.bind_encoding_memory(settings);
    const auto before = held;
    auto second = reader.next();
    if (deny_growth) {
      if (second || held != before || timing.resize_workers != 0 || !requests)
        return fail("Nested animation resize ignored retained encoder storage or failed reservation growth.");
    } else if (!second || !*second || held <= before || held > resources.memory_limit_bytes || timing.resize_workers != 2 || !requests) {
      return fail("Nested animation resize did not reserve its combined reader/codec/geometry peak.");
    }
  }
  return 0;
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(std::byte{static_cast<unsigned char>(value >> shift)});
}
std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value{};
  for (std::size_t i = 0; i < 4; ++i) value = (value << 8) | std::to_integer<unsigned char>(bytes[offset + i]);
  return value;
}
void append_chunk(std::vector<std::byte>& out, const std::array<char, 4>& type, std::span<const std::byte> data) {
  append_u32(out, static_cast<std::uint32_t>(data.size()));
  const auto start = out.size();
  for (const auto c : type) out.push_back(std::byte{static_cast<unsigned char>(c)});
  out.insert(out.end(), data.begin(), data.end());
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = start; i < out.size(); ++i) {
    crc ^= std::to_integer<unsigned char>(out[i]);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
  }
  append_u32(out, crc ^ 0xffffffffu);
}

// 单帧 APNG 在静态渐进 AVIF 编码期间保留真实 reader，
// 从而覆盖动画、预览及最终转换结果共存的组合。
awj::compat::expected<std::vector<std::byte>, std::string> single_frame_apng(std::uint32_t width, std::uint32_t height) {
  auto png = png_bytes(width, height);
  if (!png) return awj::compat::unexpected{png.error()};
  std::vector<std::byte> out(png->begin(), png->begin() + 8);
  bool inserted{};
  for (std::size_t offset = 8; offset + 12 <= png->size();) {
    const auto size = read_u32(*png, offset);
    if (size > png->size() - offset - 12) return awj::compat::unexpected{"Generated PNG chunk is truncated."};
    if (!inserted && (*png)[offset + 4] == std::byte{'I'} && (*png)[offset + 5] == std::byte{'D'}) {
      std::vector<std::byte> control;
      append_u32(control, 1); append_u32(control, 0);
      append_chunk(out, {'a', 'c', 'T', 'L'}, control);
      control.clear();
      for (const auto value : {0u, width, height, 0u, 0u}) append_u32(control, value);
      control.insert(control.end(), {std::byte{0}, std::byte{1}, std::byte{0}, std::byte{10}, std::byte{0}, std::byte{0}});
      append_chunk(out, {'f', 'c', 'T', 'L'}, control);
      inserted = true;
    }
    out.insert(out.end(), png->begin() + static_cast<std::ptrdiff_t>(offset),
        png->begin() + static_cast<std::ptrdiff_t>(offset + size + 12));
    offset += size + 12;
  }
  if (!inserted) return awj::compat::unexpected{"Generated PNG has no IDAT."};
  return out;
}

int progressive_batch(const std::filesystem::path& root) {
  for (const bool animation : {false, true}) {
    const auto input = root / (animation ? "single-frame.png" : "layered-source.png");
    auto bytes = animation ? single_frame_apng(1024, 256) : png_bytes(1024, 256);
    if (!bytes || !write_bytes(input, *bytes)) return fail(bytes ? "Cannot write progressive fixture." : bytes.error());
    auto cfg = png_config(input, root / (animation ? "single-frame-layered" : "static-layered"));
    cfg.output_format = awj::OutputFormat::avif;
    cfg.quality = 70;
    cfg.speed = 10;
    cfg.chroma_mode = awj::ChromaMode::yuv444;
    cfg.avif_layers.count = 3;
    cfg.avif_layers.layers[0].scale = std::pair{1u, 2u};
    cfg.avif_layers.layers[1].scale = std::pair{3u, 4u};
    std::optional<awj::EncodeResult> result;
    auto batch = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
      if (event.kind == awj::BatchEventKind::item_finished) result = event.result;
    });
    if (!batch || batch->ok_count != 1 || !result || !result->ok)
      return fail(result ? result->message : batch ? "Progressive batch omitted its result." : batch.error());
    if (result->avif_layer_count != 3 || result->resize_workers < 1 || result->resize_workers > 8 ||
        result->memory_plan.reserved_peak_bytes > cfg.memory_limit_bytes || !result->memory_plan.layer_retained_bytes)
      return fail("Progressive preview/reader state was not part of the finite batch reservation.");
    auto decoded = awj::make_avif_image_decoder(1)->decode(result->output_path);
    if (!decoded || decoded->image.width != 512 || decoded->image.height != 128)
      return fail(decoded ? "Progressive batch lost its final layer." : decoded.error());
  }
  // 源图/预览各自可容纳，但最终转换结果、SVC、引用与输出流的共存预约超预算；
  // 直接调用编码器也必须拒绝，不能把整份全局额度交给 Spline36。
  awj::NativeEncodeSettings settings;
  settings.quality = 70; settings.speed = 10; settings.bit_depth = 8;
  settings.chroma_mode = awj::ChromaMode::yuv444;
  settings.avif_layers.count = 2;
  settings.avif_layers.layers[0].scale = std::pair{1u, 2u};
  settings.resources = {.file_parallelism = 2, .encoder_threads_per_file = 8,
      .global_thread_budget = 5, .memory_limit_bytes = 8 * mib};
  auto encoder = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
  const awj::encoding_defaults::ScopedMemoryPolicy finite{{256 * mib, false, false}};
  const auto rejected = encoder->encode(constant_image(512, 256), settings);
  if (rejected || rejected.error().find("Spline36") == std::string::npos)
    return fail("Manual AVIF preview did not reject the combined geometry/encoder working set.");
  settings.resources.memory_limit_bytes = 128 * mib;
  const auto accepted = encoder->encode(constant_image(512, 256), settings);
  if (!accepted || accepted->diagnostics.encoder_threads != 2 || accepted->diagnostics.timing.resize_workers != 0)
    return fail(accepted ? "Progressive encoding lost the original global/files CPU quota or published preview as user resize." : accepted.error());
  return 0;
}
int visual_progressive_admission(const std::filesystem::path& root) {
  const auto input = root / "visual-progressive-wide.png";
  auto bytes = png_bytes(16384, 64);
  if (!bytes || bytes->size() > 32 * 1024 || !write_bytes(input, *bytes))
    return fail(bytes ? "视觉分层夹具不够可压缩或写入失败。" : bytes.error());
  auto cfg = png_config(input, root / "visual-progressive-output");
  cfg.output_format = awj::OutputFormat::avif;
  cfg.quality = 70;
  cfg.visual_quality = 90;
  cfg.bit_depth = 8;
  cfg.chroma_mode = awj::ChromaMode::yuv444;
  cfg.alpha_policy = awj::AlphaModePolicy::off;
  cfg.speed = 10;
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::none};
  cfg.memory_limit_bytes = 144 * mib;
  cfg.avif_layers.count = 2;
  cfg.avif_layers.layers[0].scale = std::pair{1u, 2u};
  const auto dimensions = awj::probe_image_dimensions_for_path(input, {.allow_wic_fallback = false});
  if (!dimensions) return fail(dimensions.error());
  const auto preferred = awj::estimated_regular_memory(*dimensions, cfg, bytes->size());
  const auto selected = awj::estimated_regular_memory(*dimensions, cfg, bytes->size(), false, nullptr, cfg.memory_limit_bytes);
  if (preferred.bytes <= cfg.memory_limit_bytes || selected.bytes > cfg.memory_limit_bytes ||
      !selected.visual_encode_retained_bytes || selected.layer_retained_bytes >= preferred.layer_retained_bytes)
    return fail("视觉缓存尚未在可选预览 scratch 选择前扣除。" );
  const std::vector<awj::ImageFile> files{{.index = 0, .path = input, .bytes = bytes->size()}};
  auto classified = awj::pipeline_detail::classify_work_for_avif(cfg, files, cfg.memory_limit_bytes);
  if (!classified) return fail(classified.error());
  auto filtered = awj::pipeline_detail::reject_over_memory_budget(*classified, cfg.memory_limit_bytes, cfg);
  if (!filtered || classified->ordinary.size() != 1 || !classified->memory_rejected.empty())
    return fail("视觉搜索与手动分层组合在精化前被误拒。" );
  auto groups = awj::pipeline_detail::build_work_groups(cfg, classified->ordinary);
  if (!groups) return fail(groups.error());
  awj::WorkMemoryAdmission admission{cfg.memory_limit_bytes, *groups};
  const auto ticket = admission.acquire({});
  if (!ticket || ticket->bytes > cfg.memory_limit_bytes) return fail("视觉分层未取得合法 ticket。" );
  admission.release(ticket->bytes);
  std::optional<awj::EncodeResult> result;
  auto batch = awj::run_batch(cfg, [&](const awj::BatchProgress& event) {
    if (event.kind == awj::BatchEventKind::item_finished) result = event.result;
  });
  if (!batch || batch->ok_count != 1 || !result || !result->ok)
    return fail(result ? result->message : batch ? "视觉分层批处理缺失结果。" : batch.error());
  if (!result->memory_plan.refined || result->memory_plan.reserved_peak_bytes > cfg.memory_limit_bytes ||
      result->visual_quality_candidate_count < 1 || result->avif_layer_count != 2 ||
      result->resize_seconds != 0.0 || result->resize_workers != 0)
    return fail("视觉分层预约越界、跳过搜索或把内部预览计为用户缩放。" );
  auto decoded = awj::make_avif_image_decoder(1)->decode(result->output_path);
  if (!decoded || decoded->image.width != 16384 || decoded->image.height != 64)
    return fail(decoded ? "视觉分层丢失最终全尺寸图像。" : decoded.error());
  std::printf("visual-progressive budget=%llu selected=%llu reserved_peak=%llu candidates=%d user_resize_workers=%d\n",
      static_cast<unsigned long long>(cfg.memory_limit_bytes), static_cast<unsigned long long>(selected.bytes),
      static_cast<unsigned long long>(result->memory_plan.reserved_peak_bytes),
      result->visual_quality_candidate_count, result->resize_workers);
  return 0;
}
int fp16_visual_progressive_admission(const std::filesystem::path& root) {
  constexpr std::size_t width = 8192, height = 64;
  constexpr auto global_budget = 512 * mib;
  const awj::encoding_defaults::ScopedMemoryPolicy finite{{global_budget, false, false}};
  auto owner = std::make_shared<std::vector<std::byte>>(width * height * 8);
  for (std::size_t p = 0; p < width * height; ++p) {
    const std::array<std::uint16_t, 4> rgba{
        static_cast<std::uint16_t>((p % width) / 256 % 2 ? 0x3c00u : 0x3800u),
        0xb400u, 0x4000u, 0x3c00u};
    std::memcpy(owner->data() + p * 8, rgba.data(), 8);
  }
  const auto original = *owner;
  awj::ImageDecodeResult decoded;
  decoded.decoder_id = "predecoded-scrgb-budget";
  decoded.image = {.width = width, .height = height, .pixel_format = awj::PixelFormat::rgba,
      .bit_depth = 16, .sample_representation = awj::SampleRepresentation::ieee_half_float,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
          .color_primaries = 1, .transfer_characteristics = 8, .matrix_coefficients = 0,
          .color_range = 1, .has_hdr_metadata = true, .color_metadata_source = "wgc-scrgb-half-linear",
          .linear_reference_white_nits = 80.0F}};
  decoded.image.planes.push_back({.stride = width * 8, .borrowed = std::span<const std::byte>{*owner}, .lease = owner});
  decoded.image.metadata.push_back({awj::MetadataKind::xmp, std::vector<std::byte>(1024, std::byte{' '})});
  decoded.image.metadata.front().bytes.reserve(8192);
  auto cfg = png_config({}, {});
  cfg.output_format = awj::OutputFormat::avif;
  cfg.quality = 70;
  cfg.visual_quality = 90;
  cfg.visual_quality_fallback = true;
  cfg.bit_depth = 12;
  cfg.chroma_mode = awj::ChromaMode::yuv444;
  cfg.alpha_policy = awj::AlphaModePolicy::off;
  cfg.speed = 10;
  cfg.max_jobs = 5;
  cfg.strip_metadata = true;
  cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::none};
  cfg.memory_limit_bytes = global_budget;
  cfg.avif_layers.count = 2;
  cfg.avif_layers.layers[0].scale = std::pair{1u, 2u};
  awj::ResourcePlan resources{.file_parallelism = 2, .encoder_threads_per_file = 8,
      .global_thread_budget = 5, .memory_limit_bytes = global_budget};
  auto settings = awj::native_backend_detail::settings_from_config(cfg, resources);
  settings.applied_alpha = "stripped";
  settings.applied_color_primaries = 9;
  settings.applied_transfer_characteristics = 16;
  settings.applied_matrix_coefficients = 9;
  settings.applied_color_range = 1;
  const auto preferred = awj::refined_regular_memory(decoded, settings, cfg, 0);
  const auto scratch = awj::geometry_scratch_requirements(width, height, width / 2, height / 2, 2);
  std::uint64_t ticket_bytes{}, minimum_required{};
  {
    auto reference = awj::convert_image_pixels(decoded.image,
        {.format = awj::PixelFormat::rgba, .bit_depth = 16, .preserve_alpha = false}, {}, resources);
    if (!reference) return fail(reference.error());
    const awj::PixelConversionRequest conversion{.format = awj::PixelFormat::yuv444, .bit_depth = 12,
        .matrix_coefficients = 9, .color_range = 1, .preserve_alpha = false};
    auto final_pixels = awj::convert_image_pixels(*reference, conversion, {}, resources);
    if (!final_pixels) return fail(final_pixels.error());
    const auto exact = awj::refined_regular_memory(decoded, settings, cfg, 0, UINT64_MAX, 0, &*reference);
    if (exact.visual_reference_bytes != awj::resident_image_bytes(*reference) ||
        exact.source_resident_bytes != awj::resident_image_bytes(decoded.image))
      return fail("FP16 原图与已物化参考的实际容量没有分别进入模型。");

    // 独立重建 AOM 首个手动预览的常驻公式：几何自身计参考与 RGBA 预览，
    // 外层补原图、指标/候选码流、最终 YUV、SVC/引用、流和后续预览 YUV。
    const auto planes = awj::pixel_detail::destination_pixel_bytes(width, height, conversion, awj::AlphaMode::none);
    const auto preview_planes = awj::pixel_detail::destination_pixel_bytes(width / 2, height / 2, conversion, awj::AlphaMode::none);
    auto codec_retained = awj::regular_memory_fixed_bytes(2) +
        awj::avif_codec_workspace_bytes(width * height, planes, 2, 12, true, false, 2) +
        (planes + 64 * 1024) * 6 + planes + awj::resident_image_bytes(*final_pixels) + preview_planes + 4 * sizeof(awj::ImagePlane);
    codec_retained += reference->metadata.size() * sizeof(awj::MetadataBlock);
    for (const auto& block : reference->metadata) codec_retained += block.bytes.capacity();
    if (reference->source_info) codec_retained += reference->source_info->color_metadata_source.capacity() + 1;
    auto layer_resources = resources;
    layer_resources.geometry_retained_bytes = codec_retained + exact.visual_encode_retained_bytes +
        awj::resident_image_bytes(decoded.image);
    const auto geometry_live = awj::pixel_detail::geometry_resident_bytes(*reference, width / 2 * (height / 2) * 8) +
        layer_resources.geometry_retained_bytes;
    const auto model_live = std::max(preferred.encode_bytes, exact.encode_bytes) - scratch.preferred_bytes;
    minimum_required = std::max(geometry_live, model_live) + scratch.minimum_bytes;
    ticket_bytes = minimum_required + 128 * 1024;
    const auto selected = awj::refined_regular_memory(decoded, settings, cfg, 0, ticket_bytes, 0, &*reference);
    if (preferred.bytes <= ticket_bytes || selected.bytes > ticket_bytes ||
        selected.layer_retained_bytes >= exact.layer_retained_bytes || ticket_bytes >= global_budget / 2)
      return fail("FP16 夹具没有隔离可选预览缓存，或紧 ticket 仍不可准入。");
    layer_resources.memory_reservation_bytes = ticket_bytes;
    const awj::ImageGeometryPlan geometry{.source_width = width, .source_height = height,
        .content_width = width / 2, .content_height = height / 2, .canvas_width = width / 2, .canvas_height = height / 2};
    auto admitted = awj::admit_image_geometry(*reference, geometry, layer_resources);
    if (!admitted || admitted->memory_limit_bytes != ticket_bytes || admitted->encoder_threads_per_file != 2)
      return fail(admitted ? "已有 ticket 被再除文件并发或丢失 CPU 配额。" : admitted.error());
    awj::GeometryTransformDiagnostics diagnostics;
    auto preview = awj::apply_image_geometry(*reference, geometry, false, {}, *admitted, &diagnostics);
    if (!preview || !diagnostics.workers || diagnostics.workers > 2 ||
        diagnostics.scratch_bytes >= scratch.preferred_bytes || diagnostics.working_set_bytes > ticket_bytes)
      return fail(preview ? "FP16 手动预览没有在双图常驻的紧 ticket 内降缓存。" : preview.error());
  }

  // 本回归使用跨平台预解码 memory 入口，不经过文件探测/分类，也不依赖 WIC。
  // 同时占满其余会话预算，使偷用 global 或重复除 file_parallelism 都会真正失败。
  awj::FileLogger logger{root, false};
  for (const bool insufficient : {false, true}) {
    const auto initial = insufficient ? minimum_required - awj::resident_image_bytes(decoded.image) / 2 : ticket_bytes;
    if (insufficient && initial >= preferred.encode_bytes - scratch.preferred_bytes + scratch.minimum_bytes)
      return fail("FP16 拒绝夹具没有低于原图与稳定参考共存的最低模型。");
    struct Group { std::uint64_t estimated_bytes; };
    const std::array<Group, 2> groups{{{initial}, {global_budget - initial}}};
    awj::WorkMemoryAdmission admission{global_budget, groups};
    const auto blocker = admission.acquire({});
    const auto ticket = admission.acquire({});
    if (!blocker || blocker->index != 1 || !ticket || ticket->index != 0 || ticket->bytes != initial)
      return fail("无法取得 FP16 有限 ticket 与其余预算占位预约。");
    auto held = ticket->bytes;
    std::vector<std::uint64_t> requests;
    const auto reserve = [&](std::uint64_t bytes) {
      requests.push_back(bytes);
      return admission.try_resize(held, bytes);
    };
    auto bounded_resources = resources;
    bounded_resources.memory_reservation_bytes = held;
    awj::NativeBackend backend{cfg, logger, bounded_resources, reserve};
    auto input = decoded;
    input.image.metadata.front().bytes.reserve(decoded.image.metadata.front().bytes.capacity());
    auto encoded = backend.encode_memory(std::move(input));
    if (*owner != original || decoded.image.sample_representation != awj::SampleRepresentation::ieee_half_float ||
        decoded.image.source_info->transfer_characteristics != 8)
      return fail("FP16 视觉搜索成功/拒绝路径改写了负值、扩展范围样本或输入色彩信令。");
    if (insufficient) {
      if (encoded || requests.empty() || requests.front() <= initial || held != initial ||
          encoded.error().find("预约不足") == std::string::npos)
        return fail(encoded ? "FP16 真实必要增长无法预约时仍启动编码。" : encoded.error());
    } else {
      if (!encoded) return fail(encoded.error());
      if (requests.size() < 2 || *std::max_element(requests.begin(), requests.end()) > ticket_bytes ||
          encoded->diagnostics.timing.visual_quality_candidate_count < 2 || encoded->diagnostics.avif_layer_count != 2 ||
          encoded->diagnostics.encoder_threads != 2 || encoded->diagnostics.timing.resize_workers != 0 ||
          encoded->diagnostics.timing.resize_seconds != 0.0 || encoded->diagnostics.timing.decode_seconds != -1.0 ||
          encoded->diagnostics.timing.write_seconds != -1.0)
        return fail("FP16 后续候选/预览偷用全局额度、跳过真实参考精化或污染 memory/用户 resize 诊断。");
      const auto restored = awj::make_avif_image_decoder(1)->decode_memory(encoded->encoded.bytes, ".avif");
      if (!restored || restored->image.width != width || restored->image.height != height ||
          !restored->image.source_info || restored->image.source_info->bit_depth != 12)
        return fail(restored ? "FP16 视觉分层输出丢失最终尺寸或 12-bit 精度。" : restored.error());
      std::printf("fp16-memory ticket=%llu requested_peak=%llu candidates=%d\n",
          static_cast<unsigned long long>(ticket_bytes),
          static_cast<unsigned long long>(*std::max_element(requests.begin(), requests.end())),
          encoded->diagnostics.timing.visual_quality_candidate_count);
    }
    admission.release(held);
    admission.release(blocker->bytes);
  }
  return 0;
}
} // namespace

int main() {
  const auto root = std::filesystem::path{AWJ_TEST_RESULTS_ROOT} /
      ("resize-admission-" + std::to_string(awj::current_process_id()));
  std::error_code ec;
  if (!std::filesystem::create_directories(root, ec) || ec) return fail("Dedicated resize admission directory already exists or cannot be created.");
  if (const auto result = batch_admission(root)) return result;
  if (const auto result = batch_cpu_quota(root)) return result;
  if (const auto result = held_ticket_and_pair()) return result;
  if (const auto result = animation_reservation()) return result;
  if (const auto result = progressive_batch(root)) return result;
  if (const auto result = visual_progressive_admission(root)) return result;
  if (const auto result = fp16_visual_progressive_admission(root)) return result;
  std::filesystem::remove_all(root, ec);
  return ec ? fail("Could not clean generated resize admission fixtures.") : 0;
}
