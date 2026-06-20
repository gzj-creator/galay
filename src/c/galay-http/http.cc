#include "http.h"

#include "../../cpp/galay-http/protoc/http_header.h"
#include "../../cpp/galay-http/protoc/http_request.h"
#include "../../cpp/galay-http/protoc/http_response.h"

#include <new>
#include <string>
#include <string_view>
#include <vector>
#include <sys/uio.h>

using galay::http::HeaderPair;
using galay::http::HttpErrorCode;
using galay::http::HttpMethod;
using galay::http::HttpRequest;
using galay::http::HttpResponse;
using galay::http::HttpStatusCode;
using galay::http::HttpVersion;

struct galay_http_headers {
    HeaderPair headers;
};

struct galay_http_request {
    HttpRequest request;
    std::string serialized;
};

struct galay_http_response {
    HttpResponse response;
    std::string serialized;
};

namespace {

galay_status_t map_http_error(HttpErrorCode error)
{
    switch (error) {
    case galay::http::kNoError:
    case galay::http::kIncomplete:
        return GALAY_OK;
    case galay::http::kHeaderPairNotExist:
    case galay::http::kNotFound:
        return GALAY_NOT_FOUND;
    case galay::http::kTcpRecvError:
    case galay::http::kTcpSendError:
    case galay::http::kRecvError:
    case galay::http::kSendError:
    case galay::http::kCloseError:
    case galay::http::kTcpConnectError:
        return GALAY_IO_ERROR;
    case galay::http::kNotImplemented:
        return GALAY_UNSUPPORTED;
    case galay::http::kInternalError:
    case galay::http::kUnknownError:
        return GALAY_INTERNAL_ERROR;
    default:
        return GALAY_PROTOCOL_ERROR;
    }
}

bool is_valid_method(galay_http_method_t method)
{
    return method >= GALAY_HTTP_METHOD_GET && method <= GALAY_HTTP_METHOD_UNKNOWN;
}

bool is_valid_version(galay_http_version_t version)
{
    return version >= GALAY_HTTP_VERSION_1_0 && version <= GALAY_HTTP_VERSION_UNKNOWN;
}

HttpVersion to_cpp_version(galay_http_version_t version)
{
    return static_cast<HttpVersion>(version);
}

galay_http_version_t from_cpp_version(HttpVersion version)
{
    return static_cast<galay_http_version_t>(version);
}

galay_status_t set_string_body(std::string& body, const void* data, size_t data_len)
{
    if (data == nullptr && data_len != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    body.assign(static_cast<const char*>(data == nullptr ? "" : data), data_len);
    return GALAY_OK;
}

galay_status_t find_header(const HeaderPair& headers,
                           const char* key,
                           const char** value,
                           size_t* value_len)
{
    if (key == nullptr || value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = nullptr;
    *value_len = 0;
    const std::string* found = headers.getValuePtr(key);
    if (found == nullptr) {
        return GALAY_NOT_FOUND;
    }
    *value = found->data();
    *value_len = found->size();
    return GALAY_OK;
}

galay_status_t add_header(HeaderPair& headers, const char* key, const char* value)
{
    if (key == nullptr || key[0] == '\0' || value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return map_http_error(headers.addHeaderPair(key, value));
}

galay_status_t remove_header(HeaderPair& headers, const char* key)
{
    if (key == nullptr || key[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    return map_http_error(headers.removeHeaderPair(key));
}

galay_status_t check_header_size(std::string_view data, size_t max_header_size)
{
    if (max_header_size == 0) {
        return GALAY_OK;
    }
    const std::string_view marker = "\r\n\r\n";
    const size_t header_end = data.find(marker);
    if (header_end == std::string_view::npos) {
        return data.size() > max_header_size ? GALAY_PROTOCOL_ERROR : GALAY_OK;
    }
    return header_end + marker.size() > max_header_size ? GALAY_PROTOCOL_ERROR : GALAY_OK;
}

galay_status_t parse_into_request(HttpRequest& request,
                                  const void* data,
                                  size_t data_len,
                                  size_t max_header_size,
                                  size_t max_body_size,
                                  size_t* consumed)
{
    if ((data == nullptr && data_len != 0) || consumed == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *consumed = 0;
    std::string_view view(static_cast<const char*>(data == nullptr ? "" : data), data_len);
    galay_status_t header_status = check_header_size(view, max_header_size);
    if (header_status != GALAY_OK) {
        return header_status;
    }
    iovec iov;
    iov.iov_base = const_cast<char*>(view.data());
    iov.iov_len = view.size();
    std::vector<iovec> iovecs;
    iovecs.push_back(iov);
    auto result = request.fromIOVec(iovecs, max_body_size);
    if (result.second < 0) {
        return map_http_error(result.first);
    }
    *consumed = static_cast<size_t>(result.second);
    return map_http_error(result.first);
}

galay_status_t parse_into_response(HttpResponse& response,
                                   const void* data,
                                   size_t data_len,
                                   size_t max_header_size,
                                   size_t max_body_size,
                                   size_t* consumed)
{
    if ((data == nullptr && data_len != 0) || consumed == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *consumed = 0;
    std::string_view view(static_cast<const char*>(data == nullptr ? "" : data), data_len);
    galay_status_t header_status = check_header_size(view, max_header_size);
    if (header_status != GALAY_OK) {
        return header_status;
    }
    iovec iov;
    iov.iov_base = const_cast<char*>(view.data());
    iov.iov_len = view.size();
    std::vector<iovec> iovecs;
    iovecs.push_back(iov);
    auto result = response.fromIOVec(iovecs, max_body_size);
    if (result.second < 0) {
        return map_http_error(result.first);
    }
    *consumed = static_cast<size_t>(result.second);
    return map_http_error(result.first);
}

} // namespace

extern "C" {

galay_status_t galay_http_headers_create(galay_http_headers_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        *out = new (std::nothrow) galay_http_headers();
        return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_http_headers_destroy(galay_http_headers_t* headers)
{
    delete headers;
}

galay_status_t galay_http_headers_add(galay_http_headers_t* headers,
                                      const char* key,
                                      const char* value)
{
    if (headers == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        return add_header(headers->headers, key, value);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_headers_find(const galay_http_headers_t* headers,
                                       const char* key,
                                       const char** value,
                                       size_t* value_len)
{
    if (headers == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        return find_header(headers->headers, key, value, value_len);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_headers_remove(galay_http_headers_t* headers, const char* key)
{
    if (headers == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        return remove_header(headers->headers, key);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_request_create(galay_http_request_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        *out = new (std::nothrow) galay_http_request();
        return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_http_request_destroy(galay_http_request_t* request)
{
    delete request;
}

void galay_http_request_reset(galay_http_request_t* request)
{
    if (request != nullptr) {
        request->request.reset();
        request->serialized.clear();
    }
}

galay_status_t galay_http_request_set_method(galay_http_request_t* request,
                                             galay_http_method_t method)
{
    if (request == nullptr || !is_valid_method(method)) {
        return GALAY_INVALID_ARGUMENT;
    }
    request->request.header().method() = static_cast<HttpMethod>(method);
    return GALAY_OK;
}

galay_status_t galay_http_request_method(const galay_http_request_t* request,
                                         galay_http_method_t* method)
{
    if (request == nullptr || method == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    HttpRequest& mutable_request = const_cast<HttpRequest&>(request->request);
    *method = static_cast<galay_http_method_t>(mutable_request.header().method());
    return GALAY_OK;
}

galay_status_t galay_http_request_set_uri(galay_http_request_t* request, const char* uri)
{
    if (request == nullptr || uri == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        request->request.header().uri() = uri;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_request_set_version(galay_http_request_t* request,
                                              galay_http_version_t version)
{
    if (request == nullptr || !is_valid_version(version)) {
        return GALAY_INVALID_ARGUMENT;
    }
    request->request.header().version() = to_cpp_version(version);
    return GALAY_OK;
}

galay_status_t galay_http_request_add_header(galay_http_request_t* request,
                                             const char* key,
                                             const char* value)
{
    if (request == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        return add_header(request->request.header().headerPairs(), key, value);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_request_find_header(const galay_http_request_t* request,
                                              const char* key,
                                              const char** value,
                                              size_t* value_len)
{
    if (request == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        HttpRequest& mutable_request = const_cast<HttpRequest&>(request->request);
        return find_header(mutable_request.header().headerPairs(), key, value, value_len);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_request_remove_header(galay_http_request_t* request, const char* key)
{
    if (request == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        return remove_header(request->request.header().headerPairs(), key);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_request_set_body(galay_http_request_t* request,
                                           const void* data,
                                           size_t data_len)
{
    if (request == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        std::string body;
        galay_status_t status = set_string_body(body, data, data_len);
        if (status != GALAY_OK) {
            return status;
        }
        request->request.setBodyStr(std::move(body));
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_request_body(const galay_http_request_t* request,
                                       const char** data,
                                       size_t* data_len)
{
    if (request == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string& body = request->request.bodyStr();
    *data = body.data();
    *data_len = body.size();
    return GALAY_OK;
}

galay_status_t galay_http_request_parse(galay_http_request_t* request,
                                        const void* data,
                                        size_t data_len,
                                        size_t max_header_size,
                                        size_t max_body_size,
                                        size_t* consumed)
{
    if (request == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        return parse_into_request(request->request, data, data_len,
                                  max_header_size, max_body_size, consumed);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_bool_t galay_http_request_is_complete(const galay_http_request_t* request)
{
    if (request == nullptr) {
        return GALAY_FALSE;
    }
    return request->request.isComplete() ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_http_request_serialize(galay_http_request_t* request,
                                            const char** data,
                                            size_t* data_len)
{
    if (request == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        request->serialized = request->request.toString();
        *data = request->serialized.data();
        *data_len = request->serialized.size();
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_response_create(galay_http_response_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        *out = new (std::nothrow) galay_http_response();
        return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_http_response_destroy(galay_http_response_t* response)
{
    delete response;
}

void galay_http_response_reset(galay_http_response_t* response)
{
    if (response != nullptr) {
        response->response.reset();
        response->serialized.clear();
    }
}

galay_status_t galay_http_response_set_status(galay_http_response_t* response,
                                              galay_http_status_code_t status)
{
    if (response == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    response->response.header().code() = static_cast<HttpStatusCode>(status);
    return GALAY_OK;
}

galay_status_t galay_http_response_status(const galay_http_response_t* response,
                                          galay_http_status_code_t* status)
{
    if (response == nullptr || status == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    HttpResponse& mutable_response = const_cast<HttpResponse&>(response->response);
    *status = static_cast<galay_http_status_code_t>(mutable_response.header().code());
    return GALAY_OK;
}

galay_status_t galay_http_response_set_version(galay_http_response_t* response,
                                               galay_http_version_t version)
{
    if (response == nullptr || !is_valid_version(version)) {
        return GALAY_INVALID_ARGUMENT;
    }
    response->response.header().version() = to_cpp_version(version);
    return GALAY_OK;
}

galay_status_t galay_http_response_add_header(galay_http_response_t* response,
                                              const char* key,
                                              const char* value)
{
    if (response == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        return add_header(response->response.header().headerPairs(), key, value);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_response_find_header(const galay_http_response_t* response,
                                               const char* key,
                                               const char** value,
                                               size_t* value_len)
{
    if (response == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        HttpResponse& mutable_response = const_cast<HttpResponse&>(response->response);
        return find_header(mutable_response.header().headerPairs(), key, value, value_len);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_response_remove_header(galay_http_response_t* response, const char* key)
{
    if (response == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        return remove_header(response->response.header().headerPairs(), key);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_response_set_body(galay_http_response_t* response,
                                            const void* data,
                                            size_t data_len)
{
    if (response == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        std::string body;
        galay_status_t status = set_string_body(body, data, data_len);
        if (status != GALAY_OK) {
            return status;
        }
        response->response.setBodyStr(std::move(body));
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http_response_body(const galay_http_response_t* response,
                                        const char** data,
                                        size_t* data_len)
{
    if (response == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string& body = response->response.bodyStr();
    *data = body.data();
    *data_len = body.size();
    return GALAY_OK;
}

galay_status_t galay_http_response_parse(galay_http_response_t* response,
                                         const void* data,
                                         size_t data_len,
                                         size_t max_header_size,
                                         size_t max_body_size,
                                         size_t* consumed)
{
    if (response == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        return parse_into_response(response->response, data, data_len,
                                   max_header_size, max_body_size, consumed);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_bool_t galay_http_response_is_complete(const galay_http_response_t* response)
{
    if (response == nullptr) {
        return GALAY_FALSE;
    }
    return response->response.isComplete() ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_http_response_serialize(galay_http_response_t* response,
                                             const char** data,
                                             size_t* data_len)
{
    if (response == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        response->serialized = response->response.toString();
        *data = response->serialized.data();
        *data_len = response->serialized.size();
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

} // extern "C"
