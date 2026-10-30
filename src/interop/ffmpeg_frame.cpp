#include "awj/awj_ffmpeg.h"
#include "sdk_internal.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>
}
#if AWJ_SDK_D3D11
#include "awj/awj_d3d11.h"
#include <d3d11.h>
#endif
#include <cmath>

namespace {
struct Lease {
  std::atomic<uint32_t> refs{1};
  awj_ffmpeg_api api{};
  std::vector<AVBufferRef*> buffers;
  std::vector<uint8_t> pixels;
  std::vector<uint8_t> icc;
  bool module_retained{};
  ~Lease() {
    for (auto it = buffers.rbegin(); it != buffers.rend(); ++it) { try { api.buffer_unref(&*it); } catch (...) {} }
    if (module_retained) { try { api.module_owner.release(api.module_owner.user); } catch (...) {} }
  }
};
void AWJ_CALL retain_lease(void* user) { static_cast<Lease*>(user)->refs.fetch_add(1); }
void AWJ_CALL release_lease(void* user) {
  auto* lease = static_cast<Lease*>(user);
  if (lease->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete lease;
}
struct Release { void operator()(Lease* value) const noexcept { release_lease(value); } };
using LeasePtr = std::unique_ptr<Lease, Release>;

awj_status dimensions(const AVFrame& frame, uint32_t& width, uint32_t& height) {
  if (frame.width <= 0 || frame.height <= 0 || frame.crop_left >= static_cast<uint32_t>(frame.width) ||
      frame.crop_right >= static_cast<uint32_t>(frame.width) - frame.crop_left ||
      frame.crop_top >= static_cast<uint32_t>(frame.height) ||
      frame.crop_bottom >= static_cast<uint32_t>(frame.height) - frame.crop_top)
    return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame 视频尺寸/裁切无效。");
  width = static_cast<uint32_t>(frame.width - frame.crop_left - frame.crop_right);
  height = static_cast<uint32_t>(frame.height - frame.crop_top - frame.crop_bottom);
  return AWJ_OK;
}
awj_status metadata(const AVFrame& frame, const awj_ffmpeg_options& options, awj_frame_desc& desc) {
  desc.frame_id = options.frame_id;
  desc.timestamp = frame.pts;
  if (frame.time_base.num > 0 && frame.time_base.den > 0) {
    desc.time_base_num = static_cast<uint32_t>(frame.time_base.num); desc.time_base_den = static_cast<uint32_t>(frame.time_base.den);
  } else if (options.time_base_num && options.time_base_den) {
    desc.time_base_num = options.time_base_num; desc.time_base_den = options.time_base_den;
  } else if (frame.pts != AV_NOPTS_VALUE) {
    return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame 有 pts，但没有可用 time_base。");
  }
  desc.color.primaries = frame.color_primaries == AVCOL_PRI_UNSPECIFIED ? -1 : static_cast<int32_t>(frame.color_primaries);
  desc.color.transfer = frame.color_trc == AVCOL_TRC_UNSPECIFIED ? -1 : static_cast<int32_t>(frame.color_trc);
  desc.color.matrix = frame.colorspace == AVCOL_SPC_UNSPECIFIED ? -1 : static_cast<int32_t>(frame.colorspace);
  desc.color.range = frame.color_range == AVCOL_RANGE_JPEG ? 1 : frame.color_range == AVCOL_RANGE_MPEG ? 0 : -1;
  desc.color.chroma_location = frame.chroma_location == AVCHROMA_LOC_UNSPECIFIED ? -1 : static_cast<int32_t>(frame.chroma_location);
  desc.color.linear_reference_white_nits = options.linear_reference_white_nits;
  if (options.alpha != AWJ_FFMPEG_ALPHA_FROM_SOURCE) desc.color.alpha = options.alpha;
  else desc.color.alpha = frame.alpha_mode == AVALPHA_MODE_PREMULTIPLIED ? AWJ_ALPHA_PREMULTIPLIED
      : frame.alpha_mode == AVALPHA_MODE_STRAIGHT ? AWJ_ALPHA_STRAIGHT : AWJ_ALPHA_NONE;
  if (frame.nb_side_data < 0 || frame.nb_side_data > 64 || (frame.nb_side_data && !frame.side_data))
    return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame side data 数量无效。");
  for (int i = 0; i < frame.nb_side_data; ++i) {
    const auto* side = frame.side_data[i];
    if (!side) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame side data 指针为空。");
    if (side->type == AV_FRAME_DATA_ICC_PROFILE) {
      if (side->size > 4 * 1024 * 1024 || (side->size && !side->data))
        return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "AVFrame ICC 超过预算或地址无效。");
      desc.color.icc = side->data; desc.color.icc_size = side->size;
    }
  }
  return AWJ_OK;
}
awj_status format(const AVFrame& frame, awj_frame_desc& desc) {
  desc.bit_depth = 8;
  switch (frame.format) {
    case AV_PIX_FMT_RGBA: desc.format = AWJ_RGBA8; break;
    case AV_PIX_FMT_BGRA: desc.format = AWJ_BGRA8; break;
    case AV_PIX_FMT_RGB24: desc.format = AWJ_RGB8; break;
    case AV_PIX_FMT_GRAY8: desc.format = AWJ_GRAY8; break;
    case AV_PIX_FMT_GRAY16LE: desc.format = AWJ_GRAY16; desc.bit_depth = 16; break;
    case AV_PIX_FMT_RGBA64LE: desc.format = AWJ_RGBA16; desc.bit_depth = 16; break;
    case AV_PIX_FMT_RGBAF16LE: desc.format = AWJ_RGBA16F; desc.bit_depth = 16; break;
    case AV_PIX_FMT_YUV420P: desc.format = AWJ_YUV420P; break;
    case AV_PIX_FMT_YUV422P: desc.format = AWJ_YUV422P; break;
    case AV_PIX_FMT_YUV444P: desc.format = AWJ_YUV444P; break;
    case AV_PIX_FMT_YUV420P10LE: desc.format = AWJ_YUV420P; desc.bit_depth = 10; break;
    case AV_PIX_FMT_YUV422P10LE: desc.format = AWJ_YUV422P; desc.bit_depth = 10; break;
    case AV_PIX_FMT_YUV444P10LE: desc.format = AWJ_YUV444P; desc.bit_depth = 10; break;
    case AV_PIX_FMT_YUV420P12LE: desc.format = AWJ_YUV420P; desc.bit_depth = 12; break;
    case AV_PIX_FMT_YUV422P12LE: desc.format = AWJ_YUV422P; desc.bit_depth = 12; break;
    case AV_PIX_FMT_YUV444P12LE: desc.format = AWJ_YUV444P; desc.bit_depth = 12; break;
    case AV_PIX_FMT_YUV420P16LE: desc.format = AWJ_YUV420P; desc.bit_depth = 16; break;
    case AV_PIX_FMT_YUV422P16LE: desc.format = AWJ_YUV422P; desc.bit_depth = 16; break;
    case AV_PIX_FMT_YUV444P16LE: desc.format = AWJ_YUV444P; desc.bit_depth = 16; break;
    case AV_PIX_FMT_NV12: desc.format = AWJ_NV12; break;
    case AV_PIX_FMT_P010LE: desc.format = AWJ_P010; desc.bit_depth = 10; break;
    default: return awj_sdk::fail(AWJ_UNSUPPORTED, "此 AVFrame 像素/硬件格式尚无适配器，未隐式下载或转换。");
  }
  return AWJ_OK;
}
awj_status describe(const AVFrame& frame, const awj_ffmpeg_options& options,
                      awj_frame_desc& desc, bool bounded_buffers) {
  awj_frame_desc_init(&desc);
  auto status = dimensions(frame, desc.width, desc.height); if (status != AWJ_OK) return status;
  status = format(frame, desc); if (status != AWJ_OK) return status;
  status = metadata(frame, options, desc); if (status != AWJ_OK) return status;
  awj_sdk::Layout shape{}; status = awj_sdk::layout(desc, shape); if (status != AWJ_OK) return status;
  desc.plane_count = shape.count;
  const bool subsampled_x = desc.format == AWJ_YUV420P || desc.format == AWJ_YUV422P || desc.format == AWJ_NV12 || desc.format == AWJ_P010;
  const bool subsampled_y = desc.format == AWJ_YUV420P || desc.format == AWJ_NV12 || desc.format == AWJ_P010;
  if ((subsampled_x && frame.crop_left % 2) || (subsampled_y && frame.crop_top % 2))
    return awj_sdk::fail(AWJ_UNSUPPORTED, "此奇数色度相位裁切需要显式转换，未静默取整。");
  if (frame.nb_extended_buf < 0 || frame.nb_extended_buf > 64 || (frame.nb_extended_buf && !frame.extended_buf))
    return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame 扩展缓冲区无效。");
  for (uint32_t p = 0; p < shape.count; ++p) {
    if (!frame.data[p] || !frame.linesize[p]) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame 平面为空。");
    const int64_t stride = frame.linesize[p];
    const uint64_t absolute = static_cast<uint64_t>(stride < 0 ? -stride : stride);
    const uint64_t y = p && subsampled_y ? frame.crop_top / 2 : frame.crop_top;
    uint64_t x = p && subsampled_x ? frame.crop_left / 2 : frame.crop_left;
    if (p && (desc.format == AWJ_NV12 || desc.format == AWJ_P010)) x *= 2;
    if (shape.count == 1) x *= desc.format == AWJ_RGB8 ? 3 : desc.format == AWJ_GRAY8 || desc.format == AWJ_GRAY16 ? 1 : 4;
    x *= desc.bit_depth > 8 ? 2u : 1u;
    const auto pointer = reinterpret_cast<uintptr_t>(frame.data[p]);
    const uint64_t displacement = y * absolute;
    if ((stride < 0 && displacement > pointer) || (stride > 0 && displacement > UINTPTR_MAX - pointer))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame 裁切地址溢出。");
    const auto first = stride < 0 ? pointer - displacement : pointer + displacement;
    if (x > UINTPTR_MAX - first) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame 横向裁切地址溢出。");
    const auto address = first + x;
    const auto use_buffer = [&](const AVBufferRef* buffer) {
      if (!buffer || !buffer->data) return false;
      const auto begin = reinterpret_cast<uintptr_t>(buffer->data);
      if (buffer->size > UINTPTR_MAX - begin || address < begin || address >= begin + buffer->size) return false;
      desc.planes[p] = {buffer->data, buffer->size, address - begin, stride}; return true;
    };
    bool found = false;
    if (bounded_buffers) {
      for (uint32_t i = 0; i < AV_NUM_DATA_POINTERS && !found; ++i) found = use_buffer(frame.buf[i]);
      for (int i = 0; i < frame.nb_extended_buf && !found; ++i) found = use_buffer(frame.extended_buf[i]);
      if (!found) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame 平面不在引用缓冲区范围内。");
    } else {
      uint64_t distance{};
      if (!awj_sdk::multiply(absolute, shape.rows[p] - 1, distance) ||
          distance > UINT64_MAX - shape.row_bytes[p] || (stride < 0 && distance > address))
        return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "非引用计数 AVFrame 行范围溢出。");
      const auto begin = stride < 0 ? address - distance : address;
      desc.planes[p] = {reinterpret_cast<const uint8_t*>(begin), distance + shape.row_bytes[p], stride < 0 ? distance : 0, stride};
    }
  }
  return AWJ_OK;
}
}

