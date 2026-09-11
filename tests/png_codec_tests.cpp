#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <span>
#include <stop_token>
#include <string>
#include <vector>
#include <zlib.h>

import awj.codec;
import awj.config;
import awj.image;
import awj.png_codec;
import awj.visual_metrics;

namespace fs = std::filesystem;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class T> T require(std::expected<T, std::string> result) {
  if (!result) throw std::runtime_error(result.error());
  return std::move(*result);
}
std::uint16_t sample(const awj::ImageBuffer& image, std::size_t i) {
  const auto* ptr = image.planes[0].bytes.data() + i * (image.bit_depth / 8);
  std::uint16_t value{};
  if (image.bit_depth == 16) std::memcpy(&value, ptr, 2);
  else value = std::to_integer<std::uint8_t>(*ptr);
  return value;
}
void put_sample(awj::ImageBuffer& image, std::size_t i, unsigned input) {
  const auto value = static_cast<std::uint16_t>(input);
  auto* ptr = image.planes[0].bytes.data() + i * (image.bit_depth / 8);
  if (image.bit_depth == 16) std::memcpy(ptr, &value, 2);
  else *ptr = static_cast<std::byte>(value);
}
awj::ImageBuffer make_image(int depth) {
  awj::ImageBuffer image{.width = 256, .height = 256,
      .pixel_format = awj::PixelFormat::rgba, .alpha_mode = awj::AlphaMode::straight,
      .bit_depth = depth};
  image.planes.push_back({.bytes = std::vector<std::byte>(256 * 256 * 4 * (depth / 8)),
                         .stride = static_cast<std::size_t>(256 * 4 * (depth / 8))});
  const auto full = (1u << depth) - 1;
  for (unsigned i = 0; i < 65536; ++i) {
    put_sample(image, i * 4, i & full);
    put_sample(image, i * 4 + 1, (full - i) & full);
    put_sample(image, i * 4 + 2, (i * 31337u) & full);
    put_sample(image, i * 4 + 3, (i * 1777u) & full);
  }
  return image;
}
awj::ImageBuffer roundtrip(const awj::ImageBuffer& image, int quality, const fs::path& path) {
  auto encoded = require(awj::PngImageEncoder{}.encode(image, {.output_format = awj::OutputFormat::png, .quality = quality}));
  check(encoded.final_quality == quality, "PNG quality was discarded");
  if (quality == 100) check(encoded.lossless, "q100 reported lossy");
  check(std::to_integer<int>(encoded.encoded.bytes[24]) == image.bit_depth, "IHDR storage depth changed");
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded.encoded.bytes.data()), encoded.encoded.bytes.size());
    check(static_cast<bool>(output), "write failed");
  }
  return require(awj::PngImageDecoder{}.decode(path)).image;
}

