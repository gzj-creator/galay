#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-http/server/http_server.h>
#include <serde/json/json.hpp>
#include <serde/reflect/reflect_macros.hpp>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace fixture {

struct GetInput { int id{}; std::optional<bool> verbose = true; };
#define GET_FIELDS(X) X(id, "user-id", (reflect::field_options<int>{.minimum = 1})) X(verbose)
REFLECT_FIELDS(GetInput, GET_FIELDS)
#undef GET_FIELDS
struct PostInput { int id{}; std::string title; std::optional<int> retries = 3; };
#define POST_FIELDS(X) X(id) X(title, "display-title", (reflect::field_options<std::string>{.min_length = 1})) X(retries)
REFLECT_FIELDS(PostInput, POST_FIELDS)
#undef POST_FIELDS
struct Output { int id{}; std::string title; bool verbose{}; };
#define OUTPUT_FIELDS(X) X(id) X(title) X(verbose)
REFLECT_FIELDS(Output, OUTPUT_FIELDS)
#undef OUTPUT_FIELDS
struct Number { double value{}; };
#define NUMBER_FIELDS(X) X(value)
REFLECT_FIELDS(Number, NUMBER_FIELDS)
#undef NUMBER_FIELDS
struct Lifetime {
    std::atomic<int> calls{0};
    std::atomic<int> completed{0};
    std::string label = "kept-alive";
};

} // namespace fixture

namespace {

using namespace galay::api;
using namespace galay::http;
using galay::kernel::Task;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << " errno=" << errno << " (" << std::strerror(errno) << ")\n";
    std::exit(1);
}
void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}
void close_fd(int fd) {
    require(::close(fd) == 0, "client socket close");
}

std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "port probe socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "port probe bind");
    socklen_t size = sizeof(address);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "port probe getsockname");
    const auto port = ntohs(address.sin_port);
    close_fd(fd);
    return port;
}

int connect_client(std::uint16_t port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "client socket");
        const timeval timeout{5, 0};
        require(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "recv timeout");
        require(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0, "send timeout");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) return fd;
        const int saved = errno;
        close_fd(fd);
        require(saved == ECONNREFUSED || saved == ETIMEDOUT, "unexpected connect failure");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    fail("server did not listen");
}

struct Response {
    int status{};
    std::string headers;
    std::string body;
};

Response call(std::uint16_t port, std::string_view method, std::string_view target,
              std::string_view body = {}, std::string_view content_type = {}, bool framed = false) {
    const int fd = connect_client(port);
    std::string wire = std::string(method) + " " + std::string(target) +
        " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!content_type.empty()) wire += "Content-Type: " + std::string(content_type) + "\r\n";
    if (!body.empty() || framed) wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    wire += "\r\n";
    wire += body;
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const auto sent = ::send(fd, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        require(sent > 0, "send request");
        offset += static_cast<std::size_t>(sent);
    }
    std::string received;
    char buffer[4096];
    while (true) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "response recv or server-owned close timeout");
        if (count == 0) break;
        received.append(buffer, static_cast<std::size_t>(count));
    }
    close_fd(fd);
    const auto split = received.find("\r\n\r\n");
    require(split != std::string::npos, "complete response header");
    const auto status_start = received.find(' ');
    require(status_start != std::string::npos, "status line");
    Response result;
    const auto converted = std::from_chars(received.data() + status_start + 1,
        received.data() + status_start + 4, result.status);
    require(converted.ec == std::errc{} && converted.ptr == received.data() + status_start + 4, "status code");
    result.headers = received.substr(0, split);
    result.body = received.substr(split + 4);
    return result;
}

void expect_error(const Response& response, int status, std::string_view code) {
    require(response.status == status, "expected error status");
    const auto body = json::parse(response.body);
    require(body && body->is_object() && body->size() == 2, "error envelope has code/message only");
    const auto actual = body->at("code").as_string();
    const auto message = body->at("message").as_string();
    require(actual && *actual == code && message && !message->empty(), "typed error envelope");
}

