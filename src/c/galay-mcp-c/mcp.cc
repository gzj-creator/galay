#include <galay/c/galay-mcp-c/mcp.h>

#include <cstdlib>
#include <new>
#include <string>

struct galay_mcp_message_t { std::string data; };
struct galay_mcp_parsed_request_t {
    bool notification = false;
    int64_t id = 0;
    std::string method;
    std::string params;
};
struct galay_mcp_parsed_response_t {
    int64_t id = 0;
    std::string result;
};
struct galay_mcp_client_config_t {
    galay_mcp_mode_t mode = GALAY_MCP_MODE_STDIO;
    std::string url;
};

static bool valid_method(const char* method)
{
    if (method == nullptr || method[0] == '\0') return false;
    for (const char* p = method; *p != '\0'; ++p) {
        if (*p == ' ') return false;
    }
    return true;
}

static bool find_string_field(const std::string& data, const char* key, std::string* out)
{
    const std::string marker = std::string("\"") + key + "\":\"";
    const size_t start = data.find(marker);
    if (start == std::string::npos) return false;
    const size_t value_start = start + marker.size();
    const size_t end = data.find('"', value_start);
    if (end == std::string::npos) return false;
    *out = data.substr(value_start, end - value_start);
    return true;
}

static bool find_int_field(const std::string& data, const char* key, int64_t* out)
{
    const std::string marker = std::string("\"") + key + "\":";
    const size_t start = data.find(marker);
    if (start == std::string::npos) return false;
    char* end = nullptr;
    const long long value = std::strtoll(data.c_str() + start + marker.size(), &end, 10);
    if (end == data.c_str() + start + marker.size()) return false;
    *out = value;
    return true;
}

static bool find_json_field(const std::string& data, const char* key, std::string* out)
{
    const std::string marker = std::string("\"") + key + "\":";
    const size_t start = data.find(marker);
    if (start == std::string::npos) return false;
    const size_t value_start = start + marker.size();
    const char open = data[value_start];
    const char close = open == '{' ? '}' : ']';
    const size_t end = data.find(close, value_start);
    if (end == std::string::npos) return false;
    *out = data.substr(value_start, end - value_start + 1);
    return true;
}

