#ifndef GALAY_KERNEL_MPSC_CHANNEL_C_H
#define GALAY_KERNEL_MPSC_CHANNEL_C_H

#include "../coro-c/coro_result_c.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file mpsc_channel_c.h
 * @brief Galay kernel MpscChannel 的 C ABI 封装。
 *
 * @details 该头文件封装多生产者、单消费者异步通道。C ABI 只移动或复制
 * C_MpscChannelMessage 结构体本身，不复制、不释放 data 指向的 payload。
 * 调用方必须保证 payload 在消费者读取前保持有效。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MpscChannel C ABI 操作结果码。
 *
 * @details 同步发送/非阻塞接收返回该枚举；会挂起的 recv/recv_batch 返回
 * C_IOResult。Timeout 同时表示 try_recv 当前无消息和 blocking recv 等待超时。
 */
typedef enum C_MpscChannelResultCode {
    C_MpscChannelSuccess,                ///< 操作成功。
    C_MpscChannelParameterInvalid,       ///< 参数错误。
    C_MpscChannelMemoryAllocFailed,      ///< 内存分配失败。
    C_MpscChannelIOFailed,               ///< 底层通道操作失败。
    C_MpscChannelOperationInvalid,       ///< 当前通道状态不允许执行该操作。
    C_MpscChannelTimeout,                ///< 接收超时或非阻塞接收当前无消息。
} C_MpscChannelResultCode;

/**
 * @brief MpscChannel 消息。
 *
 * @note data 是非拥有指针；C wrapper 不复制、不释放 payload。user 字段原样透传，
 * 可用于携带调用方上下文。
 */
typedef struct C_MpscChannelMessage {
    void* data;      ///< 非拥有 payload 指针。
    size_t size;     ///< payload 字节数；size 非 0 时 data 不能为 NULL。
    void* user;      ///< 调用方自定义上下文，wrapper 原样保存和返回。
} C_MpscChannelMessage;

/**
 * @brief MpscChannel C 句柄。
 *
 * @note channel 指向内部 C++ MpscChannel<C_MpscChannelMessage> 对象，调用方不能
 * 解引用或直接释放。
 */
typedef struct galay_kernel_mpsc_channel {
    void* channel;       ///< 内部 MpscChannel<C_MpscChannelMessage> 对象指针。
} galay_kernel_mpsc_channel_t;

/**
 * @brief 将 MpscChannel 结果码转换为可读错误信息。
 *
 * @param code C_MpscChannelResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_kernel_mpsc_channel_get_error(C_MpscChannelResultCode code);

/**
 * @brief 创建 MpscChannel。
 *
 * @param c_channel 输出 channel 句柄；成功时其 channel 字段指向内部 MpscChannel。
 * @return 成功返回 C_MpscChannelSuccess；参数无效返回 C_MpscChannelParameterInvalid；
 * 内存分配失败返回 C_MpscChannelMemoryAllocFailed。
 *
 * @note 该函数只创建通道对象，不启动协程。通道支持多生产者发送，但接收侧必须
 * 保持单消费者语义。
 */
C_MpscChannelResultCode galay_kernel_mpsc_channel_create(galay_kernel_mpsc_channel_t* c_channel);

/**
 * @brief 销毁 MpscChannel 内部资源。
 *
 * @param c_channel 由 galay_kernel_mpsc_channel_create 初始化的 channel 句柄。
 * @return 成功返回 C_MpscChannelSuccess；参数无效返回 C_MpscChannelParameterInvalid。
 *
 * @note 调用方必须保证没有未完成的 direct C coroutine recv 仍可能访问该 channel。
 * 该函数会释放 c_channel->channel 指向的内部对象，并将其置空。
 */
C_MpscChannelResultCode galay_kernel_mpsc_channel_destroy(galay_kernel_mpsc_channel_t* c_channel);

/**
 * @brief 发送单条消息。
 *
 * @param c_channel channel 句柄。
 * @param message 待发送消息；wrapper 只复制结构体，不复制 data 指向的 payload。
 * @return 成功返回 C_MpscChannelSuccess；参数无效返回 C_MpscChannelParameterInvalid；
 * 底层队列入队失败返回 C_MpscChannelIOFailed。
 *
 * @note send 可由多个生产者线程/协程调用，但 message->data 的生命周期仍由调用方
 * 负责，直到消费者取走并完成使用。
 */
C_MpscChannelResultCode galay_kernel_mpsc_channel_send(
    galay_kernel_mpsc_channel_t* c_channel,
    const C_MpscChannelMessage* message);

/**
 * @brief 批量发送消息。
 *
 * @param c_channel channel 句柄。
 * @param messages 待发送消息数组；count 为 0 时可为 NULL。
 * @param count 待发送消息数量。
 * @return 成功返回 C_MpscChannelSuccess；参数无效返回 C_MpscChannelParameterInvalid；
 * 内存分配失败返回 C_MpscChannelMemoryAllocFailed；底层队列入队失败返回 C_MpscChannelIOFailed。
 *
 * @note 批量发送按数组顺序逐条入队；如果中途失败，已成功入队的消息不会回滚。
 */
