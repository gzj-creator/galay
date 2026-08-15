#include <galay/c/galay-http2-c/http2.h>

#include <string.h>

static int expect_ok(galay_status_t status)
{
    return status == GALAY_OK ? 0 : 1;
}

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    return actual == expected ? 0 : 1;
}

static int test_standard_static_index(void)
{
    static const uint8_t indexed_get[] = {0x82};
    galay_http2_headers_t* headers = NULL;
    const char* name = NULL;
    const char* value = NULL;

    if (expect_ok(galay_http2_hpack_decode(indexed_get, sizeof(indexed_get), &headers)) ||
        galay_http2_headers_count(headers) != 1 ||
        expect_ok(galay_http2_headers_get(headers, 0, &name, &value)) ||
        strcmp(name, ":method") != 0 || strcmp(value, "GET") != 0) {
        galay_http2_headers_destroy(headers);
        return 1;
    }
    galay_http2_headers_destroy(headers);
    return 0;
}

static int test_standard_huffman_block(void)
{
    static const uint8_t block[] = {
        0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b,
        0xa0, 0xab, 0x90, 0xf4, 0xff,
    };
    galay_http2_headers_t* headers = NULL;
    const galay_status_t status = galay_http2_hpack_decode(block, sizeof(block), &headers);
    if (status != GALAY_OK ||
        galay_http2_headers_count(headers) != 4) {
        galay_http2_headers_destroy(headers);
        return 1;
    }
    const char* name = NULL;
    const char* value = NULL;
    if (expect_ok(galay_http2_headers_get(headers, 3, &name, &value)) ||
        strcmp(name, ":authority") != 0 || strcmp(value, "www.example.com") != 0) {
        galay_http2_headers_destroy(headers);
        return 2;
    }
    galay_http2_headers_destroy(headers);
    return 0;
}

int main(void)
{
    if (test_standard_static_index() != 0) {
        return 1;
    }
    if (test_standard_huffman_block() != 0) {
        return 2;
    }
    galay_http2_headers_t* headers = NULL;
    if (expect_ok(galay_http2_headers_create(&headers))) {
        return 1;
    }
    if (expect_ok(galay_http2_headers_add(headers, ":method", "GET")) ||
        galay_http2_headers_add(headers, "bad name", "value") != GALAY_INVALID_ARGUMENT ||
        galay_http2_headers_add(headers, "x-\x80-test", "value") != GALAY_INVALID_ARGUMENT ||
        galay_http2_headers_add(headers, "x-test", "line\nfeed") != GALAY_INVALID_ARGUMENT) {
        galay_http2_headers_destroy(headers);
        return 3;
    }
    if (expect_ok(galay_http2_headers_add(headers, ":path", "/"))) {
        galay_http2_headers_destroy(headers);
        return 4;
    }
    char long_value[301];
    memset(long_value, 'a', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    if (expect_ok(galay_http2_headers_add(headers, "x-long", long_value))) {
        galay_http2_headers_destroy(headers);
        return 5;
    }

    uint8_t block[512];
    size_t block_len = sizeof(block);
    if (expect_ok(galay_http2_hpack_encode(headers, block, &block_len))) {
        galay_http2_headers_destroy(headers);
        return 6;
    }

    galay_http2_headers_t* decoded = NULL;
    if (expect_ok(galay_http2_hpack_decode(block, block_len, &decoded))) {
        galay_http2_headers_destroy(headers);
        return 7;
    }
    if (galay_http2_headers_count(decoded) != 3) {
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 8;
    }

    const char* name = NULL;
    const char* value = NULL;
    if (expect_ok(galay_http2_headers_get(decoded, 0, &name, &value))) {
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 9;
    }
    if (strcmp(name, ":method") != 0 || strcmp(value, "GET") != 0) {
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 10;
    }
    if (expect_ok(galay_http2_headers_get(decoded, 2, &name, &value)) ||
        strcmp(name, "x-long") != 0 || strlen(value) != 300) {
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 11;
    }

    static const uint8_t malformed[] = {0x40, 0x03, 'x'};
    galay_http2_headers_t* malformed_headers = NULL;
    if (expect_status(galay_http2_hpack_decode(malformed, sizeof(malformed), &malformed_headers),
                      GALAY_PROTOCOL_ERROR)) {
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 12;
    }
    if (malformed_headers != NULL) {
        galay_http2_headers_destroy(malformed_headers);
        galay_http2_headers_destroy(decoded);
        galay_http2_headers_destroy(headers);
        return 13;
    }

    galay_http2_headers_destroy(decoded);
    galay_http2_headers_destroy(headers);
    return 0;
}
