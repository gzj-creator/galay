#ifndef GALAY_C_RPC_RPC_H
#define GALAY_C_RPC_RPC_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALAY_RPC_HEADER_SIZE 16u

typedef enum galay_rpc_call_mode_t {
    GALAY_RPC_CALL_UNARY = 1,
    GALAY_RPC_CALL_CLIENT_STREAMING = 2,
    GALAY_RPC_CALL_SERVER_STREAMING = 3,
    GALAY_RPC_CALL_BIDI_STREAMING = 4
} galay_rpc_call_mode_t;

typedef enum galay_rpc_error_code_t {
    GALAY_RPC_ERROR_OK = 0,
    GALAY_RPC_ERROR_INVALID_REQUEST = 1,
    GALAY_RPC_ERROR_INVALID_RESPONSE = 2,
    GALAY_RPC_ERROR_METHOD_NOT_FOUND = 3,
    GALAY_RPC_ERROR_DESERIALIZATION_ERROR = 4,
    GALAY_RPC_ERROR_UNKNOWN_ERROR = 10,
    GALAY_RPC_ERROR_CANCELLED = 11,
    GALAY_RPC_ERROR_DEADLINE_EXCEEDED = 12,
    GALAY_RPC_ERROR_RESOURCE_EXHAUSTED = 13,
    GALAY_RPC_ERROR_RATE_LIMITED = 14,
    GALAY_RPC_ERROR_CIRCUIT_OPEN = 15,
    GALAY_RPC_ERROR_UNAUTHENTICATED = 16,
    GALAY_RPC_ERROR_PERMISSION_DENIED = 17,
    GALAY_RPC_ERROR_UNAVAILABLE = 18
} galay_rpc_error_code_t;

typedef struct galay_rpc_request_t {
    uint32_t request_id;
    galay_rpc_call_mode_t call_mode;
    galay_bool_t end_of_stream;
    const char* service;
    size_t service_len;
    const char* method;
    size_t method_len;
    const void* payload;
    size_t payload_len;
} galay_rpc_request_t;

typedef struct galay_rpc_response_t {
    uint32_t request_id;
    galay_rpc_call_mode_t call_mode;
    galay_bool_t end_of_stream;
    galay_rpc_error_code_t error_code;
    const void* payload;
    size_t payload_len;
} galay_rpc_response_t;

const char* galay_rpc_error_string(galay_rpc_error_code_t code);
galay_status_t galay_rpc_error_to_status(galay_rpc_error_code_t code);
galay_bool_t galay_rpc_name_is_valid(const char* name, size_t name_len);
galay_status_t galay_rpc_request_encoded_size(const galay_rpc_request_t* request, size_t* size);
galay_status_t galay_rpc_response_encoded_size(const galay_rpc_response_t* response, size_t* size);
galay_status_t galay_rpc_encode_request(const galay_rpc_request_t* request, uint8_t* out,
                                        size_t out_len, size_t* written);
galay_status_t galay_rpc_encode_response(const galay_rpc_response_t* response, uint8_t* out,
                                         size_t out_len, size_t* written);
galay_status_t galay_rpc_decode_request(const uint8_t* data, size_t data_len,
                                        galay_rpc_request_t* out, size_t* consumed,
                                        galay_rpc_error_code_t* rpc_error);
galay_status_t galay_rpc_decode_response(const uint8_t* data, size_t data_len,
                                         galay_rpc_response_t* out, size_t* consumed,
                                         galay_rpc_error_code_t* rpc_error);

#ifdef __cplusplus
}
#endif

#endif