void test_error_codes() {
    constexpr std::pair<ApiErrorCode, std::string_view> cases[]{
        {ApiErrorCode::kInvalidSchema, "invalid_schema"},
        {ApiErrorCode::kUnsupportedType, "unsupported_type"},
        {ApiErrorCode::kInvalidMetadata, "invalid_metadata"},
        {ApiErrorCode::kInvalidPath, "invalid_path"},
        {ApiErrorCode::kRouteConflict, "route_conflict"},
        {ApiErrorCode::kDuplicateOperation, "duplicate_operation"},
        {ApiErrorCode::kInvalidBinding, "invalid_binding"},
        {ApiErrorCode::kFrozenBuilder, "frozen_builder"},
        {ApiErrorCode::kBadRequest, "bad_request"},
        {ApiErrorCode::kUnsupportedMediaType, "unsupported_media_type"},
        {ApiErrorCode::kBusinessError, "business_error"},
        {ApiErrorCode::kTaskError, "task_error"},
        {ApiErrorCode::kEncodingError, "encoding_error"},
        {ApiErrorCode::kTransportError, "transport_error"},
        {ApiErrorCode::kResourceError, "resource_error"},
        {ApiErrorCode::kServerError, "server_error"},
    };
    for (const auto& [code, name] : cases) {
        require(api_error_name(code) == name, "k-prefixed error codes keep stable names");
        const auto encoded = router_detail::encode_error(ApiError{code, "test error", 500});
        require(encoded.has_value(), "encode renamed API error code");
        const auto body = json::parse(*encoded);
        require(body && body->is_object() && body->size() == 2, "renamed error code envelope");
        const auto actual = body->at("code").as_string();
        require(actual && *actual == name, "JSON error codes keep snake-case names");
    }
    const ApiError default_error;
    require(default_error.code == ApiErrorCode::kBadRequest && default_error.status == 400 &&
            default_error.message.empty(), "default error remains a bad request");
    require(api_error_name(static_cast<ApiErrorCode>(-1)) == "invalid_error_code",
            "unknown error code keeps its diagnostic name");
}

Task<ApiResult<fixture::Output>> get(ApiContext&, fixture::GetInput input) {
    co_return fixture::Output{input.id, "get", input.verbose.value_or(false)};
}
Task<ApiResult<fixture::Output>> post(ApiContext&, fixture::PostInput input) {
    co_return fixture::Output{input.id, std::move(input.title), input.retries == 3};
}
Task<ApiResult<NoContent>> no_content(ApiContext&, NoInput) {
    co_return NoContent{};
}

PreparedApi prepared(std::shared_ptr<fixture::Lifetime> lifetime) {
    ApiBuilder builder(ApiInfo{.title = "P2 loopback", .version = "1.0.0"});
    auto get_binding = InputBinding<fixture::GetInput>{};
    get_binding.path<&fixture::GetInput::id>("id").query<&fixture::GetInput::verbose>("verbose");
    require(builder.add<HttpMethod::GET, fixture::GetInput, fixture::Output>(
        "/users/:id", get, Operation{.id = "getUser"}, get_binding).has_value(), "register GET");
    auto post_binding = InputBinding<fixture::PostInput>{};
    post_binding.path<&fixture::PostInput::id>("id");
    require(builder.add<HttpMethod::POST, fixture::PostInput, fixture::Output>(
        "/users/:id", post, Operation{.id = "postUser", .success_status = 201}, post_binding).has_value(), "register POST");

    // The passed coroutine lambda is temporary. Its closure must stay alive through suspension.
    require(builder.add<HttpMethod::GET, fixture::GetInput, fixture::Output>(
        "/async/:id", [lifetime](ApiContext& context, fixture::GetInput input) -> Task<ApiResult<fixture::Output>> {
            lifetime->calls.fetch_add(1);
            co_yield true;
            co_yield true;
            require(context.request.body_str().empty(), "request survives async handler");
            require(lifetime->label == "kept-alive" && input.id > 0, "closure/input survive async handler");
            lifetime->completed.fetch_add(1);
            co_return fixture::Output{input.id, lifetime->label, input.verbose.value_or(false)};
        }, Operation{.id = "asyncUser"}, get_binding).has_value(), "temporary coroutine lambda");

    require(builder.add<HttpMethod::GET, NoInput, NoContent>(
        "/empty", no_content, Operation{.id = "empty"}).has_value(), "NoContent 200");
    require(builder.add<HttpMethod::GET, NoInput, NoContent>(
        "/no-content", no_content, Operation{.id = "noContent", .success_status = 204}).has_value(), "204");
    require(builder.add<HttpMethod::POST, NoInput, NoContent>(
        "/reset-content", no_content, Operation{.id = "resetContent", .success_status = 205}).has_value(), "205");
    require(builder.add<HttpMethod::HEAD, fixture::GetInput, fixture::Output>(
        "/head/:id", get, Operation{.id = "headUser"}, get_binding).has_value(), "HEAD typed output");
    require(builder.add<HttpMethod::GET, NoInput, fixture::Output>(
        "/business", [](ApiContext&, NoInput) -> Task<ApiResult<fixture::Output>> {
            co_return std::unexpected(ApiError{ApiErrorCode::kBusinessError, "missing \"user\"\n", 404});
        }, Operation{.id = "business", .errors = {{404, "Not found"}}}).has_value(), "business error");
    require(builder.add<HttpMethod::GET, NoInput, fixture::Output>(
        "/undeclared", [](ApiContext&, NoInput) -> Task<ApiResult<fixture::Output>> {
            co_return std::unexpected(ApiError{ApiErrorCode::kBusinessError, "undeclared", 409});
        }, Operation{.id = "undeclared"}).has_value(), "undeclared business error");
    require(builder.add<HttpMethod::GET, NoInput, fixture::Output>(
        "/task-error", [](ApiContext&, NoInput) -> Task<ApiResult<fixture::Output>> { return {}; },
        Operation{.id = "taskError"}).has_value(), "empty Task scheduling failure");
    require(builder.add<HttpMethod::GET, fixture::GetInput, fixture::Output>(
        "/schedule-error/:id", [](ApiContext& context, fixture::GetInput input) {
            auto task = get(context, std::move(input));
            // Inject the awaiter's typed scheduling failure without changing a live scheduler.
            const auto& ref = galay::kernel::detail::TaskAccess::task_ref(task);
            galay::kernel::detail::store_task_error(ref,
                galay::kernel::detail::TaskResultError(galay::kernel::detail::TaskResultErrorCode::kScheduleFailed));
            galay::kernel::detail::complete_task_state(ref);
            return task;
        }, Operation{.id = "scheduleError"}, get_binding).has_value(), "typed scheduling error boundary");
    require(builder.add<HttpMethod::POST, fixture::PostInput, std::string_view>(
        "/borrowed/:id", [](ApiContext& context, fixture::PostInput) -> Task<ApiResult<std::string_view>> {
            co_yield true;
            co_return std::string_view(context.request.body_str());
        }, Operation{.id = "borrowedRequest"}, post_binding).has_value(), "request body borrow through suspension");
    require(builder.add<HttpMethod::GET, NoInput, fixture::Number>(
        "/encoding-error", [](ApiContext&, NoInput) -> Task<ApiResult<fixture::Number>> {
            co_return fixture::Number{std::numeric_limits<double>::infinity()};
        }, Operation{.id = "encodingError"}).has_value(), "encoding error");

    auto result = builder.build();
    require(result.has_value(), "build API");
    const auto frozen_add = builder.add<HttpMethod::GET, NoInput, NoContent>(
        "/later", no_content, Operation{.id = "later"});
    require(!frozen_add && frozen_add.error().code == ApiErrorCode::kFrozenBuilder, "add after build forbidden");
    const auto frozen_build = builder.build();
    require(!frozen_build && frozen_build.error().code == ApiErrorCode::kFrozenBuilder, "repeat build forbidden");
    return std::move(*result);
}

