#ifndef GALAY_C_KERNEL_CONCURRENCY_SPSC_BOUNDED_CHANNEL_H
#define GALAY_C_KERNEL_CONCURRENCY_SPSC_BOUNDED_CHANNEL_H

#include "../bounded_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef galay_c_bounded_channel_t galay_c_spsc_bounded_channel_t;

C_IOResult galay_c_spsc_bounded_channel_create(galay_c_spsc_bounded_channel_t* channel,
                                                size_t capacity);
C_IOResult galay_c_spsc_bounded_channel_destroy(galay_c_spsc_bounded_channel_t* channel);
C_IOResult galay_c_spsc_bounded_channel_try_send(galay_c_spsc_bounded_channel_t* channel,
                                                  const galay_c_channel_message_t* message);
C_IOResult galay_c_spsc_bounded_channel_send(galay_c_spsc_bounded_channel_t* channel,
                                              const galay_c_channel_message_t* message,
                                              int64_t timeout_ms);
C_IOResult galay_c_spsc_bounded_channel_try_recv(galay_c_spsc_bounded_channel_t* channel,
                                                  galay_c_channel_message_t* message);
C_IOResult galay_c_spsc_bounded_channel_recv(galay_c_spsc_bounded_channel_t* channel,
                                              galay_c_channel_message_t* message,
                                              int64_t timeout_ms);
C_IOResult galay_c_spsc_bounded_channel_close(galay_c_spsc_bounded_channel_t* channel);
size_t galay_c_spsc_bounded_channel_size(const galay_c_spsc_bounded_channel_t* channel);
int galay_c_spsc_bounded_channel_is_empty(const galay_c_spsc_bounded_channel_t* channel);
int galay_c_spsc_bounded_channel_is_full(const galay_c_spsc_bounded_channel_t* channel);

#ifdef __cplusplus
}
#endif

#endif
