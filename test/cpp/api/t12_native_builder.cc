#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-http/server/http_server.h>
#ifdef GALAY_HTTP2_FEATURE_ENABLED
#include <galay/cpp/galay-http2/server/http2_server.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <type_traits>

using namespace galay::api;
using namespace galay::http;

namespace {

void require(bool condition, const char* message)
{
    if (condition) return;
    if (std::fprintf(stderr, "%s\n", message) < 0) std::abort();
    std::exit(1);
}

galay::kernel::Task<ApiResult<std::string>> get_value(ApiContext&, NoInput)
{
    co_return "value";
}

// This is the registration shape emitted by a future .api generator.
template<class Builder>
ApiResult<void> register_service(Builder& builder)
{
    return builder.template add_api<HttpMethod::GET, NoInput, std::string>(
        "/value", get_value, Operation{.id = "getValue"});
}

template<class Builder>
concept HasDocs = requires(Builder& builder) { builder.docs(DocsConfig{}); };
template<class Builder>
concept HasDocument = requires(const Builder& builder) { builder.export_openapi(); };

static_assert(!HasDocs<HttpServerBuilder<false>>);
static_assert(HasDocs<HttpServerBuilder<true>>);
static_assert(!HasDocument<HttpServerBuilder<false>>);
static_assert(HasDocument<HttpServerBuilder<true>>);
static_assert(sizeof(server_detail::ServerRoutes<false>) < sizeof(server_detail::ServerRoutes<true>));

galay::kernel::Task<HttpResponseResult> ordinary(HttpRequest)
{
    co_return HttpResponse{};
}

void test_registration()
{
    HttpServerBuilder<> builder;
    require(builder.add_request_handler<HttpMethod::GET, HttpMethod::POST>("/items/:id", ordinary).has_value(),
        "one ordinary registration can serve several methods");
    const auto alias = builder.add_request_handler<HttpMethod::GET>("/items//:id", ordinary);
    require(!alias && alias.error().code == ApiErrorCode::kRouteConflict,
        "native router path aliases cannot silently replace a registered route");
    const auto duplicate = builder.add_request_handler<HttpMethod::PUT, HttpMethod::PUT>("/atomic", ordinary);
    require(!duplicate && builder.add_request_handler<HttpMethod::PUT>("/atomic", ordinary).has_value(),
        "failed multi-method registration leaves no partial routes");
    require(builder.add_request_handler<HttpMethod::GET>("/literal", ordinary).has_value() &&
        builder.add_request_handler<HttpMethod::GET>("/literal/", ordinary).has_value(),
        "native exact paths preserve distinct trailing-slash handlers");
    require(builder.add_request_handler<HttpMethod::GET>("/literal/child", ordinary).has_value() &&
        builder.add_request_handler<HttpMethod::GET>("/literal//child", ordinary).has_value(),
        "native exact paths preserve distinct repeated-slash handlers");
    require(builder.add_request_handler<HttpMethod::GET>("/files/*", ordinary).has_value(),
        "ordinary wildcard route");
    require(builder.add_request_handler<HttpMethod::GET>("/files/exact", ordinary).has_value(),
        "exact routes retain native precedence over wildcard routes");
    require(builder.add_request_handler<HttpMethod::GET>("/files/:name", ordinary).has_value(),
        "parameter routes retain native precedence over wildcard routes");
    require(builder.add_request_handler<HttpMethod::GET>("/owners/:name/*", ordinary).has_value(),
        "ordinary parameter route with a wildcard suffix");
    const auto renamed_wildcard = builder.add_request_handler<HttpMethod::GET>("/owners/:id/*", ordinary);
    require(!renamed_wildcard && renamed_wildcard.error().code == ApiErrorCode::kRouteConflict,
        "renaming a parameter before a wildcard cannot overwrite the same native trie route");
    require(builder.add_request_handler<HttpMethod::GET>("/owners/:name/**", ordinary).has_value(),
        "single and greedy wildcards retain distinct native precedence");
    const auto duplicate_operation = builder.add_api<HttpMethod::GET, NoInput, std::string>(
        "/first", get_value, Operation{.id = "sharedId"});
    require(duplicate_operation.has_value(), "first operation id");
    const auto repeated_operation = builder.add_api<HttpMethod::GET, NoInput, std::string>(
        "/second", get_value, Operation{.id = "sharedId"});
    require(!repeated_operation && repeated_operation.error().code == ApiErrorCode::kDuplicateOperation,
        "operation ids are unique even when Swagger is disabled");
}

template<class Builder>
void test_transport_restriction()
{
    Builder builder;
    builder.io_scheduler_count(1).parallel_scheduler_count(1);
    const auto added = builder.template add_handler<HttpMethod::GET>("/raw",
        [](HttpConn&, HttpRequest) -> galay::kernel::Task<void> { co_return; });
    require(added.has_value(), "register connection-bound route for validation");
    const auto built = builder.build();
    require(!built && built.error().code == ApiErrorCode::kInvalidBinding,
        "TLS and HTTP/2 cannot run plaintext connection-bound handlers");
}

#ifdef GALAY_HTTP2_FEATURE_ENABLED
template<class Builder>
void test_http2_modes()
{
    Builder builder;
    builder.io_scheduler_count(1).parallel_scheduler_count(1);
    require(register_service(builder).has_value(), "typed HTTP/2 route");
    builder.stream_handler([](galay::http2::Http2Stream::ptr) -> galay::kernel::Task<void> { co_return; });
    const auto conflict = builder.build();
    require(!conflict && conflict.error().code == ApiErrorCode::kInvalidBinding,
        "native stream handlers cannot replace builder routes");
    builder.stream_handler({});
    require(builder.build().has_value(), "corrected HTTP/2 mode can retry the same routes");
}
#endif

} // namespace

