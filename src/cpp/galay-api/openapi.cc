#include "openapi.h"
#include "schema.h"
#include <serde/json/stream.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace galay::api {
namespace {

using Writer = json::stream::StreamWriter;

ApiError failure(ApiErrorCode code, std::string message) {
    return {code, std::move(message), 500};
}

ApiResult<void> text_metadata(std::string_view value, ApiErrorCode code = ApiErrorCode::invalid_metadata) {
    return schema_detail::validate_text(value, code);
}

bool numeric_less(const SchemaNumber& left, const SchemaNumber& right) {
    return std::visit([](auto a, auto b) {
        if constexpr (std::is_integral_v<decltype(a)> && std::is_integral_v<decltype(b)>) {
            return std::cmp_less(a, b);
        } else {
            return static_cast<long double>(a) < static_cast<long double>(b);
        }
    }, left, right);
}

bool finite_number(const SchemaNumber& number) {
    return std::visit([](auto value) { return std::isfinite(static_cast<long double>(value)); }, number);
}

ApiResult<void> check_schema(const Schema& schema, std::vector<const Schema*>& parents) {
    if (parents.size() >= 128 || std::find(parents.begin(), parents.end(), &schema) != parents.end()) {
        return std::unexpected(failure(ApiErrorCode::invalid_schema, "recursive or excessively nested inline schema"));
    }
    const auto invalid = [](std::string message) -> ApiResult<void> {
        return std::unexpected(failure(ApiErrorCode::invalid_schema, std::move(message)));
    };
    const bool numeric = schema.type == "integer" || schema.type == "number";
    const bool string = schema.type == "string";
    const bool array = schema.type == "array";
    const bool object = schema.type == "object";
    if (!numeric && !string && !array && !object && schema.type != "boolean" && schema.type != "null") {
        return invalid("schema type is unsupported or empty");
    }
    if (auto checked = text_metadata(schema.format, ApiErrorCode::invalid_schema); !checked) return checked;
    if (auto checked = text_metadata(schema.description, ApiErrorCode::invalid_schema); !checked) return checked;
    if ((schema.minimum || schema.maximum) && !numeric) return invalid("numeric bounds require a numeric schema");
    if ((schema.minimum && !finite_number(*schema.minimum)) || (schema.maximum && !finite_number(*schema.maximum))) {
        return invalid("schema numeric bounds must be finite");
    }
    if (schema.minimum && schema.maximum && numeric_less(*schema.maximum, *schema.minimum)) {
        return invalid("schema minimum exceeds maximum");
    }
    if ((schema.min_length || schema.max_length) && !string) return invalid("length bounds require a string schema");
    if (schema.min_length && schema.max_length && *schema.min_length > *schema.max_length) {
        return invalid("schema minLength exceeds maxLength");
    }
    if ((schema.min_items || schema.max_items || schema.items) && !array) return invalid("items require an array schema");
    if (array && !schema.items) return invalid("array schema requires items");
    if (schema.min_items && schema.max_items && *schema.min_items > *schema.max_items) {
        return invalid("schema minItems exceeds maxItems");
    }
    if ((!schema.properties.empty() || !schema.required.empty() || schema.additional_properties ||
         schema.allow_additional_properties || schema.min_properties || schema.max_properties) && !object) {
        return invalid("properties require an object schema");
    }
    if (schema.min_properties && schema.max_properties && *schema.min_properties > *schema.max_properties) {
        return invalid("schema minProperties exceeds maxProperties");
    }
    std::set<std::string_view> required;
    for (const auto& name : schema.required) {
        if (!schema.properties.contains(name)) return invalid("required field is absent from properties: " + name);
        const auto [position, inserted] = required.insert(name);
        if (!inserted) return invalid("duplicate required field: " + std::string(*position));
    }
    for (std::size_t i = 0; i < schema.enum_values.size(); ++i) {
        const auto& value = schema.enum_values[i];
        auto checked = std::visit([&](const auto& entry) -> ApiResult<void> {
            using T = std::remove_cvref_t<decltype(entry)>;
            if constexpr (std::same_as<T, std::string>) {
                if (!string) return invalid("string enum value requires a string schema");
                if (auto valid = text_metadata(entry, ApiErrorCode::invalid_schema); !valid) return valid;
                struct Probe { std::string value; };
                const auto descriptor = reflect::make_field("enum", &Probe::value,
                    reflect::field_options<std::string>{.min_length = schema.min_length, .max_length = schema.max_length});
                if (auto valid = reflect::validate_field(descriptor, entry); !valid) return invalid(valid.error());
            } else if constexpr (std::same_as<T, bool>) {
                if (schema.type != "boolean") return invalid("boolean enum value requires a boolean schema");
            } else {
                if (!numeric || (schema.type == "integer" && std::same_as<T, double>)) {
                    return invalid("enum value does not match schema numeric type");
                }
                const SchemaNumber number{entry};
                if (!finite_number(number)) return invalid("enum number must be finite");
                if ((schema.minimum && numeric_less(number, *schema.minimum)) ||
                    (schema.maximum && numeric_less(*schema.maximum, number))) {
                    return invalid("enum value contradicts numeric bounds");
                }
            }
            return {};
        }, value);
        if (!checked) return checked;
        for (std::size_t j = 0; j < i; ++j) {
            if (value == schema.enum_values[j]) return invalid("duplicate schema enum value");
        }
    }
    parents.push_back(&schema);
    ApiResult<void> status;
    for (const auto& [name, property] : schema.properties) {
        if (auto checked = text_metadata(name, ApiErrorCode::invalid_schema); !checked) {
            status = std::unexpected(std::move(checked.error()));
            break;
        }
        if (auto checked = check_schema(property, parents); !checked) {
            status = std::unexpected(std::move(checked.error()));
            break;
        }
    }
    if (status && schema.items) status = check_schema(*schema.items, parents);
    if (status && schema.additional_properties) status = check_schema(*schema.additional_properties, parents);
    parents.pop_back();
    return status;
}

ApiResult<void> check_schema(const Schema& schema) {
    std::vector<const Schema*> parents;
    return check_schema(schema, parents);
}

template<class T>
json::result<void> member(Writer& writer, std::string_view name, const T& value) {
    if (auto written = writer.key(name); !written) return written;
    return writer.value(value);
}

json::result<void> write_number(Writer& writer, const SchemaNumber& number) {
    return std::visit([&](auto value) { return writer.number(value); }, number);
}

json::result<void> write_schema(Writer& writer, const Schema& schema) {
    if (auto written = writer.start_object(); !written) return written;
    if (schema.nullable && schema.type != "null") {
        if (auto written = writer.key("type"); !written) return written;
        if (auto written = writer.start_array(); !written) return written;
        if (auto written = writer.string(schema.type); !written) return written;
        if (auto written = writer.string("null"); !written) return written;
        if (auto written = writer.end_array(); !written) return written;
    } else if (auto written = member(writer, "type", schema.type); !written) return written;
    if (!schema.format.empty()) {
        if (auto written = member(writer, "format", schema.format); !written) return written;
    }
    if (!schema.description.empty()) {
        if (auto written = member(writer, "description", schema.description); !written) return written;
    }
    if (schema.minimum) {
        if (auto written = writer.key("minimum"); !written) return written;
        if (auto written = write_number(writer, *schema.minimum); !written) return written;
    }
    if (schema.maximum) {
        if (auto written = writer.key("maximum"); !written) return written;
        if (auto written = write_number(writer, *schema.maximum); !written) return written;
    }
    const std::pair<std::string_view, const std::optional<std::size_t>*> counts[]{
        {"minLength", &schema.min_length}, {"maxLength", &schema.max_length},
        {"minItems", &schema.min_items}, {"maxItems", &schema.max_items},
        {"minProperties", &schema.min_properties}, {"maxProperties", &schema.max_properties}};
    for (const auto& [name, value] : counts) {
        if (*value) {
            if (auto written = member(writer, name, **value); !written) return written;
        }
    }
    if (!schema.enum_values.empty()) {
        if (auto written = writer.key("enum"); !written) return written;
        if (auto written = writer.start_array(); !written) return written;
        for (const auto& value : schema.enum_values) {
            if (auto written = std::visit([&](const auto& item) { return writer.value(item); }, value); !written) return written;
        }
        if (schema.nullable) {
            if (auto written = writer.null_value(); !written) return written;
        }
        if (auto written = writer.end_array(); !written) return written;
    }
    if (schema.type == "object") {
        if (!schema.properties.empty()) {
            if (auto written = writer.key("properties"); !written) return written;
            if (auto written = writer.start_object(); !written) return written;
            for (const auto& [name, property] : schema.properties) {
                if (auto written = writer.key(name); !written) return written;
                if (auto written = write_schema(writer, property); !written) return written;
            }
            if (auto written = writer.end_object(); !written) return written;
        }
        if (!schema.required.empty()) {
            auto required = schema.required;
            std::sort(required.begin(), required.end());
            if (auto written = member(writer, "required", required); !written) return written;
        }
        if (auto written = writer.key("additionalProperties"); !written) return written;
        if (schema.additional_properties) {
            if (auto written = write_schema(writer, *schema.additional_properties); !written) return written;
        } else if (auto written = writer.boolean(schema.allow_additional_properties); !written) return written;
    }
    if (schema.items) {
        if (auto written = writer.key("items"); !written) return written;
        if (auto written = write_schema(writer, *schema.items); !written) return written;
    }
    return writer.end_object();
}

std::string_view method_name(http::HttpMethod method) {
    switch (method) {
    case http::HttpMethod::GET: return "get";
    case http::HttpMethod::POST: return "post";
    case http::HttpMethod::HEAD: return "head";
    case http::HttpMethod::PUT: return "put";
    case http::HttpMethod::DELETE: return "delete";
    case http::HttpMethod::TRACE: return "trace";
    case http::HttpMethod::OPTIONS: return "options";
    case http::HttpMethod::PATCH: return "patch";
    default: return {};
    }
}

bool letter(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool digit(char ch) { return ch >= '0' && ch <= '9'; }

bool valid_parameter(std::string_view name) {
    if (name.empty() || (!letter(name.front()) && name.front() != '_')) return false;
    return std::all_of(name.begin(), name.end(), [](char ch) { return letter(ch) || digit(ch) || ch == '_'; });
}

std::vector<std::string_view> segments(std::string_view path) {
    std::vector<std::string_view> result;
    for (std::size_t begin = 1; begin < path.size();) {
        const auto end = path.find('/', begin);
        result.push_back(path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return result;
}

bool ambiguous_paths(std::string_view left, std::string_view right) {
    const auto a = segments(left);
    const auto b = segments(right);
    if (a.size() != b.size()) return false;
    bool a_specific = false;
    bool b_specific = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const bool a_param = a[i].starts_with(':');
        const bool b_param = b[i].starts_with(':');
        if (!a_param && !b_param && a[i] != b[i]) return false;
        a_specific |= !a_param && b_param;
        b_specific |= a_param && !b_param;
    }
    return a_specific && b_specific;
}

Schema error_schema() {
    Schema schema;
    schema.type = "object";
    Schema text;
    text.type = "string";
    const auto [code, code_inserted] = schema.properties.emplace("code", text);
    const auto [message, message_inserted] = schema.properties.emplace("message", std::move(text));
    if (code_inserted && message_inserted) schema.required = {code->first, message->first};
    return schema;
}

json::result<void> write_content(Writer& writer, const Schema& schema) {
    if (auto written = writer.key("content"); !written) return written;
    if (auto written = writer.start_object(); !written) return written;
    if (auto written = writer.key("application/json"); !written) return written;
    if (auto written = writer.start_object(); !written) return written;
    if (auto written = writer.key("schema"); !written) return written;
    if (auto written = write_schema(writer, schema); !written) return written;
    if (auto written = writer.end_object(); !written) return written;
    return writer.end_object();
}

json::result<void> write_operation(Writer& writer, const EndpointSpec& endpoint) {
    const auto& operation = endpoint.operation;
    if (auto written = writer.start_object(); !written) return written;
    if (auto written = member(writer, "operationId", operation.id); !written) return written;
    if (!operation.summary.empty()) {
        if (auto written = member(writer, "summary", operation.summary); !written) return written;
    }
    if (!operation.description.empty()) {
        if (auto written = member(writer, "description", operation.description); !written) return written;
    }
    if (!operation.tags.empty()) {
        if (auto written = member(writer, "tags", operation.tags); !written) return written;
    }
    if (!endpoint.parameters.empty()) {
        std::vector<const Parameter*> parameters;
        for (const auto& parameter : endpoint.parameters) parameters.push_back(&parameter);
        std::sort(parameters.begin(), parameters.end(), [](const Parameter* left, const Parameter* right) {
            return std::tie(left->source, left->name) < std::tie(right->source, right->name);
        });
        if (auto written = writer.key("parameters"); !written) return written;
        if (auto written = writer.start_array(); !written) return written;
        for (const auto* parameter : parameters) {
            if (auto written = writer.start_object(); !written) return written;
            if (auto written = member(writer, "name", parameter->name); !written) return written;
            const std::string_view source = parameter->source == ParameterSource::path ? "path" : "query";
            if (auto written = member(writer, "in", source); !written) return written;
            if (auto written = member(writer, "required", parameter->required); !written) return written;
            if (!parameter->description.empty()) {
                if (auto written = member(writer, "description", parameter->description); !written) return written;
            }
            if (auto written = writer.key("schema"); !written) return written;
            if (auto written = write_schema(writer, parameter->schema); !written) return written;
            if (auto written = writer.end_object(); !written) return written;
        }
        if (auto written = writer.end_array(); !written) return written;
    }
    if (endpoint.request_body) {
        if (auto written = writer.key("requestBody"); !written) return written;
        if (auto written = writer.start_object(); !written) return written;
        if (auto written = member(writer, "required", endpoint.body_required); !written) return written;
        if (auto written = write_content(writer, *endpoint.request_body); !written) return written;
        if (auto written = writer.end_object(); !written) return written;
    }
    std::map<int, std::string> responses;
    responses[500] = "Internal Server Error";
    responses[400] = "Bad Request";
    if (endpoint.request_body) responses[415] = "Unsupported Media Type";
    for (const auto& response : operation.errors) responses[response.status] = response.description;
    responses[operation.success_status] = operation.success_description;
    const auto error_body = error_schema();
    if (auto written = writer.key("responses"); !written) return written;
    if (auto written = writer.start_object(); !written) return written;
    for (const auto& [status, description] : responses) {
        if (auto written = writer.key(std::to_string(status)); !written) return written;
        if (auto written = writer.start_object(); !written) return written;
        if (auto written = member(writer, "description", description); !written) return written;
        if (endpoint.method != http::HttpMethod::HEAD) {
            if (status == operation.success_status) {
                if (endpoint.response_body && status != 204 && status != 205) {
                    if (auto written = write_content(writer, *endpoint.response_body); !written) return written;
                }
            } else if (auto written = write_content(writer, error_body); !written) return written;
        }
        if (auto written = writer.end_object(); !written) return written;
    }
    if (auto written = writer.end_object(); !written) return written;
    return writer.end_object();
}

json::result<void> write_document(Writer& writer, const ApiInfo& info,
                                const std::map<std::string, std::map<std::string, const EndpointSpec*>>& paths) {
    if (auto written = writer.start_object(); !written) return written;
    if (auto written = member(writer, "openapi", std::string_view("3.1.0")); !written) return written;
    if (auto written = writer.key("info"); !written) return written;
    if (auto written = writer.start_object(); !written) return written;
    if (auto written = member(writer, "title", info.title); !written) return written;
    if (auto written = member(writer, "version", info.version); !written) return written;
    if (!info.description.empty()) {
        if (auto written = member(writer, "description", info.description); !written) return written;
    }
    if (auto written = writer.end_object(); !written) return written;
    if (auto written = writer.key("paths"); !written) return written;
    if (auto written = writer.start_object(); !written) return written;
    for (const auto& [path, methods] : paths) {
        if (auto written = writer.key(path); !written) return written;
        if (auto written = writer.start_object(); !written) return written;
        for (const auto& [method, endpoint] : methods) {
            if (auto written = writer.key(method); !written) return written;
            if (auto written = write_operation(writer, *endpoint); !written) return written;
        }
        if (auto written = writer.end_object(); !written) return written;
    }
    if (auto written = writer.end_object(); !written) return written;
    if (auto written = writer.end_object(); !written) return written;
    return writer.finish();
}

} // namespace

ApiResult<std::string> schema_json(const Schema& schema) {
    if (auto checked = check_schema(schema); !checked) return std::unexpected(std::move(checked.error()));
    std::string output;
    Writer writer{[&](std::string_view fragment) -> json::result<void> {
        // append returns only *this, not a fallible result.
        (void)output.append(fragment);
        return {};
    }};
    if (auto written = write_schema(writer, schema); !written) {
        return std::unexpected(failure(ApiErrorCode::encoding_error, std::move(written.error())));
    }
    if (auto written = writer.finish(); !written) {
        return std::unexpected(failure(ApiErrorCode::encoding_error, std::move(written.error())));
    }
    return output;
}

ApiResult<PathInfo> inspect_path(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.size() > 2048) {
        return std::unexpected(failure(ApiErrorCode::invalid_path, "path must start with '/' and contain at most 2048 bytes"));
    }
    if (path.size() > 1 && path.back() == '/') {
        return std::unexpected(failure(ApiErrorCode::invalid_path, "path contains a trailing empty segment"));
    }
    PathInfo result;
    if (path == "/") {
        result.openapi_path = result.shape = "/";
        return result;
    }
    for (const auto segment : segments(path)) {
        if (segment.empty()) return std::unexpected(failure(ApiErrorCode::invalid_path, "path contains an empty segment"));
        result.openapi_path.push_back('/');
        result.shape.push_back('/');
        if (segment.front() == ':') {
            const auto name = segment.substr(1);
            if (!valid_parameter(name)) return std::unexpected(failure(ApiErrorCode::invalid_path, "invalid path parameter name: " + std::string(name)));
            if (std::find(result.parameters.begin(), result.parameters.end(), name) != result.parameters.end()) {
                return std::unexpected(failure(ApiErrorCode::invalid_path, "duplicate path parameter name: " + std::string(name)));
            }
            result.parameters.push_back(std::string(name));
            result.openapi_path.push_back('{');
            // append returns only the destination string reference.
            (void)result.openapi_path.append(name);
            result.openapi_path.push_back('}');
            (void)result.shape.append("{}");
        } else {
            if (segment == "." || segment == "..") {
                return std::unexpected(failure(ApiErrorCode::invalid_path, "path contains a URI dot segment"));
            }
            for (const auto ch : segment) {
                if (!letter(ch) && !digit(ch) && ch != '-' && ch != '_' && ch != '.' && ch != '~') {
                    return std::unexpected(failure(ApiErrorCode::invalid_path, "path literal segments must use unreserved ASCII characters"));
                }
            }
            // append returns only the destination string reference.
            (void)result.openapi_path.append(segment);
            (void)result.shape.append(segment);
        }
    }
    return result;
}

ApiResult<void> validate_endpoint(const EndpointSpec& endpoint, std::span<const EndpointSpec> existing) {
    if (method_name(endpoint.method).empty()) {
        return std::unexpected(failure(ApiErrorCode::invalid_metadata, "unsupported HTTP/1 REST method"));
    }
    auto path = inspect_path(endpoint.path);
    if (!path) return std::unexpected(std::move(path.error()));
    const auto& operation = endpoint.operation;
    if (operation.id.empty()) return std::unexpected(failure(ApiErrorCode::invalid_metadata, "operationId must not be empty"));
    for (const auto* value : {&operation.id, &operation.summary, &operation.description, &operation.success_description}) {
        if (auto checked = text_metadata(*value); !checked) return checked;
    }
    for (const auto& tag : operation.tags) {
        if (tag.empty()) return std::unexpected(failure(ApiErrorCode::invalid_metadata, "operation tag must not be empty"));
        if (auto checked = text_metadata(tag); !checked) return checked;
    }
    if (operation.success_status < 200 || operation.success_status > 299) {
        return std::unexpected(failure(ApiErrorCode::invalid_metadata, "success status must be a 2xx response"));
    }
    if ((operation.success_status == 204 || operation.success_status == 205) && endpoint.response_body) {
        return std::unexpected(failure(ApiErrorCode::invalid_metadata, "204 and 205 responses cannot declare a body"));
    }
    std::set<int> error_statuses;
    for (const auto& response : operation.errors) {
        if (response.status < 400 || response.status > 599) {
            return std::unexpected(failure(ApiErrorCode::invalid_metadata, "business error status must be 4xx or 5xx"));
        }
        const auto [position, inserted] = error_statuses.insert(response.status);
        if (!inserted) return std::unexpected(failure(ApiErrorCode::invalid_metadata, "duplicate business error status: " + std::to_string(*position)));
        if (auto checked = text_metadata(response.description); !checked) return checked;
    }
    std::set<std::pair<ParameterSource, std::string>> sources;
    std::set<std::string> fields;
    std::set<std::string> bound_paths;
    for (const auto& parameter : endpoint.parameters) {
        if (parameter.source != ParameterSource::path && parameter.source != ParameterSource::query) {
            return std::unexpected(failure(ApiErrorCode::invalid_binding, "unknown parameter source"));
        }
        if (parameter.name.empty() || parameter.field_name.empty()) {
            return std::unexpected(failure(ApiErrorCode::invalid_binding, "parameter and DTO field names must not be empty"));
        }
        if (std::any_of(parameter.name.begin(), parameter.name.end(), [](unsigned char ch) {
                return ch < 0x20 || ch == 0x7f;
            })) {
            return std::unexpected(failure(ApiErrorCode::invalid_binding, "parameter source names must not contain control characters"));
        }
        for (const auto* value : {&parameter.name, &parameter.field_name, &parameter.description}) {
            if (auto checked = text_metadata(*value); !checked) return checked;
        }
        const auto [source_position, source_inserted] = sources.emplace(parameter.source, parameter.name);
        if (!source_inserted) return std::unexpected(failure(ApiErrorCode::invalid_binding, "duplicate parameter source: " + source_position->second));
        const auto [field_position, field_inserted] = fields.insert(parameter.field_name);
        if (!field_inserted) return std::unexpected(failure(ApiErrorCode::invalid_binding, "DTO field has multiple parameter sources: " + *field_position));
        if (parameter.schema.type != "string" && parameter.schema.type != "boolean" &&
            parameter.schema.type != "integer" && parameter.schema.type != "number") {
            return std::unexpected(failure(ApiErrorCode::invalid_binding, "path and query parameters must have scalar schemas"));
        }
        if (auto checked = check_schema(parameter.schema); !checked) return checked;
        if (parameter.source == ParameterSource::path) {
            if (!parameter.required || parameter.schema.nullable ||
                std::find(path->parameters.begin(), path->parameters.end(), parameter.name) == path->parameters.end()) {
                return std::unexpected(failure(ApiErrorCode::invalid_binding, "path parameter must match the path and be required and non-nullable"));
            }
            const auto [position, inserted] = bound_paths.insert(parameter.name);
            if (!inserted) return std::unexpected(failure(ApiErrorCode::invalid_binding, "duplicate path parameter: " + *position));
        }
    }
    if (bound_paths.size() != path->parameters.size()) {
        return std::unexpected(failure(ApiErrorCode::invalid_binding, "every path parameter requires one binding"));
    }
    if (endpoint.body_required && !endpoint.request_body) {
        return std::unexpected(failure(ApiErrorCode::invalid_binding, "body_required requires a request body schema"));
    }
    if (endpoint.request_body) {
        if (endpoint.method == http::HttpMethod::GET || endpoint.method == http::HttpMethod::HEAD) {
            return std::unexpected(failure(ApiErrorCode::invalid_binding, "GET and HEAD request bodies are unsupported"));
        }
        if (endpoint.request_body->type != "object" || endpoint.request_body->nullable) {
            return std::unexpected(failure(ApiErrorCode::invalid_binding, "request body must describe a non-nullable DTO object"));
        }
        if (auto checked = check_schema(*endpoint.request_body); !checked) return checked;
        for (const auto& field : fields) {
            if (endpoint.request_body->properties.contains(field)) {
                return std::unexpected(failure(ApiErrorCode::invalid_binding, "DTO field is bound both as a parameter and body property: " + field));
            }
        }
    }
    if (endpoint.response_body) {
        if (auto checked = check_schema(*endpoint.response_body); !checked) return checked;
    }
    for (const auto& other : existing) {
        auto other_path = inspect_path(other.path);
        if (!other_path) return std::unexpected(std::move(other_path.error()));
        if (other_path->shape == path->shape && other.path != endpoint.path) {
            return std::unexpected(failure(ApiErrorCode::route_conflict, "same-shaped routes must use identical parameter names across all methods"));
        }
        if (other.path == endpoint.path && other.method == endpoint.method) {
            return std::unexpected(failure(ApiErrorCode::route_conflict, "HTTP method and path are already registered"));
        }
        if (ambiguous_paths(other.path, endpoint.path)) {
            return std::unexpected(failure(ApiErrorCode::route_conflict, "overlapping routes have incomparable literal segments"));
        }
        if (other.operation.id == operation.id) {
            return std::unexpected(failure(ApiErrorCode::duplicate_operation, "operationId is already registered: " + operation.id));
        }
    }
    return {};
}

ApiResult<std::string> render_openapi(const ApiInfo& info, std::span<const EndpointSpec> endpoints) {
    if (info.title.empty() || info.version.empty()) {
        return std::unexpected(failure(ApiErrorCode::invalid_metadata, "API title and version must not be empty"));
    }
    for (const auto* value : {&info.title, &info.version, &info.description}) {
        if (auto checked = text_metadata(*value); !checked) return std::unexpected(std::move(checked.error()));
    }
    std::map<std::string, std::map<std::string, const EndpointSpec*>> paths;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        const auto& endpoint = endpoints[i];
        if (auto checked = validate_endpoint(endpoint, endpoints.first(i)); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        auto path = inspect_path(endpoint.path);
        if (!path) return std::unexpected(std::move(path.error()));
        const auto [position, inserted] = paths[path->openapi_path].emplace(std::string(method_name(endpoint.method)), &endpoint);
        if (!inserted) return std::unexpected(failure(ApiErrorCode::route_conflict, "duplicate document operation: " + position->first));
    }
    std::string output;
    Writer writer{[&](std::string_view fragment) -> json::result<void> {
        // append returns only *this, not a fallible result.
        (void)output.append(fragment);
        return {};
    }};
    if (auto written = write_document(writer, info, paths); !written) {
        return std::unexpected(failure(ApiErrorCode::encoding_error, std::move(written.error())));
    }
    return output;
}

} // namespace galay::api
