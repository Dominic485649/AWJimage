module;
#include "../compat/stdlib_compat.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
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

bool exceeds_budget(std::uint64_t bytes, std::uint64_t budget) noexcept {
  return budget != 0 && bytes > budget;
}

bool fits_working_set(std::uint64_t owned, std::uint64_t buffer_bytes,
    std::uint64_t copies, std::uint64_t budget) noexcept {
  // Only the original limit can mean unlimited. A finite exhausted remainder
  // stays zero available bytes; it is never passed on as a new unlimited limit.
  if (copies && buffer_bytes > (std::numeric_limits<std::uint64_t>::max() - owned) / copies)
    return false;
  return budget == 0 || (owned <= budget && (!copies || buffer_bytes <= (budget - owned) / copies));
}

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

  awj::compat::expected<void, std::string> initialize(const AnimationInfo& info, int depth) {
    auto stride = decoder_common::checked_rgba_stride(info.width, "动画画布", depth == 16 ? 2 : 1);
    if (!stride) return awj::compat::unexpected{stride.error()};
    auto bytes = decoder_common::checked_image_bytes(*stride, info.height, "动画画布");
    if (!bytes) return awj::compat::unexpected{bytes.error()};
    auto pixels = decoder_common::make_byte_buffer(*bytes, "动画画布");
    if (!pixels) return awj::compat::unexpected{pixels.error()};
    image = ImageBuffer{.width = info.width, .height = info.height,
        .pixel_format = PixelFormat::rgba, .alpha_mode = AlphaMode::straight, .bit_depth = depth};
    image.planes.push_back({std::move(*pixels), *stride});
    fill(FrameSpec{.width = info.width, .height = info.height}, background);
    return {};
  }

  bool fill(const FrameSpec& spec, std::array<std::uint16_t, 4> color, awj::compat::stop_token stop = {}) {
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

  awj::compat::expected<void, std::string> before(const FrameSpec& spec, awj::compat::stop_token stop) {
    const auto pixel_bytes = image.bit_depth == 16 ? 8u : 4u;
    auto& plane = image.planes.front();
    if (has_last) {
      if (last.dispose == 1 && !fill(last, background, stop)) return awj::compat::unexpected{"任务已取消。"};
      else if (last.dispose == 2) {
        for (std::size_t y = 0; y < last.height; ++y) {
          if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
          std::memcpy(plane.bytes.data() + (last.y + y) * plane.stride + last.x * pixel_bytes,
              previous.data() + y * last.width * pixel_bytes, last.width * pixel_bytes);
        }
      }
    }
    previous.clear();
    if (spec.dispose == 2) {
      auto size = decoder_common::checked_image_bytes(spec.width * pixel_bytes, spec.height, "动画 PREVIOUS");
      if (!size) return awj::compat::unexpected{size.error()};
      auto resized = decoder_common::resize_buffer(previous, *size, "动画 PREVIOUS");
      if (!resized) return awj::compat::unexpected{resized.error()};
      for (std::size_t y = 0; y < spec.height; ++y) {
        if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
        std::memcpy(previous.data() + y * spec.width * pixel_bytes,
            plane.bytes.data() + (spec.y + y) * plane.stride + spec.x * pixel_bytes, spec.width * pixel_bytes);
      }
    }
    last = FrameSpec{.x = spec.x, .y = spec.y, .width = spec.width, .height = spec.height, .dispose = spec.dispose};
    has_last = true;
    return {};
  }

  awj::compat::expected<void, std::string> blend(const ImageBuffer& frame, const FrameSpec& spec, awj::compat::stop_token stop) {
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
      if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
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

  awj::compat::expected<void, std::string> open(awj::compat::stop_token stop) {
    if (bytes.size() < 13 || (std::memcmp(bytes.data(), "GIF87a", 6) && std::memcmp(bytes.data(), "GIF89a", 6)))
      return awj::compat::unexpected{"GIF 签名无效。"};
    details.width = gif_detail::read_le_u16(bytes, 6);
    details.height = gif_detail::read_le_u16(bytes, 8);
    auto dimensions = decoder_common::make_image_dimensions_checked(details.width, details.height, "GIF 动画");
    if (!dimensions) return awj::compat::unexpected{dimensions.error()};
    std::size_t pos = 13;
    const auto packed = std::to_integer<unsigned>(bytes[10]);
    if (packed & 128) {
      const auto size = 3u * (2u << (packed & 7));
      if (size > bytes.size() - pos) return awj::compat::unexpected{"GIF 全局色表截断。"};
      const auto bg = std::to_integer<unsigned>(bytes[11]);
      if (bg * 3 + 2 >= size) return awj::compat::unexpected{"GIF 背景色索引无效。"};
      for (std::size_t c = 0; c < 3; ++c) opaque_background[c] = std::to_integer<unsigned char>(bytes[pos + bg * 3 + c]);
      pos += size;
    }
    FrameSpec control{};
    FrameDuration duration{0, 100};
    bool terminated = false;
    std::optional<int> loop;
    std::vector<std::byte> icc;
    while (pos < bytes.size()) {
      if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
      const auto marker = std::to_integer<unsigned>(bytes[pos++]);
      if (marker == 0x3b) { terminated = true; break; }
      if (marker == 0x21) {
        if (pos >= bytes.size()) return awj::compat::unexpected{"GIF 扩展截断。"};
        const auto label = std::to_integer<unsigned>(bytes[pos++]);
        if (label == 1) return awj::compat::unexpected{"GIF Plain Text 图形扩展暂不支持，不能保真合成。"};
        if (label == 0xf9) {
          if (bytes.size() - pos < 6 || bytes[pos] != std::byte{4} || bytes[pos + 5] != std::byte{0})
            return awj::compat::unexpected{"GIF GCE 无效。"};
          const auto flags = std::to_integer<unsigned>(bytes[pos + 1]);
          if (flags & 2) return awj::compat::unexpected{"GIF user-input 帧需要交互，无法转换为固定时间动画。"};
          const auto disposal = (flags >> 2) & 7;
          if (disposal > 3) return awj::compat::unexpected{"GIF disposal 未定义。"};
          control.dispose = disposal == 2 ? 1 : disposal == 3 ? 2 : 0;
          control.transparent = flags & 1 ? std::to_integer<unsigned char>(bytes[pos + 4]) : -1;
          duration = {gif_detail::read_le_u16(bytes, pos + 2), 100};
          if (!duration.numerator) { duration.numerator = 1; details.zero_delay_adjusted = true; }
          pos += 6;
        } else {
          bool netscape = false, profile = false;
          if (label == 0xff) {
            if (pos >= bytes.size() || std::to_integer<unsigned>(bytes[pos]) != 11 || bytes.size() - pos < 12)
              return awj::compat::unexpected{"GIF application 扩展无效。"};
            const std::string_view app{reinterpret_cast<const char*>(bytes.data() + pos + 1), 11};
            netscape = app == "NETSCAPE2.0" || app == "ANIMEXTS1.0";
            profile = app == "ICCRGBG1012";
            pos += 12;
          }
          bool first = true;
          for (;;) {
            if (pos >= bytes.size()) return awj::compat::unexpected{"GIF 扩展数据截断。"};
            const auto size = std::to_integer<unsigned>(bytes[pos++]);
            if (!size) break;
            if (size > bytes.size() - pos) return awj::compat::unexpected{"GIF 扩展数据截断。"};
            if (netscape && first) {
              if (size != 3 || bytes[pos] != std::byte{1}) return awj::compat::unexpected{"GIF loop 扩展无效。"};
              auto count = gif_detail::read_le_u16(bytes, pos + 1);
              const int repetitions = count ? count : -1;
              if (loop && *loop != repetitions) return awj::compat::unexpected{"GIF 存在冲突的 loop 扩展。"};
              loop = repetitions;
            }
            if (profile) {
              if (icc.size() + size > encoding_defaults::codec_metadata_max_bytes) return awj::compat::unexpected{"GIF ICC 太大。"};
              icc.insert(icc.end(), bytes.begin() + pos, bytes.begin() + pos + size);
            }
            pos += size;
            first = false;
          }
        }
      } else if (marker == 0x2c) {
        if (bytes.size() - pos < 9) return awj::compat::unexpected{"GIF 帧描述截断。"};
        auto spec = control;
        spec.x = gif_detail::read_le_u16(bytes, pos);
        spec.y = gif_detail::read_le_u16(bytes, pos + 2);
        spec.width = gif_detail::read_le_u16(bytes, pos + 4);
        spec.height = gif_detail::read_le_u16(bytes, pos + 6);
        if (!valid_rect(spec, details)) return awj::compat::unexpected{"GIF frame rect 超出画布。"};
        const auto flags = std::to_integer<unsigned>(bytes[pos + 8]);
        pos += 9;
        if (flags & 128) {
          const auto size = 3u * (2u << (flags & 7));
          if (size > bytes.size() - pos) return awj::compat::unexpected{"GIF 局部色表截断。"};
          pos += size;
        }
        if (pos >= bytes.size()) return awj::compat::unexpected{"GIF LZW 数据截断。"};
        ++pos;
        auto skipped = gif_detail::skip_sub_blocks(bytes, pos, "LZW");
        if (!skipped) return awj::compat::unexpected{skipped.error()};
        if (specs.size() == max_frames) return awj::compat::unexpected{"GIF 帧数超过 100000。"};
        specs.push_back(spec);
        if (!duration.numerator) { duration.numerator = 1; details.zero_delay_adjusted = true; }
        details.durations.push_back(duration);
        if (exceeds_budget(bytes.capacity() + icc.capacity() + index_bytes(specs, details), budget))
          return awj::compat::unexpected{"GIF 帧索引超出内存预算。"};
        control = {};
        duration = {0, 100};
      } else return awj::compat::unexpected{"GIF 记录类型无效。"};
    }
    if (!terminated || specs.empty()) return awj::compat::unexpected{"GIF 无帧或缺少 trailer。"};
    details.repetitions = loop.value_or(0);
    details.decoder_id = "giflib-animation";
    canvas.background = specs.front().transparent >= 0 ? std::array<std::uint16_t, 4>{} : opaque_background;
    auto stride = decoder_common::checked_rgba_stride(details.width, "GIF 动画");
    if (!stride) return awj::compat::unexpected{stride.error()};
    auto raw_bytes = decoder_common::checked_image_bytes(*stride, details.height, "GIF 动画");
    if (!raw_bytes) return awj::compat::unexpected{raw_bytes.error()};
    const auto owned = bytes.capacity() + icc.capacity() + index_bytes(specs, details);
    if (!fits_working_set(owned, *raw_bytes, 3, budget))
      return awj::compat::unexpected{"GIF 画布/局部帧/PREVIOUS 超出内存预算。"};
    auto initialized = canvas.initialize(details, 8);
    if (!initialized) return initialized;
    canvas.image.source_info = ImageSourceInfo{.pixel_format = PixelFormat::rgba, .bit_depth = 8,
        .color_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 0, .color_range = 1,
        .color_metadata_source = icc.empty() ? "gif-srgb" : "gif-icc"};
    if (!icc.empty()) canvas.image.metadata.push_back({MetadataKind::icc, std::move(icc)});
    state = {.data = bytes.data(), .size = bytes.size()};
    int error = 0;
    gif.reset(DGifOpen(&state, gif_detail::read_callback, &error));
    if (!gif) return awj::compat::unexpected{"giflib 无法打开动画。"};
    return {};
  }

  awj::compat::expected<bool, std::string> next(awj::compat::stop_token stop) override {
    if (index == specs.size()) return false;
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    auto decoded = gif_detail::read_first_frame(gif.get());
    if (!decoded) return awj::compat::unexpected{decoded.error()};
    const auto& spec = specs[index];
    if (decoded->image.Width != spec.width || decoded->image.Height != spec.height ||
        decoded->image.Left != spec.x || decoded->image.Top != spec.y)
      return awj::compat::unexpected{"giflib 与容器帧索引不一致。"};
    if (canvas.has_last) canvas.background = specs[index - 1].transparent >= 0
        ? std::array<std::uint16_t, 4>{} : opaque_background;
    auto prepared = canvas.before(spec, stop);
    if (!prepared) return awj::compat::unexpected{prepared.error()};
    const auto* colors = gif_detail::active_color_map(gif.get(), decoded->image);
    if (!colors) return awj::compat::unexpected{"GIF 帧缺少色表。"};
    for (std::size_t y = 0; y < spec.height; ++y) {
      if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
      for (std::size_t x = 0; x < spec.width; ++x) {
        const auto value = decoded->raster[y * spec.width + x];
        if (value == spec.transparent) continue;
        if (value >= colors->ColorCount) return awj::compat::unexpected{"GIF 像素索引超出色表。"};
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

  awj::compat::expected<void, std::string> open(awj::compat::stop_token stop) {
    png_detail::PngAnimationLayout layout;
    auto append_data = [&](std::span<const std::byte> data) {
      auto& parts = specs.back().data;
      const auto old_capacity = parts.capacity();
      parts.push_back(data);
      span_bytes += (parts.capacity() - old_capacity) * sizeof(std::span<const std::byte>);
    };
    auto scanned = png_detail::visit_png_chunks(bytes, "APNG",
        [&](std::string_view type, std::span<const std::byte> prefix,
            std::uint32_t size, std::uint64_t payload_offset) -> awj::compat::expected<void, std::string> {
      if (auto accepted = layout.accept(type, prefix, size, payload_offset); !accepted) return accepted;
      const auto offset = static_cast<std::size_t>(payload_offset);
      const auto payload = std::span<const std::byte>{bytes}.subspan(offset, size);
      if (type == "IHDR") {
        std::copy(payload.begin(), payload.end(), ihdr.begin());
        details.width = layout.width; details.height = layout.height;
      } else if (type == "acTL") {
        details.repetitions = layout.plays ? static_cast<int>(layout.plays) - 1 : -1;
      } else if (type == "fcTL") {
        FrameSpec spec{.x = png_detail::read_be_u32(payload, 12), .y = png_detail::read_be_u32(payload, 16),
            .width = png_detail::read_be_u32(payload, 4), .height = png_detail::read_be_u32(payload, 8),
            .dispose = std::to_integer<unsigned>(payload[24]), .blend = std::to_integer<unsigned>(payload[25])};
        // PREVIOUS on the first frame is defined as BACKGROUND.
        if (specs.empty() && spec.dispose == 2) spec.dispose = 1;
        auto u16 = [&](std::size_t pos) { return (std::to_integer<unsigned>(payload[pos]) << 8) | std::to_integer<unsigned>(payload[pos + 1]); };
        FrameDuration duration{u16(20), u16(22)};
        if (!duration.denominator) duration.denominator = 100;
        if (!duration.numerator) { duration = {1, 1000}; details.zero_delay_adjusted = true; }
        details.durations.push_back(duration);
        specs.push_back(std::move(spec));
      } else if (type == "IDAT") {
        if (layout.first_uses_idat) append_data(payload);
      } else if (type == "fdAT") {
        append_data(payload.subspan(4));
      } else if (png_detail::is_global_decoding_chunk(type)) {
        if (inherited.size() + size + 12ull > encoding_defaults::codec_metadata_max_bytes + 1024ull)
          return awj::compat::unexpected{"APNG 全局 metadata 过大。"};
        inherited.insert(inherited.end(), bytes.begin() + offset - 8, bytes.begin() + offset + size + 4);
      }
      if (exceeds_budget(memory_usage_bytes(), budget)) return awj::compat::unexpected{"APNG 帧索引超出内存预算。"};
      return {};
    }, stop);
    if (!scanned) return awj::compat::unexpected{scanned.error()};
    if (!layout.uses_animation_reader())
      return awj::compat::unexpected{"PNG 含独立默认静态图像或不含动画，应使用 PNG 静态 decoder。"};
    auto stride = decoder_common::checked_rgba_stride(details.width, "APNG", ihdr[8] == std::byte{16} ? 2 : 1);
    if (!stride) return awj::compat::unexpected{stride.error()};
    auto raw_bytes = decoder_common::checked_image_bytes(*stride, details.height, "APNG");
    if (!raw_bytes) return awj::compat::unexpected{raw_bytes.error()};
    // A synthesized frame duplicates compressed payloads while libpng decodes it.
    if (!fits_working_set(memory_usage_bytes(), *raw_bytes, 4, budget))
      return awj::compat::unexpected{"APNG 画布/局部帧/PREVIOUS 超出内存预算。"};
    details.decoder_id = "libpng-apng";
    return {};
  }

  awj::compat::expected<bool, std::string> next(awj::compat::stop_token stop) override {
    if (index == specs.size()) return false;
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    const auto& spec = specs[index];
    std::uint64_t encoded_size = 45 + inherited.size();
    const auto limit = encoding_defaults::effective_max_input_file_bytes();
    for (auto data : spec.data) {
      if (encoded_size > limit - 12 || data.size() > limit - encoded_size - 12)
        return awj::compat::unexpected{"APNG 合成帧 buffer 超过当前运行时上限。"};
      encoded_size += data.size() + 12;
    }
    if (encoded_size > std::numeric_limits<std::size_t>::max())
      return awj::compat::unexpected{"APNG 合成帧 buffer 尺寸溢出。"};
    std::vector<std::byte> frame_png(bytes.begin(), bytes.begin() + 8);
    frame_png.reserve(static_cast<std::size_t>(encoded_size));
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
    if (!decoded) return awj::compat::unexpected{decoded.error()};
    if (!index) {
      auto initialized = canvas.initialize(details, decoded->image.bit_depth);
      if (!initialized) return awj::compat::unexpected{initialized.error()};
      canvas.image.source_info = decoded->image.source_info;
      canvas.image.metadata = std::move(decoded->image.metadata);
      canvas.image.significant_bits.reset();
    }
    if (decoded->image.bit_depth != canvas.image.bit_depth || decoded->image.width != spec.width || decoded->image.height != spec.height)
      return awj::compat::unexpected{"APNG 帧像素布局不一致。"};
    auto prepared = canvas.before(spec, stop);
    if (!prepared) return awj::compat::unexpected{prepared.error()};
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (auto blended = canvas.blend(decoded->image, spec, stop); !blended) return awj::compat::unexpected{blended.error()};
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
    std::uint64_t size = bytes.capacity() + static_cast<std::uint64_t>(description.width) * description.height * 20 +
        description.durations.capacity() * sizeof(FrameDuration);
    for (const auto& block : current.metadata) size += block.bytes.capacity();
    return size;
  }
  awj::compat::expected<void, std::string> open(awj::compat::stop_token stop = {}) {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &features) != VP8_STATUS_OK ||
        !features.has_animation || features.width <= 0 || features.height <= 0)
      return awj::compat::unexpected{"WebP 动画尺寸/容器无效。"};
    auto stride = decoder_common::checked_rgba_stride(static_cast<std::size_t>(features.width), "WebP 动画");
    if (!stride) return awj::compat::unexpected{stride.error()};
    auto raw_bytes = decoder_common::checked_image_bytes(*stride, static_cast<std::size_t>(features.height), "WebP 动画");
    if (!raw_bytes) return awj::compat::unexpected{raw_bytes.error()};
    if (!fits_working_set(bytes.capacity(), *raw_bytes, 16, budget))
      return awj::compat::unexpected{"WebP 动画工作区超出内存预算。"};
    WebPAnimDecoderOptions options{};
    if (!WebPAnimDecoderOptionsInit(&options)) return awj::compat::unexpected{"初始化 WebP 动画失败。"};
    options.color_mode = MODE_RGBA; options.use_threads = 0;
    const WebPData data{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
    decoder.reset(WebPAnimDecoderNew(&data, &options));
    WebPAnimInfo info{};
    if (!decoder || !WebPAnimDecoderGetInfo(decoder.get(), &info) || !info.frame_count || info.frame_count > max_frames ||
        info.canvas_width != static_cast<unsigned>(features.width) || info.canvas_height != static_cast<unsigned>(features.height))
      return awj::compat::unexpected{"WebP 动画容器无效。"};
    description.width = info.canvas_width; description.height = info.canvas_height;
    description.decoder_id = "libwebp-animation";
    description.repetitions = info.loop_count ? static_cast<int>(info.loop_count) - 1 : -1;
    const auto* demux = WebPAnimDecoderGetDemuxer(decoder.get());
    WebPIterator iterator{};
    if (!WebPDemuxGetFrame(demux, 1, &iterator)) return awj::compat::unexpected{"WebP 动画缺少帧。"};
    do {
      if (stop.stop_requested()) { WebPDemuxReleaseIterator(&iterator); return awj::compat::unexpected{"任务已取消。"}; }
      if (iterator.duration < 0) { WebPDemuxReleaseIterator(&iterator); return awj::compat::unexpected{"WebP 动画时长无效。"}; }
      description.durations.push_back({static_cast<std::uint32_t>(std::max(iterator.duration, 1)), 1000});
      if (!iterator.duration) description.zero_delay_adjusted = true;
    } while (WebPDemuxNextFrame(&iterator));
    WebPDemuxReleaseIterator(&iterator);
    if (description.durations.size() != info.frame_count) return awj::compat::unexpected{"WebP 动画帧数不一致。"};
    current.width = info.canvas_width; current.height = info.canvas_height;
    current.pixel_format = PixelFormat::rgba; current.alpha_mode = AlphaMode::straight;
    current.source_info = ImageSourceInfo{.pixel_format = PixelFormat::rgba, .bit_depth = 8};
    if (auto metadata = webp_detail::copy_metadata(current, bytes); !metadata) return metadata;
    auto owned = bytes.capacity() + description.durations.capacity() * sizeof(FrameDuration);
    for (const auto& block : current.metadata) owned += block.bytes.capacity();
    if (!fits_working_set(owned, *raw_bytes, 16, budget))
      return awj::compat::unexpected{"WebP 动画 metadata/工作区超出内存预算。"};
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    auto pixels = decoder_common::make_byte_buffer(*raw_bytes, "WebP 动画");
    if (!pixels) return awj::compat::unexpected{pixels.error()};
    current.planes.push_back({std::move(*pixels), *stride});
    return {};
  }
  awj::compat::expected<bool, std::string> next(awj::compat::stop_token stop) override {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (index == description.durations.size()) return false;
    std::uint8_t* pixels{}; int timestamp{};
    if (!WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp) || !pixels || timestamp < last_timestamp)
      return awj::compat::unexpected{"WebP 动画帧解码失败。"};
    std::memcpy(current.planes.front().bytes.data(), pixels, current.planes.front().bytes.size());
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    last_timestamp = timestamp; ++index;
    return true;
  }
};
} // namespace animation_detail

awj::compat::expected<bool, std::string> input_uses_animation_reader(
    const fs::path& path, awj::compat::stop_token stop = {}) {
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  const auto extension = decoder_common::lower_extension(path);
  if (extension == L".gif") return true;
  if (extension == L".png" || extension == L".apng")
    return png_detail::file_contains_animation_control_chunk(path, stop);
  if (extension == L".avif" || extension == L".avifs") return avif_has_sequence(path);
  if (extension == L".webp") {
    std::ifstream file(path, std::ios::binary);
    std::array<std::uint8_t, 64> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    WebPBitstreamFeatures features{};
    if (file.gcount() <= 0 || WebPGetFeatures(bytes.data(), static_cast<std::size_t>(file.gcount()), &features) != VP8_STATUS_OK)
      return awj::compat::unexpected{"WebP 动画探测失败。"};
    return features.has_animation != 0;
  }
  return false;
}

// budget == 0 disables only working-set admission, not input/buffer safety limits.
awj::compat::expected<std::unique_ptr<AnimationReader>, std::string> open_animation(
    const fs::path& path, std::uint64_t budget, awj::compat::stop_token stop = {}) {
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  auto animated = input_uses_animation_reader(path, stop);
  if (!animated) return awj::compat::unexpected{animated.error()};
  if (!*animated) return std::unique_ptr<AnimationReader>{};
  const auto extension = decoder_common::lower_extension(path);
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  if (extension == L".avif" || extension == L".avifs") return open_avif_animation(path, budget, stop);
  std::error_code size_error;
  const auto file_size = fs::file_size(path, size_error);
  if (size_error) return awj::compat::unexpected{"无法读取动画输入大小。"};
  if (file_size > encoding_defaults::effective_max_input_file_bytes())
    return awj::compat::unexpected{"动画压缩输入超过当前运行时上限。"};
  if (animation_detail::exceeds_budget(file_size, budget)) return awj::compat::unexpected{"动画压缩输入超出内存预算。"};
  auto bytes = decoder_common::read_file_bytes(path, "动画");
  if (!bytes) return awj::compat::unexpected{bytes.error()};
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  if (animation_detail::exceeds_budget(bytes->capacity(), budget)) return awj::compat::unexpected{"动画压缩输入超出内存预算。"};
  if (extension == L".webp") {
    auto reader = std::make_unique<animation_detail::WebpReader>();
    reader->bytes = std::move(*bytes); reader->budget = budget;
    auto opened = reader->open(stop);
    if (!opened) return awj::compat::unexpected{opened.error()};
    return std::unique_ptr<AnimationReader>{std::move(reader)};
  }
  if (extension == L".gif") {
    auto reader = std::make_unique<animation_detail::GifReader>();
    reader->bytes = std::move(*bytes); reader->budget = budget;
    auto opened = reader->open(stop);
    if (!opened) return awj::compat::unexpected{opened.error()};
    return std::unique_ptr<AnimationReader>{std::move(reader)};
  }
  auto reader = std::make_unique<animation_detail::ApngReader>();
  reader->bytes = std::move(*bytes); reader->budget = budget;
  auto opened = reader->open(stop);
  if (!opened) return awj::compat::unexpected{opened.error()};
  return std::unique_ptr<AnimationReader>{std::move(reader)};
}

} // namespace awj
