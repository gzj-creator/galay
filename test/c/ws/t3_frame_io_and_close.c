#include <galay/c/galay-ws-c/ws_c.h>

#include <stdint.h>
#include <stdio.h>
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

static int decode_once(const uint8_t* data,
                       size_t data_len,
                       galay_bool_t expect_masked,
                       uint8_t* payload,
                       size_t payload_len,
                       galay_ws_frame_t* frame,
                       galay_ws_error_t* ws_error)
{
    size_t consumed = 0;
    *ws_error = GALAY_WS_ERROR_NONE;
    return galay_ws_decode_frame(data,
                                 data_len,
                                 expect_masked,
                                 frame,
                                 payload,
                                 payload_len,
                                 &consumed,
                                 ws_error);
}

static int test_data_and_control_round_trip(void)
{
    static const uint8_t text[] = {'h', 'e', 'l', 'l', 'o'};
    static const uint8_t binary[] = {0x00, 0x01, 0x7F, 0x80};
    static const uint8_t ping[] = {'p', 'i', 'n', 'g'};
    static const uint8_t pong[] = {'p', 'o', 'n', 'g'};
    static const uint8_t close_payload[] = {0x03, 0xE8};
    const struct {
        galay_ws_opcode_t opcode;
        const uint8_t* payload;
        size_t payload_len;
    } cases[] = {
        {GALAY_WS_OPCODE_TEXT, text, sizeof(text)},
        {GALAY_WS_OPCODE_BINARY, binary, sizeof(binary)},
        {GALAY_WS_OPCODE_PING, ping, sizeof(ping)},
        {GALAY_WS_OPCODE_PONG, pong, sizeof(pong)},
        {GALAY_WS_OPCODE_CLOSE, close_payload, sizeof(close_payload)},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t encoded[64] = {0};
        uint8_t decoded[64] = {0};
        size_t written = 0;
        galay_ws_frame_t frame = {0};
        galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;

        EXPECT_EQ_U64(galay_ws_encode_frame(cases[i].opcode,
                                            cases[i].payload,
                                            cases[i].payload_len,
                                            GALAY_TRUE,
                                            NULL,
                                            encoded,
                                            sizeof(encoded),
                                            &written),
                      GALAY_OK);
        EXPECT_EQ_U64(decode_once(encoded,
                                  written,
                                  GALAY_FALSE,
                                  decoded,
                                  sizeof(decoded),
                                  &frame,
                                  &ws_error),
                      GALAY_OK);
        EXPECT_EQ_U64(frame.opcode, cases[i].opcode);
        EXPECT_EQ_U64(frame.fin, GALAY_TRUE);
        EXPECT_EQ_U64(frame.payload_len, cases[i].payload_len);
        EXPECT_EQ_U64(ws_error, GALAY_WS_ERROR_NONE);
        EXPECT_TRUE(memcmp(decoded, cases[i].payload, cases[i].payload_len) == 0);
    }
    return 0;
}

static int test_fragment_frames_are_accepted(void)
{
    static const uint8_t part1[] = {'h', 'e'};
    static const uint8_t part2[] = {'l', 'l', 'o'};
    uint8_t encoded[32] = {0};
    uint8_t decoded[8] = {0};
    size_t written = 0;
    galay_ws_frame_t frame = {0};
    galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;

    EXPECT_EQ_U64(galay_ws_encode_frame(GALAY_WS_OPCODE_TEXT,
                                        part1,
                                        sizeof(part1),
                                        GALAY_FALSE,
                                        NULL,
                                        encoded,
                                        sizeof(encoded),
                                        &written),
                  GALAY_OK);
    EXPECT_EQ_U64(decode_once(encoded,
                              written,
                              GALAY_FALSE,
                              decoded,
                              sizeof(decoded),
                              &frame,
                              &ws_error),
                  GALAY_OK);
    EXPECT_EQ_U64(frame.opcode, GALAY_WS_OPCODE_TEXT);
    EXPECT_EQ_U64(frame.fin, GALAY_FALSE);
    EXPECT_TRUE(memcmp(decoded, part1, sizeof(part1)) == 0);

    memset(encoded, 0, sizeof(encoded));
    memset(decoded, 0, sizeof(decoded));
    EXPECT_EQ_U64(galay_ws_encode_frame(GALAY_WS_OPCODE_CONTINUATION,
                                        part2,
                                        sizeof(part2),
                                        GALAY_TRUE,
                                        NULL,
                                        encoded,
                                        sizeof(encoded),
                                        &written),
                  GALAY_OK);
    EXPECT_EQ_U64(decode_once(encoded,
                              written,
                              GALAY_FALSE,
                              decoded,
                              sizeof(decoded),
                              &frame,
                              &ws_error),
                  GALAY_OK);
    EXPECT_EQ_U64(frame.opcode, GALAY_WS_OPCODE_CONTINUATION);
    EXPECT_EQ_U64(frame.fin, GALAY_TRUE);
    EXPECT_TRUE(memcmp(decoded, part2, sizeof(part2)) == 0);
    return 0;
}

