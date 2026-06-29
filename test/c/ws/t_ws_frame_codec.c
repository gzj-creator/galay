#include <galay/c/galay-ws-c/ws.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "%s:%d: expected true: %s\n", __FILE__, __LINE__, #expr); \
            return 1; \
        } \
    } while (0)

#define EXPECT_EQ_U64(actual, expected) \
    do { \
        uint64_t actual_value = (uint64_t)(actual); \
        uint64_t expected_value = (uint64_t)(expected); \
        if (actual_value != expected_value) { \
            fprintf(stderr, "%s:%d: expected %s == %s (%llu != %llu)\n", \
                    __FILE__, __LINE__, #actual, #expected, \
                    (unsigned long long)actual_value, \
                    (unsigned long long)expected_value); \
            return 1; \
        } \
    } while (0)

static int expect_payload_round_trip(size_t payload_len)
{
    uint8_t* payload = (uint8_t*)malloc(payload_len == 0 ? 1 : payload_len);
    uint8_t* decoded_payload = (uint8_t*)malloc(payload_len == 0 ? 1 : payload_len);
    uint8_t* encoded = NULL;
    if (payload == NULL || decoded_payload == NULL) {
        free(payload);
        free(decoded_payload);
        return 1;
    }

    for (size_t i = 0; i < payload_len; ++i) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    size_t encoded_size = 0;
    EXPECT_EQ_U64(galay_ws_encoded_size(payload_len, GALAY_FALSE, &encoded_size), GALAY_OK);
    encoded = (uint8_t*)malloc(encoded_size == 0 ? 1 : encoded_size);
    EXPECT_TRUE(encoded != NULL);

    size_t written = 0;
    EXPECT_EQ_U64(galay_ws_encode_frame(GALAY_WS_OPCODE_BINARY,
                                        payload,
                                        payload_len,
                                        GALAY_TRUE,
                                        NULL,
                                        encoded,
                                        encoded_size,
                                        &written),
                  GALAY_OK);
    EXPECT_EQ_U64(written, encoded_size);

    galay_ws_frame_t frame;
    size_t consumed = 0;
    galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;
    EXPECT_EQ_U64(galay_ws_decode_frame(encoded,
                                        written,
                                        GALAY_FALSE,
                                        &frame,
                                        decoded_payload,
                                        payload_len,
                                        &consumed,
                                        &ws_error),
                  GALAY_OK);
    EXPECT_EQ_U64(consumed, written);
    EXPECT_EQ_U64(frame.fin, GALAY_TRUE);
    EXPECT_EQ_U64(frame.opcode, GALAY_WS_OPCODE_BINARY);
    EXPECT_EQ_U64(frame.masked, GALAY_FALSE);
    EXPECT_EQ_U64(frame.payload_len, payload_len);
    EXPECT_EQ_U64(ws_error, GALAY_WS_ERROR_NONE);
    EXPECT_TRUE(payload_len == 0 || memcmp(decoded_payload, payload, payload_len) == 0);

    free(payload);
    free(decoded_payload);
    free(encoded);
    return 0;
}

static int test_header_smoke(void)
{
    _Static_assert(GALAY_WS_OPCODE_CONTINUATION == 0x0, "continuation opcode");
    _Static_assert(GALAY_WS_OPCODE_TEXT == 0x1, "text opcode");
    _Static_assert(GALAY_WS_OPCODE_BINARY == 0x2, "binary opcode");
    _Static_assert(GALAY_WS_OPCODE_CLOSE == 0x8, "close opcode");
    _Static_assert(GALAY_WS_OPCODE_PING == 0x9, "ping opcode");
    _Static_assert(GALAY_WS_OPCODE_PONG == 0xA, "pong opcode");
    _Static_assert(GALAY_WS_CLOSE_NORMAL == 1000, "normal close code");
    _Static_assert(GALAY_WS_CLOSE_PROTOCOL_ERROR == 1002, "protocol close code");
    return 0;
}

static int test_payload_boundaries(void)
{
    EXPECT_TRUE(expect_payload_round_trip(0) == 0);
    EXPECT_TRUE(expect_payload_round_trip(125) == 0);
    EXPECT_TRUE(expect_payload_round_trip(126) == 0);
    EXPECT_TRUE(expect_payload_round_trip(65535) == 0);
    EXPECT_TRUE(expect_payload_round_trip(65536) == 0);
    return 0;
}

