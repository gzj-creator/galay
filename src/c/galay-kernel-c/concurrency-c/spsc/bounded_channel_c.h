#ifndef GALAY_KERNEL_SPSC_BOUNDED_CHANNEL_C_H
#define GALAY_KERNEL_SPSC_BOUNDED_CHANNEL_C_H

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
 * @brief 单生产者、单消费者的固定容量通道。
 * @note 一个时间只能有一个生产者和一个消费者调用各自侧的操作。
 */
typedef struct galay_kernel_spsc_bounded_channel {
    void* channel;
} galay_kernel_spsc_bounded_channel_t;

C_ChannelResultCode galay_kernel_spsc_bounded_channel_create(
    galay_kernel_spsc_bounded_channel_t* channel, size_t capacity);
C_ChannelResultCode galay_kernel_spsc_bounded_channel_destroy(
    galay_kernel_spsc_bounded_channel_t* channel);
C_ChannelResultCode galay_kernel_spsc_bounded_channel_try_send(
    galay_kernel_spsc_bounded_channel_t* channel,
    const C_ChannelMessage* message);
C_ChannelResultCode galay_kernel_spsc_bounded_channel_try_send_batch(
    galay_kernel_spsc_bounded_channel_t* channel,
    const C_ChannelMessage* messages,
    size_t count,
    size_t* out_count);
C_IOResult galay_kernel_spsc_bounded_channel_send(
    galay_kernel_spsc_bounded_channel_t* channel,
    const C_ChannelMessage* message,
    int64_t timeout_ms);
C_ChannelResultCode galay_kernel_spsc_bounded_channel_try_recv(
    galay_kernel_spsc_bounded_channel_t* channel,
    C_ChannelMessage* message);
C_ChannelResultCode galay_kernel_spsc_bounded_channel_try_recv_batch(
    galay_kernel_spsc_bounded_channel_t* channel,
    C_ChannelMessage* messages,
    size_t max_count,
    size_t* out_count);
C_IOResult galay_kernel_spsc_bounded_channel_recv(
    galay_kernel_spsc_bounded_channel_t* channel,
    C_ChannelMessage* message,
    int64_t timeout_ms);
C_IOResult galay_kernel_spsc_bounded_channel_recv_batch(
    galay_kernel_spsc_bounded_channel_t* channel,
    C_ChannelMessage* messages,
    size_t max_count,
    size_t* out_count,
    int64_t timeout_ms);
C_ChannelResultCode galay_kernel_spsc_bounded_channel_close(
    galay_kernel_spsc_bounded_channel_t* channel);
bool galay_kernel_spsc_bounded_channel_is_closed(
    const galay_kernel_spsc_bounded_channel_t* channel);
size_t galay_kernel_spsc_bounded_channel_capacity(
    const galay_kernel_spsc_bounded_channel_t* channel);
size_t galay_kernel_spsc_bounded_channel_size(
    const galay_kernel_spsc_bounded_channel_t* channel);
bool galay_kernel_spsc_bounded_channel_empty(
    const galay_kernel_spsc_bounded_channel_t* channel);
bool galay_kernel_spsc_bounded_channel_full(
    const galay_kernel_spsc_bounded_channel_t* channel);

#ifdef __cplusplus
}
#endif

#endif /* GALAY_KERNEL_SPSC_BOUNDED_CHANNEL_C_H */
