#include <galay/c/galay-http-c/http.h>

#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct galay_http_header_item {
    std::string name;
    std::string value;
};

struct galay_http_headers_t {
    std::vector<galay_http_header_item> entries;
};

struct galay_http_request_t {
    galay_http_method_t method = GALAY_HTTP_METHOD_GET;
    std::string path = "/";
    galay_http_headers_t headers;
    std::string body;
    std::string serialized;
    bool complete = false;
};

struct galay_http_response_t {
    int status = GALAY_HTTP_STATUS_OK;
    galay_http_headers_t headers;
    std::string body;
    std::string serialized;
    bool complete = false;
};

struct galay_http_session_t {
    galay_kernel_tcp_socket_t socket{nullptr};
    bool closed = false;
};

struct galay_http_client_t {
    galay_http_session_t session;
};

struct galay_http_route_entry {
    galay_http_method_t method;
    std::string path;
    galay_http_route_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct galay_http_server_t {
    galay_kernel_tcp_socket_t listener{nullptr};
    bool listening = false;
    std::vector<galay_http_route_entry> routes;
};

namespace
{

enum class ParseState {
    kComplete,
    kNeedMore,
    kError,
};

struct ParseResult {
    ParseState state = ParseState::kError;
    size_t consumed = 0;
};

C_IOResult make_io_result(C_IOResultCode code, int sys_errno = 0, size_t bytes = 0, void* ptr = nullptr)
{
    return C_IOResult{code, sys_errno, bytes, 0, ptr};
}

C_IOResult io_from_status(galay_status_t status)
{
    return status == GALAY_INVALID_ARGUMENT
        ? make_io_result(C_IOResultInvalid)
        : make_io_result(C_IOResultError);
}

std::string lower_copy(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string trim_header_value(std::string_view text)
{
    size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

const char* method_name(galay_http_method_t method)
{
    switch (method) {
    case GALAY_HTTP_METHOD_GET:
        return "GET";
    case GALAY_HTTP_METHOD_POST:
        return "POST";
    }
    return nullptr;
}

bool method_from_text(std::string_view text, galay_http_method_t& method)
{
    if (text == "GET") {
        method = GALAY_HTTP_METHOD_GET;
        return true;
    }
    if (text == "POST") {
        method = GALAY_HTTP_METHOD_POST;
        return true;
    }
    return false;
}

bool valid_status(int status)
{
    return status == GALAY_HTTP_STATUS_OK ||
           status == GALAY_HTTP_STATUS_NO_CONTENT ||
           status == GALAY_HTTP_STATUS_BAD_REQUEST ||
           status == GALAY_HTTP_STATUS_NOT_FOUND ||
           status == GALAY_HTTP_STATUS_INTERNAL_SERVER_ERROR;
}

const char* reason_phrase(int status)
{
    switch (status) {
    case GALAY_HTTP_STATUS_NO_CONTENT:
        return "No Content";
    case GALAY_HTTP_STATUS_BAD_REQUEST:
        return "Bad Request";
    case GALAY_HTTP_STATUS_NOT_FOUND:
        return "Not Found";
    case GALAY_HTTP_STATUS_INTERNAL_SERVER_ERROR:
        return "Internal Server Error";
    case GALAY_HTTP_STATUS_OK:
    default:
        return "OK";
    }
}

bool parse_size_decimal(std::string_view text, size_t& value)
{
    if (text.empty()) {
        return false;
    }
    size_t parsed = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const size_t digit = static_cast<size_t>(ch - '0');
        if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

bool parse_status_decimal(std::string_view text, int& value)
{
    if (text.size() != 3) {
        return false;
    }
    int parsed = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        parsed = parsed * 10 + (ch - '0');
    }
    value = parsed;
    return valid_status(parsed);
}

galay_status_t add_header_value(galay_http_headers_t& headers,
                                std::string_view name,
                                std::string_view value)
{
    if (name.empty()) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string key = lower_copy(name);
    for (auto& entry : headers.entries) {
        if (entry.name == key) {
            entry.value += ", ";
            entry.value += value;
            return GALAY_OK;
        }
    }
    headers.entries.push_back(galay_http_header_item{key, std::string(value)});
    return GALAY_OK;
}

bool read_content_length(const galay_http_headers_t& headers, size_t& content_length)
{
    content_length = 0;
    for (const auto& entry : headers.entries) {
        if (entry.name == "content-length") {
            return parse_size_decimal(entry.value, content_length);
        }
    }
    return true;
}

ParseResult parse_headers_block(std::string_view header_block,
                                galay_http_headers_t& headers,
                                size_t& content_length)
{
    headers.entries.clear();
    content_length = 0;

    size_t pos = 0;
    while (pos < header_block.size()) {
        const size_t next = header_block.find("\r\n", pos);
        const size_t line_end = next == std::string_view::npos ? header_block.size() : next;
        const std::string_view line = header_block.substr(pos, line_end - pos);
        if (line.empty()) {
            return ParseResult{ParseState::kError, 0};
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return ParseResult{ParseState::kError, 0};
        }
        const std::string_view name = line.substr(0, colon);
        const std::string value = trim_header_value(line.substr(colon + 1));
        const galay_status_t add_status = add_header_value(headers, name, value);
        if (add_status != GALAY_OK) {
            return ParseResult{ParseState::kError, 0};
        }
        pos = next == std::string_view::npos ? header_block.size() : next + 2;
    }

    if (!read_content_length(headers, content_length)) {
        return ParseResult{ParseState::kError, 0};
    }
    return ParseResult{ParseState::kComplete, 0};
}

ParseResult parse_request_internal(galay_http_request_t& request,
                                   std::string_view data,
                                   size_t max_header_len,
                                   size_t max_body_len)
{
    const size_t header_end = data.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return data.size() > max_header_len
            ? ParseResult{ParseState::kError, 0}
            : ParseResult{ParseState::kNeedMore, 0};
    }
    if (header_end + 4 > max_header_len) {
        return ParseResult{ParseState::kError, 0};
    }

    const size_t line_end = data.find("\r\n");
    if (line_end == std::string_view::npos || line_end == 0 || line_end > header_end) {
        return ParseResult{ParseState::kError, 0};
    }

    const std::string_view request_line = data.substr(0, line_end);
    const size_t first_space = request_line.find(' ');
    const size_t second_space = first_space == std::string_view::npos
        ? std::string_view::npos
        : request_line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos ||
        second_space == std::string_view::npos ||
        second_space + 1 >= request_line.size()) {
        return ParseResult{ParseState::kError, 0};
    }

    galay_http_method_t method = GALAY_HTTP_METHOD_GET;
    if (!method_from_text(request_line.substr(0, first_space), method)) {
        return ParseResult{ParseState::kError, 0};
    }

    const std::string_view path = request_line.substr(first_space + 1,
                                                      second_space - first_space - 1);
    const std::string_view version = request_line.substr(second_space + 1);
    if (path.empty() || (version != "HTTP/1.1" && version != "HTTP/1.0")) {
        return ParseResult{ParseState::kError, 0};
    }

    size_t content_length = 0;
    const std::string_view header_block = data.substr(line_end + 2, header_end - line_end - 2);
    if (!header_block.empty()) {
        const ParseResult headers_result =
            parse_headers_block(header_block, request.headers, content_length);
        if (headers_result.state != ParseState::kComplete) {
            return headers_result;
        }
    } else {
        request.headers.entries.clear();
    }

    if (content_length > max_body_len) {
        return ParseResult{ParseState::kError, 0};
    }
    const size_t body_begin = header_end + 4;
    if (data.size() < body_begin + content_length) {
        return ParseResult{ParseState::kNeedMore, 0};
    }

    request.method = method;
    request.path.assign(path);
    request.body.assign(data.substr(body_begin, content_length));
    request.complete = true;
    return ParseResult{ParseState::kComplete, body_begin + content_length};
}

ParseResult parse_response_internal(galay_http_response_t& response,
                                    std::string_view data,
                                    size_t max_header_len,
                                    size_t max_body_len)
{
    const size_t header_end = data.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return data.size() > max_header_len
            ? ParseResult{ParseState::kError, 0}
            : ParseResult{ParseState::kNeedMore, 0};
    }
    if (header_end + 4 > max_header_len) {
        return ParseResult{ParseState::kError, 0};
    }

