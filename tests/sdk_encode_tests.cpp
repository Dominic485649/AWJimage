#define NOMINMAX
#include <windows.h>
#include "awj/awj.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <vector>
import awj.image;
import awj.codec;
import awj.avif_aom_codec;
import awj.png_codec;
import awj.webp_codec;
import awj.jxl_codec;
import awj.jpegli_codec;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "第 %d 行内存编码测试失败: %s; %s\n", __LINE__, #value, awj_last_error()); return 1; } } while (false)
std::unique_ptr<awj::ImageDecoder> decoder(uint32_t codec) {
  switch (codec) {
    case AWJ_CODEC_PNG: return std::make_unique<awj::PngImageDecoder>();
    case AWJ_CODEC_WEBP: return std::make_unique<awj::WebPImageDecoder>();
    case AWJ_CODEC_JXL: return std::make_unique<awj::JXLImageDecoder>();
    case AWJ_CODEC_AVIF: return awj::make_avif_image_decoder(1);
    default: return std::make_unique<awj::JpegliImageDecoder>();
  }
}
void color(awj_frame_desc& desc) {
  desc.color.primaries = 1; desc.color.transfer = 13; desc.color.matrix = 0; desc.color.range = 1;
}
int main() {
  CHECK(awj_capabilities() & AWJ_CAP_NATIVE_CODECS);
  awj_context* context{}; CHECK(awj_context_create(nullptr, &context) == AWJ_OK);
  constexpr uint32_t width = 8, height = 6, stride = 40;
  constexpr size_t size = (height - 1) * stride + width * 4;
  auto* pixels = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)); CHECK(pixels);
  std::array<uint8_t, size> original{};
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    auto* pixel = original.data() + y * stride + x * 4;
    pixel[0] = static_cast<uint8_t>(x * 29); pixel[1] = static_cast<uint8_t>(y * 41);
    pixel[2] = static_cast<uint8_t>(x * 7 + y * 13); pixel[3] = static_cast<uint8_t>(128 + x * 11);
  }
  std::memcpy(pixels, original.data(), size);
  DWORD protection{}; CHECK(VirtualProtect(pixels, 4096, PAGE_READONLY, &protection));
  awj_frame_desc desc; awj_frame_desc_init(&desc); desc.width = width; desc.height = height; color(desc);
  desc.color.alpha = AWJ_ALPHA_STRAIGHT; desc.planes[0] = {pixels, size, 0, stride};
  awj_frame* frame{}; CHECK(awj_frame_create(context, &desc, nullptr, &frame) == AWJ_OK);
  awj_encode_options options; awj_encode_options_init(&options); options.speed = 9;
  awj_result* result{};
  for (uint32_t codec : {AWJ_CODEC_PNG, AWJ_CODEC_WEBP, AWJ_CODEC_JXL, AWJ_CODEC_JPEGLI, AWJ_CODEC_AVIF}) {
    options.codec = codec; options.quality = codec == AWJ_CODEC_JPEGLI ? 90 : 100;
    options.lossless = codec == AWJ_CODEC_PNG || codec == AWJ_CODEC_WEBP || codec == AWJ_CODEC_JXL;
    options.copy_policy = AWJ_COPY_ALLOW_REQUIRED;
    CHECK(awj_encode(context, frame, &options, &result) == AWJ_OK && result);
    CHECK(awj_result_size(result) > 0 && awj_result_data(result));
    auto decoded = decoder(codec)->decode_memory({reinterpret_cast<const std::byte*>(awj_result_data(result)), static_cast<size_t>(awj_result_size(result))}, "memory-sdk");
    if (!decoded) std::fprintf(stderr, "codec %u 回读失败：%s\n", codec, decoded.error().c_str());
    CHECK(decoded && decoded->image.width == width && decoded->image.height == height);
    if (codec == AWJ_CODEC_PNG || codec == AWJ_CODEC_WEBP) {
      CHECK(decoded->image.bit_depth == 8);
      const auto& plane = decoded->image.planes[0];
      for (uint32_t y = 0; y < height; ++y)
        CHECK(std::memcmp(plane.read_bytes().data() + y * plane.stride, pixels + y * stride, width * 4) == 0);
    }
    awj_diagnostics diagnostics{sizeof(diagnostics), AWJ_ABI_VERSION};
    CHECK(awj_result_diagnostics(result, &diagnostics) == AWJ_OK && diagnostics.encoder_internal_copies_known == 0 && diagnostics.conversion_bytes_known == 0);
    CHECK(diagnostics.input_copy_bytes == (codec == AWJ_CODEC_JXL ? width * height * 4 : 0));
    CHECK(std::memcmp(pixels, original.data(), size) == 0);
    awj_result_release(result);
  }
  options.codec = AWJ_CODEC_JPEGLI; options.lossless = 1;
  CHECK(awj_encode(context, frame, &options, &result) == AWJ_UNSUPPORTED && !result);
  awj_frame_release(frame);
  // 负 stride 兼容路径输出垂直翻转像素；严格模式不能偷偷重排。
  desc.planes[0].offset = (height - 1) * stride; desc.planes[0].stride = -static_cast<int64_t>(stride);
  CHECK(awj_frame_create(context, &desc, nullptr, &frame) == AWJ_OK);
  options.codec = AWJ_CODEC_PNG; options.quality = 100; options.lossless = 1; options.copy_policy = AWJ_COPY_REQUIRE_BORROWED;
  CHECK(awj_encode(context, frame, &options, &result) == AWJ_COPY_REQUIRED && !result);
  options.copy_policy = AWJ_COPY_ALLOW_REQUIRED;
  CHECK(awj_encode(context, frame, &options, &result) == AWJ_OK);
  auto flipped = decoder(AWJ_CODEC_PNG)->decode_memory({reinterpret_cast<const std::byte*>(awj_result_data(result)), static_cast<size_t>(awj_result_size(result))}, "negative-stride");
  CHECK(flipped);
  for (uint32_t y = 0; y < height; ++y)
    CHECK(std::memcmp(flipped->image.planes[0].read_bytes().data() + y * flipped->image.planes[0].stride, pixels + (height - y - 1) * stride, width * 4) == 0);
  awj_result_release(result); awj_frame_release(frame);
  CHECK(std::memcmp(pixels, original.data(), size) == 0);
  CHECK(VirtualFree(pixels, 0, MEM_RELEASE));

  // 常见 FFmpeg CPU 布局，包括奇数 NV12/P010 和 10/16-bit planar。
  for (uint32_t format : {AWJ_BGRA8, AWJ_RGB8, AWJ_GRAY8, AWJ_GRAY16, AWJ_RGBA16, AWJ_YUV420P, AWJ_YUV422P, AWJ_YUV444P, AWJ_NV12, AWJ_P010}) {
    awj_frame_desc_init(&desc); desc.width = 5; desc.height = 3; desc.format = format;
    desc.bit_depth = format == AWJ_P010 || format == AWJ_YUV420P ? 10 : format == AWJ_GRAY16 || format == AWJ_RGBA16 ? 16 : 8;
    color(desc); desc.color.chroma_location = 2;
    if (format >= AWJ_YUV420P) { desc.color.matrix = 6; desc.color.range = 0; }
    awj_frame_layout_info shape{sizeof(shape), AWJ_ABI_VERSION}; CHECK(awj_frame_layout(&desc, &shape) == AWJ_OK);
    std::vector<uint8_t> storage(static_cast<size_t>(shape.pixel_bytes), 128);
    if (desc.bit_depth > 8) {
      const uint16_t sample = format == AWJ_P010 ? static_cast<uint16_t>(512 << 6) : desc.bit_depth == 10 ? 512 : 32768;
      for (size_t i = 0; i < storage.size(); i += 2) std::memcpy(storage.data() + i, &sample, 2);
    }
    desc.plane_count = shape.plane_count; uint64_t offset{};
    for (uint32_t p = 0; p < shape.plane_count; ++p) {
      desc.planes[p] = {storage.data(), storage.size(), offset, static_cast<int64_t>(shape.row_bytes[p])};
      offset += shape.row_bytes[p] * shape.rows[p];
    }
    const auto saved = storage;
    CHECK(awj_frame_create(context, &desc, nullptr, &frame) == AWJ_OK);
    CHECK(awj_encode(context, frame, &options, &result) == AWJ_OK);
    auto decoded = decoder(AWJ_CODEC_PNG)->decode_memory({reinterpret_cast<const std::byte*>(awj_result_data(result)), static_cast<size_t>(awj_result_size(result))}, "cpu-layout");
    CHECK(decoded && decoded->image.width == 5 && decoded->image.height == 3 && storage == saved);
    awj_result_release(result); awj_frame_release(frame);
    // 非居中的 FFmpeg 色度位置需要真实重采样，不默默丢弃标签。
    if (format == AWJ_YUV420P) {
      desc.color.chroma_location = 1;
      CHECK(awj_frame_create(context, &desc, nullptr, &frame) == AWJ_OK);
      options.copy_policy = AWJ_COPY_REQUIRE_BORROWED;
      CHECK(awj_encode(context, frame, &options, &result) == AWJ_COPY_REQUIRED && !result);
      options.copy_policy = AWJ_COPY_ALLOW_REQUIRED;
      CHECK(awj_encode(context, frame, &options, &result) == AWJ_OK);
      awj_diagnostics diagnostics{sizeof(diagnostics), AWJ_ABI_VERSION};
      CHECK(awj_result_diagnostics(result, &diagnostics) == AWJ_OK && diagnostics.conversion_bytes > 0);
      awj_result_release(result); awj_frame_release(frame);
    }
  }
  // FP16 scRGB 保留负值/扩展范围，编码过程不得原地改写来源。
  std::array<uint16_t, 16> half{0x3800,0xb400,0x4000,0x3c00, 0x3c00,0,0,0x3c00, 0,0x3c00,0,0x3800, 0,0,0x3c00,0x3c00};
  const auto saved_half = half;
  awj_frame_desc_init(&desc); desc.width = 2; desc.height = 2; desc.format = AWJ_RGBA16F; desc.bit_depth = 16;
  color(desc); desc.color.transfer = 8; desc.color.linear_reference_white_nits = 80; desc.color.alpha = AWJ_ALPHA_STRAIGHT;
  desc.planes[0] = {reinterpret_cast<const uint8_t*>(half.data()), sizeof(half), 0, 16};
  CHECK(awj_frame_create(context, &desc, nullptr, &frame) == AWJ_OK);
  for (uint32_t codec : {AWJ_CODEC_PNG, AWJ_CODEC_JXL, AWJ_CODEC_AVIF}) {
    options.codec = codec; options.bit_depth = 0; options.lossless = 0; options.quality = 85;
    CHECK(awj_encode(context, frame, &options, &result) == AWJ_OK);
    auto decoded = decoder(codec)->decode_memory({reinterpret_cast<const std::byte*>(awj_result_data(result)), static_cast<size_t>(awj_result_size(result))}, "scrgb-half");
    CHECK(decoded && decoded->image.width == 2 && decoded->image.height == 2 && half == saved_half);
    awj_result_release(result);
  }
  awj_frame_release(frame);
  // 视觉质量候选只走内存；这里仅测试受支持的 WebP 路径。
  std::array<uint8_t, 16> opaque{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
  awj_frame_desc_init(&desc); desc.width = 2; desc.height = 2; color(desc); desc.planes[0] = {opaque.data(), opaque.size(), 0, 8};
  CHECK(awj_frame_create(context, &desc, nullptr, &frame) == AWJ_OK);
  options.codec = AWJ_CODEC_WEBP; options.quality = 80; options.visual_quality = 1;
  CHECK(awj_encode(context, frame, &options, &result) == AWJ_OK);
  awj_result_release(result); awj_frame_release(frame);
  CHECK(awj_frame_create(context, &desc, nullptr, &frame) == AWJ_OK);
  awj_context_cancel(context);
  CHECK(awj_encode(context, frame, &options, &result) == AWJ_CANCELLED && !result);
  awj_frame_release(frame); awj_context_destroy(context);
  std::puts("五种编码器的 C ABI 内存编码/解码、只读 padding、负 stride、CPU 布局、FP16 与视觉质量路径通过。");
  return 0;
}