C_MpscChannelResultCode galay_kernel_mpsc_channel_send_batch(
    galay_kernel_mpsc_channel_t* c_channel,
    const C_MpscChannelMessage* messages,
    size_t count);

/**
 * @brief 非阻塞接收单条消息。
 *
 * @param c_channel channel 句柄。
 * @param message 输出消息。
 * @return 成功取到消息返回 C_MpscChannelSuccess；当前无消息返回 C_MpscChannelTimeout；
 * 参数无效返回 C_MpscChannelParameterInvalid。
 *
 * @note 接收侧必须保持单消费者语义。message 由调用方提供；成功时 wrapper 只写入
 * C_MpscChannelMessage 结构体，不复制 payload。
 */
C_MpscChannelResultCode galay_kernel_mpsc_channel_try_recv(
    galay_kernel_mpsc_channel_t* c_channel,
    C_MpscChannelMessage* message);

/**
 * @brief 非阻塞批量接收消息。
 *
 * @param c_channel channel 句柄。
 * @param messages 输出消息数组。
 * @param max_count 最多接收消息数，必须大于 0。
 * @param out_count 实际接收消息数。
 * @return 成功取到至少一条消息返回 C_MpscChannelSuccess；当前无消息返回
 * C_MpscChannelTimeout；参数无效返回 C_MpscChannelParameterInvalid。
 *
 * @note messages 数组由调用方提供，wrapper 只写入消息结构体。
 * 接收侧必须保持单消费者语义。
 */
C_MpscChannelResultCode galay_kernel_mpsc_channel_try_recv_batch(
    galay_kernel_mpsc_channel_t* c_channel,
    C_MpscChannelMessage* messages,
    size_t max_count,
    size_t* out_count);

/**
 * @brief 挂起当前 C coroutine 并接收单条消息。
 *
 * @param c_channel channel 句柄。
 * @param message 输出消息。
 * @param timeout_ms 负数无限等待，0 立即超时检查，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk；超时返回 C_IOResultTimeout；参数无效或不在
 * C coroutine 内调用返回 C_IOResultInvalid。
 *
 * @note 该函数通过 galay_coro_yield 让出当前 C coroutine，等待期间不会阻塞 OS
 * 线程。message 必须在函数返回前保持有效。接收侧必须保持单消费者语义。
 */
C_IOResult galay_kernel_mpsc_channel_recv(
    galay_kernel_mpsc_channel_t* c_channel,
    C_MpscChannelMessage* message,
    int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并批量接收消息。
 *
 * @param c_channel channel 句柄。
 * @param messages 输出消息数组，容量至少为 max_count。
 * @param max_count 最多接收消息数，必须大于 0。
 * @param out_count 实际接收消息数，进入等待前会置 0。
 * @param timeout_ms 负数无限等待，0 立即超时检查，正数为毫秒超时。
 * @return 成功接收到至少一条消息返回 C_IOResultOk；超时返回 C_IOResultTimeout；
 * 参数无效或不在 C coroutine 内调用返回 C_IOResultInvalid；yield 失败返回
 * C_IOResultError/sys_errno。
 *
 * @note 该函数通过 galay_coro_yield 让出当前 C coroutine，等待期间不会阻塞 OS
 * 线程。messages 和 out_count 必须在函数返回前保持有效。接收侧必须保持单消费者
 * 语义；wrapper 只复制 C_MpscChannelMessage 结构体，不复制 payload。
 */
C_IOResult galay_kernel_mpsc_channel_recv_batch(
    galay_kernel_mpsc_channel_t* c_channel,
    C_MpscChannelMessage* messages,
    size_t max_count,
    size_t* out_count,
    int64_t timeout_ms);

/**
 * @brief 查询当前待消费消息数量。
 *
 * @param c_channel channel 句柄；为空或未初始化时返回 0。
 * @return 当前通道中的消息数近似值。
 *
 * @note 并发生产者存在时，该值只适合监控和启发式判断，不能作为后续接收一定成功
 * 的同步条件。
 */
size_t galay_kernel_mpsc_channel_size(const galay_kernel_mpsc_channel_t* c_channel);

/**
 * @brief 查询通道是否为空。
 *
 * @param c_channel channel 句柄；为空或未初始化时返回 true。
 * @return true 表示当前没有待消费消息。
 *
 * @note 并发生产者存在时，返回值只是调用瞬间的观察结果。
 */
bool galay_kernel_mpsc_channel_empty(const galay_kernel_mpsc_channel_t* c_channel);

#ifdef __cplusplus
}
#endif

#endif
