module;

#include <algorithm>
#include <array>
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
#include <gif_lib.h>
#include <webp/demux.h>
#include <webp/decode.h>

export module awj.animation_decoder;

import awj.animation;
import awj.codec;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.gif_codec;
import awj.image;
import awj.png_codec;
import awj.webp_codec;
import awj.avif_aom_codec;

export namespace awj {
namespace animation_detail {

constexpr std::size_t max_frames = 100000;

struct FrameSpec {
  std::size_t x{}, y{}, width{}, height{};
  unsigned dispose{}, blend{};
  int transparent{-1};
  std::vector<std::span<const std::byte>> data{};
};

bool valid_rect(const FrameSpec& spec, const AnimationInfo& info) noexcept {
  return spec.width && spec.height && spec.x <= info.width && spec.y <= info.height &&
      spec.width <= info.width - spec.x && spec.height <= info.height - spec.y;
}

std::uint64_t index_bytes(const std::vector<FrameSpec>& specs, const AnimationInfo& info,
    std::uint64_t span_bytes = 0) noexcept {
  return specs.capacity() * sizeof(FrameSpec) + info.durations.capacity() * sizeof(FrameDuration) + span_bytes;
}

// Shared canvas operations. Samples remain in the source PNG/GIF encoding space,
// as specified by their OVER operation; HDR/color conversion happens afterwards.
class Canvas {
 public:
  ImageBuffer image{};
  std::vector<std::byte> previous{};
  std::array<std::uint16_t, 4> background{};
  FrameSpec last{};
  bool has_last{};

  std::expected<void, std::string> initialize(const AnimationInfo& info, int depth) {
    auto bytes = decoder_common::checked_image_bytes(info.width * 4 * (depth == 16 ? 2 : 1), info.height, "动画画布");
    if (!bytes) return std::unexpected{bytes.error()};
    auto pixels = decoder_common::make_byte_buffer(*bytes, "动画画布");
    if (!pixels) return std::unexpected{pixels.error()};
    image = ImageBuffer{.width = info.width, .height = info.height,
        .pixel_format = PixelFormat::rgba, .alpha_mode = AlphaMode::straight, .bit_depth = depth};
    image.planes.push_back({std::move(*pixels), info.width * 4 * (depth == 16 ? 2 : 1)});
    fill(FrameSpec{.width = info.width, .height = info.height}, background);
    return {};
  }

  bool fill(const FrameSpec& spec, std::array<std::uint16_t, 4> color, std::stop_token stop = {}) {
    auto& plane = image.planes.front();
    const auto sample_bytes = image.bit_depth == 16 ? 2u : 1u;
    for (std::size_t y = 0; y < spec.height; ++y) {
      if (stop.stop_requested()) return false;
      auto* row = plane.bytes.data() + (spec.y + y) * plane.stride + spec.x * 4 * sample_bytes;
      for (std::size_t x = 0; x < spec.width; ++x) for (std::size_t c = 0; c < 4; ++c) {
        if (sample_bytes == 2) std::memcpy(row + (x * 4 + c) * 2, &color[c], 2);
        else row[x * 4 + c] = std::byte{static_cast<unsigned char>(color[c])};
      }
    }
    return true;
  }

  std::expected<void, std::string> before(const FrameSpec& spec, std::stop_token stop) {
    const auto pixel_bytes = image.bit_depth == 16 ? 8u : 4u;
    auto& plane = image.planes.front();
    if (has_last) {
      if (last.dispose == 1 && !fill(last, background, stop)) return std::unexpected{"任务已取消。"};
      else if (last.dispose == 2) {
        for (std::size_t y = 0; y < last.height; ++y) {
          if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
          std::memcpy(plane.bytes.data() + (last.y + y) * plane.stride + last.x * pixel_bytes,
              previous.data() + y * last.width * pixel_bytes, last.width * pixel_bytes);
        }
      }
    }
    previous.clear();
    if (spec.dispose == 2) {
      auto size = decoder_common::checked_image_bytes(spec.width * pixel_bytes, spec.height, "动画 PREVIOUS");
      if (!size) return std::unexpected{size.error()};
      auto resized = decoder_common::resize_buffer(previous, *size, "动画 PREVIOUS");
      if (!resized) return std::unexpected{resized.error()};
      for (std::size_t y = 0; y < spec.height; ++y) {
        if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
        std::memcpy(previous.data() + y * spec.width * pixel_bytes,
            plane.bytes.data() + (spec.y + y) * plane.stride + spec.x * pixel_bytes, spec.width * pixel_bytes);
      }
    }
    last = FrameSpec{.x = spec.x, .y = spec.y, .width = spec.width, .height = spec.height, .dispose = spec.dispose};
    has_last = true;
    return {};
  }

