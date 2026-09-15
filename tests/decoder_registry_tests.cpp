#include <filesystem>
#include <iostream>
#include <array>
#include <cstdint>
#include <span>
#include <stop_token>

import awj.decoder_registry;
import awj.heif_codec;

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::array<std::uint8_t, 26> exif{0x49, 0x49, 42, 0, 8, 0, 0, 0,
      1, 0, 0x12, 1, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0};
  awj::heif_detail::normalize_exif_orientation(std::as_writable_bytes(std::span{exif}));
  if (exif[18] != 1) return fail("HEIC EXIF orientation would be applied twice by an output reader.");
  for (int bits : {10, 12, 16}) {
    const auto maximum = static_cast<std::uint16_t>((1u << bits) - 1u);
    std::array<std::uint16_t, 4> input{0, maximum, static_cast<std::uint16_t>(maximum / 2), maximum};
    std::array<std::uint16_t, 4> output{};
    awj::heif_detail::copy_rgba16_row(std::as_writable_bytes(std::span{output}),
        reinterpret_cast<const std::uint8_t*>(input.data()), bits);
    if (output[0] != 0 || output[1] != 65535 || output[3] != 65535 ||
        output[2] < 32700 || output[2] > 32768) {
      return fail("HEIC high-bit-depth RGBA samples were not expanded to the pipeline range.");
    }
  }
  awj::HeifImageDecoder heif_decoder;
  for (int i = 1; i < argc; ++i) {
    auto decoded = heif_decoder.decode(std::filesystem::path{argv[i]});
    if (!decoded) { std::cerr << decoded.error() << '\n'; return 1; }
    const auto& image = decoded->image;
    if (!image.width || !image.height || !image.source_info || !image.significant_bits ||
        image.significant_bits->alpha < 1 || image.significant_bits->alpha > image.bit_depth) {
      return fail("HEIC fixture has invalid dimensions or channel precision.");
    }
    std::cout << "HEIC fixture " << image.width << 'x' << image.height
              << " storage=" << image.bit_depth << " source=" << image.source_info->bit_depth << '\n';
  }
  const std::array<std::byte, 8> truncated{};
  if (heif_decoder.decode_memory(truncated, "truncated.heic")) {
    return fail("HEIC decoder accepted a truncated container.");
  }
  std::stop_source canceled;
  canceled.request_stop();
  if (awj::HeifImageDecoder{1, canceled.get_token()}.decode_memory(truncated, "canceled.heic")) {
    return fail("HEIC decoder ignored cancellation.");
  }
  auto png = awj::select_decoder_for_path("sample.png", {.allow_wic_fallback = true});
  if (!png || png->fallback || png->decoder->id() != "libpng") {
    return fail("PNG did not select libpng before WIC.");
  }

  auto jpeg = awj::select_decoder_for_path("sample.jpeg", {.allow_wic_fallback = true});
  if (!jpeg || jpeg->fallback) {
    return fail("JPEG did not select a native decoder before WIC.");
  }
#if AWJ_HAS_JPEGLI
  if (jpeg->decoder->id() != "jpegli") {
    return fail("JPEG did not select Jpegli before libjpeg-turbo/WIC.");
  }
#else
  if (jpeg->decoder->id() != "libjpeg-turbo") {
    return fail("JPEG did not select libjpeg-turbo before WIC.");
  }
#endif
  auto jpeg_without_wic = awj::select_decoder_for_path("sample.jfif",
                                                       {.allow_wic_fallback = false});
  if (!jpeg_without_wic || jpeg_without_wic->fallback) {
    return fail("JPEG series should be native with WIC fallback disabled.");
  }

  for (const char* path : {"sample.bmp", "sample.dib", "sample.rle"}) {
    auto bmp = awj::select_decoder_for_path(path, {.allow_wic_fallback = false});
    if (!bmp || bmp->fallback || bmp->decoder->id() != "awj-bmp") {
      return fail("BMP series did not select the native BMP decoder.");
    }
  }

  for (const char* path : {"sample.jxr", "sample.wdp", "sample.hdp"}) {
    auto jxr = awj::select_decoder_for_path(path, {.allow_wic_fallback = false});
#if AWJ_HAS_WINDOWS_CODECS
    if (!jxr || jxr->fallback || jxr->decoder->id() != "windows-jxr") {
      return fail("JXR series did not select the Windows native JXR decoder.");
    }
#else
    if (jxr) {
      return fail("Linux unexpectedly registered the Windows JXR decoder.");
    }
#endif
  }

  auto gif = awj::select_decoder_for_path("sample.gif", {.allow_wic_fallback = true});
  if (!gif || gif->fallback || gif->decoder->id() != "giflib") {
    return fail("GIF did not select giflib before WIC.");
  }

  auto tiff = awj::select_decoder_for_path("sample.tiff", {.allow_wic_fallback = true});
  if (!tiff || tiff->fallback || tiff->decoder->id() != "libtiff") {
    return fail("TIFF did not select libtiff before WIC.");
  }

  auto raw = awj::select_decoder_for_path("sample.awsraw", {.allow_wic_fallback = true});
#if AWJ_HAS_AWJ_RAW_CODEC
  if (!raw || raw->fallback || raw->decoder->id() != "awj-raw") {
    return fail("AWJ raw did not select internal raw decoder.");
  }
#else
  if (raw) {
    return fail("Build without AWJ raw support unexpectedly registered its decoder.");
  }
#endif

  auto camera_raw = awj::select_decoder_for_path("sample.dng", {.allow_wic_fallback = true});
  if (!camera_raw || camera_raw->fallback || camera_raw->decoder->id() != "libraw") {
    return fail("Camera RAW did not select LibRaw before WIC.");
  }

  auto heif = awj::select_decoder_for_path("sample.heif", {.allow_wic_fallback = true});
  if (!heif || heif->fallback || heif->decoder->id() != "libheif-libde265") {
    return fail("HEIF did not select the native libheif/libde265 decoder.");
  }

  auto ico = awj::select_decoder_for_path("sample.ico", {.allow_wic_fallback = true});
#if AWJ_HAS_WINDOWS_CODECS
  if (!ico || !ico->fallback || ico->decoder->id() != "wic") {
    return fail("ICO did not route to WIC fallback when enabled.");
  }
#else
  if (ico) {
    return fail("Linux unexpectedly accepted ICO through WIC fallback.");
  }
#endif

  auto disabled = awj::select_decoder_for_path("sample.heif", {.allow_wic_fallback = false});
  if (!disabled || disabled->fallback || disabled->decoder->id() != "libheif-libde265") {
    return fail("HEIF native decoder disappeared when WIC fallback was disabled.");
  }

  return 0;
}
