#include <galay/c/galay-mcp-c/mcp.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket.h>

#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>


constexpr size_t kMcpMaxHttpMessage = 1024 * 1024;

C_IOResult make_io_result(C_IOResultCode code, int64_t value = 0)
{
    return C_IOResult{code, 0, 0, value, nullptr};
}

C_IOResult io_result_from_status(galay_status_t status)
{
    return make_io_result(status == GALAY_INVALID_ARGUMENT ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(status));
}

bool valid_method(const char* method)
{
    if (method == nullptr || method[0] == '\0') {
        return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(method); *p != '\0'; ++p) {
        if (*p <= 0x20 || *p == 0x7f || *p == '"' || *p == '\\') {
            return false;
        }
    }
    return true;
}

void skip_ws(const std::string& data, size_t* pos)
{
    while (*pos < data.size() &&
           (data[*pos] == ' ' || data[*pos] == '\t' || data[*pos] == '\r' ||
            data[*pos] == '\n')) {
        ++(*pos);
    }
}

bool parse_string_at(const std::string& data, size_t start, std::string* out, size_t* end_pos)
{
    if (out == nullptr || start >= data.size() || data[start] != '"') {
        return false;
    }
    const auto hex_value = [](unsigned char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    const auto append_utf8 = [](std::string& value, uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            value.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            value.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            value.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            value.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            value.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    };
    std::string value;
    for (size_t pos = start + 1; pos < data.size(); ++pos) {
        const unsigned char ch = static_cast<unsigned char>(data[pos]);
        if (ch < 0x20) {
            return false;
        }
        if (ch == '\\') {
            if (++pos >= data.size()) {
                return false;
            }
            switch (data[pos]) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                if (pos + 4 >= data.size()) {
                    return false;
                }
                uint32_t codepoint = 0;
                for (size_t digit = 1; digit <= 4; ++digit) {
                    const int hex = hex_value(static_cast<unsigned char>(data[pos + digit]));
                    if (hex < 0) {
                        return false;
                    }
                    codepoint = (codepoint << 4) | static_cast<uint32_t>(hex);
                }
                pos += 4;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (pos + 6 >= data.size() || data[pos + 1] != '\\' || data[pos + 2] != 'u') {
                        return false;
                    }
                    uint32_t low = 0;
                    for (size_t digit = 3; digit <= 6; ++digit) {
                        const int hex = hex_value(static_cast<unsigned char>(data[pos + digit]));
                        if (hex < 0) {
                            return false;
                        }
                        low = (low << 4) | static_cast<uint32_t>(hex);
                    }
                    if (low < 0xdc00 || low > 0xdfff) {
                        return false;
                    }
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    pos += 6;
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return false;
                }
                append_utf8(value, codepoint);
                break;
            }
            default:
                return false;
            }
            continue;
        }
        if (ch == '"') {
            *out = value;
            if (end_pos != nullptr) {
                *end_pos = pos + 1;
            }
            return true;
        }
        value.push_back(ch);
    }
    return false;
}

bool scan_json_string(const std::string& data, size_t start, size_t* end_pos)
{
    if (end_pos == nullptr || start >= data.size() || data[start] != '"') {
        return false;
    }
    for (size_t pos = start + 1; pos < data.size(); ++pos) {
        const unsigned char ch = static_cast<unsigned char>(data[pos]);
        if (ch == '"') {
            *end_pos = pos + 1;
            return true;
        }
        if (ch < 0x20) {
            return false;
        }
        if (ch != '\\') {
            continue;
        }
        if (++pos >= data.size()) {
            return false;
        }
        const char escaped = data[pos];
        if (escaped == 'u') {
            if (pos + 4 >= data.size()) {
                return false;
            }
            for (size_t digit = 1; digit <= 4; ++digit) {
                const unsigned char value = static_cast<unsigned char>(data[pos + digit]);
                if (!std::isxdigit(value)) {
                    return false;
                }
            }
            pos += 4;
        } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                   escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                   escaped != 'r' && escaped != 't') {
            return false;
        }
    }
    return false;
}

bool scan_json_value(const std::string& data, size_t start, size_t* end_pos);

bool scan_json_object(const std::string& data, size_t start, size_t* end_pos)
{
    size_t pos = start + 1;
    skip_ws(data, &pos);
    if (pos < data.size() && data[pos] == '}') {
        *end_pos = pos + 1;
        return true;
    }
    while (pos < data.size()) {
        size_t key_end = 0;
        if (!scan_json_string(data, pos, &key_end)) {
            return false;
        }
        pos = key_end;
        skip_ws(data, &pos);
        if (pos >= data.size() || data[pos] != ':') {
            return false;
        }
        ++pos;
        skip_ws(data, &pos);
        if (!scan_json_value(data, pos, &pos)) {
            return false;
        }
        skip_ws(data, &pos);
        if (pos < data.size() && data[pos] == '}') {
            *end_pos = pos + 1;
            return true;
        }
        if (pos >= data.size() || data[pos] != ',') {
            return false;
        }
        ++pos;
        skip_ws(data, &pos);
    }
    return false;
}

bool scan_json_array(const std::string& data, size_t start, size_t* end_pos)
{
    size_t pos = start + 1;
    skip_ws(data, &pos);
    if (pos < data.size() && data[pos] == ']') {
        *end_pos = pos + 1;
        return true;
    }
    while (pos < data.size()) {
        if (!scan_json_value(data, pos, &pos)) {
            return false;
        }
        skip_ws(data, &pos);
        if (pos < data.size() && data[pos] == ']') {
            *end_pos = pos + 1;
            return true;
        }
        if (pos >= data.size() || data[pos] != ',') {
            return false;
        }
        ++pos;
        skip_ws(data, &pos);
    }
    return false;
}

bool scan_json_number(const std::string& data, size_t start, size_t* end_pos)
{
    size_t pos = start;
    if (pos < data.size() && data[pos] == '-') {
        ++pos;
    }
    if (pos >= data.size()) {
        return false;
    }
    if (data[pos] == '0') {
        ++pos;
    } else {
        if (data[pos] < '1' || data[pos] > '9') {
            return false;
        }
        while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
            ++pos;
        }
    }
    if (pos < data.size() && data[pos] == '.') {
        ++pos;
        const size_t fraction_start = pos;
        while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
            ++pos;
        }
        if (pos == fraction_start) {
            return false;
        }
    }
    if (pos < data.size() && (data[pos] == 'e' || data[pos] == 'E')) {
        ++pos;
        if (pos < data.size() && (data[pos] == '+' || data[pos] == '-')) {
            ++pos;
        }
        const size_t exponent_start = pos;
        while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
            ++pos;
        }
        if (pos == exponent_start) {
            return false;
        }
    }
    *end_pos = pos;
    return true;
}

