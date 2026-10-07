#include <galay/cpp/galay-api/api_server.h>
#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-api/docs.h>
#include <serde/json/json.hpp>
#include <serde/reflect/reflect_macros.hpp>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace consumer {
struct Input { std::uint64_t id{}; std::optional<bool> verbose; };
struct Output { std::uint64_t id{}; std::optional<std::string> label; };
#define INPUT_FIELDS(X) X(id) X(verbose)
REFLECT_FIELDS(Input, INPUT_FIELDS)
#undef INPUT_FIELDS
#define OUTPUT_FIELDS(X) X(id) X(label)
REFLECT_FIELDS(Output, OUTPUT_FIELDS)
#undef OUTPUT_FIELDS

galay::kernel::Task<galay::api::ApiResult<Output>> get(galay::api::ApiContext&, Input input) {
    co_return Output{input.id, std::nullopt};
}

galay::kernel::Task<void> raw(galay::http::HttpConn&, galay::http::HttpRequest) {
    co_return;
}

class RejectPolicy {
public:
    explicit RejectPolicy(galay::api::ApiError error)
        : error_(std::make_unique<galay::api::ApiError>(std::move(error))) {}
    RejectPolicy(RejectPolicy&&) = default;
    RejectPolicy& operator=(RejectPolicy&&) = default;
    RejectPolicy(const RejectPolicy&) = delete;
    RejectPolicy& operator=(const RejectPolicy&) = delete;

    galay::api::ApiResult<void> install(galay::api::PreparedApi&) {
        return std::unexpected(*error_);
    }

private:
    std::unique_ptr<galay::api::ApiError> error_;
};

void require(bool condition, std::string_view message) {
    if (condition) return;
    if (std::fprintf(stderr, "installed.api: %.*s\n", static_cast<int>(message.size()), message.data()) < 0) {
        std::exit(2);
    }
    std::exit(1);
}

static_assert(galay::api::ApiDocsPolicy<galay::api::NoSwagger>);
static_assert(galay::api::ApiDocsPolicy<galay::api::HttpSwagger>);
static_assert(galay::api::ApiDocsPolicy<RejectPolicy>);
static_assert(!std::is_copy_constructible_v<galay::api::ApiServer<>>);
static_assert(!std::is_move_constructible_v<galay::api::ApiServer<>>);
static_assert(std::same_as<decltype(&galay::api::ApiServer<>::is_running),
    bool (galay::api::ApiServer<>::*)() const noexcept>);
static_assert(std::same_as<decltype(std::declval<const galay::api::ApiServer<>&>().document()),
    std::shared_ptr<const std::string>>);
} // namespace consumer

int main() {
    using namespace galay::api;
    using consumer::require;
    auto prepared = []() -> ApiResult<PreparedApi> {
        ApiBuilder builder({.title = "Installed API", .version = "1.0.0"});
        auto added = builder.add<galay::http::HttpMethod::GET, consumer::Input, consumer::Output>(
            "/items/:id", consumer::get, Operation{.id = "getItem"},
            InputBinding<consumer::Input>{}.path<&consumer::Input::id>("id")
                .query<&consumer::Input::verbose>("verbose"));
        if (!added) return std::unexpected(added.error());
        return builder.build();
    }();
    require(prepared && prepared->document, "installed typed builder owns its document");
    const auto document = prepared->document;
    const auto bytes = *document;
    const auto parsed = json::parse(*prepared->document);
    require(parsed.has_value(), "installed document parses");
    const auto version = (*parsed)["openapi"].as_string();
    require(version && *version == "3.1.0", "installed OpenAPI version");
    const auto limit = parsed->at("paths").at("/items/{id}").at("get").at("parameters")
        .at(0).at("schema").at("maximum").as_uint64();
    require(limit && *limit == std::numeric_limits<std::uint64_t>::max(), "installed schema keeps exact uint64 limit");
    const auto none = NoSwagger{}.install(*prepared);
    require(none && prepared->document == document && *prepared->document == bytes &&
            !prepared->router.findHandler(galay::http::HttpMethod::GET, "/docs").handler &&
            !prepared->router.findHandler(galay::http::HttpMethod::GET, "/openapi.json").handler,
            "installed default policy preserves offline document without docs routes");
    prepared->router.addHandler<galay::http::HttpMethod::GET>("/healthz", consumer::raw);
    const DocsConfig docs_config{};
    const auto docs = HttpSwagger{docs_config}.install(*prepared);
    require(docs && prepared->document == document && *prepared->document == bytes &&
            prepared->router.findHandler(galay::http::HttpMethod::GET, "/docs").handler &&
            prepared->router.findHandler(galay::http::HttpMethod::GET, "/openapi.json").handler,
            "installed HttpSwagger uses embedded assets without changing the document");
    const auto installed = json::parse(*prepared->document);
    require(installed && (*installed)["paths"].size() == 1 && !(*installed)["paths"].contains("/healthz") &&
            !(*installed)["paths"].contains("/docs") && !(*installed)["paths"].contains("/openapi.json"),
            "installed raw and documentation routes are not typed operations");

    galay::http::HttpServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.io_scheduler_count = 1;
    config.parallel_scheduler_count = 1;
    ApiServer<> default_server(config);
    ApiServer<HttpSwagger> swagger_server(config, HttpSwagger{docs_config});
    require(!default_server.is_running() && !default_server.document() &&
            !swagger_server.is_running() && !swagger_server.document(), "installed ApiServer constructs without starting HTTP");
    default_server.stop();
    swagger_server.stop();

    const ApiError expected{ApiErrorCode::resource_error, "installed consumer deliberately rejects startup", 409};
    ApiServer<consumer::RejectPolicy> rejected_server(config, consumer::RejectPolicy{expected});
    // Policy failure exercises start without constructing Runtime or opening any listener.
    const auto rejected = rejected_server.start(std::move(*prepared));
    require(!rejected && rejected.error().code == expected.code && rejected.error().message == expected.message &&
            rejected.error().status == expected.status && !rejected_server.is_running() && !rejected_server.document(),
            "installed move-only policy failure propagates through ApiServer");
    require(prepared->document == document && *prepared->document == bytes &&
            prepared->router.findHandler(galay::http::HttpMethod::GET, "/items/1").handler,
            "installed rejected policy does not consume typed routes or document");
    rejected_server.stop();
    require(std::puts("Installed API policy consumer passed without Runtime or listeners") >= 0, "write success output");
}
