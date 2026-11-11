#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include "../src/compat/stdlib_compat.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

import awj.codec;
import awj.config;
import awj.image;
#if AWJ_HAS_JPEGLI
import awj.jpegli_codec;
#endif
import awj.native_visual_search;
import awj.resource_planner;
import awj.visual_metrics;
import awj.visual_quality;
import awj.webp_codec;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

awj::ImageBuffer make_test_image(std::size_t width = 4, std::size_t height = 4) {
  awj::ImagePlane plane{.stride = width * 4};
  plane.bytes.resize(width * height * 4);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto offset = y * plane.stride + x * 4;
      plane.bytes[offset + 0] = std::byte{static_cast<unsigned char>(x * 60)};
      plane.bytes[offset + 1] = std::byte{static_cast<unsigned char>(y * 60)};
      plane.bytes[offset + 2] = std::byte{static_cast<unsigned char>((x + y) * 30)};
      plane.bytes[offset + 3] = std::byte{255};
    }
  }
  awj::ImageBuffer image{.width = width,
                          .height = height,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

int verify_cpu_candidates(const awj::ImageEncoder& encoder,
                          const awj::ImageDecoder& decoder) {
  // 超过原 GPU session 的大图阈值，验证指标仍与 CPU 基准一致，并复用候选缓存。
  const auto image = make_test_image(1024, 1024);
  const awj::NativeEncodeSettings settings{
      .output_format = awj::OutputFormat::webp,
      .speed = 10,
      .resources = awj::ResourcePlan{.file_parallelism = 2,
                                    .encoder_threads_per_file = 7,
                                    .global_thread_budget = 8}};
  std::optional<awj::LumaImage> reference_luma;
  awj::CpuVisualMetricWorkspace workspace;
  const float* reference_storage = nullptr;
  const float* candidate_storage = nullptr;
  const float* reference_level_storage = nullptr;
  for (const int quality : {30, 70}) {
    auto evaluated = awj::evaluate_visual_quality_candidate(
        reference_luma, workspace, image, encoder, decoder, settings, quality,
        "cpu-visual-quality.webp");
    if (!evaluated) return fail(evaluated.error());
    auto decoded = decoder.decode_memory(evaluated->encode_result.encoded.bytes,
                                         "cpu-visual-quality.webp");
    if (!decoded) return fail(decoded.error());
    auto candidate_luma = awj::make_luma_image(decoded->image);
    if (!candidate_luma) return fail(candidate_luma.error());
    if (!reference_luma) return fail("CPU reference luma 未建立。");
    auto expected = awj::calculate_visual_metrics_cpu(*reference_luma, *candidate_luma);
    if (!expected || !std::isfinite(evaluated->candidate.raw_gmsd) ||
        !std::isfinite(evaluated->candidate.raw_ms_ssim) ||
        !std::isfinite(evaluated->candidate.visual_score) ||
        std::abs(evaluated->candidate.raw_gmsd - expected->raw_gmsd) > 1e-9 ||
        std::abs(evaluated->candidate.raw_ms_ssim - expected->raw_ms_ssim) > 1e-9 ||
        std::abs(evaluated->candidate.visual_score - expected->score.visual_score) > 1e-9) {
      return fail("大图视觉质量候选没有使用 CPU 指标。");
    }
    if (!workspace.reference_ready || workspace.candidate_luma.pixels.empty() ||
        workspace.reference_levels[0].pixels.empty()) {
      return fail("CPU 视觉质量工作区未建立可复用缓存。");
    }
    if (reference_storage &&
        (reference_storage != reference_luma->pixels.data() ||
         candidate_storage != workspace.candidate_luma.pixels.data() ||
         reference_level_storage != workspace.reference_levels[0].pixels.data())) {
      return fail("CPU 视觉质量候选没有复用 reference 与候选缓存。");
    }
    reference_storage = reference_luma->pixels.data();
    candidate_storage = workspace.candidate_luma.pixels.data();
    reference_level_storage = workspace.reference_levels[0].pixels.data();
  }
  awj::compat::stop_source canceled;
  canceled.request_stop();
  auto result = awj::evaluate_visual_quality_candidate(
      reference_luma, workspace, image, encoder, decoder, settings, 50,
      "cpu-visual-quality.webp", canceled.get_token());
  if (result || result.error().find("已取消") == std::string::npos) {
    return fail("CPU 视觉质量计算没有响应取消。");
  }
  return 0;
}

#if AWJ_HAS_JPEGLI
int verify_jpeg_auto_search() {
  awj::JpegliImageEncoder encoder;
  awj::JpegliImageDecoder decoder;
  const auto candidate = std::filesystem::temp_directory_path() / "awj-jpegli-auto-vq-test.jpg";
  for (const auto source : {awj::PixelFormat::yuv420, awj::PixelFormat::yuv422,
      awj::PixelFormat::yuv444, awj::PixelFormat::rgb, awj::PixelFormat::rgba,
      awj::PixelFormat::gray, awj::PixelFormat::unknown}) {
    auto image = make_test_image();
    // Search sees expanded RGBA, but original source sampling must win.
    image.source_info = awj::ImageSourceInfo{.pixel_format = source, .bit_depth = 8};
    const auto chroma = source == awj::PixelFormat::yuv420 ? awj::ChromaMode::yuv420
                      : source == awj::PixelFormat::yuv422 ? awj::ChromaMode::yuv422
                      : awj::ChromaMode::yuv444;
    const auto format = chroma == awj::ChromaMode::yuv420 ? awj::PixelFormat::yuv420
                      : chroma == awj::ChromaMode::yuv422 ? awj::PixelFormat::yuv422
                      : awj::PixelFormat::yuv444;
    auto searched = awj::encode_with_native_visual_quality_search(image, encoder, decoder,
        awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jpgli,
            .quality = 60, .visual_quality = 30, .chroma_mode = awj::ChromaMode::auto_keep,
            .strip_metadata = true, .visual_quality_fallback = true,
            .resources = awj::ResourcePlan{.file_parallelism = 1,
                .encoder_threads_per_file = 1, .global_thread_budget = 1}}, candidate);
    std::error_code ec;
    std::filesystem::remove(candidate, ec);
    if (!searched) return fail(searched.error());
    const auto& result = searched->encode_result;
    auto decoded = decoder.decode_memory(result.encoded.bytes, "auto-search SOF");
    if (!decoded || !decoded->image.source_info ||
        decoded->image.source_info->pixel_format != format ||
        result.diagnostics.requested_chroma != "auto" ||
        result.diagnostics.applied_chroma != awj::chroma_mode_name(chroma) ||
        result.search_attempt_count < 1 || result.diagnostics.visual_quality_search_trace.find("selected=") == std::string::npos)
      return fail(decoded ? "JPEG auto search diagnostics disagree with actual three-component SOF." : decoded.error());
  }
  return 0;
}
#endif

