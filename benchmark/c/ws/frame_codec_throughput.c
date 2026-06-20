#include <galay/c/galay-ws/ws.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    enum { iterations = 100000 };
    const unsigned char mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    const unsigned char payload[] = "galay websocket c api benchmark payload";
    unsigned char encoded[128] = {0};
    unsigned char decoded[sizeof(payload) - 1] = {0};
    size_t written = 0;
    size_t consumed = 0;
    galay_ws_frame_t frame;

    const clock_t start = clock();
    for (int i = 0; i < iterations; ++i) {
        if (galay_ws_encode_frame(GALAY_WS_OPCODE_BINARY,
                                  payload,
                                  sizeof(payload) - 1,
                                  GALAY_TRUE,
                                  mask_key,
                                  encoded,
                                  sizeof(encoded),
                                  &written) != GALAY_OK) {
            return 1;
        }
        if (galay_ws_decode_frame(encoded,
                                  written,
                                  GALAY_TRUE,
                                  &frame,
                                  decoded,
                                  sizeof(decoded),
                                  &consumed,
                                  NULL) != GALAY_OK) {
            return 1;
        }
        if (frame.payload_len != sizeof(decoded) ||
            consumed != written ||
            memcmp(decoded, payload, sizeof(decoded)) != 0) {
            return 1;
        }
    }
    const clock_t end = clock();

    const double seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
    const double ops = seconds > 0.0 ? (double)iterations / seconds : 0.0;
    printf("c.ws.frame_codec iterations=%d seconds=%.6f ops_per_sec=%.2f\n",
           iterations,
           seconds,
           ops);
    return 0;
}

