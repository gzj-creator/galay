#ifndef GALAY_KERNEL_SPSC_UNBOUNDED_CHANNEL_C_H
#define GALAY_KERNEL_SPSC_UNBOUNDED_CHANNEL_C_H

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

/** @brief SPSC 无界通道 waiter 的兼容唤醒模式。 */
typedef enum C_SpscUnboundedChannelWakeMode {
    C_SpscUnboundedChannelWakeInline,
    C_SpscUnboundedChannelWakeDeferred,
} C_SpscUnboundedChannelWakeMode;

/**
 * @brief 单生产者、单消费者的分段无界通道。
 * @note SPSC C++ 数据面未定义 close 语义，因此此 C API 也不提供 close。
 */
typedef struct galay_kernel_spsc_unbounded_channel {
    void* channel;
} galay_kernel_spsc_unbounded_channel_t;

C_ChannelResultCode galay_kernel_spsc_unbounded_channel_create(
    galay_kernel_spsc_unbounded_channel_t* channel,
    C_SpscUnboundedChannelWakeMode wake_mode);
C_ChannelResultCode galay_kernel_spsc_unbounded_channel_destroy(
    galay_kernel_spsc_unbounded_channel_t* channel);
C_ChannelResultCode galay_kernel_spsc_unbounded_channel_send(
    galay_kernel_spsc_unbounded_channel_t* channel,
    const C_ChannelMessage* message);
C_ChannelResultCode galay_kernel_spsc_unbounded_channel_send_batch(
    galay_kernel_spsc_unbounded_channel_t* channel,
    const C_ChannelMessage* messages,
    size_t count,
    size_t* out_count);
C_ChannelResultCode galay_kernel_spsc_unbounded_channel_try_recv(
    galay_kernel_spsc_unbounded_channel_t* channel,
    C_ChannelMessage* message);
C_ChannelResultCode galay_kernel_spsc_unbounded_channel_try_recv_batch(
    galay_kernel_spsc_unbounded_channel_t* channel,
    C_ChannelMessage* messages,
    size_t max_count,
    size_t* out_count);
C_IOResult galay_kernel_spsc_unbounded_channel_recv(
    galay_kernel_spsc_unbounded_channel_t* channel,
    C_ChannelMessage* message,
    int64_t timeout_ms);
C_IOResult galay_kernel_spsc_unbounded_channel_recv_batch(
    galay_kernel_spsc_unbounded_channel_t* channel,
    C_ChannelMessage* messages,
    size_t max_count,
    size_t* out_count,
    int64_t timeout_ms);
size_t galay_kernel_spsc_unbounded_channel_size(
    const galay_kernel_spsc_unbounded_channel_t* channel);
bool galay_kernel_spsc_unbounded_channel_empty(
    const galay_kernel_spsc_unbounded_channel_t* channel);

#ifdef __cplusplus
}
#endif

#endif /* GALAY_KERNEL_SPSC_UNBOUNDED_CHANNEL_C_H */
