#include <galay/c/galay-redis-c/redis_c.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int parse_iterations(int argc, char** argv)
{
    if (argc < 2) {
        return 10000;
    }
    char* end = NULL;
    long parsed = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || parsed <= 0 || parsed > 10000000) {
        return -1;
    }
    return (int)parsed;
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

int main(int argc, char** argv)
{
    static const char reply[] =
        "*6\r\n"
        ":123\r\n"
        "$5\r\nhello\r\n"
        "-ERR bad\r\n"
        "$-1\r\n"
        "#t\r\n"
        "%1\r\n+count\r\n:3\r\n";
    int iterations = parse_iterations(argc, argv);
    if (iterations <= 0) {
        if (fprintf(stderr, "usage: %s [iterations]\n", argv[0]) < 0) {
            return 2;
        }
        return 1;
    }

    int64_t begin = monotonic_ns();
    if (begin < 0) {
        return 3;
    }
    size_t total_children = 0;
    for (int i = 0; i < iterations; ++i) {
        galay_redis_reply_t* parsed = NULL;
        size_t consumed = 0;
        size_t size = 0;
        if (galay_redis_parse_reply(reply, sizeof(reply) - 1, &parsed, &consumed) != GALAY_OK ||
            consumed != sizeof(reply) - 1 ||
            galay_redis_reply_array_size(parsed, &size) != GALAY_OK) {
            if (parsed != NULL) {
                galay_redis_reply_free(parsed);
            }
            return 4;
        }
        total_children += size;
        galay_redis_reply_free(parsed);
    }
    int64_t end = monotonic_ns();
    if (end < begin) {
        return 5;
    }
    if (printf("c_redis_resp_parse iterations=%d children=%zu elapsed_ns=%lld\n",
               iterations,
               total_children,
               (long long)(end - begin)) < 0) {
        return 6;
    }
    return 0;
}
