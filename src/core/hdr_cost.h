#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace awj::hdr_cost {

inline constexpr std::size_t tone_lut_entries = 16'385;
inline constexpr std::size_t gamut_block_pixels = 1'024;
inline constexpr std::uint64_t tone_lut_bytes = tone_lut_entries * sizeof(float);
// Three IPT floats and a source pixel index, not a full-image IPT allocation.
inline constexpr std::uint64_t tone_pixel_scratch_bytes = 3 * sizeof(float) + sizeof(std::size_t);
inline constexpr std::uint64_t codec_fixed_bytes = 32ull * 1024 * 1024;

constexpr std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept {
  return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

constexpr std::uint64_t multiply(std::uint64_t a, std::uint64_t b) noexcept {
  return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

constexpr std::uint64_t tone_scratch_bytes(
    std::uint64_t workers, bool with_lut = true,
    std::uint64_t block_pixels = gamut_block_pixels) noexcept {
  // Small vector/thread descriptors and the fixed per-worker control state.
  const auto per_worker = add(multiply(block_pixels, tone_pixel_scratch_bytes), 1'024);
  return add(with_lut ? tone_lut_bytes : 0, multiply(std::max<std::uint64_t>(1, workers), per_worker));
}

constexpr std::uint64_t padded_pixels(std::uint64_t width, std::uint64_t height,
                                      std::uint64_t alignment) noexcept {
  if (!width || !height || !alignment || width > UINT64_MAX - alignment + 1 ||
      height > UINT64_MAX - alignment + 1) return UINT64_MAX;
  width = ((width + alignment - 1) / alignment) * alignment;
  height = ((height + alignment - 1) / alignment) * alignment;
  return multiply(width, height);
}

constexpr std::uint64_t jpeg_source_reservation_bytes(std::uint64_t width,
    std::uint64_t height, std::uint64_t source_bytes = 0) noexcept {
  const auto rgba16 = multiply(multiply(width, height), 8);
  // Native YUV/geometry conversion can retain the original decoded image while
  // the wrapper owns a normalized RGBA16 intent. Reserve both, even if a packed
  // scRGB caller can avoid that extra normalization buffer.
  return add(std::max(source_bytes, rgba16), rgba16);
}

// Ordinary SDR JPEGli has no canonical HDR reference, gain generation or ISO
// wrapper. Include coefficient storage, normalization and entropy/output growth
// separately from that HDR pipeline. This remains a reservation estimate, not
// a libjpeg allocation limit; 4:4:4 and progressive coefficient storage need
// headroom even for an easily compressed image.
constexpr std::uint64_t jpeg_sdr_working_set_bytes(
    std::uint64_t width, std::uint64_t height, std::uint64_t compressed_bytes = 0,
    std::uint64_t source_bytes = 0, bool native_yuv420 = false) noexcept {
  if (!width || !height) return UINT64_MAX;
  const auto n = multiply(width, height);
  const auto padded = padded_pixels(width, height, 16);
  const auto source = source_bytes ? source_bytes : multiply(n, 4);
  const auto normalized = multiply(padded, native_yuv420 ? 2 : 4);
  const auto coefficients = multiply(padded, 12);
  const auto stream = multiply(padded, native_yuv420 ? 4 : 6);
  const auto encode = add(add(source, normalized), add(coefficients, stream));
  // Growing libjpeg output and a metadata rewrite can retain old/new streams;
  // these copies need not coexist with the coefficient workspace after finish.
  const auto output = add(add(source, normalized), multiply(stream, 3));
  return add(add(std::max(encode, output), 8ull * 1024 * 1024),
             multiply(compressed_bytes, 2));
}

// A reservation estimate for the maximum simultaneously-live HDR JPEG phase,
// not an allocator/RSS hard bound. Keep the phases in step with jpeg_hdr_codec:
// canonical/reference are freed before gain JPEG encoding; raw gain pixels are
// freed before the ISO wrapper copies compressed streams. An existing padded
// source and its metadata can supply their true byte size.
constexpr std::uint64_t jpeg_working_set_bytes(
    std::uint64_t width, std::uint64_t height, std::uint64_t compressed_bytes = 0,
    std::uint64_t tone_workers = 1, std::uint64_t source_bytes = 0) noexcept {
  if (!width || !height) return UINT64_MAX;
  const auto n = multiply(width, height);
  const auto padded = padded_pixels(width, height, 16);
  const auto scale = std::min<std::uint64_t>({4, width, height});
  const auto gain_width = std::max<std::uint64_t>(1, width / scale);
  const auto gain_height = std::max<std::uint64_t>(1, height / scale);
  // The engine aligns gain row width (not image height) to 64 pixels. Its
  // raw map is freed before JPEGli, whose gain stream is always 4:4:4 / DCT8.
  const auto gain_row = gain_width > UINT64_MAX - 63 ? UINT64_MAX : (gain_width + 63) & ~63ull;
  const auto gain_storage = multiply(gain_row, gain_height);
  const auto gain_pixels = multiply(gain_width, gain_height);
  const auto gain_codec = padded_pixels(gain_width, gain_height, 8);
  const auto source = jpeg_source_reservation_bytes(width, height, source_bytes);
  const auto rgba8 = multiply(n, 4);
  const auto canonical = multiply(n, 8);
  const auto base_jpeg = multiply(padded, 6);
  const auto gain_jpeg = multiply(gain_codec, 6);
  const auto tone = add(add(source, rgba8), tone_scratch_bytes(tone_workers));
  const auto base_encode = add(add(add(source, rgba8), base_jpeg), multiply(padded, 32));
  const auto base_decode = add(add(add(source, multiply(rgba8, 2)), base_jpeg), multiply(padded, 16));
  const auto gain_generate = add(add(add(add(source, rgba8), canonical), base_jpeg),
                                 add(multiply(padded, 24), multiply(gain_storage, 3)));
  const auto gain_copy = add(add(source, base_jpeg), add(multiply(gain_storage, 3), multiply(gain_pixels, 4)));
  const auto gain_encode = add(add(source, base_jpeg),
                               add(multiply(gain_pixels, 4), add(multiply(gain_codec, 32), gain_jpeg)));
  // Original JPEGs + wrapper copies + composite + final vector during assign.
  const auto assemble = add(source, multiply(add(base_jpeg, gain_jpeg), 4));
  const auto peak = std::max({tone, base_encode, base_decode, gain_generate, gain_copy, gain_encode, assemble});
  return add(add(peak, codec_fixed_bytes), multiply(compressed_bytes, 2));
}

constexpr std::uint64_t jpeg_assembly_bytes(std::uint64_t source_bytes,
    std::uint64_t base_bytes, std::uint64_t gain_bytes, std::uint64_t stream_bytes) noexcept {
  return add(codec_fixed_bytes, add(source_bytes,
      add(multiply(add(base_bytes, gain_bytes), 2), multiply(stream_bytes, 2))));
}

}  // namespace awj::hdr_cost
