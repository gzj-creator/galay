/**
 * @file rpc_discovery.h
 * @brief 服务发现模块
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供服务发现的抽象接口，使用C++20 concept约束。
 * 支持本地服务发现和未来的etcd等注册中心。
 */

#ifndef GALAY_RPC_DISCOVERY_H
#define GALAY_RPC_DISCOVERY_H

#include "rpc_service.h"
#include "galay-kernel/concurrency/async_mutex.h"
#include "galay-kernel/common/async_strategy.hpp"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <expected>
#include <concepts>
#include <functional>

namespace galay::rpc
{

/**
 * @brief 服务端点信息
 */
struct ServiceEndpoint {
    std::string host;           ///< 主机地址
    uint16_t port;              ///< 端口
    std::string service_name;   ///< 服务名
    std::string instance_id;    ///< 实例ID
    uint32_t weight = 100;      ///< 权重（用于负载均衡）

    /// @brief 获取地址字符串（host:port）
    std::string address() const {
        return host + ":" + std::to_string(port);
    }
};

/**
 * @brief 服务发现错误
 */
struct DiscoveryError {
    enum Code {
        OK = 0,             ///< 成功
        NOT_FOUND,          ///< 未找到
        CONNECTION_ERROR,   ///< 连接错误
        LOCK_TIMEOUT,       ///< 锁超时
        INTERNAL_ERROR      ///< 内部错误
    };

    Code code = OK;         ///< 错误码
    std::string message;    ///< 错误消息

    DiscoveryError() = default;
    /**
     * @brief 构造错误
     * @param c 错误码
     * @param msg 错误消息
     */
    DiscoveryError(Code c, std::string msg = "") : code(c), message(std::move(msg)) {}

    /// @brief 判断是否成功
    bool isOk() const { return code == OK; }
    /// @brief 判断是否存在错误
    explicit operator bool() const { return !isOk(); }
};

/**
 * @brief 服务变更事件类型
 */
enum class ServiceEventType {
    ADDED,      ///< 服务添加
    REMOVED,    ///< 服务移除
    UPDATED     ///< 服务更新
};

/**
 * @brief 服务变更事件
 */
struct ServiceEvent {
    ServiceEventType type;    ///< 事件类型
    ServiceEndpoint endpoint; ///< 服务端点
};

/**
 * @brief 服务变更回调
 */
using ServiceWatchCallback = std::function<void(const ServiceEvent&)>;

/**
 * @brief 服务注册中心 Concept
 *
 * @details 定义服务注册中心必须实现的接口。
 * 支持本地注册、etcd、consul等多种实现。
 */
template<typename T>
concept ServiceRegistry = requires(T registry,
                                   const std::string& service_name,
                                   const ServiceEndpoint& endpoint,
                                   ServiceWatchCallback callback) {
    // 注册服务
    { registry.registerService(endpoint) } -> std::same_as<std::expected<void, DiscoveryError>>;

    // 注销服务
    { registry.deregisterService(endpoint) } -> std::same_as<std::expected<void, DiscoveryError>>;

    // 发现服务（获取所有实例）
    { registry.discoverService(service_name) } -> std::same_as<std::expected<std::vector<ServiceEndpoint>, DiscoveryError>>;

    // 监听服务变更
    { registry.watchService(service_name, callback) } -> std::same_as<std::expected<void, DiscoveryError>>;

    // 取消监听
    { registry.unwatchService(service_name) } -> std::same_as<void>;
};

/**
 * @brief 本地服务注册中心（非线程安全）
 *
 * @details 简单的内存服务注册，用于单线程环境。
 * 如需多线程/协程环境，请使用 AsyncLocalServiceRegistry。
 *
 * @note 非线程安全，仅适用于单线程环境
 */
class LocalServiceRegistry {
public:
    LocalServiceRegistry() = default;
    ~LocalServiceRegistry() = default;

    /**
     * @brief 注册服务
     */
    std::expected<void, DiscoveryError> registerService(const ServiceEndpoint& endpoint) {
        m_services[endpoint.service_name].push_back(endpoint);

        // 触发回调
        if (auto it = m_watchers.find(endpoint.service_name); it != m_watchers.end()) {
            ServiceEvent event{ServiceEventType::ADDED, endpoint};
            for (auto& callback : it->second) {
                callback(event);
            }
        }

        return {};
    }

    /**
     * @brief 注销服务
     */
    std::expected<void, DiscoveryError> deregisterService(const ServiceEndpoint& endpoint) {
        auto it = m_services.find(endpoint.service_name);
        if (it == m_services.end()) {
            return std::unexpected(DiscoveryError(DiscoveryError::NOT_FOUND, "Service not found"));
        }

        auto& endpoints = it->second;
        auto ep_it = std::find_if(endpoints.begin(), endpoints.end(),
            [&](const ServiceEndpoint& ep) {
                return ep.instance_id == endpoint.instance_id;
            });

        if (ep_it == endpoints.end()) {
            return std::unexpected(DiscoveryError(DiscoveryError::NOT_FOUND, "Instance not found"));
        }

        ServiceEndpoint removed = *ep_it;
        endpoints.erase(ep_it);

        // 触发回调
        if (auto wit = m_watchers.find(endpoint.service_name); wit != m_watchers.end()) {
            ServiceEvent event{ServiceEventType::REMOVED, removed};
            for (auto& callback : wit->second) {
                callback(event);
            }
        }

        return {};
    }

