#define _GNU_SOURCE
#include <galay/c/galay-kernel-c/concurrency-c/mpsc/bounded_channel.h>

#define CHANNEL_FAMILY "mpsc_bounded"
#define CHANNEL_HANDLE galay_c_mpsc_bounded_channel_t
#define CHANNEL_PRODUCER_COUNT 4
#define CHANNEL_CONSUMER_COUNT 1
#define CHANNEL_CAPACITY 4096
#define CHANNEL_HAS_CLOSE 1
#define CHANNEL_CREATE(channel) galay_c_mpsc_bounded_channel_create(channel, CHANNEL_CAPACITY)
#define CHANNEL_DESTROY(channel) galay_c_mpsc_bounded_channel_destroy(channel)
#define CHANNEL_TRY_SEND(channel, message) galay_c_mpsc_bounded_channel_try_send(channel, message)
#define CHANNEL_TRY_RECV(channel, message) galay_c_mpsc_bounded_channel_try_recv(channel, message)
#define CHANNEL_CLOSE(channel) galay_c_mpsc_bounded_channel_close(channel)
#define CHANNEL_EMPTY(channel) galay_c_mpsc_bounded_channel_is_empty(channel)

#include "channel_family_throughput.inc"
