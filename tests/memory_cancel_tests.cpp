#include "../src/compat/stdlib_compat.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <utility>

import awj.codec;
import awj.config;
import awj.encoding_defaults;
import awj.image;
import awj.jxl_codec;
import awj.native_visual_search;
import awj.raw_image_io;
import awj.resource_planner;
import awj.webp_codec;

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

bool contains_any(std::string_view text,
                  std::initializer_list<std::string_view> needles) {
  for (const auto needle : needles) {
    if (text.find(needle) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

awj::ImageBuffer make_test_image(std::size_t width = 4, std::size_t height = 4) {
  awj::ImagePlane plane{.stride = width * 4};
  plane.bytes.resize(width * height * 4);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto offset = y * plane.stride + x * 4;
      plane.bytes[offset + 0] = std::byte{static_cast<unsigned char>(x * 40)};
      plane.bytes[offset + 1] = std::byte{static_cast<unsigned char>(y * 40)};
      plane.bytes[offset + 2] = std::byte{static_cast<unsigned char>((x + y) * 20)};
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

}  // namespace

int main() {
  awj::JXLImageEncoder jxl;
  const auto default_speed = awj::default_speed_for(awj::OutputFormat::jxl);
  auto encoded = jxl.encode(
      make_test_image(2, 2),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                .quality = 90,
                                .speed = default_speed,
                                .resources = awj::ResourcePlan{
                                    .file_parallelism = 1,
                                    .encoder_threads_per_file = 1,
                                    .global_thread_budget = 1}});
  if (!encoded) {
    return fail(encoded.error());
  }
  if (encoded->diagnostics.speed_mapping.codec_value != awj::encoding_defaults::default_jxl_effort) {
    return fail("JXL default speed should report default effort 7.");
  }

  auto malformed = make_test_image(2, 2);
  malformed.width = std::numeric_limits<std::size_t>::max() / 2 + 1;
  auto rejected = jxl.encode(
      malformed,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                .quality = 90,
                                .speed = default_speed,
                                .resources = awj::ResourcePlan{
                                    .file_parallelism = 1,
                                    .encoder_threads_per_file = 1,
                                    .global_thread_budget = 1}});
  if (rejected || rejected.error().find("过大") == std::string::npos) {
    return fail("JXL malformed huge dimensions were not rejected before multiplication.");
  }

  const auto raw_path = std::filesystem::temp_directory_path() / "awjimage-raw-malformed.awsraw";
  {
    std::ofstream raw{raw_path, std::ios::binary};
    char magic[8] = {'A', 'W', 'S', 'R', 'A', 'W', '1', '\0'};
    raw.write(magic, sizeof(magic));
    const std::uint32_t width = 1;
    const std::uint32_t height = 2;
    const std::uint32_t pixel_format = 1;
    const std::uint32_t alpha_mode = 1;
    const std::uint32_t bit_depth = 8;
    const std::uint32_t stride = std::numeric_limits<std::uint32_t>::max();
    const std::uint64_t byte_count = std::numeric_limits<std::uint64_t>::max();
    raw.write(reinterpret_cast<const char*>(&width), sizeof(width));
    raw.write(reinterpret_cast<const char*>(&height), sizeof(height));
    raw.write(reinterpret_cast<const char*>(&pixel_format), sizeof(pixel_format));
    raw.write(reinterpret_cast<const char*>(&alpha_mode), sizeof(alpha_mode));
    raw.write(reinterpret_cast<const char*>(&bit_depth), sizeof(bit_depth));
    raw.write(reinterpret_cast<const char*>(&stride), sizeof(stride));
    raw.write(reinterpret_cast<const char*>(&byte_count), sizeof(byte_count));
  }
  auto raw = awj::read_raw_image_file(raw_path);
  std::error_code ec;
  std::filesystem::remove(raw_path, ec);
  if (raw || !contains_any(raw.error(), {"invalid", "无效"})) {
    return fail("malformed raw dimensions were not rejected.");
  }

  awj::WebPImageEncoder webp;
  awj::WebPImageDecoder decoder;
  const auto candidate_path = std::filesystem::temp_directory_path() / "awjimage-vq-cancel.webp";
  awj::compat::stop_source stop_source;
  stop_source.request_stop();
  auto canceled = awj::encode_with_native_visual_quality_search(
      make_test_image(), webp, decoder,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                .quality = 40,
                                .visual_quality = 80,
                                .speed = 10,
                                .visual_quality_fallback = true,
                                .resources = awj::ResourcePlan{
                                    .file_parallelism = 1,
                                    .encoder_threads_per_file = 1,
                                    .global_thread_budget = 1}},
      candidate_path, stop_source.get_token());
  std::filesystem::remove(candidate_path, ec);
  if (canceled || canceled.error().find("取消") == std::string::npos) {
    return fail("visual quality search did not honor a pre-canceled stop token.");
  }

  return 0;
}
