#include <galay/c/galay-tracing-c/tracing_c.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int file_contains(const char* path, const char* needle)
{
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    char buffer[2048];
    const size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
    const int close_result = fclose(file);
    if (close_result != 0) {
        return 0;
    }
    buffer[n] = '\0';
    return strstr(buffer, needle) != NULL;
}

int main(void)
{
    const char traceparent[] = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    const char tracestate[] = "vendor=value";
    galay_tracing_trace_context_t* context = NULL;
    galay_tracing_trace_context_t* extracted = NULL;
    char out_traceparent[GALAY_TRACING_TRACEPARENT_LENGTH];
    char out_tracestate[64];
    size_t written_traceparent = 0;
    size_t written_tracestate = 0;
    galay_tracing_sampler_t* sampler = NULL;
    galay_bool_t sampled = GALAY_FALSE;
    char path[] = "/tmp/galay-c-tracing-log-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    assert(galay_tracing_trace_context_extract(
        traceparent, sizeof(traceparent) - 1, tracestate, sizeof(tracestate) - 1, &context) == GALAY_OK);
    assert(galay_tracing_trace_context_inject(context,
                                              out_traceparent,
                                              sizeof(out_traceparent),
                                              &written_traceparent,
                                              out_tracestate,
                                              sizeof(out_tracestate),
                                              &written_tracestate) == GALAY_OK);
    assert(written_traceparent == GALAY_TRACING_TRACEPARENT_LENGTH);
    assert(memcmp(out_traceparent, traceparent, sizeof(out_traceparent)) == 0);
    assert(written_tracestate == sizeof(tracestate) - 1);
    assert(memcmp(out_tracestate, tracestate, written_tracestate) == 0);
    assert(galay_tracing_trace_context_extract(
        out_traceparent, written_traceparent, out_tracestate, written_tracestate, &extracted) == GALAY_OK);

    assert(galay_tracing_sampler_create(GALAY_TRACING_SAMPLER_ALWAYS_OFF, 0.0, &sampler) == GALAY_OK);
    assert(galay_tracing_sampler_should_sample(sampler, context, &sampled) == GALAY_OK);
    assert(sampled == GALAY_FALSE);
    galay_tracing_sampler_destroy(&sampler);
    assert(galay_tracing_sampler_create(GALAY_TRACING_SAMPLER_ALWAYS_ON, 1.0, &sampler) == GALAY_OK);
    assert(galay_tracing_sampler_should_sample(sampler, context, &sampled) == GALAY_OK);
    assert(sampled == GALAY_TRUE);

    galay_tracing_logger_t* logger = NULL;
    assert(galay_tracing_logger_create_file(path, strlen(path), GALAY_TRACING_LOG_INFO, &logger) == GALAY_OK);
    assert(galay_tracing_logger_log(logger,
                                    GALAY_TRACING_LOG_INFO,
                                    context,
                                    "hello trace",
                                    strlen("hello trace")) == GALAY_OK);
    galay_tracing_logger_destroy(&logger);
    assert(file_contains(path, "hello trace"));
    assert(file_contains(path, "4bf92f3577b34da6a3ce929d0e0e4736"));

    galay_tracing_sampler_destroy(&sampler);
    galay_tracing_trace_context_destroy(&context);
    galay_tracing_trace_context_destroy(&extracted);
    assert(unlink(path) == 0);
    return 0;
}