bool scan_json_value(const std::string& data, size_t start, size_t* end_pos)
{
    if (end_pos == nullptr || start >= data.size()) {
        return false;
    }
    switch (data[start]) {
    case '"':
        return scan_json_string(data, start, end_pos);
    case '{':
        return scan_json_object(data, start, end_pos);
    case '[':
        return scan_json_array(data, start, end_pos);
    case 't':
        if (data.compare(start, 4, "true") == 0) {
            *end_pos = start + 4;
            return true;
        }
        return false;
    case 'f':
        if (data.compare(start, 5, "false") == 0) {
            *end_pos = start + 5;
            return true;
        }
        return false;
    case 'n':
        if (data.compare(start, 4, "null") == 0) {
            *end_pos = start + 4;
            return true;
        }
        return false;
    default:
        return scan_json_number(data, start, end_pos);
    }
}

bool find_top_level_json_field(const std::string& data, const char* key,
                               size_t* value_start, size_t* value_end)
{
    if (key == nullptr || value_start == nullptr || value_end == nullptr) {
        return false;
    }
    size_t pos = 0;
    skip_ws(data, &pos);
    if (pos >= data.size() || data[pos] != '{') {
        return false;
    }
    ++pos;
    skip_ws(data, &pos);
    if (pos < data.size() && data[pos] == '}') {
        ++pos;
        skip_ws(data, &pos);
        return pos == data.size();
    }
    bool found = false;
    while (pos < data.size()) {
        const size_t key_start = pos;
        size_t key_end = 0;
        if (!scan_json_string(data, key_start, &key_end)) {
            return false;
        }
        std::string parsed_key;
        if (!parse_string_at(data, key_start, &parsed_key, nullptr)) {
            return false;
        }
        pos = key_end;
        skip_ws(data, &pos);
        if (pos >= data.size() || data[pos] != ':') {
            return false;
        }
        ++pos;
        skip_ws(data, &pos);
        const size_t current_start = pos;
        size_t current_end = 0;
        if (!scan_json_value(data, current_start, &current_end)) {
            return false;
        }
        if (!found && parsed_key == key) {
            *value_start = current_start;
            *value_end = current_end;
            found = true;
        }
        pos = current_end;
        skip_ws(data, &pos);
        if (pos < data.size() && data[pos] == '}') {
            ++pos;
            skip_ws(data, &pos);
            return found && pos == data.size();
        }
        if (pos >= data.size() || data[pos] != ',') {
            return false;
        }
        ++pos;
        skip_ws(data, &pos);
    }
    return false;
}

bool find_string_field(const std::string& data, const char* key, std::string* out)
{
    size_t start = 0;
    size_t end = 0;
    return find_top_level_json_field(data, key, &start, &end) &&
        start < end && parse_string_at(data, start, out, nullptr);
}

bool find_int_field(const std::string& data, const char* key, int64_t* out)
{
    if (out == nullptr) {
        return false;
    }
    size_t start = 0;
    size_t end = 0;
    if (!find_top_level_json_field(data, key, &start, &end)) {
        return false;
    }
    int64_t value = 0;
    const auto parsed = std::from_chars(data.data() + start, data.data() + end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != data.data() + end) {
        return false;
    }
    *out = value;
    return true;
}

bool find_json_value_end(const std::string& data, size_t start, size_t* end)
{
    return scan_json_value(data, start, end);
}

bool find_json_field(const std::string& data, const char* key, std::string* out)
{
    if (out == nullptr) {
        return false;
    }
    size_t start = 0;
    size_t end = 0;
    if (!find_top_level_json_field(data, key, &start, &end)) {
        return false;
    }
    *out = data.substr(start, end - start);
    return true;
}

