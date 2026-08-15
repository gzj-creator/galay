#include <galay/c/galay-tracing-c/tracing.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static galay_tracing_trace_context_t* make_context(void)
{
    const char input[] = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    galay_tracing_trace_context_t* context = NULL;
    assert(galay_tracing_traceparent_parse(input, sizeof(input) - 1, NULL, 0, &context) == GALAY_OK);
    return context;
}

static int file_contains(const char* path, const char* needle)
{
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    char buffer[4096];
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
    char path[] = "/tmp/galay-c-tracing-span-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    galay_tracing_trace_context_t* context = make_context();
    galay_tracing_provider_t* provider = NULL;
    galay_tracing_tracer_t* tracer = NULL;
    galay_tracing_span_t* span = NULL;
    size_t count = 0;
    galay_tracing_span_status_code_t status = GALAY_TRACING_SPAN_STATUS_UNSET;

    galay_tracing_attribute_t attr = {0};
    attr.name = "http.method";
    attr.name_len = strlen(attr.name);
    attr.value = "GET";
    attr.value_len = strlen(attr.value);
    attr.type = GALAY_TRACING_ATTRIBUTE_STRING;

    galay_tracing_attribute_t link_attr = {0};
    link_attr.name = "link.kind";
    link_attr.name_len = strlen(link_attr.name);
    link_attr.value = "causal";
    link_attr.value_len = strlen(link_attr.value);
    link_attr.type = GALAY_TRACING_ATTRIBUTE_STRING;

    assert(galay_tracing_provider_create(&provider) == GALAY_OK);
    assert(galay_tracing_provider_set_file_exporter(provider, path, strlen(path)) == GALAY_OK);
    assert(galay_tracing_tracer_create(provider, "c-export", strlen("c-export"), &tracer) == GALAY_OK);
    assert(galay_tracing_tracer_start_span(tracer, "operation", strlen("operation"), context, &span) == GALAY_OK);
    assert(galay_tracing_span_set_attribute(span, &attr) == GALAY_OK);
    assert(galay_tracing_span_set_status(span, GALAY_TRACING_SPAN_STATUS_ERROR, "boom", strlen("boom")) == GALAY_OK);
    assert(galay_tracing_span_add_event(span, "dispatch", strlen("dispatch"), &attr, 1) == GALAY_OK);
    assert(galay_tracing_span_add_link(span, context, &link_attr, 1) == GALAY_OK);
    assert(galay_tracing_span_attribute_count(span, &count) == GALAY_OK && count == 1);
    assert(galay_tracing_span_event_count(span, &count) == GALAY_OK && count == 1);
    assert(galay_tracing_span_link_count(span, &count) == GALAY_OK && count == 1);
    assert(galay_tracing_span_status(span, &status) == GALAY_OK && status == GALAY_TRACING_SPAN_STATUS_ERROR);
    assert(galay_tracing_span_end(span) == GALAY_OK);
    assert(galay_tracing_provider_force_flush(provider, 1000) == GALAY_OK);
    assert(galay_tracing_provider_shutdown(provider, 1000) == GALAY_OK);

    assert(file_contains(path, "\"name\":\"operation\""));
    assert(file_contains(path, "\"attributes\""));
    assert(file_contains(path, "\"events\""));
    assert(file_contains(path, "\"links\""));
    assert(file_contains(path, "\"status\""));

    galay_tracing_span_destroy(&span);
    galay_tracing_tracer_destroy(&tracer);
    galay_tracing_provider_destroy(&provider);
    galay_tracing_trace_context_destroy(&context);
    assert(unlink(path) == 0);
    return 0;
}
