#ifndef GALAY_HTTP_API_OPERATION_H
#define GALAY_HTTP_API_OPERATION_H

#include "schema_model.h"
#include "../protoc/http_base.h"
#include <span>

namespace galay::api {

enum class ParameterSource { path, query };

struct ErrorResponse {
    int status = 400;
    std::string description;
};

struct Operation {
    std::string id;
    std::string summary;
    std::string description;
    std::vector<std::string> tags;
    int success_status = 200;
    std::string success_description = "Success";
    std::vector<ErrorResponse> errors;
};

struct Parameter {
    ParameterSource source = ParameterSource::query;
    std::string name;
    std::string field_name;
    std::string description;
    bool required = true;
    Schema schema;
};

struct EndpointSpec {
    http::HttpMethod method = http::HttpMethod::GET;
    std::string path;
    Operation operation;
    std::vector<Parameter> parameters;
    std::optional<Schema> request_body;
    bool body_required = false;
    std::optional<Schema> response_body;
};

struct ApiInfo {
    std::string title = "Galay API";
    std::string version = "1.0.0";
    std::string description;
};

struct PathInfo {
    std::string openapi_path;
    std::string shape;
    std::vector<std::string> parameters;
};

ApiResult<PathInfo> inspect_path(std::string_view path);
ApiResult<void> validate_endpoint(const EndpointSpec& endpoint,
                                  std::span<const EndpointSpec> existing);
ApiResult<std::string> render_openapi(const ApiInfo& info,
                                     std::span<const EndpointSpec> endpoints);

} // namespace galay::api

#endif
