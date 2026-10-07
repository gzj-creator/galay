#ifndef GALAY_API_SERVER_H
#define GALAY_API_SERVER_H

#include "docs.h"
#include <galay/cpp/galay-http/server/http_server.h>

#include <memory>
#include <optional>
#include <utility>

namespace galay::api {

template<ApiDocsPolicy Policy = NoSwagger>
class ApiServer {
public:
    explicit ApiServer(http::HttpServerConfig config = {}, Policy policy = Policy{})
        : policy_(std::move(policy)), config_(std::move(config)) {}

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;
    ApiServer(ApiServer&&) = delete;
    ApiServer& operator=(ApiServer&&) = delete;

    ApiResult<void> start(PreparedApi&& prepared)
    {
        if (server_) {
            return std::unexpected(ApiError{ApiErrorCode::kServerError,
                "API server has already attempted to start", 409});
        }
        if (!prepared.document || prepared.document->empty()) {
            return std::unexpected(ApiError{ApiErrorCode::kResourceError,
                "PreparedApi must own a nonempty OpenAPI document", 500});
        }

        auto installed = policy_.install(prepared);
        if (!installed) return std::unexpected(std::move(installed.error()));

        document_ = prepared.document;
        server_.emplace(config_);
        server_->start(std::move(prepared.router));
        if (!server_->isRunning()) {
            server_->stop();
            return std::unexpected(ApiError{ApiErrorCode::kTransportError,
                "HTTP/1 server failed to start on " + config_.host + ":" + std::to_string(config_.port), 500});
        }
        return {};
    }

    void stop()
    {
        if (server_) server_->stop();
    }

    bool is_running() const noexcept
    {
        return server_.has_value() && server_->isRunning();
    }

    std::shared_ptr<const std::string> document() const noexcept
    {
        return document_;
    }

private:
    // Destroy HTTP routes and suspended Tasks before releasing their policy.
    Policy policy_;
    http::HttpServerConfig config_;
    std::shared_ptr<const std::string> document_;
    std::optional<http::HttpServer> server_;
};

} // namespace galay::api

#endif
