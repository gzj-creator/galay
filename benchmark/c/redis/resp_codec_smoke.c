#include <galay/c/galay-redis/redis.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    enum { iterations = 100000 };
    galay_redis_command_builder_t* builder = NULL;
    const char* args[] = {"key"};
    const char raw_reply[] = "+OK\r\n";

    if (galay_redis_command_builder_create(&builder) != GALAY_OK) {
        return 1;
    }

    const clock_t start = clock();
    for (int i = 0; i < iterations; ++i) {
        const char* encoded = NULL;
        size_t encoded_len = 0;
        galay_redis_reply_t* reply = NULL;
        size_t consumed = 0;
        const char* value = NULL;
        size_t value_len = 0;

        if (galay_redis_command_builder_build(builder,
                                              "GET",
                                              args,
                                              NULL,
                                              1,
                                              &encoded,
                                              &encoded_len) != GALAY_OK) {
            galay_redis_command_builder_destroy(builder);
            return 1;
        }
        if (encoded == NULL || encoded_len == 0) {
            galay_redis_command_builder_destroy(builder);
            return 1;
        }
        if (galay_redis_parse_reply(raw_reply, sizeof(raw_reply) - 1, &reply, &consumed) != GALAY_OK) {
            galay_redis_command_builder_destroy(builder);
            return 1;
        }
        if (galay_redis_reply_string(reply, &value, &value_len) != GALAY_OK ||
            consumed != sizeof(raw_reply) - 1 ||
            value_len != 2 ||
            strncmp(value, "OK", value_len) != 0) {
            galay_redis_reply_destroy(reply);
            galay_redis_command_builder_destroy(builder);
            return 1;
        }
        galay_redis_reply_destroy(reply);
    }
    const clock_t end = clock();

    galay_redis_command_builder_destroy(builder);

    const double seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
    const double ops = seconds > 0.0 ? (double)iterations / seconds : 0.0;
    printf("c.redis.resp_codec iterations=%d seconds=%.6f ops_per_sec=%.2f\n",
           iterations,
           seconds,
           ops);
    return 0;
}
