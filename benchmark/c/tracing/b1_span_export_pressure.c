#include <galay/c/galay-tracing-c/tracing_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { kSpanCount = 256 };

int main(void)
{
    const char traceparent[] = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    char path[] = "/tmp/galay-c-tracing-bench-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0 || close(fd) != 0) {
        return 1;
    }

    galay_tracing_trace_context_t* context = NULL;
    galay_tracing_provider_t* provider = NULL;
    galay_tracing_tracer_t* tracer = NULL;
    galay_tracing_attribute_t attribute = {0};
    attribute.name = "batch";
    attribute.name_len = strlen(attribute.name);
    attribute.type = GALAY_TRACING_ATTRIBUTE_INT64;

    if (galay_tracing_trace_context_extract(traceparent, sizeof(traceparent) - 1, NULL, 0, &context) != GALAY_OK ||
        galay_tracing_provider_create(&provider) != GALAY_OK ||
        galay_tracing_provider_set_file_exporter(provider, path, strlen(path)) != GALAY_OK ||
        galay_tracing_tracer_create(provider, "bench", strlen("bench"), &tracer) != GALAY_OK) {
        return 2;
    }

    for (int i = 0; i < kSpanCount; ++i) {
        galay_tracing_span_t* span = NULL;
        attribute.int64_value = i;
        if (galay_tracing_tracer_start_span(tracer, "pressure", strlen("pressure"), context, &span) != GALAY_OK ||
            galay_tracing_span_set_attribute(span, &attribute) != GALAY_OK ||
            galay_tracing_span_end(span) != GALAY_OK) {
            return 3;
        }
        galay_tracing_span_destroy(&span);
    }
    if (galay_tracing_provider_force_flush(provider, 1000) != GALAY_OK ||
        galay_tracing_provider_shutdown(provider, 1000) != GALAY_OK) {
        return 4;
    }

    printf("exported_spans=%d path=%s\n", kSpanCount, path);
    galay_tracing_tracer_destroy(&tracer);
    galay_tracing_provider_destroy(&provider);
    galay_tracing_trace_context_destroy(&context);
    if (unlink(path) != 0) {
        return 5;
    }
    return 0;
}
