#ifndef AWJ_SDK_H
#define AWJ_SDK_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define AWJ_CALL __cdecl
#  if defined(AWJ_SDK_STATIC)
#    define AWJ_API
#  elif defined(AWJ_SDK_BUILD)
#    define AWJ_API __declspec(dllexport)
#  else
#    define AWJ_API __declspec(dllimport)
#  endif
#else
#  define AWJ_CALL
#  define AWJ_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AWJ_ABI_VERSION 1u
#define AWJ_MAX_PLANES 4u
#define AWJ_COLOR_UNSPECIFIED (-1)
#define AWJ_TIMESTAMP_UNKNOWN INT64_MIN

typedef uint32_t awj_status;
#define AWJ_OK 0u
#define AWJ_INVALID_ARGUMENT 1u
#define AWJ_UNSUPPORTED 2u
#define AWJ_OUT_OF_MEMORY 3u
#define AWJ_CANCELLED 4u
#define AWJ_LIMIT_EXCEEDED 5u
#define AWJ_IO_ERROR 6u
#define AWJ_END_OF_STREAM 7u
#define AWJ_BUSY 8u
#define AWJ_TIMEOUT 9u
#define AWJ_INTERNAL_ERROR 10u
#define AWJ_COPY_REQUIRED 11u

typedef uint32_t awj_pixel_format;
#define AWJ_RGBA8 1u
#define AWJ_BGRA8 2u
#define AWJ_RGB8 3u
#define AWJ_GRAY8 4u
#define AWJ_RGBA16 5u
#define AWJ_GRAY16 6u
#define AWJ_RGBA16F 7u
#define AWJ_YUV420P 8u
#define AWJ_YUV422P 9u
#define AWJ_YUV444P 10u
#define AWJ_NV12 11u
#define AWJ_P010 12u

#define AWJ_ALPHA_NONE 0u
#define AWJ_ALPHA_STRAIGHT 1u
#define AWJ_ALPHA_PREMULTIPLIED 2u
#define AWJ_COPY_ALLOW_REQUIRED 0u
#define AWJ_COPY_REQUIRE_BORROWED 1u

#define AWJ_CODEC_AVIF 1u
#define AWJ_CODEC_WEBP 2u
#define AWJ_CODEC_JXL 3u
#define AWJ_CODEC_JPEGLI 4u
#define AWJ_CODEC_PNG 5u

#define AWJ_CAP_CPU_FRAMES (UINT64_C(1) << 0)
#define AWJ_CAP_NATIVE_CODECS (UINT64_C(1) << 1)
#define AWJ_CAP_D3D11 (UINT64_C(1) << 2)
#define AWJ_CAP_SHARED_MEMORY (UINT64_C(1) << 3)
#define AWJ_CAP_FRAME_STREAM (UINT64_C(1) << 4)
#define AWJ_CAP_FFMPEG (UINT64_C(1) << 5)

/* data 是分配区起点；offset 是第一行位置。负 stride 的首行可位于分配区末端。
 * size 覆盖所有被访问的字节；SDK 不读取 padding。CPU 样本采用小端存储。
 * 调用方保证该分配区真实可读，SDK 只能验证描述范围，不能探测任意裸地址。 */
typedef struct awj_plane {
  const uint8_t* data;
  uint64_t size;
  uint64_t offset;
  int64_t stride;
} awj_plane;

typedef struct awj_color {
  int32_t primaries;
  int32_t transfer;
  int32_t matrix;
  int32_t range;
  int32_t chroma_location;
  uint32_t alpha;
  float linear_reference_white_nits;
  uint32_t reserved;
  const uint8_t* icc;
  uint64_t icc_size;
} awj_color;

typedef struct awj_frame_desc {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t width;
  uint32_t height;
  awj_pixel_format format;
  uint32_t bit_depth;
  uint32_t plane_count;
  uint32_t reserved;
  awj_plane planes[AWJ_MAX_PLANES];
  awj_color color;
  uint64_t frame_id;
  int64_t timestamp;
  uint32_t time_base_num;
  uint32_t time_base_den;
} awj_frame_desc;

