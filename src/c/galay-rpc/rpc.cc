#include <galay/c/galay-rpc/rpc.h>

#include <galay/cpp/galay-rpc/protoc/rpc_base.h>
#include <galay/cpp/galay-rpc/protoc/rpc_message.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

namespace {

using galay::rpc::RpcCallMode;
using galay::rpc::RpcErrorCode;
using galay::rpc::RpcHeader;
using galay::rpc::RpcMessageType;
using galay::rpc::RpcRequest;
using galay::rpc::RpcResponse;

constexpr size_t kRpcHeaderSize = galay::rpc::RPC_HEADER_SIZE;
constexpr size_t kRpcMaxBodySize = galay::rpc::RPC_MAX_BODY_SIZE;

bool is_alpha_or_underscore(char ch) noexcept
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

bool is_name_tail(char ch) noexcept
{
    return is_alpha_or_underscore(ch) || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
}

bool is_valid_name(const char* name, size_t name_len) noexcept
{
    if (name == nullptr || name_len == 0 || !is_alpha_or_underscore(name[0])) {
        return false;
    }
    for (size_t i = 1; i < name_len; ++i) {
        if (!is_name_tail(name[i])) {
            return false;
        }
    }
    return true;
}

bool is_valid_call_mode(galay_rpc_call_mode_t mode) noexcept
{
    switch (mode) {
    case GALAY_RPC_CALL_UNARY:
    case GALAY_RPC_CALL_CLIENT_STREAMING:
    case GALAY_RPC_CALL_SERVER_STREAMING:
    case GALAY_RPC_CALL_BIDI_STREAMING:
        return true;
    default:
        return false;
    }
}

RpcCallMode to_cpp_call_mode(galay_rpc_call_mode_t mode) noexcept
{
    return static_cast<RpcCallMode>(static_cast<uint8_t>(mode));
}

galay_rpc_call_mode_t to_c_call_mode(RpcCallMode mode) noexcept
{
    return static_cast<galay_rpc_call_mode_t>(static_cast<uint8_t>(mode));
}

RpcErrorCode to_cpp_error(galay_rpc_error_code_t error) noexcept
{
    return static_cast<RpcErrorCode>(static_cast<uint16_t>(error));
}

galay_rpc_error_code_t to_c_error(RpcErrorCode error) noexcept
{
    return static_cast<galay_rpc_error_code_t>(static_cast<uint16_t>(error));
}

void set_rpc_error(galay_rpc_error_code_t* out, galay_rpc_error_code_t error) noexcept
{
    if (out != nullptr) {
        *out = error;
    }
}

void set_consumed(size_t* out, size_t value) noexcept
{
    if (out != nullptr) {
        *out = value;
    }
}

galay_status_t checked_request_size(const galay_rpc_request_t* request, size_t* out_size) noexcept
{
    if (out_size == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_size = 0;
    if (request == nullptr ||
        !is_valid_call_mode(request->call_mode) ||
        !is_valid_name(request->service, request->service_len) ||
        !is_valid_name(request->method, request->method_len) ||
        (request->payload_len > 0 && request->payload == nullptr) ||
        request->service_len > std::numeric_limits<uint16_t>::max() ||
        request->method_len > std::numeric_limits<uint16_t>::max()) {
        return GALAY_INVALID_ARGUMENT;
    }

    const size_t body_size = 2 + request->service_len + 2 + request->method_len + request->payload_len;
    if (body_size > kRpcMaxBodySize ||
        body_size > std::numeric_limits<uint32_t>::max() ||
        body_size > std::numeric_limits<size_t>::max() - kRpcHeaderSize) {
        return GALAY_OUT_OF_MEMORY;
    }
    *out_size = kRpcHeaderSize + body_size;
    return GALAY_OK;
}

galay_status_t checked_response_size(const galay_rpc_response_t* response, size_t* out_size) noexcept
{
    if (out_size == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_size = 0;
    if (response == nullptr ||
        !is_valid_call_mode(response->call_mode) ||
        (response->payload_len > 0 && response->payload == nullptr)) {
        return GALAY_INVALID_ARGUMENT;
    }

    const size_t body_size = 2 + response->payload_len;
    if (body_size > kRpcMaxBodySize ||
        body_size > std::numeric_limits<uint32_t>::max() ||
        body_size > std::numeric_limits<size_t>::max() - kRpcHeaderSize) {
        return GALAY_OUT_OF_MEMORY;
    }
    *out_size = kRpcHeaderSize + body_size;
    return GALAY_OK;
}

galay_status_t decode_header(const void* data,
                             size_t data_len,
                             uint8_t expected_type,
                             RpcHeader& header,
                             galay_rpc_error_code_t error_code,
                             galay_rpc_error_code_t* rpc_error) noexcept
{
    if ((data_len > 0 && data == nullptr) || data_len < kRpcHeaderSize) {
        set_rpc_error(rpc_error, error_code);
        return GALAY_PROTOCOL_ERROR;
    }

    if (!header.deserialize(static_cast<const char*>(data)) || header.m_type != expected_type) {
        set_rpc_error(rpc_error, error_code);
        return GALAY_PROTOCOL_ERROR;
    }
    if (data_len < kRpcHeaderSize + header.m_body_length) {
        set_rpc_error(rpc_error, error_code);
        return GALAY_PROTOCOL_ERROR;
    }
    return GALAY_OK;
}

} // namespace

const char* galay_rpc_error_string(galay_rpc_error_code_t error)
{
    return galay::rpc::rpcErrorCodeToString(to_cpp_error(error));
}

galay_status_t galay_rpc_error_to_status(galay_rpc_error_code_t error)
{
    switch (error) {
    case GALAY_RPC_ERROR_OK:
        return GALAY_OK;
    case GALAY_RPC_ERROR_SERVICE_NOT_FOUND:
    case GALAY_RPC_ERROR_METHOD_NOT_FOUND:
        return GALAY_NOT_FOUND;
    case GALAY_RPC_ERROR_INVALID_REQUEST:
    case GALAY_RPC_ERROR_INVALID_RESPONSE:
    case GALAY_RPC_ERROR_SERIALIZATION_ERROR:
    case GALAY_RPC_ERROR_DESERIALIZATION_ERROR:
        return GALAY_PROTOCOL_ERROR;
    case GALAY_RPC_ERROR_REQUEST_TIMEOUT:
    case GALAY_RPC_ERROR_CONNECTION_CLOSED:
        return GALAY_IO_ERROR;
    case GALAY_RPC_ERROR_UNKNOWN_ERROR:
    case GALAY_RPC_ERROR_INTERNAL_ERROR:
    default:
        return GALAY_INTERNAL_ERROR;
    }
}

galay_bool_t galay_rpc_name_is_valid(const char* name, size_t name_len)
{
    return is_valid_name(name, name_len) ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_rpc_request_encoded_size(const galay_rpc_request_t* request, size_t* out_size)
{
    return checked_request_size(request, out_size);
}

galay_status_t galay_rpc_encode_request(const galay_rpc_request_t* request,
                                        void* out,
                                        size_t out_capacity,
                                        size_t* written)
{
    if (written != nullptr) {
        *written = 0;
    }
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        size_t required = 0;
        galay_status_t status = checked_request_size(request, &required);
        if (status != GALAY_OK) {
            return status;
        }
        if (out_capacity < required) {
            return GALAY_OUT_OF_MEMORY;
        }

        RpcRequest cpp_request(request->request_id,
                               std::string_view(request->service, request->service_len),
                               std::string_view(request->method, request->method_len));
        cpp_request.callMode(to_cpp_call_mode(request->call_mode));
        cpp_request.endOfStream(request->end_of_stream != GALAY_FALSE);
        if (request->payload_len > 0) {
            cpp_request.payload(static_cast<const char*>(request->payload), request->payload_len);
        }

        const auto encoded = cpp_request.serialize();
        std::memcpy(out, encoded.data(), encoded.size());
        if (written != nullptr) {
            *written = encoded.size();
        }
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_rpc_decode_request(const void* data,
                                        size_t data_len,
                                        galay_rpc_request_t* out_request,
                                        size_t* consumed,
                                        galay_rpc_error_code_t* rpc_error)
{
    set_consumed(consumed, 0);
    set_rpc_error(rpc_error, GALAY_RPC_ERROR_OK);
    if (out_request == nullptr) {
        set_rpc_error(rpc_error, GALAY_RPC_ERROR_INVALID_REQUEST);
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        RpcHeader header;
        galay_status_t status = decode_header(data,
                                              data_len,
                                              static_cast<uint8_t>(RpcMessageType::REQUEST),
                                              header,
                                              GALAY_RPC_ERROR_INVALID_REQUEST,
                                              rpc_error);
        if (status != GALAY_OK) {
            return status;
        }

        const char* body = static_cast<const char*>(data) + kRpcHeaderSize;
        const size_t body_len = header.m_body_length;
        if (body_len < 4) {
            set_rpc_error(rpc_error, GALAY_RPC_ERROR_DESERIALIZATION_ERROR);
            return GALAY_PROTOCOL_ERROR;
        }

        size_t offset = 0;
        uint16_t service_len = 0;
        std::memcpy(&service_len, body + offset, sizeof(service_len));
        service_len = galay::rpc::rpcNtohs(service_len);
        offset += 2;
        if (offset + service_len > body_len) {
            set_rpc_error(rpc_error, GALAY_RPC_ERROR_DESERIALIZATION_ERROR);
            return GALAY_PROTOCOL_ERROR;
        }
        const char* service = body + offset;
        offset += service_len;

        if (offset + 2 > body_len) {
            set_rpc_error(rpc_error, GALAY_RPC_ERROR_DESERIALIZATION_ERROR);
            return GALAY_PROTOCOL_ERROR;
        }
        uint16_t method_len = 0;
        std::memcpy(&method_len, body + offset, sizeof(method_len));
        method_len = galay::rpc::rpcNtohs(method_len);
        offset += 2;
        if (offset + method_len > body_len) {
            set_rpc_error(rpc_error, GALAY_RPC_ERROR_DESERIALIZATION_ERROR);
            return GALAY_PROTOCOL_ERROR;
        }
        const char* method = body + offset;
        offset += method_len;

        if (!is_valid_name(service, service_len) || !is_valid_name(method, method_len)) {
            set_rpc_error(rpc_error, GALAY_RPC_ERROR_DESERIALIZATION_ERROR);
            return GALAY_PROTOCOL_ERROR;
        }

        out_request->request_id = header.m_request_id;
        out_request->call_mode = to_c_call_mode(galay::rpc::rpcDecodeCallMode(header.m_flags));
        out_request->end_of_stream = galay::rpc::rpcIsEndStream(header.m_flags) ? GALAY_TRUE : GALAY_FALSE;
        out_request->service = service;
        out_request->service_len = service_len;
        out_request->method = method;
        out_request->method_len = method_len;
        out_request->payload = body + offset;
        out_request->payload_len = body_len - offset;
        set_consumed(consumed, kRpcHeaderSize + body_len);
        set_rpc_error(rpc_error, GALAY_RPC_ERROR_OK);
        return GALAY_OK;
    } catch (...) {
        set_rpc_error(rpc_error, GALAY_RPC_ERROR_INTERNAL_ERROR);
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_rpc_response_encoded_size(const galay_rpc_response_t* response, size_t* out_size)
{
    return checked_response_size(response, out_size);
}

galay_status_t galay_rpc_encode_response(const galay_rpc_response_t* response,
                                         void* out,
                                         size_t out_capacity,
                                         size_t* written)
{
    if (written != nullptr) {
        *written = 0;
    }
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        size_t required = 0;
        galay_status_t status = checked_response_size(response, &required);
        if (status != GALAY_OK) {
            return status;
        }
        if (out_capacity < required) {
            return GALAY_OUT_OF_MEMORY;
        }

        RpcResponse cpp_response(response->request_id, to_cpp_error(response->error_code));
        cpp_response.callMode(to_cpp_call_mode(response->call_mode));
        cpp_response.endOfStream(response->end_of_stream != GALAY_FALSE);
        if (response->payload_len > 0) {
            cpp_response.payload(static_cast<const char*>(response->payload), response->payload_len);
        }

        const auto encoded = cpp_response.serialize();
        std::memcpy(out, encoded.data(), encoded.size());
        if (written != nullptr) {
            *written = encoded.size();
        }
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_rpc_decode_response(const void* data,
                                         size_t data_len,
                                         galay_rpc_response_t* out_response,
                                         size_t* consumed,
                                         galay_rpc_error_code_t* rpc_error)
{
    set_consumed(consumed, 0);
    set_rpc_error(rpc_error, GALAY_RPC_ERROR_OK);
    if (out_response == nullptr) {
        set_rpc_error(rpc_error, GALAY_RPC_ERROR_INVALID_RESPONSE);
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        RpcHeader header;
        galay_status_t status = decode_header(data,
                                              data_len,
                                              static_cast<uint8_t>(RpcMessageType::RESPONSE),
                                              header,
                                              GALAY_RPC_ERROR_INVALID_RESPONSE,
                                              rpc_error);
        if (status != GALAY_OK) {
            return status;
        }

        const char* body = static_cast<const char*>(data) + kRpcHeaderSize;
        const size_t body_len = header.m_body_length;
        if (body_len < 2) {
            set_rpc_error(rpc_error, GALAY_RPC_ERROR_DESERIALIZATION_ERROR);
            return GALAY_PROTOCOL_ERROR;
        }

        uint16_t error = 0;
        std::memcpy(&error, body, sizeof(error));
        error = galay::rpc::rpcNtohs(error);

        out_response->request_id = header.m_request_id;
        out_response->call_mode = to_c_call_mode(galay::rpc::rpcDecodeCallMode(header.m_flags));
        out_response->end_of_stream = galay::rpc::rpcIsEndStream(header.m_flags) ? GALAY_TRUE : GALAY_FALSE;
        out_response->error_code = static_cast<galay_rpc_error_code_t>(error);
        out_response->payload = body + 2;
        out_response->payload_len = body_len - 2;
        set_consumed(consumed, kRpcHeaderSize + body_len);
        set_rpc_error(rpc_error, GALAY_RPC_ERROR_OK);
        return GALAY_OK;
    } catch (...) {
        set_rpc_error(rpc_error, GALAY_RPC_ERROR_INTERNAL_ERROR);
        return GALAY_INTERNAL_ERROR;
    }
}
