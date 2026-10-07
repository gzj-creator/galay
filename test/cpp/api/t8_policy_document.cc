#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-api/docs.h>
#include <galay/cpp/galay-http/protoc/http_response.h>
#include <serde/json/json.hpp>
#include <serde/json/stream.hpp>
#include <serde/reflect/reflect_macros.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fixture {

enum class Mode { active, disabled };
constexpr auto reflect_enum(std::type_identity<Mode>) {
    return reflect::enum_descriptor<Mode, 2>{reflect::enum_encoding::string,
        {{{Mode::active, "active"}, {Mode::disabled, "disabled"}}}};
}

enum class Code : std::uint64_t { large = 9007199254740993ULL, maximum = UINT64_MAX };
constexpr auto reflect_enum(std::type_identity<Code>) {
    return reflect::enum_descriptor<Code, 2>{reflect::enum_encoding::underlying,
        {{{Code::large, "large"}, {Code::maximum, "maximum"}}}};
}

struct Payload {
    std::uint64_t id{};
    std::string name;
    std::optional<int> limit = 25;
    std::vector<std::string> labels;
    std::array<int, 2> fixed{};
    std::map<std::string, std::optional<std::string>> attributes;
    std::optional<Mode> mode;
    Code code = Code::maximum;
};
#define PAYLOAD_FIELDS(X) \
    X(id, "external-id", (reflect::field_options<std::uint64_t>{.minimum = 9007199254740993ULL, .maximum = UINT64_MAX})) \
    X(name, "display-name", (reflect::field_options<std::string>{.description = "Name\n\"quoted\"", .min_length = 1, .max_length = 4})) \
    X(limit, "limit", (reflect::field_options<std::optional<int>>{.minimum = 18, .maximum = 120})) \
    X(labels, "labels", (reflect::field_options<std::vector<std::string>>{.min_items = 1, .max_items = 3})) \
    X(fixed, "fixed", (reflect::field_options<std::array<int, 2>>{.min_items = 1, .max_items = 3})) \
    X(attributes, "attributes", (reflect::field_options<std::map<std::string, std::optional<std::string>>>{.min_items = 1, .max_items = 2})) \
    X(mode) \
    X(code, "code", (reflect::field_options<Code>{.minimum = UINT64_MAX}))
REFLECT_FIELDS(Payload, PAYLOAD_FIELDS)
#undef PAYLOAD_FIELDS

struct Parameters {
    std::uint64_t id{};
    std::optional<int> count = 20;
};
#define PARAMETER_FIELDS(X) \
    X(id) \
    X(count, "count", (reflect::field_options<std::optional<int>>{.minimum = 1, .maximum = 30}))
REFLECT_FIELDS(Parameters, PARAMETER_FIELDS)
#undef PARAMETER_FIELDS

} // namespace fixture

