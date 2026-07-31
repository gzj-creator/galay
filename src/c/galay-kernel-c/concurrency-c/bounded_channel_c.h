#ifndef GALAY_KERNEL_BOUNDED_CHANNEL_C_H
#define GALAY_KERNEL_BOUNDED_CHANNEL_C_H

#include "../coro-c/coro_result_c.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file bounded_channel_c.h
 * @brief Galay kernel BoundedChannel 的 C ABI 封装。
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details C wrapper 内部持有 C++ BoundedChannel<C_BoundedChannelMessage>，支持
 * 多生产者和多消费者并发访问。wrapper 只复制消息结构体，不复制、不释放 data
 * 指向的 payload；调用方必须保证 payload 在消费者完成使用前保持有效。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BoundedChannel C ABI 同步操作结果码。
 */
typedef enum C_BoundedChannelResultCode {
    C_BoundedChannelSuccess,            ///< 操作成功。
    C_BoundedChannelParameterInvalid,   ///< 参数、句柄或容量无效。
    C_BoundedChannelMemoryAllocFailed,  ///< 创建内部通道对象失败。
    C_BoundedChannelClosed,             ///< 通道已关闭，不能继续发送或已无消息可读。
    C_BoundedChannelFull,               ///< 非阻塞发送时 ring 当前已满。
    C_BoundedChannelEmpty,              ///< 非阻塞接收时 ring 当前为空。
} C_BoundedChannelResultCode;

/**
 * @brief BoundedChannel 消息。
 *
 * @note data 是非拥有指针；size 非 0 时 data 不能为 NULL。user 字段由 wrapper
 * 原样保存和返回，可用于携带调用方上下文。
 */
typedef struct C_BoundedChannelMessage {
    void* data;      ///< 非拥有 payload 指针。
    size_t size;     ///< payload 字节数。
    void* user;      ///< 调用方自定义上下文。
} C_BoundedChannelMessage;

/**
 * @brief BoundedChannel C 句柄。
 *
 * @note channel 指向内部 C++ BoundedChannel<C_BoundedChannelMessage>，调用方
 * 不能解引用或直接释放。
 */
typedef struct galay_kernel_bounded_channel {
    void* channel;   ///< 内部 C++ 通道对象指针。
} galay_kernel_bounded_channel_t;

/**
 * @brief 将 BoundedChannel 结果码转换为稳定错误字符串。
 * @param code C_BoundedChannelResultCode 结果码，允许传入未知枚举值。
 * @return 静态字符串，调用方不需要释放；未知值返回通用 unknown 字符串。
 */
const char* galay_kernel_bounded_channel_get_error(C_BoundedChannelResultCode code);

/**
 * @brief 创建固定容量 MPMC 通道。
 * @param c_channel 输出句柄，成功时 channel 字段指向内部 C++ 对象。
 * @param capacity 期望容量；不大于 2 时实际容量为 2，其余向上取整到 2 的幂。
 * @return 成功返回 C_BoundedChannelSuccess；参数或容量无效返回
 * C_BoundedChannelParameterInvalid；对象分配失败返回 C_BoundedChannelMemoryAllocFailed。
 * @note 该函数只创建通道，不启动协程。实际容量通过
 * galay_kernel_bounded_channel_capacity() 查询。
 */
C_BoundedChannelResultCode galay_kernel_bounded_channel_create(
    galay_kernel_bounded_channel_t* c_channel,
    size_t capacity);

/**
 * @brief 销毁 BoundedChannel 内部资源。
 * @param c_channel 由 create 初始化的句柄。
 * @return 成功返回 C_BoundedChannelSuccess；空句柄地址返回
 * C_BoundedChannelParameterInvalid。
 * @pre 调用方必须保证没有线程或 C coroutine 仍在访问该通道。
 * @note 函数会先关闭通道，再释放内部对象并把 channel 字段置空。
 */
C_BoundedChannelResultCode galay_kernel_bounded_channel_destroy(
    galay_kernel_bounded_channel_t* c_channel);

/**
 * @brief 非阻塞发送一条消息。
 * @param c_channel 通道句柄。
 * @param message 待发送消息；仅复制结构体，不复制 payload。
 * @return 成功返回 C_BoundedChannelSuccess；通道关闭返回 C_BoundedChannelClosed；
 * 当前已满返回 C_BoundedChannelFull；参数无效返回 C_BoundedChannelParameterInvalid。
 * @note 可由多个生产者线程并发调用；失败不会修改 message 或其 payload。
 */
C_BoundedChannelResultCode galay_kernel_bounded_channel_try_send(
    galay_kernel_bounded_channel_t* c_channel,
    const C_BoundedChannelMessage* message);

/**
 * @brief 按数组顺序非阻塞批量发送消息。
 * @param c_channel 通道句柄。
 * @param messages 待发送数组；count 为 0 时可为 NULL。
 * @param count 待发送消息数。
 * @param out_count 实际成功发送数量，调用前会置 0。
 * @return 全部发送成功返回 C_BoundedChannelSuccess；中途遇到关闭或满分别返回
 * C_BoundedChannelClosed 或 C_BoundedChannelFull；参数无效返回
 * C_BoundedChannelParameterInvalid。
 * @note 批量操作不回滚已经成功发送的消息，调用方必须检查 out_count。
 */
C_BoundedChannelResultCode galay_kernel_bounded_channel_try_send_batch(
    galay_kernel_bounded_channel_t* c_channel,
    const C_BoundedChannelMessage* messages,
    size_t count,
    size_t* out_count);