  std::expected<void, std::string> blend(const ImageBuffer& frame, const FrameSpec& spec, std::stop_token stop) {
    const auto sample_bytes = image.bit_depth == 16 ? 2u : 1u;
    const std::uint64_t maximum = sample_bytes == 2 ? 65535 : 255;
    auto read = [sample_bytes](const std::byte* p) -> std::uint64_t {
      if (sample_bytes == 1) return std::to_integer<unsigned char>(*p);
      std::uint16_t value; std::memcpy(&value, p, 2); return value;
    };
    auto write = [sample_bytes](std::byte* p, std::uint64_t value) {
      if (sample_bytes == 1) *p = std::byte{static_cast<unsigned char>(value)};
      else { auto v = static_cast<std::uint16_t>(value); std::memcpy(p, &v, 2); }
    };
    for (std::size_t y = 0; y < spec.height; ++y) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      for (std::size_t x = 0; x < spec.width; ++x) {
      const auto* src = frame.planes.front().bytes.data() + y * frame.planes.front().stride + x * 4 * sample_bytes;
      auto* dst = image.planes.front().bytes.data() + (spec.y + y) * image.planes.front().stride + (spec.x + x) * 4 * sample_bytes;
      if (spec.blend == 0) { std::memcpy(dst, src, 4 * sample_bytes); continue; }
      const auto as = read(src + 3 * sample_bytes), ad = read(dst + 3 * sample_bytes);
      const auto alpha = as * maximum + ad * (maximum - as);
      for (std::size_t c = 0; c < 3; ++c) {
        const auto numerator = read(src + c * sample_bytes) * as * maximum +
            read(dst + c * sample_bytes) * ad * (maximum - as);
        write(dst + c * sample_bytes, alpha ? (numerator + alpha / 2) / alpha : 0);
      }
      write(dst + 3 * sample_bytes, (alpha + maximum / 2) / maximum);
      }
    }
    return {};
  }
};

class GifReader final : public AnimationReader {
 public:
  std::vector<std::byte> bytes{};
  AnimationInfo details{};
  std::vector<FrameSpec> specs{};
  Canvas canvas{};
  std::array<std::uint16_t, 4> opaque_background{0, 0, 0, 255};
  gif_detail::ReadState state{};
  gif_detail::GifPtr gif{};
  std::size_t index{};
  std::uint64_t budget{};
  const AnimationInfo& info() const noexcept override { return details; }
  const ImageBuffer& frame() const noexcept override { return canvas.image; }
  std::uint64_t memory_usage_bytes() const noexcept override {
    std::uint64_t size = bytes.capacity() + canvas.image.planes.front().bytes.capacity() + canvas.previous.capacity() + index_bytes(specs, details);
    for (const auto& block : canvas.image.metadata) size += block.bytes.capacity();
    return size;
  }