typedef void (AWJ_CALL *awj_owner_callback)(void* user);
typedef struct awj_owner {
  void* user;
  awj_owner_callback retain;
  awj_owner_callback release;
} awj_owner;

typedef struct awj_context_options {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t max_frame_bytes;
  uint64_t memory_budget_bytes;
  uint32_t max_threads;
  uint32_t reserved;
} awj_context_options;

typedef struct awj_encode_options {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t codec;
  uint32_t copy_policy;
  int32_t quality;
  int32_t speed;
  uint32_t bit_depth;
  /* 请求编码器无损模式；JPEGli 不支持。格式/色彩转换本身仍可能有损。 */
  uint32_t lossless;
  uint32_t visual_quality;
  uint32_t reserved;
} awj_encode_options;

typedef struct awj_diagnostics {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t input_copy_bytes;
  uint64_t conversion_bytes;
  uint64_t gpu_copy_bytes;
  uint64_t gpu_readback_bytes;
  /* 第三方编码器内部工作缓冲不受本库完整观测，0 表示未知而非零复制。 */
  uint32_t encoder_internal_copies_known;
  /* 为 0 时 conversion_bytes 只表示已观测的下界，不得解释为完整转换量。 */
  uint32_t conversion_bytes_known;
} awj_diagnostics;

typedef struct awj_frame_layout_info {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t plane_count;
  uint32_t reserved;
  uint64_t row_bytes[AWJ_MAX_PLANES];
  uint32_t rows[AWJ_MAX_PLANES];
  uint64_t pixel_bytes;
} awj_frame_layout_info;

typedef struct awj_context awj_context;
typedef struct awj_frame awj_frame;
typedef struct awj_result awj_result;
typedef struct awj_queue awj_queue;
typedef struct awj_stream_reader awj_stream_reader;

/* 初始化所有描述符，再覆盖需要的字段。所有 reserved 必须为 0。 */
AWJ_API uint32_t AWJ_CALL awj_abi_version(void);
AWJ_API const char* AWJ_CALL awj_version(void);
AWJ_API uint64_t AWJ_CALL awj_capabilities(void);
/* 当前线程的 UTF-8 错误，保存到本线程下一次可能失败的 SDK 调用之前。 */
AWJ_API const char* AWJ_CALL awj_last_error(void);
AWJ_API void AWJ_CALL awj_context_options_init(awj_context_options* options);
AWJ_API void AWJ_CALL awj_frame_desc_init(awj_frame_desc* desc);
AWJ_API void AWJ_CALL awj_encode_options_init(awj_encode_options* options);
AWJ_API awj_status AWJ_CALL awj_context_create(const awj_context_options* options, awj_context** output);
/* context 销毁前，调用方必须等待使用它的所有 API 调用结束。 */
AWJ_API void AWJ_CALL awj_context_destroy(awj_context* context);
/* 永久取消此 context；已借用的帧仍可安全释放。 */
AWJ_API void AWJ_CALL awj_context_cancel(awj_context* context);
AWJ_API awj_status AWJ_CALL awj_frame_validate(awj_context* context, const awj_frame_desc* desc);
/* 仅根据尺寸/格式查询紧凑平面布局，不访问像素；调用前设置输出 size/version。 */
AWJ_API awj_status AWJ_CALL awj_frame_layout(const awj_frame_desc* desc, awj_frame_layout_info* output);
AWJ_API awj_status AWJ_CALL awj_frame_diagnostics(const awj_frame* frame, awj_diagnostics* output);
/* 复制描述符，不复制像素。成功时 retain 一次，最后一个引用释放时 release 一次。
 * 两回调必须同时提供或同时省略；省略时由调用方保持像素/ICC 到 frame 最后释放。
 * 回调不可抛异常；release 可能在最后一个 frame_release 的线程执行，不持内部锁。 */
