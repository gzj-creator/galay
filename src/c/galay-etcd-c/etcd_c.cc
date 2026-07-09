#include <galay/c/galay-etcd-c/etcd_c.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr int64_t kDefaultTimeoutMs = 1000;
constexpr size_t kMaxHttpResponseBytes = 1024 * 1024;

struct EtcdKv {
    std::string key;
    std::string value;
};

struct ParsedEndpoint {
    std::string host;
    std::string host_header;
    uint16_t port = 0;
};

struct HttpResponse {
    std::string body;
    int status = 0;
};

bool is_http_endpoint(std::string_view endpoint)
{
    return endpoint.rfind("http://", 0) == 0;
}

bool parse_endpoint(std::string_view endpoint, ParsedEndpoint& parsed)
{
    if (!is_http_endpoint(endpoint)) {
        return false;
    }
    std::string_view rest = endpoint.substr(7);
    const size_t slash = rest.find('/');
    if (slash != std::string_view::npos) {
        rest = rest.substr(0, slash);
    }
    if (rest.empty()) {
        return false;
    }

    const size_t colon = rest.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= rest.size()) {
        return false;
    }

    uint32_t port = 0;
    const std::string_view port_text = rest.substr(colon + 1);
    const char* port_begin = port_text.data();
    const char* port_end = port_begin + port_text.size();
    auto [ptr, ec] = std::from_chars(port_begin, port_end, port);
    if (ec != std::errc() || ptr != port_end || port == 0 || port > 65535) {
        return false;
    }

    parsed.host.assign(rest.substr(0, colon));
    parsed.port = static_cast<uint16_t>(port);
    parsed.host_header.assign(rest);
    return parsed.host.size() < C_HOST_ADDRESS_MAX_LENGTH;
}

bool endpoint_to_host(const ParsedEndpoint& endpoint, C_Host& host)
{
    if (endpoint.host.empty() || endpoint.host.size() >= sizeof(host.address)) {
        return false;
    }
    host.type = endpoint.host.find(':') == std::string::npos ? C_IPTypeIPV4 : C_IPTypeIPV6;
    std::fill(std::begin(host.address), std::end(host.address), '\0');
    std::copy(endpoint.host.begin(), endpoint.host.end(), host.address);
    host.port = endpoint.port;
    return true;
}

galay_status_t status_from_io(C_IOResult result)
{
    switch (result.code) {
        case C_IOResultOk:
            return GALAY_OK;
        case C_IOResultInvalid:
            return GALAY_INVALID_ARGUMENT;
        case C_IOResultEof:
        case C_IOResultTimeout:
        case C_IOResultCancelled:
        case C_IOResultError:
            return GALAY_IO_ERROR;
    }
    return GALAY_INTERNAL_ERROR;
}

galay_etcd_error_code_t error_from_status(galay_status_t status)
{
    switch (status) {
        case GALAY_OK:
            return GALAY_ETCD_ERROR_SUCCESS;
        case GALAY_INVALID_ARGUMENT:
            return GALAY_ETCD_ERROR_INVALID_ARGUMENT;
        case GALAY_PROTOCOL_ERROR:
            return GALAY_ETCD_ERROR_PROTOCOL;
        case GALAY_CANCELLED:
            return GALAY_ETCD_ERROR_CANCELLED;
        case GALAY_EOF:
        case GALAY_TIMEOUT:
        case GALAY_IO_ERROR:
            return GALAY_ETCD_ERROR_IO;
        case GALAY_NOT_FOUND:
        case GALAY_OUT_OF_MEMORY:
        case GALAY_UNSUPPORTED:
        case GALAY_INTERNAL_ERROR:
            return GALAY_ETCD_ERROR_IO;
    }
    return GALAY_ETCD_ERROR_IO;
}

void set_code(galay_etcd_error_code_t* code, galay_etcd_error_code_t value)
{
    if (code != nullptr) {
        *code = value;
    }
}

std::string base64_encode(std::string_view input)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i < input.size()) {
        const uint32_t b0 = static_cast<unsigned char>(input[i++]);
        const bool has_b1 = i < input.size();
        const uint32_t b1 = has_b1 ? static_cast<unsigned char>(input[i++]) : 0;
        const bool has_b2 = i < input.size();
        const uint32_t b2 = has_b2 ? static_cast<unsigned char>(input[i++]) : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        out.push_back(has_b1 ? kAlphabet[(triple >> 6) & 0x3f] : '=');
        out.push_back(has_b2 ? kAlphabet[triple & 0x3f] : '=');
    }
    return out;
}

std::optional<unsigned char> base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<unsigned char>(ch - 'A');
    }
    if (ch >= 'a' && ch <= 'z') {
        return static_cast<unsigned char>(ch - 'a' + 26);
    }
    if (ch >= '0' && ch <= '9') {
        return static_cast<unsigned char>(ch - '0' + 52);
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return std::nullopt;
}

