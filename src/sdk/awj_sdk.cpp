#include "sdk_internal.h"
#include <algorithm>
#include <cmath>

#ifndef AWJ_SDK_VERSION
#define AWJ_SDK_VERSION "1.3.0"
#endif

namespace {
thread_local std::array<char, 1024> last_error{};
constexpr uint64_t gib = UINT64_C(1024) * 1024 * 1024;
}

namespace awj_sdk {
void clear_error() noexcept { last_error[0] = '\0'; }
awj_status fail(awj_status status, std::string_view message) noexcept {
  const auto count = std::min(message.size(), last_error.size() - 1);
  std::memcpy(last_error.data(), message.data(), count);
  last_error[count] = '\0';
  return status;
}

awj_status layout(const awj_frame_desc& desc, Layout& result) noexcept {
  if (!desc.width || !desc.height) return fail(AWJ_INVALID_ARGUMENT, "帧尺寸不能为零。");
  const uint64_t width = desc.width;
  const auto half_width = (width + 1) / 2;
  const auto half_height = (static_cast<uint64_t>(desc.height) + 1) / 2;
  uint32_t channels = 0;
  uint32_t sample_bytes = 1;
  switch (desc.format) {
    case AWJ_RGBA8: case AWJ_BGRA8: channels = 4; break;
    case AWJ_RGB8: channels = 3; break;
    case AWJ_GRAY8: channels = 1; break;
    case AWJ_RGBA16: case AWJ_RGBA16F: channels = 4; sample_bytes = 2; break;
    case AWJ_GRAY16: channels = 1; sample_bytes = 2; break;
    case AWJ_YUV420P: case AWJ_YUV422P: case AWJ_YUV444P:
      if (desc.bit_depth != 8 && desc.bit_depth != 10 && desc.bit_depth != 12 && desc.bit_depth != 16)
        return fail(AWJ_INVALID_ARGUMENT, "YUV 有效位深必须是 8/10/12/16。");
      sample_bytes = desc.bit_depth > 8 ? 2u : 1u;
      result.count = 3;
      result.row_bytes[0] = width * sample_bytes;
      result.row_bytes[1] = result.row_bytes[2] =
          (desc.format == AWJ_YUV444P ? width : half_width) * sample_bytes;
      result.rows[0] = desc.height;
      result.rows[1] = result.rows[2] = desc.format == AWJ_YUV420P
          ? static_cast<uint32_t>(half_height) : desc.height;
      return AWJ_OK;
    case AWJ_NV12: case AWJ_P010:
      sample_bytes = desc.format == AWJ_P010 ? 2u : 1u;
      if (desc.bit_depth != (desc.format == AWJ_P010 ? 10u : 8u))
        return fail(AWJ_INVALID_ARGUMENT, "半平面格式与有效位深不符。");
      result.count = 2;
      result.row_bytes[0] = width * sample_bytes;
      result.row_bytes[1] = half_width * 2 * sample_bytes;
      result.rows[0] = desc.height;
      result.rows[1] = static_cast<uint32_t>(half_height);
      return AWJ_OK;
    default: return fail(AWJ_UNSUPPORTED, "不支持此像素格式。");
  }
  if (desc.bit_depth != sample_bytes * 8)
    return fail(AWJ_INVALID_ARGUMENT, "打包像素格式与样本位深不符。");
  result.count = 1;
  result.row_bytes[0] = width * channels * sample_bytes;
  result.rows[0] = desc.height;
  return AWJ_OK;
}

awj_status validate(const awj_context_options& options, const awj_frame_desc& desc) noexcept {
  if (!valid_header(&desc) || desc.reserved || desc.color.reserved)
    return fail(AWJ_INVALID_ARGUMENT, "帧描述符的 size/version/reserved 无效。");
  const auto& color = desc.color;
  const auto cicp = [](int32_t value) { return value == -1 || (value >= 0 && value <= 255); };
  if (!cicp(color.primaries) || !cicp(color.transfer) || !cicp(color.matrix) ||
      color.range < -1 || color.range > 1 || color.chroma_location < -1 || color.chroma_location > 6 ||
      color.alpha > AWJ_ALPHA_PREMULTIPLIED ||
      !std::isfinite(color.linear_reference_white_nits) || color.linear_reference_white_nits < 0 ||
      color.icc_size > 4 * 1024 * 1024 || (color.icc_size && !color.icc) ||
      !desc.time_base_num || !desc.time_base_den)
    return fail(AWJ_INVALID_ARGUMENT, "色彩、ICC 或时间基信息无效。");
  if (color.alpha && desc.format != AWJ_RGBA8 && desc.format != AWJ_BGRA8 &&
      desc.format != AWJ_RGBA16 && desc.format != AWJ_RGBA16F)
    return fail(AWJ_UNSUPPORTED, "此帧格式没有受支持的 alpha 平面。");
  Layout shape{};
  const auto status = layout(desc, shape);
  if (status != AWJ_OK) return status;
  if (desc.plane_count != shape.count) return fail(AWJ_INVALID_ARGUMENT, "平面数量与像素格式不符。");
  uint64_t total = color.icc_size;
  for (uint32_t p = 0; p < shape.count; ++p) {
    const auto& plane = desc.planes[p];
    if (!plane.data || !plane.size || !plane.stride || plane.stride == INT64_MIN || plane.offset >= plane.size)
      return fail(AWJ_INVALID_ARGUMENT, "平面地址、stride 或 offset 无效。");
    const auto stride = static_cast<uint64_t>(plane.stride < 0 ? -plane.stride : plane.stride);
    uint64_t displacement{};
    uint64_t samples{};
    if (stride < shape.row_bytes[p] ||
        !multiply(stride, shape.rows[p] - 1, displacement) ||
        !multiply(shape.row_bytes[p], shape.rows[p], samples) ||
        samples > options.max_frame_bytes || total > options.max_frame_bytes - samples)
      return fail(AWJ_LIMIT_EXCEEDED, "平面尺寸溢出或超过帧预算。");
    if ((plane.stride < 0 && displacement > plane.offset) ||
        (plane.stride > 0 && displacement > plane.size - plane.offset))
      return fail(AWJ_INVALID_ARGUMENT, "平面行超出分配范围。");
    const auto last = plane.stride < 0 ? plane.offset : plane.offset + displacement;
    if (last > plane.size || shape.row_bytes[p] > plane.size - last ||
        plane.size > static_cast<uint64_t>(PTRDIFF_MAX) ||
        reinterpret_cast<uintptr_t>(plane.data) > UINTPTR_MAX - static_cast<uintptr_t>(plane.size))
      return fail(AWJ_INVALID_ARGUMENT, "平面长度或地址计算越界。");
    total += samples;
  }
  return AWJ_OK;
}

const uint8_t* row(const awj_plane& plane, uint32_t index) noexcept {
  const auto stride = static_cast<uint64_t>(plane.stride < 0 ? -plane.stride : plane.stride);
  const auto offset = plane.stride < 0 ? plane.offset - stride * index : plane.offset + stride * index;
  return plane.data + static_cast<size_t>(offset);
}

#if !AWJ_SDK_NATIVE_CODECS
awj_status native_encode(awj_context&, const awj_frame&, const awj_encode_options&, awj_result&) {
  return fail(AWJ_UNSUPPORTED, "此 SDK 构建未包含原生编码器组件。");
}
uint64_t native_capabilities() noexcept { return 0; }
#endif
}

