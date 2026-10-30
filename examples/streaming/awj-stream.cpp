#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "awj/awj.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <unistd.h>
#endif

namespace {
std::atomic_bool cancelled{};
#ifdef _WIN32
BOOL WINAPI interrupt(DWORD) { cancelled.store(true); return TRUE; }
#else
volatile std::sig_atomic_t interrupted{};
void interrupt(int) { interrupted = 1; }
#endif
bool stopped() {
#ifndef _WIN32
  if (interrupted) cancelled.store(true);
#endif
  return cancelled.load();
}
using Clock = std::chrono::steady_clock;
awj_status AWJ_CALL read_input(void*, uint8_t* data, uint64_t size, uint64_t* transferred, uint32_t timeout) {
  *transferred = 0;
  const auto end = Clock::now() + std::chrono::milliseconds{timeout};
  while (!stopped()) {
#ifdef _WIN32
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto type = GetFileType(input);
    DWORD available = static_cast<DWORD>(std::min<uint64_t>(size, 65536));
    if (type == FILE_TYPE_PIPE) {
      if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE ? AWJ_OK : AWJ_IO_ERROR;
      if (!available) {
        if (Clock::now() >= end) return AWJ_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds{2}); continue;
      }
      available = static_cast<DWORD>(std::min<uint64_t>(size, std::min<DWORD>(available, 65536)));
    } else if (type != FILE_TYPE_DISK) return AWJ_INVALID_ARGUMENT;
    DWORD received{};
    if (!ReadFile(input, data, available, &received, nullptr))
      return GetLastError() == ERROR_BROKEN_PIPE ? AWJ_OK : AWJ_IO_ERROR;
    *transferred = received; return AWJ_OK;
#else
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    const auto ready = poll(&descriptor, 1, 25);
    if (ready < 0 && errno != EINTR) return AWJ_IO_ERROR;
    if (ready > 0) {
      const auto count = ::read(STDIN_FILENO, data, static_cast<size_t>(std::min<uint64_t>(size, 65536)));
      if (count >= 0) { *transferred = static_cast<uint64_t>(count); return AWJ_OK; }
      if (errno != EINTR) return AWJ_IO_ERROR;
    }
    if (Clock::now() >= end) return AWJ_TIMEOUT;
#endif
  }
  return AWJ_CANCELLED;
}
bool write_output(const uint8_t* bytes, uint64_t size) {
  while (size && !stopped()) {
    const auto count = static_cast<size_t>(std::min<uint64_t>(size, 65536));
    const auto written = std::fwrite(bytes, 1, count, stdout);
    if (!written) return false;
    bytes += written; size -= written;
  }
  return !size;
}
void put32(uint8_t* p, uint32_t value) { for (size_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8)); }
void put64(uint8_t* p, uint64_t value) { for (size_t i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8)); }
template<class T> bool number(const char* text, T& output) {
  const auto end = text + std::strlen(text); const auto parsed = std::from_chars(text, end, output);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}
bool pixel_format(std::string_view name, awj_frame_desc& desc) {
  struct Format { std::string_view name; uint32_t format, depth; };
  constexpr Format formats[]{
    {"rgba8", AWJ_RGBA8, 8}, {"bgra8", AWJ_BGRA8, 8}, {"rgb8", AWJ_RGB8, 8},
    {"gray8", AWJ_GRAY8, 8}, {"gray16le", AWJ_GRAY16, 16}, {"rgba16le", AWJ_RGBA16, 16},
    {"rgba16f", AWJ_RGBA16F, 16}, {"yuv420p", AWJ_YUV420P, 8}, {"yuv422p", AWJ_YUV422P, 8},
    {"yuv444p", AWJ_YUV444P, 8}, {"yuv420p10le", AWJ_YUV420P, 10}, {"yuv420p12le", AWJ_YUV420P, 12},
    {"yuv444p10le", AWJ_YUV444P, 10}, {"yuv444p12le", AWJ_YUV444P, 12}, {"nv12", AWJ_NV12, 8}, {"p010le", AWJ_P010, 10}};
  for (const auto& value : formats) if (value.name == name) { desc.format = value.format; desc.bit_depth = value.depth; return true; }
  return false;
}
void usage() {
  std::fputs("awj-stream [--wire | --raw <format> <width> <height> --color <primaries> <transfer> <matrix> <range>]\n"
      "  --codec avif|webp|jxl|jpegli|png --quality 0..100 --speed 0..10 --lossless --visual-quality\n"
      "  --alpha none|straight|premultiplied --chroma-location 1..6 --reference-white <nits> --single\n"
      "输入/输出均为二进制 stdin/stdout；默认 AWJFRM1 多帧输入和 AWJRES1 多帧结果。\n"
      "--single 仅用于单帧输入，输出原编码码流；FFmpeg rawvideo 的色彩/布局须显式提供。\n"
      "普通管道存在接收复制；不会创建输入快照、中间文件、候选文件或文件日志。\n", stderr);
}
}

