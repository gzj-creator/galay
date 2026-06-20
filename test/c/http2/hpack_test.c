#include <galay/c/galay-http2/http2.h>

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
    galay_http2_headers_t* headers = NULL;
    if (expect_ok(galay_http2_headers_create(&headers))) {
        return 1;
    }
    if (expect_ok(galay_http2_headers_add(headers, ":method", "GET"))) {
        galay_http2_headers_destroy(headers);
        return 2;
    }
    if (expect_ok(galay_http2_headers_add(headers, ":path", "/"))) {
        galay_http2_headers_destroy(headers);
        return 3;
    }

    uint8_t block[128];
    size_t block_len = sizeof(block);
    if (expect_ok(galay_http2_hpack_encode(headers, block, &block_len))) {
        galay_http2_headers_destroy(headers);
        return 4;
    }

    galay_http2_headers_t* decoded = NULL;
    if (expect_ok(galay_http2_hpack_decode(block, block_len, &decoded))) {
        galay_http2_headers_destroy(headers);
        return 5;
    }
    if (galay_http2_headers_count(decoded) != 2) {
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 6;
    }

    const char* name = NULL;
    const char* value = NULL;
    if (expect_ok(galay_http2_headers_get(decoded, 0, &name, &value))) {
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 7;
    }
    if (strcmp(name, ":method") != 0 || strcmp(value, "GET") != 0) {
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 8;
    }

    static const uint8_t malformed[] = {0x40, 0x03, 'x'};
    galay_http2_headers_t* malformed_headers = NULL;
    if (expect_status(galay_http2_hpack_decode(malformed, sizeof(malformed), &malformed_headers),
                      GALAY_PROTOCOL_ERROR)) {
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 9;
    }
    if (malformed_headers != NULL) {
        galay_http2_headers_destroy(malformed_headers);
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 10;
    }

    galay_http2_headers_destroy(decoded);
    galay_http2_headers_destroy(headers);
    return 0;
}
