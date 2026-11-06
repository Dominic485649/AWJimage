#include "../src/compat/stdlib_compat.h"
#include <avif/avif.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

import awj.config;
import awj.codec;
import awj.image;
import awj.pixel_conversion;
import awj.avif_aom_codec;

namespace {
int fail(const std::string& message) { std::fprintf(stderr, "%s\n", message.c_str()); return 1; }
struct PrefixInput { std::span<const std::byte> bytes; std::size_t available; };
avifResult read_prefix(avifIO* io, std::uint32_t flags, std::uint64_t offset, std::size_t size, avifROData* out) {
  const auto& input = *static_cast<PrefixInput*>(io->data);
  if (flags || offset > input.bytes.size()) return AVIF_RESULT_IO_ERROR;
  size = std::min(size, input.bytes.size() - static_cast<std::size_t>(offset));
  if (offset > input.available || size > input.available - static_cast<std::size_t>(offset))
    return AVIF_RESULT_WAITING_ON_IO;
  out->data = reinterpret_cast<const std::uint8_t*>(input.bytes.data()) + offset;
  out->size = size;
  return AVIF_RESULT_OK;
}

int inspect(std::span<const std::byte> bytes, int expected_layers, std::uint32_t width, std::uint32_t height) {
  std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder{avifDecoderCreate(), avifDecoderDestroy};
  decoder->allowProgressive = AVIF_TRUE;
  decoder->maxThreads = 1;
  if (avifDecoderSetIOMemory(decoder.get(), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()) != AVIF_RESULT_OK ||
      avifDecoderParse(decoder.get()) != AVIF_RESULT_OK) return fail("无法解析分层 AVIF。");
  if (expected_layers && decoder->imageCount != expected_layers) return fail("实际 AVIF 层数不符。");
  if (decoder->imageSequenceTrackPresent) return fail("渐进静态图被编码为动画序列。");
  const auto layers = decoder->imageCount;
  if (layers < 1 || layers > awj::max_avif_layers) return fail("AVIF 层数超界。");
  std::uint64_t first_prefix{};
  for (int i = 0; i < layers; ++i) {
    avifExtent extent{};
    if (avifDecoderNthImageMaxExtent(decoder.get(), i, &extent) != AVIF_RESULT_OK ||
        extent.offset > bytes.size() || extent.size > bytes.size() - extent.offset)
      return fail("层数据范围不合法。");
    if (!i) first_prefix = extent.offset + extent.size;
    if (avifDecoderNextImage(decoder.get()) != AVIF_RESULT_OK) return fail("渐进层解码失败。");
    if (i == layers - 1 && width && (decoder->image->width != width || decoder->image->height != height))
      return fail("最终层尺寸被改变。");
    std::printf("layer=%d presented=%ux%u depth=%u extent=%llu+%zu alpha=%d\n", i + 1,
        decoder->image->width, decoder->image->height, decoder->image->depth,
        static_cast<unsigned long long>(extent.offset), extent.size, decoder->alphaPresent);
  }
  if (layers > 1) {
    if (!first_prefix || first_prefix >= bytes.size()) return fail("首层需要整份文件，未形成可用前缀。");
    PrefixInput input{bytes, static_cast<std::size_t>(first_prefix)};
    avifIO io{};
    io.read = read_prefix; io.sizeHint = bytes.size(); io.persistent = AVIF_TRUE; io.data = &input;
    std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> partial{avifDecoderCreate(), avifDecoderDestroy};
    partial->allowProgressive = AVIF_TRUE; partial->maxThreads = 1;
    avifDecoderSetIO(partial.get(), &io);
    if (avifDecoderParse(partial.get()) != AVIF_RESULT_OK || avifDecoderNextImage(partial.get()) != AVIF_RESULT_OK)
      return fail("未收齐文件时无法解出首层（包括 alpha）。");
    if (avifDecoderNextImage(partial.get()) != AVIF_RESULT_WAITING_ON_IO)
      return fail("未到达的增强层没有正确等待输入。");
    std::printf("first_decodable_prefix=%llu total_bytes=%zu\n", static_cast<unsigned long long>(first_prefix), bytes.size());
    // io 借用栈对象；先销毁 decoder，再结束 input/io 的生命周期。
    partial.reset();
  }
  return 0;
}
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view{argv[1]} == "--inspect-directory") {
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(argv[2])}})) {
      if (entry.path().extension() != L".avif") continue;
      const auto size = std::filesystem::file_size(entry.path());
      if (!size || size > 64ull * 1024 * 1024) return fail("诊断文件超出 64 MiB 有界输入范围。");
      std::vector<std::byte> bytes(static_cast<std::size_t>(size));
      std::ifstream input(entry.path(), std::ios::binary);
      input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!input) return fail("无法完整读取待检查 AVIF。");
      const auto name = entry.path().filename().u8string();
      std::printf("file=%s\n", reinterpret_cast<const char*>(name.c_str()));
      if (inspect(bytes, 0, 0, 0)) return 1;
    }
    return 0;
  }
  if (awj::default_app_config().avif_layers.enabled()) return fail("默认不应启用渐进式。");
  auto automatic = awj::parse_arguments({L"--avif-progressive"});
  if (!automatic || !automatic->config.avif_layers.automatic) return fail("自动渐进 CLI 未生效。");
  auto plans = awj::plan_avif_layers(automatic->config.avif_layers, 129, 97, 70, 70);
  if (!plans || plans->size() != 2 || (*plans)[0].width != 65 || (*plans)[0].height != 49 ||
      (*plans)[0].quality != 10 || (*plans)[0].alpha_quality != 70 || (*plans)[1].width != 129)
    return fail("自动渐进未沿用上游半尺寸/质量策略。");
  for (const auto& args : {
      std::vector<std::wstring>{L"--avif-progressive", L"-q", L"9"},
      std::vector<std::wstring>{L"--avif-progressive", L"--avif-layers", L"2"},
      std::vector<std::wstring>{L"--avif-layers", L"5"},
      std::vector<std::wstring>{L"--avif-layer-quality", L"1:10"},
      std::vector<std::wstring>{L"--avif-layers", L"2", L"--avif-layer-size", L"0:64x64"},
      std::vector<std::wstring>{L"--avif-layers", L"2", L"--avif-layer-scale", L"1:1/0"},
      std::vector<std::wstring>{L"--avif-layers", L"2", L"--avif-layer-size", L"1:64x64", L"--avif-layer-scale", L"1:1/2"},
      std::vector<std::wstring>{L"--format", L"webp", L"--avif-progressive"}})
    if (awj::parse_arguments(args)) return fail("非法渐进参数未被拒绝。");

  awj::ImageBuffer image{.width = 128, .height = 96, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = 8};
  awj::ImagePlane plane{.stride = image.width * 4};
  plane.bytes.resize(plane.stride * image.height);
  for (std::size_t y = 0; y < image.height; ++y) for (std::size_t x = 0; x < image.width; ++x) {
    auto* p = plane.bytes.data() + y * plane.stride + x * 4;
    p[0] = std::byte{static_cast<unsigned char>(x * 7)};
    p[1] = std::byte{static_cast<unsigned char>(y * 11)};
    p[2] = std::byte{static_cast<unsigned char>(x + y)};
    p[3] = std::byte{static_cast<unsigned char>((x + y) % 255)};
  }
  image.planes.push_back(std::move(plane));
  auto encoder = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
  for (int count = 1; count <= 4; ++count) {
    awj::NativeEncodeSettings settings{};
    settings.quality = 70; settings.speed = 10; settings.speed_explicit = true;
    settings.bit_depth = 10; settings.chroma_mode = awj::ChromaMode::yuv444;
    settings.source_has_alpha_channel = true; settings.encoder_supports_alpha = true;
    settings.applied_alpha = "kept";
    if (count == 1) settings.avif_layers.automatic = true;
    else {
      settings.avif_layers.count = count;
      for (int i = 0; i < count - 1; ++i) {
        settings.avif_layers.layers[i].size = std::pair{static_cast<std::uint32_t>(31 + i * 29),
                                                       static_cast<std::uint32_t>(23 + i * 21)};
        settings.avif_layers.layers[i].quality = 10 + i * 20;
        settings.avif_layers.layers[i].alpha_quality = 70;
      }
    }
    if (count == 3) settings.quality = 100;
    if (count > 1) settings.avif_layers.layers[count - 1].quality = 82;
    auto encoded = encoder->encode(image, settings);
    if (!encoded) return fail(encoded.error());
    if (encoded->final_quality != (count == 1 ? 70 : 82) || encoded->lossless)
      return fail("最终质量诊断没有遵循手动最后层。");
    if (inspect(encoded->encoded.bytes, count == 1 ? 2 : count, 128, 96)) return 1;
  }
  awj::NativeEncodeSettings exact{};
  exact.quality = 70; exact.speed = 10; exact.bit_depth = 10;
  exact.chroma_mode = awj::ChromaMode::yuv444;
  exact.applied_matrix_coefficients = 6; exact.applied_color_range = 1;
  exact.source_has_alpha_channel = true; exact.encoder_supports_alpha = true;
  exact.applied_alpha = "kept";
  exact.avif_layers.count = 2;
  exact.avif_layers.layers[0].scale = std::pair{1u, 2u};
  exact.avif_layers.layers[0].quality = 20;
  exact.avif_layers.layers[1].quality = 100;
  exact.avif_layers.layers[1].alpha_quality = 100;
  auto encoded = encoder->encode(image, exact);
  if (!encoded) return fail(encoded.error());
  if (encoded->final_quality != 100 || !encoded->lossless) return fail("末层 q100 诊断不符。");
  auto decoded = awj::make_avif_image_decoder(1)->decode_memory(encoded->encoded.bytes, "layer-q100",
      awj::DecodeOptions{.native_pixels = true});
  auto expected = awj::convert_image_pixels(image, awj::PixelConversionRequest{
      .format = awj::PixelFormat::yuv444, .bit_depth = 10, .matrix_coefficients = 6,
      .color_range = 1, .preserve_alpha = true});
  if (!decoded) return fail(decoded.error());
  if (!expected) return fail(expected.error());
  if (decoded->image.planes.size() != expected->planes.size())
    return fail("末层 q100 目标像素平面数不符。");
  for (std::size_t c = 0; c < expected->planes.size(); ++c)
    for (std::size_t y = 0; y < image.height; ++y) {
      const auto a = expected->planes[c].read_bytes().subspan(y * expected->planes[c].stride, image.width * 2);
      const auto b = decoded->image.planes[c].read_bytes().subspan(y * decoded->image.planes[c].stride, image.width * 2);
      if (!std::equal(a.begin(), a.end(), b.begin())) return fail("末层 q100 未保持目标 YUV/alpha 样本。");
    }
  return 0;
}
