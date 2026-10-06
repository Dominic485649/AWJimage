#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <vector>
#include <gif_lib.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <zlib.h>
import awj.animation_decoder;
import awj.codec;
import awj.config;
import awj.image;
import awj.png_codec;

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
  awj::PngImageEncoder encoder; awj::NativeEncodeSettings options{.output_format = awj::OutputFormat::png};
  auto output = encoder.encode(image, options); require(bool(output), "PNG fixture encoding failed");
  require(output->encoded.bytes.size() >= 12 && read32(output->encoded.bytes.data() + output->encoded.bytes.size() - 4) == 0xae426082,
      "PNG empty IEND chunk CRC failed");
  return output->encoded.bytes;
}
Bytes apng(bool poster = false, bool deep = false, unsigned count = 4, bool zero_delay = false) {
  auto first = png(solid(3, 255, 0, 0), deep);
  Bytes result(first.begin(), first.begin() + 33); // signature + IHDR
  Bytes control; be32(control, count); be32(control, 3); chunk(result, "acTL", control);
  if (poster) {
    for (std::size_t pos = 8; pos + 12 <= first.size();) {
      const auto size = read32(first.data() + pos);
      if (std::memcmp(first.data() + pos + 4, "IDAT", 4) == 0)
        chunk(result, "IDAT", Bytes(first.begin() + pos + 8, first.begin() + pos + 8 + size));
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
}
int main(int argc, char** argv) {
  const auto root = argc > 1 ? std::filesystem::path{argv[1]}
      : std::filesystem::temp_directory_path() / "awj-animation-readers";
  std::filesystem::create_directories(root);
  const auto gif_path = root / "disposal.gif", apng_path = root / "blend.png", webp_path = root / "blend.webp";
  gif(gif_path); write(apng_path, apng()); webp(webp_path);
  for (const auto& path : {gif_path, apng_path, webp_path}) {
    auto opened = awj::open_animation(path, 128 * 1024 * 1024);
    if (!opened) { std::fprintf(stderr, "%s\n", opened.error().c_str()); return 1; }
    require(bool(*opened) && (*opened)->info().durations.size() == 4, "frame count failed");
    auto& reader = **opened;
    require(reader.info().repetitions == (path == gif_path ? 0 : 2), "loop conversion failed");
    for (int i = 0; i < 4; ++i) {
      auto next = reader.next(); require(next && *next, "frame decode failed");
      const auto& f = reader.frame(); require(f.width == 3 && f.height == 1, "full canvas failed");
      const auto& p = f.planes[0].bytes;
      if (i == 0) require(p[0] == std::byte{255} && p[8] == std::byte{255}, "first full canvas failed");
      if (i == 2 && path != webp_path) require(p[0] == std::byte{0} && p[6] == std::byte{255}, "BACKGROUND disposal failed");
      if (i == 3 && path != webp_path) require(p[4] == std::byte{255} && p[6] == std::byte{0} && p[8] == std::byte{255}, "PREVIOUS disposal failed");
      if (i == 1 && path == apng_path) require(std::to_integer<int>(p[0]) >= 126 && std::to_integer<int>(p[1]) >= 127, "APNG OVER blend failed");
    }
    auto end = reader.next(); require(end && !*end, "end of animation failed");
    require(!awj::open_animation(path, 1), "memory budget ignored");
    std::stop_source stop; stop.request_stop(); require(!awj::open_animation(path, 128 * 1024 * 1024, stop.get_token()), "cancellation ignored");
  }
  auto bad = apng(); bad[45] ^= std::byte{1}; write(root / "bad.png", bad);
  require(!awj::open_animation(root / "bad.png", 128 * 1024 * 1024), "bad APNG accepted");
  for (const auto count : {1u, 4u}) {
    const auto path = root / (count == 1 ? "single16.apng" : "poster16.apng");
    write(path, apng(true, true, count, true));
    auto reader = awj::open_animation(path, 128 * 1024 * 1024);
    require(reader && *reader && (*reader)->info().durations.size() == count, "APNG poster/single frame count failed");
    require((*reader)->info().durations[0].numerator == 1 && (*reader)->info().durations[0].denominator == 1000, "APNG zero delay normalization failed");
    for (unsigned i = 0; i < count; ++i) {
      auto next = (*reader)->next(); require(next && *next && (*reader)->frame().bit_depth == 16, "APNG 16-bit sample precision failed");
    }
  }
  if (argc == 1) std::filesystem::remove_all(root);
  return 0;
}