    const size_t line_end = data.find("\r\n");
    if (line_end == std::string_view::npos || line_end == 0 || line_end > header_end) {
        return ParseResult{ParseState::kError, 0};
    }

    const std::string_view status_line = data.substr(0, line_end);
    if (status_line.size() < 12 || status_line.substr(0, 9) != "HTTP/1.1 ") {
        return ParseResult{ParseState::kError, 0};
    }

    int status = 0;
    if (!parse_status_decimal(status_line.substr(9, 3), status)) {
        return ParseResult{ParseState::kError, 0};
    }

    size_t content_length = 0;
    const std::string_view header_block = data.substr(line_end + 2, header_end - line_end - 2);
    if (!header_block.empty()) {
        const ParseResult headers_result =
            parse_headers_block(header_block, response.headers, content_length);
        if (headers_result.state != ParseState::kComplete) {
            return headers_result;
        }
    } else {
        response.headers.entries.clear();
    }

    if (content_length > max_body_len) {
        return ParseResult{ParseState::kError, 0};
    }
    const size_t body_begin = header_end + 4;
    if (data.size() < body_begin + content_length) {
        return ParseResult{ParseState::kNeedMore, 0};
    }

    response.status = status;
    response.body.assign(data.substr(body_begin, content_length));
    response.complete = true;
    return ParseResult{ParseState::kComplete, body_begin + content_length};
}

galay_status_t serialize_headers(const galay_http_headers_t& headers,
                                 std::string& serialized)
{
    for (const auto& entry : headers.entries) {
        if (entry.name == "content-length") {
            continue;
        }
        if (entry.name.empty()) {
            return GALAY_PROTOCOL_ERROR;
        }
        serialized += entry.name;
        serialized += ": ";
        serialized += entry.value;
        serialized += "\r\n";
    }
    return GALAY_OK;
}

galay_status_t serialize_request(galay_http_request_t& request)
{
    const char* method = method_name(request.method);
    if (method == nullptr || request.path.empty()) {
        return GALAY_INVALID_ARGUMENT;
    }
    request.serialized.clear();
    request.serialized += method;
    request.serialized += ' ';
    request.serialized += request.path;
    request.serialized += " HTTP/1.1\r\n";
    const galay_status_t headers_status = serialize_headers(request.headers, request.serialized);
    if (headers_status != GALAY_OK) {
        return headers_status;
    }
    request.serialized += "content-length: ";
    request.serialized += std::to_string(request.body.size());
    request.serialized += "\r\n\r\n";
    request.serialized += request.body;
    return GALAY_OK;
}

galay_status_t serialize_response(galay_http_response_t& response)
{
    if (!valid_status(response.status)) {
        return GALAY_INVALID_ARGUMENT;
    }
    response.serialized.clear();
    response.serialized = "HTTP/1.1 " + std::to_string(response.status) + " " +
        reason_phrase(response.status) + "\r\n";
    const galay_status_t headers_status = serialize_headers(response.headers, response.serialized);
    if (headers_status != GALAY_OK) {
        return headers_status;
    }
    response.serialized += "content-length: ";
    response.serialized += std::to_string(response.body.size());
    response.serialized += "\r\n\r\n";
    response.serialized += response.body;
    return GALAY_OK;
}

C_IOResult send_all(galay_http_session_t* session,
                    const char* data,
                    size_t data_len,
                    int64_t timeout_ms)
{
    if (session == nullptr || session->socket.socket == nullptr ||
        session->closed || (data == nullptr && data_len != 0)) {
        return make_io_result(C_IOResultInvalid);
    }
    size_t sent = 0;
    while (sent < data_len) {
        C_IOResult result =
            galay_kernel_tcp_socket_send(&session->socket, data + sent, data_len - sent, timeout_ms);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        sent += result.bytes;
    }
    return make_io_result(C_IOResultOk, 0, sent);
}

template<typename MessageT, typename Parser>
C_IOResult recv_message(galay_http_session_t* session,
                        MessageT** out_message,
                        size_t max_header_len,
                        size_t max_body_len,
                        int64_t timeout_ms,
                        Parser parser)
{
    if (out_message != nullptr) {
        *out_message = nullptr;
    }
    if (session == nullptr || session->socket.socket == nullptr ||
        session->closed || out_message == nullptr || max_header_len == 0) {
        return make_io_result(C_IOResultInvalid);
    }

    auto* message = new (std::nothrow) MessageT();
    if (message == nullptr) {
        return make_io_result(C_IOResultError);
    }

    std::string buffer;
    char chunk[1024];
    for (;;) {
        const ParseResult parsed = parser(*message, std::string_view(buffer.data(), buffer.size()),
                                          max_header_len, max_body_len);
        if (parsed.state == ParseState::kComplete) {
            *out_message = message;
            return make_io_result(C_IOResultOk, 0, parsed.consumed, message);
        }
        if (parsed.state == ParseState::kError ||
            buffer.size() > max_header_len + max_body_len + 4) {
            delete message;
            return make_io_result(C_IOResultError);
        }

        C_IOResult read_result =
            galay_kernel_tcp_socket_recv(&session->socket, chunk, sizeof(chunk), timeout_ms);
        if (read_result.code != C_IOResultOk) {
            delete message;
            return read_result;
        }
        if (read_result.bytes == 0) {
            delete message;
            return make_io_result(C_IOResultEof);
        }
        buffer.append(chunk, read_result.bytes);
    }
}

galay_status_t destroy_socket(galay_kernel_tcp_socket_t& socket)
{
    if (socket.socket == nullptr) {
        return GALAY_OK;
    }
    const C_TcpSocketResultCode result = galay_kernel_tcp_socket_destroy(&socket);
    return result == C_TcpSocketSuccess ? GALAY_OK : GALAY_IO_ERROR;
}

C_IOResult close_session_socket(galay_http_session_t* session, int64_t timeout_ms)
{
    if (session == nullptr || session->socket.socket == nullptr || session->closed) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult result = galay_kernel_tcp_socket_close(&session->socket, timeout_ms);
    if (result.code == C_IOResultOk) {
        session->closed = true;
    }
    return result;
}

galay_status_t create_socket(galay_kernel_tcp_socket_t& socket, C_IPType type)
{
    if (socket.socket != nullptr) {
        return GALAY_OK;
    }
    const C_TcpSocketResultCode result = galay_kernel_tcp_socket_create(&socket, type);
    if (result == C_TcpSocketSuccess) {
        return GALAY_OK;
    }
    return result == C_TcpSocketMemoryAllocFailed ? GALAY_OUT_OF_MEMORY : GALAY_INVALID_ARGUMENT;
}

} // namespace

