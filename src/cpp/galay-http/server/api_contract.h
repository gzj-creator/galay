#ifndef GALAY_HTTP_API_CONTRACT_H
#define GALAY_HTTP_API_CONTRACT_H

#include "api_binding.h"
#include "http_router.h"

namespace galay::api {

struct ApiContext {
    // Owned by the route coroutine, including normalized HTTP/2 requests.
    // Borrowed through response encoding only; never retain this reference.
    const http::HttpRequest& request;
};

struct DocsConfig {
    std::string spec_path = "/openapi.json";
    std::string ui_path = "/docs";
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
