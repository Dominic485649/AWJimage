#include <cmath>
#include <cstddef>
#include <optional>
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
      .resources = awj::ResourcePlan{.file_parallelism = 1,
                                    .encoder_threads_per_file = 1,
                                    .global_thread_budget = 1}};
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

}  // namespace

int main() {
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
