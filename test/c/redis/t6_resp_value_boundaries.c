#include <galay/c/galay-redis-c/redis.h>

#include <math.h>
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

static int parse_one(const char* raw, galay_redis_reply_t** reply)
{
    size_t consumed = 0;
    size_t raw_len = strlen(raw);
    REQUIRE_STATUS(galay_redis_parse_reply(raw, raw_len, reply, &consumed), GALAY_OK);
    REQUIRE_TRUE(*reply != NULL);
    REQUIRE_TRUE(consumed == raw_len);
    return 0;
}

static int test_resp2_scalars_and_nil(void)
{
    galay_redis_reply_t* reply = NULL;
    const char* text = NULL;
    size_t text_len = 0;
    int64_t integer = 0;

    if (parse_one(":12345\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_INTEGER);
    REQUIRE_STATUS(galay_redis_reply_integer(reply, &integer), GALAY_OK);
    REQUIRE_TRUE(integer == 12345);
    REQUIRE_STATUS(galay_redis_reply_string(reply, &text, &text_len), GALAY_INVALID_ARGUMENT);
    galay_redis_reply_free(reply);

    if (parse_one("$5\r\nhello\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_BULK_STRING);
    REQUIRE_STATUS(galay_redis_reply_string(reply, &text, &text_len), GALAY_OK);
    REQUIRE_TRUE(text_len == 5);
    REQUIRE_TRUE(memcmp(text, "hello", text_len) == 0);
    galay_redis_reply_free(reply);

    if (parse_one("-ERR bad\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_ERROR);
    REQUIRE_STATUS(galay_redis_reply_string(reply, &text, &text_len), GALAY_OK);
    REQUIRE_TRUE(text_len == 7);
    REQUIRE_TRUE(memcmp(text, "ERR bad", text_len) == 0);
    galay_redis_reply_free(reply);

    if (parse_one("$-1\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_NIL);
    REQUIRE_STATUS(galay_redis_reply_string(reply, &text, &text_len), GALAY_INVALID_ARGUMENT);
    galay_redis_reply_free(reply);
    return 0;
}

static int test_arrays_expose_borrowed_children(void)
{
    galay_redis_reply_t* reply = NULL;
    const galay_redis_reply_t* child = NULL;
    const char* text = NULL;
    size_t text_len = 0;
    size_t size = 0;
    int64_t integer = 0;

    if (parse_one("*2\r\n:7\r\n$3\r\ntwo\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_ARRAY);
    REQUIRE_STATUS(galay_redis_reply_array_size(reply, &size), GALAY_OK);
    REQUIRE_TRUE(size == 2);

    REQUIRE_STATUS(galay_redis_reply_array_at(reply, 0, &child), GALAY_OK);
    REQUIRE_TRUE(galay_redis_reply_type(child) == GALAY_REDIS_RESP_INTEGER);
    REQUIRE_STATUS(galay_redis_reply_integer(child, &integer), GALAY_OK);
    REQUIRE_TRUE(integer == 7);

    REQUIRE_STATUS(galay_redis_reply_array_at(reply, 1, &child), GALAY_OK);
    REQUIRE_TRUE(galay_redis_reply_type(child) == GALAY_REDIS_RESP_BULK_STRING);
    REQUIRE_STATUS(galay_redis_reply_string(child, &text, &text_len), GALAY_OK);
    REQUIRE_TRUE(text_len == 3);
    REQUIRE_TRUE(memcmp(text, "two", text_len) == 0);
    REQUIRE_STATUS(galay_redis_reply_array_at(reply, 2, &child), GALAY_NOT_FOUND);

    galay_redis_reply_free(reply);
    return 0;
}

static int test_resp3_scalars_and_aggregates(void)
{
    galay_redis_reply_t* reply = NULL;
    const galay_redis_reply_t* key = NULL;
    const galay_redis_reply_t* value = NULL;
    const char* text = NULL;
    size_t text_len = 0;
    size_t size = 0;
    double double_value = 0.0;
    galay_bool_t bool_value = GALAY_FALSE;
    int64_t integer = 0;

    if (parse_one("#t\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_BOOLEAN);
    REQUIRE_STATUS(galay_redis_reply_boolean(reply, &bool_value), GALAY_OK);
    REQUIRE_TRUE(bool_value == GALAY_TRUE);
    galay_redis_reply_free(reply);

    if (parse_one(",1.25\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_DOUBLE);
    REQUIRE_STATUS(galay_redis_reply_double(reply, &double_value), GALAY_OK);
    REQUIRE_TRUE(fabs(double_value - 1.25) < 0.000001);
    galay_redis_reply_free(reply);

    if (parse_one("%1\r\n+count\r\n:3\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_MAP);
    REQUIRE_STATUS(galay_redis_reply_map_size(reply, &size), GALAY_OK);
    REQUIRE_TRUE(size == 1);
    REQUIRE_STATUS(galay_redis_reply_map_at(reply, 0, &key, &value), GALAY_OK);
    REQUIRE_STATUS(galay_redis_reply_string(key, &text, &text_len), GALAY_OK);
    REQUIRE_TRUE(text_len == 5);
    REQUIRE_TRUE(memcmp(text, "count", text_len) == 0);
    REQUIRE_STATUS(galay_redis_reply_integer(value, &integer), GALAY_OK);
    REQUIRE_TRUE(integer == 3);
    galay_redis_reply_free(reply);

    if (parse_one("~2\r\n+one\r\n+two\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_SET);
    REQUIRE_STATUS(galay_redis_reply_array_size(reply, &size), GALAY_OK);
    REQUIRE_TRUE(size == 2);
    galay_redis_reply_free(reply);

    if (parse_one(">2\r\n+message\r\n$4\r\nbody\r\n", &reply) != 0) {
        return 1;
    }
    REQUIRE_TRUE(galay_redis_reply_type(reply) == GALAY_REDIS_RESP_PUSH);
    REQUIRE_STATUS(galay_redis_reply_array_size(reply, &size), GALAY_OK);
    REQUIRE_TRUE(size == 2);
    REQUIRE_STATUS(galay_redis_reply_array_at(reply, 1, &value), GALAY_OK);
    REQUIRE_STATUS(galay_redis_reply_string(value, &text, &text_len), GALAY_OK);
    REQUIRE_TRUE(text_len == 4);
    REQUIRE_TRUE(memcmp(text, "body", text_len) == 0);
    galay_redis_reply_free(reply);
    return 0;
}

int main(void)
{
    if (test_resp2_scalars_and_nil() != 0) {
        return 1;
    }
    if (test_arrays_expose_borrowed_children() != 0) {
        return 1;
    }
    if (test_resp3_scalars_and_aggregates() != 0) {
        return 1;
    }
    return 0;
}
