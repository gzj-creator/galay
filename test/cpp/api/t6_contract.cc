#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-http/server/http_server.h>
#include <serde/reflect/reflect_macros.hpp>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace fixture {

std::atomic<int> descriptor_mode = 0;
std::atomic<int> enum_mode = 0;

enum class Mode { active, disabled };

constexpr auto reflect_enum(std::type_identity<Mode>) {
    if consteval {
        return reflect::enum_descriptor<Mode, 2>{reflect::enum_encoding::string,
            {{{Mode::active, "active"}, {Mode::disabled, "disabled"}}}};
    } else {
        const auto mode = enum_mode.load(std::memory_order_relaxed);
        return reflect::enum_descriptor<Mode, 2>{
            mode == 2 ? reflect::enum_encoding::underlying : reflect::enum_encoding::string,
            {{{Mode::active, mode == 1 ? "changed" : "active"}, {Mode::disabled, "disabled"}}}};
    }
}

struct DynamicChild {
    int value{};
    int other{};
};

constexpr auto reflect_fields(std::type_identity<DynamicChild>) {
    if consteval {
        return std::make_tuple(reflect::make_field("value", &DynamicChild::value,
            reflect::field_options<int>{.minimum = 0}));
    } else {
        const auto mode = descriptor_mode.load(std::memory_order_relaxed);
        return std::make_tuple(reflect::make_field("value",
            mode == 5 ? &DynamicChild::other : &DynamicChild::value,
            reflect::field_options<int>{.minimum = mode == 6 ? 5 : 0}));
    }
}

inline auto reflect_fields(const DynamicChild& child) {
    const auto mode = descriptor_mode.load(std::memory_order_relaxed);
    return std::make_tuple(reflect::make_field(
        mode == 1 || (mode == 4 && child.value == 9) ? "changed" : "value",
        mode == 2 ? &DynamicChild::other : &DynamicChild::value,
        reflect::field_options<int>{.minimum = mode == 3 ? 5 : 0}));
}

struct DriftOutput {
    DynamicChild child;
};

constexpr auto reflect_fields(std::type_identity<DriftOutput>) {
    return std::make_tuple(reflect::make_field("child", &DriftOutput::child));
}

constexpr auto reflect_fields(const DriftOutput&) {
    return reflect_fields(std::type_identity<DriftOutput>{});
}

struct TopLevelDrift {
    int value{};
};
constexpr auto reflect_fields(std::type_identity<TopLevelDrift>) {
    return std::make_tuple(reflect::make_field("value", &TopLevelDrift::value));
}
inline auto reflect_fields(const TopLevelDrift&) {
    return std::make_tuple(reflect::make_field(
        descriptor_mode.load(std::memory_order_relaxed) == 7 ? "changed" : "value",
        &TopLevelDrift::value));
}

struct NestedInput {
    std::vector<DynamicChild> children;
    std::map<std::string, std::optional<DynamicChild>> attributes;
    std::optional<Mode> mode;
};
#define NESTED_FIELDS(X) X(children) X(attributes) X(mode)
REFLECT_FIELDS(NestedInput, NESTED_FIELDS)
#undef NESTED_FIELDS

struct NullMember { int value{}; };
constexpr auto reflect_fields(std::type_identity<NullMember>) {
    return std::tuple{reflect::make_field("value", static_cast<int NullMember::*>(nullptr))};
}
constexpr auto reflect_fields(const NullMember&) {
    return reflect_fields(std::type_identity<NullMember>{});
}

} // namespace fixture

namespace {

using namespace galay::api;
using namespace galay::http;
using galay::kernel::Task;

void fail(std::string_view message) {
    std::fprintf(stderr, "api.contract: %.*s\n", static_cast<int>(message.size()), message.data());
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "bind");
    socklen_t size = sizeof(address);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname");
    const auto port = ntohs(address.sin_port);
    require(::close(fd) == 0, "close probe");
    return port;
}

std::string request(std::uint16_t port, std::string_view path = "/drift",
                    std::string_view body = {}) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "client socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "connect");
    std::string wire = std::string(body.empty() ? "GET " : "POST ") + std::string(path) +
        " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!body.empty()) wire += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    wire += "\r\n";
    wire += body;
    require(::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(wire.size()), "send");
    std::string response;
    char buffer[4096];
    for (;;) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        require(count >= 0, "recv");
        if (count == 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    require(::close(fd) == 0, "close client");
    return response;
}

Task<ApiResult<fixture::DriftOutput>> handler(ApiContext&, NoInput) {
    co_return fixture::DriftOutput{fixture::DynamicChild{7}};
}

void check_response(const std::string& response, int expected, std::string_view envelope) {
    const auto status = response.find(' ');
    require(status != std::string::npos, "status line");
    int code = 0;
    const auto parsed = std::from_chars(response.data() + status + 1, response.data() + status + 4, code);
    require(parsed.ec == std::errc{} && code == expected, "contract drift must return documented status");
    require(response.find(envelope) != std::string::npos, "contract drift envelope");
}

