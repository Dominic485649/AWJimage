#include "../src/compat/stdlib_compat.h"
#include <cstddef>
#include <cstdint>
#include <array>
#include <lcms2.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <expected>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include <string_view>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

import awj.animation;
import awj.codec;
import awj.config;
import awj.encoding_defaults;
import awj.image;
import awj.resource_planner;
import awj.webp_codec;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

struct AnimEncoderDeleter {
  void operator()(WebPAnimEncoder* value) const noexcept {
    if (value != nullptr) WebPAnimEncoderDelete(value);
  }
};

bool add_animation_frame(WebPAnimEncoder* encoder, const std::uint8_t* rgba,
                         int timestamp, const WebPConfig& config) {
  WebPPicture picture{};
  if (!WebPPictureInit(&picture)) return false;
  picture.use_argb = 1;
  picture.width = 2;
  picture.height = 2;
  const bool ok = WebPPictureImportRGBA(&picture, rgba, 8) &&
                  WebPAnimEncoderAdd(encoder, &picture, timestamp, &config);
  WebPPictureFree(&picture);
  return ok;
}

class TestAnimation final : public awj::AnimationReader {
 public:
  awj::AnimationInfo description{.width = 2, .height = 2,
      .durations = {{1, 60}, {1, 60}, {1, 60}}, .repetitions = -1};
  awj::ImageBuffer current{.width = 2, .height = 2, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 8};
  std::size_t index{};
  TestAnimation() {
    awj::ImagePlane plane{.stride = 8}; plane.bytes.resize(16); current.planes.push_back(std::move(plane));
    current.metadata.push_back({awj::MetadataKind::xmp, {std::byte{'x'}, std::byte{'m'}, std::byte{'p'}}});
  }
  const awj::AnimationInfo& info() const noexcept override { return description; }
  const awj::ImageBuffer& frame() const noexcept override { return current; }
  std::uint64_t memory_usage_bytes() const noexcept override { return 256; }
  awj::compat::expected<bool, std::string> next(awj::compat::stop_token stop = {}) override {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (index == description.durations.size()) return false;
    const auto color = index % 3;
    for (std::size_t p = 0; p < 16; p += 4) {
      for (std::size_t c = 0; c < 3; ++c) current.planes[0].bytes[p + c] = c == color ? std::byte{255} : std::byte{0};
      current.planes[0].bytes[p + 3] = index == 1 && p == 0 ? std::byte{0} : std::byte{255};
    }
    current.alpha_mode = index == 0 ? awj::AlphaMode::none : awj::AlphaMode::straight;
    ++index; return true;
  }
};

