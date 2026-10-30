module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include <avif/internal.h>
#include "uhdr_thread_adapter.h"

export module awj.gain_map;

import awj.decoder_common;
import awj.encoding_defaults;
import awj.hdr_tonemap;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace gain_map_detail {

std::expected<void, std::string> transform_rgba(ImageBuffer& image, unsigned ccw_quarters, int mirror = -1) {
  if (auto valid = hdr::hdr_detail::validate_rgba(image, "ISO 21496-1 orientation"); !valid) return valid;
  ccw_quarters %= 4;
  if (!ccw_quarters && mirror < 0) return {};
  const auto width = ccw_quarters & 1 ? image.height : image.width;
  const auto height = ccw_quarters & 1 ? image.width : image.height;
  const auto sample_bytes = image.bit_depth == 16 ? 2u : 1u;
  auto bytes = hdr::hdr_detail::make_rgba_bytes(width, height, sample_bytes, "ISO 21496-1 orientation");
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
        image.planes.front().read_bytes().data() + y * image.planes.front().stride + x * pixel_bytes, pixel_bytes);
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
      default: return std::unexpected{"Gain Map: ISO 21496-1: Exif orientation 无效。"};
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
      width > image.width - x || height > image.height - y) return std::unexpected{"Gain Map: ISO 21496-1: crop 越界。"};
  const auto sample_bytes = image.bit_depth == 16 ? 2u : 1u;
  auto bytes = hdr::hdr_detail::make_rgba_bytes(width, height, sample_bytes, "ISO 21496-1 crop");
  if (!bytes) return std::unexpected{bytes.error()};
  const auto stride = width * 4 * sample_bytes;
  for (std::size_t row = 0; row < height; ++row)
    std::memcpy(bytes->data() + row * stride,
        image.planes.front().read_bytes().data() + (y + row) * image.planes.front().stride + x * 4 * sample_bytes, stride);
  image.width = width; image.height = height; image.planes.front() = {std::move(*bytes), stride};
  return {};
}

std::string uhdr_error(const uhdr_error_info_t& error) {
  return std::string{"Gain Map: ISO 21496-1: "} +
      (error.has_detail ? error.detail : "unsupported or invalid image");
}

// This policy consumes libavif's authoritative parse result/diagnostic only;
// it does not inspect container bytes. Unrelated diagnostics are not HDR.
bool avif_iso_gain_map_failure(avifResult result, const avifDecoder* decoder) noexcept {
  return result == AVIF_RESULT_INVALID_TONE_MAPPED_IMAGE || result == AVIF_RESULT_DECODE_GAIN_MAP_FAILED ||
      (decoder && ((result != AVIF_RESULT_OK && decoder->image && decoder->image->gainMap) ||
       std::string_view{decoder->diag.error}.starts_with("Box[tmap]")));
}

std::expected<void, std::string> validate_avif_iso_gain_map_parse(avifResult result, const avifDecoder& decoder) {
  // Pinned libavif ignores unsupported tmap versions, returning OK while
  // leaving this diagnostic. Inspect it before NextImage() clears diag.
  if (avif_iso_gain_map_failure(result, &decoder))
    return std::unexpected{std::string{"Gain Map: ISO 21496-1 AVIF: "} +
        (decoder.diag.error[0] ? decoder.diag.error : avifResultToString(result))};
  if (result == AVIF_RESULT_OK && decoder.image && decoder.image->gainMap) {
    avifDiagnostics diagnostics{};
    if (avifGainMapValidateMetadata(decoder.image->gainMap, &diagnostics) != AVIF_RESULT_OK)
      return std::unexpected{std::string{"Gain Map: ISO 21496-1 AVIF metadata: "} + diagnostics.error};
  }
  return {};
}

inline constexpr std::string_view legacy_gain_map_error =
    "Gain Map: ISO 21496-1 only; legacy Adobe/Google/Apple gain maps are unsupported.";

