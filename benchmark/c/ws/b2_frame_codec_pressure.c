#include <galay/c/galay-ws-c/ws_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    static const uint8_t mask_key[4] = {0x10, 0x20, 0x30, 0x40};
    const size_t sizes[] = {0, 1, 125, 126, 4096, 65536};
    size_t total_bytes = 0;

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        const size_t payload_len = sizes[s];
        uint8_t* payload = (uint8_t*)malloc(payload_len == 0 ? 1 : payload_len);
        uint8_t* decoded = (uint8_t*)malloc(payload_len == 0 ? 1 : payload_len);
        size_t encoded_size = 0;
        if (payload == NULL || decoded == NULL ||
            galay_ws_encoded_size(payload_len, GALAY_TRUE, &encoded_size) != GALAY_OK) {
            free(payload);
            free(decoded);
            return 1;
        }
        uint8_t* encoded = (uint8_t*)malloc(encoded_size == 0 ? 1 : encoded_size);
        if (encoded == NULL) {
            free(payload);
            free(decoded);
            return 2;
        }
        for (size_t i = 0; i < payload_len; ++i) {
            payload[i] = (uint8_t)(i & 0xFFU);
        }
        for (int iteration = 0; iteration < 512; ++iteration) {
            size_t written = 0;
            size_t consumed = 0;
            galay_ws_frame_t frame = {0};
            galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;
            if (galay_ws_encode_frame(GALAY_WS_OPCODE_BINARY,
                                      payload,
                                      payload_len,
                                      GALAY_TRUE,
                                      mask_key,
                                      encoded,
                                      encoded_size,
                                      &written) != GALAY_OK ||
                galay_ws_decode_frame(encoded,
                                      written,
                                      GALAY_TRUE,
                                      &frame,
                                      decoded,
                                      payload_len,
                                      &consumed,
                                      &ws_error) != GALAY_OK ||
                frame.payload_len != payload_len ||
                consumed != written ||
                (payload_len != 0 && memcmp(decoded, payload, payload_len) != 0)) {
                free(encoded);
                free(payload);
                free(decoded);
                return 3;
            }
            total_bytes += payload_len;
        }
        free(encoded);
        free(payload);
        free(decoded);
    }

    if (printf("codec_pressure_bytes=%zu\n", total_bytes) < 0) {
        return 4;
    }
    return 0;
}
