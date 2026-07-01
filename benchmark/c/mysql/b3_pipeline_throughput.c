#include <galay/c/galay-mysql-c/mysql_c.h>

#include <stdio.h>
#include <time.h>

static long long elapsed_ns(struct timespec start, struct timespec end)
{
    return (long long)(end.tv_sec - start.tv_sec) * 1000000000LL +
        (long long)(end.tv_nsec - start.tv_nsec);
}

int main(void)
{
    const int iterations = 10000;
    struct timespec start = {0};
    struct timespec end = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return 1;
    }
    for (int i = 0; i < iterations; ++i) {
        galay_mysql_pipeline_t* pipeline = NULL;
        if (galay_mysql_pipeline_create(&pipeline) != GALAY_OK ||
            galay_mysql_pipeline_append_query(pipeline, "SELECT 1") != GALAY_OK ||
            galay_mysql_pipeline_append_query(pipeline, "SELECT 2") != GALAY_OK ||
            galay_mysql_pipeline_append_query(pipeline, "SELECT 3") != GALAY_OK) {
            galay_mysql_pipeline_destroy(pipeline);
            return 2;
        }
        galay_mysql_pipeline_destroy(pipeline);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        return 3;
    }
    if (printf("c_mysql_pipeline_builder iterations=%d elapsed_ns=%lld\n",
               iterations,
               elapsed_ns(start, end)) < 0) {
        return 4;
    }
    return 0;
}