bool xml_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Rejection signatures only, not an XMP metadata parser. An unused namespace
// declaration (or an xmlns prefix named after a property) is not HDR content.
std::size_t xmp_property_value(std::string_view text, std::string_view name, std::size_t start = 0) {
  for (auto pos = text.find(name, start); pos != text.npos; pos = text.find(name, pos + name.size())) {
    if (!pos || (text[pos - 1] != ':' && text[pos - 1] != '<')) continue;
    if (text[pos - 1] == ':') {
      auto prefix = pos - 1;
      while (prefix && text[prefix - 1] != '<' && !xml_space(text[prefix - 1])) --prefix;
      if (text.substr(prefix, pos - prefix - 1) == "xmlns") continue;
    }
    auto value = pos + name.size();
    while (value < text.size() && xml_space(text[value])) ++value;
    if (value < text.size() && (text[value] == '=' || text[value] == '>')) return value;
  }
  return text.npos;
}

bool xmp_has_legacy_gain_map(std::string_view text) {
  if (text.find("http://ns.adobe.com/hdr-gain-map/1.0/") != text.npos) {
    for (const auto name : {"GainMapMin", "GainMapMax", "OffsetSDR", "OffsetHDR",
                            "HDRCapacityMin", "HDRCapacityMax", "BaseRenditionIsHDR"})
      if (xmp_property_value(text, name) != text.npos) return true;
  }
  if (text.find("http://ns.apple.com/HDRGainMap/1.0/") != text.npos &&
      (xmp_property_value(text, "HDRGainMapVersion") != text.npos ||
       xmp_property_value(text, "HDRGainMapHeadroom") != text.npos)) return true;
  if (text.find("http://ns.google.com/photos/1.0/container/") != text.npos) {
    for (auto pos = xmp_property_value(text, "Semantic"); pos != text.npos;
         pos = xmp_property_value(text, "Semantic", pos + 1)) {
      const char delimiter = text[pos++];
      while (pos < text.size() && xml_space(text[pos])) ++pos;
      if (delimiter == '=') {
        if (pos == text.size() || (text[pos] != '\'' && text[pos] != '"')) continue;
        const char quote = text[pos++];
        if (text.substr(pos, 7) == "GainMap" && pos + 7 < text.size() && text[pos + 7] == quote) return true;
      } else if (text.substr(pos, 7) == "GainMap") {
        pos += 7;
        while (pos < text.size() && xml_space(text[pos])) ++pos;
        if (pos < text.size() && text[pos] == '<') return true;
      }
    }
  }
  return false;
}

inline constexpr std::string_view jpeg_xmp_namespace{"http://ns.adobe.com/xap/1.0/\0",29};
inline constexpr std::string_view jpeg_extended_xmp_namespace{"http://ns.adobe.com/xmp/extension/\0",35};
struct JpegXmpSegment { std::span<const std::byte> bytes; std::size_t image_index{}; };

std::optional<std::string> extended_xmp_guid(std::string_view value) {
  if (value.size() != 32) return {};
  std::string guid;
  for (char c : value) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return {};
    guid.push_back(c);
  }
  return guid;
}