namespace {

using namespace galay::api;
using namespace galay::http;
using galay::kernel::Task;

static_assert(std::same_as<decltype(PreparedApi::document), std::shared_ptr<const std::string>>);

void require(bool condition, std::string_view message) {
    if (condition) return;
    if (std::fprintf(stderr, "api.policy_document: %.*s\n", static_cast<int>(message.size()), message.data()) < 0) {
        std::exit(2);
    }
    std::exit(1);
}

ApiInfo info() {
    return {.title = "Policy document", .version = "1.0.0", .description = "Frozen typed operations"};
}

Task<ApiResult<fixture::Payload>> read(ApiContext&, fixture::Parameters input) {
    fixture::Payload output;
    output.id = input.id;
    co_return output;
}

Task<ApiResult<fixture::Payload>> write(ApiContext&, fixture::Payload input) {
    co_return input;
}

Task<ApiResult<NoContent>> remove(ApiContext&, fixture::Parameters) {
    co_return NoContent{};
}

Task<ApiResult<NoContent>> empty(ApiContext&, NoInput) {
    co_return NoContent{};
}

Task<void> raw(HttpConn&, HttpRequest) {
    co_return;
}

PreparedApi prepared() {
    ApiBuilder builder(info());
    const auto parameters = InputBinding<fixture::Parameters>{}
        .path<&fixture::Parameters::id>("id").query<&fixture::Parameters::count>("count");
    require(builder.add<HttpMethod::GET, fixture::Parameters, fixture::Payload>(
        "/items/:id", read, Operation{.id = "getItem", .errors = {{404, "Not found"}}}, parameters).has_value(), "add GET");
    require(builder.add<HttpMethod::HEAD, fixture::Parameters, fixture::Payload>(
        "/items/:id", read, Operation{.id = "headItem", .errors = {{404, "Not found"}}}, parameters).has_value(), "add HEAD");
    require(builder.add<HttpMethod::DELETE, fixture::Parameters, NoContent>(
        "/items/:id", remove, Operation{.id = "deleteItem", .success_status = 204,
            .errors = {{409, "Conflict"}}}, parameters).has_value(), "add DELETE");
    require(builder.add<HttpMethod::POST, fixture::Payload, fixture::Payload>(
        "/items", write, Operation{.id = "createItem", .success_status = 201,
            .errors = {{409, "Conflict"}}}).has_value(), "add POST");
    require(builder.add<HttpMethod::PUT, fixture::Payload, fixture::Payload>(
        "/items", write, Operation{.id = "replaceItem", .errors = {{409, "Conflict"}}}).has_value(), "add PUT");
    require(builder.add<HttpMethod::PATCH, fixture::Payload, fixture::Payload>(
        "/items", write, Operation{.id = "patchItem", .errors = {{422, "Rejected"}}}).has_value(), "add PATCH");
    require(builder.add<HttpMethod::OPTIONS, NoInput, NoContent>(
        "/items", empty, Operation{.id = "optionsItems", .success_status = 204,
            .errors = {{503, "Unavailable"}}}).has_value(), "add OPTIONS");
    require(builder.add<HttpMethod::TRACE, NoInput, NoContent>(
        "/diagnostics", empty, Operation{.id = "traceDiagnostics", .errors = {{503, "Unavailable"}}}).has_value(), "add TRACE");
    auto result = builder.build();
    require(result.has_value(), "build all typed operations");
    return std::move(*result);
}

bool text_is(const json::Json& value, std::string_view expected) {
    const auto text = value.as_string();
    return text && *text == expected;
}

bool number_is(const json::Json& value, std::uint64_t expected) {
    const auto number = value.as_uint64();
    return number && *number == expected;
}

bool includes(const json::Json& array, std::string_view expected) {
    if (!array.is_array()) return false;
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (text_is(array.at(i), expected)) return true;
    }
    return false;
}

bool nullable(const json::Json& schema, std::string_view type) {
    const auto types = schema["type"];
    return types.is_array() && types.size() == 2 && includes(types, type) && includes(types, "null") &&
        !schema.contains("nullable");
}

