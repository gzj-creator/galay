/**
 * @file rpc_managed_client.h
 * @brief RPC托管客户端
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供服务发现快照、round-robin端点选择、连接池租约和一元调用封装。
 *          当前版本不实现Phase 6 retry/governance/backpressure；端点失败仅影响下一次
 *          选择，错误通过RpcError显式返回。
 */

#ifndef GALAY_RPC_MANAGED_CLIENT_H
#define GALAY_RPC_MANAGED_CLIENT_H

#include "rpc_call.h"
#include "rpc_client.h"
#include "rpc_connection_pool.h"
#include "rpc_policy.h"
#include "../protoc/rpc_error.h"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace galay::rpc
{

using RpcEndpointList = std::vector<RpcEndpoint>;
using RpcManagedCallResult = RpcCallResult;

/**
 * @brief 静态服务发现快照
 *
 * @details 用于本地配置和测试的无外部依赖发现实现。调用方可按服务名设置
 *          endpoint列表，resolve返回当前快照副本。
 */
class RpcStaticDiscovery {
public:
    /// @brief 设置服务endpoint快照
    void set(std::string service, RpcEndpointList endpoints) {
        m_services[std::move(service)] = std::move(endpoints);
    }

    /// @brief 解析服务endpoint快照
    std::expected<RpcEndpointList, RpcError> resolve(const std::string& service) const {
        auto it = m_services.find(service);
        if (it == m_services.end() || it->second.empty()) {
            return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE,
                                            "RPC service has no discovered endpoints"));
        }
        return it->second;
    }

private:
    std::unordered_map<std::string, RpcEndpointList> m_services;
};

/**
 * @brief 托管客户端配置
 */
struct RpcManagedClientConfig {
    RpcConnectionPoolConfig pool;  ///< endpoint连接池配置
    RpcClientConfig client;        ///< 每次真实调用使用的RpcClient配置
    RpcRetryPolicy retry;          ///< 调用重试策略，默认不重试
    RpcGovernancePolicy governance;  ///< 限流和熔断策略，默认关闭
};

/**
 * @brief RPC托管客户端
 *
 * @details Discovery类型只需提供
 *          `std::expected<std::vector<RpcEndpoint>, RpcError> resolve(const std::string&)`。
 *          call()在Task路径中只通过连接池waiter和现有RpcClient I/O挂起，不使用阻塞锁。
 */
template<typename Discovery = RpcStaticDiscovery>
class RpcManagedClientImpl {
public:
    explicit RpcManagedClientImpl(Discovery& discovery, RpcManagedClientConfig config = {})
        : m_discovery(&discovery)
        , m_config(std::move(config))
        , m_pool(m_config.pool)
        , m_governance(m_config.governance)
    {
    }

    RpcManagedClientImpl(const RpcManagedClientImpl&) = delete;
    RpcManagedClientImpl& operator=(const RpcManagedClientImpl&) = delete;
    RpcManagedClientImpl(RpcManagedClientImpl&&) = delete;
    RpcManagedClientImpl& operator=(RpcManagedClientImpl&&) = delete;

    /**
     * @brief 刷新并缓存服务发现快照
     * @param service 服务名
     * @return endpoint快照或发现错误
     */
    std::expected<RpcEndpointList, RpcError> refresh(const std::string& service) {
        auto endpoints = m_discovery->resolve(service);
        if (!endpoints.has_value()) {
            return std::unexpected(endpoints.error());
        }
        auto& snapshot = m_services[service];
        snapshot.endpoints = endpoints.value();
        snapshot.next_index = 0;
        for (const auto& endpoint : snapshot.endpoints) {
            auto ensure_result = m_pool.ensure_endpoint(endpoint);
            if (!ensure_result.has_value()) {
                return std::unexpected(ensure_result.error());
            }
        }
        return snapshot.endpoints;
    }

