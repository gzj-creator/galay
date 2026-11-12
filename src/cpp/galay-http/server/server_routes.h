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

// 此基类只共用路由注册；各原生 server 仍独立管理协议处理和生命周期。
/**
 * @brief 原生 server builder 共用的类型化路由注册基类。
 * @tparam EnableDocs 是否在构建时生成并安装 OpenAPI 文档和 Swagger UI。
 * @details 原生 server 继承此类以注册 typed 或普通路由；线程、监听和协议生命周期由原生 server 管理。
 */
template<bool EnableDocs>
class ServerRoutes {
public:
    ServerRoutes() = default;
    ServerRoutes(const ServerRoutes&) = delete;
    ServerRoutes& operator=(const ServerRoutes&) = delete;
    ServerRoutes(ServerRoutes&&) = default;
    ServerRoutes& operator=(ServerRoutes&&) = default;

    /**
     * @brief 注册类型化 API 操作，并从 DTO 契约生成请求绑定和文档 Schema。
     * @tparam Method HTTP 方法。
     * @tparam Input 输入 DTO，或表示无输入的 api::NoInput。
     * @tparam Output 输出 DTO，或表示无响应体的 api::NoContent。
     * @param path 路由模板；`:name` 形式的路径参数必须绑定到 Input 成员。
     * @param handler 接收 ApiContext 和 Input，返回 Task<ApiResult<Output>> 的协程处理器。
     * @param operation OpenAPI 操作信息及已声明的业务错误。
     * @param binding path/query 成员绑定；未绑定字段作为 JSON 请求体。
     * @return 注册成功时为空结果；路径、绑定或操作冲突时返回 ApiError。
     */
    template<HttpMethod Method, class Input, class Output, class Handler>
    api::ApiResult<void> add_api(std::string path, Handler handler, api::Operation operation,
                                 api::InputBinding<Input> binding = {});

    /** @brief 注册由当前连接处理器执行的普通路由；该路由不会生成 OpenAPI 操作。 */
    template<HttpMethod... Methods>
    api::ApiResult<void> add_handler(std::string path, HttpRouteHandler handler)
    {
        return add_handlers<Methods...>(std::move(path), HttpRouteEntry{std::move(handler), {}});
    }

    /** @brief 注册接收完整请求视图的普通路由；该路由不会生成 OpenAPI 操作。 */
    template<HttpMethod... Methods>
    api::ApiResult<void> add_request_handler(std::string path, HttpRequestHandler handler)
    {
        return add_handlers<Methods...>(std::move(path), HttpRouteEntry{{}, std::move(handler)});
    }

    /** @brief 设置生成文档的标题、版本和说明；仅 EnableDocs 模式提供。 */
    void api_info(api::ApiInfo info) requires EnableDocs { docs_.info = std::move(info); }
    /** @brief 使用内嵌 UI 资源并设置 OpenAPI 与 UI 路由路径。 */
    void docs(api::DocsConfig config = {}) requires EnableDocs
    {
        docs_.config = std::move(config);
        docs_.directory.reset();
    }
    /** @brief 从指定目录加载完整 UI 资源；任一资源缺失时 build() 返回错误。 */
    void docs(api::DocsConfig config, std::string directory) requires EnableDocs
    {
        docs_.config = std::move(config);
        docs_.directory = std::move(directory);
    }

    /**
     * @brief 导出当前已登记操作对应的 OpenAPI JSON。
     * @return 文档文本；尚未生成或某项契约无效时返回 ApiError。
     */
    api::ApiResult<std::string> export_openapi() const requires EnableDocs
    {
        return api::render_openapi(docs_.info, docs_.endpoints);
    }

    /** @brief 检查 builder 是否包含路由，或启用了文档路由。 */
    bool has_routes() const noexcept { return !routes_.empty() || EnableDocs; }

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
        if constexpr (EnableDocs) {
            auto document = export_openapi();
            if (!document) return std::unexpected(std::move(document.error()));
            prepared.document = std::make_shared<const std::string>(std::move(*document));
            prepared.endpoints = docs_.endpoints;
        }
        for (const auto& route : routes_) {
            prepared.router.add_route(route.key.method, route.key.path, route.handlers);
        }
        if constexpr (EnableDocs) {
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
    [[no_unique_address]] std::conditional_t<EnableDocs, DocsState, NoDocsState> docs_;
};

} // namespace galay::http::server_detail

#endif
