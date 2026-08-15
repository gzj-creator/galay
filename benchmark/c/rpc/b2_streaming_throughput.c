#include <galay/c/galay-rpc-c/rpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char** argv)
{
    int frames = 10000;
    if (argc > 1) {
        frames = atoi(argv[1]);
    }
    if (frames <= 0) {
        return 1;
    }

    char payload[128];
    memset(payload, 'x', sizeof(payload));
    uint64_t bytes = 0;
    clock_t start = clock();
    for (int i = 0; i < frames; ++i) {
        galay_rpc_response_t response;
        memset(&response, 0, sizeof(response));
        response.request_id = (uint32_t)(i + 1);
        response.call_mode = GALAY_RPC_CALL_BIDI_STREAMING;
        response.end_of_stream = i == frames - 1 ? GALAY_TRUE : GALAY_FALSE;
        response.error_code = GALAY_RPC_ERROR_OK;
        response.payload = payload;
        response.payload_len = sizeof(payload);
        size_t encoded_size = 0;
        if (galay_rpc_response_encoded_size(&response, &encoded_size) != GALAY_OK) {
            return 2;
        }
        uint8_t* buffer = (uint8_t*)malloc(encoded_size);
        if (buffer == NULL) {
            return 3;
        }
        size_t written = 0;
        if (galay_rpc_encode_response(&response, buffer, encoded_size, &written) != GALAY_OK) {
            free(buffer);
            return 4;
        }
        galay_rpc_response_t decoded;
        size_t consumed = 0;
        galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_UNKNOWN_ERROR;
        if (galay_rpc_decode_response(buffer, written, &decoded, &consumed, &rpc_error) != GALAY_OK ||
            consumed != written ||
            rpc_error != GALAY_RPC_ERROR_OK ||
            decoded.payload_len != sizeof(payload)) {
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
    printf("rpc_streaming_throughput_smoke frames=%d bytes=%llu frames_per_sec=%.2f\n",
           frames,
           (unsigned long long)bytes,
           (double)frames / elapsed_sec);
    return 0;
}
