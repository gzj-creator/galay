#include <galay/c/galay-rpc-c/rpc_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char** argv)
{
    int iterations = 10000;
    if (argc > 1) {
        iterations = atoi(argv[1]);
    }
    if (iterations <= 0) {
        return 1;
    }

    const char payload[] = "payload";
    const char service[] = "BenchService";
    const char method[] = "Echo";
    size_t encoded_size = 0;
    uint64_t bytes = 0;
    clock_t start = clock();
    for (int i = 0; i < iterations; ++i) {
        galay_rpc_request_t request;
        memset(&request, 0, sizeof(request));
        request.request_id = (uint32_t)(i + 1);
        request.call_mode = GALAY_RPC_CALL_UNARY;
        request.end_of_stream = GALAY_TRUE;
        request.service = service;
        request.service_len = strlen(service);
        request.method = method;
        request.method_len = strlen(method);
        request.payload = payload;
        request.payload_len = sizeof(payload) - 1;
        if (galay_rpc_request_encoded_size(&request, &encoded_size) != GALAY_OK) {
            return 2;
        }
        uint8_t* buffer = (uint8_t*)malloc(encoded_size);
        if (buffer == NULL) {
            return 3;
        }
        size_t written = 0;
        if (galay_rpc_encode_request(&request, buffer, encoded_size, &written) != GALAY_OK) {
            free(buffer);
            return 4;
        }
        galay_rpc_request_t decoded;
        size_t consumed = 0;
        galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_UNKNOWN_ERROR;
        if (galay_rpc_decode_request(buffer, written, &decoded, &consumed, &rpc_error) != GALAY_OK ||
            consumed != written ||
            rpc_error != GALAY_RPC_ERROR_OK) {
            free(buffer);
            return 5;
        }
        bytes += written;
        free(buffer);
    }
    clock_t end = clock();
    double elapsed_sec = (double)(end - start) / (double)CLOCKS_PER_SEC;
    if (elapsed_sec <= 0.0) {
        elapsed_sec = 0.000001;
    }
    printf("rpc_unary_concurrency_smoke iterations=%d bytes=%llu ops_per_sec=%.2f\n",
           iterations,
           (unsigned long long)bytes,
           (double)iterations / elapsed_sec);
    return 0;
}
