#include <galay/c/galay-kernel-c/concurrency-c/mpmc/bounded_channel_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/mpmc/unbounded_channel_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/mpsc/bounded_channel_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/mpsc/unbounded_channel_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/spsc/bounded_channel_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/spsc/unbounded_channel_c.h>

#define CHECK(condition, error_code) \
    do { \
        if (!(condition)) { \
            return (error_code); \
        } \
    } while (0)

#define TEST_BOUNDED(prefix, handle_type, base) \
    do { \
        handle_type channel = {0}; \
        int values[] = {11, 22, 33}; \
        C_ChannelMessage first = {&values[0], sizeof(values[0]), 0}; \
        C_ChannelMessage second = {&values[1], sizeof(values[1]), 0}; \
        C_ChannelMessage third = {&values[2], sizeof(values[2]), 0}; \
        C_ChannelMessage received = {0}; \
        CHECK(prefix##_create(&channel, 2) == C_ChannelSuccess, base + 1); \
        CHECK(prefix##_try_recv(&channel, &received) == C_ChannelEmpty, base + 2); \
        CHECK(prefix##_recv(&channel, &received, 0).code == C_IOResultTimeout, \
              base + 3); \
        CHECK(prefix##_try_send(&channel, &first) == C_ChannelSuccess, base + 3); \
        CHECK(prefix##_try_send(&channel, &second) == C_ChannelSuccess, base + 4); \
        CHECK(prefix##_try_send(&channel, &third) == C_ChannelFull, base + 5); \
        CHECK(prefix##_send(&channel, &third, 0).code == C_IOResultTimeout, \
              base + 6); \
        CHECK(prefix##_try_recv(&channel, &received) == C_ChannelSuccess && \
              received.data == &values[0], base + 7); \
        CHECK(prefix##_close(&channel) == C_ChannelSuccess, base + 8); \
        CHECK(prefix##_try_send(&channel, &third) == C_ChannelClosed, base + 9); \
        CHECK(prefix##_try_recv(&channel, &received) == C_ChannelSuccess && \
              received.data == &values[1], base + 10); \
        CHECK(prefix##_try_recv(&channel, &received) == C_ChannelClosed, base + 11); \
        CHECK(prefix##_recv(&channel, &received, 0).code == C_IOResultCancelled, \
              base + 12); \
        CHECK(prefix##_destroy(&channel) == C_ChannelSuccess, base + 13); \
    } while (0)

#define TEST_CLOSABLE_UNBOUNDED(prefix, handle_type, base) \
    do { \
        handle_type channel = {0}; \
        int values[] = {41, 42, 43}; \
        C_ChannelMessage messages[] = { \
            {&values[0], sizeof(values[0]), 0}, \
            {&values[1], sizeof(values[1]), 0}, \
            {&values[2], sizeof(values[2]), 0}, \
        }; \
        C_ChannelMessage received[2] = {{0}}; \
        size_t count = 0; \
        CHECK(prefix##_create(&channel) == C_ChannelSuccess, base + 1); \
        CHECK(prefix##_try_recv(&channel, &received[0]) == C_ChannelEmpty, base + 2); \
        CHECK(prefix##_recv(&channel, &received[0], 0).code == C_IOResultTimeout, \
              base + 3); \
        CHECK(prefix##_send(&channel, &messages[0]) == C_ChannelSuccess, base + 3); \
        CHECK(prefix##_send_batch(&channel, &messages[1], 2, &count) == \
                  C_ChannelSuccess && count == 2, base + 4); \
        CHECK(prefix##_try_recv_batch(&channel, received, 2, &count) == \
                  C_ChannelSuccess && count == 2 && \
                  received[0].data == &values[0] && received[1].data == &values[1], \
              base + 5); \
        CHECK(prefix##_close(&channel) == C_ChannelSuccess, base + 6); \
        CHECK(prefix##_send(&channel, &messages[0]) == C_ChannelClosed, base + 7); \
        CHECK(prefix##_try_recv(&channel, &received[0]) == C_ChannelSuccess && \
              received[0].data == &values[2], base + 8); \
        CHECK(prefix##_try_recv(&channel, &received[0]) == C_ChannelClosed, base + 9); \
        CHECK(prefix##_recv(&channel, &received[0], 0).code == C_IOResultCancelled, \
              base + 10); \
        CHECK(prefix##_destroy(&channel) == C_ChannelSuccess, base + 11); \
    } while (0)

static int test_spsc_unbounded(void)
{
    galay_kernel_spsc_unbounded_channel_t channel = {0};
    int values[] = {71, 72, 73};
    C_ChannelMessage messages[] = {
        {&values[0], sizeof(values[0]), 0},
        {&values[1], sizeof(values[1]), 0},
        {&values[2], sizeof(values[2]), 0},
    };
    C_ChannelMessage received[2] = {{0}};
    size_t count = 0;

    CHECK(galay_kernel_spsc_unbounded_channel_create(
              &channel, C_SpscUnboundedChannelWakeInline) == C_ChannelSuccess,
          61);
    CHECK(galay_kernel_spsc_unbounded_channel_send_batch(
              &channel, messages, 3, &count) == C_ChannelSuccess && count == 3,
          62);
    CHECK(galay_kernel_spsc_unbounded_channel_try_recv_batch(
              &channel, received, 2, &count) == C_ChannelSuccess && count == 2 &&
              received[0].data == &values[0] && received[1].data == &values[1],
          63);
    CHECK(galay_kernel_spsc_unbounded_channel_try_recv(
              &channel, &received[0]) == C_ChannelSuccess &&
              received[0].data == &values[2],
          64);
    CHECK(galay_kernel_spsc_unbounded_channel_empty(&channel), 65);
    CHECK(galay_kernel_spsc_unbounded_channel_recv(
              &channel, &received[0], 0).code == C_IOResultTimeout,
          66);
    CHECK(galay_kernel_spsc_unbounded_channel_destroy(&channel) == C_ChannelSuccess, 67);
    return 0;
}

static int test_mpmc_unbounded_tokens(void)
{
    galay_kernel_mpmc_unbounded_channel_t channel = {0};
    galay_kernel_mpmc_unbounded_channel_t foreign = {0};
    galay_kernel_mpmc_unbounded_channel_producer_token_t producer = {0};
    galay_kernel_mpmc_unbounded_channel_consumer_token_t consumer = {0};
    int value = 101;
    C_ChannelMessage sent = {&value, sizeof(value), 0};
    C_ChannelMessage received = {0};

    CHECK(galay_kernel_mpmc_unbounded_channel_create(&channel) == C_ChannelSuccess, 110);
    CHECK(galay_kernel_mpmc_unbounded_channel_producer_token_create(
              &channel, &producer) == C_ChannelSuccess,
          111);
    CHECK(galay_kernel_mpmc_unbounded_channel_consumer_token_create(
              &channel, &consumer) == C_ChannelSuccess,
          112);
    CHECK(galay_kernel_mpmc_unbounded_channel_create(&foreign) == C_ChannelSuccess, 113);
    CHECK(galay_kernel_mpmc_unbounded_channel_send_with_producer_token(
              &foreign, &producer, &sent) == C_ChannelParameterInvalid,
          114);
    CHECK(galay_kernel_mpmc_unbounded_channel_destroy(&foreign) == C_ChannelSuccess, 115);
    CHECK(galay_kernel_mpmc_unbounded_channel_send_with_producer_token(
              &channel, &producer, &sent) == C_ChannelSuccess,
          116);
    CHECK(galay_kernel_mpmc_unbounded_channel_try_recv_with_consumer_token(
              &channel, &consumer, &received) == C_ChannelSuccess &&
              received.data == &value,
          117);
    CHECK(galay_kernel_mpmc_unbounded_channel_producer_token_destroy(&producer) ==
              C_ChannelSuccess,
          118);
    CHECK(galay_kernel_mpmc_unbounded_channel_send_with_producer_token(
              &channel, &producer, &sent) == C_ChannelParameterInvalid,
          119);
    CHECK(galay_kernel_mpmc_unbounded_channel_consumer_token_destroy(&consumer) ==
              C_ChannelSuccess,
          120);
    CHECK(galay_kernel_mpmc_unbounded_channel_destroy(&channel) == C_ChannelSuccess, 121);
    return 0;
}

static int test_mpsc_unbounded_token(void)
{
    galay_kernel_mpsc_unbounded_channel_t channel = {0};
    galay_kernel_mpsc_unbounded_channel_producer_token_t producer = {0};
    int value = 102;
    C_ChannelMessage sent = {&value, sizeof(value), 0};
    C_ChannelMessage received = {0};

    CHECK(galay_kernel_mpsc_unbounded_channel_create(&channel) == C_ChannelSuccess, 120);
    CHECK(galay_kernel_mpsc_unbounded_channel_producer_token_create(
              &channel, &producer) == C_ChannelSuccess,
          121);
    CHECK(galay_kernel_mpsc_unbounded_channel_send_with_producer_token(
              &channel, &producer, &sent) == C_ChannelSuccess,
          122);
    CHECK(galay_kernel_mpsc_unbounded_channel_try_recv(&channel, &received) ==
              C_ChannelSuccess && received.data == &value,
          123);
    CHECK(galay_kernel_mpsc_unbounded_channel_producer_token_destroy(&producer) ==
              C_ChannelSuccess,
          124);
    CHECK(galay_kernel_mpsc_unbounded_channel_destroy(&channel) == C_ChannelSuccess, 125);
    return 0;
}

int main(void)
{
    const C_ChannelResultCode codes[] = {
        C_ChannelSuccess,
        C_ChannelParameterInvalid,
        C_ChannelMemoryAllocFailed,
        C_ChannelClosed,
        C_ChannelFull,
        C_ChannelEmpty,
        C_ChannelOperationInvalid,
        C_ChannelIOFailed,
    };
    for (size_t index = 0; index < sizeof(codes) / sizeof(codes[0]); ++index) {
        const char* error = galay_kernel_channel_get_error(codes[index]);
        CHECK(error != 0 && error[0] != '\0', 1);
    }
    CHECK(galay_kernel_channel_get_error((C_ChannelResultCode)999) != 0, 2);

    TEST_BOUNDED(galay_kernel_mpmc_bounded_channel,
                 galay_kernel_mpmc_bounded_channel_t,
                 10);
    TEST_BOUNDED(galay_kernel_mpsc_bounded_channel,
                 galay_kernel_mpsc_bounded_channel_t,
                 25);
    TEST_BOUNDED(galay_kernel_spsc_bounded_channel,
                 galay_kernel_spsc_bounded_channel_t,
                 40);
    TEST_CLOSABLE_UNBOUNDED(galay_kernel_mpmc_unbounded_channel,
                            galay_kernel_mpmc_unbounded_channel_t,
                            70);
    TEST_CLOSABLE_UNBOUNDED(galay_kernel_mpsc_unbounded_channel,
                            galay_kernel_mpsc_unbounded_channel_t,
                            85);
    CHECK(test_mpmc_unbounded_tokens() == 0, 100);
    CHECK(test_mpsc_unbounded_token() == 0, 101);
    return test_spsc_unbounded();
}
