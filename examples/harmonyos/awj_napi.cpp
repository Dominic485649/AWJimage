#include <napi/native_api.h>
#include <awj/awj.h>
#include <cmath>
#include <cstdio>
#include <new>

namespace {
struct Job {
  napi_async_work work{};
  napi_deferred promise{};
  napi_ref input{};
  void* pixels{};
  size_t size{};
  uint32_t width{}, height{};
  awj_result* result{};
  awj_status status{AWJ_OK};
  char error[1024]{};
};
void execute(napi_env, void* data) {
  auto& job = *static_cast<Job*>(data);
  awj_context* context{};
  job.status = awj_context_create(nullptr, &context);
  awj_frame* frame{};
  if (job.status == AWJ_OK) {
    awj_frame_desc desc; awj_frame_desc_init(&desc);
    desc.width = job.width; desc.height = job.height;
    desc.planes[0] = {static_cast<const uint8_t*>(job.pixels), job.size, 0, static_cast<int64_t>(job.width) * 4};
    desc.color.primaries = 1; desc.color.transfer = 13; desc.color.matrix = 0; desc.color.range = 1;
    desc.color.alpha = AWJ_ALPHA_STRAIGHT;
    // 原始 ArrayBuffer 的 napi_ref 由主线程保持；Promise 完成前调用方不得写入或 detach。
    job.status = awj_frame_create(context, &desc, nullptr, &frame);
    if (job.status == AWJ_OK) {
      awj_encode_options options; awj_encode_options_init(&options);
      options.codec = AWJ_CODEC_PNG; options.lossless = 1;
      job.status = awj_encode(context, frame, &options, &job.result);
    }
  }
  if (job.status != AWJ_OK) std::snprintf(job.error, sizeof(job.error), "%s", awj_last_error());
  awj_frame_release(frame); awj_context_destroy(context);
}
void finalize_result(napi_env, void*, void* hint) { awj_result_release(static_cast<awj_result*>(hint)); }
void reject(napi_env env, napi_deferred promise, const char* message) {
  napi_value text{}, error{};
  if (napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &text) == napi_ok &&
      napi_create_error(env, nullptr, text, &error) == napi_ok) napi_reject_deferred(env, promise, error);
}
void complete(napi_env env, napi_status status, void* data) {
  auto* job = static_cast<Job*>(data);
  napi_value output{};
  if (status == napi_ok && job->status == AWJ_OK &&
      napi_create_external_arraybuffer(env, const_cast<uint8_t*>(awj_result_data(job->result)),
          static_cast<size_t>(awj_result_size(job->result)), finalize_result, job->result, &output) == napi_ok) {
    job->result = nullptr; // 结果交给外部 ArrayBuffer 的 finalizer，无额外码流复制。
    napi_resolve_deferred(env, job->promise, output);
  } else reject(env, job->promise, job->error[0] ? job->error : "编码取消，或运行时不支持外部 ArrayBuffer。");
  awj_result_release(job->result);
  napi_delete_reference(env, job->input); napi_delete_async_work(env, job->work); delete job;
}
napi_value encode(napi_env env, napi_callback_info info) {
  size_t argc = 3; napi_value args[3]{};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc != 3) {
    napi_throw_type_error(env, nullptr, "encodeRgba(ArrayBuffer, width, height)"); return nullptr;
  }
  double width{}, height{}; bool is_arraybuffer{};
  if (napi_is_arraybuffer(env, args[0], &is_arraybuffer) != napi_ok || !is_arraybuffer ||
      napi_get_value_double(env, args[1], &width) != napi_ok || napi_get_value_double(env, args[2], &height) != napi_ok ||
      !std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 ||
      width > UINT32_MAX || height > UINT32_MAX || std::floor(width) != width || std::floor(height) != height) {
    napi_throw_type_error(env, nullptr, "需要 ArrayBuffer 和有效整数尺寸。"); return nullptr;
  }
  auto* job = new(std::nothrow) Job;
  if (!job) { napi_throw_error(env, nullptr, "内存不足。"); return nullptr; }
  job->width = static_cast<uint32_t>(width); job->height = static_cast<uint32_t>(height);
  napi_value promise{}, resource{};
  const auto pixels = static_cast<uint64_t>(job->width) * job->height;
  if (napi_get_arraybuffer_info(env, args[0], &job->pixels, &job->size) != napi_ok ||
      pixels > UINT64_C(512) * 1024 * 1024 / 4 || pixels * 4 != job->size) {
    delete job; napi_throw_range_error(env, nullptr, "ArrayBuffer 必须恰好是有界的紧凑 RGBA8。"); return nullptr;
  }
  if (napi_create_reference(env, args[0], 1, &job->input) != napi_ok ||
      napi_create_promise(env, &job->promise, &promise) != napi_ok ||
      napi_create_string_utf8(env, "AWJimage.encodeRgba", NAPI_AUTO_LENGTH, &resource) != napi_ok ||
      napi_create_async_work(env, nullptr, resource, execute, complete, job, &job->work) != napi_ok ||
      napi_queue_async_work(env, job->work) != napi_ok) {
    if (job->promise) reject(env, job->promise, "无法排队内存编码任务。");
    if (job->input) napi_delete_reference(env, job->input);
    if (job->work) napi_delete_async_work(env, job->work);
    const bool promised = job->promise != nullptr; delete job;
    if (promised) return promise;
    napi_throw_error(env, nullptr, "无法初始化内存编码任务。"); return nullptr;
  }
  return promise;
}
napi_value initialize(napi_env env, napi_value exports) {
  napi_property_descriptor method{"encodeRgba", nullptr, encode, nullptr, nullptr, nullptr, napi_default, nullptr};
  if (napi_define_properties(env, exports, 1, &method) != napi_ok) return nullptr;
  return exports;
}
napi_module module{1, 0, nullptr, initialize, "awj_napi", nullptr, {0}};
}
extern "C" __attribute__((constructor)) void register_awj_napi() { napi_module_register(&module); }