int animation_encoder_checks() {
  for (const bool strip : {false, true}) for (const int loops : {-1, 2}) {
    TestAnimation reader; reader.description.repetitions = loops;
    if (!reader.next().value_or(false)) return fail("初始化动画测试失败。");
    const awj::NativeEncodeSettings settings{.output_format = awj::OutputFormat::webp, .quality = 100, .speed = 10,
        .alpha_policy = awj::AlphaModePolicy::automatic, .strip_metadata = strip};
    auto encoded = awj::encode_webp_animation(reader, settings);
    if (!encoded || !encoded->lossless) return fail(encoded ? "动画无损语义错误。" : encoded.error());
    const WebPData data{reinterpret_cast<const std::uint8_t*>(encoded->encoded.bytes.data()), encoded->encoded.bytes.size()};
    WebPAnimDecoderOptions options{}; WebPAnimDecoderOptionsInit(&options); options.color_mode = MODE_RGBA;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder{WebPAnimDecoderNew(&data, &options), WebPAnimDecoderDelete};
    WebPAnimInfo info{};
    if (!decoder || !WebPAnimDecoderGetInfo(decoder.get(), &info) || info.frame_count != 3 ||
        info.loop_count != (loops < 0 ? 0u : static_cast<unsigned>(loops + 1))) return fail("动画 WebP 帧或循环封装错误。");
    const std::array expected_ms{17, 33, 50};
    for (std::size_t i = 0; i < 3; ++i) {
      std::uint8_t* frame{}; int timestamp{};
      if (!WebPAnimDecoderGetNext(decoder.get(), &frame, &timestamp) || timestamp != expected_ms[i])
        return fail("动画 WebP 没有按累计毫秒时间线编码。");
      if (frame[3] != (i == 1 ? 0 : 255)) return fail("后续帧透明度被第一帧的 opaque 策略丢弃。");
      if (frame[4 + i] != 255) return fail("动画 WebP 帧颜色或顺序错误。");
    }
    const auto* demux = WebPAnimDecoderGetDemuxer(decoder.get());
    WebPChunkIterator chunk{};
    const bool has_xmp = WebPDemuxGetChunk(demux, "XMP ", 1, &chunk) != 0;
    if (has_xmp) WebPDemuxReleaseChunkIterator(&chunk);
    if (has_xmp == strip) return fail("动画 WebP 元数据剥离策略错误或 mux 丢失了动画。");
  }
  TestAnimation tiny; tiny.description.durations = {{1, 10000}};
  if (!tiny.next().value_or(false)) return fail("亚毫秒测试初始化失败。");
  if (awj::encode_webp_animation(tiny, {})) return fail("不可表示的帧时长没有失败。");
  TestAnimation loop; loop.description.repetitions = 65535;
  if (!loop.next().value_or(false)) return fail("循环边界测试初始化失败。");
  if (awj::encode_webp_animation(loop, {})) return fail("不可表示的循环次数没有失败。");
  TestAnimation budget;
  if (!budget.next().value_or(false)) return fail("预算测试初始化失败。");
  awj::NativeEncodeSettings limited; limited.resources.memory_limit_bytes = 1;
  if (awj::encode_webp_animation(budget, limited)) return fail("动画 WebP 忽略了内存预算。");
  TestAnimation cancelled;
  if (!cancelled.next().value_or(false)) return fail("取消测试初始化失败。");
  awj::compat::stop_source stop; stop.request_stop();
  if (awj::encode_webp_animation(cancelled, {}, stop.get_token())) return fail("动画 WebP 忽略了取消。");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  {
    using namespace awj::encoding_defaults;
    const ScopedMemoryPolicy unlocked{MemoryPolicy{UINT64_MAX, true, true}};
    awj::webp_detail::WebPVectorWriter writer;
    WebPPicture picture{};
    picture.custom_ptr = &writer;
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    {
      const ScopedMemoryPolicy unrelated{MemoryPolicy{1, false, false}};
      if (!awj::webp_detail::write_webp_bytes(bytes.data(), bytes.size(), &picture) ||
          writer.bytes.size() != bytes.size() || writer.byte_limit != UINT64_MAX)
        return fail("WebP C callback consulted the invoking thread rather than its operation snapshot.");
    }
    awj::webp_detail::WebPVectorWriter bounded{.byte_limit = 3};
    picture.custom_ptr = &bounded;
    if (!awj::webp_detail::write_webp_bytes(bytes.data(), 2, &picture) ||
        awj::webp_detail::write_webp_bytes(bytes.data(), 2, &picture) || bounded.bytes.size() != 2 ||
        !bounded.error || awj::webp_detail::write_webp_bytes(bytes.data(), SIZE_MAX, &picture))
      return fail("WebP writer lost its finite cap or accepted overflow before allocation.");
    awj::compat::stop_source cancelled;
    cancelled.request_stop();
    auto token = cancelled.get_token();
    awj::webp_detail::WebPVectorWriter stopped{.stop_token = &token};
    picture.custom_ptr = &stopped;
    if (awj::webp_detail::write_webp_bytes(bytes.data(), bytes.size(), &picture) || !stopped.error)
      return fail("WebP full unlock disabled cancellation.");
  }
  if (argc == 2 && std::string_view{argv[1]} == "--memory-policy-only") return 0;
  if (const auto result = animation_encoder_checks()) return result;
  awj::ImagePlane plane{.stride = 8};
  plane.bytes = {
      std::byte{255}, std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{0},   std::byte{255}, std::byte{255},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
  };
  awj::ImageBuffer image{.width = 2,
                          .height = 2,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));

  awj::WebPImageEncoder encoder;
  auto encoded = encoder.encode(
      image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                 .quality = 100,
                                 .speed = 10,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!encoded) {
    return fail(encoded.error());
  }
  if (!encoded->lossless || encoded->encoded.bytes.empty()) {
    return fail("WebP lossless encode result invalid.");
  }

  const auto path = std::filesystem::temp_directory_path() /
                    "awjimage-webp-codec-test.webp";
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  }

  awj::WebPImageDecoder decoder;
  auto decoded = decoder.decode(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!decoded) {
    return fail(decoded.error());
  }
  if (decoded->image.width != 2 || decoded->image.height != 2 ||
      decoded->image.pixel_format != awj::PixelFormat::rgba ||
      decoded->image.planes.empty()) {
    return fail("WebP decode result invalid.");
  }

  WebPAnimEncoderOptions anim_options{};
  WebPConfig anim_config{};
  if (!WebPAnimEncoderOptionsInit(&anim_options) ||
      !WebPConfigInit(&anim_config)) {
    return fail("WebP animation test initialization failed.");
  }
  anim_config.lossless = 1;
  anim_config.quality = 100.0f;
  std::unique_ptr<WebPAnimEncoder, AnimEncoderDeleter> anim{
      WebPAnimEncoderNew(2, 2, &anim_options)};
  const std::uint8_t red[16]{255, 0, 0, 255, 255, 0, 0, 255,
                             255, 0, 0, 255, 255, 0, 0, 255};
  const std::uint8_t blue[16]{0, 0, 255, 255, 0, 0, 255, 255,
                              0, 0, 255, 255, 0, 0, 255, 255};
  WebPData animation{};
  if (!anim || !add_animation_frame(anim.get(), red, 0, anim_config) ||
      !add_animation_frame(anim.get(), blue, 100, anim_config) ||
      !WebPAnimEncoderAdd(anim.get(), nullptr, 200, nullptr) ||
      !WebPAnimEncoderAssemble(anim.get(), &animation)) {
    return fail("WebP animation test encoding failed.");
  }
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(animation.bytes),
                 static_cast<std::streamsize>(animation.size));
  }
  WebPDataClear(&animation);
  decoded = decoder.decode(path);
  std::filesystem::remove(path, ec);
  if (!decoded || decoded->image.planes.empty() ||
      decoded->image.planes[0].bytes.size() < 4 ||
      decoded->image.planes[0].bytes[0] != std::byte{255} ||
      decoded->image.planes[0].bytes[1] != std::byte{0} ||
      decoded->image.planes[0].bytes[2] != std::byte{0}) {
    return fail(decoded ? "WebP animation did not flatten to its first frame."
                        : decoded.error());
  }

  // 选择源 SDR 后仍须保存其真实色彩解释，strip 不能把线性或广色域样本假作 sRGB。
  for (const bool strip : {false, true}) {
    awj::ImageBuffer linear{.width = 1, .height = 1, .pixel_format = awj::PixelFormat::rgba,
        .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 8,
            .color_primaries = 1, .transfer_characteristics = 8, .matrix_coefficients = 0,
            .color_range = 1, .source_has_gain_map = true},
        .planes = {{{std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}}, 4}}};
    awj::NativeEncodeSettings settings{.output_format = awj::OutputFormat::webp, .quality = 100, .speed = 10,
        .strip_metadata = strip};
    auto result = encoder.encode(linear, settings);
    if (!result) return fail(result.error());
    auto restored = decoder.decode_memory(result->encoded.bytes, "existing SDR color", {.copy_metadata_payloads = true});
    if (!restored) return fail(restored.error());
    const awj::MetadataBlock* icc = nullptr;
    for (const auto& block : restored->image.metadata) if (block.kind == awj::MetadataKind::icc) icc = &block;
    if (!icc || restored->image.planes[0].bytes[0] != std::byte{128})
      return fail("WebP changed the existing SDR pixels or discarded their essential ICC");
    std::unique_ptr<void, decltype(&cmsCloseProfile)> input_profile{
        cmsOpenProfileFromMem(icc->bytes.data(), static_cast<cmsUInt32Number>(icc->bytes.size())), &cmsCloseProfile};
    std::unique_ptr<void, decltype(&cmsCloseProfile)> output_profile{cmsCreate_sRGBProfile(), &cmsCloseProfile};
    if (!input_profile || !output_profile) return fail("WebP essential SDR ICC cannot be read");
    std::unique_ptr<void, decltype(&cmsDeleteTransform)> transform{cmsCreateTransform(input_profile.get(), TYPE_RGB_8,
        output_profile.get(), TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC, 0), &cmsDeleteTransform};
    if (!transform) return fail("WebP essential SDR ICC cannot be color-converted");
    const std::array<unsigned char, 3> source{128, 128, 128};
    std::array<unsigned char, 3> output{};
    cmsDoTransform(transform.get(), source.data(), output.data(), 1);
    if (output[0] < 180 || output[0] > 194 || output[1] != output[0] || output[2] != output[0])
      return fail("WebP relabeled linear SDR as sRGB instead of preserving its true transfer");
  }
  return 0;
}
