#include <galay/c/galay-mcp/mcp.h>

#include <galay/cpp/galay-mcp/common/json_parser.h>
#include <galay/cpp/galay-mcp/common/mcp_base.h>

#include <cctype>
#include <new>
#include <optional>
#include <string>
#include <string_view>

using galay::mcp::JsonDocument;
using galay::mcp::JsonHelper;
using galay::mcp::JsonRpcError;
using galay::mcp::JsonRpcRequest;
using galay::mcp::JsonRpcResponse;
using galay::mcp::JsonString;
using galay::mcp::JsonWriter;
using galay::mcp::JSONRPC_VERSION;
using galay::mcp::MCP_VERSION;
using galay::mcp::ParsedJsonRpcRequest;
using galay::mcp::ParsedJsonRpcResponse;
using galay::mcp::parseJsonRpcRequest;
using galay::mcp::parseJsonRpcResponse;

struct galay_mcp_message {
    std::string data;
};

struct galay_mcp_parsed_request {
    ParsedJsonRpcRequest parsed;
    std::string params;
};

struct galay_mcp_parsed_response {
    ParsedJsonRpcResponse parsed;
    std::string result;
    std::string error;
};

struct galay_mcp_client_config {
    galay_mcp_client_mode_t mode = GALAY_MCP_MODE_INVALID;
    FILE* input = nullptr;
    FILE* output = nullptr;
    std::string url;
};

namespace {

bool is_valid_method(std::string_view method)
{
    if (method.empty()) {
        return false;
    }
    for (unsigned char ch : method) {
        if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '/' || ch == '.') {
            continue;
        }
        return false;
    }
    return true;
}

galay_status_t validate_optional_json(const char* json, std::optional<JsonString>& out)
{
    out.reset();
    if (json == nullptr) {
        return GALAY_OK;
    }
    std::string_view view(json);
    if (view.empty()) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto parsed = JsonDocument::Parse(view);
    if (!parsed) {
        return GALAY_INVALID_ARGUMENT;
    }
    out.emplace(view);
    return GALAY_OK;
}

