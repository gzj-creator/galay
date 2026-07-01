#include <galay/c/galay-redis-c/redis_c.h>

#include <stdio.h>
#include <string.h>

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "require failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1; \
        } \
    } while (0)

#define REQUIRE_STATUS(expr, expected) \
    do { \
        galay_status_t got_status = (expr); \
        if (got_status != (expected)) { \
            fprintf(stderr, "status failed: %s:%d: got %d expected %d\n", \
                    __FILE__, __LINE__, (int)got_status, (int)(expected)); \
            return 1; \
        } \
    } while (0)

static int test_command_builder_encodes_bulk_array(void)
{
    galay_redis_command_builder_t* builder = NULL;
    const char* args[] = {"key", "value"};
    const char* encoded = NULL;
    size_t encoded_len = 0;
    const char expected[] = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";

    REQUIRE_STATUS(galay_redis_command_builder_create(&builder), GALAY_OK);
    REQUIRE_STATUS(galay_redis_command_builder_build(builder,
                                                     "SET",
                                                     args,
                                                     NULL,
                                                     2,
                                                     &encoded,
                                                     &encoded_len),
                   GALAY_OK);
    REQUIRE_TRUE(encoded != NULL);
    REQUIRE_TRUE(encoded_len == sizeof(expected) - 1);
    REQUIRE_TRUE(memcmp(encoded, expected, encoded_len) == 0);

    galay_redis_command_builder_destroy(builder);
    return 0;
}

static int test_null_command_is_rejected(void)
{
    galay_redis_command_builder_t* builder = NULL;
    const char* encoded = NULL;
    size_t encoded_len = 0;

    REQUIRE_STATUS(galay_redis_command_builder_create(&builder), GALAY_OK);
    REQUIRE_STATUS(galay_redis_command_builder_build(builder,
                                                     NULL,
                                                     NULL,
                                                     NULL,
                                                     0,
                                                     &encoded,
                                                     &encoded_len),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_TRUE(encoded == NULL);
    REQUIRE_TRUE(encoded_len == 0);

    galay_redis_command_builder_destroy(builder);
    return 0;
}

static int test_parser_reads_simple_string(void)
{
    const char raw[] = "+OK\r\n";
    galay_redis_reply_t* reply = NULL;
    size_t consumed = 0;
    const char* value = NULL;
    size_t value_len = 0;

    REQUIRE_STATUS(galay_redis_parse_reply(raw, sizeof(raw) - 1, &reply, &consumed), GALAY_OK);
    REQUIRE_TRUE(reply != NULL);
    REQUIRE_TRUE(consumed == sizeof(raw) - 1);
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_SIMPLE_STRING);
    REQUIRE_STATUS(galay_redis_reply_string(reply, &value, &value_len), GALAY_OK);
    REQUIRE_TRUE(value_len == 2);
    REQUIRE_TRUE(strncmp(value, "OK", value_len) == 0);

    galay_redis_reply_destroy(reply);
    return 0;
}

static int test_parser_rejects_malformed_resp(void)
{
    const char raw[] = "$5\r\nabc\r\n";
    galay_redis_reply_t* reply = NULL;
    size_t consumed = 99;

    REQUIRE_STATUS(galay_redis_parse_reply(raw, sizeof(raw) - 1, &reply, &consumed),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(reply == NULL);
    REQUIRE_TRUE(consumed == 0);
    return 0;
}

static int test_parser_accepts_empty_array(void)
{
    const char raw[] = "*0\r\n";
    galay_redis_reply_t* reply = NULL;
    size_t consumed = 0;
    size_t size = 99;

    REQUIRE_STATUS(galay_redis_parse_reply(raw, sizeof(raw) - 1, &reply, &consumed), GALAY_OK);
    REQUIRE_TRUE(reply != NULL);
    REQUIRE_TRUE(consumed == sizeof(raw) - 1);
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_ARRAY);
    REQUIRE_STATUS(galay_redis_reply_array_size(reply, &size), GALAY_OK);
    REQUIRE_TRUE(size == 0);

    galay_redis_reply_destroy(reply);
    return 0;
}

int main(void)
{
    if (test_command_builder_encodes_bulk_array() != 0) {
        return 1;
    }
    if (test_null_command_is_rejected() != 0) {
        return 1;
    }
    if (test_parser_reads_simple_string() != 0) {
        return 1;
    }
    if (test_parser_rejects_malformed_resp() != 0) {
        return 1;
    }
    if (test_parser_accepts_empty_array() != 0) {
        return 1;
    }
    return 0;
}
