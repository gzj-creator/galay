#ifndef GALAY_TEST_API_ROUTE_FIXTURE_H
#define GALAY_TEST_API_ROUTE_FIXTURE_H

#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-http/server/http_server.h>

namespace fixture {

// Unit tests inspect routes and contracts without allocating a native Runtime.
class ContractRoutes : public galay::http::HttpServerBuilder<true> {
public:
    explicit ContractRoutes(galay::api::ApiInfo info = {}) { api_info(std::move(info)); }

    galay::api::ApiResult<galay::api::router_detail::PreparedRoutes> prepare()
    {
        auto prepared = prepare_routes();
        if (prepared) freeze_routes();
        return prepared;
    }
};

} // namespace fixture

#endif
