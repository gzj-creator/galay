#include <galay/c/galay-redis-c/redis.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace
{

constexpr size_t kRedisMaxReplyBuffer = 1024 * 1024;

C_IOResult make_io_result(C_IOResultCode code, int64_t value = 0)
{
    return C_IOResult{code, 0, 0, value, nullptr};
}

C_IOResult io_result_from_status(galay_status_t status)
{
    return make_io_result(status == GALAY_INVALID_ARGUMENT ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(status));
}

C_IOResult io_result_from_socket_create(C_TcpSocketResultCode code)
{
    return make_io_result(code == C_TcpSocketParameterInvalid ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(code));
}

bool copy_host_to_c_host(const std::string& host, uint16_t port, C_Host* out)
{
    if (out == nullptr || host.empty() || host.size() >= sizeof(out->address) || port == 0) {
        return false;
    }
    out->type = host.find(':') == std::string::npos ? C_IPTypeIPV4 : C_IPTypeIPV6;
    std::memset(out->address, 0, sizeof(out->address));
    std::memcpy(out->address, host.data(), host.size());
    out->port = port;
    return true;
}

bool reply_may_be_incomplete(const std::string& buffer, galay_status_t status)
{
    if (status == GALAY_INVALID_ARGUMENT) {
        return true;
    }
    if (buffer.empty()) {
        return true;
    }
    if (buffer[0] == '+') {
        return buffer.find("\r\n") == std::string::npos;
    }
    if (buffer[0] == '*') {
        return buffer.size() < 4;
    }
    return false;
}

} // namespace

struct galay_redis_command_builder_t {
    std::string encoded;
};

struct galay_redis_pipeline_t {
    std::vector<std::string> encoded_commands;
};

struct galay_redis_reply_t {
    galay_redis_resp_type_t type = GALAY_REDIS_RESP_SIMPLE_STRING;
    std::string value;
    std::vector<galay_redis_reply_t*> array;
};

struct galay_redis_client_t {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    std::string username;
    std::string password;
    int db_index = 0;
    int resp_version = 2;
    int connect_timeout_ms = -1;
    galay_kernel_tcp_socket_t socket{};
    bool connected = false;
    std::string recv_buffer;
};