// Only Extended XMP transport is reconstructed here, never HDR numbers/XML.
// Packets must be linked by HasExtendedXMP in the same JPEG. Check all ranges
// and complete non-overlapping coverage before allocating a bounded buffer.
std::expected<std::vector<std::vector<std::byte>>, std::string> reassemble_extended_xmp(
    std::span<const JpegXmpSegment> segments) {
  constexpr auto limit = encoding_defaults::codec_metadata_max_bytes;
  constexpr auto marker_limit = encoding_defaults::jpeg_max_saved_metadata_marker_count;
  if (segments.size() > marker_limit) return std::unexpected{"JPEG Extended XMP marker 数量超过上限。"};
  std::size_t raw_bytes = 0;
  for (const auto& segment : segments) {
    if (segment.bytes.size() > limit - raw_bytes) return std::unexpected{"JPEG Extended XMP payload 超过 metadata 上限。"};
    raw_bytes += segment.bytes.size();
  }
  struct Link { std::size_t image; std::string guid; };
  std::vector<Link> links;
  for (const auto& segment : segments) {
    const std::string_view packet{reinterpret_cast<const char*>(segment.bytes.data()),segment.bytes.size()};
    if (!packet.starts_with(jpeg_xmp_namespace) || packet.find("http://ns.adobe.com/xmp/note/") == packet.npos) continue;
    for (auto at = xmp_property_value(packet,"HasExtendedXMP"); at != packet.npos;
         at = xmp_property_value(packet,"HasExtendedXMP",at+1)) {
      auto value = at + 1;
      while (value < packet.size() && xml_space(packet[value])) ++value;
      char end = '<';
      if (packet[at] == '=') {
        if (value == packet.size() || (packet[value] != '\'' && packet[value] != '"')) continue;
        end = packet[value++];
      }
      if (packet.size() - value < 33) continue;
      auto guid = extended_xmp_guid(packet.substr(value,32));
      auto tail = value + 32;
      if (end == '<') while (tail < packet.size() && xml_space(packet[tail])) ++tail;
      if (!guid || tail == packet.size() || packet[tail] != end) continue;
      if (std::ranges::any_of(links,[&](const Link& link) { return link.image == segment.image_index && link.guid == *guid; })) continue;
      if (links.size() == marker_limit) return std::unexpected{"JPEG Extended XMP GUID 数量超过上限。"};
      links.push_back({segment.image_index,std::move(*guid)});
    }
  }
  struct Chunk { std::size_t offset; std::span<const std::byte> bytes; };
  struct Group { std::size_t link, total; std::vector<Chunk> chunks; };
  std::vector<Group> groups;
  std::size_t declared_bytes = 0;
  auto u32 = [](std::span<const std::byte> data, std::size_t pos) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8) | std::to_integer<unsigned>(data[pos+i]);
    return static_cast<std::size_t>(value);
  };
  for (const auto& segment : segments) {
    const std::string_view packet{reinterpret_cast<const char*>(segment.bytes.data()),segment.bytes.size()};
    if (!packet.starts_with(jpeg_extended_xmp_namespace) || packet.size() < 67) continue;
    auto guid = extended_xmp_guid(packet.substr(35,32));
    if (!guid) continue;
    const auto linked = std::ranges::find_if(links,[&](const Link& link) { return link.image == segment.image_index && link.guid == *guid; });
    if (linked == links.end()) continue; // Unassociated ordinary APP data is not HDR.
    if (packet.size() <= 75) return std::unexpected{"JPEG Extended XMP chunk 头部或数据截断。"};
    const auto total = u32(segment.bytes,67), offset = u32(segment.bytes,71);
    const auto data = segment.bytes.subspan(75);
    if (!total || total > limit || offset > total || data.size() > total - offset)
      return std::unexpected{"JPEG Extended XMP total/offset 越界。"};
    const auto link_index = static_cast<std::size_t>(linked - links.begin());
    auto group = std::ranges::find_if(groups,[&](const Group& value) { return value.link == link_index; });
    if (group == groups.end()) {
      if (total > limit - declared_bytes) return std::unexpected{"JPEG Extended XMP 重组超过 metadata 上限。"};
      declared_bytes += total;
      groups.push_back({link_index,total,{}}); group = groups.end()-1;
    } else if (group->total != total) return std::unexpected{"JPEG Extended XMP total 不一致。"};
    group->chunks.push_back({offset,data});
  }
  std::vector<std::vector<std::byte>> packets;
  for (auto& group : groups) {
    std::ranges::sort(group.chunks,{},&Chunk::offset);
    std::size_t end = 0;
    for (const auto& chunk : group.chunks) {
      if (chunk.offset != end) return std::unexpected{"JPEG Extended XMP chunk 缺失或重叠。"};
      end += chunk.bytes.size();
    }
    if (end != group.total) return std::unexpected{"JPEG Extended XMP packet 不完整。"};
    auto packet = decoder_common::make_byte_buffer(group.total,"JPEG Extended XMP");
    if (!packet) return std::unexpected{packet.error()};
    for (const auto& chunk : group.chunks) std::memcpy(packet->data()+chunk.offset,chunk.bytes.data(),chunk.bytes.size());
    packets.push_back(std::move(*packet));
  }
  return packets;
}

struct JpegGainMapMarkers {
  bool iso{}, legacy{}, mpf{}, extended{}, xmp_over_budget{};
  std::size_t xmp_bytes{};
  std::vector<JpegXmpSegment> xmp_segments;
  bool candidate() const noexcept { return iso || legacy || mpf || extended; }
};