awj::ImageBuffer threshold_fixture(int amplitude) {
  constexpr std::size_t width = 257, height = 259;
  awj::ImageBuffer image{.width = width, .height = height, .pixel_format = awj::PixelFormat::gray, .bit_depth = 16};
  awj::ImagePlane plane{.stride = width * 2 + 16};
  plane.bytes.resize(plane.stride * height, std::byte{0xa5});
  for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
    const int base = 18000 + static_cast<int>((x * 197 + y * 89) % 30000);
    const int noise = static_cast<int>(((x * 7477 + y * 4721) ^ (x * y * 31)) % 257) - 128;
    const auto value = static_cast<std::uint16_t>(std::clamp(base + noise * amplitude / 128, 0, 65535));
    std::memcpy(plane.bytes.data() + y * plane.stride + x * 2, &value, 2);
  }
  image.planes.push_back(std::move(plane));
  return image;
}

class ThresholdEncoder final : public awj::ImageEncoder {
 public:
  mutable std::vector<int> probes;
  std::string_view id() const noexcept override { return "threshold-fixture"; }
  awj::CodecCapabilities capabilities() const noexcept override {
    return {.output_format = awj::OutputFormat::webp,
        .features = awj::CodecFeature::visual_quality_search | awj::CodecFeature::thread_control};
  }
  awj::compat::expected<awj::NativeEncodeResult, std::string> encode(const awj::ImageBuffer&,
      const awj::NativeEncodeSettings& settings, awj::compat::stop_token stop) const override {
    if (stop.stop_requested()) return awj::compat::unexpected{"fixture 已取消"};
    probes.push_back(settings.quality);
    awj::NativeEncodeResult result{.final_quality = settings.quality};
    result.encoded.codec_name = "threshold-fixture";
    result.encoded.bytes.assign(static_cast<std::size_t>(settings.quality) + 16, std::byte{0});
    result.encoded.bytes[0] = std::byte{static_cast<unsigned char>(settings.quality)};
    return result;
  }
};

