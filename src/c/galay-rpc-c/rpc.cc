#include <galay/c/galay-rpc-c/rpc.h>

#include <cctype>
#include <cstring>

static bool valid_mode(galay_rpc_call_mode_t mode)
{
    return mode == GALAY_RPC_CALL_UNARY || mode == GALAY_RPC_CALL_CLIENT_STREAMING ||
        mode == GALAY_RPC_CALL_SERVER_STREAMING || mode == GALAY_RPC_CALL_BIDI_STREAMING;
}

static void put16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    out[1] = static_cast<uint8_t>(value & 0xFFU);
}

static void put32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
    out[1] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    out[2] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    out[3] = static_cast<uint8_t>(value & 0xFFU);
}

static uint16_t get16(const uint8_t* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) | data[1]);
}

static uint32_t get32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24U) |
        (static_cast<uint32_t>(data[1]) << 16U) |
        (static_cast<uint32_t>(data[2]) << 8U) | data[3];
}

static galay_status_t validate_request(const galay_rpc_request_t* request, size_t* size)
{
    if (size != nullptr) {
        *size = 0;
    }
    if (request == nullptr || size == nullptr || !valid_mode(request->call_mode) ||
        !galay_rpc_name_is_valid(request->service, request->service_len) ||
        !galay_rpc_name_is_valid(request->method, request->method_len) ||
        request->service_len > 65535 || request->method_len > 65535 ||
        (request->payload == nullptr && request->payload_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    *size = GALAY_RPC_HEADER_SIZE + 4 + request->service_len + request->method_len +
        request->payload_len;
    return GALAY_OK;
}

extern "C" {

const char* galay_rpc_error_string(galay_rpc_error_code_t code)
{
    switch (code) {
        case GALAY_RPC_ERROR_OK:
            return "OK";
        case GALAY_RPC_ERROR_INVALID_REQUEST:
            return "Invalid request";
        case GALAY_RPC_ERROR_INVALID_RESPONSE:
            return "Invalid response";
        case GALAY_RPC_ERROR_METHOD_NOT_FOUND:
            return "Method not found";
        case GALAY_RPC_ERROR_DESERIALIZATION_ERROR:
            return "Deserialization error";
        case GALAY_RPC_ERROR_CANCELLED:
            return "Cancelled";
        case GALAY_RPC_ERROR_DEADLINE_EXCEEDED:
            return "Deadline exceeded";
        case GALAY_RPC_ERROR_RESOURCE_EXHAUSTED:
            return "Resource exhausted";
        case GALAY_RPC_ERROR_RATE_LIMITED:
            return "Rate limited";
        case GALAY_RPC_ERROR_CIRCUIT_OPEN:
            return "Circuit open";
        case GALAY_RPC_ERROR_UNAUTHENTICATED:
            return "Unauthenticated";
        case GALAY_RPC_ERROR_PERMISSION_DENIED:
            return "Permission denied";
        case GALAY_RPC_ERROR_UNAVAILABLE:
            return "Unavailable";
        case GALAY_RPC_ERROR_UNKNOWN_ERROR:
            return "Unknown";
    }
    return "Unknown";
}

galay_status_t galay_rpc_error_to_status(galay_rpc_error_code_t code)
{
    switch (code) {
        case GALAY_RPC_ERROR_OK:
            return GALAY_OK;
        case GALAY_RPC_ERROR_METHOD_NOT_FOUND:
            return GALAY_NOT_FOUND;
        case GALAY_RPC_ERROR_RESOURCE_EXHAUSTED:
            return GALAY_OUT_OF_MEMORY;
        case GALAY_RPC_ERROR_UNAUTHENTICATED:
        case GALAY_RPC_ERROR_PERMISSION_DENIED:
            return GALAY_UNSUPPORTED;
        case GALAY_RPC_ERROR_INVALID_REQUEST:
        case GALAY_RPC_ERROR_INVALID_RESPONSE:
        case GALAY_RPC_ERROR_DESERIALIZATION_ERROR:
            return GALAY_PROTOCOL_ERROR;
        case GALAY_RPC_ERROR_CANCELLED:
        case GALAY_RPC_ERROR_DEADLINE_EXCEEDED:
        case GALAY_RPC_ERROR_RATE_LIMITED:
        case GALAY_RPC_ERROR_CIRCUIT_OPEN:
        case GALAY_RPC_ERROR_UNAVAILABLE:
            return GALAY_IO_ERROR;
        case GALAY_RPC_ERROR_UNKNOWN_ERROR:
            return GALAY_INTERNAL_ERROR;
    }
    return GALAY_INTERNAL_ERROR;
}

galay_bool_t galay_rpc_name_is_valid(const char* name, size_t name_len)
{
    if (name == nullptr || name_len == 0 || name_len > 65535) {
        return GALAY_FALSE;
    }
    for (size_t i = 0; i < name_len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(ch) && ch != '_' && ch != '.') {
            return GALAY_FALSE;
        }
    }
    return GALAY_TRUE;
}

galay_status_t galay_rpc_request_encoded_size(const galay_rpc_request_t* request, size_t* size)
{
    return validate_request(request, size);
}

galay_status_t galay_rpc_response_encoded_size(const galay_rpc_response_t* response, size_t* size)
{
    if (size != nullptr) {
        *size = 0;
    }
    if (response == nullptr || size == nullptr || !valid_mode(response->call_mode) ||
        (response->payload == nullptr && response->payload_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    *size = GALAY_RPC_HEADER_SIZE + 1 + response->payload_len;
    return GALAY_OK;
}

galay_status_t galay_rpc_encode_request(const galay_rpc_request_t* request, uint8_t* out,
                                        size_t out_len, size_t* written)
{
    if (written != nullptr) {
        *written = 0;
    }
    size_t need = 0;
    galay_status_t status = validate_request(request, &need);
    if (status != GALAY_OK) {
        return status;
    }
    if (out == nullptr || written == nullptr || out_len < need) {
        return GALAY_OUT_OF_MEMORY;
    }
    out[0] = 'G'; out[1] = 'R'; out[2] = 'P'; out[3] = 'C';
    out[4] = 1;
    out[5] = 1;
    out[6] = static_cast<uint8_t>(request->call_mode);
    out[7] = request->end_of_stream == GALAY_TRUE ? 1 : 0;
    put32(out + 8, request->request_id);
    put32(out + 12, static_cast<uint32_t>(need - GALAY_RPC_HEADER_SIZE));
    size_t pos = GALAY_RPC_HEADER_SIZE;
    put16(out + pos, static_cast<uint16_t>(request->service_len)); pos += 2;
    std::memcpy(out + pos, request->service, request->service_len); pos += request->service_len;
    put16(out + pos, static_cast<uint16_t>(request->method_len)); pos += 2;
    std::memcpy(out + pos, request->method, request->method_len); pos += request->method_len;
    if (request->payload_len != 0) {
        std::memcpy(out + pos, request->payload, request->payload_len);
    }
    *written = need;
    return GALAY_OK;
}

galay_status_t galay_rpc_encode_response(const galay_rpc_response_t* response, uint8_t* out,
                                         size_t out_len, size_t* written)
{
    if (written != nullptr) {
        *written = 0;
    }
    size_t need = 0;
    const galay_status_t status = galay_rpc_response_encoded_size(response, &need);
    if (status != GALAY_OK) {
        return status;
    }
    if (out == nullptr || written == nullptr || out_len < need) {
        return GALAY_OUT_OF_MEMORY;
    }
    out[0] = 'G'; out[1] = 'R'; out[2] = 'P'; out[3] = 'C';
    out[4] = 1;
    out[5] = 2;
    out[6] = static_cast<uint8_t>(response->call_mode);
    out[7] = response->end_of_stream == GALAY_TRUE ? 1 : 0;
    put32(out + 8, response->request_id);
    put32(out + 12, static_cast<uint32_t>(response->payload_len + 1));
    out[GALAY_RPC_HEADER_SIZE] = static_cast<uint8_t>(response->error_code);
    if (response->payload_len != 0) {
        std::memcpy(out + GALAY_RPC_HEADER_SIZE + 1, response->payload, response->payload_len);
    }
    *written = need;
    return GALAY_OK;
}

galay_status_t galay_rpc_decode_request(const uint8_t* data, size_t data_len,
                                        galay_rpc_request_t* out, size_t* consumed,
                                        galay_rpc_error_code_t* rpc_error)
{
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (rpc_error != nullptr) {
        *rpc_error = GALAY_RPC_ERROR_INVALID_REQUEST;
    }
    if (data == nullptr || out == nullptr || consumed == nullptr || rpc_error == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data_len < GALAY_RPC_HEADER_SIZE) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (std::memcmp(data, "GRPC", 4) != 0 || data[4] != 1 || data[5] != 1 ||
        (data[7] & 0xFEU) != 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    const uint32_t body_len = get32(data + 12);
    if (data_len < GALAY_RPC_HEADER_SIZE + body_len || body_len < 4) {
        *rpc_error = GALAY_RPC_ERROR_DESERIALIZATION_ERROR;
        return GALAY_PROTOCOL_ERROR;
    }
    const auto mode = static_cast<galay_rpc_call_mode_t>(data[6]);
    if (!valid_mode(mode)) {
        return GALAY_PROTOCOL_ERROR;
    }
    size_t pos = GALAY_RPC_HEADER_SIZE;
    if (data[pos] == 0xff && data[pos + 1] == 0xff) {
        pos += 2;
        if (pos + 2 > data_len) {
            return GALAY_PROTOCOL_ERROR;
        }
        const uint16_t metadata_count = get16(data + pos);
        pos += 2;
        for (uint16_t i = 0; i < metadata_count; ++i) {
            if (pos + 4 > GALAY_RPC_HEADER_SIZE + body_len) {
                *rpc_error = GALAY_RPC_ERROR_DESERIALIZATION_ERROR;
                return GALAY_PROTOCOL_ERROR;
            }
            const uint16_t key_len = get16(data + pos);
            pos += 2;
            const uint16_t value_len = get16(data + pos);
            pos += 2;
            if (pos + key_len + value_len > GALAY_RPC_HEADER_SIZE + body_len) {
                *rpc_error = GALAY_RPC_ERROR_DESERIALIZATION_ERROR;
                return GALAY_PROTOCOL_ERROR;
            }
            pos += key_len + value_len;
        }
    }
    if (pos + 4 > GALAY_RPC_HEADER_SIZE + body_len) {
        *rpc_error = GALAY_RPC_ERROR_DESERIALIZATION_ERROR;
        return GALAY_PROTOCOL_ERROR;
    }
    const uint16_t service_len = get16(data + pos);
    pos += 2;
    if (pos + service_len + 2 > GALAY_RPC_HEADER_SIZE + body_len) {
        *rpc_error = GALAY_RPC_ERROR_DESERIALIZATION_ERROR;
        return GALAY_PROTOCOL_ERROR;
    }
    const char* service = reinterpret_cast<const char*>(data + pos);
    pos += service_len;
    const uint16_t method_len = get16(data + pos);
    pos += 2;
    if (pos + method_len > GALAY_RPC_HEADER_SIZE + body_len) {
        *rpc_error = GALAY_RPC_ERROR_DESERIALIZATION_ERROR;
        return GALAY_PROTOCOL_ERROR;
    }
    const char* method = reinterpret_cast<const char*>(data + pos);
    pos += method_len;
    out->request_id = get32(data + 8);
    out->call_mode = mode;
    out->end_of_stream = data[7] == 1 ? GALAY_TRUE : GALAY_FALSE;
    out->service = service;
    out->service_len = service_len;
    out->method = method;
    out->method_len = method_len;
    out->payload = data + pos;
    out->payload_len = GALAY_RPC_HEADER_SIZE + body_len - pos;
    *consumed = GALAY_RPC_HEADER_SIZE + body_len;
    *rpc_error = GALAY_RPC_ERROR_OK;
    return GALAY_OK;
}

galay_status_t galay_rpc_decode_response(const uint8_t* data, size_t data_len,
                                         galay_rpc_response_t* out, size_t* consumed,
                                         galay_rpc_error_code_t* rpc_error)
{
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (rpc_error != nullptr) {
        *rpc_error = GALAY_RPC_ERROR_INVALID_RESPONSE;
    }
    if (data == nullptr || out == nullptr || consumed == nullptr || rpc_error == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data_len < GALAY_RPC_HEADER_SIZE || std::memcmp(data, "GRPC", 4) != 0 ||
        data[4] != 1 || data[5] != 2 || (data[7] & 0xFEU) != 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    const uint32_t body_len = get32(data + 12);
    if (data_len < GALAY_RPC_HEADER_SIZE + body_len || body_len == 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    out->request_id = get32(data + 8);
    out->call_mode = static_cast<galay_rpc_call_mode_t>(data[6]);
    out->end_of_stream = data[7] == 1 ? GALAY_TRUE : GALAY_FALSE;
    out->error_code = static_cast<galay_rpc_error_code_t>(data[GALAY_RPC_HEADER_SIZE]);
    out->payload = data + GALAY_RPC_HEADER_SIZE + 1;
    out->payload_len = body_len - 1;
    *consumed = GALAY_RPC_HEADER_SIZE + body_len;
    *rpc_error = GALAY_RPC_ERROR_OK;
    return GALAY_OK;
}

}
