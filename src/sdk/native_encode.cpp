#include "sdk_internal.h"
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <utility>

import awj.image;
import awj.codec;
import awj.config;
import awj.core;
import awj.native_backend;
import awj.resource_planner;
import awj.pixel_conversion;

namespace awj_sdk {
namespace {
awj::PixelFormat pixel_format(uint32_t format) {
  switch (format) {
    case AWJ_RGB8: return awj::PixelFormat::rgb;
    case AWJ_GRAY8: case AWJ_GRAY16: return awj::PixelFormat::gray;
    case AWJ_YUV420P: case AWJ_NV12: case AWJ_P010: return awj::PixelFormat::yuv420;
    case AWJ_YUV422P: return awj::PixelFormat::yuv422;
    case AWJ_YUV444P: return awj::PixelFormat::yuv444;
    default: return awj::PixelFormat::rgba;
  }
}
awj::OutputFormat output_format(uint32_t codec) {
  switch (codec) {
    case AWJ_CODEC_PNG: return awj::OutputFormat::png;
    case AWJ_CODEC_WEBP: return awj::OutputFormat::webp;
    case AWJ_CODEC_JXL: return awj::OutputFormat::jxl;
    case AWJ_CODEC_JPEGLI: return awj::OutputFormat::jpgli;
    default: return awj::OutputFormat::avif;
  }
}
std::optional<int> optional_color(int32_t value) { return value < 0 ? std::nullopt : std::optional<int>{value}; }

awj_status import_image(const awj_frame& frame, const awj_encode_options& options,
                         awj::ImageBuffer& image, awj_diagnostics& diagnostics) {
  const auto& desc = frame.desc;
  Layout shape{};
  const auto shape_status = layout(desc, shape);
  if (shape_status != AWJ_OK) return shape_status;
  if (!desc.color.icc_size && (desc.color.primaries < 0 || desc.color.transfer < 0 ||
      desc.color.matrix < 0 || desc.color.range < 0))
    return fail(AWJ_INVALID_ARGUMENT, "内存编码必须明确源色彩 CICP/range，或提供有效 ICC。");
  if (desc.format >= AWJ_YUV420P && (desc.color.matrix < 0 || desc.color.range < 0))
    return fail(AWJ_INVALID_ARGUMENT, "YUV 帧必须明确 matrix/range；ICC 不能替代 YUV 采样矩阵。");
  if (desc.format == AWJ_RGBA16F && (desc.color.linear_reference_white_nits <= 0 || desc.color.transfer != 8))
    return fail(AWJ_INVALID_ARGUMENT, "FP16 编码输入须明确 linear transfer 和参考白亮度，不能猜测其为 scRGB。");
  const bool semiplanar = desc.format == AWJ_NV12 || desc.format == AWJ_P010;
  const bool swizzle = desc.format == AWJ_BGRA8;
  bool repack = false;
  for (uint32_t p = 0; p < shape.count; ++p) {
    const auto& plane = desc.planes[p];
    const auto* first = row(plane, 0);
    repack |= (options.codec == AWJ_CODEC_JXL && desc.format <= AWJ_RGBA16F &&
               static_cast<uint64_t>(plane.stride) != shape.row_bytes[p]) ||
        plane.stride < 0 || static_cast<uint64_t>(plane.stride) > UINT32_MAX ||
        (desc.bit_depth > 8 && ((reinterpret_cast<uintptr_t>(first) & 1) || (plane.stride & 1)));
  }
  if (options.copy_policy == AWJ_COPY_REQUIRE_BORROWED && (repack || semiplanar || swizzle))
    return fail(AWJ_COPY_REQUIRED, "目标编码器需要行重排、通道交换或半平面转换；严格输入复用模式拒绝。");

  image.width = desc.width;
  image.height = desc.height;
  image.pixel_format = pixel_format(desc.format);
  image.bit_depth = static_cast<int>(desc.bit_depth);
  image.alpha_mode = desc.color.alpha == AWJ_ALPHA_STRAIGHT ? awj::AlphaMode::straight
      : desc.color.alpha == AWJ_ALPHA_PREMULTIPLIED ? awj::AlphaMode::premultiplied : awj::AlphaMode::none;
  image.sample_representation = desc.format == AWJ_RGBA16F
      ? awj::SampleRepresentation::ieee_half_float : awj::SampleRepresentation::unorm;
  image.source_info = awj::ImageSourceInfo{
      .pixel_format = image.pixel_format, .bit_depth = image.bit_depth,
      .color_primaries = optional_color(desc.color.primaries),
      .transfer_characteristics = optional_color(desc.color.transfer),
      .matrix_coefficients = optional_color(desc.color.matrix),
      .color_range = optional_color(desc.color.range),
      .has_hdr_metadata = desc.format == AWJ_RGBA16F || desc.color.transfer == 16 || desc.color.transfer == 18,
      .color_metadata_source = desc.color.icc_size ? "source-icc" : "external-frame-cicp"};
  image.source_info->chroma_location = desc.color.chroma_location > 0 ? optional_color(desc.color.chroma_location) : std::nullopt;
  if (desc.color.linear_reference_white_nits > 0)
    image.source_info->linear_reference_white_nits = desc.color.linear_reference_white_nits;
  if (desc.color.icc_size) {
    const auto* begin = reinterpret_cast<const std::byte*>(desc.color.icc);
    image.metadata.push_back({awj::MetadataKind::icc, {begin, begin + desc.color.icc_size}});
  }
  if (semiplanar) {
    image.planes.resize(3);
    const auto sample_bytes = desc.bit_depth > 8 ? 2u : 1u;
    const uint32_t cw = desc.width / 2 + desc.width % 2;
    const uint32_t ch = desc.height / 2 + desc.height % 2;
    for (uint32_t p = 0; p < 3; ++p) {
      auto& target = image.planes[p];
      const auto width = p ? cw : desc.width;
      const auto height = p ? ch : desc.height;
      target.stride = static_cast<size_t>(width) * sample_bytes;
      target.bytes.resize(target.stride * height);
      for (uint32_t y = 0; y < height; ++y) {
        const auto* source = row(desc.planes[p ? 1 : 0], y);
        auto* destination = target.bytes.data() + y * target.stride;
        for (uint32_t x = 0; x < width; ++x) {
          const auto offset = (p ? static_cast<size_t>(x) * 2 + (p - 1) : x) * sample_bytes;
          if (sample_bytes == 1) destination[x] = static_cast<std::byte>(source[offset]);
          else {
            uint16_t value{};
            std::memcpy(&value, source + offset, 2);
            value >>= 6; // P010 的有效 10 位位于每个小端 word 的高位。
            std::memcpy(destination + static_cast<size_t>(x) * 2, &value, 2);
          }
        }
      }
      diagnostics.input_copy_bytes += target.bytes.size();
    }
    return AWJ_OK;
  }

  awj_frame_retain(const_cast<awj_frame*>(&frame));
  auto lease = std::shared_ptr<const void>{&frame, [](const void* value) {
    awj_frame_release(const_cast<awj_frame*>(static_cast<const awj_frame*>(value)));
  }};
  image.planes.resize(image.pixel_format == awj::PixelFormat::gray ? 3 : shape.count);
  for (uint32_t p = 0; p < shape.count; ++p) {
    auto& target = image.planes[p];
    if (repack || swizzle) {
      target.stride = static_cast<size_t>(shape.row_bytes[p]);
      target.bytes.resize(target.stride * shape.rows[p]);
      for (uint32_t y = 0; y < shape.rows[p]; ++y) {
        auto* destination = target.bytes.data() + y * target.stride;
        const auto* source = row(desc.planes[p], y);
        std::memcpy(destination, source, target.stride);
        if (swizzle) for (uint32_t x = 0; x < desc.width; ++x)
          std::swap(destination[x * 4], destination[x * 4 + 2]);
      }
      diagnostics.input_copy_bytes += target.bytes.size();
    } else {
      const auto& source = desc.planes[p];
      target.stride = static_cast<size_t>(source.stride);
      target.borrowed = {reinterpret_cast<const std::byte*>(row(source, 0)), static_cast<size_t>(source.size - source.offset)};
      target.lease = lease;
    }
  }
  return AWJ_OK;
}
}

uint64_t native_capabilities() noexcept { return AWJ_CAP_NATIVE_CODECS; }
awj_status native_encode(awj_context& context, const awj_frame& frame,
                         const awj_encode_options& options, awj_result& output) {
  if (options.lossless && options.codec == AWJ_CODEC_JPEGLI)
    return fail(AWJ_UNSUPPORTED, "JPEGli 不提供像素无损编码，不能把 quality=100 当作无损。");
  if (options.copy_policy == AWJ_COPY_REQUIRE_BORROWED &&
      (frame.diagnostics.input_copy_bytes || frame.diagnostics.gpu_copy_bytes || frame.diagnostics.gpu_readback_bytes))
    return fail(AWJ_COPY_REQUIRED, "此帧在 transport/硬件导入中已经发生搬运，不能标为严格零复制输入。");
  uint64_t pixels{}, workspace{};
  if (!multiply(frame.desc.width, frame.desc.height, pixels) || !multiply(pixels, 96, workspace))
    return fail(AWJ_LIMIT_EXCEEDED, "编码工作集估算溢出。");
  Admission admission{context, workspace};
  if (!admission.acquired()) return fail(AWJ_BUSY, "context 的并行编码工作集预算已用尽。");
  awj::ImageDecodeResult decoded;
  const auto imported = import_image(frame, options, decoded.image, output.diagnostics);
  if (imported != AWJ_OK) return imported;
  const auto& desc = frame.desc;
  const bool shifted_chroma = (desc.format == AWJ_YUV420P || desc.format == AWJ_YUV422P ||
      desc.format == AWJ_NV12 || desc.format == AWJ_P010) && desc.color.chroma_location > 0 && desc.color.chroma_location != 2;
  if (shifted_chroma) {
    if (options.copy_policy == AWJ_COPY_REQUIRE_BORROWED)
      return fail(AWJ_COPY_REQUIRED, "非居中的色度相位需要显式重采样；严格输入复用模式拒绝。");
    const awj::PixelConversionRequest request{.format = awj::PixelFormat::rgba,
        .bit_depth = desc.bit_depth > 8 ? 16 : 8, .matrix_coefficients = 0,
        .color_range = 1, .preserve_alpha = desc.color.alpha != AWJ_ALPHA_NONE};
    auto normalized = awj::convert_image_pixels(decoded.image, request, context.stop.get_token());
    if (!normalized) return fail(AWJ_UNSUPPORTED, normalized.error());
    normalized->source_info->chroma_location.reset();
    for (const auto& plane : normalized->planes) output.diagnostics.conversion_bytes += plane.read_bytes().size();
    decoded.image = std::move(*normalized);
  }
  decoded.decoder_id = "external-frame";
  awj::AppConfig config;
  config.input_path.clear();
  config.output_dir.clear();
  config.output_format = output_format(options.codec);
  config.quality = options.lossless ? 100 : options.quality;
  config.speed = options.speed;
  if (options.bit_depth) config.bit_depth = static_cast<int>(options.bit_depth);
  if (options.visual_quality) config.visual_quality = options.quality;
  config.visual_quality_gpu = false;
  config.allow_wic_fallback = false;
  config.max_jobs = static_cast<int>(context.options.max_threads);
  config.memory_limit_bytes = context.options.memory_budget_bytes;
  config.write_log = false;
  config.write_summary = false;
  awj::ResourcePlan resources{.file_parallelism = 1,
      .encoder_threads_per_file = config.max_jobs, .global_thread_budget = config.max_jobs,
      .memory_limit_bytes = config.memory_limit_bytes, .memory_file_parallelism = 1};
  // 禁用的 logger 构造函数会在任何路径/文件操作之前返回。
  awj::FileLogger no_file_logger{{}, false};
  awj::NativeBackend backend{config, no_file_logger, resources};
  auto encoded = backend.encode_memory(std::move(decoded), context.stop.get_token());
  if (!encoded) return fail(context.stop.stop_requested() ? AWJ_CANCELLED : AWJ_UNSUPPORTED, encoded.error());
  output.bytes = std::move(encoded->encoded.bytes);
  // 第三方编码器可能内部转换/暂存；不能把不可观测的工作缓冲宣称为零复制。
  output.diagnostics.encoder_internal_copies_known = 0;
  output.diagnostics.conversion_bytes_known = 0;
  return AWJ_OK;
}
}