bool valid_http_host(std::string_view host)
{
    if (host.empty()) {
        return false;
    }
    for (const unsigned char ch : host) {
        if (ch <= 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool valid_http_path(std::string_view path)
{
    if (path.empty() || path.front() != '/') {
        return false;
    }
    for (const unsigned char ch : path) {
        if (ch <= 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool valid_bearer_token(std::string_view token)
{
    if (token.empty()) {
        return false;
    }
    for (const unsigned char ch : token) {
        if (ch <= 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

void append_json_string(std::string* out, const std::string& value)
{
    static constexpr char hex[] = "0123456789abcdef";
    out->push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':
            *out += "\\\"";
            break;
        case '\\':
            *out += "\\\\";
            break;
        case '\b':
            *out += "\\b";
            break;
        case '\f':
            *out += "\\f";
            break;
        case '\n':
            *out += "\\n";
            break;
        case '\r':
            *out += "\\r";
            break;
        case '\t':
            *out += "\\t";
            break;
        default:
            if (ch < 0x20) {
                out->append("\\u00");
                out->push_back(hex[ch >> 4]);
                out->push_back(hex[ch & 0x0f]);
            } else {
                out->push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out->push_back('"');
}

std::string json_string(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 2);
    append_json_string(&out, value);
    return out;
}

std::string non_empty_json_or_object(const std::string& value)
{
    return value.empty() ? std::string("{}") : value;
}

std::string non_empty_json_or_array(const std::string& value)
{
    return value.empty() ? std::string("[]") : value;
}

std::string make_result_response(int64_t id, const std::string& result)
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"result\":" + non_empty_json_or_object(result) + "}";
}

std::string make_error_response(int64_t id, int code, const std::string& message)
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"error\":{\"code\":" + std::to_string(code) +
        ",\"message\":" + json_string(message) + "}}";
}

bool copy_host_to_c_host(const std::string& host, uint16_t port, C_Host* out)
{
    if (out == nullptr || !valid_http_host(host) || host.size() >= sizeof(out->address) || port == 0) {
        return false;
    }
    out->type = host.find(':') == std::string::npos ? C_IPTypeIPV4 : C_IPTypeIPV6;
    std::memset(out->address, 0, sizeof(out->address));
    std::memcpy(out->address, host.data(), host.size());
    out->port = port;
    return true;
}

bool parse_http_url(const std::string& url, std::string* host, uint16_t* port, std::string* path)
{
    constexpr const char* kScheme = "http://";
    if (host == nullptr || port == nullptr || path == nullptr || url.rfind(kScheme, 0) != 0) {
        return false;
    }
    const size_t authority_start = std::strlen(kScheme);
    const size_t path_start = url.find('/', authority_start);
    const size_t authority_end = path_start == std::string::npos ? url.size() : path_start;
    const size_t colon = url.rfind(':', authority_end);
    if (colon == std::string::npos || colon < authority_start) {
        return false;
    }
    std::string parsed_host = url.substr(authority_start, colon - authority_start);
    const std::string port_text = url.substr(colon + 1, authority_end - colon - 1);
    char* end = nullptr;
    const long parsed_port = std::strtol(port_text.c_str(), &end, 10);
    if (!valid_http_host(parsed_host) || end == port_text.c_str() || *end != '\0' ||
        parsed_port <= 0 || parsed_port > 65535) {
        return false;
    }
    const std::string parsed_path = path_start == std::string::npos ? std::string("/") :
        url.substr(path_start);
    if (!valid_http_path(parsed_path)) {
        return false;
    }
    *host = parsed_host;
    *port = static_cast<uint16_t>(parsed_port);
    *path = parsed_path;
    return true;
}

C_IOResult socket_write_exact(galay_c_tcp_socket_t* socket,
                              const char* data,
                              size_t data_len,
                              int64_t timeout_ms)
{
    if (socket == nullptr || data == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    size_t sent = 0;
    while (sent < data_len) {
        C_IOResult result = galay_c_tcp_socket_send(socket, data + sent, data_len - sent, timeout_ms);
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

bool parse_content_length(const std::string& headers, size_t* content_length)
{
    if (content_length == nullptr) {
        return false;
    }
    const std::string marker = "Content-Length:";
    const size_t pos = headers.find(marker);
    if (pos == std::string::npos) {
        return false;
    }
    size_t value_start = pos + marker.size();
    while (value_start < headers.size() &&
           std::isspace(static_cast<unsigned char>(headers[value_start])) != 0) {
        ++value_start;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(headers.c_str() + value_start, &end, 10);
    if (end == headers.c_str() + value_start) {
        return false;
    }
    *content_length = static_cast<size_t>(value);
    return true;
}

C_IOResult read_http_message(galay_c_tcp_socket_t* socket,
                             int64_t timeout_ms,
                             std::string* headers_out,
                             std::string* body)
{
    if (socket == nullptr || body == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    std::string buffer;
    char chunk[1024];
    size_t header_end = std::string::npos;
    size_t content_length = 0;
    for (;;) {
        if (buffer.size() > kMcpMaxHttpMessage) {
            return io_result_from_status(GALAY_PROTOCOL_ERROR);
        }
        header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            const std::string headers = buffer.substr(0, header_end + 4);
            if (!parse_content_length(headers, &content_length) ||
                content_length > kMcpMaxHttpMessage) {
                return io_result_from_status(GALAY_PROTOCOL_ERROR);
            }
            const size_t body_start = header_end + 4;
            if (buffer.size() >= body_start + content_length) {
                if (headers_out != nullptr) {
                    *headers_out = headers;
                }
                *body = buffer.substr(body_start, content_length);
                C_IOResult result = make_io_result(C_IOResultOk);
                result.bytes = body->size();
                return result;
            }
        }
        C_IOResult recv_result = galay_c_tcp_socket_recv(socket, chunk, sizeof(chunk), timeout_ms);
        if (recv_result.code != C_IOResultOk) {
            return recv_result;
        }
        if (recv_result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        buffer.append(chunk, recv_result.bytes);
    }
}

C_IOResult read_http_body(galay_c_tcp_socket_t* socket,
                          int64_t timeout_ms,
                          std::string* body)
{
    return read_http_message(socket, timeout_ms, nullptr, body);
}

bool has_expected_authorization(const std::string& headers, const std::string& bearer_token)
{
    if (bearer_token.empty()) {
        return true;
    }
    const std::string expected = "Authorization: Bearer " + bearer_token + "\r\n";
    return headers.find(expected) != std::string::npos;
}

struct ToolRegistration {
    std::string name;
    std::string description;
    std::string input_schema;
    galay_mcp_tool_handler_fn handler = nullptr;
    void* userdata = nullptr;
};

struct ResourceRegistration {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type;
    galay_mcp_resource_reader_fn reader = nullptr;
    void* userdata = nullptr;
};

struct PromptRegistration {
    std::string name;
    std::string description;
    std::string arguments;
    galay_mcp_prompt_getter_fn getter = nullptr;
    void* userdata = nullptr;
};


struct galay_mcp_message_t {
    std::string data;
};

struct galay_mcp_parsed_request_t {
    std::string method;
    std::string params;
    int64_t id = 0;
    bool notification = false;
};

struct galay_mcp_parsed_response_t {
    int64_t id = 0;
    std::string result;
};

struct galay_mcp_client_config_t {
    std::string url;
    std::string bearer_token;
    galay_mcp_mode_t mode = GALAY_MCP_MODE_STDIO;
};

struct galay_mcp_server_t {
    std::string name = "galay-c-mcp-server";
    std::string version = "1.0.0";
    std::vector<ToolRegistration> tools;
    std::vector<ResourceRegistration> resources;
    std::vector<PromptRegistration> prompts;
    std::string http_host = "127.0.0.1";
    std::string bearer_token;
    galay_c_tcp_socket_t listener{};
    galay_mcp_mode_t mode = GALAY_MCP_MODE_STDIO;
    uint16_t http_port = 0;
    bool listening = false;
};

struct galay_mcp_client_t {
    std::string url;
    std::string http_host;
    std::string http_path = "/mcp";
    std::string bearer_token;
    galay_mcp_server_t* loopback_server = nullptr;
    int64_t next_id = 1;
    galay_mcp_mode_t mode = GALAY_MCP_MODE_STDIO;
    uint16_t http_port = 0;
    bool connected = false;
    bool initialized = false;
};


ToolRegistration* find_tool(galay_mcp_server_t* server, const std::string& name)
{
    for (ToolRegistration& tool : server->tools) {
        if (tool.name == name) {
            return &tool;
        }
    }
    return nullptr;
}

ResourceRegistration* find_resource(galay_mcp_server_t* server, const std::string& uri)
{
    for (ResourceRegistration& resource : server->resources) {
        if (resource.uri == uri) {
            return &resource;
        }
    }
    return nullptr;
}

PromptRegistration* find_prompt(galay_mcp_server_t* server, const std::string& name)
{
    for (PromptRegistration& prompt : server->prompts) {
        if (prompt.name == name) {
            return &prompt;
        }
    }
    return nullptr;
}

std::string build_initialize_result(const galay_mcp_server_t* server)
{
    std::string result = "{\"protocolVersion\":\"2024-11-05\",\"serverInfo\":{\"name\":";
    append_json_string(&result, server->name);
    result += ",\"version\":";
    append_json_string(&result, server->version);
    result += "},\"capabilities\":{";
    bool needs_comma = false;
    if (!server->tools.empty()) {
        result += "\"tools\":{}";
        needs_comma = true;
    }
    if (!server->resources.empty()) {
        if (needs_comma) {
            result.push_back(',');
        }
        result += "\"resources\":{}";
        needs_comma = true;
    }
    if (!server->prompts.empty()) {
        if (needs_comma) {
            result.push_back(',');
        }
        result += "\"prompts\":{}";
    }
    result += "}}";
    return result;
}

std::string build_tools_list(const galay_mcp_server_t* server)
{
    std::string result = "{\"tools\":[";
    for (size_t index = 0; index < server->tools.size(); ++index) {
        const ToolRegistration& tool = server->tools[index];
        if (index != 0) {
            result.push_back(',');
        }
        result += "{\"name\":";
        append_json_string(&result, tool.name);
        result += ",\"description\":";
        append_json_string(&result, tool.description);
        result += ",\"inputSchema\":";
        result += non_empty_json_or_object(tool.input_schema);
        result.push_back('}');
    }
    result += "]}";
    return result;
}

std::string build_resources_list(const galay_mcp_server_t* server)
{
    std::string result = "{\"resources\":[";
    for (size_t index = 0; index < server->resources.size(); ++index) {
        const ResourceRegistration& resource = server->resources[index];
        if (index != 0) {
            result.push_back(',');
        }
        result += "{\"uri\":";
        append_json_string(&result, resource.uri);
        result += ",\"name\":";
        append_json_string(&result, resource.name);
        result += ",\"description\":";
        append_json_string(&result, resource.description);
        result += ",\"mimeType\":";
        append_json_string(&result, resource.mime_type);
        result.push_back('}');
    }
    result += "]}";
    return result;
}

std::string build_prompts_list(const galay_mcp_server_t* server)
{
    std::string result = "{\"prompts\":[";
    for (size_t index = 0; index < server->prompts.size(); ++index) {
        const PromptRegistration& prompt = server->prompts[index];
        if (index != 0) {
            result.push_back(',');
        }
        result += "{\"name\":";
        append_json_string(&result, prompt.name);
        result += ",\"description\":";
        append_json_string(&result, prompt.description);
        result += ",\"arguments\":";
        result += non_empty_json_or_array(prompt.arguments);
        result.push_back('}');
    }
    result += "]}";
    return result;
}

galay_status_t process_mcp_request(galay_mcp_server_t* server,
                                   const std::string& request,
                                   std::string* response)
{
    if (server == nullptr || response == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    response->clear();
    galay_mcp_parsed_request_t* parsed = nullptr;
    const galay_status_t parsed_status =
        galay_mcp_parse_request(request.data(), request.size(), &parsed);
    if (parsed_status != GALAY_OK) {
        *response = make_error_response(0, -32700, "parse error");
        return GALAY_OK;
    }
    const int64_t id = parsed->id;
    const bool notification = parsed->notification;
    const std::string method = parsed->method;
    const std::string params = parsed->params;
    galay_mcp_parsed_request_destroy(parsed);

    if (notification) {
        return GALAY_OK;
    }

    if (method == "initialize") {
        *response = make_result_response(id, build_initialize_result(server));
        return GALAY_OK;
    }
    if (method == "ping") {
        *response = make_result_response(id, "{}");
        return GALAY_OK;
    }
    if (method == "tools/list") {
        *response = make_result_response(id, build_tools_list(server));
        return GALAY_OK;
    }
    if (method == "resources/list") {
        *response = make_result_response(id, build_resources_list(server));
        return GALAY_OK;
    }
    if (method == "prompts/list") {
        *response = make_result_response(id, build_prompts_list(server));
        return GALAY_OK;
    }
    if (method == "tools/call") {
        std::string tool_name;
        if (!find_string_field(params, "name", &tool_name)) {
            *response = make_error_response(id, -32602, "missing tool name");
            return GALAY_OK;
        }
        ToolRegistration* tool = find_tool(server, tool_name);
        if (tool == nullptr) {
            *response = make_error_response(id, -32601, "tool not found");
            return GALAY_OK;
        }
        std::string arguments;
        if (!find_json_field(params, "arguments", &arguments)) {
            arguments = "{}";
        }
        galay_mcp_message_t handler_result;
        const galay_status_t handler_status =
            tool->handler(arguments.data(), arguments.size(), &handler_result, tool->userdata);
        if (handler_status != GALAY_OK) {
            *response = make_error_response(id, -32603, galay_status_string(handler_status));
            return GALAY_OK;
        }
        *response = make_result_response(id, handler_result.data);
        return GALAY_OK;
    }
    if (method == "resources/read") {
        std::string uri;
        if (!find_string_field(params, "uri", &uri)) {
            *response = make_error_response(id, -32602, "missing resource uri");
            return GALAY_OK;
        }
        ResourceRegistration* resource = find_resource(server, uri);
        if (resource == nullptr) {
            *response = make_error_response(id, -32601, "resource not found");
            return GALAY_OK;
        }
        galay_mcp_message_t reader_result;
        const galay_status_t reader_status =
            resource->reader(uri.data(), uri.size(), &reader_result, resource->userdata);
        if (reader_status != GALAY_OK) {
            *response = make_error_response(id, -32603, galay_status_string(reader_status));
            return GALAY_OK;
        }
        *response = make_result_response(id, reader_result.data);
        return GALAY_OK;
    }
    if (method == "prompts/get") {
        std::string name;
        if (!find_string_field(params, "name", &name)) {
            *response = make_error_response(id, -32602, "missing prompt name");
            return GALAY_OK;
        }
        PromptRegistration* prompt = find_prompt(server, name);
        if (prompt == nullptr) {
            *response = make_error_response(id, -32601, "prompt not found");
            return GALAY_OK;
        }
        std::string arguments;
        if (!find_json_field(params, "arguments", &arguments)) {
            arguments = "{}";
        }
        galay_mcp_message_t getter_result;
        const galay_status_t getter_status =
            prompt->getter(name.data(), name.size(), arguments.data(), arguments.size(),
                           &getter_result, prompt->userdata);
        if (getter_status != GALAY_OK) {
            *response = make_error_response(id, -32603, galay_status_string(getter_status));
            return GALAY_OK;
        }
        *response = make_result_response(id, getter_result.data);
        return GALAY_OK;
    }

    *response = make_error_response(id, -32601, "method not found");
    return GALAY_OK;
}

C_IOResult http_round_trip(galay_mcp_client_t* client,
                           const std::string& request_body,
                           int64_t timeout_ms,
                           std::string* response_body)
{
    if (client == nullptr || response_body == nullptr || client->http_host.empty() ||
        client->http_port == 0) {
        return make_io_result(C_IOResultInvalid);
    }
    C_Host host{};
    if (!copy_host_to_c_host(client->http_host, client->http_port, &host)) {
        return make_io_result(C_IOResultInvalid);
    }

    galay_c_tcp_socket_t socket{};
    socket.fd = -1;
    C_IOResult created = galay_c_tcp_socket_create(&socket, host.type);
    if (created.code != C_IOResultOk) {
        return created;
    }

    C_IOResult final_result = galay_c_tcp_socket_connect(&socket, &host, timeout_ms);
    if (final_result.code == C_IOResultOk) {
        std::string request = "POST " + client->http_path + " HTTP/1.1\r\nHost: " +
            client->http_host + "\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(request_body.size()) + "\r\n";
        if (!client->bearer_token.empty()) {
            request += "Authorization: Bearer " + client->bearer_token + "\r\n";
        }
        request += "Connection: close\r\n\r\n" + request_body;
        final_result = socket_write_exact(&socket, request.data(), request.size(), timeout_ms);
    }
    if (final_result.code == C_IOResultOk) {
        final_result = read_http_body(&socket, timeout_ms, response_body);
    }

    if (socket.fd >= 0) {
        C_IOResult close_result = galay_c_tcp_socket_close(&socket);
        if (final_result.code == C_IOResultOk && close_result.code != C_IOResultOk) {
            final_result = close_result;
        }
    }
    return final_result;
}

C_IOResult send_client_request(galay_mcp_client_t* client,
                               const char* method,
                               const std::string& params,
                               int64_t timeout_ms,
                               galay_mcp_message_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (client == nullptr || method == nullptr || !client->connected) {
        return make_io_result(C_IOResultInvalid);
    }
    if (std::strcmp(method, "initialize") != 0 && !client->initialized) {
        return make_io_result(C_IOResultInvalid);
    }

    const int64_t request_id = client->next_id++;
    std::string request;
    galay_mcp_message_t request_message;
    const galay_status_t built =
        galay_mcp_build_request(&request_message, request_id, method, params.c_str());
    if (built != GALAY_OK) {
        return io_result_from_status(built);
    }
    request = request_message.data;

    std::string response;
    if (client->mode == GALAY_MCP_MODE_STDIO) {
        if (client->loopback_server == nullptr) {
            return make_io_result(C_IOResultInvalid);
        }
        const galay_status_t processed = process_mcp_request(client->loopback_server, request, &response);
        if (processed != GALAY_OK) {
            return io_result_from_status(processed);
        }
    } else {
        C_IOResult http_result = http_round_trip(client, request, timeout_ms, &response);
        if (http_result.code != C_IOResultOk) {
            return http_result;
        }
    }

    if (response.find("\"error\"") != std::string::npos) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    std::string result_json;
    if (!find_json_field(response, "result", &result_json)) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    if (out != nullptr) {
        auto* message = new (std::nothrow) galay_mcp_message_t();
        if (message == nullptr) {
            return make_io_result(C_IOResultError, GALAY_OUT_OF_MEMORY);
        }
        message->data = result_json;
        *out = message;
    }
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = result_json.size();
    if (out != nullptr) {
        result.ptr = *out;
    }
    return result;
}


extern "C" {

const char* galay_mcp_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_mcp_message_create(galay_mcp_message_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_mcp_message_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mcp_message_destroy(galay_mcp_message_t* message)
{
    delete message;
}

galay_status_t galay_mcp_message_set_json(galay_mcp_message_t* message, const char* json)
{
    if (message == nullptr || json == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    message->data = json;
    return GALAY_OK;
}

galay_status_t galay_mcp_message_data(const galay_mcp_message_t* message, const char** data, size_t* data_len)
{
    if (message == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *data = message->data.data();
    *data_len = message->data.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_build_request(galay_mcp_message_t* message, int64_t id, const char* method, const char* params)
{
    if (message == nullptr || !valid_method(method)) {
        return GALAY_INVALID_ARGUMENT;
    }
    message->data = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":" +
        json_string(method);
    if (params != nullptr) {
        message->data += ",\"params\":" + std::string(params);
    }
    message->data += "}";
    return GALAY_OK;
}

galay_status_t galay_mcp_build_notification(galay_mcp_message_t* message, const char* method, const char* params)
{
    if (message == nullptr || !valid_method(method)) {
        return GALAY_INVALID_ARGUMENT;
    }
    message->data = "{\"jsonrpc\":\"2.0\",\"method\":" + json_string(method);
    if (params != nullptr) {
        message->data += ",\"params\":" + std::string(params);
    }
    message->data += "}";
    return GALAY_OK;
}

galay_status_t galay_mcp_build_initialized_notification(galay_mcp_message_t* message)
{
    return galay_mcp_build_notification(message, "notifications/initialized", nullptr);
}

galay_status_t galay_mcp_build_empty_result_response(galay_mcp_message_t* message, int64_t id)
{
    if (message == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    message->data = make_result_response(id, "{}");
    return GALAY_OK;
}

galay_status_t galay_mcp_parse_request(const char* data, size_t data_len, galay_mcp_parsed_request_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (data == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::string text(data, data_len);
    std::string jsonrpc;
    if (!find_string_field(text, "jsonrpc", &jsonrpc) || jsonrpc != "2.0") {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* parsed = new (std::nothrow) galay_mcp_parsed_request_t();
    if (parsed == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    if (!find_string_field(text, "method", &parsed->method)) {
        delete parsed;
        return GALAY_PROTOCOL_ERROR;
    }
    size_t id_start = 0;
    size_t id_end = 0;
    if (find_top_level_json_field(text, "id", &id_start, &id_end)) {
        if (!find_int_field(text, "id", &parsed->id)) {
            delete parsed;
            return GALAY_PROTOCOL_ERROR;
        }
        parsed->notification = false;
    } else {
        parsed->notification = true;
    }
    std::string params;
    if (find_json_field(text, "params", &params)) {
        parsed->params = params;
    }
    *out = parsed;
    return GALAY_OK;
}

void galay_mcp_parsed_request_destroy(galay_mcp_parsed_request_t* request)
{
    delete request;
}

galay_bool_t galay_mcp_request_is_notification(const galay_mcp_parsed_request_t* request)
{
    return request != nullptr && request->notification ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_mcp_request_id(const galay_mcp_parsed_request_t* request, int64_t* id)
{
    if (request == nullptr || id == nullptr || request->notification) {
        return GALAY_PROTOCOL_ERROR;
    }
    *id = request->id;
    return GALAY_OK;
}

galay_status_t galay_mcp_request_method(const galay_mcp_parsed_request_t* request, const char** method, size_t* method_len)
{
    if (request == nullptr || method == nullptr || method_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *method = request->method.data();
    *method_len = request->method.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_request_params(const galay_mcp_parsed_request_t* request, const char** params, size_t* params_len)
{
    if (request == nullptr || params == nullptr || params_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *params = request->params.data();
    *params_len = request->params.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_parse_response(const char* data, size_t data_len, galay_mcp_parsed_response_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (data == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::string text(data, data_len);
    std::string jsonrpc;
    size_t method_start = 0;
    size_t method_end = 0;
    size_t error_start = 0;
    size_t error_end = 0;
    if (!find_string_field(text, "jsonrpc", &jsonrpc) || jsonrpc != "2.0" ||
        find_top_level_json_field(text, "method", &method_start, &method_end) ||
        find_top_level_json_field(text, "error", &error_start, &error_end)) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* parsed = new (std::nothrow) galay_mcp_parsed_response_t();
    if (parsed == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    if (!find_int_field(text, "id", &parsed->id) || !find_json_field(text, "result", &parsed->result)) {
        delete parsed;
        return GALAY_PROTOCOL_ERROR;
    }
    *out = parsed;
    return GALAY_OK;
}

void galay_mcp_parsed_response_destroy(galay_mcp_parsed_response_t* response)
{
    delete response;
}

galay_status_t galay_mcp_response_id(const galay_mcp_parsed_response_t* response, int64_t* id)
{
    if (response == nullptr || id == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *id = response->id;
    return GALAY_OK;
}

galay_bool_t galay_mcp_response_has_result(const galay_mcp_parsed_response_t* response)
{
    return response != nullptr && !response->result.empty() ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_mcp_response_result(const galay_mcp_parsed_response_t* response, const char** result, size_t* result_len)
{
    if (response == nullptr || result == nullptr || result_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *result = response->result.data();
    *result_len = response->result.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_stdio_config_create(const char*, const char*, galay_mcp_client_config_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* config = new (std::nothrow) galay_mcp_client_config_t();
    if (config == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    config->mode = GALAY_MCP_MODE_STDIO;
    *out = config;
    return GALAY_OK;
}

galay_status_t galay_mcp_http_config_create(const char* url, galay_mcp_client_config_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (url == nullptr || url[0] == '\0' || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* config = new (std::nothrow) galay_mcp_client_config_t();
    if (config == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    config->mode = GALAY_MCP_MODE_HTTP;
    config->url = url;
    *out = config;
    return GALAY_OK;
}

galay_status_t galay_mcp_http_config_set_bearer_token(galay_mcp_client_config_t* config,
                                                      const char* bearer_token)
{
    if (config == nullptr || config->mode != GALAY_MCP_MODE_HTTP ||
        bearer_token == nullptr || !valid_bearer_token(bearer_token)) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->bearer_token = bearer_token;
    return GALAY_OK;
}

void galay_mcp_client_config_destroy(galay_mcp_client_config_t* config)
{
    delete config;
}

galay_mcp_mode_t galay_mcp_client_config_mode(const galay_mcp_client_config_t* config)
{
    return config == nullptr ? GALAY_MCP_MODE_STDIO : config->mode;
}

galay_status_t galay_mcp_http_config_url(const galay_mcp_client_config_t* config, const char** url, size_t* url_len)
{
    if (config == nullptr || url == nullptr || url_len == nullptr || config->mode != GALAY_MCP_MODE_HTTP) {
        return GALAY_INVALID_ARGUMENT;
    }
    *url = config->url.data();
    *url_len = config->url.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_client_create(const galay_mcp_client_config_t* config, galay_mcp_client_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (config == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* client = new (std::nothrow) galay_mcp_client_t();
    if (client == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    client->mode = config->mode;
    client->url = config->url;
    client->bearer_token = config->bearer_token;
    if (client->mode == GALAY_MCP_MODE_HTTP &&
        !parse_http_url(client->url, &client->http_host, &client->http_port, &client->http_path)) {
        delete client;
        return GALAY_INVALID_ARGUMENT;
    }
    *out = client;
    return GALAY_OK;
}

void galay_mcp_client_destroy(galay_mcp_client_t* client)
{
    delete client;
}

galay_bool_t galay_mcp_client_is_connected(const galay_mcp_client_t* client)
{
    return client != nullptr && client->connected ? GALAY_TRUE : GALAY_FALSE;
}

C_IOResult galay_mcp_client_connect_stdio_loopback(galay_mcp_client_t* client,
                                                   galay_mcp_server_t* server,
                                                   int64_t)
{
    if (client == nullptr || server == nullptr || client->mode != GALAY_MCP_MODE_STDIO ||
        server->mode != GALAY_MCP_MODE_STDIO) {
        return make_io_result(C_IOResultInvalid);
    }
    client->loopback_server = server;
    client->connected = true;
    client->initialized = false;
    return make_io_result(C_IOResultOk);
}

C_IOResult galay_mcp_client_connect_async(galay_mcp_client_t* client, int64_t)
{
    if (client == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    if (client->mode == GALAY_MCP_MODE_HTTP &&
        (client->http_host.empty() || client->http_port == 0 || client->http_path.empty())) {
        return make_io_result(C_IOResultInvalid);
    }
    if (client->mode == GALAY_MCP_MODE_STDIO && client->loopback_server == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    client->connected = true;
    client->initialized = false;
    return make_io_result(C_IOResultOk);
}

C_IOResult galay_mcp_client_disconnect_async(galay_mcp_client_t* client, int64_t)
{
    if (client == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    client->connected = false;
    client->initialized = false;
    return make_io_result(C_IOResultOk);
}

C_IOResult galay_mcp_client_initialize_async(galay_mcp_client_t* client,
                                             const char* client_name,
                                             const char* client_version,
                                             int64_t timeout_ms)
{
    if (client == nullptr || client_name == nullptr || client_version == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    std::string params = "{\"protocolVersion\":\"2024-11-05\",\"clientInfo\":{\"name\":";
    append_json_string(&params, client_name);
    params += ",\"version\":";
    append_json_string(&params, client_version);
    params += "},\"capabilities\":{}}";
    C_IOResult result = send_client_request(client, "initialize", params, timeout_ms, nullptr);
    if (result.code == C_IOResultOk) {
        client->initialized = true;
    }
    return result;
}

C_IOResult galay_mcp_client_ping_async(galay_mcp_client_t* client, int64_t timeout_ms)
{
    return send_client_request(client, "ping", "{}", timeout_ms, nullptr);
}

C_IOResult galay_mcp_client_list_tools_async(galay_mcp_client_t* client,
                                             int64_t timeout_ms,
                                             galay_mcp_message_t** result)
{
    return send_client_request(client, "tools/list", "{}", timeout_ms, result);
}

C_IOResult galay_mcp_client_call_tool_async(galay_mcp_client_t* client,
                                            const char* tool_name,
                                            const char* arguments_json,
                                            int64_t timeout_ms,
                                            galay_mcp_message_t** result)
{
    if (tool_name == nullptr || result == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    std::string params = "{\"name\":";
    append_json_string(&params, tool_name);
    params += ",\"arguments\":";
    params += arguments_json == nullptr || arguments_json[0] == '\0' ? "{}" : arguments_json;
    params.push_back('}');
    return send_client_request(client, "tools/call", params, timeout_ms, result);
}

C_IOResult galay_mcp_client_list_resources_async(galay_mcp_client_t* client,
                                                 int64_t timeout_ms,
                                                 galay_mcp_message_t** result)
{
    return send_client_request(client, "resources/list", "{}", timeout_ms, result);
}

C_IOResult galay_mcp_client_read_resource_async(galay_mcp_client_t* client,
                                                const char* uri,
                                                int64_t timeout_ms,
                                                galay_mcp_message_t** result)
{
    if (uri == nullptr || result == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    std::string params = "{\"uri\":";
    append_json_string(&params, uri);
    params.push_back('}');
    return send_client_request(client, "resources/read", params, timeout_ms, result);
}

C_IOResult galay_mcp_client_list_prompts_async(galay_mcp_client_t* client,
                                               int64_t timeout_ms,
                                               galay_mcp_message_t** result)
{
    return send_client_request(client, "prompts/list", "{}", timeout_ms, result);
}

C_IOResult galay_mcp_client_get_prompt_async(galay_mcp_client_t* client,
                                             const char* name,
                                             const char* arguments_json,
                                             int64_t timeout_ms,
                                             galay_mcp_message_t** result)
{
    if (name == nullptr || result == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    std::string params = "{\"name\":";
    append_json_string(&params, name);
    params += ",\"arguments\":";
    params += arguments_json == nullptr || arguments_json[0] == '\0' ? "{}" : arguments_json;
    params.push_back('}');
    return send_client_request(client, "prompts/get", params, timeout_ms, result);
}

galay_status_t galay_mcp_stdio_server_create(galay_mcp_server_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* server = new (std::nothrow) galay_mcp_server_t();
    if (server == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    server->mode = GALAY_MCP_MODE_STDIO;
    server->listener.fd = -1;
    *out = server;
    return GALAY_OK;
}

galay_status_t galay_mcp_http_server_create(const char* host, uint16_t port, galay_mcp_server_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (host == nullptr || !valid_http_host(host) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* server = new (std::nothrow) galay_mcp_server_t();
    if (server == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    server->mode = GALAY_MCP_MODE_HTTP;
    server->http_host = host;
    server->http_port = port;
    server->listener.fd = -1;
    *out = server;
    return GALAY_OK;
}

void galay_mcp_server_destroy(galay_mcp_server_t* server)
{
    if (server != nullptr && server->listener.fd >= 0) {
        const C_IOResult closed = galay_c_tcp_socket_close(&server->listener);
        (void)closed; /* void destroy cannot propagate an OS close failure. */
    }
    delete server;
}

galay_status_t galay_mcp_server_set_info(galay_mcp_server_t* server,
                                         const char* name,
                                         const char* version)
{
    if (server == nullptr || name == nullptr || version == nullptr ||
        name[0] == '\0' || version[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    server->name = name;
    server->version = version;
    return GALAY_OK;
}

galay_status_t galay_mcp_http_server_require_bearer_token(galay_mcp_server_t* server,
                                                          const char* bearer_token)
{
    if (server == nullptr || server->mode != GALAY_MCP_MODE_HTTP ||
        bearer_token == nullptr || !valid_bearer_token(bearer_token)) {
        return GALAY_INVALID_ARGUMENT;
    }
    server->bearer_token = bearer_token;
    return GALAY_OK;
}

galay_status_t galay_mcp_server_add_tool(galay_mcp_server_t* server,
                                         const char* name,
                                         const char* description,
                                         const char* input_schema_json,
                                         galay_mcp_tool_handler_fn handler,
                                         void* userdata)
{
    if (server == nullptr || name == nullptr || name[0] == '\0' || handler == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    ToolRegistration tool;
    tool.name = name;
    tool.description = description == nullptr ? "" : description;
    tool.input_schema = input_schema_json == nullptr || input_schema_json[0] == '\0'
        ? "{}"
        : input_schema_json;
    tool.handler = handler;
    tool.userdata = userdata;
    server->tools.push_back(tool);
    return GALAY_OK;
}

galay_status_t galay_mcp_server_add_resource(galay_mcp_server_t* server,
                                             const char* uri,
                                             const char* name,
                                             const char* description,
                                             const char* mime_type,
                                             galay_mcp_resource_reader_fn reader,
                                             void* userdata)
{
    if (server == nullptr || uri == nullptr || uri[0] == '\0' || name == nullptr ||
        name[0] == '\0' || reader == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    ResourceRegistration resource;
    resource.uri = uri;
    resource.name = name;
    resource.description = description == nullptr ? "" : description;
    resource.mime_type = mime_type == nullptr ? "application/octet-stream" : mime_type;
    resource.reader = reader;
    resource.userdata = userdata;
    server->resources.push_back(resource);
    return GALAY_OK;
}

galay_status_t galay_mcp_server_add_prompt(galay_mcp_server_t* server,
                                           const char* name,
                                           const char* description,
                                           const char* arguments_json,
                                           galay_mcp_prompt_getter_fn getter,
                                           void* userdata)
{
    if (server == nullptr || name == nullptr || name[0] == '\0' || getter == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    PromptRegistration prompt;
    prompt.name = name;
    prompt.description = description == nullptr ? "" : description;
    prompt.arguments = arguments_json == nullptr || arguments_json[0] == '\0' ? "[]" : arguments_json;
    prompt.getter = getter;
    prompt.userdata = userdata;
    server->prompts.push_back(prompt);
    return GALAY_OK;
}

galay_status_t galay_mcp_http_server_start(galay_mcp_server_t* server)
{
    if (server == nullptr || server->mode != GALAY_MCP_MODE_HTTP || server->listening) {
        return GALAY_INVALID_ARGUMENT;
    }
    C_Host host{};
    if (!copy_host_to_c_host(server->http_host, server->http_port, &host) && server->http_port != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (server->http_port == 0) {
        if (server->http_host.empty() || server->http_host.size() >= sizeof(host.address)) {
            return GALAY_INVALID_ARGUMENT;
        }
        host.type = server->http_host.find(':') == std::string::npos ? C_IPTypeIPV4 : C_IPTypeIPV6;
        std::memset(host.address, 0, sizeof(host.address));
        std::memcpy(host.address, server->http_host.data(), server->http_host.size());
        host.port = 0;
    }
    C_IOResult created = galay_c_tcp_socket_create(&server->listener, host.type);
    if (created.code != C_IOResultOk) {
        return GALAY_IO_ERROR;
    }
    C_IOResult bound = galay_c_tcp_socket_bind(&server->listener, &host);
    if (bound.code != C_IOResultOk) {
        const C_IOResult closed = galay_c_tcp_socket_close(&server->listener);
        return closed.code == C_IOResultOk ? GALAY_IO_ERROR : GALAY_INTERNAL_ERROR;
    }
    C_IOResult listening = galay_c_tcp_socket_listen(&server->listener, 16);
    if (listening.code != C_IOResultOk) {
        const C_IOResult closed = galay_c_tcp_socket_close(&server->listener);
        return closed.code == C_IOResultOk ? GALAY_IO_ERROR : GALAY_INTERNAL_ERROR;
    }
    C_Host endpoint{};
    C_IOResult endpoint_status =
        galay_c_tcp_socket_local_endpoint(&server->listener, &endpoint);
    if (endpoint_status.code != C_IOResultOk) {
        const C_IOResult closed = galay_c_tcp_socket_close(&server->listener);
        return closed.code == C_IOResultOk ? GALAY_IO_ERROR : GALAY_INTERNAL_ERROR;
    }
    server->http_host = endpoint.address;
    server->http_port = endpoint.port;
    server->listening = true;
    return GALAY_OK;
}

galay_status_t galay_mcp_http_server_endpoint(const galay_mcp_server_t* server,
                                              const char** host,
                                              uint16_t* port)
{
    if (server == nullptr || host == nullptr || port == nullptr ||
        server->mode != GALAY_MCP_MODE_HTTP || !server->listening) {
        return GALAY_INVALID_ARGUMENT;
    }
    *host = server->http_host.data();
    *port = server->http_port;
    return GALAY_OK;
}

C_IOResult galay_mcp_http_server_serve_once(galay_mcp_server_t* server, int64_t timeout_ms)
{
    if (server == nullptr || server->mode != GALAY_MCP_MODE_HTTP || !server->listening) {
        return make_io_result(C_IOResultInvalid);
    }
    galay_c_tcp_socket_t accepted{};
    accepted.fd = -1;
    C_IOResult final_result =
        galay_c_tcp_socket_accept(&server->listener, &accepted, nullptr, timeout_ms);
    if (final_result.code == C_IOResultOk) {
        std::string headers;
        std::string request_body;
        final_result = read_http_message(&accepted, timeout_ms, &headers, &request_body);
        std::string response_body;
        if (final_result.code == C_IOResultOk) {
            if (!has_expected_authorization(headers, server->bearer_token)) {
                response_body = make_error_response(0, -32001, "unauthorized");
            } else {
                const galay_status_t processed = process_mcp_request(server, request_body, &response_body);
                if (processed != GALAY_OK) {
                    final_result = io_result_from_status(processed);
                }
            }
        }
        if (final_result.code == C_IOResultOk) {
            if (response_body.empty()) {
                response_body = "{}";
            }
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(response_body.size()) + "\r\nConnection: close\r\n\r\n" + response_body;
            final_result = socket_write_exact(&accepted, response.data(), response.size(), timeout_ms);
        }
    }
    if (accepted.fd >= 0) {
        C_IOResult close_result = galay_c_tcp_socket_close(&accepted);
        if (final_result.code == C_IOResultOk && close_result.code != C_IOResultOk) {
            final_result = close_result;
        }
    }
    return final_result;
}

C_IOResult galay_mcp_http_server_stop(galay_mcp_server_t* server)
{
    if (server == nullptr || server->mode != GALAY_MCP_MODE_HTTP) {
        return make_io_result(C_IOResultInvalid);
    }
    if (server->listener.fd >= 0) {
        C_IOResult closed = galay_c_tcp_socket_close(&server->listener);
        if (closed.code != C_IOResultOk) {
            return closed;
        }
    }
    server->listening = false;
    return make_io_result(C_IOResultOk);
}

}
