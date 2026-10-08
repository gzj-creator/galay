#ifndef GALAY_API_CONTRACT_H
#define GALAY_API_CONTRACT_H

#include "binding_contract.h"
#include <galay/cpp/galay-http/server/http_router.h>

namespace galay::api {

struct ApiContext {
    // Owned by the route coroutine, including normalized HTTP/2 requests.
    // Borrowed through response encoding only; never retain this reference.
    const http::HttpRequest& request;
};

struct PreparedApi {
    http::HttpRouter router;
    std::shared_ptr<const std::string> document;
    std::vector<EndpointSpec> endpoints;
    bool docs_installed = false;
};

class ApiBuilder {
public:
    explicit ApiBuilder(ApiInfo info = {}) : info_(std::move(info)) {}
    ApiBuilder(const ApiBuilder&) = delete;
    ApiBuilder& operator=(const ApiBuilder&) = delete;

    template<http::HttpMethod Method, class Input, class Output, class Handler>
    ApiResult<void> add(std::string path, Handler handler, Operation operation,
                        InputBinding<Input> binding = {});
    ApiResult<PreparedApi> build();

private:
    struct RegisteredRoute {
        http::HttpMethod method;
        std::string path;
        http::HttpRequestHandler handler;
    };
    ApiInfo info_;
    bool built_ = false;
    std::vector<EndpointSpec> endpoints_;
    std::vector<RegisteredRoute> routes_;
};

} // namespace galay::api

#endif
