#include <galay/c/galay-rpc-c/rpc_c.h>

#include <string.h>

int main(void)
{
    const galay_rpc_error_code_t codes[] = {
        GALAY_RPC_ERROR_OK,
        GALAY_RPC_ERROR_UNKNOWN_ERROR,
        GALAY_RPC_ERROR_SERVICE_NOT_FOUND,
        GALAY_RPC_ERROR_METHOD_NOT_FOUND,
        GALAY_RPC_ERROR_INVALID_REQUEST,
        GALAY_RPC_ERROR_INVALID_RESPONSE,
        GALAY_RPC_ERROR_REQUEST_TIMEOUT,
        GALAY_RPC_ERROR_CONNECTION_CLOSED,
        GALAY_RPC_ERROR_SERIALIZATION_ERROR,
        GALAY_RPC_ERROR_DESERIALIZATION_ERROR,
        GALAY_RPC_ERROR_INTERNAL_ERROR,
        GALAY_RPC_ERROR_CANCELLED,
        GALAY_RPC_ERROR_DEADLINE_EXCEEDED,
        GALAY_RPC_ERROR_RESOURCE_EXHAUSTED,
        GALAY_RPC_ERROR_RATE_LIMITED,
        GALAY_RPC_ERROR_CIRCUIT_OPEN,
        GALAY_RPC_ERROR_UNAUTHENTICATED,
        GALAY_RPC_ERROR_PERMISSION_DENIED,
        GALAY_RPC_ERROR_UNAVAILABLE,
    };
    const size_t count = sizeof(codes) / sizeof(codes[0]);
    for (size_t i = 0; i < count; ++i) {
        const char* text = galay_rpc_get_error(codes[i]);
        if (text == 0 || text[0] == '\0' || strcmp(text, "Unknown") == 0) {
            return (int)i + 1;
        }
    }
    if (strcmp(galay_rpc_get_error((galay_rpc_error_code_t)1000), "Unknown") != 0) {
        return 100;
    }
    return 0;
}
