#define _GNU_SOURCE
#include <galay/c/galay-kernel-c/concurrency-c/spsc/unbounded_channel_c.h>

#define CHANNEL_FAMILY "spsc_unbounded"
#define CHANNEL_HANDLE galay_kernel_spsc_unbounded_channel_t
#define CHANNEL_PRODUCER_COUNT 1
#define CHANNEL_CONSUMER_COUNT 1
#define CHANNEL_CAPACITY 0
#define CHANNEL_HAS_CLOSE 0
#define CHANNEL_RETRYABLE_SEND_RESULT C_ChannelIOFailed
#define CHANNEL_CREATE(channel) galay_kernel_spsc_unbounded_channel_create( \
    channel, C_SpscUnboundedChannelWakeInline)
#define CHANNEL_DESTROY(channel) galay_kernel_spsc_unbounded_channel_destroy(channel)
#define CHANNEL_TRY_SEND(channel, message) galay_kernel_spsc_unbounded_channel_send(channel, message)
#define CHANNEL_TRY_RECV(channel, message) galay_kernel_spsc_unbounded_channel_try_recv(channel, message)
#define CHANNEL_EMPTY(channel) galay_kernel_spsc_unbounded_channel_empty(channel)

#include "channel_family_throughput.inc"