extern "C" {

const char* galay_mcp_get_error(galay_status_t status) { return galay_status_string(status); }

galay_status_t galay_mcp_message_create(galay_mcp_message_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_mcp_message_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mcp_message_destroy(galay_mcp_message_t* message) { delete message; }

galay_status_t galay_mcp_message_data(const galay_mcp_message_t* message, const char** data, size_t* data_len)
{
    if (message == nullptr || data == nullptr || data_len == nullptr) return GALAY_INVALID_ARGUMENT;
    *data = message->data.data();
    *data_len = message->data.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_build_request(galay_mcp_message_t* message, int64_t id, const char* method, const char* params)
{
    if (message == nullptr || !valid_method(method)) return GALAY_INVALID_ARGUMENT;
    message->data = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":\"" +
        method + "\"";
    if (params != nullptr) message->data += ",\"params\":" + std::string(params);
    message->data += "}";
    return GALAY_OK;
}

galay_status_t galay_mcp_build_notification(galay_mcp_message_t* message, const char* method, const char* params)
{
    if (message == nullptr || !valid_method(method)) return GALAY_INVALID_ARGUMENT;
    message->data = "{\"jsonrpc\":\"2.0\",\"method\":\"" + std::string(method) + "\"";
    if (params != nullptr) message->data += ",\"params\":" + std::string(params);
    message->data += "}";
    return GALAY_OK;
}

galay_status_t galay_mcp_build_initialized_notification(galay_mcp_message_t* message)
{
    return galay_mcp_build_notification(message, "notifications/initialized", nullptr);
}

galay_status_t galay_mcp_build_empty_result_response(galay_mcp_message_t* message, int64_t id)
{
    if (message == nullptr) return GALAY_INVALID_ARGUMENT;
    message->data = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":{}}";
    return GALAY_OK;
}

galay_status_t galay_mcp_parse_request(const char* data, size_t data_len, galay_mcp_parsed_request_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (data == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    std::string text(data, data_len);
    if (text.find("\"jsonrpc\":\"2.0\"") == std::string::npos) return GALAY_PROTOCOL_ERROR;
    auto* parsed = new (std::nothrow) galay_mcp_parsed_request_t();
    if (parsed == nullptr) return GALAY_OUT_OF_MEMORY;
    if (!find_string_field(text, "method", &parsed->method)) {
        delete parsed;
        return GALAY_PROTOCOL_ERROR;
    }
    parsed->notification = !find_int_field(text, "id", &parsed->id);
    std::string params;
    if (find_json_field(text, "params", &params)) parsed->params = params;
    *out = parsed;
    return GALAY_OK;
}

void galay_mcp_parsed_request_destroy(galay_mcp_parsed_request_t* request) { delete request; }
galay_bool_t galay_mcp_request_is_notification(const galay_mcp_parsed_request_t* request) { return request != nullptr && request->notification ? GALAY_TRUE : GALAY_FALSE; }

galay_status_t galay_mcp_request_id(const galay_mcp_parsed_request_t* request, int64_t* id)
{
    if (request == nullptr || id == nullptr || request->notification) return GALAY_PROTOCOL_ERROR;
    *id = request->id;
    return GALAY_OK;
}

galay_status_t galay_mcp_request_method(const galay_mcp_parsed_request_t* request, const char** method, size_t* method_len)
{
    if (request == nullptr || method == nullptr || method_len == nullptr) return GALAY_INVALID_ARGUMENT;
    *method = request->method.data();
    *method_len = request->method.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_request_params(const galay_mcp_parsed_request_t* request, const char** params, size_t* params_len)
{
    if (request == nullptr || params == nullptr || params_len == nullptr) return GALAY_INVALID_ARGUMENT;
    *params = request->params.data();
    *params_len = request->params.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_parse_response(const char* data, size_t data_len, galay_mcp_parsed_response_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (data == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    std::string text(data, data_len);
    if (text.find("\"jsonrpc\":\"2.0\"") == std::string::npos || text.find("\"method\"") != std::string::npos) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* parsed = new (std::nothrow) galay_mcp_parsed_response_t();
    if (parsed == nullptr) return GALAY_OUT_OF_MEMORY;
    if (!find_int_field(text, "id", &parsed->id) || !find_json_field(text, "result", &parsed->result)) {
        delete parsed;
        return GALAY_PROTOCOL_ERROR;
    }
    *out = parsed;
    return GALAY_OK;
}

void galay_mcp_parsed_response_destroy(galay_mcp_parsed_response_t* response) { delete response; }

galay_status_t galay_mcp_response_id(const galay_mcp_parsed_response_t* response, int64_t* id)
{
    if (response == nullptr || id == nullptr) return GALAY_INVALID_ARGUMENT;
    *id = response->id;
    return GALAY_OK;
}

galay_bool_t galay_mcp_response_has_result(const galay_mcp_parsed_response_t* response)
{
    return response != nullptr && !response->result.empty() ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_mcp_response_result(const galay_mcp_parsed_response_t* response, const char** result, size_t* result_len)
{
    if (response == nullptr || result == nullptr || result_len == nullptr) return GALAY_INVALID_ARGUMENT;
    *result = response->result.data();
    *result_len = response->result.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_stdio_config_create(const char*, const char*, galay_mcp_client_config_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    auto* config = new (std::nothrow) galay_mcp_client_config_t();
    if (config == nullptr) return GALAY_OUT_OF_MEMORY;
    config->mode = GALAY_MCP_MODE_STDIO;
    *out = config;
    return GALAY_OK;
}

galay_status_t galay_mcp_http_config_create(const char* url, galay_mcp_client_config_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (url == nullptr || url[0] == '\0' || out == nullptr) return GALAY_INVALID_ARGUMENT;
    auto* config = new (std::nothrow) galay_mcp_client_config_t();
    if (config == nullptr) return GALAY_OUT_OF_MEMORY;
    config->mode = GALAY_MCP_MODE_HTTP;
    config->url = url;
    *out = config;
    return GALAY_OK;
}

void galay_mcp_client_config_destroy(galay_mcp_client_config_t* config) { delete config; }
galay_mcp_mode_t galay_mcp_client_config_mode(const galay_mcp_client_config_t* config) { return config == nullptr ? GALAY_MCP_MODE_STDIO : config->mode; }

galay_status_t galay_mcp_http_config_url(const galay_mcp_client_config_t* config, const char** url, size_t* url_len)
{
    if (config == nullptr || url == nullptr || url_len == nullptr || config->mode != GALAY_MCP_MODE_HTTP) return GALAY_INVALID_ARGUMENT;
    *url = config->url.data();
    *url_len = config->url.size();
    return GALAY_OK;
}

}
