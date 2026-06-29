#include <galay/c/galay-mysql-c/mysql.h>

#include <stdio.h>
#include <time.h>

static long long elapsed_ns(struct timespec start, struct timespec end)
{
    return (long long)(end.tv_sec - start.tv_sec) * 1000000000LL +
        (long long)(end.tv_nsec - start.tv_nsec);
}

int main(void)
{
    const int iterations = 5000;
    struct timespec start = {0};
    struct timespec end = {0};
    galay_mysql_config_t* config = NULL;

    if (galay_mysql_config_create(&config) != GALAY_OK) {
        return 1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        galay_mysql_config_destroy(config);
        return 2;
    }
    for (int i = 0; i < iterations; ++i) {
        galay_mysql_pool_t* pool = NULL;
        if (galay_mysql_pool_create(config, 4, &pool) != GALAY_OK) {
            galay_mysql_config_destroy(config);
            return 3;
        }
        galay_mysql_pool_destroy(pool);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        galay_mysql_config_destroy(config);
        return 4;
    }
    galay_mysql_config_destroy(config);
    if (printf("c_mysql_pool_create_destroy iterations=%d elapsed_ns=%lld\n",
               iterations,
               elapsed_ns(start, end)) < 0) {
        return 5;
    }
    return 0;
}
