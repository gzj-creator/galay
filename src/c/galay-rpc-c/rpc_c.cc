#include <galay/c/galay-rpc-c/rpc_c.h>

#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr size_t kDefaultBacklog = 128;
constexpr size_t kMaxRpcBodySize = 16u * 1024u * 1024u;
constexpr uint8_t kMessageRequest = 1;
constexpr uint8_t kMessageResponse = 2;
constexpr uint8_t kMessageHeartbeat = 3;

struct RpcRouteSlot {
    std::string method;
    galay_rpc_method_handler_fn handler = nullptr;
    void* user_data = nullptr;
    galay_rpc_call_mode_t mode = GALAY_RPC_CALL_UNARY;
};

struct galay_rpc_service_t {
    std::string name;
    std::vector<RpcRouteSlot> methods;
};

struct galay_rpc_server_t {
    std::string host = "127.0.0.1";
    galay_kernel_tcp_socket_t listener{};
    std::vector<galay_rpc_service_t*> services;
    int backlog = static_cast<int>(kDefaultBacklog);
    uint16_t port = 0;
    bool listening = false;
};

struct galay_rpc_client_t {
    std::string host = "127.0.0.1";
    galay_kernel_tcp_socket_t socket{};
    int64_t connect_timeout_ms = -1;
    uint32_t next_request_id = 1;
    uint16_t port = 0;
    bool connected = false;
};

struct galay_rpc_stream_t {
    galay_rpc_client_t* client = nullptr;
    std::string service;
    std::string method;
    uint32_t request_id = 0;
    galay_rpc_call_mode_t mode = GALAY_RPC_CALL_BIDI_STREAMING;
    bool closed = false;
};

struct galay_rpc_cancellation_source_t {
    std::atomic<bool> cancelled{false};
};

struct galay_rpc_pool_lease_t {
    std::string host;
    uint64_t id = 0;
    uint16_t port = 0;
    bool valid = false;
};

struct PoolBucket {
    std::string host;
    std::vector<uint64_t> available;
    size_t in_use = 0;
    size_t total = 0;
    uint16_t port = 0;
};

struct galay_rpc_pool_t {
    galay_rpc_pool_config_t config{};
    std::unordered_map<std::string, PoolBucket> buckets;
    uint64_t next_id = 1;
    bool shutdown = false;
};

struct IncomingFrame {
    std::vector<uint8_t> bytes;
    galay_rpc_request_t request{};
    uint32_t request_id = 0;
    uint32_t body_len = 0;
    uint8_t type = 0;
};

namespace {

bool valid_mode(galay_rpc_call_mode_t mode)
{
    return mode == GALAY_RPC_CALL_UNARY || mode == GALAY_RPC_CALL_CLIENT_STREAMING ||
        mode == GALAY_RPC_CALL_SERVER_STREAMING || mode == GALAY_RPC_CALL_BIDI_STREAMING;
}

bool streaming_mode(galay_rpc_call_mode_t mode)
{
    return mode == GALAY_RPC_CALL_CLIENT_STREAMING ||
        mode == GALAY_RPC_CALL_SERVER_STREAMING ||
        mode == GALAY_RPC_CALL_BIDI_STREAMING;
}

void put16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    out[1] = static_cast<uint8_t>(value & 0xFFU);
}

void put32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
    out[1] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    out[2] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    out[3] = static_cast<uint8_t>(value & 0xFFU);
}

uint16_t get16(const uint8_t* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) | data[1]);
}

uint32_t get32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24U) |
        (static_cast<uint32_t>(data[1]) << 16U) |
        (static_cast<uint32_t>(data[2]) << 8U) | data[3];
}

C_IOResult make_io_result(C_IOResultCode code)
{
    C_IOResult result{};
    result.code = code;
    return result;
}

template<typename T>
T* allocate_handle()
{
    void* memory = std::malloc(sizeof(T));
    if (memory == nullptr) {
        return nullptr;
    }
    return ::new (memory) T();
}

template<typename T>
void destroy_handle(T* handle)
{
    if (handle == nullptr) {
        return;
    }
    handle->~T();
    std::free(handle);
}

C_IOResult io_result_from_status(galay_status_t status)
{
    switch (status) {
        case GALAY_OK:
            return make_io_result(C_IOResultOk);
        case GALAY_INVALID_ARGUMENT:
            return make_io_result(C_IOResultInvalid);
        case GALAY_EOF:
            return make_io_result(C_IOResultEof);
        case GALAY_TIMEOUT:
            return make_io_result(C_IOResultTimeout);
        case GALAY_CANCELLED:
            return make_io_result(C_IOResultCancelled);
        case GALAY_NOT_FOUND:
        case GALAY_PROTOCOL_ERROR:
        case GALAY_UNSUPPORTED:
        case GALAY_IO_ERROR:
        case GALAY_INTERNAL_ERROR:
        case GALAY_OUT_OF_MEMORY:
            return make_io_result(C_IOResultError);
    }
    return make_io_result(C_IOResultError);
}

galay_status_t status_from_tcp_create(C_TcpSocketResultCode code)
{
    switch (code) {
        case C_TcpSocketSuccess:
            return GALAY_OK;
        case C_TcpSocketParameterInvalid:
        case C_TcpSocketOperationInvalid:
            return GALAY_INVALID_ARGUMENT;
        case C_TcpSocketMemoryAllocFailed:
            return GALAY_OUT_OF_MEMORY;
        case C_TcpSocketIOFailed:
            return GALAY_IO_ERROR;
    }
    return GALAY_IO_ERROR;
}

C_IOResult io_result_from_tcp_create(C_TcpSocketResultCode code)
{
    return io_result_from_status(status_from_tcp_create(code));
}

bool payload_arg_valid(const void* payload, size_t payload_len)
{
    return payload != nullptr || payload_len == 0;
}

