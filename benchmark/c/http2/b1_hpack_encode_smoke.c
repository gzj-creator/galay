#include <galay/c/galay-http2/http2.h>

#include <stdio.h>

int main(void)
{
    galay_http2_headers_t* headers = NULL;
    if (galay_http2_headers_create(&headers) != GALAY_OK) {
        return 1;
    }
    if (galay_http2_headers_add(headers, ":method", "GET") != GALAY_OK ||
        galay_http2_headers_add(headers, ":path", "/benchmark") != GALAY_OK ||
        galay_http2_headers_add(headers, "user-agent", "galay-c-api") != GALAY_OK) {
        galay_http2_headers_destroy(headers);
        return 2;
    }

    size_t total_bytes = 0;
    for (size_t i = 0; i < 10000; ++i) {
        uint8_t block[256];
        size_t block_len = sizeof(block);
        if (galay_http2_hpack_encode(headers, block, &block_len) != GALAY_OK) {
            galay_http2_headers_destroy(headers);
            return 3;
        }
        total_bytes += block_len;
    }

    printf("encoded %zu HPACK bytes\n", total_bytes);
    galay_http2_headers_destroy(headers);
    return 0;
}