class ThresholdDecoder final : public awj::ImageDecoder {
 public:
  const awj::ImageBuffer& passing;
  const awj::ImageBuffer& failing;
  const int boundary;
  const awj::compat::stop_token expected_stop;
  awj::compat::stop_source* cancel_during_decode{};
  ThresholdDecoder(const awj::ImageBuffer& pass, const awj::ImageBuffer& fail, int quality,
      awj::compat::stop_token token) : passing(pass), failing(fail), boundary(quality), expected_stop(token) {}
  std::string_view id() const noexcept override { return "threshold-fixture"; }
  bool can_decode(const std::filesystem::path&) const override { return true; }
  awj::compat::expected<awj::ImageDecodeResult, std::string> decode(const std::filesystem::path&) const override {
    return awj::compat::unexpected{"fixture accepts memory only"};
  }
  awj::compat::expected<awj::ImageDecodeResult, std::string> decode_memory(std::span<const std::byte> bytes,
      std::string_view, awj::DecodeOptions options) const override {
    if (options.stop_token != expected_stop) return awj::compat::unexpected{"candidate decode lost stop_token"};
    if (cancel_during_decode) cancel_during_decode->request_stop();
    if (bytes.empty()) return awj::compat::unexpected{"empty fixture candidate"};
    const auto quality = std::to_integer<unsigned>(bytes[0]);
    return awj::ImageDecodeResult{.image = quality >= static_cast<unsigned>(boundary) ? passing : failing};
  }
};