static int test_mask_policy_is_enforced(void)
{
    static const uint8_t payload[] = {'m', 'a', 's', 'k'};
    static const uint8_t mask_key[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t encoded[32] = {0};
    uint8_t decoded[8] = {0};
    size_t written = 0;
    galay_ws_frame_t frame = {0};
    galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;

    EXPECT_EQ_U64(galay_ws_encode_frame(GALAY_WS_OPCODE_TEXT,
                                        payload,
                                        sizeof(payload),
                                        GALAY_TRUE,
                                        NULL,
                                        encoded,
                                        sizeof(encoded),
                                        &written),
                  GALAY_OK);
    EXPECT_EQ_U64(decode_once(encoded,
                              written,
                              GALAY_TRUE,
                              decoded,
                              sizeof(decoded),
                              &frame,
                              &ws_error),
                  GALAY_PROTOCOL_ERROR);
    EXPECT_EQ_U64(ws_error, GALAY_WS_ERROR_MASK_REQUIRED);

    EXPECT_EQ_U64(galay_ws_encode_frame(GALAY_WS_OPCODE_TEXT,
                                        payload,
                                        sizeof(payload),
                                        GALAY_TRUE,
                                        mask_key,
                                        encoded,
                                        sizeof(encoded),
                                        &written),
                  GALAY_OK);
    EXPECT_EQ_U64(decode_once(encoded,
                              written,
                              GALAY_FALSE,
                              decoded,
                              sizeof(decoded),
                              &frame,
                              &ws_error),
                  GALAY_PROTOCOL_ERROR);
    EXPECT_EQ_U64(ws_error, GALAY_WS_ERROR_MASK_UNEXPECTED);
    return 0;
}

static int test_control_frame_validation(void)
{
    uint8_t encoded[256] = {0};
    uint8_t payload[126] = {0};
    uint8_t decoded[126] = {0};
    size_t written = 0;
    galay_ws_frame_t frame = {0};
    galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;

    EXPECT_EQ_U64(galay_ws_encode_frame(GALAY_WS_OPCODE_PING,
                                        payload,
                                        1,
                                        GALAY_FALSE,
                                        NULL,
                                        encoded,
                                        sizeof(encoded),
                                        &written),
                  GALAY_OK);
    EXPECT_EQ_U64(decode_once(encoded,
                              written,
                              GALAY_FALSE,
                              decoded,
                              sizeof(decoded),
                              &frame,
                              &ws_error),
                  GALAY_PROTOCOL_ERROR);
    EXPECT_EQ_U64(ws_error, GALAY_WS_ERROR_CONTROL_FRAGMENTED);

    EXPECT_EQ_U64(galay_ws_encode_frame(GALAY_WS_OPCODE_PING,
                                        payload,
                                        sizeof(payload),
                                        GALAY_TRUE,
                                        NULL,
                                        encoded,
                                        sizeof(encoded),
                                        &written),
                  GALAY_OK);
    EXPECT_EQ_U64(decode_once(encoded,
                              written,
                              GALAY_FALSE,
                              decoded,
                              sizeof(decoded),
                              &frame,
                              &ws_error),
                  GALAY_PROTOCOL_ERROR);
    EXPECT_EQ_U64(ws_error, GALAY_WS_ERROR_CONTROL_TOO_LARGE);

    const uint8_t invalid_close[] = {0x88, 0x01, 0x03};
    EXPECT_EQ_U64(decode_once(invalid_close,
                              sizeof(invalid_close),
                              GALAY_FALSE,
                              decoded,
                              sizeof(decoded),
                              &frame,
                              &ws_error),
                  GALAY_PROTOCOL_ERROR);
    EXPECT_EQ_U64(ws_error, GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH);
    return 0;
}

int main(void)
{
    if (test_data_and_control_round_trip() != 0) return 1;
    if (test_fragment_frames_are_accepted() != 0) return 1;
    if (test_mask_policy_is_enforced() != 0) return 1;
    if (test_control_frame_validation() != 0) return 1;
    return 0;
}
