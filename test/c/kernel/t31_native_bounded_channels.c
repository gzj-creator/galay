#include <galay/c/galay-kernel-c/concurrency-c/mpmc/bounded_channel.h>
#include <galay/c/galay-kernel-c/concurrency-c/mpsc/bounded_channel.h>
#include <galay/c/galay-kernel-c/concurrency-c/spsc/bounded_channel.h>

#define TEST_CHANNEL(prefix, type, base) \
    do { \
        type channel = {0}; \
        int values[] = {11, 22, 33}; \
        galay_c_channel_message_t first = {&values[0], sizeof(values[0]), NULL}; \
        galay_c_channel_message_t second = {&values[1], sizeof(values[1]), NULL}; \
        galay_c_channel_message_t third = {&values[2], sizeof(values[2]), NULL}; \
        galay_c_channel_message_t received = {0}; \
        if (prefix##_create(&channel, 2).code != C_IOResultOk) return base + 1; \
        if (prefix##_try_recv(&channel, &received).code != C_IOResultInvalid) return base + 2; \
        if (prefix##_try_send(&channel, &first).code != C_IOResultOk) return base + 3; \
        if (prefix##_try_send(&channel, &second).code != C_IOResultOk) return base + 4; \
        if (prefix##_try_send(&channel, &third).code != C_IOResultInvalid) return base + 5; \
        if (prefix##_send(&channel, &third, 0).code != C_IOResultTimeout) return base + 6; \
        if (prefix##_try_recv(&channel, &received).code != C_IOResultOk || \
            received.data != &values[0]) return base + 7; \
        if (prefix##_close(&channel).code != C_IOResultOk) return base + 8; \
        if (prefix##_try_send(&channel, &third).code != C_IOResultClosed) return base + 9; \
        if (prefix##_try_recv(&channel, &received).code != C_IOResultOk || \
            received.data != &values[1]) return base + 10; \
        if (prefix##_try_recv(&channel, &received).code != C_IOResultClosed) return base + 11; \
        if (prefix##_destroy(&channel).code != C_IOResultOk) return base + 12; \
    } while (0)

int main(void)
{
    TEST_CHANNEL(galay_c_spsc_bounded_channel, galay_c_spsc_bounded_channel_t, 10);
    TEST_CHANNEL(galay_c_mpsc_bounded_channel, galay_c_mpsc_bounded_channel_t, 30);
    TEST_CHANNEL(galay_c_mpmc_bounded_channel, galay_c_mpmc_bounded_channel_t, 50);
    return 0;
}