void inspect_jpeg_app(unsigned marker, std::span<const std::byte> bytes, JpegGainMapMarkers& found,
    std::size_t image_index = 0, bool collect_xmp = false) {
  const std::string_view payload{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  // The patched library owns ISO parsing. XMP reconstruction is delayed until
  // all JPEG candidates are scanned, so valid ISO always wins over bad XMP.
  if (marker == 0xe2) {
    found.iso |= payload.starts_with("urn:iso:std:iso:ts:21496:-1");
    found.mpf |= payload.starts_with(std::string_view{"MPF\0", 4});
  } else if (marker == 0xe1) {
    const bool standard = payload.starts_with(jpeg_xmp_namespace);
    const bool extended = payload.starts_with(jpeg_extended_xmp_namespace);
    if (standard) found.legacy |= xmp_has_legacy_gain_map(payload);
    found.extended |= extended;
    if (collect_xmp && (standard || extended)) {
      if (found.xmp_segments.size() == encoding_defaults::jpeg_max_saved_metadata_marker_count ||
          bytes.size() > encoding_defaults::codec_metadata_max_bytes - found.xmp_bytes) found.xmp_over_budget = true;
      else { found.xmp_segments.push_back({bytes,image_index}); found.xmp_bytes += bytes.size(); }
    }
  }
}

// Bounded APP segments, not an ISO JPEG parser. Entropy is skipped with JPEG
// byte-stuffing/restart rules; a secondary JPEG is inspected only for candidates.
JpegGainMapMarkers jpeg_gain_map_markers(std::span<const std::byte> bytes) {
  JpegGainMapMarkers found;
  if (bytes.size() < 2 || bytes[0] != std::byte{0xff} || bytes[1] != std::byte{0xd8}) return found;
  bool entropy = false, between_images = false;
  std::size_t image_index = 0;
  for (std::size_t pos = 2; pos + 1 < bytes.size();) {
    if (between_images) {
      while (pos + 1 < bytes.size() && (bytes[pos] != std::byte{0xff} || bytes[pos + 1] != std::byte{0xd8})) ++pos;
      if (pos + 1 == bytes.size()) break;
      pos += 2; between_images = false; ++image_index;
      continue;
    }
    if (entropy) {
      while (pos + 1 < bytes.size()) {
        if (bytes[pos] != std::byte{0xff}) { ++pos; continue; }
        const auto next = std::to_integer<unsigned>(bytes[pos + 1]);
        if (next == 0 || (next >= 0xd0 && next <= 0xd7)) { pos += 2; continue; }
        if (next == 0xff) { ++pos; continue; }
        entropy = false; break;
      }
      if (entropy || pos + 1 >= bytes.size()) break;
    }
    if (bytes[pos++] != std::byte{0xff}) break;
    while (pos < bytes.size() && bytes[pos] == std::byte{0xff}) ++pos;
    if (pos == bytes.size()) break;
    const auto marker = std::to_integer<unsigned>(bytes[pos++]);
    if (marker == 0xd9) { between_images = true; continue; }
    if (marker == 0xd8) { ++image_index; continue; }
    if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (pos + 2 > bytes.size()) break;
    const auto length = (std::to_integer<unsigned>(bytes[pos]) << 8) | std::to_integer<unsigned>(bytes[pos + 1]);
    if (length < 2 || length > bytes.size() - pos) break;
    if (marker >= 0xe0 && marker <= 0xef) inspect_jpeg_app(marker, bytes.subspan(pos + 2, length - 2), found,image_index,true);
    pos += length;
    if (marker == 0xda) {
      if (!found.candidate()) return found; // Ordinary JPEG never scans entropy.
      entropy = true;
    }
  }
  if (!found.iso && found.extended && !found.legacy && !found.xmp_over_budget) {
    auto packets = reassemble_extended_xmp(found.xmp_segments);
    // Transport errors or orphaned chunks are not proof of legacy HDR. This
    // bounded helper only detects signatures; ordinary XMP remains opaque.
    if (packets) for (const auto& packet : *packets)
      found.legacy |= xmp_has_legacy_gain_map({reinterpret_cast<const char*>(packet.data()),packet.size()});
  }
  return found;
}

} // namespace gain_map_detail

