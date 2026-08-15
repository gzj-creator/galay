#include <galay/c/galay-http2-c/http2.h>

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    return actual == expected ? 0 : 1;
}

int main(void)
{
    if (expect_status(galay_http2_stream_id_validate(1, GALAY_FALSE), GALAY_OK)) {
        return 1;
    }
    if (expect_status(galay_http2_stream_id_validate(0, GALAY_FALSE), GALAY_INVALID_ARGUMENT)) {
        return 2;
    }
    if (expect_status(galay_http2_stream_id_validate(0x80000000u, GALAY_FALSE), GALAY_INVALID_ARGUMENT)) {
        return 3;
    }
    if (expect_status(galay_http2_stream_id_validate(0, GALAY_TRUE), GALAY_OK)) {
        return 4;
    }

    if (expect_status(galay_http2_settings_value_validate(GALAY_HTTP2_SETTINGS_ENABLE_PUSH, 1), GALAY_OK)) {
        return 5;
    }
    if (expect_status(galay_http2_settings_value_validate(GALAY_HTTP2_SETTINGS_ENABLE_PUSH, 2),
                      GALAY_PROTOCOL_ERROR)) {
        return 6;
    }
    if (expect_status(galay_http2_settings_value_validate(GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 2147483647u),
                      GALAY_OK)) {
        return 7;
    }
    if (expect_status(galay_http2_settings_value_validate(GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 2147483648u),
                      GALAY_PROTOCOL_ERROR)) {
        return 8;
    }
    if (expect_status(galay_http2_settings_value_validate(GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE, 16384u),
                      GALAY_OK)) {
        return 9;
    }
    if (expect_status(galay_http2_settings_value_validate(GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE, 16777215u),
                      GALAY_OK)) {
        return 10;
    }
    if (expect_status(galay_http2_settings_value_validate(GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE, 16383u),
                      GALAY_PROTOCOL_ERROR)) {
        return 11;
    }
    if (expect_status(galay_http2_settings_value_validate(GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE, 16777216u),
                      GALAY_PROTOCOL_ERROR)) {
        return 12;
    }

    return 0;
}
