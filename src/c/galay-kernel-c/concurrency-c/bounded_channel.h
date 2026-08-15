#ifndef GALAY_C_KERNEL_CONCURRENCY_BOUNDED_CHANNEL_H
#define GALAY_C_KERNEL_CONCURRENCY_BOUNDED_CHANNEL_H

#include "../coro-c/coro_result.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct galay_c_channel_message {
    void* data;
    size_t size;
    void* user_data;
} galay_c_channel_message_t;

typedef struct galay_c_bounded_channel {
    galay_c_channel_message_t* buffer;
    size_t capacity;
    size_t mask;
    _Alignas(64) _Atomic size_t write_pos;
    _Alignas(64) _Atomic size_t read_pos;
    _Atomic int closed;
    atomic_flag guard;
} galay_c_bounded_channel_t;

#endif