int verify_threshold_selection() {
  constexpr int target = 75;
  const auto reference = threshold_fixture(0);
  auto ref_luma = awj::make_luma_image(reference);
  if (!ref_luma) return fail(ref_luma.error());
  const auto score_image = [&](const awj::ImageBuffer& image) -> awj::compat::expected<double, std::string> {
    auto candidate = awj::make_luma_image(image);
    if (!candidate) return awj::compat::unexpected{candidate.error()};
    auto metrics = awj::calculate_visual_metrics_cpu(*ref_luma, *candidate);
    if (!metrics) return awj::compat::unexpected{metrics.error()};
    return metrics->score.visual_score;
  };
  const auto score_at = [&](int amplitude) { return score_image(threshold_fixture(amplitude)); };
  int passing_amplitude = 0, failing_amplitude = 30000;
  auto worst = score_at(failing_amplitude);
  if (!worst || *worst >= target) return fail("Threshold fixture did not bracket an actual VQ decision.");
  while (passing_amplitude + 1 < failing_amplitude) {
    const int midpoint = passing_amplitude + (failing_amplitude - passing_amplitude) / 2;
    auto score = score_at(midpoint);
    if (!score) return fail(score.error());
    if (*score >= target) passing_amplitude = midpoint;
    else failing_amplitude = midpoint;
  }
  // Refine the adjacent 16-bit amplitudes by replacing a pixel prefix. The
  // final pair differs at one pixel, so this exercises a real near-threshold
  // final search decision, not just two comfortably separated scores.
  const auto lower = threshold_fixture(passing_amplitude), upper = threshold_fixture(failing_amplitude);
  const auto mixed = [&](std::size_t prefix) {
    auto image = lower;
    for (std::size_t i = 0; i < prefix; ++i) {
      const auto offset = (i / image.width) * image.planes[0].stride + (i % image.width) * 2;
      std::memcpy(image.planes[0].bytes.data() + offset, upper.planes[0].bytes.data() + offset, 2);
    }
    return image;
  };
  std::size_t pass_prefix = 0, fail_prefix = reference.width * reference.height;
  while (pass_prefix + 1 < fail_prefix) {
    const auto midpoint = pass_prefix + (fail_prefix - pass_prefix) / 2;
    auto score = score_image(mixed(midpoint));
    if (!score) return fail(score.error());
    if (*score >= target) pass_prefix = midpoint;
    else fail_prefix = midpoint;
  }
  const auto pass = mixed(pass_prefix), reject = mixed(fail_prefix);
  auto pass_score = score_image(pass), fail_score = score_image(reject);
  if (!pass_score || !fail_score || *pass_score < target || *fail_score >= target ||
      *pass_score - target > 1e-3 || target - *fail_score > 1e-3)
    return fail("One-pixel threshold candidates did not bracket VQ=75 within 0.001 points.");
  const auto range = awj::get_quality_search_range(target);
  const int boundary = range.q_min + (range.q_max - range.q_min) / 2;
  awj::compat::stop_source stop;
  ThresholdEncoder encoder;
  ThresholdDecoder decoder(pass, reject, boundary, stop.get_token());
  awj::NativeEncodeSettings settings{.output_format = awj::OutputFormat::webp, .visual_quality = target,
      .resources = {.file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1}};
  auto serial = awj::encode_with_native_visual_quality_search(reference, encoder, decoder, settings,
      "threshold.fixture", stop.get_token());
  if (!serial) return fail(serial.error());
  const auto probes = encoder.probes;
  settings.resources = {.file_parallelism = 2, .encoder_threads_per_file = 7, .global_thread_budget = 8};
  const auto execution = awj::native_visual_search_detail::metric_execution(settings.resources, stop.get_token());
  if (execution.thread_budget != 4 || execution.stop != stop.get_token() || !execution.scratch_limit_bytes)
    return fail("VQ execution did not receive the resolved per-file budget/token.");
  encoder.probes.clear();
  auto parallel = awj::encode_with_native_visual_quality_search(reference, encoder, decoder, settings,
      "threshold.fixture", stop.get_token());
  if (!parallel) return fail(parallel.error());
  if (!serial->target_met || !parallel->target_met || serial->candidate.quality != boundary ||
      serial->candidate.quality != parallel->candidate.quality || encoder.probes != probes ||
      serial->candidate.visual_score != parallel->candidate.visual_score ||
      serial->candidate.raw_gmsd != parallel->candidate.raw_gmsd ||
      serial->candidate.raw_ms_ssim != parallel->candidate.raw_ms_ssim ||
      serial->encode_result.encoded.bytes != parallel->encode_result.encoded.bytes ||
      serial->encode_result.diagnostics.visual_quality_search_trace != parallel->encode_result.diagnostics.visual_quality_search_trace)
    return fail("Threaded VQ changed threshold pass/fail, candidate order, final selection or retained bytes.");

  // RGB cache preparation uses the same N and token without rebuilding it for
  // each candidate. Compare the owned bytes, not only the returned view length.
  auto rgb_source = make_test_image(257, 259);
  settings.output_format = awj::OutputFormat::jxl; settings.applied_alpha = "discarded";
  auto rgb_parallel = awj::native_visual_search_detail::make_rgb8_input_cache(rgb_source, settings);
  settings.resources = {.file_parallelism = 1, .encoder_threads_per_file = 1, .global_thread_budget = 1};
  auto rgb_serial = awj::native_visual_search_detail::make_rgb8_input_cache(rgb_source, settings);
  if (!rgb_serial || !rgb_parallel || *rgb_serial != *rgb_parallel) return fail("Threaded VQ RGB cache changed packed samples.");
  std::vector<std::byte> cache;
  auto view = awj::native_visual_search_detail::make_rgb8_input_view(rgb_source, settings, cache);
  const auto* storage = cache.data();
  auto reused = awj::native_visual_search_detail::make_rgb8_input_view(rgb_source, settings, cache);
  if (!view || !reused || cache.data() != storage || reused->data() != storage || cache != *rgb_serial)
    return fail("VQ RGB cache ownership/reuse changed.");
  decoder.cancel_during_decode = &stop;
  settings.output_format = awj::OutputFormat::webp;
  auto canceled = awj::encode_with_native_visual_quality_search(reference, encoder, decoder, settings,
      "threshold.fixture", stop.get_token());
  if (canceled || canceled.error().find("已取消") == std::string::npos ||
      awj::native_visual_search_detail::make_rgb8_input_cache(rgb_source, settings, stop.get_token()) ||
      awj::native_visual_search_detail::make_rgb8_input_view(rgb_source, settings, cache, stop.get_token()))
    return fail("VQ candidate decode/cache cancellation was not propagated.");
  return 0;
}

}  // namespace