extern "C" {
uint32_t AWJ_CALL awj_abi_version(void) { return AWJ_ABI_VERSION; }
const char* AWJ_CALL awj_version(void) { return AWJ_SDK_VERSION; }
const char* AWJ_CALL awj_last_error(void) { return last_error.data(); }
uint64_t AWJ_CALL awj_capabilities(void) {
  uint64_t result = AWJ_CAP_CPU_FRAMES | AWJ_CAP_FRAME_STREAM | awj_sdk::native_capabilities();
#if AWJ_SDK_D3D11
  result |= AWJ_CAP_D3D11;
#endif
#if AWJ_SDK_SHM
  result |= AWJ_CAP_SHARED_MEMORY;
#endif
#if AWJ_SDK_FFMPEG
  result |= AWJ_CAP_FFMPEG;
#endif
  return result;
}
void AWJ_CALL awj_context_options_init(awj_context_options* options) {
  if (!options) return;
  *options = {sizeof(*options), AWJ_ABI_VERSION, gib / 2, gib, 1, 0};
}
void AWJ_CALL awj_frame_desc_init(awj_frame_desc* desc) {
  if (!desc) return;
  *desc = {};
  desc->struct_size = sizeof(*desc);
  desc->abi_version = AWJ_ABI_VERSION;
  desc->format = AWJ_RGBA8;
  desc->bit_depth = 8;
  desc->plane_count = 1;
  desc->color = {-1, -1, -1, -1, -1, AWJ_ALPHA_NONE, 0, 0, nullptr, 0};
  desc->timestamp = AWJ_TIMESTAMP_UNKNOWN;
  desc->time_base_num = 1;
  desc->time_base_den = 1000000000;
}
void AWJ_CALL awj_encode_options_init(awj_encode_options* options) {
  if (!options) return;
  *options = {sizeof(*options), AWJ_ABI_VERSION, AWJ_CODEC_AVIF, AWJ_COPY_ALLOW_REQUIRED,
              80, 6, 0, 0, 0, 0};
}
awj_status AWJ_CALL awj_context_create(const awj_context_options* options, awj_context** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少 context 输出。");
    *output = nullptr;
    awj_context_options defaults{};
    if (!options) { awj_context_options_init(&defaults); options = &defaults; }
    if (!awj_sdk::valid_header(options) || options->reserved || !options->max_frame_bytes ||
        options->max_frame_bytes > options->memory_budget_bytes || options->memory_budget_bytes > 64 * gib ||
        !options->max_threads || options->max_threads > 1024)
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "context ABI、内存预算或线程上限无效。");
    auto context = std::make_unique<awj_context>();
    context->options = *options;
    *output = context.release();
    return AWJ_OK;
  });
}
void AWJ_CALL awj_context_destroy(awj_context* context) { delete context; }
void AWJ_CALL awj_context_cancel(awj_context* context) { if (context) context->stop.request_stop(); }
awj_status AWJ_CALL awj_frame_validate(awj_context* context, const awj_frame_desc* desc) {
  return awj_sdk::boundary([&] {
    if (!context || !desc) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少 context 或帧描述。");
    return awj_sdk::validate(context->options, *desc);
  });
}
awj_status AWJ_CALL awj_frame_layout(const awj_frame_desc* desc, awj_frame_layout_info* output) {
  return awj_sdk::boundary([&] {
    if (!awj_sdk::valid_header(desc) || !awj_sdk::valid_header(output))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "平面布局查询 ABI 无效。");
    awj_sdk::Layout shape{};
    const auto status = awj_sdk::layout(*desc, shape);
    if (status != AWJ_OK) return status;
    awj_frame_layout_info result{sizeof(result), AWJ_ABI_VERSION, shape.count, 0, {}, {}, 0};
    for (uint32_t p = 0; p < shape.count; ++p) {
      uint64_t bytes{};
      if (!awj_sdk::multiply(shape.row_bytes[p], shape.rows[p], bytes) || result.pixel_bytes > UINT64_MAX - bytes)
        return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "紧凑帧布局长度溢出。");
      result.row_bytes[p] = shape.row_bytes[p]; result.rows[p] = shape.rows[p]; result.pixel_bytes += bytes;
    }
    *output = result;
    return AWJ_OK;
  });
}
awj_status AWJ_CALL awj_frame_diagnostics(const awj_frame* frame, awj_diagnostics* output) {
  return awj_sdk::boundary([&] {
    if (!frame || !awj_sdk::valid_header(output)) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "帧诊断输出 ABI 无效。");
    *output = frame->diagnostics;
    return AWJ_OK;
  });
}
awj_status AWJ_CALL awj_frame_create(awj_context* context, const awj_frame_desc* desc,
                                   const awj_owner* owner, awj_frame** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少帧输出。");
    *output = nullptr;
    if (!context || !desc) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少 context 或帧描述。");
    if (context->stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "context 已取消。");
    const auto status = awj_sdk::validate(context->options, *desc);
    if (status != AWJ_OK) return status;
    if (owner && (!!owner->retain != !!owner->release))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "retain/release 必须同时提供。");
    auto frame = std::make_unique<awj_frame>();
    frame->desc = *desc;
    if (owner && owner->retain) {
      frame->owner = *owner;
      owner->retain(owner->user);
      frame->retained = true;
    }
    *output = frame.release();
    return AWJ_OK;
  });
}
void AWJ_CALL awj_frame_retain(awj_frame* frame) { if (frame) frame->references.fetch_add(1); }
void AWJ_CALL awj_frame_release(awj_frame* frame) {
  if (frame && frame->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete frame;
}
awj_status AWJ_CALL awj_frame_describe(const awj_frame* frame, awj_frame_desc* output) {
  return awj_sdk::boundary([&] {
    if (!frame || !awj_sdk::valid_header(output)) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "无效帧或输出 ABI。");
    *output = frame->desc;
    return AWJ_OK;
  });
}
awj_status AWJ_CALL awj_frame_row(const awj_frame* frame, uint32_t plane, uint32_t index,
                                const uint8_t** data, uint64_t* size) {
  return awj_sdk::boundary([&] {
    if (!frame || !data || !size) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少帧或行输出。");
    *data = nullptr;
    *size = 0;
    awj_sdk::Layout shape{};
    const auto status = awj_sdk::layout(frame->desc, shape);
    if (status != AWJ_OK) return status;
    if (plane >= shape.count || index >= shape.rows[plane])
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "帧行索引越界。");
    *data = awj_sdk::row(frame->desc.planes[plane], index);
    *size = shape.row_bytes[plane];
    return AWJ_OK;
  });
}
awj_status AWJ_CALL awj_encode(awj_context* context, const awj_frame* frame,
                             const awj_encode_options* options, awj_result** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少编码输出。");
    *output = nullptr;
    if (!context || !frame || !awj_sdk::valid_header(options) || options->reserved ||
        options->copy_policy > AWJ_COPY_REQUIRE_BORROWED || options->quality < 0 || options->quality > 100 ||
        options->speed < 0 || options->speed > 10 || options->lossless > 1 || options->visual_quality > 1 ||
        options->codec < AWJ_CODEC_AVIF || options->codec > AWJ_CODEC_PNG ||
        (options->bit_depth && options->bit_depth != 8 && options->bit_depth != 10 && options->bit_depth != 12 && options->bit_depth != 16))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "编码选项或 ABI 无效。");
    if (context->stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "context 已取消。");
    const auto status = awj_sdk::validate(context->options, frame->desc);
    if (status != AWJ_OK) return status;
    auto result = std::make_unique<awj_result>();
    result->diagnostics = frame->diagnostics;
    const auto encoded = awj_sdk::native_encode(*context, *frame, *options, *result);
    if (encoded != AWJ_OK) return encoded;
    if (context->stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "编码结果已取消。");
    *output = result.release();
    return AWJ_OK;
  });
}
void AWJ_CALL awj_result_release(awj_result* result) { delete result; }
const uint8_t* AWJ_CALL awj_result_data(const awj_result* result) { return result ? reinterpret_cast<const uint8_t*>(result->bytes.data()) : nullptr; }
uint64_t AWJ_CALL awj_result_size(const awj_result* result) { return result ? result->bytes.size() : 0; }
awj_status AWJ_CALL awj_result_diagnostics(const awj_result* result, awj_diagnostics* output) {
  return awj_sdk::boundary([&] {
    if (!result || !awj_sdk::valid_header(output)) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "结果或诊断 ABI 无效。");
    *output = result->diagnostics;
    return AWJ_OK;
  });
}
awj_status AWJ_CALL awj_queue_create(uint32_t capacity, awj_queue** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少队列输出。");
    *output = nullptr;
    if (!capacity || capacity > 1024) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "队列容量应为 1..1024。");
    auto queue = std::make_unique<awj_queue>();
    queue->capacity = capacity;
    *output = queue.release();
    return AWJ_OK;
  });
}
awj_status AWJ_CALL awj_queue_push(awj_queue* queue, awj_frame* frame, uint32_t timeout_ms) {
  return awj_sdk::boundary([&] {
    if (!queue || !frame || timeout_ms > 60000 || (!frame->retained && !frame->storage))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "队列需要有所有者的帧和有效等待上限。");
    std::unique_lock lock{queue->mutex};
    const auto ready = [&] { return queue->closed || queue->frames.size() < queue->capacity; };
    if (!queue->changed.wait_for(lock, std::chrono::milliseconds{timeout_ms}, ready))
      return awj_sdk::fail(timeout_ms ? AWJ_TIMEOUT : AWJ_BUSY, "帧队列已满。");
    if (queue->closed) return awj_sdk::fail(AWJ_END_OF_STREAM, "帧队列已关闭。");
    queue->frames.push_back(frame);
    awj_frame_retain(frame);
    lock.unlock();
    queue->changed.notify_all();
    return AWJ_OK;
  });
}
awj_status AWJ_CALL awj_queue_pop(awj_queue* queue, awj_frame** output, uint32_t timeout_ms) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少出队帧输出。");
    *output = nullptr;
    if (!queue || timeout_ms > 60000) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "无效队列或等待上限。");
    std::unique_lock lock{queue->mutex};
    const auto ready = [&] { return queue->closed || !queue->frames.empty(); };
    if (!queue->changed.wait_for(lock, std::chrono::milliseconds{timeout_ms}, ready))
      return awj_sdk::fail(timeout_ms ? AWJ_TIMEOUT : AWJ_BUSY, "帧队列为空。");
    if (queue->frames.empty()) return AWJ_END_OF_STREAM;
    *output = queue->frames.front();
    queue->frames.pop_front();
    lock.unlock();
    queue->changed.notify_all();
    return AWJ_OK;
  });
}
void AWJ_CALL awj_queue_close(awj_queue* queue) {
  if (!queue) return;
  { std::lock_guard lock{queue->mutex}; queue->closed = true; }
  queue->changed.notify_all();
}
void AWJ_CALL awj_queue_destroy(awj_queue* queue) {
  if (!queue) return;
  for (auto* frame : queue->frames) awj_frame_release(frame);
  delete queue;
}
}
