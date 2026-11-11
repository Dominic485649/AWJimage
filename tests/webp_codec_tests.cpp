#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <array>
#include <concepts>
#include <lcms2.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
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
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.image;
import awj.resource_planner;
import awj.webp_codec;

namespace {

static_assert(std::constructible_from<awj::WebPImageDecoder, int>);

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

bool same_decoded_image(const awj::ImageBuffer& a, const awj::ImageBuffer& b) {
  if (a.width != b.width || a.height != b.height || a.pixel_format != b.pixel_format ||
      a.alpha_mode != b.alpha_mode || a.bit_depth != b.bit_depth ||
      a.planes.size() != b.planes.size() || a.metadata.size() != b.metadata.size() ||
      a.source_info.has_value() != b.source_info.has_value()) return false;
  if (a.source_info && (a.source_info->pixel_format != b.source_info->pixel_format ||
      a.source_info->bit_depth != b.source_info->bit_depth ||
      a.source_info->color_primaries != b.source_info->color_primaries ||
      a.source_info->transfer_characteristics != b.source_info->transfer_characteristics ||
      a.source_info->matrix_coefficients != b.source_info->matrix_coefficients ||
      a.source_info->color_range != b.source_info->color_range)) return false;
  for (std::size_t c = 0; c < a.planes.size(); ++c)
    if (a.planes[c].stride != b.planes[c].stride || a.planes[c].bytes != b.planes[c].bytes) return false;
  for (std::size_t c = 0; c < a.metadata.size(); ++c)
    if (a.metadata[c].kind != b.metadata[c].kind || a.metadata[c].bytes != b.metadata[c].bytes) return false;
  return true;
}

std::uint8_t budget_fixture_alpha(std::size_t x, std::size_t y) {
  const auto index = (x / 8 + y / 4) % 3;
  return index == 0 ? 0 : index == 1 ? 128 : 255;
}

awj::compat::expected<std::vector<std::byte>, std::string> make_budget_webp(
    bool lossless, bool alpha, int frame_index = 0) {
  // Odd chroma edges and enough macroblock columns/rows for libwebp's optional
  // row worker. A configured bool is tested separately; no N-worker claim.
  constexpr int width = 513, height = 131;
  std::vector<std::uint8_t> rgba(width * height * 4);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    const auto p = static_cast<std::size_t>((y * width + x) * 4);
    rgba[p] = static_cast<std::uint8_t>(x * 17 + y * 7 + frame_index * 53);
    rgba[p + 1] = static_cast<std::uint8_t>(x * 3 + y * 29 + frame_index * 19);
    rgba[p + 2] = static_cast<std::uint8_t>(x * y + frame_index * 97);
    rgba[p + 3] = alpha ? budget_fixture_alpha(x, y) : 255;
  }
  struct Encoding {
    WebPPicture picture{};
    WebPMemoryWriter writer{};
    ~Encoding() { WebPPictureFree(&picture); WebPMemoryWriterClear(&writer); }
  } output;
  WebPConfig config{};
  if (!WebPConfigInit(&config) || !WebPPictureInit(&output.picture))
    return awj::compat::unexpected{"WebP budget fixture initialization failed."};
  config.lossless = lossless ? 1 : 0;
  config.quality = lossless ? 100.0f : 83.0f;
  config.method = 0; config.exact = 1; config.alpha_quality = 100;
  output.picture.use_argb = 1; output.picture.width = width; output.picture.height = height;
  WebPMemoryWriterInit(&output.writer);
  output.picture.writer = WebPMemoryWrite; output.picture.custom_ptr = &output.writer;
  if (!WebPPictureImportRGBA(&output.picture, rgba.data(), width * 4) ||
      !WebPEncode(&config, &output.picture))
    return awj::compat::unexpected{"WebP budget fixture encoding failed."};
  const auto* begin = reinterpret_cast<const std::byte*>(output.writer.mem);
  return std::vector<std::byte>{begin, begin + output.writer.size};
}

