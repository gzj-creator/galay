#include <galay/c/galay-http2-c/http2_c.h>

#include <stdint.h>
#include <string.h>

static int expect_ok(galay_status_t status)
{
    return status == GALAY_OK ? 0 : 1;
}

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    return actual == expected ? 0 : 1;
}

int main(void)
{
    static const uint8_t opaque[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    galay_http2_frame_t* ping = NULL;
    if (expect_ok(galay_http2_ping_frame_create(opaque, GALAY_TRUE, &ping))) {
        return 1;
    }

    uint8_t encoded[GALAY_HTTP2_FRAME_HEADER_LENGTH + 8];
    size_t encoded_len = sizeof(encoded);
    if (expect_ok(galay_http2_frame_encode(ping, encoded, &encoded_len))) {
        galay_http2_frame_destroy(ping);
        return 2;
    }
    if (encoded_len != sizeof(encoded)) {
        galay_http2_frame_destroy(ping);
        return 3;
    }

    galay_http2_frame_t* decoded = NULL;
    if (expect_ok(galay_http2_frame_decode(encoded, encoded_len, &decoded))) {
        galay_http2_frame_destroy(ping);
        return 4;
    }
    if (galay_http2_frame_type(decoded) != GALAY_HTTP2_FRAME_PING) {
        galay_http2_frame_destroy(decoded);
        galay_http2_frame_destroy(ping);
        return 5;
    }
    if (galay_http2_frame_stream_id(decoded) != 0) {
        galay_http2_frame_destroy(decoded);
        galay_http2_frame_destroy(ping);
        return 6;
    }

    uint8_t decoded_opaque[8] = {0};
    if (expect_ok(galay_http2_ping_frame_opaque(decoded, decoded_opaque))) {
        galay_http2_frame_destroy(decoded);
        galay_http2_frame_destroy(ping);
        return 7;
    }
    if (memcmp(decoded_opaque, opaque, sizeof(opaque)) != 0) {
        galay_http2_frame_destroy(decoded);
        galay_http2_frame_destroy(ping);
        return 8;
    }

    encoded[0] = 0;
    encoded[1] = 0;
    encoded[2] = 9;
    galay_http2_frame_t* invalid = NULL;
    if (expect_status(galay_http2_frame_decode(encoded, encoded_len, &invalid), GALAY_PROTOCOL_ERROR)) {
        galay_http2_frame_destroy(decoded);
        galay_http2_frame_destroy(ping);
        return 9;
    }
    if (invalid != NULL) {
        galay_http2_frame_destroy(invalid);
        galay_http2_frame_destroy(decoded);
        galay_http2_frame_destroy(ping);
        return 10;
    }

    galay_http2_frame_destroy(decoded);
    galay_http2_frame_destroy(ping);
    return 0;
}
