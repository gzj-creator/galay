#ifndef GALAY_KERNEL_UNSAFE_CHANNEL_C_H
#define GALAY_KERNEL_UNSAFE_CHANNEL_C_H

#include "../core-c/runtime_c.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file unsafe_channel_c.h
 * @brief Galay kernel UnsafeChannel 的 C ABI 封装。
 *
 * @details 该头文件封装单调度器、非线程安全的 UnsafeChannel。C ABI 只复制
 * C_UnsafeChannelMessage 结构体本身，不复制、不释放 data 指向的 payload。
 * 调用方必须保证 payload 在消费者读取前保持有效。
 *
 * @warning UnsafeChannel 仅适合同一 scheduler 内协程通信；不要跨线程或跨
 * scheduler 调用。Inline wake mode 会在 send/send_batch 调用栈内恢复等待
 * 协程，可能导致 callback 重入发送侧调用栈。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UnsafeChannel 唤醒策略。
 */
typedef enum C_UnsafeChannelWakeMode {
    C_UnsafeChannelWakeModeInline,    ///< 发送侧内联恢复等待协程，可能重入调用栈。
    C_UnsafeChannelWakeModeDeferred,  ///< 通过 scheduler 延迟恢复等待协程。
} C_UnsafeChannelWakeMode;

/**
 * @brief UnsafeChannel C ABI 操作结果码。
 */
typedef enum C_UnsafeChannelResultCode {
    C_UnsafeChannelSuccess,                ///< 操作成功。
    C_UnsafeChannelParameterInvalid,       ///< 参数错误。
    C_UnsafeChannelMemoryAllocFailed,      ///< 内存分配失败。
    C_UnsafeChannelIOFailed,               ///< 底层通道操作失败。
    C_UnsafeChannelOperationInvalid,       ///< 当前通道状态不允许执行该操作。
    C_UnsafeChannelTimeout,                ///< 接收超时或非阻塞接收当前无消息。
    C_UnsafeChannelRuntimeNotRunning,      ///< runtime 未启动。
    C_UnsafeChannelRuntimeSpawnFailed,     ///< runtime 提交任务失败。
} C_UnsafeChannelResultCode;

/**
 * @brief UnsafeChannel 消息。
 *
 * @note data 是非拥有指针；C wrapper 不复制、不释放 payload。user 字段原样
 * 保存和返回，可用于携带调用方上下文。
 */
typedef struct C_UnsafeChannelMessage {
    void* data;      ///< 非拥有 payload 指针。
    size_t size;     ///< payload 字节数；size 非 0 时 data 不能为 NULL。
    void* user;      ///< 调用方自定义上下文，wrapper 原样保存和返回。
} C_UnsafeChannelMessage;

/**
 * @brief UnsafeChannel C 句柄。
 *
 * @note channel 指向内部 C++ UnsafeChannel<C_UnsafeChannelMessage> 对象，
 * 调用方不能解引用或直接释放。
 */
typedef struct galay_kernel_unsafe_channel {
    void* channel;       ///< 内部 UnsafeChannel<C_UnsafeChannelMessage> 对象指针。
} galay_kernel_unsafe_channel_t;

/**
 * @brief UnsafeChannel 异步接收回调结果。
 *
 * @note 单条接收成功时 message 有效且 messages 为 NULL、count 为 0；
 * 批量或攒批接收成功时 messages 指向 count 条消息。messages 数组只在
 * callback 调用期间有效，调用方需要长期保存时必须自行复制消息结构体。
 */
typedef struct galay_kernel_unsafe_channel_recv_result {
    C_UnsafeChannelResultCode code;        ///< 接收结果码。
    C_UnsafeChannelMessage message;        ///< 单条接收结果。
    C_UnsafeChannelMessage* messages;      ///< 批量接收结果数组，仅 callback 期间有效。
    size_t count;                          ///< 批量接收结果数量。
} galay_kernel_unsafe_channel_recv_result_t;

/**
 * @brief UnsafeChannel 异步接收完成回调。
 *
 * @param result 接收结果；只在回调期间有效。
 * @param ctx 调用 recv/recv_timeout/recv_batch/recv_batch_timeout/recv_batched/
 * recv_batched_timeout 时传入的用户上下文。
 */
