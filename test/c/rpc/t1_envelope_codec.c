#include <galay/c/galay-rpc/rpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "require failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1; \
        } \
    } while (0)

#define REQUIRE_STATUS(expr, expected) \
    do { \
        galay_status_t got_status = (expr); \
        if (got_status != (expected)) { \
            fprintf(stderr, "status failed: %s:%d: got %d expected %d\n", \
                    __FILE__, __LINE__, (int)got_status, (int)(expected)); \
            return 1; \
        } \
    } while (0)

static int test_request_round_trip(void)
{
    const char payload[] = "hello";
    galay_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = 42;
    request.call_mode = GALAY_RPC_CALL_UNARY;
    request.end_of_stream = GALAY_TRUE;
    request.service = "echo.Service";
    request.service_len = strlen(request.service);
    request.method = "SayHello";
    request.method_len = strlen(request.method);
    request.payload = payload;
    request.payload_len = sizeof(payload) - 1;

    size_t encoded_size = 0;
    REQUIRE_STATUS(galay_rpc_request_encoded_size(&request, &encoded_size), GALAY_OK);
    REQUIRE_TRUE(encoded_size > GALAY_RPC_HEADER_SIZE);

    uint8_t* encoded = (uint8_t*)malloc(encoded_size);
    REQUIRE_TRUE(encoded != NULL);
    size_t written = 0;
    REQUIRE_STATUS(galay_rpc_encode_request(&request, encoded, encoded_size, &written), GALAY_OK);
    REQUIRE_TRUE(written == encoded_size);

    galay_rpc_request_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    size_t consumed = 0;
    galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_UNKNOWN_ERROR;
    REQUIRE_STATUS(galay_rpc_decode_request(encoded, written, &decoded, &consumed, &rpc_error), GALAY_OK);
    REQUIRE_TRUE(consumed == written);
    REQUIRE_TRUE(rpc_error == GALAY_RPC_ERROR_OK);
    REQUIRE_TRUE(decoded.request_id == request.request_id);
    REQUIRE_TRUE(decoded.call_mode == GALAY_RPC_CALL_UNARY);
    REQUIRE_TRUE(decoded.end_of_stream == GALAY_TRUE);
    REQUIRE_TRUE(decoded.service_len == request.service_len);
    REQUIRE_TRUE(strncmp(decoded.service, request.service, decoded.service_len) == 0);
    REQUIRE_TRUE(decoded.method_len == request.method_len);
    REQUIRE_TRUE(strncmp(decoded.method, request.method, decoded.method_len) == 0);
    REQUIRE_TRUE(decoded.payload_len == request.payload_len);
    REQUIRE_TRUE(memcmp(decoded.payload, request.payload, decoded.payload_len) == 0);

    free(encoded);
    return 0;
}

static int test_response_round_trip_and_status_conversion(void)
{
    const uint8_t payload[] = {1, 2, 3, 4};
    galay_rpc_response_t response;
    memset(&response, 0, sizeof(response));
    response.request_id = 7;
    response.call_mode = GALAY_RPC_CALL_SERVER_STREAMING;
    response.end_of_stream = GALAY_FALSE;
    response.error_code = GALAY_RPC_ERROR_METHOD_NOT_FOUND;
    response.payload = payload;
    response.payload_len = sizeof(payload);

    size_t encoded_size = 0;
    REQUIRE_STATUS(galay_rpc_response_encoded_size(&response, &encoded_size), GALAY_OK);

    uint8_t encoded[128];
    size_t written = 0;
    REQUIRE_STATUS(galay_rpc_encode_response(&response, encoded, sizeof(encoded), &written), GALAY_OK);
    REQUIRE_TRUE(written == encoded_size);

    galay_rpc_response_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    size_t consumed = 0;
    galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_OK;
    REQUIRE_STATUS(galay_rpc_decode_response(encoded, written, &decoded, &consumed, &rpc_error), GALAY_OK);
    REQUIRE_TRUE(consumed == written);
    REQUIRE_TRUE(rpc_error == GALAY_RPC_ERROR_OK);
    REQUIRE_TRUE(decoded.request_id == response.request_id);
    REQUIRE_TRUE(decoded.call_mode == GALAY_RPC_CALL_SERVER_STREAMING);
    REQUIRE_TRUE(decoded.end_of_stream == GALAY_FALSE);
    REQUIRE_TRUE(decoded.error_code == GALAY_RPC_ERROR_METHOD_NOT_FOUND);
    REQUIRE_TRUE(decoded.payload_len == sizeof(payload));
    REQUIRE_TRUE(memcmp(decoded.payload, payload, sizeof(payload)) == 0);
    REQUIRE_STATUS(galay_rpc_error_to_status(decoded.error_code), GALAY_NOT_FOUND);
    REQUIRE_TRUE(strcmp(galay_rpc_error_string(decoded.error_code), "Method not found") == 0);
    return 0;
}