void check_payload(const json::Json& schema, bool output) {
    require(text_is(schema["type"], "object"), "DTO object schema");
    const auto extra = schema["additionalProperties"].as_bool();
    require(extra && !*extra, "body and output reject undocumented fields");
    const auto properties = schema["properties"];
    const auto required = schema["required"];
    require(properties.size() == 8 && !properties.contains("id") && !properties.contains("name"), "serde wire names preserved");
    require(required.size() == (output ? 8 : 6), "input optional versus output always-present fields");
    for (const auto name : {"external-id", "display-name", "labels", "fixed", "attributes", "code"}) {
        require(includes(required, name), "nonoptional fields required");
    }
    require(includes(required, "limit") == output && includes(required, "mode") == output,
            "required and nullable are independent");
    const auto id = properties["external-id"];
    require(text_is(id["type"], "integer") && number_is(id["minimum"], 9007199254740993ULL) &&
            number_is(id["maximum"], UINT64_MAX), "uint64 bounds stay exact beyond double precision");
    const auto name = properties["display-name"];
    require(number_is(name["minLength"], 1) && number_is(name["maxLength"], 4) &&
            text_is(name["description"], "Name\n\"quoted\""), "string constraints and escaped description");
    const auto limit = properties["limit"];
    require(nullable(limit, "integer") && number_is(limit["minimum"], 18) && number_is(limit["maximum"], 120) &&
            !limit.contains("default"), "optional bounds do not invent JSON Schema defaults");
    const auto labels = properties["labels"];
    require(number_is(labels["minItems"], 1) && number_is(labels["maxItems"], 3) &&
            text_is(labels["items"]["type"], "string"), "vector item count constraints");
    const auto fixed = properties["fixed"];
    require(number_is(fixed["minItems"], 2) && number_is(fixed["maxItems"], 2), "fixed-array bounds intersect constraints");
    const auto attributes = properties["attributes"];
    require(number_is(attributes["minProperties"], 1) && number_is(attributes["maxProperties"], 2) &&
            !attributes.contains("minItems") && !attributes.contains("maxItems") &&
            nullable(attributes["additionalProperties"], "string"), "map counts properties with nullable typed values");
    const auto mode = properties["mode"];
    require(nullable(mode, "string") && mode["enum"].size() == 3 &&
            includes(mode["enum"], "active") && includes(mode["enum"], "disabled") && mode["enum"].at(2).is_null(),
            "nullable string enum explicitly admits null");
    const auto code = properties["code"];
    require(text_is(code["type"], "integer") && number_is(code["minimum"], UINT64_MAX) &&
            number_is(code["maximum"], UINT64_MAX) && code["enum"].size() == 1 &&
            number_is(code["enum"].at(0), UINT64_MAX), "uint64 enum intersects its numeric constraint exactly");
}

struct OperationCase {
    HttpMethod method;
    std::string_view path;
    std::string_view verb;
    std::string_view id;
    std::string_view success;
    std::string_view business;
    bool body;
    bool output;
};

constexpr std::array operations{
    OperationCase{HttpMethod::GET, "/items/{id}", "get", "getItem", "200", "404", false, true},
    OperationCase{HttpMethod::HEAD, "/items/{id}", "head", "headItem", "200", "404", false, false},
    OperationCase{HttpMethod::DELETE, "/items/{id}", "delete", "deleteItem", "204", "409", false, false},
    OperationCase{HttpMethod::POST, "/items", "post", "createItem", "201", "409", true, true},
    OperationCase{HttpMethod::PUT, "/items", "put", "replaceItem", "200", "409", true, true},
    OperationCase{HttpMethod::PATCH, "/items", "patch", "patchItem", "200", "422", true, true},
    OperationCase{HttpMethod::OPTIONS, "/items", "options", "optionsItems", "204", "503", false, false},
    OperationCase{HttpMethod::TRACE, "/diagnostics", "trace", "traceDiagnostics", "200", "503", false, false}};