extern "C" {
void AWJ_CALL awj_ffmpeg_options_init(awj_ffmpeg_options* options) {
  if (!options) return;
  *options = {sizeof(*options), AWJ_ABI_VERSION, AWJ_COPY_ALLOW_REQUIRED, 0, 0,
              AWJ_FFMPEG_ALPHA_FROM_SOURCE, 0, 0, 0};
}
uint32_t AWJ_CALL awj_ffmpeg_expected_avutil_version(void) { return LIBAVUTIL_VERSION_INT; }
awj_status AWJ_CALL awj_ffmpeg_import(awj_context* context, const AVFrame* source,
                                     const awj_ffmpeg_api* api, const awj_ffmpeg_options* options,
                                     awj_frame** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少 FFmpeg 帧输出。");
    *output = nullptr;
    if (!context || !source || !awj_sdk::valid_header(api) || !awj_sdk::valid_header(options) ||
        !api->avutil_version || !api->buffer_ref || !api->buffer_unref ||
        (!!api->module_owner.retain != !!api->module_owner.release) || options->copy_policy > AWJ_COPY_REQUIRE_BORROWED ||
        (options->flags & ~3u) || (options->alpha != AWJ_FFMPEG_ALPHA_FROM_SOURCE && options->alpha > AWJ_ALPHA_PREMULTIPLIED) ||
        !std::isfinite(options->linear_reference_white_nits) || options->linear_reference_white_nits < 0 ||
        (!!options->time_base_num != !!options->time_base_den))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "FFmpeg provider、选项或 ABI 无效。");
    if (context->stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "context 已取消。");
    LeasePtr lease{new Lease}; lease->api = *api;
    if (api->module_owner.retain) { api->module_owner.retain(api->module_owner.user); lease->module_retained = true; }
    if (api->avutil_version() != LIBAVUTIL_VERSION_INT)
      return awj_sdk::fail(AWJ_UNSUPPORTED, "libavutil 与桥接组件头文件版本不匹配，请重建可选适配器。");
    const bool hardware = source->format == AV_PIX_FMT_D3D11;
    const bool referenced = source->buf[0] != nullptr;
    if (!referenced && (options->copy_policy == AWJ_COPY_REQUIRE_BORROWED || hardware))
      return awj_sdk::fail(AWJ_COPY_REQUIRED, "非引用计数 AVFrame 需要显式像素副本，或缺少硬件资源所有者。");
    if (source->nb_extended_buf < 0 || source->nb_extended_buf > 64 ||
        (source->nb_extended_buf && !source->extended_buf))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "AVFrame 扩展引用缓冲区无效。");
    awj_frame_desc desc{};
    if (!hardware) {
      auto status = describe(*source, *options, desc, referenced); if (status != AWJ_OK) return status;
      status = awj_sdk::validate(context->options, desc); if (status != AWJ_OK) return status;
    } else {
      awj_frame_desc_init(&desc);
      auto status = dimensions(*source, desc.width, desc.height); if (status != AWJ_OK) return status;
      status = metadata(*source, *options, desc); if (status != AWJ_OK) return status;
      if (source->crop_left || source->crop_top) return awj_sdk::fail(AWJ_UNSUPPORTED, "D3D11 AVFrame 左/上裁切尚需显式适配。");
    }
    if (desc.color.icc_size) {
      lease->icc.assign(desc.color.icc, desc.color.icc + desc.color.icc_size);
      desc.color.icc = lease->icc.data();
    }
    if (referenced) {
      lease->buffers.reserve(AV_NUM_DATA_POINTERS + static_cast<size_t>(source->nb_extended_buf) + 1);
      const auto hold = [&](const AVBufferRef* buffer) {
        if (!buffer) return true;
        auto* reference = api->buffer_ref(buffer);
        if (!reference) return false;
        lease->buffers.push_back(reference);
        return true;
      };
      // 反向释放，使硬件上下文晚于视频池/像素引用释放。
      if (!hold(source->hw_frames_ctx)) return awj_sdk::fail(AWJ_OUT_OF_MEMORY, "无法引用 FFmpeg 硬件上下文。");
      for (uint32_t i = 0; i < AV_NUM_DATA_POINTERS; ++i)
        if (!hold(source->buf[i])) return awj_sdk::fail(AWJ_OUT_OF_MEMORY, "无法引用 FFmpeg 像素缓冲。");
      for (int i = 0; i < source->nb_extended_buf; ++i)
        if (!hold(source->extended_buf[i])) return awj_sdk::fail(AWJ_OUT_OF_MEMORY, "无法引用 FFmpeg 扩展缓冲。");
    }
    if (hardware) {
#if AWJ_SDK_D3D11
      if (!source->data[0]) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "D3D11 AVFrame 纹理为空。");
      D3D11_TEXTURE2D_DESC texture_desc{};
      auto* texture = reinterpret_cast<ID3D11Texture2D*>(source->data[0]); texture->GetDesc(&texture_desc);
      const auto array_index = reinterpret_cast<uintptr_t>(source->data[1]);
      if (array_index >= texture_desc.ArraySize || array_index * texture_desc.MipLevels > UINT32_MAX)
        return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "D3D11 AVFrame array slice 无效。");
      awj_d3d11_options gpu; awj_d3d11_options_init(&gpu);
      gpu.texture = texture; gpu.width = desc.width; gpu.height = desc.height;
      gpu.subresource = static_cast<uint32_t>(array_index * texture_desc.MipLevels);
      gpu.flags = options->flags; gpu.copy_policy = options->copy_policy;
      gpu.color = desc.color; gpu.owner = {lease.get(), retain_lease, release_lease};
      const auto status = awj_d3d11_import(context, &gpu, output);
      if (status == AWJ_OK) {
        (*output)->desc.frame_id = options->frame_id; (*output)->desc.timestamp = desc.timestamp;
        (*output)->desc.time_base_num = desc.time_base_num; (*output)->desc.time_base_den = desc.time_base_den;
      }
      return status;
