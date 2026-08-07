#define _GNU_SOURCE
#include <galay/c/galay-kernel-c/concurrency-c/mpsc/unbounded_channel_c.h>

#define CHANNEL_FAMILY "mpsc_unbounded_token"
#define CHANNEL_HANDLE galay_kernel_mpsc_unbounded_channel_t
#define CHANNEL_PRODUCER_COUNT 4
#define CHANNEL_CONSUMER_COUNT 1
#define CHANNEL_CAPACITY 0
#define CHANNEL_HAS_CLOSE 1
#define CHANNEL_RETRYABLE_SEND_RESULT C_ChannelIOFailed
#define CHANNEL_CREATE(channel) galay_kernel_mpsc_unbounded_channel_create(channel)
#define CHANNEL_DESTROY(channel) galay_kernel_mpsc_unbounded_channel_destroy(channel)
#define CHANNEL_TRY_SEND(channel, message) galay_kernel_mpsc_unbounded_channel_send(channel, message)
#define CHANNEL_TRY_RECV(channel, message) galay_kernel_mpsc_unbounded_channel_try_recv(channel, message)
#define CHANNEL_CLOSE(channel) galay_kernel_mpsc_unbounded_channel_close(channel)
#define CHANNEL_EMPTY(channel) galay_kernel_mpsc_unbounded_channel_empty(channel)
#define CHANNEL_USE_PRODUCER_TOKEN 1
#define CHANNEL_PRODUCER_TOKEN_TYPE galay_kernel_mpsc_unbounded_channel_producer_token_t
#define CHANNEL_PRODUCER_TOKEN_CREATE(channel, token) galay_kernel_mpsc_unbounded_channel_producer_token_create(channel, token)
#define CHANNEL_PRODUCER_TOKEN_DESTROY(token) galay_kernel_mpsc_unbounded_channel_producer_token_destroy(token)
#define CHANNEL_TRY_SEND_WITH_PRODUCER_TOKEN(channel, token, message) galay_kernel_mpsc_unbounded_channel_send_with_producer_token(channel, token, message)

#include "channel_family_throughput.inc"
