#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

import awj.bmp_codec;
import awj.image;
import awj.large_image_plan;

namespace {

[[noreturn]] void terminate_test_process(int exit_code) noexcept {
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(exit_code);
}

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  terminate_test_process(1);
}

void append_u8(std::vector<std::byte>& bytes, std::uint8_t value) {
  bytes.push_back(std::byte{value});
}

void append_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
  append_u8(bytes, static_cast<std::uint8_t>(value & 0xFFu));
  append_u8(bytes, static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  append_u8(bytes, static_cast<std::uint8_t>(value & 0xFFu));
  append_u8(bytes, static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
  append_u8(bytes, static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
  append_u8(bytes, static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

void append_i32(std::vector<std::byte>& bytes, std::int32_t value) {
  append_u32(bytes, static_cast<std::uint32_t>(value));
}

void append_bytes(std::vector<std::byte>& bytes, std::initializer_list<std::uint8_t> values) {
  for (const auto value : values) {
    append_u8(bytes, value);
  }
}

void append_palette_entry(std::vector<std::byte>& bytes,
                          std::uint8_t r,
                          std::uint8_t g,
                          std::uint8_t b) {
  append_u8(bytes, b);
  append_u8(bytes, g);
  append_u8(bytes, r);
  append_u8(bytes, 0);
}

void append_dib_header(std::vector<std::byte>& bytes,
                       std::int32_t width,
                       std::int32_t height,
                       std::uint16_t bpp,
                       std::uint32_t compression,
                       std::uint32_t image_size,
                       std::uint32_t colors_used) {
  append_u32(bytes, 40);
  append_i32(bytes, width);
  append_i32(bytes, height);
  append_u16(bytes, 1);
  append_u16(bytes, bpp);
  append_u32(bytes, compression);
  append_u32(bytes, image_size);
  append_i32(bytes, 2835);
  append_i32(bytes, 2835);
  append_u32(bytes, colors_used);
  append_u32(bytes, 0);
}

std::vector<std::byte> with_bmp_file_header(std::uint32_t pixel_offset,
                                            std::vector<std::byte> dib_and_pixels) {
  std::vector<std::byte> bytes;
  const auto file_size = static_cast<std::uint32_t>(14 + dib_and_pixels.size());
  append_u8(bytes, static_cast<std::uint8_t>('B'));
  append_u8(bytes, static_cast<std::uint8_t>('M'));
  append_u32(bytes, file_size);
  append_u16(bytes, 0);
  append_u16(bytes, 0);
  append_u32(bytes, pixel_offset);
  bytes.insert(bytes.end(), dib_and_pixels.begin(), dib_and_pixels.end());
  return bytes;
}

std::vector<std::byte> make_bmp_24() {
  std::vector<std::byte> dib;
  append_dib_header(dib, 2, 2, 24, 0, 16, 0);
  append_bytes(dib, {255, 0, 0, 255, 255, 255, 0, 0});  // bottom: blue, white
  append_bytes(dib, {0, 0, 255, 0, 255, 0, 0, 0});  // top: red, green
  return with_bmp_file_header(54, std::move(dib));
}

std::vector<std::byte> make_dib_8() {
  std::vector<std::byte> bytes;
  append_dib_header(bytes, 2, 1, 8, 0, 4, 2);
  append_palette_entry(bytes, 0, 0, 0);
  append_palette_entry(bytes, 0, 255, 0);
  append_bytes(bytes, {0, 1, 0, 0});
  return bytes;
}

std::vector<std::byte> make_rle8() {
  std::vector<std::byte> dib;
  append_dib_header(dib, 4, 1, 8, 1, 8, 4);
  append_palette_entry(dib, 255, 0, 0);
  append_palette_entry(dib, 0, 255, 0);
  append_palette_entry(dib, 0, 0, 255);
  append_palette_entry(dib, 255, 255, 255);
  append_bytes(dib, {0, 4, 0, 1, 2, 3, 0, 1});
  return with_bmp_file_header(70, std::move(dib));
}

std::vector<std::byte> make_rle4() {
  std::vector<std::byte> dib;
  append_dib_header(dib, 4, 1, 4, 2, 6, 4);
  append_palette_entry(dib, 255, 0, 0);
  append_palette_entry(dib, 0, 255, 0);
  append_palette_entry(dib, 0, 0, 255);
  append_palette_entry(dib, 255, 255, 255);
  append_bytes(dib, {0, 4, 0x01, 0x23, 0, 1});
  return with_bmp_file_header(70, std::move(dib));
}

bool write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream output{path, std::ios::binary};
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

const std::uint8_t* rgba_data(const awj::ImageBuffer& image) {
  if (image.planes.empty()) {
    return nullptr;
  }
  return reinterpret_cast<const std::uint8_t*>(image.planes.front().bytes.data());
}

bool pixel_is(const awj::ImageBuffer& image,
              std::size_t x,
              std::size_t y,
              std::uint8_t r,
              std::uint8_t g,
              std::uint8_t b,
              std::uint8_t a = 255) {
  const auto* pixels = rgba_data(image);
  if (pixels == nullptr || image.planes.front().stride < image.width * 4) {
    return false;
  }
  const auto offset = y * image.planes.front().stride + x * 4;
  return pixels[offset] == r && pixels[offset + 1] == g &&
         pixels[offset + 2] == b && pixels[offset + 3] == a;
}

int decode_and_check(const std::filesystem::path& path,
                     const std::vector<std::byte>& bytes,
                     std::string_view label,
                     std::uint32_t expected_width,
                     std::uint32_t expected_height) {
  if (!write_file(path, bytes)) {
    return fail("Could not write temporary BMP test input.");
  }
  awj::BmpImageDecoder decoder;
  auto dimensions = decoder.probe_dimensions(path);
  if (!dimensions || dimensions->width != expected_width ||
      dimensions->height != expected_height || dimensions->source_bit_depth != 8 ||
      dimensions->source_dynamic_range != awj::ImageDynamicRangeHint::sdr ||
      dimensions->decoded_storage_bytes != static_cast<std::uint64_t>(expected_width) * expected_height * 4) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return fail(dimensions ? "BMP dimensions invalid." : dimensions.error());
  }
  auto decoded = decoder.decode(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!decoded || decoded->decoder_id != "awj-bmp" ||
      decoded->image.width != expected_width || decoded->image.height != expected_height ||
      decoded->image.pixel_format != awj::PixelFormat::rgba ||
      decoded->image.bit_depth != 8 || decoded->image.planes.empty()) {
    return fail(decoded ? "BMP decode result invalid." : decoded.error());
  }
  if (label == "bmp24") {
    if (!pixel_is(decoded->image, 0, 0, 255, 0, 0) ||
        !pixel_is(decoded->image, 1, 0, 0, 255, 0) ||
        !pixel_is(decoded->image, 0, 1, 0, 0, 255) ||
        !pixel_is(decoded->image, 1, 1, 255, 255, 255)) {
      return fail("24-bit BMP pixels invalid.");
    }
  } else if (label == "dib8") {
    if (!pixel_is(decoded->image, 0, 0, 0, 0, 0) ||
        !pixel_is(decoded->image, 1, 0, 0, 255, 0)) {
      return fail("8-bit DIB pixels invalid.");
    }
  } else {
    if (!pixel_is(decoded->image, 0, 0, 255, 0, 0) ||
        !pixel_is(decoded->image, 1, 0, 0, 255, 0) ||
        !pixel_is(decoded->image, 2, 0, 0, 0, 255) ||
        !pixel_is(decoded->image, 3, 0, 255, 255, 255)) {
      return fail("RLE BMP pixels invalid.");
    }
  }
  return 0;
}

}  // namespace

int main() {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif

  const auto temp = std::filesystem::temp_directory_path();
  if (const auto code = decode_and_check(temp / "awj-bmp-24-test.bmp",
                                         make_bmp_24(),
                                         "bmp24",
                                         2,
                                         2);
      code != 0) {
    return code;
  }
  if (const auto code = decode_and_check(temp / "awj-dib-8-test.dib",
                                         make_dib_8(),
                                         "dib8",
                                         2,
                                         1);
      code != 0) {
    return code;
  }
  if (const auto code = decode_and_check(temp / "awj-rle8-test.rle",
                                         make_rle8(),
                                         "rle8",
                                         4,
                                         1);
      code != 0) {
    return code;
  }
  if (const auto code = decode_and_check(temp / "awj-rle4-test.rle",
                                         make_rle4(),
                                         "rle4",
                                         4,
                                         1);
      code != 0) {
    return code;
  }
  return 0;
}