AWJ_API awj_status AWJ_CALL awj_frame_create(awj_context* context, const awj_frame_desc* desc,
                                           const awj_owner* owner, awj_frame** output);
AWJ_API void AWJ_CALL awj_frame_retain(awj_frame* frame);
AWJ_API void AWJ_CALL awj_frame_release(awj_frame* frame);
/* 输出描述符借用到 frame 最后释放；调用前设置 struct_size/abi_version。 */
AWJ_API awj_status AWJ_CALL awj_frame_describe(const awj_frame* frame, awj_frame_desc* output);
AWJ_API awj_status AWJ_CALL awj_frame_row(const awj_frame* frame, uint32_t plane, uint32_t row,
                                        const uint8_t** data, uint64_t* size);
/* 同步处理；输入不会写入/释放。每个结果由库拥有，以 result_release 释放。 */
AWJ_API awj_status AWJ_CALL awj_encode(awj_context* context, const awj_frame* frame,
                                     const awj_encode_options* options, awj_result** output);
AWJ_API void AWJ_CALL awj_result_release(awj_result* result);
AWJ_API const uint8_t* AWJ_CALL awj_result_data(const awj_result* result);
AWJ_API uint64_t AWJ_CALL awj_result_size(const awj_result* result);
AWJ_API awj_status AWJ_CALL awj_result_diagnostics(const awj_result* result, awj_diagnostics* output);

/* 有界进程内队列。成功 push 才增加 frame 引用；pop 将队列引用移交给调用方。
 * 队列仅接受带所有权回调的帧，避免无所有者裸指针跨线程。
 * timeout_ms=0 为非阻塞，最大 60000；close 后可 drain，关闭且空返回 END_OF_STREAM。
 * destroy 前须 join 所有队列调用；销毁会释放剩余帧，回调不在锁内执行。 */
AWJ_API awj_status AWJ_CALL awj_queue_create(uint32_t capacity, awj_queue** output);
AWJ_API awj_status AWJ_CALL awj_queue_push(awj_queue* queue, awj_frame* frame, uint32_t timeout_ms);
AWJ_API awj_status AWJ_CALL awj_queue_pop(awj_queue* queue, awj_frame** output, uint32_t timeout_ms);
AWJ_API void AWJ_CALL awj_queue_close(awj_queue* queue);
AWJ_API void AWJ_CALL awj_queue_destroy(awj_queue* queue);

/* I/O 回调必须在 timeout_ms 内返回，并以 transferred 表示短读/写。
 * read 的 OK+0 表示 EOF；write 的 OK+0 是错误。回调不得抛异常。
 * SDK 不能强制中断不遵守契约的第三方阻塞回调。 */
typedef awj_status (AWJ_CALL *awj_read_callback)(void* user, uint8_t* data, uint64_t capacity,
                                               uint64_t* transferred, uint32_t timeout_ms);
typedef awj_status (AWJ_CALL *awj_write_callback)(void* user, const uint8_t* data, uint64_t size,
                                                uint64_t* transferred, uint32_t timeout_ms);
AWJ_API awj_status AWJ_CALL awj_stream_write_frame(awj_context* context, const awj_frame* frame,
                                                 awj_write_callback write, void* user, uint32_t timeout_ms);
/* reader 最多保留 slots 个可复用接收缓冲区；全部借出时返回 BUSY，不无限分配。
 * reader 为单线程对象；销毁后已借出的 frame 仍有效。一次失败后不得继续读取该流。 */
AWJ_API awj_status AWJ_CALL awj_stream_reader_create(awj_context* context, uint32_t slots,
                                                   awj_stream_reader** output);
AWJ_API awj_status AWJ_CALL awj_stream_read_frame(awj_stream_reader* reader, awj_read_callback read,
                                                void* user, uint32_t timeout_ms, awj_frame** output);
AWJ_API void AWJ_CALL awj_stream_reader_destroy(awj_stream_reader* reader);

#ifdef __cplusplus
}
#endif
#endif
