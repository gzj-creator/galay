#include <galay/c/galay-ws-c/ws_c.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

static double elapsed_seconds(struct timespec begin, struct timespec end)
{
    return (double)(end.tv_sec - begin.tv_sec) +
        (double)(end.tv_nsec - begin.tv_nsec) / 1000000000.0;
}

int main(void)
{
    static const uint8_t payload[] = "galay websocket message";
    static const uint8_t mask_key[4] = {0x21, 0x43, 0x65, 0x87};
    enum { kIterations = 100000 };
    uint8_t encoded[128] = {0};
    uint8_t decoded[128] = {0};
    size_t written = 0;
    galay_ws_frame_t frame = {0};
    size_t consumed = 0;
    galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;

    struct timespec begin = {0, 0};
    struct timespec end = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &begin) != 0) {
        return 1;
    }
    for (int i = 0; i < kIterations; ++i) {
        if (galay_ws_encode_frame(GALAY_WS_OPCODE_TEXT,
                                  payload,
                                  sizeof(payload) - 1,
                                  GALAY_TRUE,
                                  mask_key,
                                  encoded,
                                  sizeof(encoded),
                                  &written) != GALAY_OK ||
            galay_ws_decode_frame(encoded,
                                  written,
                                  GALAY_TRUE,
                                  &frame,
                                  decoded,
                                  sizeof(decoded),
                                  &consumed,
                                  &ws_error) != GALAY_OK ||
            frame.opcode != GALAY_WS_OPCODE_TEXT ||
            consumed != written ||
            memcmp(decoded, payload, sizeof(payload) - 1) != 0) {
            return 2;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        return 3;
    }
    const double seconds = elapsed_seconds(begin, end);
    const double messages_per_second = seconds > 0.0 ? (double)kIterations / seconds : 0.0;
    if (printf("messages=%d seconds=%.6f messages_per_second=%.2f\n",
               kIterations,
               seconds,
               messages_per_second) < 0) {
        return 4;
    }
    return 0;
}