static int test_invalid_opcode_rejected(void)
{
    const uint8_t invalid_opcode_frame[] = {0x83, 0x00};
    uint8_t payload[1] = {0};
    galay_ws_frame_t frame;
    size_t consumed = 0;
    galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;

    EXPECT_EQ_U64(galay_ws_decode_frame(invalid_opcode_frame,
                                        sizeof(invalid_opcode_frame),
                                        GALAY_FALSE,
                                        &frame,
                                        payload,
                                        sizeof(payload),
                                        &consumed,
                                        &ws_error),
                  GALAY_PROTOCOL_ERROR);
    EXPECT_EQ_U64(ws_error, GALAY_WS_ERROR_INVALID_OPCODE);
    EXPECT_EQ_U64(consumed, 0);

    EXPECT_EQ_U64(galay_ws_encode_frame((galay_ws_opcode_t)0x3,
                                        NULL,
                                        0,
                                        GALAY_TRUE,
                                        NULL,
                                        payload,
                                        sizeof(payload),
                                        &consumed),
                  GALAY_INVALID_ARGUMENT);
    return 0;
}

static int test_truncated_frame_reports_incomplete(void)
{
    const uint8_t truncated_frame[] = {0x82, 0x7E, 0x00};
    uint8_t payload[4] = {0};
    galay_ws_frame_t frame;
    size_t consumed = 99;
    galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;

    EXPECT_EQ_U64(galay_ws_decode_frame(truncated_frame,
                                        sizeof(truncated_frame),
                                        GALAY_FALSE,
                                        &frame,
                                        payload,
                                        sizeof(payload),
                                        &consumed,
                                        &ws_error),
                  GALAY_PROTOCOL_ERROR);
    EXPECT_EQ_U64(ws_error, GALAY_WS_ERROR_INCOMPLETE);
    EXPECT_EQ_U64(consumed, 0);
    return 0;
}

static int test_mask_round_trip(void)
{
    const uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    uint8_t data[] = {'g', 'a', 'l', 'a', 'y'};
    const uint8_t original[] = {'g', 'a', 'l', 'a', 'y'};

    EXPECT_EQ_U64(galay_ws_apply_mask(data, sizeof(data), mask_key), GALAY_OK);
    EXPECT_TRUE(memcmp(data, original, sizeof(data)) != 0);
    EXPECT_EQ_U64(galay_ws_apply_mask(data, sizeof(data), mask_key), GALAY_OK);
    EXPECT_TRUE(memcmp(data, original, sizeof(data)) == 0);

    size_t encoded_size = 0;
    EXPECT_EQ_U64(galay_ws_encoded_size(sizeof(original), GALAY_TRUE, &encoded_size), GALAY_OK);
    uint8_t encoded[32] = {0};
    size_t written = 0;
    EXPECT_EQ_U64(galay_ws_encode_frame(GALAY_WS_OPCODE_TEXT,
                                        original,
                                        sizeof(original),
                                        GALAY_TRUE,
                                        mask_key,
                                        encoded,
                                        sizeof(encoded),
                                        &written),
                  GALAY_OK);
    EXPECT_EQ_U64(written, encoded_size);

    uint8_t decoded_payload[sizeof(original)] = {0};
    galay_ws_frame_t frame;
    size_t consumed = 0;
    galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;
    EXPECT_EQ_U64(galay_ws_decode_frame(encoded,
                                        written,
                                        GALAY_TRUE,
                                        &frame,
                                        decoded_payload,
                                        sizeof(decoded_payload),
                                        &consumed,
                                        &ws_error),
                  GALAY_OK);
    EXPECT_EQ_U64(frame.masked, GALAY_TRUE);
    EXPECT_TRUE(memcmp(frame.masking_key, mask_key, sizeof(mask_key)) == 0);
    EXPECT_TRUE(memcmp(decoded_payload, original, sizeof(original)) == 0);
    return 0;
}

int main(void)
{
    if (test_header_smoke() != 0) return 1;
    if (test_payload_boundaries() != 0) return 1;
    if (test_invalid_opcode_rejected() != 0) return 1;
    if (test_truncated_frame_reports_incomplete() != 0) return 1;
    if (test_mask_round_trip() != 0) return 1;
    return 0;
}