  std::expected<void, std::string> open(std::stop_token stop) {
    if (bytes.size() < 13 || (std::memcmp(bytes.data(), "GIF87a", 6) && std::memcmp(bytes.data(), "GIF89a", 6)))
      return std::unexpected{"GIF 签名无效。"};
    details.width = gif_detail::read_le_u16(bytes, 6);
    details.height = gif_detail::read_le_u16(bytes, 8);
    auto dimensions = decoder_common::make_image_dimensions_checked(details.width, details.height, "GIF 动画");
    if (!dimensions) return std::unexpected{dimensions.error()};
    std::size_t pos = 13;
    const auto packed = std::to_integer<unsigned>(bytes[10]);
    if (packed & 128) {
      const auto size = 3u * (2u << (packed & 7));
      if (size > bytes.size() - pos) return std::unexpected{"GIF 全局色表截断。"};
      const auto bg = std::to_integer<unsigned>(bytes[11]);
      if (bg * 3 + 2 >= size) return std::unexpected{"GIF 背景色索引无效。"};
      for (std::size_t c = 0; c < 3; ++c) opaque_background[c] = std::to_integer<unsigned char>(bytes[pos + bg * 3 + c]);
      pos += size;
    }
    FrameSpec control{};
    FrameDuration duration{0, 100};
    bool terminated = false;
    std::optional<int> loop;
    std::vector<std::byte> icc;
    while (pos < bytes.size()) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      const auto marker = std::to_integer<unsigned>(bytes[pos++]);
      if (marker == 0x3b) { terminated = true; break; }
      if (marker == 0x21) {
        if (pos >= bytes.size()) return std::unexpected{"GIF 扩展截断。"};
        const auto label = std::to_integer<unsigned>(bytes[pos++]);
        if (label == 1) return std::unexpected{"GIF Plain Text 图形扩展暂不支持，不能保真合成。"};
        if (label == 0xf9) {
          if (bytes.size() - pos < 6 || bytes[pos] != std::byte{4} || bytes[pos + 5] != std::byte{0})
            return std::unexpected{"GIF GCE 无效。"};
          const auto flags = std::to_integer<unsigned>(bytes[pos + 1]);
          if (flags & 2) return std::unexpected{"GIF user-input 帧需要交互，无法转换为固定时间动画。"};
          const auto disposal = (flags >> 2) & 7;
          if (disposal > 3) return std::unexpected{"GIF disposal 未定义。"};
          control.dispose = disposal == 2 ? 1 : disposal == 3 ? 2 : 0;
          control.transparent = flags & 1 ? std::to_integer<unsigned char>(bytes[pos + 4]) : -1;
          duration = {gif_detail::read_le_u16(bytes, pos + 2), 100};
          if (!duration.numerator) { duration.numerator = 1; details.zero_delay_adjusted = true; }
          pos += 6;
        } else {
          bool netscape = false, profile = false;
          if (label == 0xff) {
            if (pos >= bytes.size() || std::to_integer<unsigned>(bytes[pos]) != 11 || bytes.size() - pos < 12)
              return std::unexpected{"GIF application 扩展无效。"};
            const std::string_view app{reinterpret_cast<const char*>(bytes.data() + pos + 1), 11};
            netscape = app == "NETSCAPE2.0" || app == "ANIMEXTS1.0";
            profile = app == "ICCRGBG1012";
            pos += 12;
          }
          bool first = true;
          for (;;) {
            if (pos >= bytes.size()) return std::unexpected{"GIF 扩展数据截断。"};
            const auto size = std::to_integer<unsigned>(bytes[pos++]);
            if (!size) break;
            if (size > bytes.size() - pos) return std::unexpected{"GIF 扩展数据截断。"};
            if (netscape && first) {
              if (size != 3 || bytes[pos] != std::byte{1}) return std::unexpected{"GIF loop 扩展无效。"};
              auto count = gif_detail::read_le_u16(bytes, pos + 1);
              const int repetitions = count ? count : -1;
              if (loop && *loop != repetitions) return std::unexpected{"GIF 存在冲突的 loop 扩展。"};
              loop = repetitions;
            }
            if (profile) {
              if (icc.size() + size > encoding_defaults::codec_metadata_max_bytes) return std::unexpected{"GIF ICC 太大。"};
              icc.insert(icc.end(), bytes.begin() + pos, bytes.begin() + pos + size);
            }
            pos += size;
            first = false;
          }
        }
      } else if (marker == 0x2c) {
        if (bytes.size() - pos < 9) return std::unexpected{"GIF 帧描述截断。"};
        auto spec = control;
        spec.x = gif_detail::read_le_u16(bytes, pos);
        spec.y = gif_detail::read_le_u16(bytes, pos + 2);
        spec.width = gif_detail::read_le_u16(bytes, pos + 4);
        spec.height = gif_detail::read_le_u16(bytes, pos + 6);
        if (!valid_rect(spec, details)) return std::unexpected{"GIF frame rect 超出画布。"};
        const auto flags = std::to_integer<unsigned>(bytes[pos + 8]);
        pos += 9;
        if (flags & 128) {
          const auto size = 3u * (2u << (flags & 7));
          if (size > bytes.size() - pos) return std::unexpected{"GIF 局部色表截断。"};
          pos += size;
        }
        if (pos >= bytes.size()) return std::unexpected{"GIF LZW 数据截断。"};
        ++pos;
        auto skipped = gif_detail::skip_sub_blocks(bytes, pos, "LZW");
        if (!skipped) return std::unexpected{skipped.error()};
        if (specs.size() == max_frames) return std::unexpected{"GIF 帧数超过 100000。"};
        specs.push_back(spec);
        if (!duration.numerator) { duration.numerator = 1; details.zero_delay_adjusted = true; }
        details.durations.push_back(duration);
        if (bytes.capacity() + icc.capacity() + index_bytes(specs, details) > budget)
          return std::unexpected{"GIF 帧索引超出内存预算。"};
        control = {};
        duration = {0, 100};
      } else return std::unexpected{"GIF 记录类型无效。"};
    }
    if (!terminated || specs.empty()) return std::unexpected{"GIF 无帧或缺少 trailer。"};
    details.repetitions = loop.value_or(0);
    details.decoder_id = "giflib-animation";
    canvas.background = specs.front().transparent >= 0 ? std::array<std::uint16_t, 4>{} : opaque_background;
    const auto raw_bytes = static_cast<std::uint64_t>(details.width) * details.height * 4;
    const auto owned = bytes.capacity() + icc.capacity() + index_bytes(specs, details);
    if (owned > budget || raw_bytes > (budget - owned) / 3)
      return std::unexpected{"GIF 画布/局部帧/PREVIOUS 超出内存预算。"};
    auto initialized = canvas.initialize(details, 8);
    if (!initialized) return initialized;
    canvas.image.source_info = ImageSourceInfo{.pixel_format = PixelFormat::rgba, .bit_depth = 8,
        .color_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 0, .color_range = 1,
        .color_metadata_source = icc.empty() ? "gif-srgb" : "gif-icc"};
    if (!icc.empty()) canvas.image.metadata.push_back({MetadataKind::icc, std::move(icc)});
    state = {.data = bytes.data(), .size = bytes.size()};
    int error = 0;
    gif.reset(DGifOpen(&state, gif_detail::read_callback, &error));
    if (!gif) return std::unexpected{"giflib 无法打开动画。"};
    return {};
  }