bool copy_host_to_c_host(const std::string& host, uint16_t port, C_Host* out)
{
    if (out == nullptr || host.empty() || host.size() >= C_HOST_ADDRESS_MAX_LENGTH || port == 0) {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    out->type = C_IPTypeIPV4;
    std::memcpy(out->address, host.data(), host.size());
    out->address[host.size()] = '\0';
    out->port = port;
    return true;
}

std::string endpoint_key(const char* host, uint16_t port)
{
    std::string key(host == nullptr ? "" : host);
    key.push_back(':');
    key.append(std::to_string(port));
    return key;
}

galay_status_t validate_request(const galay_rpc_request_t* request, size_t* size)
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

void reset_response_buffer(galay_rpc_response_buffer_t* response,
                           galay_rpc_error_code_t code,
                           galay_rpc_call_mode_t mode)
{
    if (response == nullptr) {
        return;
    }
    response->request_id = 0;
    response->call_mode = mode;
    response->end_of_stream = GALAY_TRUE;
    response->error_code = code;
    response->payload = nullptr;
    response->payload_len = 0;
}

C_IOResult copy_response_to_buffer(const galay_rpc_response_t& decoded,
                                   galay_rpc_response_buffer_t* out)
{
    if (out == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    reset_response_buffer(out, decoded.error_code, decoded.call_mode);
    out->request_id = decoded.request_id;
    out->end_of_stream = decoded.end_of_stream;
    if (decoded.payload_len == 0) {
        C_IOResult result = make_io_result(C_IOResultOk);
        result.ptr = out;
        return result;
    }
    void* payload = std::malloc(decoded.payload_len);
    if (payload == nullptr) {
        out->error_code = GALAY_RPC_ERROR_RESOURCE_EXHAUSTED;
        return make_io_result(C_IOResultError);
    }
    std::memcpy(payload, decoded.payload, decoded.payload_len);
    out->payload = payload;
    out->payload_len = decoded.payload_len;
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = decoded.payload_len;
    result.ptr = out;
    return result;
}

C_IOResult send_all(galay_kernel_tcp_socket_t* socket,
                    const uint8_t* data,
                    size_t data_len,
                    int64_t timeout_ms)
{
    size_t sent = 0;
    while (sent < data_len) {
        C_IOResult result = galay_kernel_tcp_socket_send(
            socket,
            reinterpret_cast<const char*>(data + sent),
            data_len - sent,
            timeout_ms);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        sent += result.bytes;
    }
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = sent;
    return result;
}

C_IOResult recv_exact(galay_kernel_tcp_socket_t* socket,
                      uint8_t* out,
                      size_t out_len,
                      int64_t timeout_ms)
{
    size_t received = 0;
    while (received < out_len) {
        C_IOResult result = galay_kernel_tcp_socket_recv(
            socket,
            reinterpret_cast<char*>(out + received),
            out_len - received,
            timeout_ms);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        received += result.bytes;
    }
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = received;
    return result;
}

bool header_is_valid(const uint8_t* header)
{
    return header != nullptr &&
        std::memcmp(header, "GRPC", 4) == 0 &&
        header[4] == 1 &&
        get32(header + 12) <= kMaxRpcBodySize;
}

void build_heartbeat_frame(uint32_t request_id, uint8_t* out)
{
    out[0] = 'G';
    out[1] = 'R';
    out[2] = 'P';
    out[3] = 'C';
    out[4] = 1;
    out[5] = kMessageHeartbeat;
    out[6] = static_cast<uint8_t>(GALAY_RPC_CALL_UNARY);
    out[7] = 1;
    put32(out + 8, request_id);
    put32(out + 12, 0);
}

C_IOResult read_incoming_frame(galay_kernel_tcp_socket_t* socket,
                               int64_t timeout_ms,
                               IncomingFrame* frame)
{
    if (socket == nullptr || frame == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    uint8_t header[GALAY_RPC_HEADER_SIZE];
    C_IOResult header_result = recv_exact(socket, header, sizeof(header), timeout_ms);
    if (header_result.code != C_IOResultOk) {
        return header_result;
    }
    if (!header_is_valid(header)) {
        return make_io_result(C_IOResultError);
    }

    frame->type = header[5];
    frame->request_id = get32(header + 8);
    frame->body_len = get32(header + 12);
    frame->bytes.assign(header, header + GALAY_RPC_HEADER_SIZE);
    if (frame->body_len > 0) {
        const size_t old_size = frame->bytes.size();
        frame->bytes.resize(old_size + frame->body_len);
        C_IOResult body_result = recv_exact(
            socket,
            frame->bytes.data() + old_size,
            frame->body_len,
            timeout_ms);
        if (body_result.code != C_IOResultOk) {
            return body_result;
        }
    }
    if (frame->type == kMessageHeartbeat) {
        if (frame->body_len != 0) {
            return make_io_result(C_IOResultError);
        }
        return make_io_result(C_IOResultOk);
    }
    if (frame->type != kMessageRequest) {
        return make_io_result(C_IOResultError);
    }
    size_t consumed = 0;
    galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_UNKNOWN_ERROR;
    galay_status_t decoded = galay_rpc_decode_request(
        frame->bytes.data(),
        frame->bytes.size(),
        &frame->request,
        &consumed,
        &rpc_error);
    if (decoded != GALAY_OK || consumed != frame->bytes.size() || rpc_error != GALAY_RPC_ERROR_OK) {
        return io_result_from_status(decoded == GALAY_OK ? GALAY_PROTOCOL_ERROR : decoded);
    }
    return make_io_result(C_IOResultOk);
}

C_IOResult read_response_frame(galay_kernel_tcp_socket_t* socket,
                               int64_t timeout_ms,
                               galay_rpc_response_buffer_t* out_response)
{
    if (socket == nullptr || out_response == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    uint8_t header[GALAY_RPC_HEADER_SIZE];
    C_IOResult header_result = recv_exact(socket, header, sizeof(header), timeout_ms);
    if (header_result.code != C_IOResultOk) {
        if (header_result.code == C_IOResultTimeout) {
            reset_response_buffer(out_response,
                                  GALAY_RPC_ERROR_DEADLINE_EXCEEDED,
                                  GALAY_RPC_CALL_UNARY);
        } else if (header_result.code == C_IOResultEof) {
            reset_response_buffer(out_response,
                                  GALAY_RPC_ERROR_CONNECTION_CLOSED,
                                  GALAY_RPC_CALL_UNARY);
        }
        return header_result;
    }
    if (!header_is_valid(header) || header[5] != kMessageResponse) {
        reset_response_buffer(out_response, GALAY_RPC_ERROR_INVALID_RESPONSE, GALAY_RPC_CALL_UNARY);
        return make_io_result(C_IOResultError);
    }
    const uint32_t body_len = get32(header + 12);
    std::vector<uint8_t> frame(header, header + GALAY_RPC_HEADER_SIZE);
    if (body_len > 0) {
        const size_t old_size = frame.size();
        frame.resize(old_size + body_len);
        C_IOResult body_result = recv_exact(socket, frame.data() + old_size, body_len, timeout_ms);
        if (body_result.code != C_IOResultOk) {
            return body_result;
        }
    }

    galay_rpc_response_t decoded{};
    size_t consumed = 0;
    galay_rpc_error_code_t rpc_error = GALAY_RPC_ERROR_UNKNOWN_ERROR;
    galay_status_t status = galay_rpc_decode_response(
        frame.data(),
        frame.size(),
        &decoded,
        &consumed,
        &rpc_error);
    if (status != GALAY_OK || consumed != frame.size() || rpc_error != GALAY_RPC_ERROR_OK) {
        reset_response_buffer(out_response, GALAY_RPC_ERROR_INVALID_RESPONSE, GALAY_RPC_CALL_UNARY);
        return io_result_from_status(status == GALAY_OK ? GALAY_PROTOCOL_ERROR : status);
    }
    return copy_response_to_buffer(decoded, out_response);
}

C_IOResult send_request_frame(galay_rpc_client_t* client,
                              uint32_t request_id,
                              galay_rpc_call_mode_t mode,
                              galay_bool_t end_of_stream,
                              const char* service,
                              size_t service_len,
                              const char* method,
                              size_t method_len,
                              const void* payload,
                              size_t payload_len,
                              int64_t timeout_ms)
{
    if (client == nullptr || !client->connected || client->socket.socket == nullptr ||
        !payload_arg_valid(payload, payload_len)) {
        return make_io_result(C_IOResultInvalid);
    }
    galay_rpc_request_t request{};
    request.request_id = request_id;
    request.call_mode = mode;
    request.end_of_stream = end_of_stream;
    request.service = service;
    request.service_len = service_len;
    request.method = method;
    request.method_len = method_len;
    request.payload = payload;
    request.payload_len = payload_len;

    size_t encoded_size = 0;
    galay_status_t size_status = galay_rpc_request_encoded_size(&request, &encoded_size);
    if (size_status != GALAY_OK) {
        return io_result_from_status(size_status);
    }
    std::vector<uint8_t> encoded(encoded_size);
    size_t written = 0;
    galay_status_t encoded_status =
        galay_rpc_encode_request(&request, encoded.data(), encoded.size(), &written);
    if (encoded_status != GALAY_OK || written != encoded.size()) {
        return io_result_from_status(encoded_status == GALAY_OK ? GALAY_INTERNAL_ERROR : encoded_status);
    }
    return send_all(&client->socket, encoded.data(), encoded.size(), timeout_ms);
}

const RpcRouteSlot* find_method(const galay_rpc_service_t* service,
                                const char* method,
                                size_t method_len,
                                galay_rpc_call_mode_t mode)
{
    if (service == nullptr || method == nullptr) {
        return nullptr;
    }
    for (const RpcRouteSlot& entry : service->methods) {
        if (entry.mode == mode &&
            entry.method.size() == method_len &&
            std::memcmp(entry.method.data(), method, method_len) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

const galay_rpc_service_t* find_service(const galay_rpc_server_t* server,
                                        const char* service_name,
                                        size_t service_len)
{
    if (server == nullptr || service_name == nullptr) {
        return nullptr;
    }
    for (const galay_rpc_service_t* service : server->services) {
        if (service != nullptr &&
            service->name.size() == service_len &&
            std::memcmp(service->name.data(), service_name, service_len) == 0) {
            return service;
        }
    }
    return nullptr;
}

C_IOResult send_response(galay_kernel_tcp_socket_t* socket,
                         const galay_rpc_response_t* response,
                         int64_t timeout_ms)
{
    size_t encoded_size = 0;
    galay_status_t size_status = galay_rpc_response_encoded_size(response, &encoded_size);
    if (size_status != GALAY_OK) {
        return io_result_from_status(size_status);
    }
    std::vector<uint8_t> encoded(encoded_size);
    size_t written = 0;
    galay_status_t encoded_status =
        galay_rpc_encode_response(response, encoded.data(), encoded.size(), &written);
    if (encoded_status != GALAY_OK || written != encoded.size()) {
        return io_result_from_status(encoded_status == GALAY_OK ? GALAY_INTERNAL_ERROR : encoded_status);
    }
    return send_all(socket, encoded.data(), encoded.size(), timeout_ms);
}

galay_rpc_error_code_t dispatch_request(const galay_rpc_server_t* server,
                                        const galay_rpc_request_t& request,
                                        galay_rpc_response_t* response)
{
    const galay_rpc_service_t* service =
        find_service(server, request.service, request.service_len);
    if (service == nullptr) {
        return GALAY_RPC_ERROR_SERVICE_NOT_FOUND;
    }
    const RpcRouteSlot* method =
        find_method(service, request.method, request.method_len, request.call_mode);
    if (method == nullptr || method->handler == nullptr) {
        return GALAY_RPC_ERROR_METHOD_NOT_FOUND;
    }
    return method->handler(&request, response, method->user_data);
}

void normalize_pool_config(galay_rpc_pool_config_t* config)
{
    if (config->max_connections_per_endpoint == 0) {
        config->max_connections_per_endpoint = 1;
    }
    if (config->min_connections_per_endpoint > config->max_connections_per_endpoint) {
        config->min_connections_per_endpoint = config->max_connections_per_endpoint;
    }
}

bool endpoint_valid(const char* host, uint16_t port)
{
    return host != nullptr && host[0] != '\0' && port != 0;
}

PoolBucket& bucket_for(galay_rpc_pool_t* pool, const char* host, uint16_t port)
{
    const std::string key = endpoint_key(host, port);
    auto [it, inserted] = pool->buckets.try_emplace(key);
    if (inserted) {
        it->second.host = host;
        it->second.port = port;
    }
    return it->second;
}

const PoolBucket* find_bucket(const galay_rpc_pool_t* pool, const char* host, uint16_t port)
{
    if (pool == nullptr) {
        return nullptr;
    }
    const std::string key = endpoint_key(host, port);
    auto it = pool->buckets.find(key);
    return it == pool->buckets.end() ? nullptr : &it->second;
}

uint64_t pool_new_connection_id(galay_rpc_pool_t* pool)
{
    const uint64_t id = pool->next_id;
    ++pool->next_id;
    return id;
}

}  // namespace

extern "C" {

const char* galay_rpc_error_string(galay_rpc_error_code_t code)
{
    switch (code) {
        case GALAY_RPC_ERROR_OK:
            return "OK";
        case GALAY_RPC_ERROR_UNKNOWN_ERROR:
            return "Unknown error";
        case GALAY_RPC_ERROR_SERVICE_NOT_FOUND:
            return "Service not found";
        case GALAY_RPC_ERROR_METHOD_NOT_FOUND:
            return "Method not found";
        case GALAY_RPC_ERROR_INVALID_REQUEST:
            return "Invalid request";
        case GALAY_RPC_ERROR_INVALID_RESPONSE:
            return "Invalid response";
        case GALAY_RPC_ERROR_REQUEST_TIMEOUT:
            return "Request timeout";
        case GALAY_RPC_ERROR_CONNECTION_CLOSED:
            return "Connection closed";
        case GALAY_RPC_ERROR_SERIALIZATION_ERROR:
            return "Serialization error";
        case GALAY_RPC_ERROR_DESERIALIZATION_ERROR:
            return "Deserialization error";
        case GALAY_RPC_ERROR_INTERNAL_ERROR:
            return "Internal error";
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
    }
    return "Unknown";
}

const char* galay_rpc_get_error(galay_rpc_error_code_t code)
{
    return galay_rpc_error_string(code);
}

galay_status_t galay_rpc_error_to_status(galay_rpc_error_code_t code)
{
    switch (code) {
        case GALAY_RPC_ERROR_OK:
            return GALAY_OK;
        case GALAY_RPC_ERROR_SERVICE_NOT_FOUND:
        case GALAY_RPC_ERROR_METHOD_NOT_FOUND:
            return GALAY_NOT_FOUND;
        case GALAY_RPC_ERROR_RESOURCE_EXHAUSTED:
            return GALAY_OUT_OF_MEMORY;
        case GALAY_RPC_ERROR_UNAUTHENTICATED:
        case GALAY_RPC_ERROR_PERMISSION_DENIED:
            return GALAY_UNSUPPORTED;
        case GALAY_RPC_ERROR_INVALID_REQUEST:
        case GALAY_RPC_ERROR_INVALID_RESPONSE:
        case GALAY_RPC_ERROR_SERIALIZATION_ERROR:
        case GALAY_RPC_ERROR_DESERIALIZATION_ERROR:
            return GALAY_PROTOCOL_ERROR;
        case GALAY_RPC_ERROR_REQUEST_TIMEOUT:
        case GALAY_RPC_ERROR_CONNECTION_CLOSED:
        case GALAY_RPC_ERROR_CANCELLED:
        case GALAY_RPC_ERROR_DEADLINE_EXCEEDED:
        case GALAY_RPC_ERROR_RATE_LIMITED:
        case GALAY_RPC_ERROR_CIRCUIT_OPEN:
        case GALAY_RPC_ERROR_UNAVAILABLE:
            return GALAY_IO_ERROR;
        case GALAY_RPC_ERROR_UNKNOWN_ERROR:
        case GALAY_RPC_ERROR_INTERNAL_ERROR:
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

galay_status_t galay_rpc_encode_request(const galay_rpc_request_t* request,
                                        uint8_t* out,
                                        size_t out_len,
                                        size_t* written)
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
    out[0] = 'G';
    out[1] = 'R';
    out[2] = 'P';
    out[3] = 'C';
    out[4] = 1;
    out[5] = kMessageRequest;
    out[6] = static_cast<uint8_t>(request->call_mode);
    out[7] = request->end_of_stream == GALAY_TRUE ? 1 : 0;
    put32(out + 8, request->request_id);
    put32(out + 12, static_cast<uint32_t>(need - GALAY_RPC_HEADER_SIZE));
    size_t pos = GALAY_RPC_HEADER_SIZE;
    put16(out + pos, static_cast<uint16_t>(request->service_len));
    pos += 2;
    std::memcpy(out + pos, request->service, request->service_len);
    pos += request->service_len;
    put16(out + pos, static_cast<uint16_t>(request->method_len));
    pos += 2;
    std::memcpy(out + pos, request->method, request->method_len);
    pos += request->method_len;
    if (request->payload_len != 0) {
        std::memcpy(out + pos, request->payload, request->payload_len);
    }
    *written = need;
    return GALAY_OK;
}

galay_status_t galay_rpc_encode_response(const galay_rpc_response_t* response,
                                         uint8_t* out,
                                         size_t out_len,
                                         size_t* written)
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
    out[0] = 'G';
    out[1] = 'R';
    out[2] = 'P';
    out[3] = 'C';
    out[4] = 1;
    out[5] = kMessageResponse;
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

galay_status_t galay_rpc_decode_request(const uint8_t* data,
                                        size_t data_len,
                                        galay_rpc_request_t* out,
                                        size_t* consumed,
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
    if (std::memcmp(data, "GRPC", 4) != 0 || data[4] != 1 || data[5] != kMessageRequest ||
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

galay_status_t galay_rpc_decode_response(const uint8_t* data,
                                         size_t data_len,
                                         galay_rpc_response_t* out,
                                         size_t* consumed,
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
        data[4] != 1 || data[5] != kMessageResponse || (data[7] & 0xFEU) != 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    const uint32_t body_len = get32(data + 12);
    if (data_len < GALAY_RPC_HEADER_SIZE + body_len || body_len == 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    const auto mode = static_cast<galay_rpc_call_mode_t>(data[6]);
    if (!valid_mode(mode)) {
        return GALAY_PROTOCOL_ERROR;
    }
    out->request_id = get32(data + 8);
    out->call_mode = mode;
    out->end_of_stream = data[7] == 1 ? GALAY_TRUE : GALAY_FALSE;
    out->error_code = static_cast<galay_rpc_error_code_t>(data[GALAY_RPC_HEADER_SIZE]);
    out->payload = data + GALAY_RPC_HEADER_SIZE + 1;
    out->payload_len = body_len - 1;
    *consumed = GALAY_RPC_HEADER_SIZE + body_len;
    *rpc_error = GALAY_RPC_ERROR_OK;
    return GALAY_OK;
}

galay_rpc_client_config_t galay_rpc_client_config_default(void)
{
    galay_rpc_client_config_t config{};
    config.host = "127.0.0.1";
    config.port = 0;
    config.connect_timeout_ms = -1;
    return config;
}

galay_rpc_server_config_t galay_rpc_server_config_default(void)
{
    galay_rpc_server_config_t config{};
    config.host = "127.0.0.1";
    config.port = 0;
    config.backlog = static_cast<int>(kDefaultBacklog);
    return config;
}

galay_rpc_call_options_t galay_rpc_call_options_default(void)
{
    galay_rpc_call_options_t options{};
    options.timeout_ms = -1;
    options.cancellation = nullptr;
    return options;
}

galay_rpc_pool_config_t galay_rpc_pool_config_default(void)
{
    galay_rpc_pool_config_t config{};
    config.min_connections_per_endpoint = 0;
    config.max_connections_per_endpoint = 1;
    return config;
}

galay_status_t galay_rpc_client_create(const galay_rpc_client_config_t* config,
                                       galay_rpc_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* client = allocate_handle<galay_rpc_client_t>();
    if (client == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    if (config != nullptr) {
        if (config->host != nullptr && config->host[0] != '\0') {
            client->host = config->host;
        }
        client->port = config->port;
        client->connect_timeout_ms = config->connect_timeout_ms;
    }
    *out = client;
    return GALAY_OK;
}

void galay_rpc_client_destroy(galay_rpc_client_t* client)
{
    if (client != nullptr && client->socket.socket != nullptr) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
        }
    }
    destroy_handle(client);
}

C_IOResult galay_rpc_client_connect(galay_rpc_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->connected || client->socket.socket != nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_Host host{};
    if (!copy_host_to_c_host(client->host, client->port, &host)) {
        return make_io_result(C_IOResultInvalid);
    }
    C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&client->socket, host.type);
    if (created != C_TcpSocketSuccess) {
        return io_result_from_tcp_create(created);
    }
    const int64_t effective_timeout =
        timeout_ms < 0 && client->connect_timeout_ms >= 0 ? client->connect_timeout_ms : timeout_ms;
    C_IOResult connected = galay_kernel_tcp_socket_connect(&client->socket, &host, effective_timeout);
    if (connected.code != C_IOResultOk) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess && connected.code == C_IOResultOk) {
            return io_result_from_tcp_create(destroyed);
        }
        client->connected = false;
        return connected;
    }
    client->connected = true;
    connected.ptr = client;
    return connected;
}

C_IOResult galay_rpc_client_close(galay_rpc_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult close_result = galay_kernel_tcp_socket_close(&client->socket, timeout_ms);
    C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
    client->connected = false;
    if (close_result.code == C_IOResultOk && destroyed != C_TcpSocketSuccess) {
        return io_result_from_tcp_create(destroyed);
    }
    return close_result;
}

C_IOResult galay_rpc_client_heartbeat(galay_rpc_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || !client->connected || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    const uint32_t request_id = client->next_request_id++;
    uint8_t heartbeat[GALAY_RPC_HEADER_SIZE];
    build_heartbeat_frame(request_id, heartbeat);
    C_IOResult sent = send_all(&client->socket, heartbeat, sizeof(heartbeat), timeout_ms);
    if (sent.code != C_IOResultOk) {
        return sent;
    }
    uint8_t response[GALAY_RPC_HEADER_SIZE];
    C_IOResult received = recv_exact(&client->socket, response, sizeof(response), timeout_ms);
    if (received.code != C_IOResultOk) {
        return received;
    }
    if (!header_is_valid(response) || response[5] != kMessageHeartbeat ||
        get32(response + 8) != request_id || get32(response + 12) != 0) {
        return make_io_result(C_IOResultError);
    }
    C_IOResult result = make_io_result(C_IOResultOk);
    result.value = request_id;
    return result;
}

C_IOResult galay_rpc_client_call(galay_rpc_client_t* client,
                                 const char* service,
                                 size_t service_len,
                                 const char* method,
                                 size_t method_len,
                                 const void* payload,
                                 size_t payload_len,
                                 int64_t timeout_ms,
                                 galay_rpc_response_buffer_t* out_response)
{
    galay_rpc_call_options_t options = galay_rpc_call_options_default();
    options.timeout_ms = timeout_ms;
    return galay_rpc_client_call_with_options(client,
                                              service,
                                              service_len,
                                              method,
                                              method_len,
                                              payload,
                                              payload_len,
                                              &options,
                                              out_response);
}

C_IOResult galay_rpc_client_call_with_options(galay_rpc_client_t* client,
                                              const char* service,
                                              size_t service_len,
                                              const char* method,
                                              size_t method_len,
                                              const void* payload,
                                              size_t payload_len,
                                              const galay_rpc_call_options_t* options,
                                              galay_rpc_response_buffer_t* out_response)
{
    const int64_t timeout_ms = options == nullptr ? -1 : options->timeout_ms;
    reset_response_buffer(out_response, GALAY_RPC_ERROR_INTERNAL_ERROR, GALAY_RPC_CALL_UNARY);
    if (out_response == nullptr || client == nullptr || !client->connected ||
        client->socket.socket == nullptr || service == nullptr || method == nullptr ||
        !payload_arg_valid(payload, payload_len)) {
        reset_response_buffer(out_response, GALAY_RPC_ERROR_CONNECTION_CLOSED, GALAY_RPC_CALL_UNARY);
        return make_io_result(C_IOResultInvalid);
    }
    if (options != nullptr && options->cancellation != nullptr &&
        options->cancellation->cancelled.load(std::memory_order_acquire)) {
        reset_response_buffer(out_response, GALAY_RPC_ERROR_CANCELLED, GALAY_RPC_CALL_UNARY);
        return make_io_result(C_IOResultCancelled);
    }
    const uint32_t request_id = client->next_request_id++;
    C_IOResult sent = send_request_frame(client,
                                         request_id,
                                         GALAY_RPC_CALL_UNARY,
                                         GALAY_TRUE,
                                         service,
                                         service_len,
                                         method,
                                         method_len,
                                         payload,
                                         payload_len,
                                         timeout_ms);
    if (sent.code != C_IOResultOk) {
        return sent;
    }
    return read_response_frame(&client->socket, timeout_ms, out_response);
}

galay_status_t galay_rpc_server_create(const galay_rpc_server_config_t* config,
                                       galay_rpc_server_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* server = allocate_handle<galay_rpc_server_t>();
    if (server == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    if (config != nullptr) {
        if (config->host != nullptr && config->host[0] != '\0') {
            server->host = config->host;
        }
        server->port = config->port;
        if (config->backlog > 0) {
            server->backlog = config->backlog;
        }
    }
    *out = server;
    return GALAY_OK;
}

void galay_rpc_server_destroy(galay_rpc_server_t* server)
{
    if (server != nullptr && server->listener.socket != nullptr) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&server->listener);
        if (destroyed != C_TcpSocketSuccess) {
            server->listening = false;
        }
    }
    destroy_handle(server);
}

galay_status_t galay_rpc_server_listen(galay_rpc_server_t* server)
{
    if (server == nullptr || server->listener.socket != nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    C_Host host{};
    if (server->host.empty() || server->host.size() >= C_HOST_ADDRESS_MAX_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::memset(&host, 0, sizeof(host));
    host.type = C_IPTypeIPV4;
    std::memcpy(host.address, server->host.data(), server->host.size());
    host.port = server->port;

    C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&server->listener, host.type);
    if (created != C_TcpSocketSuccess) {
        return status_from_tcp_create(created);
    }
    C_TcpSocketResultCode bound = galay_kernel_tcp_socket_bind(&server->listener, &host);
    if (bound != C_TcpSocketSuccess) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&server->listener);
        if (destroyed != C_TcpSocketSuccess) {
            return status_from_tcp_create(destroyed);
        }
        return status_from_tcp_create(bound);
    }
    C_TcpSocketResultCode listened =
        galay_kernel_tcp_socket_listen(&server->listener, server->backlog);
    if (listened != C_TcpSocketSuccess) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&server->listener);
        if (destroyed != C_TcpSocketSuccess) {
            return status_from_tcp_create(destroyed);
        }
        return status_from_tcp_create(listened);
    }
    server->listening = true;
    return GALAY_OK;
}

galay_status_t galay_rpc_server_local_endpoint(galay_rpc_server_t* server, C_Host* out)
{
    if (server == nullptr || out == nullptr || server->listener.socket == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return status_from_tcp_create(galay_kernel_tcp_socket_local_endpoint(&server->listener, out));
}

galay_status_t galay_rpc_server_register_service(galay_rpc_server_t* server,
                                                 galay_rpc_service_t* service)
{
    if (server == nullptr || service == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    server->services.push_back(service);
    return GALAY_OK;
}

C_IOResult galay_rpc_server_serve_one(galay_rpc_server_t* server, int64_t timeout_ms)
{
    if (server == nullptr || !server->listening || server->listener.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    galay_kernel_tcp_socket_t accepted{};
    C_IOResult accepted_result =
        galay_kernel_tcp_socket_accept(&server->listener, &accepted, nullptr, timeout_ms);
    if (accepted_result.code != C_IOResultOk) {
        return accepted_result;
    }

    C_IOResult final_result = make_io_result(C_IOResultOk);
    for (;;) {
        IncomingFrame frame;
        C_IOResult read_result = read_incoming_frame(&accepted, timeout_ms, &frame);
        if (read_result.code == C_IOResultEof) {
            break;
        }
        if (read_result.code != C_IOResultOk) {
            final_result = read_result;
            break;
        }
        if (frame.type == kMessageHeartbeat) {
            uint8_t heartbeat[GALAY_RPC_HEADER_SIZE];
            build_heartbeat_frame(frame.request_id, heartbeat);
            C_IOResult heartbeat_result = send_all(&accepted, heartbeat, sizeof(heartbeat), timeout_ms);
            if (heartbeat_result.code != C_IOResultOk) {
                final_result = heartbeat_result;
                break;
            }
            continue;
        }

        galay_rpc_response_t response{};
        response.request_id = frame.request.request_id;
        response.call_mode = frame.request.call_mode;
        response.end_of_stream = frame.request.end_of_stream;
        response.error_code = GALAY_RPC_ERROR_OK;
        galay_rpc_error_code_t handler_result = dispatch_request(server, frame.request, &response);
        if (handler_result != GALAY_RPC_ERROR_OK) {
            response.error_code = handler_result;
            response.payload = nullptr;
            response.payload_len = 0;
            response.end_of_stream = GALAY_TRUE;
        }
        C_IOResult sent = send_response(&accepted, &response, timeout_ms);
        if (sent.code != C_IOResultOk) {
            final_result = sent;
            break;
        }
    }

    C_IOResult closed = galay_kernel_tcp_socket_close(&accepted, timeout_ms);
    C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&accepted);
    if (final_result.code == C_IOResultOk && closed.code != C_IOResultOk) {
        final_result = closed;
    }
    if (final_result.code == C_IOResultOk && destroyed != C_TcpSocketSuccess) {
        final_result = io_result_from_tcp_create(destroyed);
    }
    return final_result;
}

galay_status_t galay_rpc_service_create(const char* name,
                                        size_t name_len,
                                        galay_rpc_service_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    if (galay_rpc_name_is_valid(name, name_len) != GALAY_TRUE) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* service = allocate_handle<galay_rpc_service_t>();
    if (service == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    service->name.assign(name, name_len);
    *out = service;
    return GALAY_OK;
}

void galay_rpc_service_destroy(galay_rpc_service_t* service)
{
    destroy_handle(service);
}

galay_status_t galay_rpc_service_register_unary(galay_rpc_service_t* service,
                                                const char* method,
                                                size_t method_len,
                                                galay_rpc_method_handler_fn handler,
                                                void* user_data)
{
    if (service == nullptr || handler == nullptr ||
        galay_rpc_name_is_valid(method, method_len) != GALAY_TRUE) {
        return GALAY_INVALID_ARGUMENT;
    }
    RpcRouteSlot entry;
    entry.method.assign(method, method_len);
    entry.mode = GALAY_RPC_CALL_UNARY;
    entry.handler = handler;
    entry.user_data = user_data;
    service->methods.push_back(std::move(entry));
    return GALAY_OK;
}

galay_status_t galay_rpc_service_register_streaming(galay_rpc_service_t* service,
                                                    const char* method,
                                                    size_t method_len,
                                                    galay_rpc_call_mode_t mode,
                                                    galay_rpc_method_handler_fn handler,
                                                    void* user_data)
{
    if (service == nullptr || handler == nullptr || !streaming_mode(mode) ||
        galay_rpc_name_is_valid(method, method_len) != GALAY_TRUE) {
        return GALAY_INVALID_ARGUMENT;
    }
    RpcRouteSlot entry;
    entry.method.assign(method, method_len);
    entry.mode = mode;
    entry.handler = handler;
    entry.user_data = user_data;
    service->methods.push_back(std::move(entry));
    return GALAY_OK;
}

C_IOResult galay_rpc_client_stream_open(galay_rpc_client_t* client,
                                        const char* service,
                                        size_t service_len,
                                        const char* method,
                                        size_t method_len,
                                        galay_rpc_call_mode_t mode,
                                        galay_rpc_stream_t** out_stream)
{
    if (out_stream != nullptr) {
        *out_stream = nullptr;
    }
    if (out_stream == nullptr || client == nullptr || !client->connected ||
        !streaming_mode(mode) ||
        galay_rpc_name_is_valid(service, service_len) != GALAY_TRUE ||
        galay_rpc_name_is_valid(method, method_len) != GALAY_TRUE) {
        return make_io_result(C_IOResultInvalid);
    }
    auto* stream = allocate_handle<galay_rpc_stream_t>();
    if (stream == nullptr) {
        return make_io_result(C_IOResultError);
    }
    stream->client = client;
    stream->service.assign(service, service_len);
    stream->method.assign(method, method_len);
    stream->mode = mode;
    stream->request_id = client->next_request_id++;
    *out_stream = stream;
    C_IOResult result = make_io_result(C_IOResultOk);
    result.ptr = stream;
    return result;
}

C_IOResult galay_rpc_stream_write(galay_rpc_stream_t* stream,
                                  const void* payload,
                                  size_t payload_len,
                                  galay_bool_t end_of_stream,
                                  int64_t timeout_ms)
{
    if (stream == nullptr || stream->closed || stream->client == nullptr ||
        !payload_arg_valid(payload, payload_len)) {
        return make_io_result(C_IOResultInvalid);
    }
    return send_request_frame(stream->client,
                              stream->request_id,
                              stream->mode,
                              end_of_stream,
                              stream->service.data(),
                              stream->service.size(),
                              stream->method.data(),
                              stream->method.size(),
                              payload,
                              payload_len,
                              timeout_ms);
}

C_IOResult galay_rpc_stream_read(galay_rpc_stream_t* stream,
                                 int64_t timeout_ms,
                                 galay_rpc_response_buffer_t* out_response)
{
    if (stream == nullptr || stream->closed || stream->client == nullptr ||
        out_response == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult result = read_response_frame(&stream->client->socket, timeout_ms, out_response);
    if (result.code == C_IOResultOk && out_response->end_of_stream == GALAY_TRUE) {
        stream->closed = true;
    }
    return result;
}

C_IOResult galay_rpc_stream_close(galay_rpc_stream_t* stream, int64_t timeout_ms)
{
    if (stream == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    if (stream->closed) {
        return make_io_result(C_IOResultOk);
    }
    C_IOResult result = send_request_frame(stream->client,
                                           stream->request_id,
                                           stream->mode,
                                           GALAY_TRUE,
                                           stream->service.data(),
                                           stream->service.size(),
                                           stream->method.data(),
                                           stream->method.size(),
                                           nullptr,
                                           0,
                                           timeout_ms);
    if (result.code == C_IOResultOk) {
        stream->closed = true;
    }
    return result;
}

void galay_rpc_stream_destroy(galay_rpc_stream_t* stream)
{
    destroy_handle(stream);
}

galay_status_t galay_rpc_cancellation_source_create(galay_rpc_cancellation_source_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* source = allocate_handle<galay_rpc_cancellation_source_t>();
    if (source == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    *out = source;
    return GALAY_OK;
}

void galay_rpc_cancellation_source_cancel(galay_rpc_cancellation_source_t* source)
{
    if (source != nullptr) {
        source->cancelled.store(true, std::memory_order_release);
    }
}

void galay_rpc_cancellation_source_destroy(galay_rpc_cancellation_source_t* source)
{
    destroy_handle(source);
}

void galay_rpc_response_buffer_destroy(galay_rpc_response_buffer_t* response)
{
    if (response == nullptr) {
        return;
    }
    std::free(response->payload);
    reset_response_buffer(response, GALAY_RPC_ERROR_OK, GALAY_RPC_CALL_UNARY);
}

galay_status_t galay_rpc_pool_create(const galay_rpc_pool_config_t* config,
                                     galay_rpc_pool_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* pool = allocate_handle<galay_rpc_pool_t>();
    if (pool == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    pool->config = config == nullptr ? galay_rpc_pool_config_default() : *config;
    normalize_pool_config(&pool->config);
    *out = pool;
    return GALAY_OK;
}

void galay_rpc_pool_destroy(galay_rpc_pool_t* pool)
{
    destroy_handle(pool);
}

galay_status_t galay_rpc_pool_ensure_endpoint(galay_rpc_pool_t* pool,
                                              const char* host,
                                              uint16_t port)
{
    if (pool == nullptr || !endpoint_valid(host, port)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (pool->shutdown) {
        return GALAY_IO_ERROR;
    }
    PoolBucket& bucket = bucket_for(pool, host, port);
    while (bucket.total < pool->config.min_connections_per_endpoint &&
           bucket.total < pool->config.max_connections_per_endpoint) {
        bucket.available.push_back(pool_new_connection_id(pool));
        ++bucket.total;
    }
    return GALAY_OK;
}

galay_status_t galay_rpc_pool_acquire(galay_rpc_pool_t* pool,
                                      const char* host,
                                      uint16_t port,
                                      galay_rpc_pool_lease_t** out_lease)
{
    if (out_lease != nullptr) {
        *out_lease = nullptr;
    }
    if (pool == nullptr || out_lease == nullptr || !endpoint_valid(host, port)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (pool->shutdown) {
        return GALAY_IO_ERROR;
    }
    PoolBucket& bucket = bucket_for(pool, host, port);
    uint64_t id = 0;
    if (!bucket.available.empty()) {
        id = bucket.available.back();
        bucket.available.pop_back();
    } else if (bucket.total < pool->config.max_connections_per_endpoint) {
        id = pool_new_connection_id(pool);
        ++bucket.total;
    } else {
        return GALAY_OUT_OF_MEMORY;
    }
    ++bucket.in_use;
    auto* lease = allocate_handle<galay_rpc_pool_lease_t>();
    if (lease == nullptr) {
        --bucket.in_use;
        bucket.available.push_back(id);
        return GALAY_OUT_OF_MEMORY;
    }
    lease->host = host;
    lease->port = port;
    lease->id = id;
    lease->valid = true;
    *out_lease = lease;
    return GALAY_OK;
}

galay_status_t galay_rpc_pool_release(galay_rpc_pool_t* pool,
                                      galay_rpc_pool_lease_t* lease,
                                      galay_bool_t broken)
{
    if (pool == nullptr || lease == nullptr || !lease->valid) {
        return GALAY_INVALID_ARGUMENT;
    }
    PoolBucket* bucket = nullptr;
    const std::string key = endpoint_key(lease->host.c_str(), lease->port);
    auto it = pool->buckets.find(key);
    if (it != pool->buckets.end()) {
        bucket = &it->second;
    }
    if (bucket == nullptr) {
        destroy_handle(lease);
        return GALAY_INVALID_ARGUMENT;
    }
    if (bucket->in_use > 0) {
        --bucket->in_use;
    }
    if (pool->shutdown || broken == GALAY_TRUE) {
        if (bucket->total > 0) {
            --bucket->total;
        }
    } else {
        bucket->available.push_back(lease->id);
    }
    lease->valid = false;
    destroy_handle(lease);
    return GALAY_OK;
}

galay_status_t galay_rpc_pool_shutdown(galay_rpc_pool_t* pool)
{
    if (pool == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    pool->shutdown = true;
    for (auto& [key, bucket] : pool->buckets) {
        bucket.available.clear();
        bucket.in_use = 0;
        bucket.total = 0;
    }
    return GALAY_OK;
}

galay_status_t galay_rpc_pool_available_count(const galay_rpc_pool_t* pool,
                                              const char* host,
                                              uint16_t port,
                                              size_t* out_count)
{
    if (out_count != nullptr) {
        *out_count = 0;
    }
    if (pool == nullptr || out_count == nullptr || !endpoint_valid(host, port)) {
        return GALAY_INVALID_ARGUMENT;
    }
    const PoolBucket* bucket = find_bucket(pool, host, port);
    *out_count = bucket == nullptr ? 0 : bucket->available.size();
    return GALAY_OK;
}

galay_status_t galay_rpc_pool_in_use_count(const galay_rpc_pool_t* pool,
                                           const char* host,
                                           uint16_t port,
                                           size_t* out_count)
{
    if (out_count != nullptr) {
        *out_count = 0;
    }
    if (pool == nullptr || out_count == nullptr || !endpoint_valid(host, port)) {
        return GALAY_INVALID_ARGUMENT;
    }
    const PoolBucket* bucket = find_bucket(pool, host, port);
    *out_count = bucket == nullptr ? 0 : bucket->in_use;
    return GALAY_OK;
}

galay_status_t galay_rpc_pool_lease_id(const galay_rpc_pool_lease_t* lease, uint64_t* out_id)
{
    if (out_id != nullptr) {
        *out_id = 0;
    }
    if (lease == nullptr || out_id == nullptr || !lease->valid) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_id = lease->id;
    return GALAY_OK;
}

}
