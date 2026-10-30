#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <awj/awj.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "第 %d 行 YUV 只读页测试失败: %s; %s\n", __LINE__, #value, awj_last_error()); return 1; } } while (false)
int main() {
#ifdef _WIN32
  const size_t page = 4096;
  auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); CHECK(memory);
#else
  const auto page_size = ::sysconf(_SC_PAGESIZE); CHECK(page_size >= 72);
  const auto page = static_cast<size_t>(page_size);
  auto* memory = static_cast<uint8_t*>(::mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  CHECK(memory != MAP_FAILED);
#endif
  for (unsigned i = 0; i < 72; ++i) memory[i] = static_cast<uint8_t>(32 + i * 2);
  uint8_t saved[72]; std::memcpy(saved, memory, 72);
#ifdef _WIN32
  DWORD old{}; CHECK(VirtualProtect(memory, page, PAGE_READONLY, &old));
#else
  CHECK(::mprotect(memory, page, PROT_READ) == 0);
#endif
  awj_context* context{}; CHECK(awj_context_create(nullptr, &context) == AWJ_OK);
  for (unsigned codec : {AWJ_CODEC_AVIF, AWJ_CODEC_WEBP, AWJ_CODEC_JPEGLI}) {
    awj_frame_desc desc; awj_frame_desc_init(&desc);
    desc.width = 8; desc.height = 6; desc.format = AWJ_YUV420P; desc.plane_count = 3;
    desc.color.primaries = 1; desc.color.transfer = 13; desc.color.matrix = 6;
    desc.color.range = codec == AWJ_CODEC_WEBP ? 0 : 1; desc.color.chroma_location = 2;
    desc.planes[0] = {memory,72,0,8}; desc.planes[1] = {memory,72,48,4}; desc.planes[2] = {memory,72,60,4};
    awj_frame* frame{}; CHECK(awj_frame_create(context, &desc, nullptr, &frame) == AWJ_OK);
    awj_encode_options options; awj_encode_options_init(&options);
    options.codec = codec; options.quality = 80; options.speed = 9; options.copy_policy = AWJ_COPY_REQUIRE_BORROWED;
    awj_result* result{}; CHECK(awj_encode(context, frame, &options, &result) == AWJ_OK);
    awj_diagnostics diagnostics{sizeof(diagnostics), AWJ_ABI_VERSION};
    CHECK(awj_result_diagnostics(result, &diagnostics) == AWJ_OK && diagnostics.input_copy_bytes == 0);
    CHECK(awj_result_size(result) > 0 && std::memcmp(saved, memory, 72) == 0);
    std::printf("codec=%u：借用 YUV420 只读页编码通过，无 SDK 输入复制，来源保持不变。\n", codec);
    awj_result_release(result); awj_frame_release(frame);
  }
  awj_context_destroy(context);
#ifdef _WIN32
  CHECK(VirtualFree(memory, 0, MEM_RELEASE));
#else
  CHECK(::munmap(memory, page) == 0);
#endif
  return 0;
}