void test_nested_binding() {
    const auto null_member = validate_contract(fixture::NullMember{});
    require(!null_member && null_member.error().code == ApiErrorCode::kInvalidMetadata,
            "null member pointer rejected before any descriptor access");
    const auto null_binding = InputBinding<fixture::NullMember>{}.prepare(HttpMethod::POST, "/null");
    require(!null_binding && null_binding.error().code == ApiErrorCode::kInvalidBinding,
            "null member pointer explicitly fails input preparation");
    const auto plan = InputBinding<fixture::NestedInput>{}.prepare(HttpMethod::POST, "/nested");
    require(plan.has_value(), "nested static contract accepted");
    HttpRequest req;
    req.setBodyStr(R"({"children":[{"value":7}],"attributes":{"first":{"value":8}},"mode":"active"})");
    require(req.header().headerPairs().addHeaderPair("Content-Type", "application/json") == kNoError, "JSON header");
    const auto decoded = plan->decode(req);
    require(decoded && decoded->children.front().value == 7 && decoded->attributes.at("first")->value == 8,
            "nested codec accepted unchanged descriptor");
    for (int mode = 1; mode <= 3; ++mode) {
        fixture::descriptor_mode = mode;
        const auto rejected = plan->decode(req);
        require(!rejected && rejected.error().code == ApiErrorCode::kInvalidBinding,
                "nested descriptor rejected through containers before binding");
        const auto fixed = validate_contract(std::array<fixture::DynamicChild, 1>{});
        const auto optional = validate_contract(std::optional<fixture::DynamicChild>{std::in_place});
        require(!fixed && !optional, "array/engaged optional descriptor drift rejected");
    }
    fixture::descriptor_mode = 4;
    req.setBodyStr(R"({"children":[{"value":9}],"attributes":{}})");
    const auto after_decode = plan->decode(req);
    require(!after_decode && after_decode.error().code == ApiErrorCode::kInvalidBinding,
            "value-dependent nested descriptor rejected after decode");
    fixture::descriptor_mode = 0;
    for (int mode = 5; mode <= 6; ++mode) {
        fixture::descriptor_mode = mode;
        const auto rejected = schema_for<fixture::DriftOutput>(SchemaUse::output);
        require(!rejected && rejected.error().code == ApiErrorCode::kInvalidMetadata,
                "static/runtime pointer and options divergence rejected during schema generation");
    }
    fixture::descriptor_mode = 0;
    fixture::descriptor_mode = 7;
    const auto top_level = schema_for<fixture::TopLevelDrift>(SchemaUse::output);
    require(!top_level && top_level.error().code == ApiErrorCode::kInvalidMetadata,
            "top-level runtime descriptor drift rejected during schema generation");
    fixture::descriptor_mode = 0;
    for (int mode = 1; mode <= 2; ++mode) {
        fixture::enum_mode = mode;
        require(!validate_contract(std::optional<fixture::Mode>{}), "absent optional still checks enum contract");
        require(!validate_contract(std::vector<fixture::Mode>{}), "empty container still checks enum contract");
        const auto rejected = plan->decode(req);
        require(!rejected && rejected.error().code == ApiErrorCode::kInvalidBinding, "enum drift rejected before binding");
    }
    fixture::enum_mode = 0;
    require(validate_contract(std::vector<bool>{true, false}).has_value(), "vector bool proxy supported");
}

} // namespace

int main() {
    test_nested_binding();
    ApiBuilder builder(ApiInfo{.title = "Contract", .version = "1.0"});
    const auto added = builder.add<HttpMethod::GET, NoInput, fixture::DriftOutput>(
        "/drift", handler, Operation{.id = "drift"});
    require(added.has_value(), "register drift route");
    require(builder.add<HttpMethod::GET, NoInput, std::optional<fixture::Mode>>(
        "/enum", [](ApiContext&, NoInput) -> Task<ApiResult<std::optional<fixture::Mode>>> {
            co_return std::optional<fixture::Mode>{fixture::Mode::active};
        }, Operation{.id = "enumDrift"}).has_value(), "register enum route");
    require(builder.add<HttpMethod::POST, fixture::NestedInput, NoContent>(
        "/nested", [](ApiContext&, fixture::NestedInput) -> Task<ApiResult<NoContent>> {
            co_return NoContent{};
        }, Operation{.id = "nestedDrift"}).has_value(), "register nested input route");
    auto api = builder.build();
    require(api.has_value(), "build drift route");

    const auto port = free_port();
    HttpServer server(HttpServerBuilder().host("127.0.0.1").port(port)
        .ioSchedulerCount(1).parallelSchedulerCount(1).build());
    server.start(std::move(api->router));
    require(server.isRunning(), "start server");
    check_response(request(port), 200, "\"value\":7");
    for (int mode = 1; mode <= 3; ++mode) {
        fixture::descriptor_mode = mode;
        check_response(request(port), 500, "\"code\":\"encoding_error\"");
    }
    fixture::descriptor_mode = 4;
    check_response(request(port, "/nested", R"({"children":[{"value":9}],"attributes":{}})"),
                   500, "\"code\":\"invalid_binding\"");
    fixture::descriptor_mode = 0;
    for (int mode = 1; mode <= 2; ++mode) {
        fixture::enum_mode = mode;
        check_response(request(port, "/enum"), 500, "\"code\":\"encoding_error\"");
    }
    server.stop();
    fixture::enum_mode = 0;
    std::puts("API runtime contract drift rejected");
}
