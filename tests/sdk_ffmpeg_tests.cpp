#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "awj/awj_ffmpeg.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/buffer.h>
#include <libavutil/version.h>
}
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "第 %d 行 FFmpeg 测试失败: %s; %s\n", __LINE__, #value, awj_last_error()); return 1; } } while (false)
struct Module { int retained{}, released{}; };
void AWJ_CALL retain(void* user) { ++static_cast<Module*>(user)->retained; }
void AWJ_CALL release(void* user) { ++static_cast<Module*>(user)->released; }
uint32_t AWJ_CALL wrong_version() { return 1; }
AVBufferRef* AWJ_CALL failed_ref(const AVBufferRef*) { return nullptr; }

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  if (argc != 2) { std::puts("未提供来源 libavutil 运行库，本项未运行。"); return 77; }
  auto path = std::filesystem::absolute(std::filesystem::path{argv[1]}).make_preferred();
#ifdef _WIN32
  const auto library = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!library) std::fprintf(stderr, "加载来源 DLL 失败，Win32 错误 %lu。\n", GetLastError());
#define LOOKUP(name) GetProcAddress(library, name)
#else
  const auto library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library) std::fprintf(stderr, "加载来源共享库失败：%s\n", ::dlerror());
#define LOOKUP(name) ::dlsym(library, name)
#endif
  CHECK(library != nullptr);
#define LOAD(name) const auto name = reinterpret_cast<decltype(&::name)>(LOOKUP(#name)); CHECK(name != nullptr)
  LOAD(avutil_version); LOAD(av_buffer_ref); LOAD(av_buffer_unref); LOAD(av_buffer_get_ref_count);
  LOAD(av_frame_alloc); LOAD(av_frame_free); LOAD(av_frame_unref); LOAD(av_frame_get_buffer); LOAD(av_frame_new_side_data);
