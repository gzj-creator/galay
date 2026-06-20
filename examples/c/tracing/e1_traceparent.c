#include <galay/c/galay-tracing/tracing.h>

#include <stdio.h>

int main(void)
{
    const char input[] = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    galay_tracing_trace_context_t* context = 0;
    char output[GALAY_TRACING_TRACEPARENT_LENGTH];
    size_t output_len = 0;

    if (galay_tracing_traceparent_parse(input, sizeof(input) - 1, 0, 0, &context) != GALAY_OK) {
        return 1;
    }
    if (galay_tracing_traceparent_format(context, output, sizeof(output), &output_len) != GALAY_OK) {
        galay_tracing_trace_context_destroy(&context);
        return 1;
    }

    printf("traceparent length: %zu\n", output_len);
    galay_tracing_trace_context_destroy(&context);
    return 0;
}