std::optional<std::string> base64_decode(std::string_view input)
{
    if (input.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve((input.size() / 4) * 3);
    for (size_t i = 0; i < input.size(); i += 4) {
        const bool pad2 = input[i + 2] == '=';
        const bool pad3 = input[i + 3] == '=';
        const std::optional<unsigned char> v0 = base64_value(input[i]);
        const std::optional<unsigned char> v1 = base64_value(input[i + 1]);
        const std::optional<unsigned char> v2 = pad2 ? std::optional<unsigned char>(0) : base64_value(input[i + 2]);
        const std::optional<unsigned char> v3 = pad3 ? std::optional<unsigned char>(0) : base64_value(input[i + 3]);
        if (!v0.has_value() || !v1.has_value() || !v2.has_value() || !v3.has_value()) {
            return std::nullopt;
        }
        const uint32_t triple =
            (static_cast<uint32_t>(*v0) << 18) |
            (static_cast<uint32_t>(*v1) << 12) |
            (static_cast<uint32_t>(*v2) << 6) |
            static_cast<uint32_t>(*v3);
        out.push_back(static_cast<char>((triple >> 16) & 0xff));
        if (!pad2) {
            out.push_back(static_cast<char>((triple >> 8) & 0xff));
        }
        if (!pad3) {
            out.push_back(static_cast<char>(triple & 0xff));
        }
    }
    return out;
}

std::string make_prefix_range_end(std::string_view key)
{
    std::string range_end(key);
    for (size_t i = range_end.size(); i > 0; --i) {
        unsigned char value = static_cast<unsigned char>(range_end[i - 1]);
        if (value != 0xff) {
            range_end[i - 1] = static_cast<char>(value + 1);
            range_end.resize(i);
            return range_end;
        }
    }
    return std::string(1, '\0');
}

void append_json_string(std::string& out, std::string_view value)
{
    out.push_back('"');
    for (char ch : value) {
        switch (ch) {
            case '\\':
            case '"':
                out.push_back('\\');
                out.push_back(ch);
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    out.push_back('"');
}

std::optional<std::string_view> extract_json_string_view(std::string_view body,
                                                         std::string_view field,
                                                         size_t start)
{
    std::string needle;
    needle.reserve(field.size() + 2);
    needle.push_back('"');
    needle.append(field);
    needle.push_back('"');
    const size_t field_pos = body.find(needle, start);
    if (field_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t colon = body.find(':', field_pos + needle.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    size_t quote = body.find('"', colon + 1);
    if (quote == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t end = body.find('"', quote + 1);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return body.substr(quote + 1, end - quote - 1);
}

std::optional<int64_t> extract_json_i64(std::string_view body, std::string_view field)
{
    if (std::optional<std::string_view> text = extract_json_string_view(body, field, 0);
        text.has_value()) {
        int64_t value = 0;
        const char* begin = text->data();
        const char* end = begin + text->size();
        auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec == std::errc() && ptr == end) {
            return value;
        }
    }

    std::string needle;
    needle.reserve(field.size() + 2);
    needle.push_back('"');
    needle.append(field);
    needle.push_back('"');
    const size_t field_pos = body.find(needle);
    if (field_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t colon = body.find(':', field_pos + needle.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    size_t begin_pos = colon + 1;
    while (begin_pos < body.size() && body[begin_pos] == ' ') {
        ++begin_pos;
    }
    size_t end_pos = begin_pos;
    while (end_pos < body.size() &&
           ((body[end_pos] >= '0' && body[end_pos] <= '9') || body[end_pos] == '-')) {
        ++end_pos;
    }
    if (end_pos == begin_pos) {
        return std::nullopt;
    }
    int64_t value = 0;
    const char* begin = body.data() + begin_pos;
    const char* end = body.data() + end_pos;
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::vector<EtcdKv> parse_kvs(std::string_view body)
{
    std::vector<EtcdKv> kvs;
    size_t pos = 0;
    for (;;) {
        std::optional<std::string_view> key_text = extract_json_string_view(body, "key", pos);
        if (!key_text.has_value()) {
            break;
        }
        const size_t value_start = static_cast<size_t>(key_text->data() - body.data()) + key_text->size();
        std::optional<std::string_view> value_text = extract_json_string_view(body, "value", value_start);
        if (!value_text.has_value()) {
            break;
        }
        std::optional<std::string> key = base64_decode(*key_text);
        std::optional<std::string> value = base64_decode(*value_text);
        if (!key.has_value() || !value.has_value()) {
            break;
        }
        kvs.push_back(EtcdKv{std::move(*key), std::move(*value)});
        pos = static_cast<size_t>(value_text->data() - body.data()) + value_text->size();
    }
    return kvs;
}

std::optional<size_t> parse_content_length(std::string_view headers)
{
    constexpr std::string_view kHeader = "Content-Length:";
    size_t pos = headers.find(kHeader);
    if (pos == std::string_view::npos) {
        pos = headers.find("content-length:");
    }
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += kHeader.size();
    while (pos < headers.size() && headers[pos] == ' ') {
        ++pos;
    }
    const size_t end = headers.find("\r\n", pos);
    const std::string_view text = headers.substr(pos, end == std::string_view::npos ? end : end - pos);
    uint64_t parsed = 0;
    const char* begin = text.data();
    const char* finish = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, finish, parsed);
    if (ec != std::errc() || ptr != finish || parsed > kMaxHttpResponseBytes) {
        return std::nullopt;
    }
    return static_cast<size_t>(parsed);
}

bool parse_http_response(std::string_view raw, HttpResponse& response)
{
    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return false;
    }
    const std::string_view headers = raw.substr(0, header_end + 2);
    if (headers.rfind("HTTP/1.1 ", 0) != 0 && headers.rfind("HTTP/1.0 ", 0) != 0) {
        return false;
    }
    int status = 0;
    const char* status_begin = headers.data() + 9;
    const char* status_end = status_begin + 3;
    auto [ptr, ec] = std::from_chars(status_begin, status_end, status);
    if (ec != std::errc() || ptr != status_end) {
        return false;
    }
    std::optional<size_t> length = parse_content_length(headers);
    if (!length.has_value()) {
        return false;
    }
    const size_t body_begin = header_end + 4;
    if (body_begin + *length > raw.size()) {
        return false;
    }
    response.status = status;
    response.body.assign(raw.substr(body_begin, *length));
    return true;
}

} // namespace

struct galay_etcd_config_builder_t {
    std::string endpoint = "http://127.0.0.1:2379";
    std::string api_prefix = "/v3";
    galay_etcd_endpoint_policy_t endpoint_policy = GALAY_ETCD_ENDPOINT_POLICY_FIRST_HEALTHY;
};

struct galay_etcd_client_t {
    std::string endpoint;
    std::string api_prefix;
    ParsedEndpoint parsed;
    galay_kernel_tcp_socket_t socket{};
    std::string recv_buffer;
    galay_etcd_client_stats_t stats{};
    galay_etcd_endpoint_policy_t endpoint_policy = GALAY_ETCD_ENDPOINT_POLICY_FIRST_HEALTHY;
    bool connected = false;
};

struct galay_etcd_get_result_t {
    std::vector<EtcdKv> kvs;
};

struct PipelineOp {
    std::string key;
    std::string value;
    int64_t limit = 0;
    int64_t lease_id = 0;
    galay_etcd_pipeline_op_type_t type = GALAY_ETCD_PIPELINE_PUT;
    bool prefix = false;
};

struct PipelineItem {
    galay_etcd_get_result_t get_result;
    int64_t deleted_count = 0;
    galay_etcd_pipeline_op_type_t type = GALAY_ETCD_PIPELINE_PUT;
};

struct galay_etcd_pipeline_t {
    std::vector<PipelineOp> operations;
};

struct galay_etcd_pipeline_result_t {
    std::vector<PipelineItem> items;
};

struct galay_etcd_watch_t {
    galay_etcd_client_t* client = nullptr;
    std::string key;
    bool prefix = false;
    bool cancelled = false;
};

struct galay_etcd_watch_event_t {
    std::string key;
    std::string value;
    int64_t watch_id = 0;
    galay_etcd_watch_event_type_t type = GALAY_ETCD_WATCH_EVENT_UNKNOWN;
};

namespace
{

galay_status_t send_all(galay_etcd_client_t* client, std::string_view data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        C_IOResult result = galay_kernel_tcp_socket_send(&client->socket,
                                                         data.data() + sent,
                                                         data.size() - sent,
                                                         kDefaultTimeoutMs);
        if (result.code != C_IOResultOk) {
            return status_from_io(result);
        }
        if (result.bytes == 0) {
            return GALAY_IO_ERROR;
        }
        sent += result.bytes;
    }
    return GALAY_OK;
}

galay_status_t read_http_response(galay_etcd_client_t* client, HttpResponse& response)
{
    client->recv_buffer.clear();
    char chunk[4096];
    for (;;) {
        C_IOResult result =
            galay_kernel_tcp_socket_recv(&client->socket, chunk, sizeof(chunk), kDefaultTimeoutMs);
        if (result.code != C_IOResultOk) {
            return status_from_io(result);
        }
        if (result.bytes == 0) {
            return GALAY_IO_ERROR;
        }
        client->recv_buffer.append(chunk, result.bytes);
        if (client->recv_buffer.size() > kMaxHttpResponseBytes) {
            return GALAY_PROTOCOL_ERROR;
        }

        const size_t header_end = client->recv_buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            continue;
        }
        std::optional<size_t> content_length =
            parse_content_length(std::string_view(client->recv_buffer).substr(0, header_end + 2));
        if (!content_length.has_value()) {
            return GALAY_PROTOCOL_ERROR;
        }
        const size_t total = header_end + 4 + *content_length;
        if (client->recv_buffer.size() < total) {
            continue;
        }
        if (!parse_http_response(std::string_view(client->recv_buffer).substr(0, total), response)) {
            return GALAY_PROTOCOL_ERROR;
        }
        return response.status >= 200 && response.status < 300 ? GALAY_OK : GALAY_IO_ERROR;
    }
}

galay_status_t post_json(galay_etcd_client_t* client,
                         std::string_view api_path,
                         std::string_view body,
                         HttpResponse& response)
{
    if (client == nullptr || !client->connected || client->socket.socket == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    ++client->stats.requests;
    std::string request;
    request.reserve(client->api_prefix.size() + api_path.size() + body.size() + 256);
    request += "POST ";
    request += client->api_prefix;
    request += api_path;
    request += " HTTP/1.1\r\nHost: ";
    request += client->parsed.host_header;
    request += "\r\nAccept: application/json\r\nConnection: keep-alive\r\n"
               "Content-Type: application/json\r\nContent-Length: ";
    request += std::to_string(body.size());
    request += "\r\n\r\n";
    request.append(body);

    galay_status_t sent = send_all(client, request);
    if (sent != GALAY_OK) {
        ++client->stats.request_failures;
        return sent;
    }
    galay_status_t read = read_http_response(client, response);
    if (read != GALAY_OK) {
        ++client->stats.request_failures;
    }
    return read;
}

std::string build_put_body(std::string_view key, std::string_view value, int64_t lease_id)
{
    std::string body;
    body += "{\"key\":";
    append_json_string(body, base64_encode(key));
    body += ",\"value\":";
    append_json_string(body, base64_encode(value));
    if (lease_id > 0) {
        body += ",\"lease\":";
        body += std::to_string(lease_id);
    }
    body.push_back('}');
    return body;
}

std::string build_key_body(std::string_view key, galay_bool_t prefix, int64_t limit)
{
    std::string body;
    body += "{\"key\":";
    append_json_string(body, base64_encode(key));
    if (prefix == GALAY_TRUE) {
        body += ",\"range_end\":";
        append_json_string(body, base64_encode(make_prefix_range_end(key)));
    }
    if (limit > 0) {
        body += ",\"limit\":";
        body += std::to_string(limit);
    }
    body.push_back('}');
    return body;
}

galay_status_t put_impl(galay_etcd_client_t* client,
                        const char* key,
                        const char* value,
                        size_t value_len,
                        int64_t lease_id,
                        galay_etcd_error_code_t* code)
{
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || key == nullptr || key[0] == '\0' ||
        (value == nullptr && value_len != 0) || lease_id < 0) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    if (!client->connected) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }
    std::string body = build_put_body(key, std::string_view(value == nullptr ? "" : value, value_len), lease_id);

    HttpResponse response;
    galay_status_t status = post_json(client, "/kv/put", body, response);
    if (status != GALAY_OK) {
        set_code(code, error_from_status(status));
    }
    return status;
}

} // namespace

extern "C" {

const char* galay_etcd_error_string(galay_etcd_error_code_t code)
{
    switch (code) {
        case GALAY_ETCD_ERROR_SUCCESS:
            return "success";
        case GALAY_ETCD_ERROR_INVALID_ENDPOINT:
            return "invalid endpoint";
        case GALAY_ETCD_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case GALAY_ETCD_ERROR_NOT_CONNECTED:
            return "not connected";
        case GALAY_ETCD_ERROR_IO:
            return "io error";
        case GALAY_ETCD_ERROR_PROTOCOL:
            return "protocol error";
        case GALAY_ETCD_ERROR_CANCELLED:
            return "cancelled";
    }
    return "unknown";
}

const char* galay_etcd_get_error(galay_etcd_error_code_t code)
{
    return galay_etcd_error_string(code);
}

galay_status_t galay_etcd_error_status(galay_etcd_error_code_t code)
{
    switch (code) {
        case GALAY_ETCD_ERROR_SUCCESS:
            return GALAY_OK;
        case GALAY_ETCD_ERROR_INVALID_ENDPOINT:
        case GALAY_ETCD_ERROR_INVALID_ARGUMENT:
        case GALAY_ETCD_ERROR_NOT_CONNECTED:
            return GALAY_INVALID_ARGUMENT;
        case GALAY_ETCD_ERROR_IO:
        case GALAY_ETCD_ERROR_CANCELLED:
            return GALAY_IO_ERROR;
        case GALAY_ETCD_ERROR_PROTOCOL:
            return GALAY_PROTOCOL_ERROR;
    }
    return GALAY_INTERNAL_ERROR;
}

galay_status_t galay_etcd_config_builder_create(galay_etcd_config_builder_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_etcd_config_builder_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_etcd_config_builder_destroy(galay_etcd_config_builder_t* builder)
{
    delete builder;
}

galay_status_t galay_etcd_config_builder_set_endpoint(galay_etcd_config_builder_t* builder,
                                                      const char* endpoint)
{
    if (builder == nullptr || endpoint == nullptr || endpoint[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    builder->endpoint = endpoint;
    return GALAY_OK;
}

galay_status_t galay_etcd_config_builder_set_endpoint_policy(
    galay_etcd_config_builder_t* builder,
    galay_etcd_endpoint_policy_t policy)
{
    if (builder == nullptr ||
        (policy != GALAY_ETCD_ENDPOINT_POLICY_FIRST_HEALTHY &&
         policy != GALAY_ETCD_ENDPOINT_POLICY_ROUND_ROBIN &&
         policy != GALAY_ETCD_ENDPOINT_POLICY_STICKY_LEADER)) {
        return GALAY_INVALID_ARGUMENT;
    }
    builder->endpoint_policy = policy;
    return GALAY_OK;
}

galay_status_t galay_etcd_client_create(const galay_etcd_config_builder_t* builder,
                                        galay_etcd_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* client = new (std::nothrow) galay_etcd_client_t();
    if (client == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    client->endpoint = builder == nullptr ? "http://127.0.0.1:2379" : builder->endpoint;
    client->api_prefix = builder == nullptr ? "/v3" : builder->api_prefix;
    client->endpoint_policy = builder == nullptr
        ? GALAY_ETCD_ENDPOINT_POLICY_FIRST_HEALTHY
        : builder->endpoint_policy;
    *out = client;
    return GALAY_OK;
}

void galay_etcd_client_destroy(galay_etcd_client_t* client)
{
    if (client != nullptr && client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
        }
    }
    delete client;
}

galay_status_t galay_etcd_client_connect(galay_etcd_client_t* client,
                                         galay_etcd_error_code_t* code)
{
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (client->connected) {
        return GALAY_OK;
    }
    if (!parse_endpoint(client->endpoint, client->parsed)) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ENDPOINT);
        return GALAY_INVALID_ARGUMENT;
    }
    C_Host host{};
    if (!endpoint_to_host(client->parsed, host)) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ENDPOINT);
        return GALAY_INVALID_ARGUMENT;
    }
    C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&client->socket, host.type);
    if (created != C_TcpSocketSuccess) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ENDPOINT);
        return GALAY_IO_ERROR;
    }
    C_IOResult connected =
        galay_kernel_tcp_socket_connect(&client->socket, &host, kDefaultTimeoutMs);
    if (connected.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        client->connected = false;
        set_code(code, error_from_status(status_from_io(connected)));
        if (destroyed != C_TcpSocketSuccess && connected.code == C_IOResultOk) {
            return GALAY_IO_ERROR;
        }
        return status_from_io(connected);
    }
    client->connected = true;
    return GALAY_OK;
}

