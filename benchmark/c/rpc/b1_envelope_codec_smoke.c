#include <galay/c/galay-rpc/rpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double elapsed_seconds(clock_t start, clock_t end)
{
    double seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
    return seconds > 0.000001 ? seconds : 0.000001;
}

static int run_case(size_t payload_size, int iterations)
{
    char* payload = NULL;
    if (payload_size > 0) {
        payload = (char*)malloc(payload_size);
        if (payload == NULL) {
            return 1;
        }
        memset(payload, 'x', payload_size);
    }

    galay_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = 100;
    request.call_mode = GALAY_RPC_CALL_UNARY;
    request.end_of_stream = GALAY_TRUE;
    request.service = "bench.Service";
    request.service_len = strlen(request.service);
    request.method = "Echo";
    request.method_len = strlen(request.method);
    request.payload = payload;
    request.payload_len = payload_size;

    size_t encoded_size = 0;
    if (galay_rpc_request_encoded_size(&request, &encoded_size) != GALAY_OK) {
        free(payload);
        return 1;
    }

    uint8_t* encoded = (uint8_t*)malloc(encoded_size);
    if (encoded == NULL) {
        free(payload);
        return 1;
    }

    size_t total_encode_bytes = 0;
    clock_t encode_start = clock();
    for (int i = 0; i < iterations; ++i) {
        size_t written = 0;
        if (galay_rpc_encode_request(&request, encoded, encoded_size, &written) != GALAY_OK ||
            written != encoded_size) {
            free(encoded);
            free(payload);
            return 1;
        }
        total_encode_bytes += written;
    }
    clock_t encode_end = clock();

    size_t total_decode_bytes = 0;
    clock_t decode_start = clock();
    for (int i = 0; i < iterations; ++i) {
        galay_rpc_request_t decoded;
        size_t consumed = 0;
        galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_OK;
        if (galay_rpc_decode_request(encoded, encoded_size, &decoded, &consumed, &rpc_error) != GALAY_OK ||
            consumed != encoded_size ||
            rpc_error != GALAY_RPC_ERROR_OK ||
            decoded.payload_len != payload_size) {
            free(encoded);
            free(payload);
            return 1;
        }
        total_decode_bytes += consumed;
    }
    clock_t decode_end = clock();

    const double encode_seconds = elapsed_seconds(encode_start, encode_end);
    const double decode_seconds = elapsed_seconds(decode_start, decode_end);
    printf("rpc envelope codec payload=%zu iterations=%d encoded_size=%zu "
           "encode_per_s=%.2f decode_per_s=%.2f encode_mbps=%.2f decode_mbps=%.2f\n",
           payload_size,
           iterations,
           encoded_size,
           (double)iterations / encode_seconds,
           (double)iterations / decode_seconds,
           ((double)total_encode_bytes / encode_seconds) / 1024.0 / 1024.0,
           ((double)total_decode_bytes / decode_seconds) / 1024.0 / 1024.0);

    free(encoded);
    free(payload);
    return 0;
}

int main(void)
{
    const size_t payload_sizes[] = {0, 128, 4 * 1024, 64 * 1024};
    for (size_t i = 0; i < sizeof(payload_sizes) / sizeof(payload_sizes[0]); ++i) {
        const size_t size = payload_sizes[i];
        const int iterations = size <= 128 ? 20000 : (size <= 4 * 1024 ? 5000 : 1000);
        if (run_case(size, iterations) != 0) {
            return 1;
        }
    }
    return 0;
}
