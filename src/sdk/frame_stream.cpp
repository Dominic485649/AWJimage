#include "sdk_internal.h"
#include <algorithm>
#include <bit>

struct awj_stream_reader {
  awj_context_options options{};
  awj::compat::stop_token stop{};
  std::vector<std::shared_ptr<std::vector<uint8_t>>> slots{};
  bool failed{};
  bool ended{};
};

namespace {
constexpr size_t header_size = 160;
constexpr std::array<uint8_t, 8> magic{'A', 'W', 'J', 'F', 'R', 'M', '1', 0};
using Header = std::array<uint8_t, header_size>;
void put32(Header& header, size_t offset, uint32_t value) noexcept {
  for (size_t i = 0; i < 4; ++i) header[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}
void put64(Header& header, size_t offset, uint64_t value) noexcept {
  for (size_t i = 0; i < 8; ++i) header[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}
uint32_t get32(const Header& header, size_t offset) noexcept {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(header[offset + i]) << (i * 8);
  return value;
}
uint64_t get64(const Header& header, size_t offset) noexcept {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(header[offset + i]) << (i * 8);
  return value;
}
class Deadline {
 public:
  explicit Deadline(uint32_t milliseconds) : duration_{milliseconds},
      end_{std::chrono::steady_clock::now() + std::chrono::milliseconds{milliseconds}} {}
  bool remaining(uint32_t& value) const noexcept {
    if (!duration_) { value = 0; return true; }
    const auto now = std::chrono::steady_clock::now();
    if (now >= end_) return false;
    value = static_cast<uint32_t>(std::max<int64_t>(1,
        std::chrono::duration_cast<std::chrono::milliseconds>(end_ - now).count()));
    return true;
  }
 private:
  uint32_t duration_{};
  std::chrono::steady_clock::time_point end_{};
};

awj_status read_all(awj_read_callback callback, void* user, std::span<uint8_t> bytes,
                    Deadline& deadline, awj::compat::stop_token stop, bool header = false) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    if (stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "帧流读取已取消。");
    uint32_t remaining{};
    if (!deadline.remaining(remaining)) return awj_sdk::fail(AWJ_TIMEOUT, "帧流读取超时。");
    uint64_t transferred{};
    const auto status = callback(user, bytes.data() + offset, bytes.size() - offset, &transferred, remaining);
    if (status != AWJ_OK) return awj_sdk::fail(status, "帧流读取回调失败，必须关闭此流。");
    if (transferred > bytes.size() - offset) return awj_sdk::fail(AWJ_IO_ERROR, "读取回调返回了越界长度。");
    if (!transferred) return header && offset == 0 ? AWJ_END_OF_STREAM
        : awj_sdk::fail(AWJ_IO_ERROR, "帧流在帧中间截断。");
    offset += static_cast<size_t>(transferred);
  }
  return AWJ_OK;
}
awj_status write_all(awj_write_callback callback, void* user, std::span<const uint8_t> bytes,
                     Deadline& deadline, awj::compat::stop_token stop) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    if (stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "帧流写入已取消。");
    uint32_t remaining{};
    if (!deadline.remaining(remaining)) return awj_sdk::fail(AWJ_TIMEOUT, "帧流写入超时。");
    uint64_t transferred{};
    const auto status = callback(user, bytes.data() + offset, bytes.size() - offset, &transferred, remaining);
    if (status != AWJ_OK) return awj_sdk::fail(status, "帧流写入回调失败，必须关闭此流。");
    if (!transferred || transferred > bytes.size() - offset)
      return awj_sdk::fail(AWJ_IO_ERROR, "写入回调没有前进或返回越界长度。");
    offset += static_cast<size_t>(transferred);
  }
  return AWJ_OK;
}