galay_status_t galay_etcd_client_close(galay_etcd_client_t* client,
                                       galay_etcd_error_code_t* code)
{
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || client->socket.socket == nullptr) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }
    C_IOResult closed = galay_kernel_tcp_socket_close(&client->socket, kDefaultTimeoutMs);
    const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
    client->socket.socket = nullptr;
    client->connected = false;
    client->recv_buffer.clear();
    if (closed.code != C_IOResultOk) {
        set_code(code, error_from_status(status_from_io(closed)));
        return status_from_io(closed);
    }
    if (destroyed != C_TcpSocketSuccess) {
        set_code(code, GALAY_ETCD_ERROR_IO);
        return GALAY_IO_ERROR;
    }
    return GALAY_OK;
}

galay_status_t galay_etcd_client_put(galay_etcd_client_t* client, const char* key,
                                     const char* value, size_t value_len,
                                     galay_etcd_error_code_t* code)
{
    return put_impl(client, key, value, value_len, 0, code);
}

galay_status_t galay_etcd_client_get(galay_etcd_client_t* client, const char* key,
                                     galay_bool_t prefix, int64_t limit,
                                     galay_etcd_get_result_t** result,
                                     galay_etcd_error_code_t* code)
{
    if (result != nullptr) {
        *result = nullptr;
    }
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || key == nullptr || key[0] == '\0' || result == nullptr || limit < 0) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    if (!client->connected) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }

    HttpResponse response;
    std::string body = build_key_body(key, prefix, limit);
    galay_status_t status = post_json(client, "/kv/range", body, response);
    if (status != GALAY_OK) {
        set_code(code, error_from_status(status));
        return status;
    }

    auto* out = new (std::nothrow) galay_etcd_get_result_t();
    if (out == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    out->kvs = parse_kvs(response.body);
    *result = out;
    return GALAY_OK;
}

