#include "awj/awj.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {
#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "第 %d 行失败: %s; %s\n", __LINE__, #expression, awj_last_error()); return 1; } } while (false)
struct Owner { std::atomic<int> retained{}, released{}; };
void AWJ_CALL retain(void* user) { ++static_cast<Owner*>(user)->retained; }
void AWJ_CALL release(void* user) { ++static_cast<Owner*>(user)->released; }
struct MemoryStream { std::vector<uint8_t> bytes; size_t cursor{}; size_t fragment{3}; };
awj_status AWJ_CALL write(void* user, const uint8_t* data, uint64_t size, uint64_t* transferred, uint32_t) {
  auto& stream = *static_cast<MemoryStream*>(user);
  const auto count = std::min<size_t>(static_cast<size_t>(size), stream.fragment);
  stream.bytes.insert(stream.bytes.end(), data, data + count);
  *transferred = count;
  return AWJ_OK;
}
awj_status AWJ_CALL read(void* user, uint8_t* data, uint64_t capacity, uint64_t* transferred, uint32_t) {
  auto& stream = *static_cast<MemoryStream*>(user);
  const auto count = std::min({static_cast<size_t>(capacity), stream.fragment, stream.bytes.size() - stream.cursor});
  if (count) std::memcpy(data, stream.bytes.data() + stream.cursor, count);
  stream.cursor += count;
  *transferred = count;
  return AWJ_OK;
}
awj_status AWJ_CALL bad_write(void*, const uint8_t*, uint64_t size, uint64_t* transferred, uint32_t) {
  *transferred = size + 1;
  return AWJ_OK;
}
}

