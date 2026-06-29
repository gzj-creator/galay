#include <galay/c/galay-http-c/http.h>

#include <algorithm>
#include <cctype>
#include <new>
#include <string>
#include <vector>

struct galay_http_header_item {
    std::string name;
    std::string value;
};

struct galay_http_headers_t {
    std::vector<galay_http_header_item> entries;
};

struct galay_http_request_t {
    galay_http_headers_t headers;
    std::string body;
    bool complete = false;
};

struct galay_http_response_t {
    int status = GALAY_HTTP_STATUS_OK;
    std::string body;
    std::string serialized;
};

static std::string lower_copy(const char* text)
{
    std::string out(text == nullptr ? "" : text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

static const char* reason_phrase(int status)
{
    return status == GALAY_HTTP_STATUS_NO_CONTENT ? "No Content" : "OK";
}

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
    const std::string key = lower_copy(name);
    for (auto& entry : headers->entries) {
        if (entry.name == key) {
            entry.value += ", ";
            entry.value += value;
            return GALAY_OK;
        }
    }
    headers->entries.push_back({key, value});
    return GALAY_OK;
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

galay_status_t galay_http_request_parse(galay_http_request_t* request, const char* data,
                                        size_t data_len, size_t max_header_len,
                                        size_t max_body_len, size_t* consumed)
{
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (request == nullptr || data == nullptr || consumed == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string raw(data, data_len);
    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos || header_end + 4 > max_header_len) {
        return GALAY_PROTOCOL_ERROR;
    }
    const size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) {
        return GALAY_PROTOCOL_ERROR;
    }
    const std::string first = raw.substr(0, line_end);
    if (first.find(" HTTP/1.1") == std::string::npos && first.find(" HTTP/1.0") == std::string::npos) {
        return GALAY_PROTOCOL_ERROR;
    }
    request->headers.entries.clear();
    size_t pos = line_end + 2;
    size_t content_length = 0;
    while (pos < header_end) {
        const size_t next = raw.find("\r\n", pos);
        if (next == std::string::npos || next > header_end) {
            return GALAY_PROTOCOL_ERROR;
        }
        const std::string line = raw.substr(pos, next - pos);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return GALAY_PROTOCOL_ERROR;
        }
        size_t value_pos = colon + 1;
        while (value_pos < line.size() && line[value_pos] == ' ') {
            ++value_pos;
        }
        const std::string name = line.substr(0, colon);
        const std::string value = line.substr(value_pos);
        const galay_status_t add_status = galay_http_headers_add(&request->headers, name.c_str(), value.c_str());
        if (add_status != GALAY_OK) {
            return add_status;
        }
        if (lower_copy(name.c_str()) == "content-length") {
            content_length = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
        }
        pos = next + 2;
    }
    if (content_length > max_body_len || header_end + 4 + content_length > data_len) {
        return GALAY_PROTOCOL_ERROR;
    }
    request->body.assign(data + header_end + 4, content_length);
    request->complete = true;
    *consumed = header_end + 4 + content_length;
    return GALAY_OK;
}

galay_bool_t galay_http_request_is_complete(const galay_http_request_t* request)
{
    return request != nullptr && request->complete ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_http_request_body(const galay_http_request_t* request, const char** body,
                                       size_t* body_len)
{
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
    if (response == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    response->status = status;
    return GALAY_OK;
}

galay_status_t galay_http_response_set_body(galay_http_response_t* response, const char* body,
                                            size_t body_len)
{
    if (response == nullptr || (body == nullptr && body_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    response->body.assign(body == nullptr ? "" : body, body_len);
    return GALAY_OK;
}

galay_status_t galay_http_response_serialize(galay_http_response_t* response,
                                             const char** data, size_t* data_len)
{
    if (response == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    response->serialized = "HTTP/1.1 " + std::to_string(response->status) + " " +
        reason_phrase(response->status) + "\r\ncontent-length: " +
        std::to_string(response->body.size()) + "\r\n\r\n" + response->body;
    *data = response->serialized.data();
    *data_len = response->serialized.size();
    return GALAY_OK;
}

}