galay_status_t galay_etcd_client_delete(galay_etcd_client_t* client, const char* key,
                                        galay_bool_t prefix, int64_t* deleted_count,
                                        galay_etcd_error_code_t* code)
{
    if (deleted_count != nullptr) {
        *deleted_count = 0;
    }
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || key == nullptr || key[0] == '\0') {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    if (!client->connected) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }

    HttpResponse response;
    std::string body = build_key_body(key, prefix, 0);
    galay_status_t status = post_json(client, "/kv/deleterange", body, response);
    if (status != GALAY_OK) {
        set_code(code, error_from_status(status));
        return status;
    }
    std::optional<int64_t> deleted = extract_json_i64(response.body, "deleted");
    if (!deleted.has_value()) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (deleted_count != nullptr) {
        *deleted_count = *deleted;
    }
    return GALAY_OK;
}

galay_status_t galay_etcd_get_result_create_empty(galay_etcd_get_result_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_etcd_get_result_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_etcd_get_result_destroy(galay_etcd_get_result_t* result)
{
    delete result;
}

galay_status_t galay_etcd_get_result_count(const galay_etcd_get_result_t* result, size_t* count)
{
    if (result == nullptr || count == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *count = result->kvs.size();
    return GALAY_OK;
}

galay_status_t galay_etcd_get_result_item(const galay_etcd_get_result_t* result, size_t index,
                                          const char** key, size_t* key_len,
                                          const char** value, size_t* value_len)
{
    if (key != nullptr) {
        *key = nullptr;
    }
    if (key_len != nullptr) {
        *key_len = 0;
    }
    if (value != nullptr) {
        *value = nullptr;
    }
    if (value_len != nullptr) {
        *value_len = 0;
    }
    if (result == nullptr || key == nullptr || key_len == nullptr ||
        value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (index >= result->kvs.size()) {
        return GALAY_NOT_FOUND;
    }
    const EtcdKv& kv = result->kvs[index];
    *key = kv.key.data();
    *key_len = kv.key.size();
    *value = kv.value.data();
    *value_len = kv.value.size();
    return GALAY_OK;
}

galay_status_t galay_etcd_client_lease_grant(galay_etcd_client_t* client,
                                             int64_t ttl_seconds,
                                             int64_t* lease_id,
                                             galay_etcd_error_code_t* code)
{
    if (lease_id != nullptr) {
        *lease_id = 0;
    }
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || lease_id == nullptr || ttl_seconds <= 0) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    if (!client->connected) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }

    std::string body = "{\"TTL\":";
    body += std::to_string(ttl_seconds);
    body.push_back('}');
    HttpResponse response;
    galay_status_t status = post_json(client, "/lease/grant", body, response);
    if (status != GALAY_OK) {
        set_code(code, error_from_status(status));
        return status;
    }
    std::optional<int64_t> id = extract_json_i64(response.body, "ID");
    if (!id.has_value()) {
        set_code(code, GALAY_ETCD_ERROR_PROTOCOL);
        return GALAY_PROTOCOL_ERROR;
    }
    *lease_id = *id;
    return GALAY_OK;
}

galay_status_t galay_etcd_client_lease_keepalive(galay_etcd_client_t* client,
                                                 int64_t lease_id,
                                                 int64_t* refreshed_lease_id,
                                                 galay_etcd_error_code_t* code)
{
    if (refreshed_lease_id != nullptr) {
        *refreshed_lease_id = 0;
    }
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || refreshed_lease_id == nullptr || lease_id <= 0) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    if (!client->connected) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }

    std::string body = "{\"ID\":";
    body += std::to_string(lease_id);
    body.push_back('}');
    HttpResponse response;
    galay_status_t status = post_json(client, "/lease/keepalive", body, response);
    if (status != GALAY_OK) {
        ++client->stats.lease_keepalive_failures;
        set_code(code, error_from_status(status));
        return status;
    }
    std::optional<int64_t> id = extract_json_i64(response.body, "ID");
    if (!id.has_value()) {
        ++client->stats.lease_keepalive_failures;
        set_code(code, GALAY_ETCD_ERROR_PROTOCOL);
        return GALAY_PROTOCOL_ERROR;
    }
    ++client->stats.lease_keepalive_successes;
    *refreshed_lease_id = *id;
    return GALAY_OK;
}