// Ordinary JPEGs only need their short pre-SOS header inspected. MPF is a
// candidate, not proof: stereo/burst JPEGs may contain no gain map.
bool jpeg_may_contain_gain_map(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  auto read_byte = [&]() { return input.get(); };
  if (read_byte() != 0xff || read_byte() != 0xd8) return false;
  std::vector<std::byte> segment;
  gain_map_detail::JpegGainMapMarkers found;
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
      segment.resize(static_cast<std::size_t>(length) - 2);
      input.read(reinterpret_cast<char*>(segment.data()), length - 2);
      if (!input) return false;
      gain_map_detail::inspect_jpeg_app(static_cast<unsigned>(marker), segment, found);
      if (found.candidate()) return true;
    } else {
      input.seekg(length - 2, std::ios::cur);
      if (!input) return false;
    }
  }
}

std::expected<ImageBuffer, std::string> compose_iso_gain_map(
    const ImageBuffer& base, const ImageBuffer& map, avifGainMap& metadata) {
  if (auto valid = hdr::hdr_detail::validate_rgba(base, "ISO 21496-1 base"); !valid) return std::unexpected{valid.error()};
  if (auto valid = hdr::hdr_detail::validate_rgba(map, "ISO 21496-1 auxiliary"); !valid) return std::unexpected{valid.error()};
  if (!base.source_info || !base.source_info->color_primaries || !base.source_info->transfer_characteristics ||
      base.sample_representation != SampleRepresentation::unorm || map.sample_representation != SampleRepresentation::unorm)
    return std::unexpected{"Gain Map: ISO 21496-1: 输入缺少明确编码色彩空间。"};
  if (base.alpha_mode == AlphaMode::premultiplied)
    return std::unexpected{"Gain Map: ISO 21496-1: premultiplied base 必须先转换为 straight alpha。"};
  if (std::ranges::any_of(base.metadata, [](const MetadataBlock& b) { return b.kind == MetadataKind::icc; }))
    return std::unexpected{"Gain Map: ISO 21496-1: ICC 合成空间暂不支持，不能按 CICP 猜测。"};
  if (!hdr::hdr_detail::primaries_from_cicp(*base.source_info->color_primaries) ||
      *base.source_info->transfer_characteristics == 0 || *base.source_info->transfer_characteristics == 2)
    return std::unexpected{"Gain Map: ISO 21496-1: base 色彩标签未指定或不支持。"};
  if (!metadata.useBaseColorSpace && !hdr::hdr_detail::primaries_from_cicp(static_cast<int>(metadata.altColorPrimaries)))
    return std::unexpected{"Gain Map: ISO 21496-1: alternate 合成原色未指定或不支持。"};
  using Image = std::unique_ptr<avifImage, decltype(&avifImageDestroy)>;
  const int map_depth = map.bit_depth == 8 ? 8 : std::clamp(map.source_info ? map.source_info->bit_depth : 12, 10, 12);
  Image gain{avifImageCreate(static_cast<std::uint32_t>(map.width), static_cast<std::uint32_t>(map.height),
      map_depth, AVIF_PIXEL_FORMAT_YUV444), &avifImageDestroy};
  if (!gain) return std::unexpected{"Gain Map: ISO 21496-1: 无法创建 map 像素。"};
  gain->colorPrimaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
  gain->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
  gain->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
  gain->yuvRange = AVIF_RANGE_FULL;
  avifRGBImage map_rgb{};
  avifRGBImageSetDefaults(&map_rgb, gain.get());
  map_rgb.format = AVIF_RGB_FORMAT_RGBA; map_rgb.depth = map.bit_depth; map_rgb.ignoreAlpha = AVIF_TRUE;
  map_rgb.pixels = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(map.planes.front().read_bytes().data()));
  if (map.planes.front().stride > std::numeric_limits<std::uint32_t>::max()) return std::unexpected{"Gain Map: ISO 21496-1: map stride 过大。"};
  map_rgb.rowBytes = static_cast<std::uint32_t>(map.planes.front().stride);
  const auto converted = avifImageRGBToYUV(gain.get(), &map_rgb);
  if (converted != AVIF_RESULT_OK) return std::unexpected{"Gain Map: ISO 21496-1: map 像素转换失败。"};
  metadata.image = gain.get();
  struct Detach { avifGainMap& map; ~Detach() { map.image = nullptr; } } detach{metadata};
  if (!metadata.baseHdrHeadroom.d || !metadata.alternateHdrHeadroom.d) return std::unexpected{"Gain Map: ISO 21496-1: headroom 分母为零。"};
  const float headroom = std::max(static_cast<float>(metadata.baseHdrHeadroom.n) / metadata.baseHdrHeadroom.d,
      static_cast<float>(metadata.alternateHdrHeadroom.n) / metadata.alternateHdrHeadroom.d);
  avifRGBImage base_rgb{};
  base_rgb.width = static_cast<std::uint32_t>(base.width); base_rgb.height = static_cast<std::uint32_t>(base.height);
  base_rgb.format = AVIF_RGB_FORMAT_RGBA; base_rgb.depth = base.bit_depth;
  if (base.planes.front().stride > std::numeric_limits<std::uint32_t>::max()) return std::unexpected{"Gain Map: ISO 21496-1: base stride 过大。"};
  base_rgb.rowBytes = static_cast<std::uint32_t>(base.planes.front().stride);
  base_rgb.pixels = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(base.planes.front().read_bytes().data()));
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
  if (applied != AVIF_RESULT_OK) return std::unexpected{std::string{"Gain Map: ISO 21496-1: 合成失败: "} + diag.error};
  auto pixels = hdr::hdr_detail::make_rgba_bytes(base.width, base.height, 2, "ISO 21496-1");
  if (!pixels) return std::unexpected{pixels.error()};
  for (std::size_t y = 0; y < base.height; ++y)
    std::memcpy(pixels->data() + y * base.width * 8, enhanced.pixels + y * enhanced.rowBytes, base.width * 8);
  auto image = hdr::hdr_detail::make_rgba_image(base.width, base.height, std::move(*pixels), base.alpha_mode,
      16, SampleRepresentation::unorm, ImageSourceInfo{.pixel_format = PixelFormat::rgba, .bit_depth = 16,
      .color_primaries = 9, .transfer_characteristics = 16, .matrix_coefficients = 9, .color_range = 1,
      .content_light = HdrContentLightMetadata{clli.maxCLL, clli.maxPALL}, .has_hdr_metadata = true,
      .color_metadata_source = "gain-map-heif-iso21496-1-bt2020-pq", .source_has_gain_map = true}, "ISO 21496-1");
  if (!image) return image;
  for (const auto& block : base.metadata) if (block.kind == MetadataKind::exif) image->metadata.push_back(block);
  return image;
}