  std::expected<bool, std::string> next(std::stop_token stop) override {
    if (index == specs.size()) return false;
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    auto decoded = gif_detail::read_first_frame(gif.get());
    if (!decoded) return std::unexpected{decoded.error()};
    const auto& spec = specs[index];
    if (decoded->image.Width != spec.width || decoded->image.Height != spec.height ||
        decoded->image.Left != spec.x || decoded->image.Top != spec.y)
      return std::unexpected{"giflib 与容器帧索引不一致。"};
    if (canvas.has_last) canvas.background = specs[index - 1].transparent >= 0
        ? std::array<std::uint16_t, 4>{} : opaque_background;
    auto prepared = canvas.before(spec, stop);
    if (!prepared) return std::unexpected{prepared.error()};
    const auto* colors = gif_detail::active_color_map(gif.get(), decoded->image);
    if (!colors) return std::unexpected{"GIF 帧缺少色表。"};
    for (std::size_t y = 0; y < spec.height; ++y) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      for (std::size_t x = 0; x < spec.width; ++x) {
        const auto value = decoded->raster[y * spec.width + x];
        if (value == spec.transparent) continue;
        if (value >= colors->ColorCount) return std::unexpected{"GIF 像素索引超出色表。"};
        const auto& color = colors->Colors[value];
        auto* pixel = canvas.image.planes.front().bytes.data() + (spec.y + y) * canvas.image.planes.front().stride + (spec.x + x) * 4;
        pixel[0] = std::byte{color.Red}; pixel[1] = std::byte{color.Green};
        pixel[2] = std::byte{color.Blue}; pixel[3] = std::byte{255};
      }
    }
    // DGifGetImageDesc allocates a SavedImage/color-map copy even without Slurp.
    // Its raster is already composed; release that bookkeeping before the next frame.
    GifFreeSavedImages(gif.get());
    gif->ImageCount = 0;
    ++index;
    return true;
  }
};

