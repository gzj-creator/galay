#include "bounded_channel.h"
#include "mpmc/bounded_channel.h"
#include "mpsc/bounded_channel.h"
#include "spsc/bounded_channel.h"

#include "../coro-c/coro_task_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

static void channel_lock(galay_c_bounded_channel_t* channel)
{
    while (atomic_flag_test_and_set_explicit(&channel->guard, memory_order_acquire)) {
    }
}

static void channel_unlock(galay_c_bounded_channel_t* channel)
{
    atomic_flag_clear_explicit(&channel->guard, memory_order_release);
}

static size_t next_power_of_two(size_t value)
{
    if (value <= 1) {
        return 1;
    }
    --value;
    for (size_t shift = 1; shift < sizeof(value) * 8; shift *= 2) {
        value |= value >> shift;
    }
    return value + 1;
}

static C_IOResult channel_create(galay_c_bounded_channel_t* channel, size_t capacity)
{
    if (channel == NULL || channel->buffer != NULL || capacity == 0) {
        return make_result(C_IOResultInvalid, 0);
    }
    capacity = next_power_of_two(capacity);
    if (capacity == 0 || capacity > SIZE_MAX / sizeof(*channel->buffer)) {
        return make_result(C_IOResultInvalid, 0);
    }
    galay_c_channel_message_t* const buffer = calloc(capacity, sizeof(*buffer));
    if (buffer == NULL) {
        return make_result(C_IOResultError, ENOMEM);
    }
    memset(channel, 0, sizeof(*channel));
    channel->buffer = buffer;
    channel->capacity = capacity;
    channel->mask = capacity - 1;
    atomic_init(&channel->write_pos, 0);
    atomic_init(&channel->read_pos, 0);
    atomic_init(&channel->closed, 0);
    atomic_flag_clear(&channel->guard);
    return make_result(C_IOResultOk, 0);
}

static C_IOResult channel_destroy(galay_c_bounded_channel_t* channel)
{
    if (channel == NULL || channel->buffer == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    free(channel->buffer);
    memset(channel, 0, sizeof(*channel));
    return make_result(C_IOResultOk, 0);
}

static C_IOResult channel_try_send(galay_c_bounded_channel_t* channel,
                                   const galay_c_channel_message_t* message)
{
    if (channel == NULL || channel->buffer == NULL || message == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    channel_lock(channel);
    if (atomic_load_explicit(&channel->closed, memory_order_acquire) != 0) {
        channel_unlock(channel);
        return make_result(C_IOResultClosed, 0);
    }
    const size_t write = atomic_load_explicit(&channel->write_pos, memory_order_relaxed);
    const size_t read = atomic_load_explicit(&channel->read_pos, memory_order_acquire);
    if (write - read >= channel->capacity) {
        channel_unlock(channel);
        return make_result(C_IOResultInvalid, 0);
    }
    channel->buffer[write & channel->mask] = *message;
    atomic_store_explicit(&channel->write_pos, write + 1, memory_order_release);
    channel_unlock(channel);
    return make_result(C_IOResultOk, 0);
}

static C_IOResult channel_try_recv(galay_c_bounded_channel_t* channel,
                                   galay_c_channel_message_t* message)
{
    if (channel == NULL || channel->buffer == NULL || message == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    channel_lock(channel);
    const size_t read = atomic_load_explicit(&channel->read_pos, memory_order_relaxed);
    const size_t write = atomic_load_explicit(&channel->write_pos, memory_order_acquire);
    if (read == write) {
        const int closed = atomic_load_explicit(&channel->closed, memory_order_acquire);
        channel_unlock(channel);
        return make_result(closed ? C_IOResultClosed : C_IOResultInvalid, 0);
    }
    *message = channel->buffer[read & channel->mask];
    atomic_store_explicit(&channel->read_pos, read + 1, memory_order_release);
    channel_unlock(channel);
    return make_result(C_IOResultOk, 0);
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0
        ? (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000
        : -1;
}

static C_IOResult channel_send(galay_c_bounded_channel_t* channel,
                               const galay_c_channel_message_t* message,
                               int64_t timeout_ms)
{
    if (timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const int64_t start = monotonic_milliseconds();
    for (;;) {
        const C_IOResult result = channel_try_send(channel, message);
        if (result.code != C_IOResultInvalid) {
            return result;
        }
        if (timeout_ms == 0 ||
            (timeout_ms > 0 && monotonic_milliseconds() - start >= timeout_ms)) {
            return make_result(C_IOResultTimeout, 0);
        }
        const C_IOResult yielded = galay_c_coro_yield();
        if (yielded.code != C_IOResultOk) {
            return yielded;
        }
    }
}

static C_IOResult channel_recv(galay_c_bounded_channel_t* channel,
                               galay_c_channel_message_t* message,
                               int64_t timeout_ms)
{
    if (timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const int64_t start = monotonic_milliseconds();
    for (;;) {
        const C_IOResult result = channel_try_recv(channel, message);
        if (result.code != C_IOResultInvalid) {
            return result;
        }
        if (timeout_ms == 0 ||
            (timeout_ms > 0 && monotonic_milliseconds() - start >= timeout_ms)) {
            return make_result(C_IOResultTimeout, 0);
        }
        const C_IOResult yielded = galay_c_coro_yield();
        if (yielded.code != C_IOResultOk) {
            return yielded;
        }
    }
}

static C_IOResult channel_close(galay_c_bounded_channel_t* channel)
{
    if (channel == NULL || channel->buffer == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    atomic_store_explicit(&channel->closed, 1, memory_order_release);
    return make_result(C_IOResultOk, 0);
}

static size_t channel_size(const galay_c_bounded_channel_t* channel)
{
    if (channel == NULL || channel->buffer == NULL) {
        return 0;
    }
    const size_t write = atomic_load_explicit(&channel->write_pos, memory_order_acquire);
    const size_t read = atomic_load_explicit(&channel->read_pos, memory_order_acquire);
    return write - read;
}

#define DEFINE_CHANNEL_API(prefix, type) \
    C_IOResult prefix##_create(type* channel, size_t capacity) \
    { return channel_create(channel, capacity); } \
    C_IOResult prefix##_destroy(type* channel) \
    { return channel_destroy(channel); } \
    C_IOResult prefix##_try_send(type* channel, const galay_c_channel_message_t* message) \
    { return channel_try_send(channel, message); } \
    C_IOResult prefix##_send(type* channel, const galay_c_channel_message_t* message, \
                             int64_t timeout_ms) \
    { return channel_send(channel, message, timeout_ms); } \
    C_IOResult prefix##_try_recv(type* channel, galay_c_channel_message_t* message) \
    { return channel_try_recv(channel, message); } \
    C_IOResult prefix##_recv(type* channel, galay_c_channel_message_t* message, \
                             int64_t timeout_ms) \
    { return channel_recv(channel, message, timeout_ms); } \
    C_IOResult prefix##_close(type* channel) \
    { return channel_close(channel); } \
    size_t prefix##_size(const type* channel) \
    { return channel_size(channel); } \
    int prefix##_is_empty(const type* channel) \
    { return channel_size(channel) == 0; } \
    int prefix##_is_full(const type* channel) \
    { return channel != NULL && channel_size(channel) >= channel->capacity; }

DEFINE_CHANNEL_API(galay_c_spsc_bounded_channel, galay_c_spsc_bounded_channel_t)
DEFINE_CHANNEL_API(galay_c_mpsc_bounded_channel, galay_c_mpsc_bounded_channel_t)
DEFINE_CHANNEL_API(galay_c_mpmc_bounded_channel, galay_c_mpmc_bounded_channel_t)