int main() {
  if (const int result = verify_threshold_selection()) return result;
#if AWJ_HAS_JPEGLI
  if (const int result = verify_jpeg_auto_search()) return result;
#endif
  awj::WebPImageEncoder encoder;
  awj::WebPImageDecoder decoder;
  if (const int result = verify_cpu_candidates(encoder, decoder)) return result;
  const auto candidate_path = std::filesystem::temp_directory_path() /
                              "awjimage-native-vq-test.webp";

  auto result = awj::encode_with_native_visual_quality_search(
      make_test_image(), encoder, decoder,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                 .quality = 40,
                                 .visual_quality = 30,
                                 .speed = 10,
                                 .visual_quality_fallback = true,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}},
      candidate_path);
  std::error_code ec;
  std::filesystem::remove(candidate_path, ec);
  if (!result) {
    return fail(result.error());
  }
  if (result->encode_result.encoded.bytes.empty() ||
      result->encode_result.search_attempt_count <= 1 ||
      !result->encode_result.visual_score ||
      result->encode_result.final_quality < 1 ||
      result->encode_result.final_quality > 99) {
    return fail("native visual quality search result invalid.");
  }
  if (result->encode_result.diagnostics.visual_quality_search_trace.find("predicted=q") ==
          std::string::npos ||
      result->encode_result.diagnostics.visual_quality_search_trace.find("selected=") ==
          std::string::npos) {
    return fail("native visual quality search did not record an explanatory trace.");
  }

  auto lossless = awj::encode_with_native_visual_quality_search(
      make_test_image(), encoder, decoder,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                 .quality = 20,
                                 .visual_quality = 100,
                                 .speed = 10,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}},
      candidate_path);
  std::filesystem::remove(candidate_path, ec);
  if (!lossless) {
    return fail(lossless.error());
  }
  if (!lossless->encode_result.lossless || lossless->encode_result.final_quality != 100 ||
      lossless->encode_result.search_attempt_count != 1 ||
      lossless->encode_result.diagnostics.timing.gmsd_seconds != 0.0 ||
      lossless->encode_result.diagnostics.timing.ms_ssim_seconds != 0.0) {
    return fail("native visual quality lossless path invalid.");
  }
  if (lossless->encode_result.diagnostics.visual_quality_search_trace.find("lossless-bypass") ==
      std::string::npos) {
    return fail("native visual quality lossless path did not record trace.");
  }

  return 0;
}