class ApngReader final : public AnimationReader {
 public:
  std::vector<std::byte> bytes{}, inherited{};
  std::array<std::byte, 13> ihdr{};
  AnimationInfo details{};
  std::vector<FrameSpec> specs{};
  Canvas canvas{};
  std::size_t index{};
  std::uint64_t budget{};
  std::uint64_t span_bytes{};
  const AnimationInfo& info() const noexcept override { return details; }
  const ImageBuffer& frame() const noexcept override { return canvas.image; }
  std::uint64_t memory_usage_bytes() const noexcept override {
    // Include the largest synthesized compressed frame, even between next() calls.
    std::uint64_t size = 2 * bytes.capacity() + 2 * inherited.capacity() + 64 + (canvas.image.planes.empty() ? 0 : canvas.image.planes.front().bytes.capacity()) + canvas.previous.capacity() + index_bytes(specs, details, span_bytes);
    for (const auto& block : canvas.image.metadata) size += block.bytes.capacity();
    return size;
  }

  std::expected<void, std::string> open(std::stop_token stop) {
    if (bytes.size() < 33 || std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8)) return std::unexpected{"APNG 签名无效。"};
    std::uint32_t count = 0;
    std::uint64_t sequence = 0;
    bool actl = false, idat = false, idat_closed = false, ended = false, first_uses_idat = false;
    std::size_t chunks{};
    std::optional<std::size_t> current;
    auto append_data = [&](std::span<const std::byte> data) {
      auto& parts = specs[*current].data;
      const auto old_capacity = parts.capacity();
      parts.push_back(data);
      span_bytes += (parts.capacity() - old_capacity) * sizeof(std::span<const std::byte>);
    };
    for (std::size_t pos = 8; pos < bytes.size();) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      if (++chunks > 1000000) return std::unexpected{"APNG chunk 数量超过上限。"};
      if (bytes.size() - pos < 12) return std::unexpected{"APNG chunk 截断。"};
      const auto size = png_detail::read_be_u32(bytes, pos);
      if (size > bytes.size() - pos - 12) return std::unexpected{"APNG chunk 越界。"};
      const std::string_view type{reinterpret_cast<const char*>(bytes.data() + pos + 4), 4};
      const auto payload = std::span<const std::byte>{bytes}.subspan(pos + 8, size);
      if (idat && type != "IDAT") idat_closed = true;
      if (png_detail::png_chunk_crc(type, payload) != png_detail::read_be_u32(bytes, pos + 8 + size))
        return std::unexpected{"APNG chunk CRC 错误。"};
      if (type == "IHDR") {
        if (pos != 8 || size != 13) return std::unexpected{"APNG IHDR 无效。"};
        std::copy(payload.begin(), payload.end(), ihdr.begin());
        details.width = png_detail::read_be_u32(payload, 0);
        details.height = png_detail::read_be_u32(payload, 4);
        auto dimensions = decoder_common::make_image_dimensions_checked(details.width, details.height, "APNG");
        if (!dimensions) return std::unexpected{dimensions.error()};
      } else if (type == "acTL") {
        if (actl || idat || size != 8) return std::unexpected{"APNG acTL 位置或长度无效。"};
        actl = true;
        count = png_detail::read_be_u32(payload, 0);
        const auto plays = png_detail::read_be_u32(payload, 4);
        if (!count || count > max_frames || plays > static_cast<unsigned>(std::numeric_limits<int>::max()))
          return std::unexpected{"APNG 帧数/loop 超出上限。"};
        details.repetitions = plays ? static_cast<int>(plays) - 1 : -1;
      } else if (type == "fcTL") {
        if (!actl || size != 26 || png_detail::read_be_u32(payload, 0) != sequence++)
          return std::unexpected{"APNG fcTL 顺序或长度无效。"};
        if (current && specs[*current].data.empty()) return std::unexpected{"APNG 帧缺少数据。"};
        if (specs.size() == count) return std::unexpected{"APNG 帧数超过 acTL 声明。"};
        FrameSpec spec{.x = png_detail::read_be_u32(payload, 12), .y = png_detail::read_be_u32(payload, 16),
            .width = png_detail::read_be_u32(payload, 4), .height = png_detail::read_be_u32(payload, 8),
            .dispose = std::to_integer<unsigned>(payload[24]), .blend = std::to_integer<unsigned>(payload[25])};
        if (!valid_rect(spec, details) || spec.dispose > 2 || spec.blend > 1)
          return std::unexpected{"APNG frame rect/blend/dispose 无效。"};
        if (specs.empty() && !idat) {
          first_uses_idat = true;
          if (spec.x || spec.y || spec.width != details.width || spec.height != details.height)
            return std::unexpected{"APNG IDAT 首帧必须与默认 PNG 画布一致。"};
        }
        // PREVIOUS on the first frame is defined as BACKGROUND.
        if (specs.empty() && spec.dispose == 2) spec.dispose = 1;
        auto u16 = [&](std::size_t offset) { return (std::to_integer<unsigned>(payload[offset]) << 8) | std::to_integer<unsigned>(payload[offset + 1]); };
        FrameDuration duration{u16(20), u16(22)};
        if (!duration.denominator) duration.denominator = 100;
        if (!duration.numerator) { duration = {1, 1000}; details.zero_delay_adjusted = true; }
        details.durations.push_back(duration);
        specs.push_back(std::move(spec));
        current = specs.size() - 1;
      } else if (type == "IDAT") {
        if (idat_closed) return std::unexpected{"APNG IDAT 必须连续。"};
        if (current && (!first_uses_idat || *current != 0)) return std::unexpected{"APNG IDAT 出现在非默认帧。"};
        idat = true;
        if (current) append_data(payload);
      } else if (type == "fdAT") {
        if (!idat || !current || size < 4 || (first_uses_idat && *current == 0) ||
            png_detail::read_be_u32(payload, 0) != sequence++) return std::unexpected{"APNG fdAT 顺序无效。"};
        append_data(payload.subspan(4));
      } else if (type == "IEND") {
        if (size || !idat || pos + 12 != bytes.size()) return std::unexpected{"APNG IEND 无效。"};
        ended = true;
        break;
      } else {
        // Preserve global decoding/color chunks; animation controls are excluded.
        if (type == "PLTE" || type == "tRNS" || type == "iCCP" || type == "sRGB" ||
            type == "gAMA" || type == "cHRM" || type == "cICP" || type == "cLLI" || type == "sBIT" || type == "eXIf")
        {
          if (idat && type != "eXIf") return std::unexpected{"APNG 色彩/解码 chunk 必须位于 IDAT 前。"};
          if (inherited.size() + size + 12 > encoding_defaults::codec_metadata_max_bytes + 1024)
            return std::unexpected{"APNG 全局 metadata 过大。"};
          inherited.insert(inherited.end(), bytes.begin() + pos, bytes.begin() + pos + size + 12);
        }
        else if ((type[0] & 32) == 0) return std::unexpected{"APNG 使用未知关键 chunk。"};
      }
      if (memory_usage_bytes() > budget) return std::unexpected{"APNG 帧索引超出内存预算。"};
      pos += size + 12;
    }
    if (!actl || !ended || specs.size() != count || specs.empty() || specs.back().data.empty())
      return std::unexpected{"APNG 帧数据/计数不完整。"};
    const auto raw_bytes = static_cast<std::uint64_t>(details.width) * details.height * (ihdr[8] == std::byte{16} ? 8 : 4);
    // A synthesized frame duplicates compressed payloads while libpng decodes it.
    const auto owned = memory_usage_bytes();
    if (owned > budget || raw_bytes > (budget - owned) / 4)
      return std::unexpected{"APNG 画布/局部帧/PREVIOUS 超出内存预算。"};
    details.decoder_id = "libpng-apng";
    return {};
  }

  std::expected<bool, std::string> next(std::stop_token stop) override {
    if (index == specs.size()) return false;
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    const auto& spec = specs[index];
    std::vector<std::byte> frame_png(bytes.begin(), bytes.begin() + 8);
    std::size_t encoded_size = 45 + inherited.size();
    for (auto data : spec.data) encoded_size += data.size() + 12;
    frame_png.reserve(encoded_size);
    auto header = ihdr;
    for (std::size_t i = 0; i < 4; ++i) {
      header[i] = std::byte{static_cast<unsigned char>((spec.width >> (24 - i * 8)) & 255)};
      header[i + 4] = std::byte{static_cast<unsigned char>((spec.height >> (24 - i * 8)) & 255)};
    }
    png_detail::append_png_chunk(frame_png, "IHDR", header);
    frame_png.insert(frame_png.end(), inherited.begin(), inherited.end());
    for (auto data : spec.data) png_detail::append_png_chunk(frame_png, "IDAT", data);
    png_detail::append_png_chunk(frame_png, "IEND", {});
    PngImageDecoder decoder;
    auto decoded = decoder.decode_memory(frame_png, "APNG frame", DecodeOptions{.copy_metadata_payloads = index == 0});
    if (!decoded) return std::unexpected{decoded.error()};
    if (!index) {
      auto initialized = canvas.initialize(details, decoded->image.bit_depth);
      if (!initialized) return std::unexpected{initialized.error()};
      canvas.image.source_info = decoded->image.source_info;
      canvas.image.metadata = std::move(decoded->image.metadata);
      canvas.image.significant_bits.reset();
    }
    if (decoded->image.bit_depth != canvas.image.bit_depth || decoded->image.width != spec.width || decoded->image.height != spec.height)
      return std::unexpected{"APNG 帧像素布局不一致。"};
    auto prepared = canvas.before(spec, stop);
    if (!prepared) return std::unexpected{prepared.error()};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    if (auto blended = canvas.blend(decoded->image, spec, stop); !blended) return std::unexpected{blended.error()};
    ++index;
    return true;
  }
};

