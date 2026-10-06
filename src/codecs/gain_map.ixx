module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <ultrahdr_api.h>
#include <avif/avif.h>
#include <lcms2.h>
#include <libplacebo/colorspace.h>
#include <ultrahdr/jpegrutils.h>
#include "uhdr_thread_adapter.h"

export module awj.gain_map;

import awj.decoder_common;
import awj.hdr_tonemap;
import awj.image;

export namespace awj {

namespace gain_map_detail {

std::expected<void, std::string> transform_rgba(ImageBuffer& image, unsigned ccw_quarters, int mirror = -1) {
  if (auto valid = hdr::hdr_detail::validate_rgba(image, "Gain Map orientation"); !valid) return valid;
  ccw_quarters %= 4;
  if (!ccw_quarters && mirror < 0) return {};
  const auto width = ccw_quarters & 1 ? image.height : image.width;
  const auto height = ccw_quarters & 1 ? image.width : image.height;
  const auto sample_bytes = image.bit_depth == 16 ? 2u : 1u;
  auto bytes = hdr::hdr_detail::make_rgba_bytes(width, height, sample_bytes, "Gain Map orientation");
  if (!bytes) return std::unexpected{bytes.error()};
  const auto pixel_bytes = sample_bytes * 4;
  for (std::size_t y = 0; y < image.height; ++y) for (std::size_t x = 0; x < image.width; ++x) {
    std::size_t dx = x, dy = y;
    if (ccw_quarters == 1) { dx = y; dy = image.width - 1 - x; }
    if (ccw_quarters == 2) { dx = image.width - 1 - x; dy = image.height - 1 - y; }
    if (ccw_quarters == 3) { dx = image.height - 1 - y; dy = x; }
    if (mirror == 0) dy = height - 1 - dy;
    if (mirror == 1) dx = width - 1 - dx;
    std::memcpy(bytes->data() + (dy * width + dx) * pixel_bytes,
        image.planes.front().bytes.data() + y * image.planes.front().stride + x * pixel_bytes, pixel_bytes);
  }
  image.width = width; image.height = height;
  image.planes.front() = {std::move(*bytes), width * pixel_bytes};
  return {};
}

void normalize_exif_orientation(ImageBuffer& image) noexcept {
  for (auto& block : image.metadata) if (block.kind == MetadataKind::exif) {
    std::size_t offset{};
    if (avifGetExifOrientationOffset(reinterpret_cast<const std::uint8_t*>(block.bytes.data()),
        block.bytes.size(), &offset) == AVIF_RESULT_OK && offset < block.bytes.size()) block.bytes[offset] = std::byte{1};
  }
}

std::expected<void, std::string> apply_exif_orientation(ImageBuffer& image) {
  for (auto& block : image.metadata) if (block.kind == MetadataKind::exif) {
    std::size_t offset{};
    if (avifGetExifOrientationOffset(reinterpret_cast<const std::uint8_t*>(block.bytes.data()),
        block.bytes.size(), &offset) != AVIF_RESULT_OK || offset >= block.bytes.size()) continue;
    const auto orientation = std::to_integer<unsigned>(block.bytes[offset]);
    unsigned rotation = 0; int mirror = -1;
    switch (orientation) {
      case 1: break;
      case 2: mirror = 1; break;
      case 3: rotation = 2; break;
      case 4: mirror = 0; break;
      case 5: rotation = 1; mirror = 0; break;
      case 6: rotation = 3; break;
      case 7: rotation = 1; mirror = 1; break;
      case 8: rotation = 1; break;
      default: return std::unexpected{"Gain Map: Exif orientation 无效。"};
    }
    auto transformed = transform_rgba(image, rotation, mirror);
    if (!transformed) return transformed;
    normalize_exif_orientation(image);
    break;
  }
  return {};
}

std::expected<void, std::string> crop_rgba(ImageBuffer& image, std::size_t x, std::size_t y,
    std::size_t width, std::size_t height) {
  if (!width || !height || x > image.width || y > image.height ||
      width > image.width - x || height > image.height - y) return std::unexpected{"Gain Map: crop 越界。"};
  const auto sample_bytes = image.bit_depth == 16 ? 2u : 1u;
  auto bytes = hdr::hdr_detail::make_rgba_bytes(width, height, sample_bytes, "Gain Map crop");
  if (!bytes) return std::unexpected{bytes.error()};
  const auto stride = width * 4 * sample_bytes;
  for (std::size_t row = 0; row < height; ++row)
    std::memcpy(bytes->data() + row * stride,
        image.planes.front().bytes.data() + (y + row) * image.planes.front().stride + x * 4 * sample_bytes, stride);
  image.width = width; image.height = height; image.planes.front() = {std::move(*bytes), stride};
  return {};
}

std::string uhdr_error(const uhdr_error_info_t& error) {
  return std::string{"Gain Map: libultrahdr: "} +
      (error.has_detail ? error.detail : "unsupported or invalid image");
}

// Inspect APP payloads (including the secondary JPEG), never entropy bytes.
bool jpeg_has_gain_map_marker(std::span<const std::byte> bytes) {
  if (bytes.size() < 2 || bytes[0] != std::byte{0xff} || bytes[1] != std::byte{0xd8}) return false;
  for (std::size_t pos = 2; pos + 1 < bytes.size();) {
    if (bytes[pos++] != std::byte{0xff}) continue;
    auto marker = std::to_integer<unsigned char>(bytes[pos++]);
    if (marker == 0xff) { --pos; continue; }
    if (marker == 0 || marker == 0xd8 || marker == 0xd9 || marker == 1 ||
        (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (pos + 2 > bytes.size()) break;
    const auto length = (std::to_integer<unsigned>(bytes[pos]) << 8) |
                        std::to_integer<unsigned>(bytes[pos + 1]);
    if (length < 2 || length > bytes.size() - pos) break;
    if (marker >= 0xe0 && marker <= 0xef) {
      const std::string_view payload{reinterpret_cast<const char*>(bytes.data() + pos + 2), length - 2};
      if (payload.find("urn:iso:std:iso:ts:21496:-1") != payload.npos ||
          payload.find("http://ns.adobe.com/hdr-gain-map/1.0/") != payload.npos ||
          (payload.find("http://ns.apple.com/HDRGainMap/1.0/") != payload.npos &&
           payload.find("HDRGainMapVersion") != payload.npos) ||
          (payload.find("http://ns.google.com/photos/1.0/container/") != payload.npos &&
           payload.find("GainMap") != payload.npos)) return true;
    }
    pos += length;
  }
  return false;
}

} // namespace gain_map_detail

// Ordinary JPEGs only need their short pre-SOS header inspected. MPF is a
// candidate, not proof: stereo/burst JPEGs may contain no gain map.
bool jpeg_may_contain_gain_map(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  auto read_byte = [&]() { return input.get(); };
  if (read_byte() != 0xff || read_byte() != 0xd8) return false;
  std::vector<std::byte> segment{std::byte{0xff}, std::byte{0xd8}};
  for (;;) {
    if (read_byte() != 0xff) return false;
    int marker;
    do { marker = read_byte(); } while (marker == 0xff);
    if (marker < 0 || marker == 0xda || marker == 0xd9) return false;
    if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    const auto high = read_byte(), low = read_byte();
    if (high < 0 || low < 0) return false;
    const auto length = (high << 8) | low;
    if (length < 2) return false;
    if (marker >= 0xe0 && marker <= 0xef) {
      segment.resize(static_cast<std::size_t>(length) + 4);
      segment[2] = std::byte{0xff}; segment[3] = std::byte{static_cast<unsigned char>(marker)};
      segment[4] = std::byte{static_cast<unsigned char>(high)};
      segment[5] = std::byte{static_cast<unsigned char>(low)};
      input.read(reinterpret_cast<char*>(segment.data() + 6), length - 2);
      if (!input) return false;
      if (marker == 0xe2 && length >= 6 && std::memcmp(segment.data() + 6, "MPF\0", 4) == 0) return true;
      if (gain_map_detail::jpeg_has_gain_map_marker(segment)) return true;
    } else {
      input.seekg(length - 2, std::ios::cur);
      if (!input) return false;
    }
  }
}

std::expected<ImageBuffer, std::string> compose_iso_gain_map(
    const ImageBuffer& base, const ImageBuffer& map, avifGainMap& metadata) {
  if (auto valid = hdr::hdr_detail::validate_rgba(base, "ISO Gain Map base"); !valid) return std::unexpected{valid.error()};
  if (auto valid = hdr::hdr_detail::validate_rgba(map, "ISO Gain Map auxiliary"); !valid) return std::unexpected{valid.error()};
  if (!base.source_info || !base.source_info->color_primaries || !base.source_info->transfer_characteristics ||
      base.sample_representation != SampleRepresentation::unorm || map.sample_representation != SampleRepresentation::unorm)
    return std::unexpected{"Gain Map: ISO 输入缺少明确编码色彩空间。"};
  if (base.alpha_mode == AlphaMode::premultiplied)
    return std::unexpected{"Gain Map: ISO premultiplied base 必须先转换为 straight alpha。"};
  if (std::ranges::any_of(base.metadata, [](const MetadataBlock& b) { return b.kind == MetadataKind::icc; }))
    return std::unexpected{"Gain Map: ISO ICC 合成空间暂不支持，不能按 CICP 猜测。"};
  if (!hdr::hdr_detail::primaries_from_cicp(*base.source_info->color_primaries) ||
      *base.source_info->transfer_characteristics == 0 || *base.source_info->transfer_characteristics == 2)
    return std::unexpected{"Gain Map: ISO base 色彩标签未指定或不支持。"};
  if (!metadata.useBaseColorSpace && !hdr::hdr_detail::primaries_from_cicp(static_cast<int>(metadata.altColorPrimaries)))
    return std::unexpected{"Gain Map: ISO alternate 合成原色未指定或不支持。"};
  using Image = std::unique_ptr<avifImage, decltype(&avifImageDestroy)>;
  const int map_depth = map.bit_depth == 8 ? 8 : std::clamp(map.source_info ? map.source_info->bit_depth : 12, 10, 12);
  Image gain{avifImageCreate(static_cast<std::uint32_t>(map.width), static_cast<std::uint32_t>(map.height),
      map_depth, AVIF_PIXEL_FORMAT_YUV444), &avifImageDestroy};
  if (!gain) return std::unexpected{"Gain Map: 无法创建 ISO map 像素。"};
  gain->colorPrimaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
  gain->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
  gain->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
  gain->yuvRange = AVIF_RANGE_FULL;
  avifRGBImage map_rgb{};
  avifRGBImageSetDefaults(&map_rgb, gain.get());
  map_rgb.format = AVIF_RGB_FORMAT_RGBA; map_rgb.depth = map.bit_depth; map_rgb.ignoreAlpha = AVIF_TRUE;
  map_rgb.pixels = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(map.planes.front().bytes.data()));
  if (map.planes.front().stride > std::numeric_limits<std::uint32_t>::max()) return std::unexpected{"Gain Map: map stride 过大。"};
  map_rgb.rowBytes = static_cast<std::uint32_t>(map.planes.front().stride);
  const auto converted = avifImageRGBToYUV(gain.get(), &map_rgb);
  if (converted != AVIF_RESULT_OK) return std::unexpected{"Gain Map: ISO map 像素转换失败。"};
  metadata.image = gain.get();
  struct Detach { avifGainMap& map; ~Detach() { map.image = nullptr; } } detach{metadata};
  if (!metadata.baseHdrHeadroom.d || !metadata.alternateHdrHeadroom.d) return std::unexpected{"Gain Map: ISO headroom 分母为零。"};
  const float headroom = std::max(static_cast<float>(metadata.baseHdrHeadroom.n) / metadata.baseHdrHeadroom.d,
      static_cast<float>(metadata.alternateHdrHeadroom.n) / metadata.alternateHdrHeadroom.d);
  avifRGBImage base_rgb{};
  base_rgb.width = static_cast<std::uint32_t>(base.width); base_rgb.height = static_cast<std::uint32_t>(base.height);
  base_rgb.format = AVIF_RGB_FORMAT_RGBA; base_rgb.depth = base.bit_depth;
  if (base.planes.front().stride > std::numeric_limits<std::uint32_t>::max()) return std::unexpected{"Gain Map: base stride 过大。"};
  base_rgb.rowBytes = static_cast<std::uint32_t>(base.planes.front().stride);
  base_rgb.pixels = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(base.planes.front().bytes.data()));
  avifRGBImage enhanced{};
  enhanced.format = AVIF_RGB_FORMAT_RGBA; enhanced.depth = 16;
  avifDiagnostics diag{};
  avifContentLightLevelInformationBox clli{};
  const auto applied = avifRGBImageApplyGainMap(&base_rgb,
      static_cast<avifColorPrimaries>(*base.source_info->color_primaries),
      static_cast<avifTransferCharacteristics>(*base.source_info->transfer_characteristics), &metadata,
      headroom, AVIF_COLOR_PRIMARIES_BT2020, AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084,
      &enhanced, &clli, &diag);
  struct Guard { avifRGBImage& rgb; ~Guard() { avifRGBImageFreePixels(&rgb); } } guard{enhanced};
  if (applied != AVIF_RESULT_OK) return std::unexpected{std::string{"Gain Map: ISO 合成失败: "} + diag.error};
  auto pixels = hdr::hdr_detail::make_rgba_bytes(base.width, base.height, 2, "ISO Gain Map");
  if (!pixels) return std::unexpected{pixels.error()};
  for (std::size_t y = 0; y < base.height; ++y)
    std::memcpy(pixels->data() + y * base.width * 8, enhanced.pixels + y * enhanced.rowBytes, base.width * 8);
  auto image = hdr::hdr_detail::make_rgba_image(base.width, base.height, std::move(*pixels), base.alpha_mode,
      16, SampleRepresentation::unorm, ImageSourceInfo{.pixel_format = PixelFormat::rgba, .bit_depth = 16,
      .color_primaries = 9, .transfer_characteristics = 16, .matrix_coefficients = 9, .color_range = 1,
      .content_light = HdrContentLightMetadata{clli.maxCLL, clli.maxPALL}, .has_hdr_metadata = true,
      .color_metadata_source = "gain-map-heif-iso-bt2020-pq", .source_has_gain_map = true}, "ISO Gain Map");
  if (!image) return image;
  for (const auto& block : base.metadata) if (block.kind == MetadataKind::exif) image->metadata.push_back(block);
  return image;
}

std::expected<ImageBuffer, std::string> compose_apple_gain_map(
    const ImageBuffer& base, const ImageBuffer& map, std::stop_token stop = {}) {
  if (auto valid = hdr::hdr_detail::validate_rgba(base, "Apple Gain Map base"); !valid) return std::unexpected{valid.error()};
  if (auto valid = hdr::hdr_detail::validate_rgba(map, "Apple Gain Map auxiliary"); !valid) return std::unexpected{valid.error()};
  if (base.sample_representation != SampleRepresentation::unorm || map.bit_depth != 8 ||
      map.sample_representation != SampleRepresentation::unorm)
    return std::unexpected{"Gain Map: Apple legacy map 必须为 8-bit samples。"};
  const MetadataBlock* xmp = nullptr; const MetadataBlock* exif = nullptr; const MetadataBlock* icc = nullptr;
  for (const auto& block : base.metadata) {
    if (block.kind == MetadataKind::exif) exif = &block;
    if (block.kind == MetadataKind::icc) icc = &block;
  }
  auto find_version = [&](const ImageBuffer& image) {
    for (const auto& block : image.metadata) if (block.kind == MetadataKind::xmp) {
      const std::string_view text{reinterpret_cast<const char*>(block.bytes.data()), block.bytes.size()};
      if (text.find("HDRGainMapVersion") != text.npos) xmp = &block;
    }
  };
  find_version(base); find_version(map);
  if (!xmp || xmp->bytes.size() < 2) return std::unexpected{"Gain Map: Apple 缺少 HDRGainMapVersion 元数据。"};
  // This pinned upstream parser supports Apple XMP headroom and bounded MakerNote 33/48 parsing.
  // It is intentionally isolated here because its C++ API is private upstream.
  const std::string_view prefix{"http://ns.adobe.com/xap/1.0/"};
  std::vector<std::uint8_t> packet(prefix.begin(), prefix.end()); packet.push_back(0);
  for (auto byte : xmp->bytes) packet.push_back(std::to_integer<std::uint8_t>(byte));
  if (exif && exif->bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return std::unexpected{"Gain Map: Apple Exif 太大。"};
  ultrahdr::uhdr_gainmap_metadata_ext_t metadata{};
  const auto parsed = ultrahdr::getMetadataFromXMP(packet.data(), packet.size(),
      exif ? reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(exif->bytes.data())) : nullptr,
      exif ? static_cast<int>(exif->bytes.size()) : 0, &metadata);
  if (parsed.error_code != UHDR_CODEC_OK) return std::unexpected{gain_map_detail::uhdr_error(parsed)};
  const float headroom = std::max(metadata.hdr_capacity_max, 1.0F);
  if (!std::isfinite(headroom) || headroom > 10000.0F / 203.0F) return std::unexpected{"Gain Map: Apple headroom 超出 HDR10 亮度范围。"};
  for (std::size_t y = 0; y < map.height; ++y) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    const auto* row = map.planes.front().bytes.data() + y * map.planes.front().stride;
    for (std::size_t x = 0; x < map.width; ++x)
      if (row[x * 4] != row[x * 4 + 1] || row[x * 4] != row[x * 4 + 2])
        return std::unexpected{"Gain Map: Apple legacy auxiliary 必须为灰度样本。"};
  }
  const auto* source_primaries = base.source_info && base.source_info->color_primaries
      ? hdr::hdr_detail::primaries_from_cicp(*base.source_info->color_primaries) : nullptr;
  const auto* bt2020 = hdr::hdr_detail::primaries_from_cicp(9);
  if (!icc && (!source_primaries || !base.source_info->transfer_characteristics))
    return std::unexpected{"Gain Map: Apple base 缺少 ICC 或明确原色/传递函数。"};
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
  Profile input_profile{nullptr, &cmsCloseProfile}, linear_profile{nullptr, &cmsCloseProfile};
  Transform transform{nullptr, &cmsDeleteTransform};
  if (icc) {
    input_profile.reset(cmsOpenProfileFromMem(icc->bytes.data(), static_cast<cmsUInt32Number>(icc->bytes.size())));
    cmsCIExyY white{0.3127, 0.3290, 1.0};
    cmsCIExyYTRIPLE primaries{{0.708, 0.292, 1.0}, {0.170, 0.797, 1.0}, {0.131, 0.046, 1.0}};
    cmsToneCurve* curve = cmsBuildGamma(nullptr, 1.0);
    if (!curve) return std::unexpected{"Gain Map: 无法创建线性 ICC 曲线。"};
    cmsToneCurve* curves[3]{curve, curve, curve};
    linear_profile.reset(cmsCreateRGBProfile(&white, &primaries, curves));
    cmsFreeToneCurve(curve);
    if (!input_profile || !linear_profile || cmsGetColorSpace(input_profile.get()) != cmsSigRgbData)
      return std::unexpected{"Gain Map: Apple ICC 无效或不是 RGB。"};
    transform.reset(cmsCreateTransform(input_profile.get(), base.bit_depth == 8 ? TYPE_RGBA_8 : TYPE_RGBA_16,
        linear_profile.get(), TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE));
    if (!transform) return std::unexpected{"Gain Map: 无法创建 Apple ICC 线性变换。"};
  }
  auto pixels = hdr::hdr_detail::make_rgba_bytes(base.width, base.height, 2, "Apple Gain Map");
  if (!pixels) return std::unexpected{pixels.error()};
  std::vector<float> linear_row(base.width * 3);
  const auto matrix = source_primaries ? pl_get_color_mapping_matrix(source_primaries, bt2020, PL_INTENT_RELATIVE_COLORIMETRIC) : pl_matrix3x3{};
  auto inverse = [](float sample, int transfer) -> float {
    if (transfer == 13) return sample <= 0.04045F ? sample / 12.92F : std::pow((sample + 0.055F) / 1.055F, 2.4F);
    if (transfer == 1) return sample < 0.081F ? sample / 4.5F : std::pow((sample + 0.099F) / 1.099F, 1.0F / 0.45F);
    if (transfer == 8) return sample;
    return std::numeric_limits<float>::quiet_NaN();
  };
  auto gain_sample = [&](std::size_t x, std::size_t y) {
    const auto* row = map.planes.front().bytes.data() + y * map.planes.front().stride;
    return hdr::hdr_detail::read_sample(map, row, x * 4);
  };
  for (std::size_t y = 0; y < base.height; ++y) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    const auto* row = base.planes.front().bytes.data() + y * base.planes.front().stride;
    if (transform) cmsDoTransform(transform.get(), row, linear_row.data(), static_cast<cmsUInt32Number>(base.width));
    auto* output = pixels->data() + y * base.width * 8;
    const double gy = std::clamp((static_cast<double>(y) + 0.5) * map.height / base.height - 0.5, 0.0, static_cast<double>(map.height - 1));
    const auto y0 = static_cast<std::size_t>(gy), y1 = std::min(y0 + 1, map.height - 1);
    for (std::size_t x = 0; x < base.width; ++x) {
      std::array<float, 3> rgb{};
      if (transform) std::copy_n(linear_row.data() + x * 3, 3, rgb.begin());
      else {
        for (std::size_t c = 0; c < 3; ++c) rgb[c] = inverse(hdr::hdr_detail::read_sample(base, row, x * 4 + c), *base.source_info->transfer_characteristics);
        pl_matrix3x3_apply(&matrix, rgb.data());
      }
      const double gx = std::clamp((static_cast<double>(x) + 0.5) * map.width / base.width - 0.5, 0.0, static_cast<double>(map.width - 1));
      const auto x0 = static_cast<std::size_t>(gx), x1 = std::min(x0 + 1, map.width - 1);
      const float fx = static_cast<float>(gx - x0), fy = static_cast<float>(gy - y0);
      const float encoded = std::lerp(std::lerp(gain_sample(x0, y0), gain_sample(x1, y0), fx),
          std::lerp(gain_sample(x0, y1), gain_sample(x1, y1), fx), fy);
      const float boost = 1.0F + (headroom - 1.0F) * inverse(encoded, 1);
      const float alpha = hdr::hdr_detail::read_sample(base, row, x * 4 + 3);
      for (std::size_t c = 0; c < 3; ++c) {
        if (!std::isfinite(rgb[c])) return std::unexpected{"Gain Map: Apple 色彩变换产生非有限值或使用不支持的传递函数。"};
        if (base.alpha_mode == AlphaMode::premultiplied) {
          // Linearizing premultiplied encoded samples would be incorrect.
          return std::unexpected{"Gain Map: Apple premultiplied base 必须先转换为 straight alpha。"};
        }
        hdr::hdr_detail::write_unorm16(output, x * 4 + c,
            pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, std::max(rgb[c], 0.0F) * boost * 203.0F));
      }
      hdr::hdr_detail::write_unorm16(output, x * 4 + 3, alpha);
    }
  }
  auto image = hdr::hdr_detail::make_rgba_image(base.width, base.height, std::move(*pixels), base.alpha_mode,
      16, SampleRepresentation::unorm, ImageSourceInfo{.pixel_format = PixelFormat::rgba, .bit_depth = 16,
      .color_primaries = 9, .transfer_characteristics = 16, .matrix_coefficients = 9, .color_range = 1,
      .has_hdr_metadata = true, .color_metadata_source = "gain-map-apple-bt2020-pq", .source_has_gain_map = true}, "Apple Gain Map");
  if (!image) return image;
  for (const auto& block : base.metadata) if (block.kind == MetadataKind::exif) image->metadata.push_back(block);
  return image;
}

