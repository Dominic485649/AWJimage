#ifndef AWJ_SDK_D3D11_H
#define AWJ_SDK_D3D11_H
#include "awj.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AWJ_D3D11_SOURCE_READY 1u
#define AWJ_D3D11_CALLER_SERIALIZED 2u
#define AWJ_D3D11_SCRGB 4u
#define AWJ_D3D11_SYNC_EXTERNAL 0u
#define AWJ_D3D11_SYNC_KEYED_MUTEX 1u

typedef struct awj_d3d11_options {
  uint32_t struct_size;
  uint32_t abi_version;
  /* texture 是 ID3D11Texture2D* 的 opaque 值，不在公共头暴露 C++ COM 类型。
   * 或者提供 device(ID3D11Device*) + shared_nt_handle，二者不能同时选择。
   * 输入 COM 指针和共享 NT handle 在本次调用期间必须有效，handle 仍由调用方关闭。 */
  void* texture;
  void* device;
  void* shared_nt_handle;
  uint32_t subresource;
  uint32_t width;
  uint32_t height;
  uint32_t flags;
  uint32_t copy_policy;
  uint32_t sync_kind;
  uint32_t timeout_ms;
  uint32_t reserved;
  uint64_t acquire_key;
  uint64_t release_key;
  awj_color color;
  awj_owner owner;
} awj_d3d11_options;

AWJ_API void AWJ_CALL awj_d3d11_options_init(awj_d3d11_options* options);
/* 同步导入为 CPU 可处理的帧。普通 GPU 纹理需要 Copy+readback；严格模式拒绝。
 * 已可读的 staging texture 可以直接 Map 借用，借用期间禁止来源改写/复用。
 * SOURCE_READY 明确承诺来源 GPU 同步已完成；否则须指定 keyed mutex 同步。
 * SCRGB 明确声明 FP16 是 WGC/scRGB；只在源色彩全部未指定时补入 80-nit 语义。
 * device/context 须开启多线程保护，或 CALLER_SERIALIZED 保证所有 SDK/外部 D3D
 * 调用（包括最后一次 frame_release 的 Unmap）都被调用方串行化。拒绝 SINGLETHREADED device。
 * timeout 约束 keyed mutex 获取；D3D 驱动自身调用不能被 SDK 强制中断。
 * 提交 GPU copy 后，即使取消也会等 Map/驱动返回后再释放来源，不能提前归还 WGC 帧。
 * width/height 为左上角有效区域，0 表示完整 subresource；不读取 WGC 未定义边缘。
 * ICC 元数据会复制到帧拥有的存储，像素复制情况通过 result_diagnostics 报告。 */
AWJ_API awj_status AWJ_CALL awj_d3d11_import(awj_context* context, const awj_d3d11_options* options,
                                          awj_frame** output);
#ifdef __cplusplus
}
#endif
#endif