namespace gain_map_detail {

using Decoder = std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>;
struct ProbedJpeg {
  Decoder decoder{nullptr, &uhdr_release_decoder};
  ImageDimensions dimensions;
};

// The same gate is used for file, memory and direct-codec entry points. MPF
// alone is ordinary MPO; legacy HDR is a terminal error, never not-my-format.
std::expected<std::optional<ProbedJpeg>, std::string> probe_jpeg(
    std::span<const std::byte> bytes, std::stop_token stop) {
  const auto markers = jpeg_gain_map_markers(bytes);
  if (!markers.iso) {
    if (markers.legacy) return std::unexpected{std::string{legacy_gain_map_error}};
    if (markers.extended && markers.xmp_over_budget)
      return std::unexpected{"Gain Map: ISO 21496-1: Extended XMP 超过安全检查预算，不能安全排除旧 HDR。"};
    return std::optional<ProbedJpeg>{};
  }
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  Decoder decoder{uhdr_create_decoder(), &uhdr_release_decoder};
  if (!decoder) return std::unexpected{"Gain Map: ISO 21496-1: 无法创建 decoder。"};
  uhdr_compressed_image_t input{.data = const_cast<std::byte*>(bytes.data()), .data_sz = bytes.size(),
      .capacity = bytes.size(), .cg = UHDR_CG_UNSPECIFIED,
      .ct = UHDR_CT_UNSPECIFIED, .range = UHDR_CR_UNSPECIFIED};
  auto error = uhdr_dec_set_image(decoder.get(), &input);
  if (error.error_code != UHDR_CODEC_OK) return std::unexpected{uhdr_error(error)};
  // Probe ends configuration. The default display boost is clamped to the map's HDR capacity.
  for (auto result : {uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat),
                      uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR)}) {
    if (result.error_code != UHDR_CODEC_OK) return std::unexpected{uhdr_error(result)};
  }
  error = uhdr_dec_probe(decoder.get());
  if (error.error_code != UHDR_CODEC_OK) return std::unexpected{uhdr_error(error)};
  auto* metadata = uhdr_dec_get_gainmap_metadata(decoder.get());
  if (!metadata || !std::isfinite(metadata->hdr_capacity_max) || metadata->hdr_capacity_max < 1)
    return std::unexpected{"Gain Map: ISO 21496-1: 缺少有效 HDR capacity。"};
  auto dimensions = decoder_common::make_image_dimensions_checked(
      uhdr_dec_get_image_width(decoder.get()), uhdr_dec_get_image_height(decoder.get()), "ISO 21496-1");
  if (!dimensions) return std::unexpected{dimensions.error()};
  return std::optional<ProbedJpeg>{ProbedJpeg{std::move(decoder), *dimensions}};
}

