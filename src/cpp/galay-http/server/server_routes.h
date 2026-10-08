#ifndef GALAY_HTTP_SERVER_ROUTES_H
#define GALAY_HTTP_SERVER_ROUTES_H

#include "http_router.h"
#include "api_contract.h"

#include <span>
#include <array>
#include <type_traits>

namespace galay::http::server_detail {

struct RouteKey {
    HttpMethod method;
    std::string path;
    std::string operation_id;
};

struct RegisteredRoute {
    RouteKey key;
    HttpRouteEntry handlers;
};

api::ApiResult<void> check_registration(const RouteKey& route, std::span<const RegisteredRoute> existing);

struct NoDocsState {};
struct DocsState {
    api::ApiInfo info;
    api::DocsConfig config;
    std::optional<std::string> directory;
    std::vector<api::EndpointSpec> endpoints;
};

// Shared registration only; native servers retain their own protocol and lifecycle.
template<bool EnableSwagger>
class ServerRoutes {
public:
    ServerRoutes() = default;
    ServerRoutes(const ServerRoutes&) = delete;
    ServerRoutes& operator=(const ServerRoutes&) = delete;
    ServerRoutes(ServerRoutes&&) = default;
    ServerRoutes& operator=(ServerRoutes&&) = default;

    template<HttpMethod Method, class Input, class Output, class Handler>
    api::ApiResult<void> add_api(std::string path, Handler handler, api::Operation operation,
                                 api::InputBinding<Input> binding = {});

    template<HttpMethod... Methods>
    api::ApiResult<void> add_handler(std::string path, HttpRouteHandler handler)
    {
        return add_handlers<Methods...>(std::move(path), HttpRouteEntry{std::move(handler), {}});
    }

    template<HttpMethod... Methods>
    api::ApiResult<void> add_request_handler(std::string path, HttpRequestHandler handler)
    {
        return add_handlers<Methods...>(std::move(path), HttpRouteEntry{{}, std::move(handler)});
    }

    void api_info(api::ApiInfo info) requires EnableSwagger { docs_.info = std::move(info); }
    void docs(api::DocsConfig config = {}) requires EnableSwagger
    {
        docs_.config = std::move(config);
        docs_.directory.reset();
    }
    void docs(api::DocsConfig config, std::string directory) requires EnableSwagger
    {
        docs_.config = std::move(config);
        docs_.directory = std::move(directory);
    }

    api::ApiResult<std::string> export_openapi() const requires EnableSwagger
    {
        return api::render_openapi(docs_.info, docs_.endpoints);
    }

    bool has_routes() const noexcept { return !routes_.empty() || EnableSwagger; }

protected:
    template<class Native, class Config>
    api::ApiResult<std::unique_ptr<Native>> build_native(const Config& config, bool connection_handlers)
    {
        if constexpr (requires { config.stream_handler; }) {
            if (has_routes() && (config.stream_handler || config.active_conn_handler ||
                !config.static_routes.empty() || !config.static_file_mounts.empty())) {
                return std::unexpected(api::ApiError{api::ApiErrorCode::kInvalidBinding,
                    "request routes cannot be combined with native HTTP/2 handlers or static mounts", 500});
            }
        }
        auto prepared = prepare_routes();
        if (!prepared) return std::unexpected(std::move(prepared.error()));
        if (!connection_handlers && prepared->router.has_connection_handlers()) {
            return std::unexpected(api::ApiError{api::ApiErrorCode::kInvalidBinding,
                "HTTPS/HTTP2 routes require transport-independent request handlers", 500});
        }
        std::unique_ptr<Native> server;
        if constexpr (requires { config.stream_handler; }) {
            server = has_routes() ? std::make_unique<Native>(config, std::move(prepared->router))
                                  : std::make_unique<Native>(config);
        } else {
            server = std::make_unique<Native>(config, std::move(prepared->router));
        }
        freeze_routes();
        return server;
    }

    api::ApiResult<api::router_detail::PreparedRoutes> prepare_routes()
    {
        if (built_) return std::unexpected(api::ApiError{api::ApiErrorCode::kFrozenBuilder,
            "server builder has already been built", 500});
        api::router_detail::PreparedRoutes prepared;
        if constexpr (EnableSwagger) {
            auto document = export_openapi();
            if (!document) return std::unexpected(std::move(document.error()));
            prepared.document = std::make_shared<const std::string>(std::move(*document));
            prepared.endpoints = docs_.endpoints;
        }
        for (const auto& route : routes_) {
            prepared.router.add_route(route.key.method, route.key.path, route.handlers);
        }
        if constexpr (EnableSwagger) {
            auto installed = docs_.directory
                ? api::router_detail::install_docs_from_directory(prepared, docs_.config, *docs_.directory)
                : api::router_detail::install_docs(prepared, docs_.config);
            if (!installed) return std::unexpected(std::move(installed.error()));
        }
        return prepared;
    }

    void freeze_routes()
    {
        built_ = true;
        routes_.clear();
    }

private:
    template<HttpMethod... Methods>
    api::ApiResult<void> add_handlers(std::string path, HttpRouteEntry handler)
    {
        static_assert(sizeof...(Methods) > 0);
        if (!handler.handler && !handler.request_handler) {
            return std::unexpected(api::ApiError{api::ApiErrorCode::kInvalidBinding, "empty route handler", 500});
        }
        constexpr std::array methods{Methods...};
        for (std::size_t index = 0; index < methods.size(); ++index) {
            if (auto checked = check_route(RouteKey{methods[index], path, {}}); !checked) return checked;
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (methods[earlier] == methods[index]) {
                    return std::unexpected(api::ApiError{api::ApiErrorCode::kRouteConflict,
                        "duplicate method in one registration: " + path, 409});
                }
            }
        }
        for (const auto method : methods) routes_.push_back({{method, path, {}}, handler});
        return {};
    }

    api::ApiResult<void> check_route(const RouteKey& key) const
    {
        if (built_) return std::unexpected(api::ApiError{api::ApiErrorCode::kFrozenBuilder,
            "server builder has already been built", 500});
        std::string error;
        if (!HttpRouter::validate_path(key.path, error)) {
            return std::unexpected(api::ApiError{api::ApiErrorCode::kInvalidPath, std::move(error), 400});
        }
        return check_registration(key, routes_);
    }

    bool built_ = false;
    std::vector<RegisteredRoute> routes_;
    [[no_unique_address]] std::conditional_t<EnableSwagger, DocsState, NoDocsState> docs_;
};

} // namespace galay::http::server_detail

#endif