awj::compat::expected<std::vector<std::byte>, std::string> mux_budget_webp(
    const std::vector<std::byte>& first, const std::vector<std::byte>* second = nullptr) {
  const WebPData input{reinterpret_cast<const std::uint8_t*>(first.data()), first.size()};
  std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)> mux{
      second ? WebPMuxNew() : WebPMuxCreate(&input, 1), WebPMuxDelete};
  if (!mux) return awj::compat::unexpected{"WebP budget mux creation failed."};
  if (second) {
    const WebPMuxAnimParams parameters{0, 4};
    if (WebPMuxSetCanvasSize(mux.get(), 513, 131) != WEBP_MUX_OK ||
        WebPMuxSetAnimationParams(mux.get(), &parameters) != WEBP_MUX_OK)
      return awj::compat::unexpected{"WebP budget animation parameters failed."};
    for (int i = 0; i < 2; ++i) {
      const auto& bytes = i ? *second : first;
      WebPMuxFrameInfo frame{};
      frame.bitstream = {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
      frame.id = WEBP_CHUNK_ANMF; frame.duration = i ? 37 : 17;
      frame.blend_method = WEBP_MUX_NO_BLEND;
      frame.dispose_method = WEBP_MUX_DISPOSE_NONE;
      if (WebPMuxPushFrame(mux.get(), &frame, 1) != WEBP_MUX_OK)
        return awj::compat::unexpected{"WebP budget animation frame mux failed."};
    }
  }
  constexpr std::array<std::uint8_t, 9> xmp{'<','b','u','d','g','e','t','/','>'};
  const WebPData metadata{xmp.data(), xmp.size()};
  if (WebPMuxSetChunk(mux.get(), "XMP ", &metadata, 1) != WEBP_MUX_OK)
    return awj::compat::unexpected{"WebP budget metadata mux failed."};
  struct Data { WebPData value{}; ~Data() { WebPDataClear(&value); } } assembled;
  if (WebPMuxAssemble(mux.get(), &assembled.value) != WEBP_MUX_OK)
    return awj::compat::unexpected{"WebP budget mux assemble failed."};
  const auto* begin = reinterpret_cast<const std::byte*>(assembled.value.bytes);
  return std::vector<std::byte>{begin, begin + assembled.value.size};
}