// uhdr_dec_set_image owns a compressed copy; release our read buffer after probe.
std::expected<std::optional<ProbedJpeg>, std::string> probe_jpeg(
    const fs::path& path, std::stop_token stop) {
  // Use JPEG magic, not the filename: renamed legacy/invalid HDR must not
  // escape the terminal gate into another decoder or WIC.
  if (!jpeg_may_contain_gain_map(path)) return std::optional<ProbedJpeg>{};
  auto bytes = decoder_common::read_file_bytes(path, "ISO 21496-1");
  if (!bytes) return std::unexpected{bytes.error()};
  return probe_jpeg(std::span<const std::byte>{*bytes}, stop);
}

std::expected<std::optional<ImageDimensions>, std::string> oriented_dimensions(
    std::expected<std::optional<ProbedJpeg>, std::string> probed) {
  if (!probed) return std::unexpected{probed.error()};
  if (!*probed) return std::optional<ImageDimensions>{};
  auto dimensions = (**probed).dimensions;
  if (auto* exif = uhdr_dec_get_exif((**probed).decoder.get()); exif && exif->data && exif->data_sz) {
    auto* begin = static_cast<const std::uint8_t*>(exif->data);
    auto size = exif->data_sz;
    if (size >= 6 && std::memcmp(begin, "Exif\0\0", 6) == 0) { begin += 6; size -= 6; }
    std::size_t offset{};
    if (avifGetExifOrientationOffset(begin, size, &offset) == AVIF_RESULT_OK && offset < size) {
      const auto orientation = begin[offset];
      if (orientation < 1 || orientation > 8) return std::unexpected{"Gain Map: ISO 21496-1: Exif orientation 无效。"};
      if (orientation >= 5) std::swap(dimensions.width, dimensions.height);
    }
  }
  return std::optional<ImageDimensions>{dimensions};
}