struct WebpReader final : AnimationReader {
  std::vector<std::byte> bytes;
  std::unique_ptr<WebPAnimDecoder, webp_detail::WebPAnimDecoderDeleter> decoder;
  AnimationInfo description;
  ImageBuffer current;
  std::uint64_t budget{};
  std::size_t index{};
  int last_timestamp{};
  const AnimationInfo& info() const noexcept override { return description; }
  const ImageBuffer& frame() const noexcept override { return current; }
  std::uint64_t memory_usage_bytes() const noexcept override {
    return bytes.size() + static_cast<std::uint64_t>(description.width) * description.height * 20;
  }
  std::expected<void, std::string> open() {
    WebPAnimDecoderOptions options{};
    if (!WebPAnimDecoderOptionsInit(&options)) return std::unexpected{"初始化 WebP 动画失败。"};
    options.color_mode = MODE_RGBA; options.use_threads = 0;
    const WebPData data{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
    decoder.reset(WebPAnimDecoderNew(&data, &options));
    WebPAnimInfo info{};
    if (!decoder || !WebPAnimDecoderGetInfo(decoder.get(), &info) || !info.frame_count || info.frame_count > max_frames)
      return std::unexpected{"WebP 动画容器无效。"};
    description.width = info.canvas_width; description.height = info.canvas_height;
    if (bytes.size() > budget || !info.canvas_width || !info.canvas_height ||
        info.canvas_width > (budget - bytes.size()) / 64 / info.canvas_height)
      return std::unexpected{"WebP 动画工作区超出内存预算。"};
    description.decoder_id = "libwebp-animation";
    description.repetitions = info.loop_count ? static_cast<int>(info.loop_count) - 1 : -1;
    const auto* demux = WebPAnimDecoderGetDemuxer(decoder.get());
    WebPIterator iterator{};
    if (!WebPDemuxGetFrame(demux, 1, &iterator)) return std::unexpected{"WebP 动画缺少帧。"};
    do {
      if (iterator.duration < 0) { WebPDemuxReleaseIterator(&iterator); return std::unexpected{"WebP 动画时长无效。"}; }
      description.durations.push_back({static_cast<std::uint32_t>(std::max(iterator.duration, 1)), 1000});
      if (!iterator.duration) description.zero_delay_adjusted = true;
    } while (WebPDemuxNextFrame(&iterator));
    WebPDemuxReleaseIterator(&iterator);
    if (description.durations.size() != info.frame_count) return std::unexpected{"WebP 动画帧数不一致。"};
    current.width = info.canvas_width; current.height = info.canvas_height;
    current.pixel_format = PixelFormat::rgba; current.alpha_mode = AlphaMode::straight;
    current.source_info = ImageSourceInfo{.pixel_format = PixelFormat::rgba, .bit_depth = 8};
    if (auto metadata = webp_detail::copy_metadata(current, bytes); !metadata) return metadata;
    auto pixels = decoder_common::make_byte_buffer(info.canvas_width * std::size_t{4} * info.canvas_height, "WebP 动画");
    if (!pixels) return std::unexpected{pixels.error()};
    current.planes.push_back({std::move(*pixels), info.canvas_width * std::size_t{4}});
    return {};
  }
  std::expected<bool, std::string> next(std::stop_token stop) override {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    if (index == description.durations.size()) return false;
    std::uint8_t* pixels{}; int timestamp{};
    if (!WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp) || !pixels || timestamp < last_timestamp)
      return std::unexpected{"WebP 动画帧解码失败。"};
    std::memcpy(current.planes.front().bytes.data(), pixels, current.planes.front().bytes.size());
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    last_timestamp = timestamp; ++index;
    return true;
  }
};
} // namespace animation_detail