Header encode_header(const awj_frame_desc& desc, const awj_sdk::Layout& shape) {
  Header header{};
  std::copy(magic.begin(), magic.end(), header.begin());
  put32(header, 8, 1);
  put32(header, 12, header_size);
  put32(header, 16, desc.width); put32(header, 20, desc.height);
  put32(header, 24, desc.format); put32(header, 28, desc.bit_depth);
  put32(header, 32, desc.plane_count); put32(header, 36, desc.color.alpha);
  put64(header, 40, desc.frame_id); put64(header, 48, std::bit_cast<uint64_t>(desc.timestamp));
  put32(header, 56, desc.time_base_num); put32(header, 60, desc.time_base_den);
  put32(header, 64, std::bit_cast<uint32_t>(desc.color.primaries));
  put32(header, 68, std::bit_cast<uint32_t>(desc.color.transfer));
  put32(header, 72, std::bit_cast<uint32_t>(desc.color.matrix));
  put32(header, 76, std::bit_cast<uint32_t>(desc.color.range));
  put32(header, 80, std::bit_cast<uint32_t>(desc.color.chroma_location));
  put32(header, 84, std::bit_cast<uint32_t>(desc.color.linear_reference_white_nits));
  put64(header, 88, desc.color.icc_size);
  uint64_t payload = desc.color.icc_size;
  for (uint32_t p = 0; p < shape.count; ++p) {
    put64(header, 104 + p * 8, shape.row_bytes[p]);
    put32(header, 136 + p * 4, shape.rows[p]);
    payload += shape.row_bytes[p] * shape.rows[p];
  }
  put64(header, 96, payload);
  return header;
}

awj_status decode_header(const Header& header, awj_frame_desc& desc,
                          awj_sdk::Layout& shape, uint64_t& payload,
                          const awj_context_options& limits) {
  if (!std::equal(magic.begin(), magic.end(), header.begin()) || get32(header, 8) != 1 ||
      get32(header, 12) != header_size || get64(header, 152))
    return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "帧流 magic/version/header/reserved 不受支持。");
  awj_frame_desc_init(&desc);
  desc.width = get32(header, 16); desc.height = get32(header, 20);
  desc.format = get32(header, 24); desc.bit_depth = get32(header, 28);
  desc.plane_count = get32(header, 32); desc.color.alpha = get32(header, 36);
  desc.frame_id = get64(header, 40); desc.timestamp = std::bit_cast<int64_t>(get64(header, 48));
  desc.time_base_num = get32(header, 56); desc.time_base_den = get32(header, 60);
  desc.color.primaries = std::bit_cast<int32_t>(get32(header, 64));
  desc.color.transfer = std::bit_cast<int32_t>(get32(header, 68));
  desc.color.matrix = std::bit_cast<int32_t>(get32(header, 72));
  desc.color.range = std::bit_cast<int32_t>(get32(header, 76));
  desc.color.chroma_location = std::bit_cast<int32_t>(get32(header, 80));
  desc.color.linear_reference_white_nits = std::bit_cast<float>(get32(header, 84));
  desc.color.icc_size = get64(header, 88);
  payload = get64(header, 96);
  if (!payload || payload > limits.max_frame_bytes || payload > SIZE_MAX || desc.color.icc_size > 4 * 1024 * 1024 ||
      desc.color.icc_size > payload)
    return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "帧流 payload/ICC 超过预算。");
  const auto status = awj_sdk::layout(desc, shape);
  if (status != AWJ_OK) return status;
  if (desc.plane_count != shape.count) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "帧流平面数量无效。");
  uint64_t total = desc.color.icc_size;
  for (uint32_t p = 0; p < AWJ_MAX_PLANES; ++p) {
    if (get64(header, 104 + p * 8) != shape.row_bytes[p] || get32(header, 136 + p * 4) != shape.rows[p])
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "帧流布局与格式不符。");
    uint64_t bytes{};
    if (!awj_sdk::multiply(shape.row_bytes[p], shape.rows[p], bytes) || bytes > payload || total > payload - bytes)
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "帧流平面长度溢出。");
    total += bytes;
  }
  if (total != payload) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "帧流声明长度与布局不符。");
  return AWJ_OK;
}
}

namespace awj_sdk {
WireHeader encode_wire_header(const awj_frame_desc& desc, const Layout& shape) { return encode_header(desc, shape); }
awj_status decode_wire_header(const WireHeader& header, awj_frame_desc& desc,
                              Layout& shape, uint64_t& payload, const awj_context_options& limits) {
  return decode_header(header, desc, shape, payload, limits);
}
}