int main(int argc, char** argv) {
  awj_encode_options options; awj_encode_options_init(&options);
  awj_frame_desc raw; awj_frame_desc_init(&raw);
  bool raw_input = false, single = false, has_color = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--help") { usage(); return 0; }
    if (arg == "--wire") raw_input = false;
    else if (arg == "--single") single = true;
    else if (arg == "--lossless") options.lossless = 1;
    else if (arg == "--visual-quality") options.visual_quality = 1;
    else if (arg == "--raw" && i + 3 < argc) {
      raw_input = true;
      if (!pixel_format(argv[++i], raw) || !number(argv[++i], raw.width) || !number(argv[++i], raw.height)) { usage(); return 2; }
    } else if (arg == "--color" && i + 4 < argc) {
      has_color = number(argv[++i], raw.color.primaries) && number(argv[++i], raw.color.transfer) &&
          number(argv[++i], raw.color.matrix) && number(argv[++i], raw.color.range);
      if (!has_color) return 2;
    } else if (arg == "--codec" && i + 1 < argc) {
      const std::string_view codec{argv[++i]};
      options.codec = codec == "avif" ? AWJ_CODEC_AVIF : codec == "webp" ? AWJ_CODEC_WEBP : codec == "jxl" ? AWJ_CODEC_JXL : codec == "jpegli" ? AWJ_CODEC_JPEGLI : codec == "png" ? AWJ_CODEC_PNG : 0;
      if (!options.codec) return 2;
    } else if ((arg == "--quality" || arg == "--speed") && i + 1 < argc) {
      if (!number(argv[++i], arg == "--quality" ? options.quality : options.speed)) return 2;
    } else if (arg == "--chroma-location" && i + 1 < argc) {
      if (!number(argv[++i], raw.color.chroma_location)) return 2;
    } else if (arg == "--reference-white" && i + 1 < argc) {
      if (!number(argv[++i], raw.color.linear_reference_white_nits) || !std::isfinite(raw.color.linear_reference_white_nits)) return 2;
    } else if (arg == "--alpha" && i + 1 < argc) {
      const std::string_view alpha{argv[++i]};
      raw.color.alpha = alpha == "none" ? AWJ_ALPHA_NONE : alpha == "straight" ? AWJ_ALPHA_STRAIGHT : alpha == "premultiplied" ? AWJ_ALPHA_PREMULTIPLIED : UINT32_MAX;
      if (raw.color.alpha == UINT32_MAX) return 2;
    } else { usage(); return 2; }
  }
  if (raw_input && !has_color) { std::fputs("rawvideo 必须显式提供 --color。\n", stderr); return 2; }
#ifdef _WIN32
  if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) return 2;
  SetConsoleCtrlHandler(interrupt, TRUE);
#else
  std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt); std::signal(SIGPIPE, SIG_IGN);