/**
 * @brief 在 C coroutine 中等待容量并发送一条消息。
 * @param c_channel 通道句柄。
 * @param message 待发送消息；等待期间只保存结构体副本，payload 生命周期由调用方保证。
 * @param timeout_ms 负数无限等待，0 仅尝试一次，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk 且 bytes 为 1；关闭返回 C_IOResultCancelled；
 * 超时返回 C_IOResultTimeout；参数或协程上下文无效返回 C_IOResultInvalid。
 * @note 仅在通道已满时挂起当前 C coroutine，不阻塞 OS 线程。
 */
C_IOResult galay_kernel_bounded_channel_send(
    galay_kernel_bounded_channel_t* c_channel,
    const C_BoundedChannelMessage* message,
    int64_t timeout_ms);

/**
 * @brief 非阻塞接收一条消息。
 * @param c_channel 通道句柄。
 * @param message 输出消息；失败时清零。
 * @return 成功返回 C_BoundedChannelSuccess；通道仍打开但当前为空返回
 * C_BoundedChannelEmpty；通道关闭且已排空返回 C_BoundedChannelClosed；参数无效
 * 返回 C_BoundedChannelParameterInvalid。
 * @note 可由多个消费者线程并发调用；wrapper 只复制消息结构体。
 */
C_BoundedChannelResultCode galay_kernel_bounded_channel_try_recv(
    galay_kernel_bounded_channel_t* c_channel,
    C_BoundedChannelMessage* message);

/**
 * @brief 非阻塞接收最多 max_count 条消息。
 * @param c_channel 通道句柄。
 * @param messages 输出数组，容量至少为 max_count。
 * @param max_count 最大接收数量，必须大于 0。
 * @param out_count 实际接收数量，调用前会置 0。
 * @return 收到至少一条消息返回 C_BoundedChannelSuccess；否则返回
 * C_BoundedChannelEmpty、C_BoundedChannelClosed 或 C_BoundedChannelParameterInvalid。
 * @note 获取首条消息后不会等待后续消息补齐 max_count。
 */
C_BoundedChannelResultCode galay_kernel_bounded_channel_try_recv_batch(
    galay_kernel_bounded_channel_t* c_channel,
    C_BoundedChannelMessage* messages,
    size_t max_count,
    size_t* out_count);

/**
 * @brief 在 C coroutine 中等待并接收一条消息。
 * @param c_channel 通道句柄。
 * @param message 输出消息，失败时清零。
 * @param timeout_ms 负数无限等待，0 仅尝试一次，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk 且 bytes 为 1；关闭且排空返回
 * C_IOResultCancelled；超时返回 C_IOResultTimeout；参数或协程上下文无效返回
 * C_IOResultInvalid。
 * @note 仅在通道为空时挂起当前 C coroutine，不阻塞 OS 线程。
 */
C_IOResult galay_kernel_bounded_channel_recv(
    galay_kernel_bounded_channel_t* c_channel,
    C_BoundedChannelMessage* message,
    int64_t timeout_ms);

/**
 * @brief 在 C coroutine 中等待至少一条消息并批量接收。
 * @param c_channel 通道句柄。
 * @param messages 输出数组，容量至少为 max_count。
 * @param max_count 最大接收数量，必须大于 0。
 * @param out_count 实际接收数量，调用前会置 0。
 * @param timeout_ms 负数无限等待，0 仅尝试一次，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk 且 bytes 等于实际数量；关闭且排空返回
 * C_IOResultCancelled；超时返回 C_IOResultTimeout；参数或协程上下文无效返回
 * C_IOResultInvalid。
 */
C_IOResult galay_kernel_bounded_channel_recv_batch(
    galay_kernel_bounded_channel_t* c_channel,
    C_BoundedChannelMessage* messages,
    size_t max_count,
    size_t* out_count,
    int64_t timeout_ms);

/**
 * @brief 关闭通道并使等待中的 C coroutine 观察到取消状态。
 * @param c_channel 通道句柄。
 * @return 成功返回 C_BoundedChannelSuccess；句柄无效返回
 * C_BoundedChannelParameterInvalid。
 * @note 操作幂等；关闭后禁止发送，接收仍可排空已发布消息。
 */
C_BoundedChannelResultCode galay_kernel_bounded_channel_close(
    galay_kernel_bounded_channel_t* c_channel);

/**
 * @brief 查询通道是否已关闭。
 * @param c_channel 通道句柄；无效句柄按已关闭处理。
 * @return true 表示已关闭或句柄无效。
 */
bool galay_kernel_bounded_channel_is_closed(
    const galay_kernel_bounded_channel_t* c_channel);

/**
 * @brief 返回规范化后的固定容量。
 * @param c_channel 通道句柄；无效句柄返回 0。
 * @return 实际 ring 容量。
 */
size_t galay_kernel_bounded_channel_capacity(
    const galay_kernel_bounded_channel_t* c_channel);

/**
 * @brief 返回 ring 中的近似消息数。
 * @param c_channel 通道句柄；无效句柄返回 0。
 * @return 调用时刻的近似消息数，不包含直接 waiter 交接。
 * @note 该值仅用于监控，不能作为后续发送或接收一定成功的同步条件。
 */
size_t galay_kernel_bounded_channel_size(
    const galay_kernel_bounded_channel_t* c_channel);

/**
 * @brief 近似检查 ring 是否为空。
 * @param c_channel 通道句柄；无效句柄返回 true。
 * @return true 表示当前近似为空。
 */
bool galay_kernel_bounded_channel_empty(
    const galay_kernel_bounded_channel_t* c_channel);

/**
 * @brief 近似检查 ring 是否已满。
 * @param c_channel 通道句柄；无效句柄返回 false。
 * @return true 表示当前近似已满。
 */
bool galay_kernel_bounded_channel_full(
    const galay_kernel_bounded_channel_t* c_channel);

#ifdef __cplusplus
}
#endif

#endif // GALAY_KERNEL_BOUNDED_CHANNEL_C_H
