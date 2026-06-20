#include <galay/c/galay-ssl/ssl.h>

#include <stdio.h>
#include <time.h>

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    const time_t sec = end.tv_sec - start.tv_sec;
    const long nsec = end.tv_nsec - start.tv_nsec;
    return (double)sec + (double)nsec / 1000000000.0;
}

int main(void)
{
    enum { iterations = 1000 };
    struct timespec start;
    struct timespec end;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return 1;
    }

    for (int i = 0; i < iterations; ++i) {
        galay_ssl_context_t* context = NULL;
        galay_status_t status =
            galay_ssl_context_create(GALAY_SSL_METHOD_TLS_CLIENT, &context);
        if (status != GALAY_OK) {
            fprintf(stderr, "create failed at %d: %s\n", i, galay_status_string(status));
            return 1;
        }
        status = galay_ssl_context_set_verify_mode(context, GALAY_SSL_VERIFY_PEER);
        galay_ssl_context_destroy(context);
        if (status != GALAY_OK) {
            fprintf(stderr, "set verify mode failed at %d: %s\n", i, galay_status_string(status));
            return 1;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        return 1;
    }

    printf("ssl context lifecycle iterations=%d elapsed_seconds=%.6f\n",
           iterations,
           elapsed_seconds(start, end));
    return 0;
}