void check_document(std::string_view bytes) {
    const auto parsed = json::parse(bytes);
    require(parsed.has_value(), "immutable OpenAPI document parses after builder destruction");
    require(text_is((*parsed)["openapi"], "3.1.0"), "OpenAPI 3.1");
    const auto paths = (*parsed)["paths"];
    require(paths.size() == 3 && paths["/items/{id}"].size() == 3 && paths["/items"].size() == 4 &&
            paths["/diagnostics"].size() == 1, "all and only eight typed operations");
    for (const auto& expected : operations) {
        const auto operation = paths[expected.path][expected.verb];
        require(text_is(operation["operationId"], expected.id), "every typed operationId retained");
        require(operation.contains("requestBody") == expected.body, "only JSON body operations declare requestBody");
        if (expected.body) {
            const auto body_required = operation["requestBody"]["required"].as_bool();
            require(body_required && *body_required, "nonoptional body fields require a body");
            check_payload(operation["requestBody"]["content"]["application/json"]["schema"], false);
        }
        const auto responses = operation["responses"];
        require(responses.size() == (expected.body ? 5 : 4) && responses.contains(expected.success) &&
                responses.contains(expected.business) && responses.contains("400") && responses.contains("500") &&
                responses.contains("415") == expected.body, "actual framework and declared business error statuses");
        require(responses[expected.success].contains("content") == expected.output, "HEAD and NoContent omit success content");
        if (expected.output) check_payload(responses[expected.success]["content"]["application/json"]["schema"], true);
        for (const auto status : {std::string_view("400"), std::string_view("500"), expected.business, std::string_view("415")}) {
            if (!responses.contains(status)) continue;
            const auto response = responses[status];
            if (expected.method == HttpMethod::HEAD) {
                require(!response.contains("content"), "HEAD omits error bodies too");
                continue;
            }
            const auto error = response["content"]["application/json"]["schema"];
            require(error["properties"].size() == 2 && text_is(error["properties"]["code"]["type"], "string") &&
                    text_is(error["properties"]["message"]["type"], "string") && error["required"].size() == 2 &&
                    includes(error["required"], "code") && includes(error["required"], "message"), "framework error envelope remains code/message");
        }
    }
    const auto parameters = paths["/items/{id}"]["get"]["parameters"];
    require(parameters.size() == 2, "path and query bindings retained");
    const auto path = parameters.at(0);
    const auto path_required = path["required"].as_bool();
    require(text_is(path["name"], "id") && text_is(path["in"], "path") && path_required && *path_required &&
            text_is(path["schema"]["type"], "integer") && number_is(path["schema"]["minimum"], 0) &&
            number_is(path["schema"]["maximum"], UINT64_MAX), "path uint64 is required, nonnullable and exact");
    const auto query = parameters.at(1);
    const auto query_required = query["required"].as_bool();
    // Optional textual parameters may be absent, but have no JSON null token.
    const bool query_valid = text_is(query["name"], "count") && text_is(query["in"], "query") &&
        query_required && !*query_required && text_is(query["schema"]["type"], "integer") &&
        !query["schema"].contains("nullable") && number_is(query["schema"]["minimum"], 1) &&
        number_is(query["schema"]["maximum"], 30);
    if (!query_valid) {
        std::string actual;
        const auto diagnostic = json::stream::serialize(query, [&actual](std::string_view fragment) -> json::result<void> {
            // append returns the destination reference, not a recoverable result.
            (void)actual.append(fragment);
            return {};
        });
        if (!diagnostic) require(false, "serialize optional query diagnostic: " + diagnostic.error());
        require(false, "optional query allows omission, not null, while retaining numeric bounds; actual parameter = " + actual);
    }
    for (const auto path : {"/raw", "/raw/{name}", "/openapi.json", "/docs", "/reference.json", "/reference", "/schema.json"}) {
        require(!paths.contains(path), "raw and policy-installed routes are not reflected typed operations");
    }
}

void check_unchanged(PreparedApi& api, const std::shared_ptr<const std::string>& snapshot,
                     const std::string& bytes) {
    require(api.document == snapshot && *api.document == bytes, "policy retains the identical immutable document and bytes");
    require(api.endpoints.size() == operations.size(), "policy does not append non-typed endpoint metadata");
    const auto rendered = render_openapi(info(), api.endpoints);
    require(rendered && *rendered == bytes, "endpoint metadata remains identical to the frozen document");
    for (const auto& operation : operations) {
        const std::string path = operation.path == "/items/{id}" ? "/items/18446744073709551615" : std::string(operation.path);
        require(api.router.find_handler(operation.method, path).handler != nullptr, "typed routes survive builder destruction and policy installation");
    }
    check_document(*api.document);
}

