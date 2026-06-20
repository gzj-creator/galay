#include <galay/c/galay-http2/http2.h>

#include <stdio.h>

int main(void)
{
    const uint8_t opaque[8] = {'g', 'a', 'l', 'a', 'y', 'h', '2', 0};
    galay_http2_frame_t* frame = NULL;
    if (galay_http2_ping_frame_create(opaque, GALAY_FALSE, &frame) != GALAY_OK) {
        return 1;
    }

    uint8_t bytes[GALAY_HTTP2_FRAME_HEADER_LENGTH + 8];
    size_t bytes_len = sizeof(bytes);
    if (galay_http2_frame_encode(frame, bytes, &bytes_len) != GALAY_OK) {
        galay_http2_frame_destroy(frame);
        return 2;
    }

    galay_http2_frame_t* decoded = NULL;
    if (galay_http2_frame_decode(bytes, bytes_len, &decoded) != GALAY_OK) {
        galay_http2_frame_destroy(frame);
        return 3;
    }

    printf("encoded HTTP/2 frame type=%u length=%zu\n",
           (unsigned)galay_http2_frame_type(decoded),
           bytes_len);

    galay_http2_frame_destroy(decoded);
    galay_http2_frame_destroy(frame);
    return 0;
}
