#define _GNU_SOURCE
#include <galay/c/galay-kernel-c/concurrency-c/mpmc/bounded_channel_c.h>

#define CHANNEL_FAMILY "mpmc_bounded"
#define CHANNEL_HANDLE galay_kernel_mpmc_bounded_channel_t
#define CHANNEL_PRODUCER_COUNT 4
#define CHANNEL_CONSUMER_COUNT 4
#define CHANNEL_CAPACITY 4096
#define CHANNEL_HAS_CLOSE 1
#define CHANNEL_RETRYABLE_SEND_RESULT C_ChannelFull
#define CHANNEL_CREATE(channel) galay_kernel_mpmc_bounded_channel_create(channel, CHANNEL_CAPACITY)
#define CHANNEL_DESTROY(channel) galay_kernel_mpmc_bounded_channel_destroy(channel)
#define CHANNEL_TRY_SEND(channel, message) galay_kernel_mpmc_bounded_channel_try_send(channel, message)
#define CHANNEL_TRY_RECV(channel, message) galay_kernel_mpmc_bounded_channel_try_recv(channel, message)
#define CHANNEL_CLOSE(channel) galay_kernel_mpmc_bounded_channel_close(channel)
#define CHANNEL_EMPTY(channel) galay_kernel_mpmc_bounded_channel_empty(channel)

#include "channel_family_throughput.inc"