Task<void> send_spec(std::shared_ptr<const std::string> document, HttpConn& connection) {
    HttpResponse response;
    response.header().version() = HttpVersion::HttpVersion_1_1;
    response.header().code() = HttpStatusCode::OK_200;
    require(response.header().header_pairs().add_header_pair("Content-Type", "application/json") == kNoError,
            "custom spec Content-Type");
    response.set_body_str(std::string(*document));
    auto writer = connection.get_writer();
    const auto sent = co_await writer.send_response(std::move(response));
    require(sent && *sent, "custom spec write");
}

class SpecOnly {
public:
    ApiResult<void> install(PreparedApi& api) {
        if (api.router.find_handler(HttpMethod::GET, "/schema.json").handler) {
            return std::unexpected(ApiError{ApiErrorCode::kRouteConflict, "spec route already exists", 409});
        }
        document = api.document;
        api.router.add_handler<HttpMethod::GET>("/schema.json",
            [shared = document](HttpConn& connection, HttpRequest) {
                return send_spec(shared, connection);
            });
        return {};
    }

    std::shared_ptr<const std::string> document;
};

template<ApiDocsPolicy Policy>
std::shared_ptr<const std::string> install_and_check(Policy& policy, std::string_view spec_path,
                                                    std::string_view ui_path, const std::string& baseline) {
    auto api = prepared();
    const auto snapshot = api.document;
    require(snapshot && *snapshot == baseline, "all policies start from the same deterministic document");
    check_unchanged(api, snapshot, baseline);
    api.router.add_handler<HttpMethod::GET>("/raw", raw);
    api.router.add_handler<HttpMethod::POST>("/raw/:name", raw);
    check_unchanged(api, snapshot, baseline);
    const auto old_size = api.router.size();
    const auto installed = policy.install(api);
    require(installed.has_value(), "install documentation policy");
    check_unchanged(api, snapshot, baseline);
    require(api.router.find_handler(HttpMethod::GET, "/raw").handler &&
            api.router.find_handler(HttpMethod::POST, "/raw/name").handler, "raw routes remain usable but undocumented");
    if (spec_path.empty()) {
        require(api.router.size() == old_size && !api.docs_installed, "NoSwagger is a no-op, not a document-generation toggle");
    } else {
        require(api.router.find_handler(HttpMethod::GET, std::string(spec_path)).handler != nullptr, "configured spec route installed");
        require(api.router.size() > old_size, "documentation policy adds routes");
    }
    if (!ui_path.empty()) {
        require(api.docs_installed && api.router.find_handler(HttpMethod::GET, std::string(ui_path)).handler &&
                api.router.find_handler(HttpMethod::GET, std::string(ui_path) + "/swagger-ui-bundle.js").handler,
                "HttpSwagger installs configured offline UI");
    } else if (!spec_path.empty()) {
        require(api.router.size() == old_size + 1, "custom spec-only policy adds no UI or inferred operations");
    }
    require(!api.router.find_handler(HttpMethod::GET, "/openapi.json").handler &&
            !api.router.find_handler(HttpMethod::GET, "/docs").handler, "no unconfigured default docs routes");
    return snapshot;
}

} // namespace

int main() {
    const std::string baseline = *prepared().document;
    NoSwagger none;
    const auto offline = install_and_check(none, {}, {}, baseline);
    HttpSwagger swagger(DocsConfig{.spec_path = "/reference.json", .ui_path = "/reference"});
    const auto documented = install_and_check(swagger, "/reference.json", "/reference", baseline);
    SpecOnly custom;
    const auto spec_only = install_and_check(custom, "/schema.json", {}, baseline);
    require(custom.document == spec_only, "custom spec handler owns the same immutable shared document");
    require(*offline == baseline && *documented == baseline && *spec_only == baseline,
            "offline documents outlive builders, routers and installed docs handlers");
    check_document(*documented);
    require(std::puts("API policy document invariants passed without Runtime or listeners") >= 0, "write success output");
}
