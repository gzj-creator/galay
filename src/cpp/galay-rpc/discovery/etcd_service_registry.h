/**
 * @file etcd_service_registry.h
 * @brief RPC etcd注册中心适配接口
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 默认提供无外部依赖的fake contract和占位适配器；真实etcd集成由构建开关接入。
 */

#ifndef GALAY_RPC_ETCD_SERVICE_REGISTRY_H
#define GALAY_RPC_ETCD_SERVICE_REGISTRY_H

#include "../kernel/rpc_endpoint_cache.h"
#include "../protoc/rpc_error.h"

#include <expected>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace galay::rpc
{

struct RpcEtcdRegistryConfig {
    std::string endpoint;
    std::string prefix = "/galay/rpc";
};

class FakeEtcdServiceRegistry {
public:
    explicit FakeEtcdServiceRegistry(std::string prefix)
        : m_prefix(std::move(prefix))
    {
    }

    std::expected<void, RpcError> registerEndpoint(const RpcEndpointInfo& endpoint) {
        auto& endpoints = m_services[endpoint.service];
        auto it = std::find_if(endpoints.begin(), endpoints.end(), [&](const RpcEndpointInfo& item) {
            return item.instance_id == endpoint.instance_id;
        });
        if (it == endpoints.end()) {
            endpoints.push_back(endpoint);
        } else {
            *it = endpoint;
        }
        notify(endpoint.service, RpcEndpointEvent::update(endpoint));
        return {};
    }

    std::expected<void, RpcError> deregisterEndpoint(const std::string& service,
                                                     const std::string& instance_id) {
        auto& endpoints = m_services[service];
        std::erase_if(endpoints, [&](const RpcEndpointInfo& item) {
            return item.instance_id == instance_id;
        });
        notify(service, RpcEndpointEvent::remove(service, instance_id));
        return {};
    }

    std::expected<std::vector<RpcEndpointInfo>, RpcError> discover(const std::string& service) const {
        auto it = m_services.find(service);
        if (it == m_services.end()) {
            return std::vector<RpcEndpointInfo>{};
        }
        return it->second;
    }

    std::expected<void, RpcError> watch(const std::string& service,
                                        RpcEndpointCache& cache,
                                        std::function<void()> callback = {}) {
        m_watchers[service].push_back(Watcher{&cache, std::move(callback)});
        return {};
    }

private:
    struct Watcher {
        RpcEndpointCache* cache = nullptr;
        std::function<void()> callback;
    };

    void notify(const std::string& service, const RpcEndpointEvent& event) {
        auto it = m_watchers.find(service);
        if (it == m_watchers.end()) {
            return;
        }
        for (auto& watcher : it->second) {
            if (watcher.cache != nullptr) {
                watcher.cache->apply(event);
            }
            if (watcher.callback) {
                watcher.callback();
            }
        }
    }

    std::string m_prefix;
    std::unordered_map<std::string, std::vector<RpcEndpointInfo>> m_services;
    std::unordered_map<std::string, std::vector<Watcher>> m_watchers;
};

class EtcdServiceRegistry {
public:
    explicit EtcdServiceRegistry(RpcEtcdRegistryConfig config)
        : m_config(std::move(config))
    {
    }

    std::expected<void, RpcError> integrationAvailable() const {
        if (m_config.endpoint.empty()) {
            return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE,
                                            "GALAY_ETCD_ENDPOINT is empty"));
        }
        return {};
    }

private:
    RpcEtcdRegistryConfig m_config;
};

} // namespace galay::rpc

#endif // GALAY_RPC_ETCD_SERVICE_REGISTRY_H
