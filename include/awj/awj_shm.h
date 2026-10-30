#ifndef AWJ_SDK_SHM_H
#define AWJ_SDK_SHM_H
#include "awj.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct awj_shm_channel awj_shm_channel;
typedef struct awj_shm_write {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t token;
  uint8_t* data;
  uint64_t capacity;
} awj_shm_write;

/* Windows/Linux 本机单生产者/单消费者通道；名称为 1..64 个 ASCII 字母、数字、- 或 _。
 * 建议使用不可预测的随机名称，仅向有意协作的同用户应用传递；不提供网络服务。
 * Windows 使用 page-file backing；Linux 使用预留 backing 的匿名 memfd、abstract Unix socket
 * 和 SCM_RIGHTS，不创建业务文件或命名共享内存节点；系统换页不在此保证范围。
 * Linux 要求 SO_PEERPIDFD、memfd seals 和 process-shared robust pthread mutex 支持；
 * 不支持时返回 UNSUPPORTED/IO_ERROR，不降级为磁盘文件。仅允许相同 effective UID。
 * create 为生产者，open 为消费者；每个通道只接受一次消费者连接。
 * channel 可由不同线程操作；destroy 前必须结束本 channel 的全部 API 调用。
 * Linux 通道绑定创建/open 的进程，不得在 fork 后使用继承的 channel/context；应 exec 后重建。
 * 已借出的读取帧可以晚于 channel/context 销毁，映射会延长到最后释放。
 * 对端异常退出将通道置为不可恢复错误；不重新分配仍被借用的槽位。
 * Windows 使用进程句柄及创建时间，Linux 使用内核提供的 pidfd，避免 PID 复用。 */
AWJ_API awj_status AWJ_CALL awj_shm_create(awj_context* context, const char* name,
                                         uint32_t slots, uint64_t capacity, awj_shm_channel** output);
AWJ_API awj_status AWJ_CALL awj_shm_open(awj_context* context, const char* name, awj_shm_channel** output);
/* begin 取得可写槽位；所有平面按 wire 格式紧凑排列，ICC 位于像素之后。
 * frame_desc 的 plane.data/offset 须指向该槽位，stride 为有效行字节；commit 不复制像素。
 * commit 成功后禁止继续写该指针；失败后须 abort 或修正后重试。 */
AWJ_API awj_status AWJ_CALL awj_shm_begin_write(awj_shm_channel* channel, uint32_t timeout_ms, awj_shm_write* output);
AWJ_API awj_status AWJ_CALL awj_shm_commit(awj_shm_channel* channel, uint64_t token, const awj_frame_desc* desc);
AWJ_API awj_status AWJ_CALL awj_shm_abort_write(awj_shm_channel* channel, uint64_t token);
AWJ_API awj_status AWJ_CALL awj_shm_receive(awj_shm_channel* channel, uint32_t timeout_ms, awj_frame** output);
/* close 唤醒等待者；关闭前已提交帧仍可 drain。END_OF_STREAM 表示关闭且无待取帧。
 * 帧释放归还槽位，正常路径不在持锁时调用用户代码。 */
AWJ_API void AWJ_CALL awj_shm_close(awj_shm_channel* channel);
AWJ_API void AWJ_CALL awj_shm_destroy(awj_shm_channel* channel);

#ifdef __cplusplus
}
#endif
#endif