int thread_budget_decoder_checks(const std::filesystem::path& test_root) {
  for (const int threads : {std::numeric_limits<int>::min(), 0, 1, 2, 8, 128,
                            std::numeric_limits<int>::max()}) {
    WebPDecoderOptions still{}; WebPAnimDecoderOptions animation{};
    still.use_threads = animation.use_threads = 7; // Also prove the serial reset.
    awj::webp_detail::apply_decode_thread_budget(still, threads);
    awj::webp_detail::apply_decode_thread_budget(animation, threads);
    if (still.use_threads != (threads > 1 ? 1 : 0) || animation.use_threads != still.use_threads)
      return fail("WebP's boolean thread capability did not follow the resolved per-file share.");
  }
  if (awj::webp_detail::decode_thread_workspace_bytes(1, 513) != 0 ||
      awj::webp_detail::decode_thread_workspace_bytes(2, 513) == 0 ||
      awj::webp_detail::decode_thread_workspace_bytes(2, 513) !=
          awj::webp_detail::decode_thread_workspace_bytes(128, 513))
    return fail("WebP worker reservation confused a boolean capability with N workers.");

  struct Directory {
    std::filesystem::path path;
    bool owned{};
    ~Directory() { if (owned) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
  } scratch{test_root / ("webp-budget-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
  std::filesystem::create_directories(test_root);
  scratch.owned = std::filesystem::create_directory(scratch.path);
  if (!scratch.owned) return fail("WebP budget fixture directory already exists.");
  const auto path = scratch.path / "input.webp";
  awj::compat::stop_source cancelled; cancelled.request_stop();
  for (const auto sample : {std::array{true, false}, std::array{true, true},
                            std::array{false, false}, std::array{false, true}}) {
    auto raw = make_budget_webp(sample[0], sample[1]);
    if (!raw) return fail(raw.error());
    auto bytes = mux_budget_webp(*raw);
    if (!bytes) return fail(bytes.error());
    { std::ofstream file(path, std::ios::binary);
      file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
      if (!file) return fail("WebP budget fixture write failed."); }
    std::array<std::optional<awj::ImageBuffer>, 2> baseline;
    for (const int threads : {1, 2, 8}) for (const bool native : {false, true}) {
      awj::WebPImageDecoder decoder{threads};
      auto decoded = decoder.decode_memory(*bytes, "WebP budget fixture",
          {.copy_metadata_payloads = true, .native_pixels = native});
      if (!decoded) return fail(decoded.error());
      const auto& image = decoded->image;
      if (image.width != 513 || image.height != 131 || image.bit_depth != 8 ||
          image.alpha_mode != (sample[1] ? awj::AlphaMode::straight : awj::AlphaMode::none) ||
          image.metadata.size() != 1 || image.metadata[0].kind != awj::MetadataKind::xmp)
        return fail("WebP budget decode lost dimensions, transparency or metadata.");
      if (native && !sample[0]) {
        if (image.pixel_format != awj::PixelFormat::yuv420 || image.planes.size() != (sample[1] ? 4u : 3u) ||
            !image.source_info || image.source_info->matrix_coefficients != 6 || image.source_info->color_range != 0)
          return fail("Lossy WebP did not use native YUV/YUVA under its thread budget.");
        for (std::size_t c = 0; c < image.planes.size(); ++c) {
          const auto width = c == 1 || c == 2 ? 257u : 513u;
          const auto height = c == 1 || c == 2 ? 66u : 131u;
          if (image.planes[c].stride != width || image.planes[c].bytes.size() != width * height)
            return fail("Odd-size WebP YUV/YUVA plane rounding changed across budgets.");
        }
      } else if (image.pixel_format != awj::PixelFormat::rgba || image.planes.size() != 1 ||
          image.planes[0].stride != 513 * 4 || image.planes[0].bytes.size() != 513 * 131 * 4)
        return fail("WebP RGBA/lossless layout changed across budgets.");
      if (sample[1]) for (std::size_t y = 0; y < 131; ++y) for (std::size_t x = 0; x < 513; ++x) {
        const auto alpha = native && !sample[0] ? image.planes[3].bytes[y * 513 + x]
            : image.planes[0].bytes[(y * 513 + x) * 4 + 3];
        if (std::to_integer<std::uint8_t>(alpha) != budget_fixture_alpha(x, y))
          return fail("WebP transparent/partial/opaque alpha samples changed.");
      }
      if (!baseline[native]) baseline[native] = image;
      else if (!same_decoded_image(*baseline[native], image)) return fail("WebP CPU budget changed decoded samples/metadata.");
      auto selected = awj::select_decoder_for_path(path, {.allow_wic_fallback = false, .decode_threads = threads});
      if (!selected || !selected->decoder || selected->decoder->id() != "libwebp" || selected->fallback)
        return fail("WebP registry selection lost the budget-aware native decoder.");
      auto registered = awj::decode_image_for_path(path,
          {.allow_wic_fallback = false, .decode_threads = threads, .native_pixels = native});
      if (!registered || !same_decoded_image(image, registered->image))
        return fail(registered ? "WebP registry decode disagreed with direct budget decode." : registered.error());
      auto no_metadata = decoder.decode(path, {.copy_metadata_payloads = false, .native_pixels = native});
      if (!no_metadata || !no_metadata->image.metadata.empty()) return fail("WebP file decode ignored explicit metadata suppression.");
      if (decoder.decode_memory(*bytes, "cancelled WebP", {.native_pixels = native, .stop_token = cancelled.get_token()}) ||
          decoder.decode(path, {.native_pixels = native, .stop_token = cancelled.get_token()}))
        return fail("WebP CPU budget disabled boundary cancellation.");
      auto truncated = *raw; truncated.resize(truncated.size() / 2);
      WebPBitstreamFeatures features{};
      if (WebPGetFeatures(reinterpret_cast<const std::uint8_t*>(truncated.data()), truncated.size(), &features) != VP8_STATUS_OK)
        return fail("Truncated WebP fixture did not reach the owned decode-buffer path.");
      for (int repeat = 0; repeat < 3; ++repeat) {
        if (decoder.decode_memory(truncated, "truncated WebP", {.native_pixels = native}) ||
            decoder.decode_memory({}, "empty WebP", {.native_pixels = native}))
          return fail("WebP accepted damaged input under a thread budget.");
        auto recovered = decoder.decode_memory(*bytes, "valid after failure",
            {.copy_metadata_payloads = true, .native_pixels = native});
        if (!recovered || !same_decoded_image(image, recovered->image))
          return fail("WebP failure/cleanup poisoned a later decode.");
      }
    }
  }
  auto first = make_budget_webp(false, true), second = make_budget_webp(false, true, 1);
  if (!first || !second) return fail("WebP animation budget fixture encoding failed.");
  auto animation = mux_budget_webp(*first, &*second);
  if (!animation) return fail(animation.error());
  auto expected = awj::WebPImageDecoder{1}.decode_memory(*first, "first standalone WebP frame");
  if (!expected) return fail(expected.error());
  { std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(animation->data()), static_cast<std::streamsize>(animation->size()));
    if (!file) return fail("Animated WebP registry fixture write failed."); }
  std::optional<awj::ImageBuffer> flattened;
  for (const int threads : {1, 2, 8}) {
    auto decoded = awj::WebPImageDecoder{threads}.decode_memory(*animation, "large animated WebP first frame",
        {.copy_metadata_payloads = true});
    if (!decoded || decoded->image.width != 513 || decoded->image.height != 131 ||
        decoded->image.pixel_format != awj::PixelFormat::rgba || decoded->image.planes.size() != 1 ||
        decoded->image.planes[0].bytes != expected->image.planes[0].bytes)
      return fail(decoded ? "Animated WebP did not flatten to its large lossy first frame." : decoded.error());
    if (!flattened) flattened = decoded->image;
    else if (!same_decoded_image(*flattened, decoded->image))
      return fail("Animated WebP first-frame samples/metadata changed across budgets.");
    for (const bool native : {false, true}) {
      auto registered = awj::decode_image_for_path(path,
          {.allow_wic_fallback = false, .decode_threads = threads, .native_pixels = native});
      if (!registered || !same_decoded_image(decoded->image, registered->image))
        return fail(registered ? "WebP registry did not retain first-frame flattening under a CPU share." : registered.error());
    }
  }
  return 0;
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
  const bool thread_budget_only = argc >= 2 && std::string_view{argv[1]} == "--thread-budget-only";
  const auto test_root = thread_budget_only && argc >= 3 ? std::filesystem::path{argv[2]}
      : std::filesystem::current_path() / "results" / "codec-thread-budget";
  if (const auto result = thread_budget_decoder_checks(test_root)) return result;
  if (thread_budget_only) return 0;
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