galay_status_t galay_etcd_client_lease_revoke(galay_etcd_client_t* client,
                                              int64_t lease_id,
                                              galay_etcd_error_code_t* code)
{
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || lease_id <= 0) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    if (!client->connected) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }

    std::string body = "{\"ID\":";
    body += std::to_string(lease_id);
    body.push_back('}');
    HttpResponse response;
    galay_status_t status = post_json(client, "/lease/revoke", body, response);
    if (status != GALAY_OK) {
        set_code(code, error_from_status(status));
    }
    return status;
}

galay_status_t galay_etcd_pipeline_create(galay_etcd_pipeline_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_etcd_pipeline_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_etcd_pipeline_destroy(galay_etcd_pipeline_t* pipeline)
{
    delete pipeline;
}

galay_status_t galay_etcd_pipeline_add_put(galay_etcd_pipeline_t* pipeline,
                                           const char* key,
                                           const char* value,
                                           size_t value_len,
                                           int64_t lease_id)
{
    if (pipeline == nullptr || key == nullptr || key[0] == '\0' ||
        (value == nullptr && value_len != 0) || lease_id < 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    PipelineOp op;
    op.type = GALAY_ETCD_PIPELINE_PUT;
    op.key = key;
    op.value.assign(value == nullptr ? "" : value, value_len);
    op.lease_id = lease_id;
    pipeline->operations.push_back(std::move(op));
    return GALAY_OK;
}

galay_status_t galay_etcd_pipeline_add_get(galay_etcd_pipeline_t* pipeline,
                                           const char* key,
                                           galay_bool_t prefix,
                                           int64_t limit)
{
    if (pipeline == nullptr || key == nullptr || key[0] == '\0' || limit < 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    PipelineOp op;
    op.type = GALAY_ETCD_PIPELINE_GET;
    op.key = key;
    op.prefix = prefix == GALAY_TRUE;
    op.limit = limit;
    pipeline->operations.push_back(std::move(op));
    return GALAY_OK;
}

galay_status_t galay_etcd_pipeline_add_delete(galay_etcd_pipeline_t* pipeline,
                                              const char* key,
                                              galay_bool_t prefix)
{
    if (pipeline == nullptr || key == nullptr || key[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    PipelineOp op;
    op.type = GALAY_ETCD_PIPELINE_DELETE;
    op.key = key;
    op.prefix = prefix == GALAY_TRUE;
    pipeline->operations.push_back(std::move(op));
    return GALAY_OK;
}

galay_status_t galay_etcd_client_pipeline_execute(galay_etcd_client_t* client,
                                                  const galay_etcd_pipeline_t* pipeline,
                                                  galay_etcd_pipeline_result_t** result,
                                                  galay_etcd_error_code_t* code)
{
    if (result != nullptr) {
        *result = nullptr;
    }
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || pipeline == nullptr || result == nullptr || pipeline->operations.empty()) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    if (!client->connected) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }

    auto* out = new (std::nothrow) galay_etcd_pipeline_result_t();
    if (out == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    for (const PipelineOp& op : pipeline->operations) {
        PipelineItem item;
        item.type = op.type;
        if (op.type == GALAY_ETCD_PIPELINE_PUT) {
            galay_status_t status =
                put_impl(client, op.key.c_str(), op.value.data(), op.value.size(), op.lease_id, code);
            if (status != GALAY_OK) {
                delete out;
                return status;
            }
        } else if (op.type == GALAY_ETCD_PIPELINE_GET) {
            galay_etcd_get_result_t* get_result = nullptr;
            galay_status_t status = galay_etcd_client_get(client,
                                                          op.key.c_str(),
                                                          op.prefix ? GALAY_TRUE : GALAY_FALSE,
                                                          op.limit,
                                                          &get_result,
                                                          code);
            if (status != GALAY_OK) {
                delete out;
                return status;
            }
            item.get_result.kvs = std::move(get_result->kvs);
            galay_etcd_get_result_destroy(get_result);
        } else if (op.type == GALAY_ETCD_PIPELINE_DELETE) {
            galay_status_t status = galay_etcd_client_delete(client,
                                                             op.key.c_str(),
                                                             op.prefix ? GALAY_TRUE : GALAY_FALSE,
                                                             &item.deleted_count,
                                                             code);
            if (status != GALAY_OK) {
                delete out;
                return status;
            }
        }
        out->items.push_back(std::move(item));
    }
    *result = out;
    return GALAY_OK;
}

void galay_etcd_pipeline_result_destroy(galay_etcd_pipeline_result_t* result)
{
    delete result;
}

galay_status_t galay_etcd_pipeline_result_count(const galay_etcd_pipeline_result_t* result,
                                                size_t* count)
{
    if (result == nullptr || count == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *count = result->items.size();
    return GALAY_OK;
}

galay_status_t galay_etcd_pipeline_result_item_type(
    const galay_etcd_pipeline_result_t* result,
    size_t index,
    galay_etcd_pipeline_op_type_t* type)
{
    if (result == nullptr || type == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (index >= result->items.size()) {
        return GALAY_NOT_FOUND;
    }
    *type = result->items[index].type;
    return GALAY_OK;
}

galay_status_t galay_etcd_pipeline_result_item_get_result(
    const galay_etcd_pipeline_result_t* result,
    size_t index,
    const galay_etcd_get_result_t** get_result)
{
    if (get_result != nullptr) {
        *get_result = nullptr;
    }
    if (result == nullptr || get_result == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (index >= result->items.size()) {
        return GALAY_NOT_FOUND;
    }
    if (result->items[index].type != GALAY_ETCD_PIPELINE_GET) {
        return GALAY_INVALID_ARGUMENT;
    }
    *get_result = &result->items[index].get_result;
    return GALAY_OK;
}

galay_status_t galay_etcd_pipeline_result_item_deleted_count(
    const galay_etcd_pipeline_result_t* result,
    size_t index,
    int64_t* deleted_count)
{
    if (deleted_count != nullptr) {
        *deleted_count = 0;
    }
    if (result == nullptr || deleted_count == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (index >= result->items.size()) {
        return GALAY_NOT_FOUND;
    }
    if (result->items[index].type != GALAY_ETCD_PIPELINE_DELETE) {
        return GALAY_INVALID_ARGUMENT;
    }
    *deleted_count = result->items[index].deleted_count;
    return GALAY_OK;
}

galay_status_t galay_etcd_watch_create(galay_etcd_client_t* client,
                                       const char* key,
                                       galay_bool_t prefix,
                                       galay_etcd_watch_t** watch,
                                       galay_etcd_error_code_t* code)
{
    if (watch != nullptr) {
        *watch = nullptr;
    }
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || key == nullptr || key[0] == '\0' || watch == nullptr) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    if (!client->connected) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }
    auto* out = new (std::nothrow) galay_etcd_watch_t();
    if (out == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    out->client = client;
    out->key = key;
    out->prefix = prefix == GALAY_TRUE;
    *watch = out;
    return GALAY_OK;
}

void galay_etcd_watch_destroy(galay_etcd_watch_t* watch)
{
    delete watch;
}

galay_status_t galay_etcd_watch_next(galay_etcd_watch_t* watch,
                                     galay_etcd_watch_event_t** event,
                                     galay_etcd_error_code_t* code)
{
    if (event != nullptr) {
        *event = nullptr;
    }
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (watch == nullptr || event == nullptr) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    if (watch->cancelled) {
        set_code(code, GALAY_ETCD_ERROR_CANCELLED);
        return GALAY_IO_ERROR;
    }
    if (watch->client == nullptr || !watch->client->connected) {
        set_code(code, GALAY_ETCD_ERROR_NOT_CONNECTED);
        return GALAY_INVALID_ARGUMENT;
    }

    std::string body = "{\"create_request\":";
    body += build_key_body(watch->key, watch->prefix ? GALAY_TRUE : GALAY_FALSE, 0);
    body.push_back('}');
    HttpResponse response;
    galay_status_t status = post_json(watch->client, "/watch", body, response);
    if (status != GALAY_OK) {
        set_code(code, error_from_status(status));
        return status;
    }

    auto* out = new (std::nothrow) galay_etcd_watch_event_t();
    if (out == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    std::optional<int64_t> watch_id = extract_json_i64(response.body, "watch_id");
    if (watch_id.has_value()) {
        out->watch_id = *watch_id;
    }
    if (std::optional<std::string_view> type = extract_json_string_view(response.body, "type", 0);
        type.has_value()) {
        if (*type == "PUT") {
            out->type = GALAY_ETCD_WATCH_EVENT_PUT;
        } else if (*type == "DELETE") {
            out->type = GALAY_ETCD_WATCH_EVENT_DELETE;
        }
    }
    std::vector<EtcdKv> kvs = parse_kvs(response.body);
    if (!kvs.empty()) {
        out->key = std::move(kvs.front().key);
        out->value = std::move(kvs.front().value);
    }
    *event = out;
    return GALAY_OK;
}

galay_status_t galay_etcd_watch_cancel(galay_etcd_watch_t* watch,
                                       galay_etcd_error_code_t* code)
{
    set_code(code, GALAY_ETCD_ERROR_SUCCESS);
    if (watch == nullptr) {
        set_code(code, GALAY_ETCD_ERROR_INVALID_ARGUMENT);
        return GALAY_INVALID_ARGUMENT;
    }
    watch->cancelled = true;
    return GALAY_OK;
}

void galay_etcd_watch_event_destroy(galay_etcd_watch_event_t* event)
{
    delete event;
}

galay_status_t galay_etcd_watch_event_watch_id(const galay_etcd_watch_event_t* event,
                                               int64_t* watch_id)
{
    if (event == nullptr || watch_id == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *watch_id = event->watch_id;
    return GALAY_OK;
}

galay_status_t galay_etcd_watch_event_type(const galay_etcd_watch_event_t* event,
                                           galay_etcd_watch_event_type_t* type)
{
    if (event == nullptr || type == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *type = event->type;
    return GALAY_OK;
}

galay_status_t galay_etcd_watch_event_key_value(const galay_etcd_watch_event_t* event,
                                                const char** key,
                                                size_t* key_len,
                                                const char** value,
                                                size_t* value_len)
{
    if (key != nullptr) {
        *key = nullptr;
    }
    if (key_len != nullptr) {
        *key_len = 0;
    }
    if (value != nullptr) {
        *value = nullptr;
    }
    if (value_len != nullptr) {
        *value_len = 0;
    }
    if (event == nullptr || key == nullptr || key_len == nullptr ||
        value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *key = event->key.data();
    *key_len = event->key.size();
    *value = event->value.data();
    *value_len = event->value.size();
    return GALAY_OK;
}

galay_status_t galay_etcd_client_stats(const galay_etcd_client_t* client,
                                       galay_etcd_client_stats_t* stats)
{
    if (client == nullptr || stats == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *stats = client->stats;
    return GALAY_OK;
}

}
