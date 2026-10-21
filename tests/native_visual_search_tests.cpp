#include <cstddef>
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
import awj.webp_codec;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

awj::ImageBuffer make_test_image() {
  awj::ImagePlane plane{.stride = 16};
  plane.bytes.resize(4 * 4 * 4);
  for (std::size_t y = 0; y < 4; ++y) {
    for (std::size_t x = 0; x < 4; ++x) {
      const auto offset = y * plane.stride + x * 4;
      plane.bytes[offset + 0] = std::byte{static_cast<unsigned char>(x * 60)};
      plane.bytes[offset + 1] = std::byte{static_cast<unsigned char>(y * 60)};
      plane.bytes[offset + 2] = std::byte{static_cast<unsigned char>((x + y) * 30)};
      plane.bytes[offset + 3] = std::byte{255};
    }
  }
  awj::ImageBuffer image{.width = 4,
                          .height = 4,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
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
            .strip_metadata = true, .visual_quality_fallback = true, .visual_quality_gpu = false,
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
      lossless->encode_result.search_attempt_count != 1) {
    return fail("native visual quality lossless path invalid.");
  }
  if (lossless->encode_result.diagnostics.visual_quality_search_trace.find("lossless-bypass") ==
      std::string::npos) {
    return fail("native visual quality lossless path did not record trace.");
  }

  return 0;
}