std::expected<std::optional<ImageDecodeResult>, std::string> decode_probed_jpeg(
    std::expected<std::optional<ProbedJpeg>, std::string> probed, std::stop_token stop, int decode_threads) {
  if (!probed) return std::unexpected{probed.error()};
  if (!*probed) return std::optional<ImageDecodeResult>{};
  const auto previous_threads = awjUhdrThreadLimit(static_cast<unsigned int>(std::max(1, decode_threads)));
  struct ThreadBudget { unsigned int previous; ~ThreadBudget() { awjUhdrThreadLimit(previous); } } thread_budget{previous_threads};
  auto& decoder = (**probed).decoder;
  auto* dimensions = &(**probed).dimensions;
  auto pixels = hdr::hdr_detail::make_rgba_bytes(dimensions->width, dimensions->height, 2, "ISO 21496-1");
  if (!pixels) return std::unexpected{pixels.error()};
  auto error = uhdr_decode(decoder.get());
  if (error.error_code != UHDR_CODEC_OK) return std::unexpected{uhdr_error(error)};
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  auto* raw = uhdr_get_decoded_image(decoder.get());
  if (!raw || raw->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || raw->ct != UHDR_CT_LINEAR ||
      raw->w != dimensions->width || raw->h != dimensions->height ||
      !raw->planes[0] || raw->stride[0] < raw->w)
    return std::unexpected{"Gain Map: ISO 21496-1: decoder 输出布局无效。"};
  const int primaries = raw->cg == UHDR_CG_BT_709 ? 1 : raw->cg == UHDR_CG_DISPLAY_P3 ? 12
      : raw->cg == UHDR_CG_BT_2100 ? 9 : 0;
  if (!primaries) return std::unexpected{"Gain Map: ISO 21496-1: decoder 未提供输出原色。"};
  for (std::size_t y = 0; y < dimensions->height; ++y) {
    std::memcpy(pixels->data() + y * dimensions->width * 8,
        static_cast<const std::byte*>(raw->planes[0]) + y * raw->stride[0] * 8,
        dimensions->width * 8);
  }
  ImageSourceInfo info{.pixel_format = PixelFormat::rgba, .bit_depth = 16,
      .color_primaries = primaries, .transfer_characteristics = 8,
      .matrix_coefficients = 0, .color_range = 1, .has_hdr_metadata = true,
      .color_metadata_source = "gain-map-iso21496-1-linear",
      .linear_reference_white_nits = 203.0F, .source_has_gain_map = true};
  auto image = hdr::hdr_detail::make_rgba_image(dimensions->width, dimensions->height,
      std::move(*pixels), AlphaMode::none, 16, SampleRepresentation::ieee_half_float,
      std::move(info), "ISO 21496-1");
  if (!image) return std::unexpected{image.error()};
  // The source ICC describes encoded base pixels, not the reconstructed linear RGB.
  // Gain-map XMP is deliberately absent from the output metadata.
  if (auto* exif = uhdr_dec_get_exif(decoder.get()); exif && exif->data && exif->data_sz) {
    const auto* begin = static_cast<const std::byte*>(exif->data);
    if (exif->data_sz >= 6 && std::memcmp(begin, "Exif\0\0", 6) == 0) begin += 6;
    image->metadata.push_back({MetadataKind::exif,
        {begin, static_cast<const std::byte*>(exif->data) + exif->data_sz}});
  }
  if (auto oriented = apply_exif_orientation(*image); !oriented) return std::unexpected{oriented.error()};
  return std::optional<ImageDecodeResult>{ImageDecodeResult{
      .image = std::move(*image), .decoder_id = "iso21496-1-jpeg-gain-map-enhanced"}};
}

} // namespace gain_map_detail

std::expected<std::optional<ImageDimensions>, std::string> probe_jpeg_gain_map_dimensions(
    const fs::path& path, std::stop_token stop = {}) {
  return gain_map_detail::oriented_dimensions(gain_map_detail::probe_jpeg(path, stop));
}

std::expected<std::optional<ImageDimensions>, std::string> probe_jpeg_gain_map_dimensions(
    std::span<const std::byte> bytes, std::stop_token stop = {}) {
  return gain_map_detail::oriented_dimensions(gain_map_detail::probe_jpeg(bytes, stop));
}

// A recognized legacy or invalid ISO map is an error, never an SDR fallback.
std::expected<std::optional<ImageDecodeResult>, std::string> decode_jpeg_gain_map(
    const fs::path& path, std::stop_token stop = {}, int decode_threads = 1) {
  return gain_map_detail::decode_probed_jpeg(gain_map_detail::probe_jpeg(path, stop), stop, decode_threads);
}

std::expected<std::optional<ImageDecodeResult>, std::string> decode_jpeg_gain_map(
    std::span<const std::byte> bytes, std::stop_token stop = {}, int decode_threads = 1) {
  return gain_map_detail::decode_probed_jpeg(gain_map_detail::probe_jpeg(bytes, stop), stop, decode_threads);
}

} // namespace awj
