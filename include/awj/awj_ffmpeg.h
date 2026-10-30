#ifndef AWJ_SDK_FFMPEG_H
#define AWJ_SDK_FFMPEG_H
#include "awj.h"
#ifdef __cplusplus
extern "C" {
#endif
struct AVFrame;
struct AVBufferRef;
#define AWJ_FFMPEG_ALPHA_FROM_SOURCE UINT32_MAX
#define AWJ_FFMPEG_D3D11_SOURCE_READY 1u
#define AWJ_FFMPEG_D3D11_CALLER_SERIALIZED 2u

typedef struct awj_ffmpeg_api {
  uint32_t struct_size;
  uint32_t abi_version;
  /* 必须来自同一个、与适配器头文件版本匹配的 libavutil。
   * 仅引用像素和硬件上下文的 AVBufferRef，不复制整个 AVFrame/字典或碰 private_ref。
   * 只保留 CICP、alpha、ICC、裁切与时间，不无界克隆未知 side data/字典。
   * SDK 不加载、不链接、不分发 FFmpeg 运行库。module_owner 或调用方须保持其加载。
   * 回调不得抛异常，module_owner 的 release 在最后一次 buffer_unref 后才执行。 */
  uint32_t (AWJ_CALL *avutil_version)(void);
  struct AVBufferRef* (AWJ_CALL *buffer_ref)(const struct AVBufferRef* buffer);
  void (AWJ_CALL *buffer_unref)(struct AVBufferRef** buffer);
  awj_owner module_owner;
} awj_ffmpeg_api;

typedef struct awj_ffmpeg_options {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t copy_policy;
  uint32_t flags;
  float linear_reference_white_nits;
  uint32_t alpha;
  uint64_t frame_id;
  /* 源 AVFrame 没有 time_base、但具有有效 pts 时必须显式提供。 */
  uint32_t time_base_num;
  uint32_t time_base_den;
} awj_ffmpeg_options;

AWJ_API void AWJ_CALL awj_ffmpeg_options_init(awj_ffmpeg_options* options);
AWJ_API uint32_t AWJ_CALL awj_ffmpeg_expected_avutil_version(void);
/* 借用带 AVBufferRef 的软件帧，不复制像素；非引用计数帧由 SDK 显式逐行复制，
 * 严格模式拒绝，兼容模式计入 input_copy_bytes。data/linesize、裁切与颜色均校验。
 * D3D11 硬件帧使用平台导入，其他尚未实现的硬件格式明确 UNSUPPORTED，不隐式下载。
 * 只接受真实有效、版本匹配的 AVFrame，不能把任意进程地址/字节当作 AVFrame。
 * 来源在同步调用期间保持不变；调用成功后可 av_frame_unref 原帧，但不能绕过
 * AVBufferRef 在引用仍存在时继续写同一像素存储；修改前须遵循 FFmpeg 的可写性契约。 */
AWJ_API awj_status AWJ_CALL awj_ffmpeg_import(awj_context* context, const struct AVFrame* source,
                                            const awj_ffmpeg_api* api, const awj_ffmpeg_options* options,
                                            awj_frame** output);
#ifdef __cplusplus
}
#endif
#endif