std::expected<bool, std::string> input_uses_animation_reader(const fs::path& path) {
  const auto extension = decoder_common::lower_extension(path);
  if (extension == L".gif") return true;
  if (extension == L".png" || extension == L".apng")
    return png_detail::file_contains_animation_control_chunk(path);
  if (extension == L".avif" || extension == L".avifs") return avif_has_sequence(path);
  if (extension == L".webp") {
    std::ifstream file(path, std::ios::binary);
    std::array<std::uint8_t, 64> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    WebPBitstreamFeatures features{};
    if (file.gcount() <= 0 || WebPGetFeatures(bytes.data(), static_cast<std::size_t>(file.gcount()), &features) != VP8_STATUS_OK)
      return std::unexpected{"WebP 动画探测失败。"};
    return features.has_animation != 0;
  }
  return false;
}

std::expected<std::unique_ptr<AnimationReader>, std::string> open_animation(
    const fs::path& path, std::uint64_t budget, std::stop_token stop = {}) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  auto animated = input_uses_animation_reader(path);
  if (!animated) return std::unexpected{animated.error()};
  if (!*animated) return std::unique_ptr<AnimationReader>{};
  const auto extension = decoder_common::lower_extension(path);
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (extension == L".avif" || extension == L".avifs") return open_avif_animation(path, budget, stop);
  std::error_code size_error;
  const auto file_size = fs::file_size(path, size_error);
  if (size_error) return std::unexpected{"无法读取动画输入大小。"};
  if (file_size > budget) return std::unexpected{"动画压缩输入超出内存预算。"};
  auto bytes = decoder_common::read_file_bytes(path, "动画");
  if (!bytes) return std::unexpected{bytes.error()};
  if (bytes->size() > budget) return std::unexpected{"动画压缩输入超出内存预算。"};
  if (extension == L".webp") {
    auto reader = std::make_unique<animation_detail::WebpReader>();
    reader->bytes = std::move(*bytes); reader->budget = budget;
    auto opened = reader->open();
    if (!opened) return std::unexpected{opened.error()};
    return std::unique_ptr<AnimationReader>{std::move(reader)};
  }
  if (extension == L".gif") {
    auto reader = std::make_unique<animation_detail::GifReader>();
    reader->bytes = std::move(*bytes); reader->budget = budget;
    auto opened = reader->open(stop);
    if (!opened) return std::unexpected{opened.error()};
    return std::unique_ptr<AnimationReader>{std::move(reader)};
  }
  auto reader = std::make_unique<animation_detail::ApngReader>();
  reader->bytes = std::move(*bytes); reader->budget = budget;
  auto opened = reader->open(stop);
  if (!opened) return std::unexpected{opened.error()};
  return std::unique_ptr<AnimationReader>{std::move(reader)};
}

} // namespace awj