galay_status_t validate_required_json(const char* json, JsonString& out)
{
    if (json == nullptr || json[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    std::string_view view(json);
    auto parsed = JsonDocument::Parse(view);
    if (!parsed) {
        return GALAY_INVALID_ARGUMENT;
    }
    out.assign(view);
    return GALAY_OK;
}

galay_status_t set_span(const std::string& value, const char** data, size_t* data_len)
{
    if (data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *data = value.data();
    *data_len = value.size();
    return GALAY_OK;
}

galay_status_t build_jsonrpc_request(galay_mcp_message* message,
                                     std::optional<int64_t> id,
                                     const char* method,
                                     const char* params_json)
{
    if (message == nullptr || method == nullptr || !is_valid_method(method)) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::optional<JsonString> params;
    galay_status_t status = validate_optional_json(params_json, params);
    if (status != GALAY_OK) {
        return status;
    }

    JsonRpcRequest request;
    request.id = id;
    request.method = method;
    request.params = std::move(params);
    message->data = request.toJson();
    return GALAY_OK;
}

void write_initialize_params(JsonWriter& writer,
                             const char* client_name,
                             const char* client_version,
                             const char* capabilities_json)
{
    writer.StartObject();
    writer.Key("protocolVersion");
    writer.String(MCP_VERSION);
    writer.Key("clientInfo");
    writer.StartObject();
    writer.Key("name");
    writer.String(client_name);
    writer.Key("version");
    writer.String(client_version);
    writer.EndObject();
    writer.Key("capabilities");
    if (capabilities_json == nullptr) {
        writer.StartObject();
        writer.EndObject();
    } else {
        writer.Raw(capabilities_json);
    }
    writer.EndObject();
}

galay_status_t fill_request_raw_fields(galay_mcp_parsed_request& request)
{
    if (request.parsed.request.hasParams) {
        if (!JsonHelper::GetRawJson(request.parsed.request.params, request.params)) {
            return GALAY_PROTOCOL_ERROR;
        }
    }
    return GALAY_OK;
}

template <typename ParsedMessage>
galay_status_t require_jsonrpc_version(const ParsedMessage& parsed)
{
    galay::mcp::JsonObject obj;
    std::string version;
    if (!JsonHelper::GetObject(parsed.document.Root(), obj) ||
        !JsonHelper::GetString(obj, "jsonrpc", version) ||
        version != JSONRPC_VERSION) {
        return GALAY_PROTOCOL_ERROR;
    }
    return GALAY_OK;
}

galay_status_t fill_response_raw_fields(galay_mcp_parsed_response& response)
{
    if (response.parsed.response.hasResult) {
        if (!JsonHelper::GetRawJson(response.parsed.response.result, response.result)) {
            return GALAY_PROTOCOL_ERROR;
        }
    }
    if (response.parsed.response.hasError) {
        if (!JsonHelper::GetRawJson(response.parsed.response.error, response.error)) {
            return GALAY_PROTOCOL_ERROR;
        }
    }
    return GALAY_OK;
}

} // namespace

extern "C" {

galay_status_t galay_mcp_message_create(galay_mcp_message_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        *out = new (std::nothrow) galay_mcp_message();
        return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_mcp_message_destroy(galay_mcp_message_t* message)
{
    delete message;
}

void galay_mcp_message_reset(galay_mcp_message_t* message)
{
    if (message != nullptr) {
        message->data.clear();
    }
}

galay_status_t galay_mcp_message_data(const galay_mcp_message_t* message,
                                      const char** data,
                                      size_t* data_len)
{
    if (message == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return set_span(message->data, data, data_len);
}

galay_status_t galay_mcp_build_request(galay_mcp_message_t* message,
                                       int64_t id,
                                       const char* method,
                                       const char* params_json)
{
    try {
        return build_jsonrpc_request(message, id, method, params_json);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mcp_build_notification(galay_mcp_message_t* message,
                                            const char* method,
                                            const char* params_json)
{
    try {
        return build_jsonrpc_request(message, std::nullopt, method, params_json);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mcp_build_response(galay_mcp_message_t* message,
                                        int64_t id,
                                        const char* result_json)
{
    if (message == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        JsonString result;
        galay_status_t status = validate_required_json(result_json, result);
        if (status != GALAY_OK) {
            return status;
        }
        JsonRpcResponse response;
        response.id = id;
        response.result = std::move(result);
        message->data = response.toJson();
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mcp_build_error_response(galay_mcp_message_t* message,
                                              int64_t id,
                                              int error_code,
                                              const char* error_message,
                                              const char* data_json)
{
    if (message == nullptr || error_message == nullptr || error_message[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        std::optional<JsonString> data;
        galay_status_t status = validate_optional_json(data_json, data);
        if (status != GALAY_OK) {
            return status;
        }

        JsonRpcError error;
        error.code = error_code;
        error.message = error_message;
        error.data = std::move(data);

        JsonRpcResponse response;
        response.id = id;
        response.error = error.toJson();
        message->data = response.toJson();
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mcp_build_initialize_request(galay_mcp_message_t* message,
                                                  int64_t id,
                                                  const char* client_name,
                                                  const char* client_version,
                                                  const char* capabilities_json)
{
    if (message == nullptr || client_name == nullptr || client_name[0] == '\0' ||
        client_version == nullptr || client_version[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        std::optional<JsonString> capabilities;
        galay_status_t status = validate_optional_json(capabilities_json, capabilities);
        if (status != GALAY_OK) {
            return status;
        }
        JsonWriter writer;
        write_initialize_params(writer, client_name, client_version,
                                capabilities ? capabilities->c_str() : nullptr);
        std::string params = writer.TakeString();
        return build_jsonrpc_request(message, id, galay::mcp::Methods::INITIALIZE, params.c_str());
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mcp_build_ping_request(galay_mcp_message_t* message, int64_t id)
{
    try {
        return build_jsonrpc_request(message, id, galay::mcp::Methods::PING, nullptr);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mcp_build_initialized_notification(galay_mcp_message_t* message)
{
    try {
        return build_jsonrpc_request(message, std::nullopt, galay::mcp::Methods::INITIALIZED, nullptr);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mcp_build_empty_result_response(galay_mcp_message_t* message, int64_t id)
{
    return galay_mcp_build_response(message, id, "{}");
}

galay_status_t galay_mcp_parse_request(const void* data,
                                       size_t data_len,
                                       galay_mcp_parsed_request_t** out)
{
    if ((data == nullptr && data_len != 0) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        std::string_view body(static_cast<const char*>(data == nullptr ? "" : data), data_len);
        auto parsed = parseJsonRpcRequest(body);
        if (!parsed || !is_valid_method(parsed->request.method)) {
            return GALAY_PROTOCOL_ERROR;
        }
        galay_status_t version_status = require_jsonrpc_version(parsed.value());
        if (version_status != GALAY_OK) {
            return version_status;
        }
        auto* request = new (std::nothrow) galay_mcp_parsed_request();
        if (request == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        request->parsed = std::move(parsed.value());
        galay_status_t status = fill_request_raw_fields(*request);
        if (status != GALAY_OK) {
            delete request;
            return status;
        }
        *out = request;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_mcp_parsed_request_destroy(galay_mcp_parsed_request_t* request)
{
    delete request;
}

galay_bool_t galay_mcp_request_is_notification(const galay_mcp_parsed_request_t* request)
{
    if (request == nullptr) {
        return GALAY_FALSE;
    }
    return request->parsed.request.id.has_value() ? GALAY_FALSE : GALAY_TRUE;
}

galay_status_t galay_mcp_request_id(const galay_mcp_parsed_request_t* request, int64_t* id)
{
    if (request == nullptr || id == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (!request->parsed.request.id.has_value()) {
        return GALAY_PROTOCOL_ERROR;
    }
    *id = request->parsed.request.id.value();
    return GALAY_OK;
}

galay_status_t galay_mcp_request_method(const galay_mcp_parsed_request_t* request,
                                        const char** method,
                                        size_t* method_len)
{
    if (request == nullptr || method == nullptr || method_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *method = request->parsed.request.method.data();
    *method_len = request->parsed.request.method.size();
    return GALAY_OK;
}

galay_status_t galay_mcp_request_params(const galay_mcp_parsed_request_t* request,
                                        const char** params,
                                        size_t* params_len)
{
    if (request == nullptr || params == nullptr || params_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (!request->parsed.request.hasParams) {
        *params = nullptr;
        *params_len = 0;
        return GALAY_NOT_FOUND;
    }
    return set_span(request->params, params, params_len);
}

galay_status_t galay_mcp_parse_response(const void* data,
                                        size_t data_len,
                                        galay_mcp_parsed_response_t** out)
{
    if ((data == nullptr && data_len != 0) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        std::string_view body(static_cast<const char*>(data == nullptr ? "" : data), data_len);
        auto parsed = parseJsonRpcResponse(body);
        if (!parsed || parsed->response.hasResult == parsed->response.hasError) {
            return GALAY_PROTOCOL_ERROR;
        }
        galay_status_t version_status = require_jsonrpc_version(parsed.value());
        if (version_status != GALAY_OK) {
            return version_status;
        }
        auto* response = new (std::nothrow) galay_mcp_parsed_response();
        if (response == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        response->parsed = std::move(parsed.value());
        galay_status_t status = fill_response_raw_fields(*response);
        if (status != GALAY_OK) {
            delete response;
            return status;
        }
        *out = response;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
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
    *id = response->parsed.response.id;
    return GALAY_OK;
}

galay_bool_t galay_mcp_response_has_result(const galay_mcp_parsed_response_t* response)
{
    if (response == nullptr) {
        return GALAY_FALSE;
    }
    return response->parsed.response.hasResult ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_mcp_response_result(const galay_mcp_parsed_response_t* response,
                                         const char** result,
                                         size_t* result_len)
{
    if (response == nullptr || result == nullptr || result_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (!response->parsed.response.hasResult) {
        *result = nullptr;
        *result_len = 0;
        return GALAY_NOT_FOUND;
    }
    return set_span(response->result, result, result_len);
}

galay_bool_t galay_mcp_response_has_error(const galay_mcp_parsed_response_t* response)
{
    if (response == nullptr) {
        return GALAY_FALSE;
    }
    return response->parsed.response.hasError ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_mcp_response_error(const galay_mcp_parsed_response_t* response,
                                        const char** error,
                                        size_t* error_len)
{
    if (response == nullptr || error == nullptr || error_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (!response->parsed.response.hasError) {
        *error = nullptr;
        *error_len = 0;
        return GALAY_NOT_FOUND;
    }
    return set_span(response->error, error, error_len);
}

galay_status_t galay_mcp_stdio_config_create(FILE* input,
                                             FILE* output,
                                             galay_mcp_client_config_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        auto* config = new (std::nothrow) galay_mcp_client_config();
        if (config == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        config->mode = GALAY_MCP_MODE_STDIO;
        config->input = input;
        config->output = output;
        *out = config;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mcp_http_config_create(const char* url, galay_mcp_client_config_t** out)
{
    if (url == nullptr || url[0] == '\0' || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        auto* config = new (std::nothrow) galay_mcp_client_config();
        if (config == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        config->mode = GALAY_MCP_MODE_HTTP;
        config->url = url;
        *out = config;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_mcp_client_config_destroy(galay_mcp_client_config_t* config)
{
    delete config;
}

galay_mcp_client_mode_t galay_mcp_client_config_mode(const galay_mcp_client_config_t* config)
{
    if (config == nullptr) {
        return GALAY_MCP_MODE_INVALID;
    }
    return config->mode;
}

FILE* galay_mcp_stdio_config_input(const galay_mcp_client_config_t* config)
{
    if (config == nullptr || config->mode != GALAY_MCP_MODE_STDIO) {
        return nullptr;
    }
    return config->input;
}

FILE* galay_mcp_stdio_config_output(const galay_mcp_client_config_t* config)
{
    if (config == nullptr || config->mode != GALAY_MCP_MODE_STDIO) {
        return nullptr;
    }
    return config->output;
}

galay_status_t galay_mcp_http_config_url(const galay_mcp_client_config_t* config,
                                         const char** url,
                                         size_t* url_len)
{
    if (config == nullptr || config->mode != GALAY_MCP_MODE_HTTP) {
        return GALAY_INVALID_ARGUMENT;
    }
    return set_span(config->url, url, url_len);
}

} // extern "C"
