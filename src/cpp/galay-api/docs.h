#ifndef GALAY_API_DOCS_H
#define GALAY_API_DOCS_H

#include "api_contract.h"

#include <concepts>
#include <type_traits>
#include <utility>

namespace galay::api {

struct DocsConfig {
    std::string spec_path = "/openapi.json";
    std::string ui_path = "/docs";
};

ApiResult<void> install_docs(PreparedApi& api, const DocsConfig& config = {});
ApiResult<void> install_docs_from_directory(PreparedApi& api, const DocsConfig& config,
                                          const std::string& directory);

struct NoSwagger {
    ApiResult<void> install(PreparedApi&) const noexcept { return {}; }
};

class HttpSwagger {
public:
    explicit HttpSwagger(DocsConfig config = {}) : config_(std::move(config)) {}

    ApiResult<void> install(PreparedApi& api) const
    {
        return install_docs(api, config_);
    }

private:
    DocsConfig config_;
};

template<class Policy>
concept ApiDocsPolicy = std::is_object_v<Policy> &&
    std::move_constructible<Policy> && requires(Policy& policy, PreparedApi& api) {
        { policy.install(api) } -> std::same_as<ApiResult<void>>;
    };

} // namespace galay::api

#endif