#endif
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  awj_context* context_raw{};
  if (awj_context_create(nullptr, &context_raw) != AWJ_OK) return 1;
  std::unique_ptr<awj_context, decltype(&awj_context_destroy)> context{context_raw, awj_context_destroy};
  std::jthread cancellation{[value = context.get()](std::stop_token token) {
    while (!token.stop_requested()) {
      if (stopped()) { awj_context_cancel(value); return; }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
  }};
  awj_stream_reader* reader_raw{};
  if (!raw_input && awj_stream_reader_create(context.get(), 1, &reader_raw) != AWJ_OK) return 1;
  std::unique_ptr<awj_stream_reader, decltype(&awj_stream_reader_destroy)> reader{reader_raw, awj_stream_reader_destroy};
  awj_frame_layout_info shape{sizeof(shape), AWJ_ABI_VERSION};
  std::vector<uint8_t> storage;
  if (raw_input) {
    if (awj_frame_layout(&raw, &shape) != AWJ_OK || shape.pixel_bytes > UINT64_C(512) * 1024 * 1024) return 2;
    storage.resize(static_cast<size_t>(shape.pixel_bytes)); raw.plane_count = shape.plane_count;
    uint64_t offset{};
    for (uint32_t p = 0; p < shape.plane_count; ++p) {
      raw.planes[p] = {storage.data(), storage.size(), offset, static_cast<int64_t>(shape.row_bytes[p])};
      offset += shape.row_bytes[p] * shape.rows[p];
    }
    if (awj_frame_validate(context.get(), &raw) != AWJ_OK) return 2;
  }
  uint64_t frames{};
  for (;;) {
    awj_frame* frame_raw{};
    awj_status status = AWJ_OK;
    if (raw_input) {
      uint64_t offset{}; const auto deadline = Clock::now() + std::chrono::seconds{60};
      while (offset < storage.size()) {
        uint64_t received{};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) { status = AWJ_TIMEOUT; break; }
        status = read_input(nullptr, storage.data() + offset, storage.size() - offset, &received, static_cast<uint32_t>(remaining));
        if (status != AWJ_OK || !received) { if (status == AWJ_OK) status = offset ? AWJ_IO_ERROR : AWJ_END_OF_STREAM; break; }
        offset += received;
      }
      raw.frame_id = frames;
      if (status == AWJ_OK) status = awj_frame_create(context.get(), &raw, nullptr, &frame_raw);
    } else status = awj_stream_read_frame(reader.get(), read_input, nullptr, 60000, &frame_raw);
    if (status == AWJ_END_OF_STREAM) break;
    std::unique_ptr<awj_frame, decltype(&awj_frame_release)> frame{frame_raw, awj_frame_release};
    if (status != AWJ_OK || (single && frames)) { std::fprintf(stderr, "输入失败，status=%u（或 --single 收到多帧）：%s\n", status, awj_last_error()); return 1; }
    awj_frame_desc desc; awj_frame_desc_init(&desc); if (awj_frame_describe(frame.get(), &desc) != AWJ_OK) return 1;
    awj_result* result_raw{};
    status = awj_encode(context.get(), frame.get(), &options, &result_raw);
    if (status != AWJ_OK) { std::fprintf(stderr, "编码失败，status=%u：%s\n", status, awj_last_error()); return 1; }
    std::unique_ptr<awj_result, decltype(&awj_result_release)> result{result_raw, awj_result_release};
    if (!single) {
      std::array<uint8_t, 40> header{'A','W','J','R','E','S','1',0};
      put32(header.data() + 8, 1); put32(header.data() + 12, 40); put64(header.data() + 16, desc.frame_id);
      put32(header.data() + 24, options.codec); put64(header.data() + 32, awj_result_size(result.get()));
      if (!write_output(header.data(), header.size())) return 1;
    }
    if (!write_output(awj_result_data(result.get()), awj_result_size(result.get()))) return 1;
    std::fprintf(stderr, "frame=%llu output=%llu bytes；普通管道存在接收复制。\n", static_cast<unsigned long long>(desc.frame_id), static_cast<unsigned long long>(awj_result_size(result.get())));
    ++frames;
  }
  if (!frames) { std::fputs("输入没有完整帧。\n", stderr); return 1; }
  return std::fflush(stdout) == 0 ? 0 : 1;
}