void check_expanded_precision(const fs::path& path, bool palette) {
  std::vector<std::byte> bytes{std::byte{137}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
      std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
  std::array<std::byte, 13> header{};
  header[3] = header[7] = std::byte{1};
  header[8] = std::byte{2}; // 2-bit grayscale or palette index, expanded to RGBA8.
  header[9] = palette ? std::byte{3} : std::byte{0};
  awj::png_detail::append_png_chunk(bytes, "IHDR", header);
  const std::array<std::byte, 3> color_bits{std::byte{5}, std::byte{6}, std::byte{7}};
  const std::array<std::byte, 1> gray_bits{std::byte{2}};
  if (palette) {
    awj::png_detail::append_png_chunk(bytes, "sBIT", color_bits);
    const std::array<std::byte, 3> color{std::byte{99}, std::byte{133}, std::byte{199}};
    awj::png_detail::append_png_chunk(bytes, "PLTE", color);
    const std::array<std::byte, 1> alpha{std::byte{117}};
    awj::png_detail::append_png_chunk(bytes, "tRNS", alpha);
  } else awj::png_detail::append_png_chunk(bytes, "sBIT", gray_bits);
  const std::array<unsigned char, 2> raw{0, static_cast<unsigned char>(palette ? 0 : 0x80)};
  std::array<std::byte, 64> compressed{};
  uLongf count = compressed.size();
  check(compress(reinterpret_cast<Bytef*>(compressed.data()), &count, raw.data(), raw.size()) == Z_OK,
        "fixture deflate failed");
  awj::png_detail::append_png_chunk(bytes, "IDAT", std::span{compressed}.first(count));
  awj::png_detail::append_png_chunk(bytes, "IEND", {});
  {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(static_cast<bool>(out), "fixture write failed");
  }
  const auto decoded = require(awj::PngImageDecoder{}.decode(path)).image;
  check(decoded.bit_depth == 8 && decoded.significant_bits.has_value(), "expanded sBIT missing");
  const auto bits = *decoded.significant_bits;
  check(bits.red == (palette ? 5 : 2) && bits.green == (palette ? 6 : 2) &&
        bits.blue == (palette ? 7 : 2) && bits.alpha == 8, "expanded sBIT mapping incorrect");
  check(sample(decoded, 3) == (palette ? 117 : 255), "tRNS/filler alpha changed");
}

// Optional reproducible measurement path; uses the actual encoder and project metrics.
int benchmark(const fs::path& inputs, const fs::path& outputs) {
  check(fs::is_directory(inputs) && !fs::exists(outputs), "benchmark needs inputs and a fresh output directory");
  fs::create_directories(outputs);
  std::ofstream csv{outputs / "metrics.csv"};
  csv << "sample,depth,quality,rgb_bits,bytes,encode_ms,psnr_rgb_code_values,gmsd,ms_ssim\n";
  for (const auto& file : fs::directory_iterator(inputs)) {
    if (file.path().extension() != ".png") continue;
    const auto image = require(awj::PngImageDecoder{}.decode(file.path())).image;
    const auto reference = require(awj::make_luma_image(image));
    for (int quality : {1, 25, 50, 75, 90, 99, 100}) {
      const auto started = std::chrono::steady_clock::now();
      const auto encoded = require(awj::PngImageEncoder{}.encode(image, {.quality = quality}));
      const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      const auto path = outputs / (file.path().stem().string() + "-q" + std::to_string(quality) + ".png");
      {
        std::ofstream out{path, std::ios::binary};
        out.write(reinterpret_cast<const char*>(encoded.encoded.bytes.data()), encoded.encoded.bytes.size());
        check(static_cast<bool>(out), "benchmark output failed");
      }
      const auto decoded = require(awj::PngImageDecoder{}.decode(path)).image;
      const auto metrics = require(awj::calculate_visual_metrics_cpu(reference, require(awj::make_luma_image(decoded))));
      double squared = 0;
      for (std::size_t i = 0; i < image.width * image.height; ++i) {
        check(sample(image, i * 4 + 3) == sample(decoded, i * 4 + 3), "benchmark alpha changed");
        for (unsigned c = 0; c < 3; ++c) {
          const double difference = double(sample(image, i * 4 + c)) - sample(decoded, i * 4 + c);
          squared += difference * difference;
        }
      }
      const double mse = squared / (3.0 * image.width * image.height);
      const double full = (1u << image.bit_depth) - 1;
      const double psnr = mse == 0 ? INFINITY : 10 * std::log10(full * full / mse);
      csv << file.path().stem().string() << ',' << image.bit_depth << ',' << quality << ','
          << (decoded.significant_bits ? decoded.significant_bits->red : decoded.bit_depth) << ','
          << encoded.encoded.bytes.size() << ',' << ms << ',' << psnr << ','
          << metrics.raw_gmsd << ',' << metrics.raw_ms_ssim << '\n';
      csv.flush();
    }
  }
  check(static_cast<bool>(csv), "benchmark CSV failed");
  return 0;
}

int main(int argc, char** argv) try {
  if (argc == 4 && std::string_view{argv[1]} == "--benchmark") return benchmark(argv[2], argv[3]);
  const auto directory = fs::temp_directory_path() /
      ("awj-png-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  check(fs::create_directory(directory), "test directory not isolated");
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{directory};
  const auto path = directory / "roundtrip.png";
  check_expanded_precision(path, false);
  check_expanded_precision(path, true);
  awj::PngImageEncoder encoder;
  check(encoder.capabilities().min_quality == 1 && encoder.capabilities().max_quality == 100,
        "PNG quality capability incorrect");
  for (int depth : {8, 16}) {
    auto image = make_image(depth);
    const auto original = image.planes[0].bytes;
    for (int quality : {1, 25, 50, 75, 99, 100}) {
      const auto decoded = roundtrip(image, quality, path);
      check(image.planes[0].bytes == original, "encoder changed caller pixels");
      if (quality == 100) {
        check(decoded.planes[0].bytes == original, "q100 pixels differ");
        check(!decoded.significant_bits, "q100 invented precision metadata");
        continue;
      }
      check(decoded.significant_bits.has_value(), "quantized PNG lacks sBIT");
      const auto bits = decoded.significant_bits->red;
      check(bits >= (depth == 16 ? 10 : 4) && bits < depth &&
            decoded.significant_bits->green == bits && decoded.significant_bits->blue == bits &&
            decoded.significant_bits->alpha == depth, "invalid channel precision");
      const unsigned full = (1u << depth) - 1;
      const unsigned levels = (1u << bits) - 1;
      for (unsigned i = 0; i < 65536; ++i) {
        check(sample(decoded, i * 4 + 3) == sample(image, i * 4 + 3), "alpha changed");
        for (unsigned c = 0; c < 3; ++c) {
          const auto a = sample(image, i * 4 + c), b = sample(decoded, i * 4 + c);
          check(std::abs(int(a) - int(b)) <= int((full + levels) / (2 * levels) + 1), "rounding error exceeds bound");
          if (a == 0 || a == full) check(a == b, "endpoint changed");
          check((b >> (depth - bits)) == (std::uint64_t(a) * levels + full / 2) / full,
                "sBIT does not describe recoverable high bits");
        }
      }
      auto reread = roundtrip(decoded, 100, path);
      check(reread.planes[0].bytes == decoded.planes[0].bytes &&
            reread.significant_bits->red == bits, "q100 lost sBIT or changed quantized pixels");
    }
    auto reduced = roundtrip(image, 1, path);
    reduced.significant_bits->green = depth;
    reduced.significant_bits->alpha = depth - 1;
    auto reread = roundtrip(reduced, 100, path);
    check(reread.significant_bits->red == reduced.significant_bits->red &&
          reread.significant_bits->green == depth && reread.significant_bits->alpha == depth - 1,
          "per-channel sBIT was collapsed");
    image.significant_bits = awj::SignificantBits{0, depth, depth, depth};
    check(!encoder.encode(image, {.quality = 100}), "invalid precision accepted");
    image.significant_bits.reset();
    image.sample_representation = awj::SampleRepresentation::ieee_half_float;
    check(!encoder.encode(image, {.quality = 100}), "half float treated as unorm");
    image.sample_representation = awj::SampleRepresentation::unorm;
    check(!encoder.encode(image, {.quality = 0}) && !encoder.encode(image, {.quality = 101}), "invalid quality accepted");
    std::stop_source cancel;
    cancel.request_stop();
    check(!encoder.encode(image, {.quality = 50}, cancel.get_token()), "cancellation ignored");
  }
  std::puts("PNG precision, lossless pixels, 16-bit storage, alpha, sBIT, endpoints and cancellation passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