#undef LOAD
  CHECK(avutil_version() == awj_ffmpeg_expected_avutil_version());
  Module module;
  awj_ffmpeg_api api{sizeof(api), AWJ_ABI_VERSION, avutil_version, av_buffer_ref, av_buffer_unref, {&module, retain, release}};
  awj_context_options limits; awj_context_options_init(&limits);
  limits.max_frame_bytes = 1024 * 1024; limits.memory_budget_bytes = 8 * 1024 * 1024;
  awj_context* context{}; CHECK(awj_context_create(&limits, &context) == AWJ_OK);
  awj_ffmpeg_options options; awj_ffmpeg_options_init(&options); options.copy_policy = AWJ_COPY_REQUIRE_BORROWED;
  AVFrame* source = av_frame_alloc(); CHECK(source);
  source->format = AV_PIX_FMT_RGBA; source->width = 3; source->height = 2;
  source->color_primaries = AVCOL_PRI_BT709; source->color_trc = AVCOL_TRC_IEC61966_2_1;
  source->colorspace = AVCOL_SPC_RGB; source->color_range = AVCOL_RANGE_JPEG;
  source->alpha_mode = AVALPHA_MODE_STRAIGHT; source->pts = 4500; source->time_base = {1, 90000};
  CHECK(av_frame_get_buffer(source, 64) == 0);
  std::array<uint8_t, 24> pixels{};
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i + 7);
  std::memcpy(source->data[0], pixels.data(), 12);
  std::memcpy(source->data[0] + source->linesize[0], pixels.data() + 12, 12);
  const auto* original = source->data[0];
  auto* icc = av_frame_new_side_data(source, AV_FRAME_DATA_ICC_PROFILE, 4); CHECK(icc);
  std::memcpy(icc->data, "test", 4);
  awj_frame* frame{};
  CHECK(awj_ffmpeg_import(context, source, &api, &options, &frame) == AWJ_OK);
  CHECK(av_buffer_get_ref_count(source->buf[0]) == 2);
  CHECK(module.retained == 1 && module.released == 0);
  awj_frame_desc desc; awj_frame_desc_init(&desc);
  CHECK(awj_frame_describe(frame, &desc) == AWJ_OK);
  CHECK(desc.planes[0].data + desc.planes[0].offset == original && desc.timestamp == 4500 && desc.time_base_den == 90000);
  CHECK(desc.color.alpha == AWJ_ALPHA_STRAIGHT && desc.color.icc != icc->data && desc.color.icc_size == 4);
  av_frame_unref(source);
  const uint8_t* row{}; uint64_t bytes{};
  CHECK(awj_frame_row(frame, 0, 1, &row, &bytes) == AWJ_OK && bytes == 12 && std::memcmp(row, pixels.data() + 12, 12) == 0);
  CHECK(std::memcmp(desc.color.icc, "test", 4) == 0);
  awj_diagnostics diagnostics{sizeof(diagnostics), AWJ_ABI_VERSION};
  CHECK(awj_frame_diagnostics(frame, &diagnostics) == AWJ_OK && diagnostics.input_copy_bytes == 0);
  awj_frame_release(frame); CHECK(module.released == 1);

  // 非引用计数的负 stride：只复制裁切后的有效行，不读取 padding。
  std::array<uint8_t, 32> padded{};
  std::memcpy(padded.data() + 16, pixels.data(), 12); std::memcpy(padded.data(), pixels.data() + 12, 12);
  source->format = AV_PIX_FMT_RGBA; source->width = 3; source->height = 2;
  source->data[0] = padded.data() + 16; source->linesize[0] = -16;
  CHECK(awj_ffmpeg_import(context, source, &api, &options, &frame) == AWJ_COPY_REQUIRED && !frame);
  options.copy_policy = AWJ_COPY_ALLOW_REQUIRED;
  CHECK(awj_ffmpeg_import(context, source, &api, &options, &frame) == AWJ_OK);
  CHECK(awj_frame_diagnostics(frame, &diagnostics) == AWJ_OK && diagnostics.input_copy_bytes == 24);
  padded.fill(0);
  CHECK(awj_frame_row(frame, 0, 0, &row, &bytes) == AWJ_OK && std::memcmp(row, pixels.data(), 12) == 0);
  CHECK(awj_frame_row(frame, 0, 1, &row, &bytes) == AWJ_OK && std::memcmp(row, pixels.data() + 12, 12) == 0);
  awj_frame_release(frame);
  source->data[0] = nullptr; av_frame_unref(source);

  // 真正引用计数的负 stride 及偶数左/上裁切，继续保持地址身份。
  source->format = AV_PIX_FMT_YUV420P; source->width = 6; source->height = 4;
  source->crop_left = 2; source->crop_top = 2;
  CHECK(av_frame_get_buffer(source, 64) == 0);
  source->data[0] += source->linesize[0] * 3; source->linesize[0] = -source->linesize[0];
  const auto* cropped = source->data[0] + 2 * source->linesize[0] + 2;
  options.copy_policy = AWJ_COPY_REQUIRE_BORROWED;
  CHECK(awj_ffmpeg_import(context, source, &api, &options, &frame) == AWJ_OK);
  CHECK(awj_frame_row(frame, 0, 0, &row, &bytes) == AWJ_OK && row == cropped && bytes == 4);
  CHECK(awj_frame_describe(frame, &desc) == AWJ_OK && desc.width == 4 && desc.height == 2 && desc.planes[0].stride < 0);
  awj_frame_release(frame);
  source->crop_left = 1;
  CHECK(awj_ffmpeg_import(context, source, &api, &options, &frame) == AWJ_UNSUPPORTED && !frame);
  source->crop_left = 2;
  auto broken = api; broken.avutil_version = wrong_version;
  CHECK(awj_ffmpeg_import(context, source, &broken, &options, &frame) == AWJ_UNSUPPORTED && !frame);
  broken = api; broken.buffer_ref = failed_ref;
  CHECK(awj_ffmpeg_import(context, source, &broken, &options, &frame) == AWJ_OUT_OF_MEMORY && !frame);
  CHECK(av_buffer_get_ref_count(source->buf[0]) == 1);
  source->pts = 1; source->time_base = {0, 1};
  CHECK(awj_ffmpeg_import(context, source, &api, &options, &frame) == AWJ_INVALID_ARGUMENT && !frame);
  options.time_base_num = 1; options.time_base_den = 1000;
  CHECK(awj_ffmpeg_import(context, source, &api, &options, &frame) == AWJ_OK);
  av_frame_free(&source);
  awj_context_destroy(context);
  CHECK(awj_frame_row(frame, 0, 0, &row, &bytes) == AWJ_OK);
  awj_frame_release(frame);
  CHECK(module.retained == module.released);
#ifdef _WIN32
  FreeLibrary(library);
#else
  CHECK(::dlclose(library) == 0);
#endif
  std::puts("真实 libavutil C ABI：像素缓冲借用、负 stride、裁切、ICC、来源释放、版本/错误边界通过。未验证真实 FFmpeg D3D11 解码会话。");
  return 0;
}