void test_registration_errors() {
    ApiBuilder builder;
    const auto typed_204 = builder.add<HttpMethod::GET, NoInput, fixture::Output>(
        "/typed-204", [](ApiContext&, NoInput) -> Task<ApiResult<fixture::Output>> { co_return fixture::Output{}; },
        Operation{.id = "typed204", .success_status = 204});
    const auto typed_205 = builder.add<HttpMethod::GET, NoInput, fixture::Output>(
        "/typed-205", [](ApiContext&, NoInput) -> Task<ApiResult<fixture::Output>> { co_return fixture::Output{}; },
        Operation{.id = "typed205", .success_status = 205});
    require(!typed_204 && !typed_205, "typed outputs on 204/205 rejected instead of discarded");
    require(builder.add<HttpMethod::GET, NoInput, NoContent>(
        "/same", no_content, Operation{.id = "first"}).has_value(), "initial registration");
    const auto conflict = builder.add<HttpMethod::GET, NoInput, NoContent>(
        "/same", no_content, Operation{.id = "second"});
    require(!conflict && conflict.error().code == ApiErrorCode::kRouteConflict, "duplicate route rejected");
    const auto duplicate_id = builder.add<HttpMethod::GET, NoInput, NoContent>(
        "/other", no_content, Operation{.id = "first"});
    require(!duplicate_id && duplicate_id.error().code == ApiErrorCode::kDuplicateOperation, "duplicate operation rejected");
    const auto built = builder.build();
    require(built && built->endpoints.size() == 1, "failed adds do not leave partial routes");
}

