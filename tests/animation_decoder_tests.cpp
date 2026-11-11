#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <gif_lib.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <zlib.h>
import awj.animation_decoder;
import awj.codec;
import awj.config;
import awj.image;
import awj.png_codec;
import awj.webp_codec;

namespace {
using Bytes = std::vector<std::byte>;
void require(bool value, const char* message) { if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); } }
void write(const std::filesystem::path& path, const Bytes& bytes) {
  std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  require(bool(file), "fixture write failed");
}
void be32(Bytes& bytes, std::uint32_t value) { for (int i = 3; i >= 0; --i) bytes.push_back(std::byte((value >> (i * 8)) & 255)); }
std::uint32_t read32(const std::byte* p) { std::uint32_t n{}; for (int i = 0; i < 4; ++i) n = n * 256 + std::to_integer<unsigned>(p[i]); return n; }
void chunk(Bytes& bytes, const char* tag, const Bytes& data) {
  be32(bytes, static_cast<std::uint32_t>(data.size()));
  const auto begin = bytes.size();
  for (int i = 0; i < 4; ++i) bytes.push_back(std::byte(tag[i]));
  bytes.insert(bytes.end(), data.begin(), data.end());
  be32(bytes, crc32(0, reinterpret_cast<const Bytef*>(bytes.data() + begin), data.size() + 4));
}
std::size_t chunk_offset(const Bytes& bytes, std::string_view tag) {
  for (std::size_t pos = 8; pos + 12 <= bytes.size();) {
    const auto size = read32(bytes.data() + pos);
    if (size > bytes.size() - pos - 12) break;
    if (std::memcmp(bytes.data() + pos + 4, tag.data(), 4) == 0) return pos;
    pos += size + 12;
  }
  require(false, "fixture chunk missing"); return bytes.size();
}
void set32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = std::byte((value >> (24 - i * 8)) & 255);
}
void patch_u32(Bytes& bytes, std::string_view tag, std::size_t field, std::uint32_t value) {
  const auto pos = chunk_offset(bytes, tag), size = static_cast<std::size_t>(read32(bytes.data() + pos));
  require(field + 4 <= size, "fixture patch out of range");
  set32(bytes, pos + 8 + field, value);
  set32(bytes, pos + 8 + size, crc32(0, reinterpret_cast<const Bytef*>(bytes.data() + pos + 4), size + 4));
}
std::uint16_t sample(const awj::ImageBuffer& image, std::size_t index) {
  const auto* data = image.planes[0].bytes.data() + index * (image.bit_depth / 8);
  if (image.bit_depth == 8) return std::to_integer<std::uint8_t>(*data);
  std::uint16_t value{}; std::memcpy(&value, data, sizeof(value)); return value;
}
void reject_apng(const std::filesystem::path& root, const Bytes& bytes) {
  const auto path = root / "rejected.png"; write(path, bytes);
  require(!awj::png_detail::contains_animation_control_chunk(bytes, "invalid APNG"), "memory APNG probe accepted malformed chunks");
  require(!awj::input_uses_animation_reader(path), "file APNG probe accepted malformed chunks");
  require(!awj::open_animation(path, 0), "unlimited APNG open accepted malformed chunks");
  require(!awj::open_animation(path, 128 * 1024 * 1024), "bounded APNG open accepted malformed chunks");
}
awj::ImageBuffer solid(std::size_t width, unsigned r, unsigned g, unsigned b, unsigned alpha = 255) {
  awj::ImageBuffer image{.width = width, .height = 1, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .planes = {{Bytes(width * 4), width * 4}}};
  for (std::size_t x = 0; x < width; ++x) {
    const std::byte rgba[]{std::byte(r), std::byte(g), std::byte(b), std::byte(alpha)};
    std::memcpy(image.planes[0].bytes.data() + x * 4, rgba, 4);
  }
  return image;
}
Bytes png(awj::ImageBuffer image, bool deep = false) {
  if (deep) {
    Bytes samples; samples.reserve(image.planes[0].bytes.size() * 2);
    for (auto value : image.planes[0].bytes) { samples.push_back(value); samples.push_back(value); }
    image.bit_depth = 16; image.planes[0].stride *= 2; image.planes[0].bytes = std::move(samples);
  }
  awj::PngImageEncoder encoder;
  awj::NativeEncodeSettings options{.output_format = awj::OutputFormat::png,
      .quality = 100, .bit_depth = image.bit_depth};
  auto output = encoder.encode(image, options); require(bool(output), "PNG fixture encoding failed");
  require(output->encoded.bytes.size() >= 33 &&
      std::to_integer<int>(output->encoded.bytes[24]) == (deep ? 16 : 8),
      "PNG fixture storage depth changed");
  require(output->encoded.bytes.size() >= 12 && read32(output->encoded.bytes.data() + output->encoded.bytes.size() - 4) == 0xae426082,
      "PNG empty IEND chunk CRC failed");
  return output->encoded.bytes;
}
Bytes apng(bool poster = false, bool deep = false, unsigned count = 4, bool zero_delay = false) {
  auto first = png(solid(3, 255, 0, 0), deep);
  Bytes result(first.begin(), first.begin() + 33); // signature + IHDR
  Bytes control; be32(control, count); be32(control, 3); chunk(result, "acTL", control);
  if (poster) {
    const auto independent = png(solid(3, 0, 0, 255), deep);
    for (std::size_t pos = 8; pos + 12 <= independent.size();) {
      const auto size = read32(independent.data() + pos);
      if (std::memcmp(independent.data() + pos + 4, "IDAT", 4) == 0) {
        const auto start = independent.begin() + pos + 8;
        if (count > 1 && size > 1) {
          chunk(result, "IDAT", Bytes(start, start + size / 2));
          chunk(result, "IDAT", Bytes(start + size / 2, start + size));
        } else chunk(result, "IDAT", Bytes(start, start + size));
      }
      pos += size + 12;
    }
  }
  std::uint32_t sequence{};
  for (unsigned index = 0; index < count; ++index) {
    Bytes frame; be32(frame, sequence++); be32(frame, index ? 1 : 3); be32(frame, 1);
    be32(frame, index ? index - 1 : 0); be32(frame, 0);
    frame.insert(frame.end(), {std::byte{0}, std::byte(zero_delay ? 0 : index + 1), std::byte{0}, std::byte{100}});
    frame.push_back(std::byte(index == 1 ? 1 : index == 2 ? 2 : 0));
    frame.push_back(std::byte(index == 1 ? 1 : 0));
    chunk(result, "fcTL", frame);
    auto encoded = index == 0 ? first : png(solid(1, index == 3 ? 255 : 0, index == 1 || index == 3 ? 255 : 0, index == 2 ? 255 : 0, index == 1 ? 128 : 255), deep);
    for (std::size_t pos = 8; pos + 12 <= encoded.size();) {
      const auto size = read32(encoded.data() + pos);
      if (std::memcmp(encoded.data() + pos + 4, "IDAT", 4) == 0) {
        Bytes data;
        if (index || poster) be32(data, sequence++);
        data.insert(data.end(), encoded.begin() + pos + 8, encoded.begin() + pos + 8 + size);
        chunk(result, index || poster ? "fdAT" : "IDAT", data);
      }
      pos += size + 12;
    }
  }
  chunk(result, "IEND", {}); return result;
}
void gif(const std::filesystem::path& path) {
  int error{}; auto* file = EGifOpenFileName(path.string().c_str(), false, &error);
  require(file != nullptr, "GIF open failed");
  const GifColorType colors[8]{{0,0,0},{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,0,0},{0,0,0},{0,0,0}};
  auto* palette = GifMakeMapObject(8, colors);
  require(EGifPutScreenDesc(file, 3, 1, 3, 0, palette) == GIF_OK, "GIF screen failed");
  GifFreeMapObject(palette);
  for (int index = 0; index < 4; ++index) {
    GraphicsControlBlock control{index == 1 ? 2 : index == 2 ? 3 : 1, false, index + 1, NO_TRANSPARENT_COLOR};
    GifByteType extension[4]{}; EGifGCBToExtension(&control, extension);
    require(EGifPutExtension(file, GRAPHICS_EXT_FUNC_CODE, 4, extension) == GIF_OK, "GIF GCE failed");
    require(EGifPutImageDesc(file, index ? index - 1 : 0, 0, index ? 1 : 3, 1, false, nullptr) == GIF_OK, "GIF frame failed");
    GifPixelType row[3]{static_cast<GifPixelType>(index + 1),static_cast<GifPixelType>(index + 1),static_cast<GifPixelType>(index + 1)};
    require(EGifPutLine(file, row, index ? 1 : 3) == GIF_OK, "GIF pixels failed");
  }
  require(EGifCloseFile(file, &error) == GIF_OK, "GIF close failed");
}
void webp(const std::filesystem::path& path) {
  auto* mux = WebPMuxNew(); require(mux != nullptr, "WebP mux failed");
  WebPMuxSetCanvasSize(mux, 3, 1);
  const WebPMuxAnimParams params{0, 3}; WebPMuxSetAnimationParams(mux, &params);
  for (int index = 0; index < 4; ++index) {
    auto image = solid(index ? 1 : 3, index == 0 || index == 3 ? 255 : 0, index == 1 || index == 3 ? 255 : 0, index == 2 ? 255 : 0);
    std::uint8_t* data{};
    const auto size = WebPEncodeLosslessRGBA(reinterpret_cast<const std::uint8_t*>(image.planes[0].bytes.data()), static_cast<int>(image.width), 1, static_cast<int>(image.width * 4), &data);
    require(size != 0, "WebP encode failed");
    WebPMuxFrameInfo frame{}; frame.bitstream = {data, size}; frame.x_offset = index == 3 ? 2 : 0;
    frame.duration = (index + 1) * 10; frame.id = WEBP_CHUNK_ANMF;
    frame.dispose_method = index == 1 ? WEBP_MUX_DISPOSE_BACKGROUND : WEBP_MUX_DISPOSE_NONE;
    frame.blend_method = index == 1 ? WEBP_MUX_BLEND : WEBP_MUX_NO_BLEND;
    require(WebPMuxPushFrame(mux, &frame, 1) == WEBP_MUX_OK, "WebP frame failed"); WebPFree(data);
  }
  WebPData output{}; require(WebPMuxAssemble(mux, &output) == WEBP_MUX_OK, "WebP assemble failed");
  const auto* p = reinterpret_cast<const std::byte*>(output.bytes); write(path, {p, p + output.size});
  WebPDataClear(&output); WebPMuxDelete(mux);
}