#else
      return awj_sdk::fail(AWJ_UNSUPPORTED, "此构建没有 D3D11 硬件帧适配器。");
#endif
    }
    uint64_t copied = 0;
    if (!referenced) {
      awj_sdk::Layout shape{}; const auto status = awj_sdk::layout(desc, shape); if (status != AWJ_OK) return status;
      for (uint32_t p = 0; p < shape.count; ++p) copied += shape.row_bytes[p] * shape.rows[p];
      lease->pixels.resize(static_cast<size_t>(copied));
      uint64_t offset = 0;
      for (uint32_t p = 0; p < shape.count; ++p) {
        for (uint32_t y = 0; y < shape.rows[p]; ++y)
          std::memcpy(lease->pixels.data() + offset + y * shape.row_bytes[p], awj_sdk::row(desc.planes[p], y), static_cast<size_t>(shape.row_bytes[p]));
        desc.planes[p] = {lease->pixels.data(), copied, offset, static_cast<int64_t>(shape.row_bytes[p])};
        offset += shape.row_bytes[p] * shape.rows[p];
      }
    }
    auto frame = std::make_unique<awj_frame>(); frame->desc = desc;
    frame->diagnostics.input_copy_bytes = copied;
    frame->storage = std::shared_ptr<Lease>{std::move(lease)};
    *output = frame.release();
    return AWJ_OK;
  });
}
}