static int test_invalid_method_name_rejected(void)
{
    REQUIRE_TRUE(galay_rpc_name_is_valid("Service1", strlen("Service1")) == GALAY_TRUE);
    REQUIRE_TRUE(galay_rpc_name_is_valid("bad method", strlen("bad method")) == GALAY_FALSE);

    galay_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = 1;
    request.call_mode = GALAY_RPC_CALL_UNARY;
    request.end_of_stream = GALAY_TRUE;
    request.service = "echo";
    request.service_len = strlen(request.service);
    request.method = "bad method";
    request.method_len = strlen(request.method);

    uint8_t out[64];
    size_t written = 99;
    REQUIRE_STATUS(galay_rpc_encode_request(&request, out, sizeof(out), &written), GALAY_INVALID_ARGUMENT);
    REQUIRE_TRUE(written == 0);
    return 0;
}

static int test_null_service_rejected(void)
{
    galay_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = 1;
    request.call_mode = GALAY_RPC_CALL_UNARY;
    request.end_of_stream = GALAY_TRUE;
    request.service = NULL;
    request.service_len = 4;
    request.method = "Call";
    request.method_len = strlen(request.method);

    size_t encoded_size = 0;
    REQUIRE_STATUS(galay_rpc_request_encoded_size(&request, &encoded_size), GALAY_INVALID_ARGUMENT);
    return 0;
}

static int test_malformed_payload_rejected(void)
{
    uint8_t raw[GALAY_RPC_HEADER_SIZE + 3] = {
        0x47, 0x52, 0x50, 0x43,
        0x01, 0x01, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x03,
        0x00, 0x04, 'a'
    };
    galay_rpc_request_t decoded;
    size_t consumed = 123;
    galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_OK;

    REQUIRE_STATUS(galay_rpc_decode_request(raw, sizeof(raw), &decoded, &consumed, &rpc_error),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(consumed == 0);
    REQUIRE_TRUE(rpc_error == GALAY_RPC_ERROR_DESERIALIZATION_ERROR);
    return 0;
}

static int test_decode_truncation_reports_protocol_error(void)
{
    galay_rpc_request_t decoded;
    size_t consumed = 123;
    galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_OK;
    const uint8_t truncated_header[] = {0x47, 0x52, 0x50};

    REQUIRE_STATUS(galay_rpc_decode_request(truncated_header,
                                            sizeof(truncated_header),
                                            &decoded,
                                            &consumed,
                                            &rpc_error),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(consumed == 0);
    REQUIRE_TRUE(rpc_error == GALAY_RPC_ERROR_INVALID_REQUEST);
    return 0;
}

int main(void)
{
    if (test_request_round_trip() != 0) return 1;
    if (test_response_round_trip_and_status_conversion() != 0) return 1;
    if (test_invalid_method_name_rejected() != 0) return 1;
    if (test_null_service_rejected() != 0) return 1;
    if (test_malformed_payload_rejected() != 0) return 1;
    if (test_decode_truncation_reports_protocol_error() != 0) return 1;
    return 0;
}