    /**
     * @brief 按round-robin选择下一个可用endpoint
     * @param service 服务名
     * @return endpoint或UNAVAILABLE
     */
    std::expected<RpcEndpoint, RpcError> select_endpoint(const std::string& service) {
        auto snapshot_result = ensure_snapshot(service);
        if (!snapshot_result.has_value()) {
            return std::unexpected(snapshot_result.error());
        }
        auto& snapshot = *snapshot_result.value();
        if (snapshot.endpoints.empty()) {
            return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE,
                                            "RPC service has no endpoints"));
        }

        const size_t count = snapshot.endpoints.size();
        for (size_t attempt = 0; attempt < count; ++attempt) {
            const size_t index = snapshot.next_index % count;
            snapshot.next_index = (snapshot.next_index + 1) % count;
            const auto& endpoint = snapshot.endpoints[index];
            if (!m_unavailable_endpoints.contains(endpoint.key())) {
                return endpoint;
            }
        }

        for (const auto& endpoint : snapshot.endpoints) {
            m_unavailable_endpoints.erase(endpoint.key());
        }
        for (size_t attempt = 0; attempt < count; ++attempt) {
            const size_t index = snapshot.next_index % count;
            snapshot.next_index = (snapshot.next_index + 1) % count;
            const auto& endpoint = snapshot.endpoints[index];
            if (!m_unavailable_endpoints.contains(endpoint.key())) {
                return endpoint;
            }
        }
        return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE,
                                        "RPC service has no allowed endpoints"));
    }

    /**
     * @brief 标记endpoint暂不可用
     * @param endpoint 失败endpoint
     */
    void mark_endpoint_unavailable(const RpcEndpoint& endpoint) {
        m_unavailable_endpoints.insert(endpoint.key());
    }

    /**
     * @brief 清除endpoint不可用标记
     * @param endpoint endpoint
     */
    void mark_endpoint_available(const RpcEndpoint& endpoint) {
        m_unavailable_endpoints.erase(endpoint.key());
    }

    /**
     * @brief 托管一元RPC调用
     * @param service 服务名
     * @param method 方法名
     * @param payload payload指针，可为空
     * @param payload_len payload长度
     * @param options 调用选项
     * @return RPC响应或错误
     *
     * @note 本阶段不重试同一次调用；如果选中endpoint连接/调用失败，会标记该endpoint，
     *       下一次select/call选择下一个允许的endpoint。
     */
    Task<RpcManagedCallResult> call(const std::string& service,
                                    const std::string& method,
                                    const char* payload,
                                    size_t payload_len,
                                    const RpcCallOptions& options = {}) {
        return call_owned(std::string(service),
                         std::string(method),
                         copy_payload(payload, payload_len),
                         options);
    }

    /// @brief 字符串payload一元调用
    Task<RpcManagedCallResult> call(const std::string& service,
                                    const std::string& method,
                                    const std::string& payload,
                                    const RpcCallOptions& options = {}) {
        return call(service, method, payload.data(), payload.size(), options);
    }

    /// @brief 无payload一元调用
    Task<RpcManagedCallResult> call(const std::string& service,
                                    const std::string& method,
                                    const RpcCallOptions& options) {
        return call(service, method, nullptr, 0, options);
    }

    /// @brief 关闭连接池并唤醒等待者
    std::expected<void, RpcError> shutdown() {
        return m_pool.shutdown();
    }

    /// @brief 暴露连接池统计，供测试和诊断使用
    const RpcConnectionPool& pool() const { return m_pool; }

