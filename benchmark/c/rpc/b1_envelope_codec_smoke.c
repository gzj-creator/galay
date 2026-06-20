#include <galay/c/galay-rpc/rpc.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    const char payload[] = "benchmark payload";
    uint8_t encoded[256];
    const int iterations = 10000;

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
    request.payload_len = sizeof(payload) - 1;

    clock_t start = clock();
    size_t total_bytes = 0;
    for (int i = 0; i < iterations; ++i) {
        size_t written = 0;
        if (galay_rpc_encode_request(&request, encoded, sizeof(encoded), &written) != GALAY_OK) {
            return 1;
        }

        galay_rpc_request_t decoded;
        size_t consumed = 0;
        galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_OK;
        if (galay_rpc_decode_request(encoded, written, &decoded, &consumed, &rpc_error) != GALAY_OK ||
            consumed != written ||
            decoded.payload_len != request.payload_len) {
            return 1;
        }
        total_bytes += written;
    }
    clock_t end = clock();

    double seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
    printf("rpc envelope codec smoke: iterations=%d bytes=%zu seconds=%.6f\n",
           iterations,
           total_bytes,
           seconds);
    return 0;
}