    /**
     * @brief 发现服务
     */
    std::expected<std::vector<ServiceEndpoint>, DiscoveryError> discoverService(const std::string& service_name) {
        auto it = m_services.find(service_name);
        if (it == m_services.end()) {
            return std::vector<ServiceEndpoint>{};
        }

        return it->second;
    }

    /**
     * @brief 监听服务变更
     */
    std::expected<void, DiscoveryError> watchService(const std::string& service_name, ServiceWatchCallback callback) {
        m_watchers[service_name].push_back(std::move(callback));
        return {};
    }

    /**
     * @brief 取消监听
     */
    void unwatchService(const std::string& service_name) {
        m_watchers.erase(service_name);
    }

private:
    std::unordered_map<std::string, std::vector<ServiceEndpoint>> m_services;    ///< 服务名到端点列表的映射
    std::unordered_map<std::string, std::vector<ServiceWatchCallback>> m_watchers;  ///< 服务名到监听回调的映射
};

// 验证LocalServiceRegistry满足ServiceRegistry concept
static_assert(ServiceRegistry<LocalServiceRegistry>, "LocalServiceRegistry must satisfy ServiceRegistry concept");

/**
 * @brief 异步服务注册中心 Concept
 *
 * @details 定义异步服务注册中心必须实现的接口，用于协程环境。
 * 所有方法返回Coroutine，支持co_await。
 */
template<typename T>
concept AsyncServiceRegistry = requires(T registry,
                                        const std::string& service_name,
                                        const ServiceEndpoint& endpoint,
                                        ServiceWatchCallback callback) {
    // 异步注册服务
    { registry.registerServiceAsync(endpoint) } -> std::same_as<Coroutine>;

    // 异步注销服务
    { registry.deregisterServiceAsync(endpoint) } -> std::same_as<Coroutine>;

    // 异步发现服务
    { registry.discoverServiceAsync(service_name) } -> std::same_as<Coroutine>;

    // 异步监听服务变更
    { registry.watchServiceAsync(service_name, callback) } -> std::same_as<Coroutine>;

    // 异步取消监听
    { registry.unwatchServiceAsync(service_name) } -> std::same_as<Coroutine>;

    // 获取最后一次操作的结果
    { registry.lastError() } -> std::same_as<DiscoveryError>;
    { registry.lastEndpoints() } -> std::same_as<std::vector<ServiceEndpoint>>;
};

/**
 * @brief 异步本地服务注册中心
 *
 * @details 使用AsyncMutex实现的协程友好服务注册中心。
 * 适用于协程环境，不会阻塞线程。
 */
class AsyncLocalServiceRegistry {
public:
    AsyncLocalServiceRegistry() = default;
    ~AsyncLocalServiceRegistry() = default;

    /**
     * @brief 异步注册服务
     */
    Coroutine registerServiceAsync(const ServiceEndpoint& endpoint) {
        auto lock_result = co_await m_mutex.lock();
        if (!lock_result) {
            m_last_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
            co_return;
        }

        m_services[endpoint.service_name].push_back(endpoint);

        // 触发回调
        if (auto it = m_watchers.find(endpoint.service_name); it != m_watchers.end()) {
            ServiceEvent event{ServiceEventType::ADDED, endpoint};
            for (auto& callback : it->second) {
                callback(event);
            }
        }

        m_last_error = DiscoveryError();
        m_mutex.unlock();
        co_return;
    }

    /**
     * @brief 异步注销服务
     */
    Coroutine deregisterServiceAsync(const ServiceEndpoint& endpoint) {
        auto lock_result = co_await m_mutex.lock();
        if (!lock_result) {
            m_last_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
            co_return;
        }

        auto it = m_services.find(endpoint.service_name);
        if (it == m_services.end()) {
            m_last_error = DiscoveryError(DiscoveryError::NOT_FOUND, "Service not found");
            m_mutex.unlock();
            co_return;
        }

        auto& endpoints = it->second;
        auto ep_it = std::find_if(endpoints.begin(), endpoints.end(),
            [&](const ServiceEndpoint& ep) {
                return ep.instance_id == endpoint.instance_id;
            });

        if (ep_it == endpoints.end()) {
            m_last_error = DiscoveryError(DiscoveryError::NOT_FOUND, "Instance not found");
            m_mutex.unlock();
            co_return;
        }

        ServiceEndpoint removed = *ep_it;
        endpoints.erase(ep_it);

        // 触发回调
        if (auto wit = m_watchers.find(endpoint.service_name); wit != m_watchers.end()) {
            ServiceEvent event{ServiceEventType::REMOVED, removed};
            for (auto& callback : wit->second) {
                callback(event);
            }
        }

        m_last_error = DiscoveryError();
        m_mutex.unlock();
        co_return;
    }

