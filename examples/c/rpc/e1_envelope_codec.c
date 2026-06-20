#include <galay/c/galay-rpc/rpc.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    const char payload[] = "ping";
    galay_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = 1;
    request.call_mode = GALAY_RPC_CALL_UNARY;
    request.end_of_stream = GALAY_TRUE;
    request.service = "echo.Service";
    request.service_len = strlen(request.service);
    request.method = "Ping";
    request.method_len = strlen(request.method);
    request.payload = payload;
    request.payload_len = sizeof(payload) - 1;

    uint8_t encoded[128];
    size_t written = 0;
    if (galay_rpc_encode_request(&request, encoded, sizeof(encoded), &written) != GALAY_OK) {
        return 1;
    }

    galay_rpc_request_t decoded;
    size_t consumed = 0;
    galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_OK;
    if (galay_rpc_decode_request(encoded, written, &decoded, &consumed, &rpc_error) != GALAY_OK) {
        fprintf(stderr, "decode failed: %s\n", galay_rpc_error_string(rpc_error));
        return 1;
    }

    return consumed == written &&
           decoded.request_id == request.request_id &&
           decoded.payload_len == request.payload_len
               ? 0
               : 1;
}
