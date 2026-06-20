#include <galay/c/galay-tracing/tracing.h>

#include <assert.h>
#include <string.h>

static void test_trace_and_span_id_formatting(void)
{
    galay_tracing_trace_id_t trace_id;
    galay_tracing_span_id_t span_id;
    char trace_hex[GALAY_TRACING_TRACE_ID_HEX_LENGTH];
    char span_hex[GALAY_TRACING_SPAN_ID_HEX_LENGTH];
    size_t written = 0;

    assert(galay_tracing_trace_id_generate(&trace_id) == GALAY_OK);
    assert(galay_tracing_trace_id_is_valid(&trace_id) == GALAY_TRUE);
    assert(galay_tracing_trace_id_format(&trace_id, trace_hex, sizeof(trace_hex), &written) == GALAY_OK);
    assert(written == sizeof(trace_hex));
    assert(galay_tracing_trace_id_parse(trace_hex, sizeof(trace_hex), &trace_id) == GALAY_OK);

    assert(galay_tracing_span_id_generate(&span_id) == GALAY_OK);
    assert(galay_tracing_span_id_is_valid(&span_id) == GALAY_TRUE);
    assert(galay_tracing_span_id_format(&span_id, span_hex, sizeof(span_hex), &written) == GALAY_OK);
    assert(written == sizeof(span_hex));
    assert(galay_tracing_span_id_parse(span_hex, sizeof(span_hex), &span_id) == GALAY_OK);

    assert(galay_tracing_trace_id_format(&trace_id, trace_hex, 4, &written) == GALAY_INVALID_ARGUMENT);
    assert(written == GALAY_TRACING_TRACE_ID_HEX_LENGTH);
    assert(galay_tracing_span_id_parse("0000000000000000", 16, &span_id) == GALAY_INVALID_ARGUMENT);
}

static void test_traceparent_round_trip_and_boundaries(void)
{
    const char input[] = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    const char invalid_trace_id[] = "00-00000000000000000000000000000000-00f067aa0ba902b7-01";
    galay_tracing_trace_context_t* context = 0;
    char output[GALAY_TRACING_TRACEPARENT_LENGTH];
    size_t written = 0;
    uint8_t flags = 0;

    assert(galay_tracing_traceparent_parse(input, sizeof(input) - 1, 0, 0, &context) == GALAY_OK);
    assert(context != 0);
    assert(galay_tracing_trace_context_flags(context, &flags) == GALAY_OK);
    assert(flags == 1);
    assert(galay_tracing_traceparent_format(context, output, sizeof(output), &written) == GALAY_OK);
    assert(written == sizeof(output));
    assert(memcmp(output, input, sizeof(output)) == 0);
    assert(galay_tracing_traceparent_format(context, output, 8, &written) == GALAY_INVALID_ARGUMENT);
    assert(written == GALAY_TRACING_TRACEPARENT_LENGTH);
    galay_tracing_trace_context_destroy(&context);
    assert(context == 0);

    assert(galay_tracing_traceparent_parse(invalid_trace_id, sizeof(invalid_trace_id) - 1, 0, 0, &context)
           == GALAY_PROTOCOL_ERROR);
    assert(context == 0);
    assert(galay_tracing_traceparent_parse("bad", 3, 0, 0, &context) == GALAY_PROTOCOL_ERROR);
}

int main(void)
{
    test_trace_and_span_id_formatting();
    test_traceparent_round_trip_and_boundaries();
    return 0;
}
