/**
 * @file api_contract.h
 * @brief 类型化 API handler 的请求上下文与文档路由配置。
 * @details 这些声明可供原生 server builder 使用；OpenAPI 编码和 UI 服务由 galay-doc 提供。
 */

#ifndef GALAY_HTTP_API_CONTRACT_H
#define GALAY_HTTP_API_CONTRACT_H

#include "api_binding.h"
#include "http_router.h"

namespace galay::api {

/** @brief 类型化 API handler 当前正在处理的请求上下文。 */
struct ApiContext {
    /**
     * @brief 当前请求的只读视图。
     * @details 请求对象由路由协程持有，HTTP/2 请求也会先归一化为该视图；引用仅在
     *          handler 执行和响应编码期间有效，不得保存到任务结束之后。
     */
    const http::HttpRequest& request;
};

/** @brief OpenAPI JSON 与 Swagger UI 的挂载路径配置。 */
struct DocsConfig {
    std::string spec_path = "/openapi.json"; ///< OpenAPI JSON 路由。
    std::string ui_path = "/docs"; ///< Swagger UI 首页和资源路由前缀。
};

namespace router_detail {

struct PreparedRoutes {
    http::HttpRouter router;
    std::shared_ptr<const std::string> document;
    std::vector<EndpointSpec> endpoints;
    bool docs_installed = false;
};

ApiResult<void> install_docs(PreparedRoutes& api, const DocsConfig& config = {});
ApiResult<void> install_docs_from_directory(PreparedRoutes& api, const DocsConfig& config,
                                          const std::string& directory);

} // namespace router_detail

} // namespace galay::api

#endif