typedef void (*galay_kernel_unsafe_channel_recv_callback_t)(
    galay_kernel_unsafe_channel_recv_result_t* result,
    void* ctx);

/**
 * @brief 将 UnsafeChannel 结果码转换为可读错误信息。
 *
 * @param code C_UnsafeChannelResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_kernel_unsafe_channel_get_error(C_UnsafeChannelResultCode code);

/**
 * @brief 创建 UnsafeChannel。
 *
 * @param c_channel 输出 channel 句柄；成功时其 channel 字段指向内部 UnsafeChannel。
 * @param wake_mode 数据到达时唤醒等待协程的策略。
 * @return 成功返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid；内存分配失败返回
 * C_UnsafeChannelMemoryAllocFailed。
 *
 * @note 该函数只创建通道对象，不启动协程。Inline wake mode 可能在
 * send/send_batch 调用栈内执行等待协程与 callback。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_create(
    galay_kernel_unsafe_channel_t* c_channel,
    C_UnsafeChannelWakeMode wake_mode);

/**
 * @brief 销毁 UnsafeChannel 内部资源。
 *
 * @param c_channel 由 galay_kernel_unsafe_channel_create 初始化的 channel 句柄。
 * @return 成功返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid。
 *
 * @note 调用方必须保证没有未完成的异步 recv callback 仍可能访问该 channel。
 * 该函数会释放 c_channel->channel 指向的内部对象，并将其置空。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_destroy(
    galay_kernel_unsafe_channel_t* c_channel);

/**
 * @brief 发送单条消息。
 *
 * @param c_channel channel 句柄。
 * @param message 待发送消息；wrapper 只复制结构体，不复制 data 指向的 payload。
 * @return 成功返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid；底层队列入队失败返回 C_UnsafeChannelIOFailed。
 *
 * @warning 仅可在同一 scheduler/线程语境下调用。Inline wake mode 下该调用
 * 可能在返回前重入等待协程 callback。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_send(
    galay_kernel_unsafe_channel_t* c_channel,
    const C_UnsafeChannelMessage* message);

/**
 * @brief 批量发送消息。
 *
 * @param c_channel channel 句柄。
 * @param messages 待发送消息数组；count 为 0 时可为 NULL。
 * @param count 待发送消息数量。
 * @return 成功返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid；内存分配失败返回
 * C_UnsafeChannelMemoryAllocFailed；底层队列入队失败返回
 * C_UnsafeChannelIOFailed。
 *
 * @warning 仅可在同一 scheduler/线程语境下调用。Inline wake mode 下该调用
 * 可能在返回前重入等待协程 callback。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_send_batch(
    galay_kernel_unsafe_channel_t* c_channel,
    const C_UnsafeChannelMessage* messages,
    size_t count);

/**
 * @brief 非阻塞接收单条消息。
 *
 * @param c_channel channel 句柄。
 * @param message 输出消息。
 * @return 成功取到消息返回 C_UnsafeChannelSuccess；当前无消息返回
 * C_UnsafeChannelTimeout；参数无效返回 C_UnsafeChannelParameterInvalid。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_try_recv(
    galay_kernel_unsafe_channel_t* c_channel,
    C_UnsafeChannelMessage* message);

/**
 * @brief 非阻塞批量接收消息。
 *
 * @param c_channel channel 句柄。
 * @param messages 输出消息数组。
 * @param max_count 最多接收消息数，必须大于 0。
 * @param out_count 实际接收消息数。
 * @return 成功取到至少一条消息返回 C_UnsafeChannelSuccess；当前无消息返回
 * C_UnsafeChannelTimeout；参数无效返回 C_UnsafeChannelParameterInvalid。
 *
 * @note messages 数组由调用方提供，wrapper 只写入消息结构体。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_try_recv_batch(
    galay_kernel_unsafe_channel_t* c_channel,
    C_UnsafeChannelMessage* messages,
    size_t max_count,
    size_t* out_count);

/**
 * @brief 在 runtime 上异步接收单条消息。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_channel channel 句柄；必须存活到 callback 完成。
 * @param callback 接收完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid；runtime 未运行返回
 * C_UnsafeChannelRuntimeNotRunning；提交失败返回
 * C_UnsafeChannelRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待消息；最终结果通过 callback 在 runtime 调度线程上
 * 上报。UnsafeChannel 非线程安全，不应对同一 channel 并发提交多个 recv。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步接收单条消息，带毫秒级超时。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_channel channel 句柄；必须存活到 callback 完成。
 * @param timeout_ms 超时时间，单位毫秒。
 * @param callback 接收完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid；runtime 未运行返回
 * C_UnsafeChannelRuntimeNotRunning；提交失败返回
 * C_UnsafeChannelRuntimeSpawnFailed。
 *
 * @note timeout_ms 为 0 时表示立即超时检查。超时完成时 callback 收到
 * C_UnsafeChannelTimeout，message 为零值、messages 为 NULL 且 count 为 0；成功时
 * message 只在回调期间有效。UnsafeChannel 非线程安全，不应对同一 channel 并发提交多个 recv。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    uint64_t timeout_ms,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步批量接收消息。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_channel channel 句柄；必须存活到 callback 完成。
 * @param max_count 最多接收消息数，必须大于 0。
 * @param callback 接收完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid；runtime 未运行返回
 * C_UnsafeChannelRuntimeNotRunning；提交失败返回
 * C_UnsafeChannelRuntimeSpawnFailed。
 *
 * @note callback 中的 messages 数组只在回调期间有效。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_batch(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    size_t max_count,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步批量接收消息，带毫秒级超时。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_channel channel 句柄；必须存活到 callback 完成。
 * @param max_count 最多接收消息数，必须大于 0。
 * @param timeout_ms 超时时间，单位毫秒。
 * @param callback 接收完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid；runtime 未运行返回
 * C_UnsafeChannelRuntimeNotRunning；提交失败返回
 * C_UnsafeChannelRuntimeSpawnFailed。
 *
 * @note timeout_ms 为 0 时表示立即超时检查。超时完成时 callback 收到
 * C_UnsafeChannelTimeout，messages 为 NULL 且 count 为 0；成功时 messages 数组只在
 * 回调期间有效。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_batch_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    size_t max_count,
    uint64_t timeout_ms,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步攒批接收消息。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_channel channel 句柄；必须存活到 callback 完成。
 * @param limit 唤醒前至少需要累积的消息数，必须大于 0。
 * @param callback 接收完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid；runtime 未运行返回
 * C_UnsafeChannelRuntimeNotRunning；提交失败返回
 * C_UnsafeChannelRuntimeSpawnFailed。
 *
 * @note 未达到 limit 时不会因普通 send/send_batch 唤醒；callback 中的 messages
 * 数组只在回调期间有效。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_batched(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    size_t limit,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步攒批接收消息，带毫秒级超时。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_channel channel 句柄；必须存活到 callback 完成。
 * @param limit 唤醒前至少需要累积的消息数，必须大于 0。
 * @param timeout_ms 超时时间，单位毫秒。
 * @param callback 接收完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_UnsafeChannelSuccess；参数无效返回
 * C_UnsafeChannelParameterInvalid；runtime 未运行返回
 * C_UnsafeChannelRuntimeNotRunning；提交失败返回
 * C_UnsafeChannelRuntimeSpawnFailed。
 *
 * @note timeout 触发时若通道内已有部分消息，则 callback 返回
 * C_UnsafeChannelSuccess 与这批部分消息；纯空超时才返回 C_UnsafeChannelTimeout。
 * callback 中的 messages 数组只在回调期间有效。
 */
C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_batched_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    size_t limit,
    uint64_t timeout_ms,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx);

/**
 * @brief 查询当前待消费消息数量。
 *
 * @param c_channel channel 句柄；为空或未初始化时返回 0。
 * @return 当前通道中的消息数。
 */
size_t galay_kernel_unsafe_channel_size(const galay_kernel_unsafe_channel_t* c_channel);

/**
 * @brief 查询通道是否为空。
 *
 * @param c_channel channel 句柄；为空或未初始化时返回 true。
 * @return true 表示当前没有待消费消息。
 */
bool galay_kernel_unsafe_channel_empty(const galay_kernel_unsafe_channel_t* c_channel);

#ifdef __cplusplus
}
#endif

#endif