extern "C" {

const char* galay_redis_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_redis_command_builder_create(galay_redis_command_builder_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_redis_command_builder_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_redis_command_builder_destroy(galay_redis_command_builder_t* builder)
{
    delete builder;
}

galay_status_t galay_redis_command_builder_build(galay_redis_command_builder_t* builder,
                                                 const char* command,
                                                 const char* const* args,
                                                 const size_t* arg_lens,
                                                 size_t arg_count,
                                                 const char** encoded,
                                                 size_t* encoded_len)
{
    if (encoded != nullptr) {
        *encoded = nullptr;
    }
    if (encoded_len != nullptr) {
        *encoded_len = 0;
    }
    if (builder == nullptr || command == nullptr || encoded == nullptr || encoded_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    builder->encoded = "*" + std::to_string(arg_count + 1) + "\r\n";
    const size_t command_len = std::strlen(command);
    builder->encoded += "$" + std::to_string(command_len) + "\r\n";
    builder->encoded.append(command, command_len);
    builder->encoded += "\r\n";
    for (size_t i = 0; i < arg_count; ++i) {
        if (args == nullptr || args[i] == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }
        const size_t len = arg_lens == nullptr ? std::strlen(args[i]) : arg_lens[i];
        builder->encoded += "$" + std::to_string(len) + "\r\n";
        builder->encoded.append(args[i], len);
        builder->encoded += "\r\n";
    }
    *encoded = builder->encoded.data();
    *encoded_len = builder->encoded.size();
    return GALAY_OK;
}

galay_status_t galay_redis_pipeline_create(galay_redis_pipeline_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_redis_pipeline_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_redis_pipeline_destroy(galay_redis_pipeline_t* pipeline)
{
    delete pipeline;
}

galay_status_t galay_redis_pipeline_add_command(galay_redis_pipeline_t* pipeline,
                                                const char* command,
                                                const char* const* args,
                                                const size_t* arg_lens,
                                                size_t arg_count)
{
    if (pipeline == nullptr || command == nullptr || (arg_count != 0 && args == nullptr)) {
        return GALAY_INVALID_ARGUMENT;
    }

    galay_redis_command_builder_t builder;
    const char* encoded = nullptr;
    size_t encoded_len = 0;
    galay_status_t built = galay_redis_command_builder_build(&builder,
                                                             command,
                                                             args,
                                                             arg_lens,
                                                             arg_count,
                                                             &encoded,
                                                             &encoded_len);
    if (built != GALAY_OK) {
        return built;
    }
    pipeline->encoded_commands.emplace_back(encoded, encoded_len);
    return GALAY_OK;
}

galay_status_t galay_redis_parse_reply(const char* data, size_t data_len,
                                       galay_redis_reply_t** out, size_t* consumed)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (data == nullptr || out == nullptr || consumed == nullptr || data_len < 4) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data[0] == '+') {
        const char* end = static_cast<const char*>(std::memchr(data, '\r', data_len));
        if (end == nullptr || end + 1 >= data + data_len || end[1] != '\n') {
            return GALAY_PROTOCOL_ERROR;
        }
        auto* reply = new (std::nothrow) galay_redis_reply_t();
        if (reply == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        reply->type = GALAY_REDIS_RESP_SIMPLE_STRING;
        reply->value.assign(data + 1, static_cast<size_t>(end - data - 1));
        *consumed = static_cast<size_t>(end - data + 2);
        *out = reply;
        return GALAY_OK;
    }
    if (data[0] == '*' && data_len == 4 && data[1] == '0' && data[2] == '\r' && data[3] == '\n') {
        auto* reply = new (std::nothrow) galay_redis_reply_t();
        if (reply == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        reply->type = GALAY_REDIS_RESP_ARRAY;
        *consumed = 4;
        *out = reply;
        return GALAY_OK;
    }
    return GALAY_PROTOCOL_ERROR;
}

void galay_redis_reply_destroy(galay_redis_reply_t* reply)
{
    if (reply == nullptr) {
        return;
    }
    for (auto* child : reply->array) {
        galay_redis_reply_destroy(child);
    }
    delete reply;
}

void galay_redis_pipeline_replies_destroy(galay_redis_reply_t** replies, size_t reply_count)
{
    if (replies == nullptr) {
        return;
    }
    for (size_t i = 0; i < reply_count; ++i) {
        galay_redis_reply_destroy(replies[i]);
    }
    std::free(replies);
}

galay_redis_resp_type_t galay_redis_reply_type(const galay_redis_reply_t* reply)
{
    return reply == nullptr ? GALAY_REDIS_RESP_ERROR : reply->type;
}

galay_status_t galay_redis_reply_string(const galay_redis_reply_t* reply, const char** value,
                                        size_t* value_len)
{
    if (reply == nullptr || value == nullptr || value_len == nullptr ||
        reply->type != GALAY_REDIS_RESP_SIMPLE_STRING) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = reply->value.data();
    *value_len = reply->value.size();
    return GALAY_OK;
}

galay_status_t galay_redis_reply_array_size(const galay_redis_reply_t* reply, size_t* size)
{
    if (reply == nullptr || size == nullptr || reply->type != GALAY_REDIS_RESP_ARRAY) {
        return GALAY_INVALID_ARGUMENT;
    }
    *size = reply->array.size();
    return GALAY_OK;
}

galay_status_t galay_redis_client_create(const galay_redis_client_config_t* config,
                                         galay_redis_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (config != nullptr && config->username != nullptr && config->password == nullptr) {
        *out = nullptr;
        return GALAY_INVALID_ARGUMENT;
    }
    auto* client = new (std::nothrow) galay_redis_client_t();
    if (client == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    if (config != nullptr) {
        if (config->host != nullptr && config->host[0] != '\0') {
            client->host = config->host;
        }
        if (config->port != 0) {
            client->port = config->port;
        }
        if (config->username != nullptr) {
            client->username = config->username;
        }
        if (config->password != nullptr) {
            client->password = config->password;
        }
        client->db_index = config->db_index;
        client->resp_version = config->resp_version;
        client->connect_timeout_ms = config->connect_timeout_ms;
    }
    *out = client;
    return GALAY_OK;
}

void galay_redis_client_destroy(galay_redis_client_t* client)
{
    if (client != nullptr && client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
        }
    }
    delete client;
}

galay_status_t galay_redis_client_disconnect(galay_redis_client_t* client)
{
    if (client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            return GALAY_IO_ERROR;
        }
    }
    client->connected = false;
    return GALAY_OK;
}

galay_status_t galay_redis_client_command(galay_redis_client_t* client, const char* command,
                                          const char* const* args, const size_t* arg_lens,
                                          size_t arg_count, galay_redis_reply_t** reply)
{
    if (reply != nullptr) {
        *reply = nullptr;
    }
    if (client == nullptr || command == nullptr || reply == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (arg_count != 0 && args == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_UNSUPPORTED;
}

C_IOResult galay_redis_client_connect(galay_redis_client_t* client, int64_t timeout_ms)
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
        return io_result_from_socket_create(created);
    }

    const int64_t effective_timeout =
        timeout_ms < 0 && client->connect_timeout_ms > 0 ? client->connect_timeout_ms : timeout_ms;
    C_IOResult connected = galay_kernel_tcp_socket_connect(&client->socket, &host, effective_timeout);
    if (connected.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess && connected.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        client->connected = false;
        return connected;
    }
    client->connected = true;
    connected.ptr = client;
    return connected;
}

C_IOResult galay_redis_client_command_async(galay_redis_client_t* client,
                                            const char* command,
                                            const char* const* args,
                                            const size_t* arg_lens,
                                            size_t arg_count,
                                            int64_t timeout_ms,
                                            galay_redis_reply_t** reply)
{
    if (reply != nullptr) {
        *reply = nullptr;
    }
    if (client == nullptr || command == nullptr || reply == nullptr ||
        !client->connected || client->socket.socket == nullptr ||
        (arg_count != 0 && args == nullptr)) {
        return make_io_result(C_IOResultInvalid);
    }

    galay_redis_command_builder_t builder;
    const char* encoded = nullptr;
    size_t encoded_len = 0;
    galay_status_t built = galay_redis_command_builder_build(&builder,
                                                             command,
                                                             args,
                                                             arg_lens,
                                                             arg_count,
                                                             &encoded,
                                                             &encoded_len);
    if (built != GALAY_OK) {
        return io_result_from_status(built);
    }

    size_t sent = 0;
    while (sent < encoded_len) {
        C_IOResult send_result = galay_kernel_tcp_socket_send(&client->socket,
                                                              encoded + sent,
                                                              encoded_len - sent,
                                                              timeout_ms);
        if (send_result.code != C_IOResultOk) {
            return send_result;
        }
        if (send_result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        sent += send_result.bytes;
    }

    client->recv_buffer.clear();
    char chunk[4096];
    for (;;) {
        C_IOResult recv_result =
            galay_kernel_tcp_socket_recv(&client->socket, chunk, sizeof(chunk), timeout_ms);
        if (recv_result.code != C_IOResultOk) {
            return recv_result;
        }
        if (recv_result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        client->recv_buffer.append(chunk, recv_result.bytes);
        if (client->recv_buffer.size() > kRedisMaxReplyBuffer) {
            return io_result_from_status(GALAY_PROTOCOL_ERROR);
        }

        size_t consumed = 0;
        galay_redis_reply_t* parsed = nullptr;
        galay_status_t parsed_status = galay_redis_parse_reply(client->recv_buffer.data(),
                                                               client->recv_buffer.size(),
                                                               &parsed,
                                                               &consumed);
        if (parsed_status == GALAY_OK) {
            *reply = parsed;
            C_IOResult result = make_io_result(C_IOResultOk);
            result.bytes = consumed;
            result.ptr = parsed;
            return result;
        }
        if (!reply_may_be_incomplete(client->recv_buffer, parsed_status)) {
            return io_result_from_status(parsed_status);
        }
    }
}

C_IOResult galay_redis_client_auth(galay_redis_client_t* client,
                                   const char* username,
                                   const char* password,
                                   int64_t timeout_ms)
{
    if (client == nullptr || password == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    const bool has_username = username != nullptr && username[0] != '\0';
    const char* args[2] = {nullptr, nullptr};
    size_t arg_count = 0;
    if (has_username) {
        args[0] = username;
        args[1] = password;
        arg_count = 2;
    } else {
        args[0] = password;
        arg_count = 1;
    }

    galay_redis_reply_t* reply = nullptr;
    C_IOResult result =
        galay_redis_client_command_async(client, "AUTH", args, nullptr, arg_count, timeout_ms, &reply);
    if (result.code != C_IOResultOk) {
        return result;
    }

    const char* value = nullptr;
    size_t value_len = 0;
    const galay_status_t string_status = galay_redis_reply_string(reply, &value, &value_len);
    const bool ok = string_status == GALAY_OK && value_len == 2 &&
        std::memcmp(value, "OK", value_len) == 0;
    galay_redis_reply_destroy(reply);
    return ok ? result : io_result_from_status(GALAY_PROTOCOL_ERROR);
}

C_IOResult galay_redis_client_select(galay_redis_client_t* client, int db_index, int64_t timeout_ms)
{
    if (client == nullptr || db_index < 0) {
        return make_io_result(C_IOResultInvalid);
    }

    std::string db = std::to_string(db_index);
    const char* args[] = {db.c_str()};
    galay_redis_reply_t* reply = nullptr;
    C_IOResult result =
        galay_redis_client_command_async(client, "SELECT", args, nullptr, 1, timeout_ms, &reply);
    if (result.code != C_IOResultOk) {
        return result;
    }

    const char* value = nullptr;
    size_t value_len = 0;
    const galay_status_t string_status = galay_redis_reply_string(reply, &value, &value_len);
    const bool ok = string_status == GALAY_OK && value_len == 2 &&
        std::memcmp(value, "OK", value_len) == 0;
    galay_redis_reply_destroy(reply);
    return ok ? result : io_result_from_status(GALAY_PROTOCOL_ERROR);
}

C_IOResult galay_redis_client_pipeline_async(galay_redis_client_t* client,
                                             const galay_redis_pipeline_t* pipeline,
                                             int64_t timeout_ms,
                                             galay_redis_reply_t*** replies,
                                             size_t* reply_count)
{
    if (replies != nullptr) {
        *replies = nullptr;
    }
    if (reply_count != nullptr) {
        *reply_count = 0;
    }
    if (client == nullptr || pipeline == nullptr || replies == nullptr || reply_count == nullptr ||
        !client->connected || client->socket.socket == nullptr ||
        pipeline->encoded_commands.empty()) {
        return make_io_result(C_IOResultInvalid);
    }

    for (const std::string& encoded : pipeline->encoded_commands) {
        size_t sent = 0;
        while (sent < encoded.size()) {
            C_IOResult send_result = galay_kernel_tcp_socket_send(&client->socket,
                                                                  encoded.data() + sent,
                                                                  encoded.size() - sent,
                                                                  timeout_ms);
            if (send_result.code != C_IOResultOk) {
                return send_result;
            }
            if (send_result.bytes == 0) {
                return make_io_result(C_IOResultEof);
            }
            sent += send_result.bytes;
        }
    }

    const size_t expected_count = pipeline->encoded_commands.size();
    auto** parsed_replies =
        static_cast<galay_redis_reply_t**>(std::calloc(expected_count,
                                                       sizeof(galay_redis_reply_t*)));
    if (parsed_replies == nullptr) {
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }

    client->recv_buffer.clear();
    char chunk[4096];
    size_t parsed_count = 0;
    while (parsed_count < expected_count) {
        size_t consumed = 0;
        galay_redis_reply_t* parsed = nullptr;
        galay_status_t parsed_status = galay_redis_parse_reply(client->recv_buffer.data(),
                                                               client->recv_buffer.size(),
                                                               &parsed,
                                                               &consumed);
        if (parsed_status == GALAY_OK) {
            parsed_replies[parsed_count] = parsed;
            ++parsed_count;
            client->recv_buffer.erase(0, consumed);
            continue;
        }
        if (!reply_may_be_incomplete(client->recv_buffer, parsed_status)) {
            galay_redis_pipeline_replies_destroy(parsed_replies, expected_count);
            return io_result_from_status(parsed_status);
        }

        C_IOResult recv_result =
            galay_kernel_tcp_socket_recv(&client->socket, chunk, sizeof(chunk), timeout_ms);
        if (recv_result.code != C_IOResultOk) {
            galay_redis_pipeline_replies_destroy(parsed_replies, expected_count);
            return recv_result;
        }
        if (recv_result.bytes == 0) {
            galay_redis_pipeline_replies_destroy(parsed_replies, expected_count);
            return make_io_result(C_IOResultEof);
        }
        client->recv_buffer.append(chunk, recv_result.bytes);
        if (client->recv_buffer.size() > kRedisMaxReplyBuffer) {
            galay_redis_pipeline_replies_destroy(parsed_replies, expected_count);
            return io_result_from_status(GALAY_PROTOCOL_ERROR);
        }
    }

    *replies = parsed_replies;
    *reply_count = expected_count;
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = expected_count;
    result.ptr = parsed_replies;
    return result;
}

C_IOResult galay_redis_client_close(galay_redis_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    C_IOResult close_result = galay_kernel_tcp_socket_close(&client->socket, timeout_ms);
    const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
    client->connected = false;
    client->recv_buffer.clear();
    if (close_result.code == C_IOResultOk && destroyed != C_TcpSocketSuccess) {
        return io_result_from_socket_create(destroyed);
    }
    return close_result;
}

}