// A recognized but invalid map is an error, not an ordinary JPEG fallback.
std::expected<std::optional<ImageDecodeResult>, std::string> decode_jpeg_gain_map(
    const fs::path& path, std::stop_token stop = {}, int decode_threads = 1) {
  const auto ext = decoder_common::lower_extension(path);
  if (ext != L".jpg" && ext != L".jpeg" && ext != L".jpe" && ext != L".jfif") return std::optional<ImageDecodeResult>{};
  if (!jpeg_may_contain_gain_map(path)) return std::optional<ImageDecodeResult>{};
  auto bytes = decoder_common::read_file_bytes(path, "Ultra HDR");
  if (!bytes) return std::unexpected{bytes.error()};
  if (!gain_map_detail::jpeg_has_gain_map_marker(*bytes)) return std::optional<ImageDecodeResult>{};
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  const auto previous_threads = awjUhdrThreadLimit(static_cast<unsigned int>(std::max(1, decode_threads)));
  struct ThreadBudget { unsigned int previous; ~ThreadBudget() { awjUhdrThreadLimit(previous); } } thread_budget{previous_threads};
  using Decoder = std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>;
  Decoder decoder{uhdr_create_decoder(), &uhdr_release_decoder};
  if (!decoder) return std::unexpected{"Gain Map: 无法创建 libultrahdr decoder。"};
  uhdr_compressed_image_t input{.data = bytes->data(), .data_sz = bytes->size(),
      .capacity = bytes->size(), .cg = UHDR_CG_UNSPECIFIED,
      .ct = UHDR_CT_UNSPECIFIED, .range = UHDR_CR_UNSPECIFIED};
  auto error = uhdr_dec_set_image(decoder.get(), &input);
  if (error.error_code != UHDR_CODEC_OK) return std::unexpected{gain_map_detail::uhdr_error(error)};
  // Probe ends configuration. The default display boost is clamped to the map's HDR capacity.
  for (auto result : {uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat),
                      uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR)}) {
    if (result.error_code != UHDR_CODEC_OK) return std::unexpected{gain_map_detail::uhdr_error(result)};
  }
  error = uhdr_dec_probe(decoder.get());
  if (error.error_code != UHDR_CODEC_OK) return std::unexpected{gain_map_detail::uhdr_error(error)};
  auto* metadata = uhdr_dec_get_gainmap_metadata(decoder.get());
  if (!metadata || !std::isfinite(metadata->hdr_capacity_max) || metadata->hdr_capacity_max < 1) {
    return std::unexpected{"Gain Map: 缺少有效 HDR capacity。"};
  }
  const auto width = uhdr_dec_get_image_width(decoder.get());
  const auto height = uhdr_dec_get_image_height(decoder.get());
  auto dimensions = decoder_common::make_image_dimensions_checked(width, height, "Ultra HDR");
  if (!dimensions) return std::unexpected{dimensions.error()};
  auto pixels = hdr::hdr_detail::make_rgba_bytes(dimensions->width, dimensions->height, 2, "Ultra HDR");
  if (!pixels) return std::unexpected{pixels.error()};
  error = uhdr_decode(decoder.get());
  if (error.error_code != UHDR_CODEC_OK) return std::unexpected{gain_map_detail::uhdr_error(error)};
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  auto* raw = uhdr_get_decoded_image(decoder.get());
  if (!raw || raw->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || raw->ct != UHDR_CT_LINEAR ||
      raw->w != dimensions->width || raw->h != dimensions->height ||
      !raw->planes[0] || raw->stride[0] < raw->w) {
    return std::unexpected{"Gain Map: libultrahdr 输出布局无效。"};
  }
  const int primaries = raw->cg == UHDR_CG_BT_709 ? 1 : raw->cg == UHDR_CG_DISPLAY_P3 ? 12
      : raw->cg == UHDR_CG_BT_2100 ? 9 : 0;
  if (!primaries) return std::unexpected{"Gain Map: libultrahdr 未提供输出原色。"};
  for (std::size_t y = 0; y < dimensions->height; ++y) {
    std::memcpy(pixels->data() + y * dimensions->width * 8,
        static_cast<const std::byte*>(raw->planes[0]) + y * raw->stride[0] * 8,
        dimensions->width * 8);
  }
  ImageSourceInfo info{.pixel_format = PixelFormat::rgba, .bit_depth = 16,
      .color_primaries = primaries, .transfer_characteristics = 8,
      .matrix_coefficients = 0, .color_range = 1, .has_hdr_metadata = true,
      .color_metadata_source = "gain-map-ultrahdr-linear",
      .linear_reference_white_nits = 203.0F, .source_has_gain_map = true};
  auto image = hdr::hdr_detail::make_rgba_image(dimensions->width, dimensions->height,
      std::move(*pixels), AlphaMode::none, 16, SampleRepresentation::ieee_half_float,
      std::move(info), "Ultra HDR");
  if (!image) return std::unexpected{image.error()};
  // The source ICC describes encoded base pixels, not the reconstructed linear RGB.
  // Gain-map XMP is deliberately absent from the output metadata.
  if (auto* exif = uhdr_dec_get_exif(decoder.get()); exif && exif->data && exif->data_sz) {
    const auto* begin = static_cast<const std::byte*>(exif->data);
    if (exif->data_sz >= 6 && std::memcmp(begin, "Exif\0\0", 6) == 0) begin += 6;
    image->metadata.push_back({MetadataKind::exif,
        {begin, static_cast<const std::byte*>(exif->data) + exif->data_sz}});
  }
  if (auto oriented = gain_map_detail::apply_exif_orientation(*image); !oriented) return std::unexpected{oriented.error()};
  return std::optional<ImageDecodeResult>{ImageDecodeResult{
      .image = std::move(*image), .decoder_id = "libultrahdr-gain-map-enhanced"}};
}

} // namespace awj