    /**
     * @brief 异步发现服务
     */
    Coroutine discoverServiceAsync(const std::string& service_name) {
        auto lock_result = co_await m_mutex.lock();
        if (!lock_result) {
            m_last_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
            m_last_endpoints.clear();
            co_return;
        }

        auto it = m_services.find(service_name);
        if (it == m_services.end()) {
            m_last_endpoints.clear();
        } else {
            m_last_endpoints = it->second;
        }

        m_last_error = DiscoveryError();
        m_mutex.unlock();
        co_return;
    }

    /**
     * @brief 异步监听服务变更
     */
    Coroutine watchServiceAsync(const std::string& service_name, ServiceWatchCallback callback) {
        auto lock_result = co_await m_mutex.lock();
        if (!lock_result) {
            m_last_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
            co_return;
        }

        m_watchers[service_name].push_back(std::move(callback));
        m_last_error = DiscoveryError();
        m_mutex.unlock();
        co_return;
    }

    /**
     * @brief 异步取消监听
     */
    Coroutine unwatchServiceAsync(const std::string& service_name) {
        auto lock_result = co_await m_mutex.lock();
        if (!lock_result) {
            m_last_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
            co_return;
        }

        m_watchers.erase(service_name);
        m_last_error = DiscoveryError();
        m_mutex.unlock();
        co_return;
    }

    /**
     * @brief 获取最后一次操作的错误
     */
    DiscoveryError lastError() const { return m_last_error; }

    /**
     * @brief 获取最后一次发现的端点列表
     */
    std::vector<ServiceEndpoint> lastEndpoints() const { return m_last_endpoints; }

private:
    kernel::AsyncMutex m_mutex;       ///< 异步互斥锁
    std::unordered_map<std::string, std::vector<ServiceEndpoint>> m_services;    ///< 服务名到端点列表的映射
    std::unordered_map<std::string, std::vector<ServiceWatchCallback>> m_watchers;  ///< 服务名到监听回调的映射
    DiscoveryError m_last_error;      ///< 最后一次操作错误
    std::vector<ServiceEndpoint> m_last_endpoints;  ///< 最后一次发现的端点列表
};

// 验证AsyncLocalServiceRegistry满足AsyncServiceRegistry concept
static_assert(AsyncServiceRegistry<AsyncLocalServiceRegistry>, "AsyncLocalServiceRegistry must satisfy AsyncServiceRegistry concept");

// 使用 galay-kernel 的负载均衡策略
using RoundRobinSelector = details::RoundRobinLoadBalancer<ServiceEndpoint>;           ///< 轮询选择器
using RandomSelector = details::RandomLoadBalancer<ServiceEndpoint>;                   ///< 随机选择器
using WeightedRoundRobinSelector = details::WeightRoundRobinLoadBalancer<ServiceEndpoint>;  ///< 加权轮询选择器
using WeightedRandomSelector = details::WeightedRandomLoadBalancer<ServiceEndpoint>;   ///< 加权随机选择器

/**
 * @brief 服务发现客户端
 *
 * @tparam Registry 服务注册中心类型，必须满足ServiceRegistry concept
 * @tparam Selector 服务选择器类型（使用 galay-kernel 的负载均衡器）
 */
template<ServiceRegistry Registry, typename Selector = RoundRobinSelector>
class ServiceDiscoveryClient {
public:
    /**
     * @brief 构造服务发现客户端
     * @param registry 注册中心引用
     */
    explicit ServiceDiscoveryClient(Registry& registry)
        : m_registry(registry), m_selector() {}

    /**
     * @brief 获取服务实例
     */
    std::expected<ServiceEndpoint, DiscoveryError> getServiceEndpoint(const std::string& service_name) {
        auto result = m_registry.discoverService(service_name);
        if (!result) {
            return std::unexpected(result.error());
        }

        auto& endpoints = result.value();
        if (endpoints.empty()) {
            return std::unexpected(DiscoveryError(DiscoveryError::NOT_FOUND, "No available instance"));
        }

        // 更新选择器的节点列表并选择
        m_selector = Selector(endpoints);
        auto selected = m_selector.select();

        if (!selected) {
            return std::unexpected(DiscoveryError(DiscoveryError::NOT_FOUND, "No available instance"));
        }

        return *selected;
    }

    /**
     * @brief 监听服务变更
     */
    std::expected<void, DiscoveryError> watch(const std::string& service_name, ServiceWatchCallback callback) {
        return m_registry.watchService(service_name, std::move(callback));
    }

    /**
     * @brief 取消监听
     */
    void unwatch(const std::string& service_name) {
        m_registry.unwatchService(service_name);
    }

private:
    Registry& m_registry;   ///< 注册中心引用
    Selector m_selector;     ///< 负载均衡选择器
};

} // namespace galay::rpc

#endif // GALAY_RPC_DISCOVERY_H