extern "C" {

const char* galay_http_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_http_headers_create(galay_http_headers_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_http_headers_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_http_headers_destroy(galay_http_headers_t* headers)
{
    delete headers;
}

galay_status_t galay_http_headers_add(galay_http_headers_t* headers, const char* name,
                                      const char* value)
{
    if (headers == nullptr || name == nullptr || value == nullptr || name[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    return add_header_value(*headers, name, value);
}

galay_status_t galay_http_headers_find(const galay_http_headers_t* headers, const char* name,
                                       const char** value, size_t* value_len)
{
    if (value != nullptr) {
        *value = nullptr;
    }
    if (value_len != nullptr) {
        *value_len = 0;
    }
    if (headers == nullptr || name == nullptr || value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string key = lower_copy(name);
    for (const auto& entry : headers->entries) {
        if (entry.name == key) {
            *value = entry.value.data();
            *value_len = entry.value.size();
            return GALAY_OK;
        }
    }
    return GALAY_NOT_FOUND;
}

galay_status_t galay_http_headers_remove(galay_http_headers_t* headers, const char* name)
{
    if (headers == nullptr || name == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string key = lower_copy(name);
    const auto old_size = headers->entries.size();
    headers->entries.erase(std::remove_if(headers->entries.begin(), headers->entries.end(),
                                          [&](const auto& entry) { return entry.name == key; }),
                           headers->entries.end());
    return headers->entries.size() == old_size ? GALAY_NOT_FOUND : GALAY_OK;
}

galay_status_t galay_http_request_create(galay_http_request_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_http_request_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_http_request_destroy(galay_http_request_t* request)
{
    delete request;
}

galay_status_t galay_http_request_set_method_path(galay_http_request_t* request,
                                                  galay_http_method_t method,
                                                  const char* path)
{
    if (request == nullptr || method_name(method) == nullptr || path == nullptr || path[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    request->method = method;
    request->path = path;
    request->complete = false;
    return GALAY_OK;
}

galay_status_t galay_http_request_method(const galay_http_request_t* request,
                                         galay_http_method_t* method)
{
    if (request == nullptr || method == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *method = request->method;
    return GALAY_OK;
}

galay_status_t galay_http_request_path(const galay_http_request_t* request,
                                       const char** path, size_t* path_len)
{
    if (path != nullptr) {
        *path = nullptr;
    }
    if (path_len != nullptr) {
        *path_len = 0;
    }
    if (request == nullptr || path == nullptr || path_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *path = request->path.data();
    *path_len = request->path.size();
    return GALAY_OK;
}

galay_status_t galay_http_request_add_header(galay_http_request_t* request, const char* name,
                                             const char* value)
{
    if (request == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return galay_http_headers_add(&request->headers, name, value);
}

galay_status_t galay_http_request_set_body(galay_http_request_t* request, const char* body,
                                           size_t body_len)
{
    if (request == nullptr || (body == nullptr && body_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    request->body.assign(body == nullptr ? "" : body, body_len);
    request->complete = false;
    return GALAY_OK;
}

galay_status_t galay_http_request_serialize(galay_http_request_t* request,
                                            const char** data, size_t* data_len)
{
    if (data != nullptr) {
        *data = nullptr;
    }
    if (data_len != nullptr) {
        *data_len = 0;
    }
    if (request == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t status = serialize_request(*request);
    if (status != GALAY_OK) {
        return status;
    }
    *data = request->serialized.data();
    *data_len = request->serialized.size();
    return GALAY_OK;
}

galay_status_t galay_http_request_parse(galay_http_request_t* request, const char* data,
                                        size_t data_len, size_t max_header_len,
                                        size_t max_body_len, size_t* consumed)
{
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (request == nullptr || data == nullptr || consumed == nullptr || max_header_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    const ParseResult parsed = parse_request_internal(
        *request, std::string_view(data, data_len), max_header_len, max_body_len);
    if (parsed.state != ParseState::kComplete) {
        return GALAY_PROTOCOL_ERROR;
    }
    *consumed = parsed.consumed;
    return GALAY_OK;
}

galay_bool_t galay_http_request_is_complete(const galay_http_request_t* request)
{
    return request != nullptr && request->complete ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_http_request_body(const galay_http_request_t* request, const char** body,
                                       size_t* body_len)
{
    if (body != nullptr) {
        *body = nullptr;
    }
    if (body_len != nullptr) {
        *body_len = 0;
    }
    if (body == nullptr || body_len == nullptr || request == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *body = request->body.data();
    *body_len = request->body.size();
    return GALAY_OK;
}

galay_status_t galay_http_request_find_header(const galay_http_request_t* request,
                                              const char* name, const char** value,
                                              size_t* value_len)
{
    return request == nullptr ? GALAY_INVALID_ARGUMENT :
        galay_http_headers_find(&request->headers, name, value, value_len);
}

galay_status_t galay_http_response_create(galay_http_response_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_http_response_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_http_response_destroy(galay_http_response_t* response)
{
    delete response;
}

galay_status_t galay_http_response_set_status(galay_http_response_t* response,
                                              galay_http_status_code_t status)
{
    if (response == nullptr || !valid_status(status)) {
        return GALAY_INVALID_ARGUMENT;
    }
    response->status = status;
    response->complete = false;
    return GALAY_OK;
}

galay_status_t galay_http_response_status(const galay_http_response_t* response,
                                          galay_http_status_code_t* status)
{
    if (response == nullptr || status == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *status = static_cast<galay_http_status_code_t>(response->status);
    return GALAY_OK;
}

galay_status_t galay_http_response_add_header(galay_http_response_t* response, const char* name,
                                              const char* value)
{
    if (response == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return galay_http_headers_add(&response->headers, name, value);
}

galay_status_t galay_http_response_set_body(galay_http_response_t* response, const char* body,
                                            size_t body_len)
{
    if (response == nullptr || (body == nullptr && body_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    response->body.assign(body == nullptr ? "" : body, body_len);
    response->complete = false;
    return GALAY_OK;
}

galay_status_t galay_http_response_body(const galay_http_response_t* response, const char** body,
                                        size_t* body_len)
{
    if (body != nullptr) {
        *body = nullptr;
    }
    if (body_len != nullptr) {
        *body_len = 0;
    }
    if (response == nullptr || body == nullptr || body_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *body = response->body.data();
    *body_len = response->body.size();
    return GALAY_OK;
}

galay_status_t galay_http_response_parse(galay_http_response_t* response, const char* data,
                                         size_t data_len, size_t max_header_len,
                                         size_t max_body_len, size_t* consumed)
{
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (response == nullptr || data == nullptr || consumed == nullptr || max_header_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    const ParseResult parsed = parse_response_internal(
        *response, std::string_view(data, data_len), max_header_len, max_body_len);
    if (parsed.state != ParseState::kComplete) {
        return GALAY_PROTOCOL_ERROR;
    }
    *consumed = parsed.consumed;
    return GALAY_OK;
}

galay_status_t galay_http_response_serialize(galay_http_response_t* response,
                                             const char** data, size_t* data_len)
{
    if (data != nullptr) {
        *data = nullptr;
    }
    if (data_len != nullptr) {
        *data_len = 0;
    }
    if (response == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t status = serialize_response(*response);
    if (status != GALAY_OK) {
        return status;
    }
    *data = response->serialized.data();
    *data_len = response->serialized.size();
    return GALAY_OK;
}

galay_status_t galay_http_client_create(galay_http_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_http_client_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_http_client_destroy(galay_http_client_t* client)
{
    if (client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t socket_status = destroy_socket(client->session.socket);
    delete client;
    return socket_status;
}

C_IOResult galay_http_client_connect(galay_http_client_t* client, const C_Host* endpoint,
                                     int64_t timeout_ms)
{
    if (client == nullptr || endpoint == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    const galay_status_t create_status = create_socket(client->session.socket, endpoint->type);
    if (create_status != GALAY_OK) {
        return io_from_status(create_status);
    }
    client->session.closed = false;
    C_IOResult result = galay_kernel_tcp_socket_connect(&client->session.socket, endpoint, timeout_ms);
    if (result.code != C_IOResultOk) {
        const galay_status_t destroy_status = destroy_socket(client->session.socket);
        if (destroy_status != GALAY_OK) {
            return make_io_result(C_IOResultError);
        }
    }
    return result;
}

C_IOResult galay_http_client_send_request(galay_http_client_t* client,
                                          const galay_http_request_t* request,
                                          int64_t timeout_ms)
{
    if (client == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    return galay_http_session_send_request(&client->session, request, timeout_ms);
}

C_IOResult galay_http_client_recv_response(galay_http_client_t* client,
                                           galay_http_response_t** out_response,
                                           size_t max_header_len,
                                           size_t max_body_len,
                                           int64_t timeout_ms)
{
    if (client == nullptr) {
        if (out_response != nullptr) {
            *out_response = nullptr;
        }
        return make_io_result(C_IOResultInvalid);
    }
    return galay_http_session_recv_response(&client->session, out_response,
                                            max_header_len, max_body_len, timeout_ms);
}

C_IOResult galay_http_client_close(galay_http_client_t* client, int64_t timeout_ms)
{
    return client == nullptr
        ? make_io_result(C_IOResultInvalid)
        : close_session_socket(&client->session, timeout_ms);
}

galay_status_t galay_http_server_create(galay_http_server_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_http_server_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_http_server_destroy(galay_http_server_t* server)
{
    if (server == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t socket_status = destroy_socket(server->listener);
    delete server;
    return socket_status;
}

galay_status_t galay_http_server_bind(galay_http_server_t* server, const C_Host* endpoint)
{
    if (server == nullptr || endpoint == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t create_status = create_socket(server->listener, endpoint->type);
    if (create_status != GALAY_OK) {
        return create_status;
    }
    const C_TcpSocketResultCode result = galay_kernel_tcp_socket_bind(&server->listener, endpoint);
    return result == C_TcpSocketSuccess ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_http_server_listen(galay_http_server_t* server, int backlog)
{
    if (server == nullptr || server->listener.socket == nullptr || backlog <= 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    const C_TcpSocketResultCode result = galay_kernel_tcp_socket_listen(&server->listener, backlog);
    if (result != C_TcpSocketSuccess) {
        return GALAY_IO_ERROR;
    }
    server->listening = true;
    return GALAY_OK;
}

galay_status_t galay_http_server_local_endpoint(const galay_http_server_t* server,
                                                C_Host* endpoint)
{
    if (server == nullptr || endpoint == nullptr || server->listener.socket == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const C_TcpSocketResultCode result =
        galay_kernel_tcp_socket_local_endpoint(&server->listener, endpoint);
    return result == C_TcpSocketSuccess ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_http_server_add_route(galay_http_server_t* server,
                                           galay_http_method_t method,
                                           const char* path,
                                           galay_http_route_callback_t callback,
                                           void* user_data)
{
    if (server == nullptr || method_name(method) == nullptr ||
        path == nullptr || path[0] == '\0' || callback == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    for (auto& route : server->routes) {
        if (route.method == method && route.path == path) {
            route.callback = callback;
            route.user_data = user_data;
            return GALAY_OK;
        }
    }
    server->routes.push_back(galay_http_route_entry{method, path, callback, user_data});
    return GALAY_OK;
}

C_IOResult galay_http_server_accept(galay_http_server_t* server,
                                    galay_http_session_t** out_session,
                                    C_Host* out_peer,
                                    int64_t timeout_ms)
{
    if (out_session != nullptr) {
        *out_session = nullptr;
    }
    if (server == nullptr || !server->listening ||
        server->listener.socket == nullptr || out_session == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    auto* session = new (std::nothrow) galay_http_session_t();
    if (session == nullptr) {
        return make_io_result(C_IOResultError);
    }
    C_IOResult result =
        galay_kernel_tcp_socket_accept(&server->listener, &session->socket, out_peer, timeout_ms);
    if (result.code != C_IOResultOk) {
        delete session;
        return result;
    }
    *out_session = session;
    result.ptr = session;
    return result;
}

C_IOResult galay_http_server_serve_one(galay_http_server_t* server, int64_t timeout_ms)
{
    galay_http_session_t* session = nullptr;
    C_IOResult result = galay_http_server_accept(server, &session, nullptr, timeout_ms);
    if (result.code != C_IOResultOk) {
        return result;
    }

    galay_http_request_t* request = nullptr;
    result = galay_http_session_recv_request(session, &request, 8192, 1024 * 1024, timeout_ms);
    if (result.code != C_IOResultOk) {
        const galay_status_t destroy_status = galay_http_session_destroy(session);
        return destroy_status == GALAY_OK ? result : make_io_result(C_IOResultError);
    }

    galay_http_response_t* response = nullptr;
    const galay_status_t create_response = galay_http_response_create(&response);
    if (create_response != GALAY_OK) {
        galay_http_request_destroy(request);
        const galay_status_t destroy_status = galay_http_session_destroy(session);
        return destroy_status == GALAY_OK ? io_from_status(create_response)
                                          : make_io_result(C_IOResultError);
    }

    galay_http_route_entry* matched = nullptr;
    for (auto& route : server->routes) {
        if (route.method == request->method && route.path == request->path) {
            matched = &route;
            break;
        }
    }

    galay_status_t callback_status = GALAY_OK;
    if (matched == nullptr) {
        callback_status = galay_http_response_set_status(response, GALAY_HTTP_STATUS_NOT_FOUND);
    } else {
        callback_status = matched->callback(request, response, matched->user_data);
    }

    if (callback_status != GALAY_OK) {
        const galay_status_t status_result =
            galay_http_response_set_status(response, GALAY_HTTP_STATUS_INTERNAL_SERVER_ERROR);
        if (status_result == GALAY_OK) {
            callback_status = galay_http_response_set_body(response, nullptr, 0);
        }
    }

    result = callback_status == GALAY_OK
        ? galay_http_session_send_response(session, response, timeout_ms)
        : io_from_status(callback_status);

    C_IOResult close_result = close_session_socket(session, timeout_ms);
    if (result.code == C_IOResultOk && close_result.code != C_IOResultOk) {
        result = close_result;
    }
    galay_http_response_destroy(response);
    galay_http_request_destroy(request);
    const galay_status_t destroy_status = galay_http_session_destroy(session);
    if (result.code == C_IOResultOk && destroy_status != GALAY_OK) {
        result = make_io_result(C_IOResultError);
    }
    return result;
}

C_IOResult galay_http_server_stop(galay_http_server_t* server, int64_t timeout_ms)
{
    if (server == nullptr || server->listener.socket == nullptr || !server->listening) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult result = galay_kernel_tcp_socket_close(&server->listener, timeout_ms);
    if (result.code == C_IOResultOk) {
        server->listening = false;
    }
    return result;
}

galay_status_t galay_http_session_destroy(galay_http_session_t* session)
{
    if (session == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t socket_status = destroy_socket(session->socket);
    delete session;
    return socket_status;
}

C_IOResult galay_http_session_send_request(galay_http_session_t* session,
                                           const galay_http_request_t* request,
                                           int64_t timeout_ms)
{
    if (request == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    auto* mutable_request = const_cast<galay_http_request_t*>(request);
    const galay_status_t status = serialize_request(*mutable_request);
    if (status != GALAY_OK) {
        return io_from_status(status);
    }
    return send_all(session, mutable_request->serialized.data(),
                    mutable_request->serialized.size(), timeout_ms);
}

C_IOResult galay_http_session_recv_request(galay_http_session_t* session,
                                           galay_http_request_t** out_request,
                                           size_t max_header_len,
                                           size_t max_body_len,
                                           int64_t timeout_ms)
{
    return recv_message(session, out_request, max_header_len, max_body_len, timeout_ms,
                        parse_request_internal);
}

C_IOResult galay_http_session_send_response(galay_http_session_t* session,
                                            const galay_http_response_t* response,
                                            int64_t timeout_ms)
{
    if (response == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    auto* mutable_response = const_cast<galay_http_response_t*>(response);
    const galay_status_t status = serialize_response(*mutable_response);
    if (status != GALAY_OK) {
        return io_from_status(status);
    }
    return send_all(session, mutable_response->serialized.data(),
                    mutable_response->serialized.size(), timeout_ms);
}

C_IOResult galay_http_session_recv_response(galay_http_session_t* session,
                                            galay_http_response_t** out_response,
                                            size_t max_header_len,
                                            size_t max_body_len,
                                            int64_t timeout_ms)
{
    return recv_message(session, out_response, max_header_len, max_body_len, timeout_ms,
                        parse_response_internal);
}

C_IOResult galay_http_session_send_bytes(galay_http_session_t* session, const char* data,
                                         size_t data_len, int64_t timeout_ms)
{
    return send_all(session, data, data_len, timeout_ms);
}

C_IOResult galay_http_session_recv_bytes(galay_http_session_t* session, char* data,
                                         size_t data_len, int64_t timeout_ms)
{
    if (session == nullptr || session->socket.socket == nullptr ||
        session->closed || data == nullptr || data_len == 0) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult result = galay_kernel_tcp_socket_recv(&session->socket, data, data_len, timeout_ms);
    if (result.code == C_IOResultOk && result.bytes == 0) {
        return make_io_result(C_IOResultEof);
    }
    return result;
}

C_IOResult galay_http_session_close(galay_http_session_t* session, int64_t timeout_ms)
{
    return close_session_socket(session, timeout_ms);
}

} // extern "C"
