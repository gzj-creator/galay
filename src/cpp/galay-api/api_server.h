#ifndef GALAY_API_SERVER_H
#define GALAY_API_SERVER_H

#include "docs.h"
#include <galay/cpp/galay-http/server/http_server.h>
#ifdef GALAY_API_HTTP2_FEATURE_ENABLED
#include "http2_adapter.h"
#include <galay/cpp/galay-http2/server/http2_server.h>
#endif

#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace galay::api {

using ApiServerConfig = std::variant<http::HttpServerConfig
#ifdef GALAY_SSL_FEATURE_ENABLED
    , http::HttpsServerConfig
#endif
#ifdef GALAY_API_HTTP2_FEATURE_ENABLED
    , http2::H2cServerConfig
#ifdef GALAY_SSL_FEATURE_ENABLED
    , http2::H2ServerConfig
#endif
#endif
    >;

template<ApiDocsPolicy Policy = NoSwagger>
class ApiServer {
public:
    explicit ApiServer(ApiServerConfig config = http::HttpServerConfig{}, Policy policy = Policy{})
        : policy_(std::move(policy)), config_(std::move(config)) {}

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;
    ApiServer(ApiServer&&) = delete;
    ApiServer& operator=(ApiServer&&) = delete;

    ApiResult<void> start(PreparedApi&& prepared)
    {
        if (!std::holds_alternative<std::monostate>(server_)) {
            return std::unexpected(ApiError{ApiErrorCode::kServerError,
                "API server has already attempted to start", 409});
        }

        const auto checked = std::visit([&](const auto& config) -> ApiResult<void> {
            using Config = std::remove_cvref_t<decltype(config)>;
            if constexpr (!std::same_as<Config, http::HttpServerConfig>) {
                if (prepared.router.has_connection_handlers()) {
                    return std::unexpected(ApiError{ApiErrorCode::kInvalidBinding,
                        "HTTPS/HTTP2 API routes must use transport-independent request handlers", 500});
                }
            }
#ifdef GALAY_API_HTTP2_FEATURE_ENABLED
            if constexpr (std::same_as<Config, http2::H2cServerConfig>
#ifdef GALAY_SSL_FEATURE_ENABLED
                          || std::same_as<Config, http2::H2ServerConfig>
#endif
            ) {
                if (config.stream_handler || config.active_conn_handler || !config.static_routes.empty() || !config.static_file_mounts.empty()) {
                    return std::unexpected(ApiError{ApiErrorCode::kInvalidBinding,
                        "ApiServer owns HTTP/2 stream routing; native handlers and static mounts cannot override the API", 500});
                }
            }
#endif
            return {};
        }, config_);
        if (!checked) return checked;
        if (!prepared.document || prepared.document->empty()) {
            return std::unexpected(ApiError{ApiErrorCode::kResourceError,
                "PreparedApi must own a nonempty OpenAPI document", 500});
        }

        auto installed = policy_.install(prepared);
        if (!installed) return std::unexpected(std::move(installed.error()));

        document_ = prepared.document;
        const auto start_native = [&]<class Native, class Config>(std::in_place_type_t<Native>, const Config& config) -> ApiResult<void> {
            auto& native = server_.template emplace<std::unique_ptr<Native>>(std::make_unique<Native>(config));
            if constexpr (std::same_as<Native, http::HttpServer>
#ifdef GALAY_SSL_FEATURE_ENABLED
                          || std::same_as<Native, http::HttpsServer>
#endif
            ) {
                if constexpr (!std::same_as<Native, http::HttpServer>) {
                    if (prepared.router.has_connection_handlers()) {
                        native.reset();
                        return std::unexpected(ApiError{ApiErrorCode::kInvalidBinding, "docs policy added a connection-bound handler", 500});
                    }
                }
                native->start(std::move(prepared.router));
            }
#ifdef GALAY_API_HTTP2_FEATURE_ENABLED
            else {
                if (prepared.router.has_connection_handlers()) {
                    native.reset();
                    return std::unexpected(ApiError{ApiErrorCode::kInvalidBinding, "docs policy added a connection-bound handler", 500});
                }
                auto router = std::make_shared<http::HttpRouter>(std::move(prepared.router));
                native->start(http2::Http2ConnectionHandler{[router = std::move(router)](http2::Http2Stream::ptr stream) {
                    return server_detail::execute_http2_route(router, std::move(stream));
                }});
            }
#endif
            if (!native->is_running()) {
                const auto reason = native->start_error();
                native->stop();
                native.reset();
                return std::unexpected(ApiError{ApiErrorCode::kTransportError,
                    "API transport failed to start on " + config.host + ":" + std::to_string(config.port) + ": " + reason, 500});
            }
            return {};
        };
        return std::visit([&](const auto& config) -> ApiResult<void> {
            using Config = std::remove_cvref_t<decltype(config)>;
            if constexpr (std::same_as<Config, http::HttpServerConfig>) return start_native(std::in_place_type<http::HttpServer>, config);
#ifdef GALAY_SSL_FEATURE_ENABLED
            else if constexpr (std::same_as<Config, http::HttpsServerConfig>) return start_native(std::in_place_type<http::HttpsServer>, config);
#endif
#ifdef GALAY_API_HTTP2_FEATURE_ENABLED
            else if constexpr (std::same_as<Config, http2::H2cServerConfig>) return start_native(std::in_place_type<http2::H2cServer>, config);
#ifdef GALAY_SSL_FEATURE_ENABLED
            else return start_native(std::in_place_type<http2::H2Server>, config);
#endif
#endif
        }, config_);
    }

    void stop()
    {
        std::visit([](auto& native) {
            if constexpr (!std::same_as<std::remove_cvref_t<decltype(native)>, std::monostate>) {
                if (native) { native->stop(); native.reset(); }
            }
        }, server_);
    }

    bool is_running() const noexcept
    {
        return std::visit([](const auto& native) {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(native)>, std::monostate>) return false;
            else return native && native->is_running();
        }, server_);
    }

    std::shared_ptr<const std::string> document() const noexcept
    {
        return document_;
    }

private:
    // Destroy HTTP routes and suspended Tasks before releasing their policy.
    Policy policy_;
    ApiServerConfig config_;
    std::shared_ptr<const std::string> document_;
    std::variant<std::monostate, std::unique_ptr<http::HttpServer>
#ifdef GALAY_SSL_FEATURE_ENABLED
        , std::unique_ptr<http::HttpsServer>
#endif
#ifdef GALAY_API_HTTP2_FEATURE_ENABLED
        , std::unique_ptr<http2::H2cServer>
#ifdef GALAY_SSL_FEATURE_ENABLED
        , std::unique_ptr<http2::H2Server>
#endif
#endif
        > server_;
};

} // namespace galay::api

#endif