Bytes large_lossy_webp() {
  std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)> mux{WebPMuxNew(), WebPMuxDelete};
  require(bool(mux), "large WebP mux allocation failed");
  require(WebPMuxSetCanvasSize(mux.get(), 513, 131) == WEBP_MUX_OK, "large WebP canvas failed");
  const WebPMuxAnimParams parameters{0, 5};
  require(WebPMuxSetAnimationParams(mux.get(), &parameters) == WEBP_MUX_OK, "large WebP loop failed");
  struct Spec { int width, height, x, y, duration; std::array<std::uint8_t, 4> color; bool blend, dispose; };
  const std::array<Spec, 4> specs{{
      {513, 131, 0, 0, 17, {240, 16, 24, 255}, false, false},
      {257, 65, 64, 32, 31, {16, 240, 24, 128}, true, true},
      {129, 33, 256, 64, 47, {16, 24, 240, 255}, false, false},
      {65, 17, 0, 0, 59, {240, 240, 16, 255}, false, false}}};
  for (const auto& spec : specs) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(spec.width * spec.height * 4));
    for (std::size_t p = 0; p < rgba.size(); p += 4) std::copy(spec.color.begin(), spec.color.end(), rgba.begin() + p);
    std::uint8_t* encoded{};
    const auto size = WebPEncodeRGBA(rgba.data(), spec.width, spec.height, spec.width * 4, 83.0f, &encoded);
    std::unique_ptr<std::uint8_t, decltype(&WebPFree)> owned{encoded, WebPFree};
    require(size != 0 && encoded, "large lossy WebP frame encoding failed");
    WebPMuxFrameInfo frame{};
    frame.bitstream = {encoded, size}; frame.id = WEBP_CHUNK_ANMF;
    frame.x_offset = spec.x; frame.y_offset = spec.y; frame.duration = spec.duration;
    frame.blend_method = spec.blend ? WEBP_MUX_BLEND : WEBP_MUX_NO_BLEND;
    frame.dispose_method = spec.dispose ? WEBP_MUX_DISPOSE_BACKGROUND : WEBP_MUX_DISPOSE_NONE;
    require(WebPMuxPushFrame(mux.get(), &frame, 1) == WEBP_MUX_OK, "large WebP frame mux failed");
  }
  constexpr std::array<std::uint8_t, 4> xmp{'<','a','/','>'};
  const WebPData metadata{xmp.data(), xmp.size()};
  require(WebPMuxSetChunk(mux.get(), "XMP ", &metadata, 1) == WEBP_MUX_OK, "large WebP metadata failed");
  WebPData output{};
  require(WebPMuxAssemble(mux.get(), &output) == WEBP_MUX_OK, "large WebP assemble failed");
  const auto* begin = reinterpret_cast<const std::byte*>(output.bytes);
  Bytes result{begin, begin + output.size}; WebPDataClear(&output);
  return result;
}