private:
    struct ServiceSnapshot {
        RpcEndpointList endpoints;
        size_t next_index = 0;
    };

    static std::vector<char> copy_payload(const char* payload, size_t payload_len) {
        if (payload == nullptr || payload_len == 0) {
            return {};
        }
        return std::vector<char>(payload, payload + payload_len);
    }

    Task<RpcManagedCallResult> call_owned(std::string service,
                                         std::string method,
                                         std::vector<char> payload,
                                         RpcCallOptions options) {
        auto guarded = m_governance.try_acquire();
        if (!guarded.has_value()) {
            co_return RpcManagedCallResult(std::unexpected(guarded.error()));
        }

        RpcCallOptions retry_options = options;
        auto operation = [this,
                          service = std::move(service),
                          method = std::move(method),
                          payload = std::move(payload),
                          options = std::move(options)]() mutable {
            const char* payload_data = payload.empty() ? nullptr : payload.data();
            return call_once(service, method, payload_data, payload.size(), options);
        };
        auto retry_task_result = co_await RpcRetryController::run_async<RpcManagedCallResult>(
            m_config.retry,
            retry_options,
            operation);
        if (!retry_task_result.has_value()) {
            m_governance.on_failure();
            m_governance.release();
            co_return RpcManagedCallResult(std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, retry_task_result.error().message())));
        }
        auto result = std::move(retry_task_result.value());

        if (result.has_value()) {
            m_governance.on_success();
        } else {
            m_governance.on_failure();
        }
        m_governance.release();
        co_return std::move(result);
    }

    static bool is_endpoint_failure(const RpcError& error) {
        return error.code() == RpcErrorCode::CONNECTION_CLOSED ||
               error.code() == RpcErrorCode::UNAVAILABLE ||
               error.code() == RpcErrorCode::INTERNAL_ERROR;
    }

    static RpcError combine_cleanup_error(const RpcError& primary, const RpcError& cleanup) {
        return RpcError(RpcErrorCode::INTERNAL_ERROR,
                        primary.to_string() + "; cleanup failed: " + cleanup.to_string());
    }

    Task<RpcManagedCallResult> call_once(const std::string& service,
                                        const std::string& method,
                                        const char* payload,
                                        size_t payload_len,
                                        const RpcCallOptions& options) {
        auto snapshot_result = ensure_snapshot(service);
        if (!snapshot_result.has_value()) {
            co_return RpcManagedCallResult(std::unexpected(snapshot_result.error()));
        }
        const size_t attempts = std::max<size_t>(1, snapshot_result.value()->endpoints.size());
        RpcError last_error(RpcErrorCode::UNAVAILABLE, "RPC service has no allowed endpoints");

        for (size_t attempt = 0; attempt < attempts; ++attempt) {
            auto selected = select_endpoint(service);
            if (!selected.has_value()) {
                co_return RpcManagedCallResult(std::unexpected(selected.error()));
            }

            auto endpoint_task_result = co_await call_endpoint(*selected, service, method, payload, payload_len, options);
            if (!endpoint_task_result.has_value()) {
                co_return RpcManagedCallResult(std::unexpected(
                    RpcError(RpcErrorCode::INTERNAL_ERROR, endpoint_task_result.error().message())));
            }
            auto call_result = std::move(endpoint_task_result.value());
            if (call_result.has_value() || !is_endpoint_failure(call_result.error())) {
                co_return std::move(call_result);
            }
            last_error = call_result.error();
        }

        co_return RpcManagedCallResult(std::unexpected(last_error));
    }

    Task<RpcManagedCallResult> call_endpoint(const RpcEndpoint& endpoint,
                                            const std::string& service,
                                            const std::string& method,
                                            const char* payload,
                                            size_t payload_len,
                                            const RpcCallOptions& options) {
        auto acquire_result = co_await m_pool.acquire(endpoint);
        if (!acquire_result.has_value()) {
            co_return RpcManagedCallResult(std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, acquire_result.error().message())));
        }
        if (!acquire_result->has_value()) {
            co_return RpcManagedCallResult(std::unexpected(acquire_result->error()));
        }
        auto lease = std::move(acquire_result->value());

        RpcClient client(m_config.client);
        auto connect_result = co_await client.connect(endpoint.host, endpoint.port);
        if (!connect_result.has_value()) {
            lease.mark_broken();
            mark_endpoint_unavailable(endpoint);
            RpcError primary_error(RpcErrorCode::INTERNAL_ERROR, connect_result.error().message());
            auto release_result = m_pool.release(std::move(lease));
            if (!release_result.has_value()) {
                co_return RpcManagedCallResult(std::unexpected(
                    combine_cleanup_error(primary_error, release_result.error())));
            }
            co_return RpcManagedCallResult(std::unexpected(std::move(primary_error)));
        }
        if (!connect_result->has_value()) {
            lease.mark_broken();
            mark_endpoint_unavailable(endpoint);
            auto primary_error = RpcError::from(connect_result->error(), RpcErrorCode::UNAVAILABLE);
            auto release_result = m_pool.release(std::move(lease));
            if (!release_result.has_value()) {
                co_return RpcManagedCallResult(std::unexpected(
                    combine_cleanup_error(primary_error, release_result.error())));
            }
            co_return RpcManagedCallResult(std::unexpected(std::move(primary_error)));
        }

        auto call_result = co_await client.call(service, method, payload, payload_len, options);
        auto close_result = co_await client.close();
        std::optional<RpcError> close_error;
        if (!close_result.has_value()) {
            close_error.emplace(RpcErrorCode::INTERNAL_ERROR, close_result.error().message());
        } else if (!close_result->has_value()) {
            close_error.emplace(RpcError::from(close_result->error(), RpcErrorCode::CONNECTION_CLOSED));
        }
        if (!call_result.has_value()) {
            lease.mark_broken();
            mark_endpoint_unavailable(endpoint);
            RpcError primary_error(RpcErrorCode::INTERNAL_ERROR, "Failed to schedule managed RPC call");
            if (close_error.has_value()) {
                primary_error = combine_cleanup_error(primary_error, *close_error);
            }
            auto release_result = m_pool.release(std::move(lease));
            if (!release_result.has_value()) {
                primary_error = combine_cleanup_error(primary_error, release_result.error());
            }
            co_return RpcManagedCallResult(std::unexpected(std::move(primary_error)));
        }

        if (close_error.has_value()) {
            lease.mark_broken();
            mark_endpoint_unavailable(endpoint);
            RpcError primary_error = call_result->has_value()
                ? *close_error
                : combine_cleanup_error(call_result->error(), *close_error);
            auto release_result = m_pool.release(std::move(lease));
            if (!release_result.has_value()) {
                co_return RpcManagedCallResult(std::unexpected(
                    combine_cleanup_error(primary_error, release_result.error())));
            }
            co_return RpcManagedCallResult(std::unexpected(std::move(primary_error)));
        }

        if (!call_result->has_value() &&
            (call_result->error().code() == RpcErrorCode::CONNECTION_CLOSED ||
             call_result->error().code() == RpcErrorCode::UNAVAILABLE)) {
            lease.mark_broken();
            mark_endpoint_unavailable(endpoint);
        } else {
            mark_endpoint_available(endpoint);
        }

        auto release_result = m_pool.release(std::move(lease));
        if (!release_result.has_value()) {
            if (!call_result->has_value()) {
                co_return RpcManagedCallResult(std::unexpected(
                    combine_cleanup_error(call_result->error(), release_result.error())));
            }
            co_return RpcManagedCallResult(std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR,
                         "RPC managed client release failed after successful call: " +
                             release_result.error().to_string())));
        }
        co_return std::move(call_result.value());
    }

    std::expected<ServiceSnapshot*, RpcError> ensure_snapshot(const std::string& service) {
        auto it = m_services.find(service);
        if (it == m_services.end() || it->second.endpoints.empty()) {
            auto refreshed = refresh(service);
            if (!refreshed.has_value()) {
                return std::unexpected(refreshed.error());
            }
            it = m_services.find(service);
        }
        return &it->second;
    }

    Discovery* m_discovery;
    RpcManagedClientConfig m_config;
    RpcConnectionPool m_pool;
    RpcGovernanceController m_governance;
    std::unordered_map<std::string, ServiceSnapshot> m_services;
    std::unordered_set<std::string> m_unavailable_endpoints;
};

using RpcManagedClient = RpcManagedClientImpl<RpcStaticDiscovery>;

} // namespace galay::rpc

#endif // GALAY_RPC_MANAGED_CLIENT_H
