#include <galay/c/galay-tracing-c/tracing.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    const char traceparent[] = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    const char path[] = "/tmp/galay-c-tracing-example.jsonl";
    galay_tracing_trace_context_t* context = NULL;
    galay_tracing_provider_t* provider = NULL;
    galay_tracing_tracer_t* tracer = NULL;
    galay_tracing_span_t* span = NULL;
    galay_tracing_attribute_t attribute = {0};

    attribute.name = "component";
    attribute.name_len = strlen(attribute.name);
    attribute.value = "example";
    attribute.value_len = strlen(attribute.value);
    attribute.type = GALAY_TRACING_ATTRIBUTE_STRING;

    if (galay_tracing_trace_context_extract(traceparent, sizeof(traceparent) - 1, NULL, 0, &context) != GALAY_OK ||
        galay_tracing_provider_create(&provider) != GALAY_OK ||
        galay_tracing_provider_set_file_exporter(provider, path, strlen(path)) != GALAY_OK ||
        galay_tracing_tracer_create(provider, "example", strlen("example"), &tracer) != GALAY_OK ||
        galay_tracing_tracer_start_span(tracer, "file-export", strlen("file-export"), context, &span) != GALAY_OK ||
        galay_tracing_span_set_attribute(span, &attribute) != GALAY_OK ||
        galay_tracing_span_add_event(span, "write", strlen("write"), &attribute, 1) != GALAY_OK ||
        galay_tracing_span_end(span) != GALAY_OK ||
        galay_tracing_provider_force_flush(provider, 1000) != GALAY_OK ||
        galay_tracing_provider_shutdown(provider, 1000) != GALAY_OK) {
        return 1;
    }

    printf("wrote span to %s\n", path);
    galay_tracing_span_destroy(&span);
    galay_tracing_tracer_destroy(&tracer);
    galay_tracing_provider_destroy(&provider);
    galay_tracing_trace_context_destroy(&context);
    return 0;
}
