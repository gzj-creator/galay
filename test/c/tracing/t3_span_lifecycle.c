#include <galay/c/galay-tracing-c/tracing.h>

#include <assert.h>
#include <string.h>

static galay_tracing_trace_context_t* make_context(void)
{
    const char input[] = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    galay_tracing_trace_context_t* context = 0;
    assert(galay_tracing_traceparent_parse(input, sizeof(input) - 1, 0, 0, &context) == GALAY_OK);
    return context;
}

static void test_span_lifecycle(void)
{
    galay_tracing_trace_context_t* context = make_context();
    galay_tracing_provider_t* provider = 0;
    galay_tracing_tracer_t* tracer = 0;
    galay_tracing_span_t* span = 0;

    assert(galay_tracing_provider_create(&provider) == GALAY_OK);
    assert(provider != 0);
    assert(galay_tracing_tracer_create(provider, "c-test", strlen("c-test"), &tracer) == GALAY_OK);
    assert(tracer != 0);
    assert(galay_tracing_tracer_start_span(tracer, "operation", strlen("operation"), context, &span) == GALAY_OK);
    assert(span != 0);
    assert(galay_tracing_span_add_event(span, "event", strlen("event"), 0, 0) == GALAY_OK);
    assert(galay_tracing_span_end(span) == GALAY_OK);
    assert(galay_tracing_span_end(span) == GALAY_INVALID_ARGUMENT);

    galay_tracing_span_destroy(&span);
    assert(span == 0);
    galay_tracing_span_destroy(&span);
    galay_tracing_tracer_destroy(&tracer);
    assert(tracer == 0);
    galay_tracing_provider_destroy(&provider);
    assert(provider == 0);
    galay_tracing_trace_context_destroy(&context);
}

static void test_span_boundaries(void)
{
    galay_tracing_trace_context_t* context = make_context();
    galay_tracing_provider_t* provider = 0;
    galay_tracing_tracer_t* tracer = 0;
    galay_tracing_span_t* span = 0;
    char too_long[GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH + 1];
    galay_tracing_attribute_t attr;

    memset(too_long, 'x', sizeof(too_long));
    attr.name = "too_long";
    attr.name_len = strlen(attr.name);
    attr.value = too_long;
    attr.value_len = sizeof(too_long);

    assert(galay_tracing_span_end(0) == GALAY_INVALID_ARGUMENT);
    assert(galay_tracing_provider_create(&provider) == GALAY_OK);
    assert(galay_tracing_tracer_create(provider, "c-test", strlen("c-test"), &tracer) == GALAY_OK);
    assert(galay_tracing_tracer_start_span(tracer, "operation", strlen("operation"), context, &span) == GALAY_OK);
    assert(galay_tracing_span_add_event(0, "event", strlen("event"), 0, 0) == GALAY_INVALID_ARGUMENT);
    assert(galay_tracing_span_add_event(span, "event", strlen("event"), &attr, 1) == GALAY_INVALID_ARGUMENT);

    galay_tracing_span_destroy(&span);
    galay_tracing_tracer_destroy(&tracer);
    galay_tracing_provider_destroy(&provider);
    galay_tracing_trace_context_destroy(&context);
}

int main(void)
{
    test_span_lifecycle();
    test_span_boundaries();
    return 0;
}
