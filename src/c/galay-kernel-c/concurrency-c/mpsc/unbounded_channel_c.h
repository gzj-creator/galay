#ifndef GALAY_KERNEL_MPSC_UNBOUNDED_CHANNEL_C_H
#define GALAY_KERNEL_MPSC_UNBOUNDED_CHANNEL_C_H

#ifndef GALAY_KERNEL_CHANNEL_C_COMMON_TYPES_DEFINED
#define GALAY_KERNEL_CHANNEL_C_COMMON_TYPES_DEFINED

#include "../../coro-c/coro_result_c.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum C_ChannelResultCode {
    C_ChannelSuccess,
    C_ChannelParameterInvalid,
    C_ChannelMemoryAllocFailed,
    C_ChannelClosed,
    C_ChannelFull,
    C_ChannelEmpty,
    C_ChannelOperationInvalid,
    C_ChannelIOFailed,
} C_ChannelResultCode;

typedef struct C_ChannelMessage {
    void* data;
    size_t size;
    void* user;
} C_ChannelMessage;

const char* galay_kernel_channel_get_error(C_ChannelResultCode code);

#ifdef __cplusplus
}
#endif

#endif /* GALAY_KERNEL_CHANNEL_C_COMMON_TYPES_DEFINED */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 多生产者、单消费者的分段无界通道。
 * @note 生产者可并发调用 send；所有接收操作必须串行。
 */
typedef struct galay_kernel_mpsc_unbounded_channel {
    void* channel;
} galay_kernel_mpsc_unbounded_channel_t;

/**
 * @brief 绑定一个生产线程的 MPSC 无界通道发送 token。
 * @note token 必须在所属 channel 销毁前销毁，且同一个 token 不得被多个线程并发使用。
 */
typedef struct galay_kernel_mpsc_unbounded_channel_producer_token {
    void* token;
    void* channel;
} galay_kernel_mpsc_unbounded_channel_producer_token_t;

C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_create(
    galay_kernel_mpsc_unbounded_channel_t* channel);
C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_destroy(
    galay_kernel_mpsc_unbounded_channel_t* channel);
C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_send(
    galay_kernel_mpsc_unbounded_channel_t* channel,
    const C_ChannelMessage* message);
C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_producer_token_create(
    galay_kernel_mpsc_unbounded_channel_t* channel,
    galay_kernel_mpsc_unbounded_channel_producer_token_t* token);
C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_producer_token_destroy(
    galay_kernel_mpsc_unbounded_channel_producer_token_t* token);
/** @brief 使用生产者 token 发送，避免默认发送路径的线程本地流查找。 */
C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_send_with_producer_token(
    galay_kernel_mpsc_unbounded_channel_t* channel,
    galay_kernel_mpsc_unbounded_channel_producer_token_t* token,
    const C_ChannelMessage* message);
C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_send_batch(
    galay_kernel_mpsc_unbounded_channel_t* channel,
    const C_ChannelMessage* messages,
    size_t count,
    size_t* out_count);
C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_try_recv(
    galay_kernel_mpsc_unbounded_channel_t* channel,
    C_ChannelMessage* message);
C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_try_recv_batch(
    galay_kernel_mpsc_unbounded_channel_t* channel,
    C_ChannelMessage* messages,
    size_t max_count,
    size_t* out_count);
C_IOResult galay_kernel_mpsc_unbounded_channel_recv(
    galay_kernel_mpsc_unbounded_channel_t* channel,
    C_ChannelMessage* message,
    int64_t timeout_ms);
C_IOResult galay_kernel_mpsc_unbounded_channel_recv_batch(
    galay_kernel_mpsc_unbounded_channel_t* channel,
    C_ChannelMessage* messages,
    size_t max_count,
    size_t* out_count,
    int64_t timeout_ms);
C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_close(
    galay_kernel_mpsc_unbounded_channel_t* channel);
bool galay_kernel_mpsc_unbounded_channel_is_closed(
    const galay_kernel_mpsc_unbounded_channel_t* channel);
size_t galay_kernel_mpsc_unbounded_channel_size(
    const galay_kernel_mpsc_unbounded_channel_t* channel);
bool galay_kernel_mpsc_unbounded_channel_empty(
    const galay_kernel_mpsc_unbounded_channel_t* channel);

#ifdef __cplusplus
}
#endif

#endif /* GALAY_KERNEL_MPSC_UNBOUNDED_CHANNEL_C_H */
