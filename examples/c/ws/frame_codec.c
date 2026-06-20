#include <galay/c/galay-ws/ws.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    const unsigned char payload[] = "hello";
    unsigned char encoded[32] = {0};
    unsigned char decoded[sizeof(payload) - 1] = {0};
    size_t written = 0;
    size_t consumed = 0;
    galay_ws_frame_t frame;

    if (galay_ws_encode_frame(GALAY_WS_OPCODE_TEXT,
                              payload,
                              sizeof(payload) - 1,
                              GALAY_TRUE,
                              NULL,
                              encoded,
                              sizeof(encoded),
                              &written) != GALAY_OK) {
        return 1;
    }

    if (galay_ws_decode_frame(encoded,
                              written,
                              GALAY_FALSE,
                              &frame,
                              decoded,
                              sizeof(decoded),
                              &consumed,
                              NULL) != GALAY_OK) {
        return 1;
    }

    if (frame.opcode != GALAY_WS_OPCODE_TEXT ||
        frame.payload_len != sizeof(decoded) ||
        consumed != written ||
        memcmp(decoded, payload, sizeof(decoded)) != 0) {
        return 1;
    }

    printf("%.*s\n", (int)frame.payload_len, decoded);
    return 0;
}