int main() {
  CHECK(awj_abi_version() == 1);
  CHECK(std::strcmp(awj_version(), "1.3.0") == 0);
  CHECK(awj_capabilities() & AWJ_CAP_CPU_FRAMES);
  awj_context_options options;
  awj_context_options_init(&options);
  options.max_frame_bytes = 1024;
  options.memory_budget_bytes = 4096;
  awj_context* context{};
  CHECK(awj_context_create(&options, &context) == AWJ_OK);
  std::unique_ptr<awj_context, decltype(&awj_context_destroy)> context_guard{context, awj_context_destroy};
  std::array<uint8_t, 24> pixels{};
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i);
  awj_frame_desc desc;
  awj_frame_desc_init(&desc);
  desc.width = 2; desc.height = 2;
  desc.planes[0] = {pixels.data(), pixels.size(), 12, -12};
  desc.color.primaries = 1; desc.color.transfer = 13; desc.color.matrix = 0; desc.color.range = 1;
  desc.timestamp = -42;
  desc.frame_id = 123;
  CHECK(awj_frame_validate(context, &desc) == AWJ_OK);
  Owner owner;
  awj_owner lifetime{&owner, retain, release};
  awj_frame* frame{};
  CHECK(awj_frame_create(context, &desc, &lifetime, &frame) == AWJ_OK);
  CHECK(owner.retained == 1 && owner.released == 0);
  const uint8_t* row{}; uint64_t row_size{};
  CHECK(awj_frame_row(frame, 0, 0, &row, &row_size) == AWJ_OK);
  CHECK(row == pixels.data() + 12 && row_size == 8);
  CHECK(awj_frame_row(frame, 0, 1, &row, &row_size) == AWJ_OK);
  CHECK(row == pixels.data());
  CHECK(awj_frame_row(frame, 0, 2, &row, &row_size) == AWJ_INVALID_ARGUMENT);
  awj_frame_desc described;
  awj_frame_desc_init(&described);
  CHECK(awj_frame_describe(frame, &described) == AWJ_OK && described.planes[0].data == pixels.data());
  auto invalid = desc;
  invalid.planes[0].offset = 1;
  CHECK(awj_frame_validate(context, &invalid) == AWJ_INVALID_ARGUMENT);
  invalid = desc; invalid.planes[0].stride = INT64_MIN;
  CHECK(awj_frame_validate(context, &invalid) == AWJ_INVALID_ARGUMENT);
  invalid = desc; invalid.width = UINT32_MAX;
  CHECK(awj_frame_validate(context, &invalid) != AWJ_OK);
  invalid = desc; invalid.struct_size = 1;
  CHECK(awj_frame_validate(context, &invalid) == AWJ_INVALID_ARGUMENT);
  invalid = desc; invalid.color.icc_size = UINT64_MAX;
  CHECK(awj_frame_validate(context, &invalid) == AWJ_INVALID_ARGUMENT);

  awj_queue* queue{};
  CHECK(awj_queue_create(1, &queue) == AWJ_OK);
  CHECK(awj_queue_push(queue, frame, 0) == AWJ_OK);
  CHECK(awj_queue_push(queue, frame, 0) == AWJ_BUSY);
  awj_frame* popped{};
  CHECK(awj_queue_pop(queue, &popped, 0) == AWJ_OK && popped == frame);
  awj_frame_release(popped);
  std::atomic<awj_status> pop_status{AWJ_INTERNAL_ERROR};
  std::thread waiter{[&] { awj_frame* value{}; pop_status = awj_queue_pop(queue, &value, 1000); }};
  awj_queue_close(queue);
  waiter.join();
  CHECK(pop_status == AWJ_END_OF_STREAM);
  CHECK(awj_queue_push(queue, frame, 0) == AWJ_END_OF_STREAM);
  awj_queue_destroy(queue);
  CHECK(owner.released == 0);

  MemoryStream stream;
  CHECK(awj_stream_write_frame(context, frame, write, &stream, 1000) == AWJ_OK);
  CHECK(stream.bytes.size() == 176);
  CHECK(awj_stream_write_frame(context, frame, write, &stream, 1000) == AWJ_OK);
  CHECK(awj_stream_write_frame(context, frame, write, &stream, 1000) == AWJ_OK);
  awj_stream_reader* reader{};
  CHECK(awj_stream_reader_create(context, 2, &reader) == AWJ_OK);
  awj_frame* first{}; awj_frame* second{}; awj_frame* third{};
  CHECK(awj_stream_read_frame(reader, read, &stream, 1000, &first) == AWJ_OK);
  CHECK(awj_frame_describe(first, &described) == AWJ_OK);
  CHECK(described.timestamp == -42 && described.frame_id == 123);
  CHECK(described.planes[0].stride == 8);
  CHECK(awj_frame_row(first, 0, 0, &row, &row_size) == AWJ_OK);
  CHECK(std::memcmp(row, pixels.data() + 12, 8) == 0);
  const auto* reused = row;
  CHECK(awj_stream_read_frame(reader, read, &stream, 1000, &second) == AWJ_OK);
  const auto cursor = stream.cursor;
  CHECK(awj_stream_read_frame(reader, read, &stream, 1000, &third) == AWJ_BUSY);
  CHECK(!third && stream.cursor == cursor);
  awj_frame_release(first);
  CHECK(awj_stream_read_frame(reader, read, &stream, 1000, &third) == AWJ_OK);
  CHECK(awj_frame_row(third, 0, 0, &row, &row_size) == AWJ_OK && row == reused);
  awj_frame_release(second);
  CHECK(awj_stream_read_frame(reader, read, &stream, 1000, &second) == AWJ_END_OF_STREAM);
  awj_stream_reader_destroy(reader);
  CHECK(awj_frame_row(third, 0, 1, &row, &row_size) == AWJ_OK && std::memcmp(row, pixels.data(), 8) == 0);
  awj_frame_release(third);
  CHECK(awj_stream_write_frame(context, frame, bad_write, nullptr, 1000) == AWJ_IO_ERROR);

  MemoryStream truncated = stream;
  truncated.cursor = 0; truncated.bytes.resize(165);
  CHECK(awj_stream_reader_create(context, 1, &reader) == AWJ_OK);
  CHECK(awj_stream_read_frame(reader, read, &truncated, 1000, &first) == AWJ_IO_ERROR);
  CHECK(awj_stream_read_frame(reader, read, &truncated, 1000, &first) == AWJ_INVALID_ARGUMENT);
  awj_stream_reader_destroy(reader);
  auto malformed = stream;
  malformed.cursor = 0; malformed.bytes[96] = 255;
  CHECK(awj_stream_reader_create(context, 1, &reader) == AWJ_OK);
  CHECK(awj_stream_read_frame(reader, read, &malformed, 1000, &first) != AWJ_OK);
  CHECK(malformed.cursor == 160);
  awj_stream_reader_destroy(reader);

  awj_frame_desc yuv;
  awj_frame_desc_init(&yuv);
  yuv.format = AWJ_NV12; yuv.width = 3; yuv.height = 3; yuv.plane_count = 2;
  std::array<uint8_t, 9> luma{}; std::array<uint8_t, 8> chroma{};
  yuv.planes[0] = {luma.data(), luma.size(), 0, 3};
  yuv.planes[1] = {chroma.data(), chroma.size(), 0, 4};
  CHECK(awj_frame_validate(context, &yuv) == AWJ_OK);
  yuv.planes[1].size = 7;
  CHECK(awj_frame_validate(context, &yuv) == AWJ_INVALID_ARGUMENT);

  awj_frame_release(frame);
  CHECK(owner.retained == 1 && owner.released == 1);
  awj_context_cancel(context);
  CHECK(awj_frame_create(context, &desc, &lifetime, &frame) == AWJ_CANCELLED && !frame);
  CHECK(owner.retained == 1 && owner.released == 1);
  std::puts("C ABI、内存借用、负 stride、有界队列和碎片帧流测试通过。");
  return 0;
}
