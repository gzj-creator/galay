#include <galay/cpp/galay-api/openapi.h>
#include <galay/cpp/galay-api/schema.h>
#include <serde/json/json.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace galay::api;
using galay::http::HttpMethod;

bool expect(bool condition, std::string_view name) {
    if (!condition) std::cerr << "FAIL: " << name << '\n';
    return condition;
}

template<class T>
bool error_is(const ApiResult<T>& result, ApiErrorCode code, std::string_view name) {
    return expect(!result && result.error().code == code && !result.error().message.empty(), name);
}

Schema scalar(std::string type) {
    Schema schema;
    schema.type = std::move(type);
    return schema;
}

EndpointSpec endpoint(HttpMethod method, std::string path, std::string id) {
    EndpointSpec spec;
    spec.method = method;
    spec.path = std::move(path);
    spec.operation.id = std::move(id);
    return spec;
}

Parameter path_parameter(std::string name, std::string field = "id") {
    return Parameter{.source = ParameterSource::path, .name = std::move(name),
                     .field_name = std::move(field), .schema = scalar("integer")};
}

} // namespace

int main() {
    bool passed = true;
    const auto root = inspect_path("/");
    const auto path = inspect_path("/users/:user_id/notes/:note");
    passed &= expect(root && root->openapi_path == "/" && root->parameters.empty(), "root route");
    passed &= expect(path && path->openapi_path == "/users/{user_id}/notes/{note}" &&
                     path->shape == "/users/{}/notes/{}" &&
                     path->parameters == std::vector<std::string>{"user_id", "note"}, "path conversion and shape");
    for (const auto bad : {"", "users", "/users/", "//users", "/users//notes", "/users/*", "/users/**",
                           "/users/:", "/users/:1bad", "/users/:bad-name", "/:id/:id", "/users?x=1",
                           "/users#fragment", "/users/{id}", "/bad path", "/bad\\path", "/\xFF"}) {
        passed &= error_is(inspect_path(bad), ApiErrorCode::invalid_path, bad);
    }
    for (const auto bad : {"/.", "/..", "/users/./notes", "/users/../notes"}) {
        passed &= error_is(inspect_path(bad), ApiErrorCode::invalid_path, "URI dot segment rejected");
    }
    passed &= error_is(inspect_path(std::string("/a\0b", 4)), ApiErrorCode::invalid_path, "embedded NUL in path");
    passed &= error_is(inspect_path("/" + std::string(2048, 'x')), ApiErrorCode::invalid_path, "router path limit");

    auto get = endpoint(HttpMethod::GET, "/users/:id", "getUser");
    get.parameters.push_back(path_parameter("id"));
    get.parameters.push_back(Parameter{.source = ParameterSource::query, .name = "verbose", .field_name = "verbose",
                                       .required = false, .schema = scalar("boolean")});
    get.parameters.back().schema.nullable = true;
    get.response_body = scalar("string");
    passed &= expect(validate_endpoint(get, {}).has_value(), "valid endpoint");
    const std::array existing{get};
    auto post = endpoint(HttpMethod::POST, "/users/:id", "postUser");
    post.parameters.push_back(path_parameter("id"));
    passed &= expect(validate_endpoint(post, existing).has_value(), "same spelling different method allowed");
    auto shape_conflict = endpoint(HttpMethod::POST, "/users/:user", "otherUser");
    shape_conflict.parameters.push_back(path_parameter("user"));
    passed &= error_is(validate_endpoint(shape_conflict, existing), ApiErrorCode::route_conflict,
                       "same shape different names rejected across methods");
    auto duplicate = get;
    duplicate.operation.id = "otherOperation";
    passed &= error_is(validate_endpoint(duplicate, existing), ApiErrorCode::route_conflict, "same method and path rejected");
    auto duplicate_id = endpoint(HttpMethod::POST, "/elsewhere", "getUser");
    passed &= error_is(validate_endpoint(duplicate_id, existing), ApiErrorCode::duplicate_operation, "duplicate operationId");
    auto ambiguous = endpoint(HttpMethod::GET, "/:group/lookup", "lookup");
    ambiguous.parameters.push_back(path_parameter("group", "group"));
    passed &= error_is(validate_endpoint(ambiguous, existing), ApiErrorCode::route_conflict, "overlapping incomparable routes rejected");
    auto concrete = endpoint(HttpMethod::GET, "/users/me", "self");
    passed &= expect(validate_endpoint(concrete, existing).has_value(), "concrete route may specialize templated route");

    auto invalid = get;
    invalid.operation.id.clear();
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_metadata, "empty operationId");
    for (const auto method : {HttpMethod::CONNECT, HttpMethod::PRI, HttpMethod::UNKNOWN, static_cast<HttpMethod>(99)}) {
        invalid = get;
        invalid.method = method;
        passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_metadata, "unsupported REST method");
    }
    invalid = get;
    invalid.parameters.erase(invalid.parameters.begin());
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "missing path source");
    invalid = get;
    invalid.parameters.front().name = "wrong";
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "unmatched path source");
    invalid = get;
    invalid.parameters.front().required = false;
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "optional path source");
    invalid = get;
    invalid.parameters.front().schema.nullable = true;
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "nullable path source");
    invalid = get;
    invalid.parameters.push_back(invalid.parameters.back());
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "duplicate query source");
    invalid = get;
    invalid.parameters.back().field_name = "id";
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "same DTO field from two sources");
    invalid = get;
    invalid.parameters.back().source = static_cast<ParameterSource>(99);
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "unknown parameter source");
    invalid = get;
    invalid.parameters.back().name = std::string("bad\0name", 8);
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "control character in parameter source");
    invalid = get;
    invalid.parameters.back().schema = scalar("object");
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "complex query parameter rejected");
    invalid = get;
    invalid.body_required = true;
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "body_required without body");
    invalid = get;
    invalid.request_body = scalar("object");
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "GET body prohibited");
    invalid.method = HttpMethod::HEAD;
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "HEAD body prohibited");
    invalid = post;
    invalid.request_body = scalar("object");
    invalid.request_body->properties.emplace("id", scalar("integer"));
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_binding, "body field duplicates path field");

    invalid = get;
    invalid.operation.success_status = 400;
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_metadata, "success must be 2xx");
    for (const auto status : {204, 205}) {
        invalid = get;
        invalid.operation.success_status = status;
        passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_metadata, "body forbidden for 204/205");
        invalid.response_body.reset();
        passed &= expect(validate_endpoint(invalid, {}).has_value(), "204/205 without body accepted");
    }
    invalid = get;
    invalid.operation.errors = {{404, "Not found"}, {404, "Duplicate"}};
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_metadata, "duplicate error status");
    invalid.operation.errors = {{302, "Not an error"}};
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_metadata, "invalid business status");
    invalid = get;
    invalid.operation.summary = "\xFF";
    passed &= error_is(validate_endpoint(invalid, {}), ApiErrorCode::invalid_metadata, "invalid UTF-8 operation metadata");

    auto no_input = endpoint(HttpMethod::GET, "/health", "health");
    no_input.operation.errors = {{503, "Unavailable"}};
    auto create = endpoint(HttpMethod::POST, "/users", "createUser");
    create.operation.summary = "Create \"user\"\nnow";
    create.operation.tags = {"users"};
    create.operation.success_status = 201;
    create.operation.errors = {{409, "Conflict"}};
    create.request_body = scalar("object");
    create.request_body->properties.emplace("name", scalar("string"));
    create.request_body->required = {"name"};
    create.body_required = true;
    create.response_body = scalar("string");
    auto head = endpoint(HttpMethod::HEAD, "/users/:id", "headUser");
    head.parameters.push_back(path_parameter("id"));
    head.response_body = scalar("string");
    head.operation.errors = {{404, "Not found"}, {403, "Forbidden"}};
    auto deleted = endpoint(HttpMethod::DELETE, "/users/:id", "deleteUser");
    deleted.parameters.push_back(path_parameter("id"));
    deleted.operation.success_status = 204;
    std::vector endpoints{get, no_input, create, head, deleted};
    const ApiInfo info{"Example \"API\"", "1.0", "Line one\nLine two"};
    const auto document = render_openapi(info, endpoints);
    passed &= expect(document.has_value(), "render valid OpenAPI document");
    std::reverse(endpoints.begin(), endpoints.end());
    const auto reversed = render_openapi(info, endpoints);
    passed &= expect(document && reversed && *document == *reversed, "deterministic endpoint ordering");
    if (document) {
        const auto parsed = json::parse(*document);
        passed &= expect(parsed.has_value(), "OpenAPI output parses");
        if (parsed) {
            const auto version = (*parsed)["openapi"].as_string();
            const auto title = (*parsed)["info"]["title"].as_string();
            passed &= expect(version && *version == "3.1.0" && title && *title == info.title, "version and correct string escaping");
            const auto paths = (*parsed)["paths"];
            const auto get_responses = paths["/users/{id}"]["get"]["responses"];
            passed &= expect(get_responses.contains("200") && get_responses.contains("400") &&
                             get_responses.contains("500") && !get_responses.contains("415"), "parameter framework errors only");
            const auto health_responses = paths["/health"]["get"]["responses"];
            passed &= expect(health_responses.size() == 4 && health_responses.contains("200") && health_responses.contains("503") &&
                             health_responses.contains("500") && health_responses.contains("400") && !health_responses.contains("415"),
                             "no input can reject unexpected body or malformed query, but never media type");
            const auto create_responses = paths["/users"]["post"]["responses"];
            passed &= expect(create_responses.size() == 5 && create_responses.contains("201") && create_responses.contains("409") &&
                             create_responses.contains("400") && create_responses.contains("415") && create_responses.contains("500"),
                             "declared business plus actual body framework errors");
            const auto request_required = paths["/users"]["post"]["requestBody"]["required"].as_bool();
            passed &= expect(request_required && *request_required, "request body required");
            const auto error_schema = create_responses["400"]["content"]["application/json"]["schema"];
            passed &= expect(error_schema["properties"].contains("code") && error_schema["properties"].contains("message") &&
                             error_schema["required"].size() == 2, "error JSON matches code/message runtime contract");
            const auto head_responses = paths["/users/{id}"]["head"]["responses"];
            const auto checked = head_responses.for_each_member([&](std::string_view, const json::Json& response) -> json::result<void> {
                passed &= expect(!response.contains("content"), "HEAD omits all success and error content");
                return {};
            });
            passed &= expect(checked.has_value(), "HEAD response iteration");
            passed &= expect(!paths["/users/{id}"]["delete"]["responses"]["204"].contains("content"), "204 response has no content");
            passed &= expect(!parsed->contains("components"), "schemas are inline without inferred public component names");
        }
    }
    const std::array duplicate_endpoints{get, get};
    passed &= expect(!render_openapi(info, duplicate_endpoints), "render revalidates all endpoints");
    passed &= error_is(render_openapi(ApiInfo{"\xFF", "1.0", ""}, {}), ApiErrorCode::invalid_metadata, "invalid info UTF-8");
    passed &= error_is(render_openapi(ApiInfo{"", "1.0", ""}, {}), ApiErrorCode::invalid_metadata, "empty API title");
    const auto empty_document = render_openapi(info, {});
    passed &= expect(empty_document.has_value(), "empty API document");
    if (passed) std::cout << "api OpenAPI contract passed\n";
    return passed ? 0 : 1;
}