void large_webp_thread_budget_checks(const std::filesystem::path& root) {
  struct Directory {
    std::filesystem::path path; bool owned{};
    ~Directory() { if (owned) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
  } scratch{root / ("animation-budget-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
  scratch.owned = std::filesystem::create_directory(scratch.path);
  require(scratch.owned, "animation budget fixture directory already exists");
  const auto path = scratch.path / "large-lossy.webp";
  const auto bytes = large_lossy_webp(); write(path, bytes);
  std::array<Bytes, 4> expected;
  {
    const WebPData data{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
    WebPAnimDecoderOptions options{};
    require(WebPAnimDecoderOptionsInit(&options), "WebP reference options failed");
    options.color_mode = MODE_RGBA; options.use_threads = 0;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> reference{
        WebPAnimDecoderNew(&data, &options), WebPAnimDecoderDelete};
    require(bool(reference), "large WebP reference decoder failed");
    constexpr std::array timestamps{17, 48, 95, 154};
    for (std::size_t i = 0; i < expected.size(); ++i) {
      std::uint8_t* pixels{}; int timestamp{};
      require(WebPAnimDecoderGetNext(reference.get(), &pixels, &timestamp) && pixels && timestamp == timestamps[i],
          "large WebP reference order/timestamps failed");
      const auto* begin = reinterpret_cast<const std::byte*>(pixels);
      expected[i].assign(begin, begin + 513 * 131 * 4);
    }
  }
  constexpr std::array durations{17u, 31u, 47u, 59u};
  awj::compat::stop_source stop; stop.request_stop();
  std::array<std::uint64_t, 3> resident{};
  for (const std::uint64_t budget : {0ull, 128ull * 1024 * 1024}) for (const int threads : {1, 2, 8}) {
    auto opened = awj::open_animation(path, budget, {}, false, threads);
    require(opened && *opened, "large WebP open failed under thread budget");
    auto& reader = **opened;
    require(reader.info().width == 513 && reader.info().height == 131 && reader.info().durations.size() == 4 &&
        reader.info().repetitions == 4 && reader.info().decoder_id == "libwebp-animation",
        "large WebP canvas/frame count/loop metadata changed");
    if (!budget) resident[threads == 1 ? 0 : threads == 2 ? 1 : 2] = reader.memory_usage_bytes();
    require(!reader.next(stop.get_token()), "large WebP pre-frame cancellation ignored");
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto duration = reader.info().durations[i];
      require(duration.numerator == durations[i] && duration.denominator == 1000, "large WebP VFR durations changed");
      if (i == 2) {
        const auto before = reader.frame().planes[0].bytes;
        require(!reader.next(stop.get_token()) && reader.frame().planes[0].bytes == before,
            "cancelled WebP next advanced or discarded the last full canvas");
      }
      const auto next = reader.next(); require(next && *next, "large WebP next failed");
      const auto& frame = reader.frame();
      require(frame.width == 513 && frame.height == 131 && frame.bit_depth == 8 &&
          frame.pixel_format == awj::PixelFormat::rgba && frame.planes.size() == 1 &&
          frame.planes[0].bytes == expected[i], "large WebP sequential pixels/blend/disposal changed across budgets");
      require(frame.metadata.size() == 1 && frame.metadata[0].kind == awj::MetadataKind::xmp &&
          frame.metadata[0].bytes == Bytes({std::byte{'<'}, std::byte{'a'}, std::byte{'/'}, std::byte{'>'}}),
          "large WebP reader metadata changed across budgets");
      const auto channel = [&](std::size_t x, std::size_t y, std::size_t c) {
        return std::to_integer<unsigned>(frame.planes[0].bytes[(y * 513 + x) * 4 + c]);
      };
      if (i == 1) require(channel(80, 48, 3) == 255 && channel(80, 48, 0) > 110 && channel(80, 48, 0) < 145 &&
          channel(80, 48, 1) > 110 && channel(80, 48, 1) < 145, "large WebP partial-alpha OVER did not blend");
      if (i >= 2) require(channel(80, 48, 3) == 0 && channel(280, 80, 2) > 220 && channel(280, 80, 3) == 255,
          "large WebP BACKGROUND disposal or later blue rectangle failed");
      if (i == 3) require(channel(8, 8, 0) > 220 && channel(8, 8, 1) > 220 && channel(500, 100, 0) > 220,
          "large WebP NO_BLEND or retained full-canvas region failed");
    }
    const auto end = reader.next(); require(end && !*end, "large WebP end-of-sequence changed");
    require(!awj::open_animation(path, budget, stop.get_token(), false, threads), "large WebP open cancellation ignored");
    // Destroy an active dependent stream after its first frame, then reopen it.
    auto partial = awj::open_animation(path, budget, {}, false, threads);
    require(partial && *partial && (*partial)->next().value_or(false), "WebP partial lifecycle initialization failed");
    partial->reset();
  }
  const auto increment = awj::webp_detail::decode_thread_workspace_bytes(2, 513);
  require(resident[1] == resident[0] + increment && resident[2] == resident[1],
      "WebP reader did not report its single optional worker reservation");
  const auto serial_limit = static_cast<std::uint64_t>(bytes.size()) + 513ull * 131 * 64 + 64 * 1024;
  auto serial = awj::open_animation(path, serial_limit, {}, false, 1);
  require(serial && *serial, "finite WebP serial working-set admission failed"); serial->reset();
  for (const int threads : {2, 8}) {
    std::uint64_t required{};
    auto rejected = awj::open_animation(path, serial_limit, {}, false, threads, &required);
    require(!rejected && rejected.error().find("内存预算") != std::string::npos && required > serial_limit,
        "WebP optional worker was not included in entry admission or required reservation");
    std::uint64_t refined{};
    auto retried = awj::open_animation(path, required, {}, false, threads, &refined);
    if (!retried) {
      require(refined > required && refined != UINT64_MAX,
          "WebP metadata refinement did not return a larger bounded reservation");
      retried = awj::open_animation(path, refined, {}, false, threads);
    }
    require(retried && *retried && (*retried)->next().value_or(false),
        "WebP reader could not reopen after its working-set reservation was enlarged");
  }
  for (const int threads : {1, 2, 8}) {
    require(!awj::open_animation(path, bytes.size(), {}, false, threads), "finite exhausted WebP budget became unlimited");
    auto damaged = bytes; damaged.resize(damaged.size() / 2);
    const auto broken_path = scratch.path / "truncated.webp"; write(broken_path, damaged);
    for (int repeat = 0; repeat < 2; ++repeat) {
      auto broken = awj::open_animation(broken_path, 0, {}, false, threads);
      if (broken) {
        require(bool(*broken), "damaged animation was mistaken for a static file");
        bool rejected = false;
        for (std::size_t i = 0; i < expected.size(); ++i) {
          const auto next = (*broken)->next();
          if (!next || !*next) { rejected = true; break; }
        }
        require(rejected, "truncated WebP animation fully decoded");
        broken->reset();
      }
      auto recovered = awj::open_animation(path, 0, {}, false, threads);
      require(recovered && *recovered && (*recovered)->next().value_or(false) &&
          (*recovered)->frame().planes[0].bytes == expected[0], "WebP reader destruction/failure poisoned a later stream");
    }
  }
}
}
int main(int argc, char** argv) {
  const bool budget_only = argc >= 2 && std::string_view{argv[1]} == "--thread-budget-only";
  const auto root = budget_only ? (argc >= 3 ? std::filesystem::path{argv[2]}
          : std::filesystem::current_path() / "results" / "codec-thread-budget")
      : argc > 1 ? std::filesystem::path{argv[1]}
          : std::filesystem::temp_directory_path() / "awj-animation-readers";
  std::filesystem::create_directories(root);
  large_webp_thread_budget_checks(root);
  if (budget_only) return 0;
  const auto gif_path = root / "disposal.gif", apng_path = root / "blend.png", webp_path = root / "blend.webp";
  gif(gif_path); write(apng_path, apng()); webp(webp_path);
  awj::compat::stop_source stop; stop.request_stop();
  for (const auto& path : {gif_path, apng_path, webp_path}) {
    std::array<Bytes, 4> baseline{};
    for (const std::uint64_t budget : {0ull, 128ull * 1024 * 1024}) for (const int threads : {1, 2, 8}) {
      auto opened = awj::open_animation(path, budget, {}, false, threads);
      if (!opened) { std::fprintf(stderr, "%s\n", opened.error().c_str()); return 1; }
      require(bool(*opened) && (*opened)->info().durations.size() == 4, "frame count/unlimited budget failed");
      auto& reader = **opened;
      require(reader.info().repetitions == (path == gif_path ? 0 : 2), "loop conversion failed");
      require(!reader.next(stop.get_token()), "frame cancellation ignored");
      for (int i = 0; i < 4; ++i) {
        auto next = reader.next(); require(next && *next, "frame decode failed");
        const auto& f = reader.frame(); require(f.width == 3 && f.height == 1, "full canvas failed");
        const auto& p = f.planes[0].bytes;
        if (baseline[i].empty()) baseline[i] = p;
        else require(p == baseline[i], "CPU budget changed dependent-frame blend/disposal output");
        if (i == 0) require(p[0] == std::byte{255} && p[8] == std::byte{255}, "first full canvas failed");
        if (i == 2 && path != webp_path) require(p[0] == std::byte{0} && p[6] == std::byte{255}, "BACKGROUND disposal failed");
        if (i == 3 && path != webp_path) require(p[4] == std::byte{255} && p[6] == std::byte{0} && p[8] == std::byte{255}, "PREVIOUS disposal failed");
        if (i == 1 && path == apng_path) require(std::to_integer<int>(p[0]) >= 126 && std::to_integer<int>(p[1]) >= 127, "APNG OVER blend failed");
      }
      auto end = reader.next(); require(end && !*end, "end of animation failed");
      require(!awj::open_animation(path, budget, stop.get_token(), false, threads), "open cancellation ignored");
    }
    require(!awj::open_animation(path, 1), "memory budget ignored");
    require(!awj::open_animation(path, std::filesystem::file_size(path)), "exhausted finite input budget became unlimited");
  }
  require(!awj::animation_detail::fits_working_set(32, 1, 1, 32), "zero finite remainder became unlimited");
  require(awj::animation_detail::fits_working_set(32, 1, 1, 0), "original zero budget is not unlimited");
  require(!awj::animation_detail::fits_working_set(std::numeric_limits<std::uint64_t>::max() - 3, 4, 1, 0), "unlimited working set overflow accepted");

  for (const bool deep : {false, true}) {
    const auto full = deep ? 65535u : 255u;
    for (const auto count : {1u, 2u}) {
      const auto bytes = apng(true, deep, count);
      const auto path = root / ("poster-" + std::to_string(deep ? 16 : 8) + "-" + std::to_string(count) + (deep ? ".apng" : ".png"));
      write(path, bytes);
      auto in_memory = awj::png_detail::contains_animation_control_chunk(bytes, "blue independent poster");
      auto routed = awj::input_uses_animation_reader(path);
      require(in_memory && !*in_memory && routed && !*routed, "independent poster routed to animation reader");
      for (const std::uint64_t budget : {0ull, 128ull * 1024 * 1024}) {
        auto opened = awj::open_animation(path, budget);
        require(opened && !*opened, "open_animation discarded independent default image");
      }
      auto decoded = awj::PngImageDecoder{}.decode(path);
      if (!decoded) {
        std::fprintf(stderr, "poster-%d-%u decode failed: %s\n", deep ? 16 : 8, count, decoded.error().c_str());
        return 1;
      }
      if (decoded->image.bit_depth != (deep ? 16 : 8)) {
        std::fprintf(stderr, "poster-%d-%u precision changed: actual %d\n", deep ? 16 : 8, count, decoded->image.bit_depth);
        return 1;
      }
      for (std::size_t x = 0; x < 3; ++x)
        require(sample(decoded->image, x * 4) == 0 && sample(decoded->image, x * 4 + 1) == 0 &&
            sample(decoded->image, x * 4 + 2) == full && sample(decoded->image, x * 4 + 3) == full,
            "red/green animation replaced blue static poster");
      awj::animation_detail::ApngReader direct;
      direct.bytes = bytes;
      require(!direct.open({}), "APNG reader silently discarded independent poster");
    }
    for (const auto count : {1u, 4u}) {
      const auto bytes = apng(false, deep, count, true);
      const auto path = root / ("pure-" + std::to_string(deep ? 16 : 8) + "-" + std::to_string(count) + ".apng");
      write(path, bytes);
      auto in_memory = awj::png_detail::contains_animation_control_chunk(bytes, "default is frame zero");
      auto routed = awj::input_uses_animation_reader(path);
      require(in_memory && *in_memory && routed && *routed, "animation frame zero mistaken for independent poster");
      auto reader = awj::open_animation(path, 0);
      require(reader && *reader && (*reader)->info().durations.size() == count, "pure APNG count/unlimited budget failed");
      require((*reader)->info().repetitions == 2, "pure APNG loop lost");
      for (unsigned i = 0; i < count; ++i) {
        const auto duration = (*reader)->info().durations[i];
        require(duration.numerator == 1 && duration.denominator == 1000, "APNG zero delay normalization failed");
        auto next = (*reader)->next();
        require(next && *next && (*reader)->frame().bit_depth == (deep ? 16 : 8), "APNG sample precision failed");
        if (!i) require(sample((*reader)->frame(), 0) == full && sample((*reader)->frame(), 2) == 0, "pure APNG first red frame lost");
      }
      auto end = (*reader)->next(); require(end && !*end, "pure APNG termination failed");
    }
  }
  for (const bool poster : {false, true}) {
    auto bad = apng(poster, false, 2); bad[45] ^= std::byte{1}; reject_apng(root, bad);
    bad = apng(poster, false, 2); patch_u32(bad, "fcTL", 0, 1); reject_apng(root, bad);
    bad = apng(poster, false, 2); patch_u32(bad, "fdAT", 0, 99); reject_apng(root, bad);
    bad = apng(poster, false, 2); bad.pop_back(); reject_apng(root, bad);
    bad = apng(poster, false, 2); bad.resize(bad.size() - 12); reject_apng(root, bad);
    bad = apng(poster, false, 2); bad.push_back(std::byte{0}); reject_apng(root, bad);
    bad = apng(poster, false, 2); patch_u32(bad, "acTL", 0, 3); reject_apng(root, bad);
    bad = apng(poster, false, 2); patch_u32(bad, "acTL", 0, 100001); reject_apng(root, bad);
    bad = apng(poster, false, 2); patch_u32(bad, "fcTL", 4, 4); reject_apng(root, bad);
    bad = apng(poster, false, 2); patch_u32(bad, "IHDR", 0, 0); reject_apng(root, bad);
    Bytes extra; chunk(extra, "ABCD", {});
    bad = apng(poster, false, 2); bad.insert(bad.end() - 12, extra.begin(), extra.end()); reject_apng(root, bad);
    extra.clear(); chunk(extra, "IDAT", {std::byte{0}});
    bad = apng(poster, false, 2); bad.insert(bad.end() - 12, extra.begin(), extra.end()); reject_apng(root, bad);
  }
  auto huge = apng(false, true, 1);
  for (const auto tag : {"IHDR", "fcTL"}) {
    patch_u32(huge, tag, std::string_view{tag} == "IHDR" ? 0 : 4, 0x7fffffffu);
    patch_u32(huge, tag, std::string_view{tag} == "IHDR" ? 4 : 8, 0x7fffffffu);
  }
  const auto huge_path = root / "oversized.apng"; write(huge_path, huge);
  const auto huge_route = awj::input_uses_animation_reader(huge_path);
  require(huge_route && *huge_route, "oversized APNG fixture has invalid animation structure");
  require(!awj::open_animation(huge_path, 0), "unlimited APNG bypassed buffer size/overflow protection");
  if (argc == 1) std::filesystem::remove_all(root);
  return 0;
}