int main()
{
    test_registration();
#ifdef GALAY_SSL_FEATURE_ENABLED
    test_transport_restriction<HttpsServerBuilder<>>();
#endif
#ifdef GALAY_HTTP2_FEATURE_ENABLED
    test_transport_restriction<galay::http2::H2cServerBuilder<>>();
    test_http2_modes<galay::http2::H2cServerBuilder<>>();
#ifdef GALAY_SSL_FEATURE_ENABLED
    test_transport_restriction<galay::http2::H2ServerBuilder<>>();
    test_http2_modes<galay::http2::H2ServerBuilder<>>();
#endif
#endif
    HttpServerBuilder<> hidden;
    hidden.io_scheduler_count(1).parallel_scheduler_count(1);
    require(register_service(hidden).has_value(), "register with Swagger disabled");
    auto server = hidden.build();
    require(server.has_value() && !(*server)->is_running(), "build does not listen");
    require(!register_service(hidden) && !hidden.build(), "successful build freezes registration");

    HttpServerBuilder<true> visible;
    visible.api_info({.title = "Generated service", .version = "1"});
    require(register_service(visible).has_value(), "same generated registration with Swagger enabled");
    auto document = visible.export_openapi();
    require(document && document->find("getValue") != std::string::npos, "offline document");
    require(visible.add_request_handler<HttpMethod::GET>("/docs",
        [](HttpRequest) -> galay::kernel::Task<HttpResponseResult> { co_return HttpResponse{}; }).has_value(),
        "register ordinary route");
    auto conflicting = visible.build();
    require(!conflicting && conflicting.error().code == ApiErrorCode::kRouteConflict,
        "ordinary and docs route conflict fails before creating server");
    visible.docs({.spec_path = "/schema.json", .ui_path = "/reference"});
    require(visible.build().has_value(), "retry build after correcting docs configuration");

    HttpServerBuilder<true> missing;
    missing.docs({}, "/definitely-missing-galay-assets");
    auto failed = missing.build();
    require(!failed && failed.error().code == ApiErrorCode::kResourceError,
        "explicit missing assets never fall back");
}