extern "C" {
awj_status AWJ_CALL awj_stream_write_frame(awj_context* context, const awj_frame* frame,
                                          awj_write_callback callback, void* user, uint32_t timeout_ms) {
  return awj_sdk::boundary([&] {
    if (!context || !frame || !callback || timeout_ms > 60000)
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少帧流参数或等待上限无效。");
    const auto checked = awj_sdk::validate(context->options, frame->desc);
    if (checked != AWJ_OK) return checked;
    awj_sdk::Layout shape{};
    const auto layout_status = awj_sdk::layout(frame->desc, shape);
    if (layout_status != AWJ_OK) return layout_status;
    const auto header = encode_header(frame->desc, shape);
    Deadline deadline{timeout_ms};
    const auto stop = context->stop.get_token();
    auto status = write_all(callback, user, header, deadline, stop);
    if (status != AWJ_OK) return status;
    for (uint32_t p = 0; p < shape.count; ++p) for (uint32_t y = 0; y < shape.rows[p]; ++y) {
      status = write_all(callback, user,
          {awj_sdk::row(frame->desc.planes[p], y), static_cast<size_t>(shape.row_bytes[p])}, deadline, stop);
      if (status != AWJ_OK) return status;
    }
    return write_all(callback, user, {frame->desc.color.icc, static_cast<size_t>(frame->desc.color.icc_size)}, deadline, stop);
  });
}
awj_status AWJ_CALL awj_stream_reader_create(awj_context* context, uint32_t slots, awj_stream_reader** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少 reader 输出。");
    *output = nullptr;
    if (!context || !slots || slots > 16 || context->options.max_frame_bytes > context->options.memory_budget_bytes / slots)
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "接收槽位或总内存预算无效。");
    auto reader = std::make_unique<awj_stream_reader>();
    reader->options = context->options;
    reader->stop = context->stop.get_token();
    for (uint32_t i = 0; i < slots; ++i) reader->slots.push_back(std::make_shared<std::vector<uint8_t>>());
    *output = reader.release();
    return AWJ_OK;
  });
}
awj_status AWJ_CALL awj_stream_read_frame(awj_stream_reader* reader, awj_read_callback callback,
                                         void* user, uint32_t timeout_ms, awj_frame** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少接收帧输出。");
    *output = nullptr;
    if (!reader || !callback || timeout_ms > 60000 || reader->failed)
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "无效 reader、回调或已损坏的帧流。");
    if (reader->ended) return AWJ_END_OF_STREAM;
    std::shared_ptr<std::vector<uint8_t>> buffer;
    for (auto& slot : reader->slots) if (slot.use_count() == 1) { buffer = slot; break; }
    if (!buffer) return awj_sdk::fail(AWJ_BUSY, "所有接收槽位仍在使用，必须先归还帧。");
    // 从读取第一个字节起，任何异常/失败都不能重新解释为下一个帧头。
    reader->failed = true;
    Deadline deadline{timeout_ms};
    Header header{};
    auto status = read_all(callback, user, header, deadline, reader->stop, true);
    if (status == AWJ_END_OF_STREAM) { reader->ended = true; reader->failed = false; return status; }
    if (status != AWJ_OK) return status;
    awj_frame_desc desc{};
    awj_sdk::Layout shape{};
    uint64_t payload{};
    status = decode_header(header, desc, shape, payload, reader->options);
    if (status != AWJ_OK) return status;
    buffer->clear();
    buffer->resize(static_cast<size_t>(payload));
    status = read_all(callback, user, *buffer, deadline, reader->stop);
    if (status != AWJ_OK) return status;
    uint64_t offset = 0;
    for (uint32_t p = 0; p < shape.count; ++p) {
      const auto bytes = shape.row_bytes[p] * shape.rows[p];
      desc.planes[p] = {buffer->data(), payload, offset, static_cast<int64_t>(shape.row_bytes[p])};
      offset += bytes;
    }
    desc.color.icc = desc.color.icc_size ? buffer->data() + static_cast<size_t>(offset) : nullptr;
    status = awj_sdk::validate(reader->options, desc);
    if (status != AWJ_OK) return status;
    auto frame = std::make_unique<awj_frame>();
    frame->desc = desc;
    frame->storage = std::move(buffer);
    frame->diagnostics.input_copy_bytes = payload;
    *output = frame.release();
    reader->failed = false;
    return AWJ_OK;
  });
}
void AWJ_CALL awj_stream_reader_destroy(awj_stream_reader* reader) { delete reader; }
}