void test_loopback() {
    auto lifetime = std::make_shared<fixture::Lifetime>();
    std::weak_ptr<fixture::Lifetime> weak = lifetime;
    auto api = prepared(lifetime);
    lifetime.reset();
    require(!weak.expired(), "prepared API owns handler after builder destruction");
    const auto document = json::parse(*api.document);
    require(document.has_value(), "document survives builder destruction");
    const auto paths = document->at("paths");
    const auto post_spec = paths.at("/users/{id}").at("post");
    const auto post_schema = post_spec.at("requestBody").at("content").at("application/json").at("schema");
    require(post_schema.at("properties").contains("display-title") && !post_schema.at("properties").contains("id"),
            "OpenAPI mixed body matches execution");
    require(!paths.at("/head/{id}").at("head").at("responses").at("200").contains("content"), "HEAD no content schema");
    require(!paths.at("/head/{id}").at("head").at("responses").at("400").contains("content"), "HEAD errors no content schema");
    require(!paths.at("/no-content").at("get").at("responses").at("204").contains("content"), "204 no content schema");
    require(!paths.at("/reset-content").at("post").at("responses").at("205").contains("content"), "205 no content schema");
    require(!paths.at("/empty").at("get").contains("requestBody"), "NoInput has no request body");

    const auto port = free_port();
    HttpServer server(HttpServerBuilder().host("127.0.0.1").port(port)
        .io_scheduler_count(1).parallel_scheduler_count(1).build());
    server.start(std::move(api.router));
    require(server.is_running(), "HTTP route server starts");

    auto response = call(port, "GET", "/users/7");
    auto output = json::deserialize<fixture::Output>(response.body);
    require(response.status == 200 && output && output->id == 7 && output->verbose, "real GET binding");
    response = call(port, "POST", "/users/8", R"({"display-title":"Ada"})", "application/json");
    output = json::deserialize<fixture::Output>(response.body);
    require(response.status == 201 && output && output->id == 8 && output->title == "Ada" && output->verbose,
            "real mixed POST and missing optional default");
    expect_error(call(port, "GET", "/users/7x"), 400, "bad_request");
    expect_error(call(port, "GET", "/users/0"), 400, "bad_request");
    expect_error(call(port, "GET", "/users/7?verbose=1"), 400, "bad_request");
    expect_error(call(port, "POST", "/users/8", "{}", "application/json"), 400, "bad_request");
    expect_error(call(port, "POST", "/users/8", "{", "application/json"), 400, "bad_request");
    expect_error(call(port, "POST", "/users/8", R"({"display-title":"Ada","id":8})", "application/json"), 400, "bad_request");
    expect_error(call(port, "POST", "/users/8", R"({"display-title":"Ada"})", "text/plain"), 415, "unsupported_media_type");
    expect_error(call(port, "GET", "/empty", "{}", "application/json"), 400, "bad_request");
    expect_error(call(port, "GET", "/business"), 404, "business_error");
    expect_error(call(port, "GET", "/undeclared"), 500, "business_error");
    expect_error(call(port, "GET", "/task-error"), 500, "task_error");
    const auto scheduling = call(port, "GET", "/schedule-error/7");
    expect_error(scheduling, 500, "task_error");
    const auto task_failure = json::parse(scheduling.body);
    require(task_failure.has_value(), "typed scheduling failure JSON");
    const auto reason = task_failure->at("message").as_string();
    require(reason && *reason == galay::kernel::detail::TaskResultError(
        galay::kernel::detail::TaskResultErrorCode::kScheduleFailed).message(), "scheduling cause preserved");
    const auto borrowed = call(port, "POST", "/borrowed/8", R"({"display-title":"Ada"})", "application/json");
    const auto echoed = json::deserialize<std::string>(borrowed.body);
    require(borrowed.status == 200 && echoed && *echoed == R"({"display-title":"Ada"})",
            "ApiContext request remains valid through handler completion and response encoding");
    expect_error(call(port, "GET", "/encoding-error"), 500, "encoding_error");
    for (const auto& [method, path, status] : std::vector<std::tuple<std::string, std::string, int>>{
            {"GET", "/empty", 200}, {"GET", "/no-content", 204}, {"POST", "/reset-content", 205},
            {"HEAD", "/head/7", 200}, {"HEAD", "/head/7?verbose=1", 400}}) {
        const auto empty = call(port, method, path);
        require(empty.status == status && empty.body.empty(), "HEAD/NoContent/204/205 really empty");
        require(empty.headers.find("content-type:") == std::string::npos, "empty response does not advertise JSON body");
    }
    for (int i = 1; i <= 12; ++i) {
        response = call(port, "GET", "/async/" + std::to_string(i));
        output = json::deserialize<fixture::Output>(response.body);
        require(response.status == 200 && output && output->id == i && output->title == "kept-alive", "suspended temporary handler");
    }
    auto observed = weak.lock();
    require(observed && observed->calls == 12 && observed->completed == 12, "async handlers fully completed");
    observed.reset();
    server.stop();
}

} // namespace

int main() {
    test_error_codes();
    test_registration_errors();
    test_loopback();
    std::cout << "P2 real HTTP loopback PASS\n";
}
