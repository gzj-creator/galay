/**
 * @file rpc_endpoint_cache.h
 * @brief RPC endpoint快照缓存
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 缓存由单个发现/调度上下文拥有，事件原地更新；需要保留的快照按值复制。
 *          不在读写路径维护共享所有权、原子发布或互斥锁。
 */

#ifndef GALAY_RPC_ENDPOINT_CACHE_H
#define GALAY_RPC_ENDPOINT_CACHE_H

#include "rpc_endpoint.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace galay::rpc
{

/**
 * @brief endpoint变更事件类型
 */
enum class RpcEndpointEventType {
    Add,     ///< 添加或替换endpoint
    Update,  ///< 更新endpoint
    Remove   ///< 删除endpoint
};

/**
 * @brief endpoint变更事件
 */
struct RpcEndpointEvent {
    RpcEndpointEventType type = RpcEndpointEventType::Add;  ///< 事件类型
    RpcEndpointInfo endpoint;  ///< 添加/更新事件携带的endpoint
    std::string service;  ///< 删除事件服务名
    std::string instance_id;  ///< 删除事件实例ID

    /// @brief 构造添加事件
    static RpcEndpointEvent add(RpcEndpointInfo info) {
        RpcEndpointEvent event;
        event.type = RpcEndpointEventType::Add;
        event.service = info.service;
        event.instance_id = info.instance_id;
        event.endpoint = std::move(info);
        return event;
    }

    /// @brief 构造更新事件
    static RpcEndpointEvent update(RpcEndpointInfo info) {
        RpcEndpointEvent event = add(std::move(info));
        event.type = RpcEndpointEventType::Update;
        return event;
    }

    /// @brief 构造删除事件
    static RpcEndpointEvent remove(std::string service_name, std::string instance) {
        RpcEndpointEvent event;
        event.type = RpcEndpointEventType::Remove;
        event.service = std::move(service_name);
        event.instance_id = std::move(instance);
        return event;
    }
};

/**
 * @brief endpoint缓存快照
 */
struct RpcEndpointSnapshot {
    RpcEndpointSnapshot() = default;
    RpcEndpointSnapshot(RpcEndpointSnapshot&&) noexcept = default;
    RpcEndpointSnapshot& operator=(RpcEndpointSnapshot&&) noexcept = default;

    /// @brief 显式复制独立拥有的 endpoint 数据。
    RpcEndpointSnapshot clone() const { return RpcEndpointSnapshot(*this); }

    std::unordered_map<std::string, std::vector<RpcEndpointInfo>> by_service;  ///< 按服务分组

private:
    RpcEndpointSnapshot(const RpcEndpointSnapshot&) = default;
    RpcEndpointSnapshot& operator=(const RpcEndpointSnapshot&) = delete;
};

/**
 * @brief RPC endpoint快照缓存
 * @note 非线程安全。读取和 apply() 必须在同一 owner 上串行执行；跨线程变更
 *       应先通过调度器/消息通道投递给 owner。返回的值快照可独立转移到其他线程。
 */
class RpcEndpointCache {
public:
    RpcEndpointCache() = default;
    RpcEndpointCache(RpcEndpointCache&&) noexcept = default;
    RpcEndpointCache& operator=(RpcEndpointCache&&) noexcept = default;

    /**
     * @brief 获取完整快照
     * @return 独立拥有的值快照；缓存更新或销毁不会影响已返回的数据。
     * @note 复制所有服务；只需单个服务时使用 snapshot(service)。
     */
    RpcEndpointSnapshot snapshot() const {
        return m_snapshot.clone();
    }

    /**
     * @brief 获取指定服务endpoint快照副本
     */
    std::vector<RpcEndpointInfo> snapshot(const std::string& service) const {
        auto it = m_snapshot.by_service.find(service);
        if (it == m_snapshot.by_service.end()) {
            return {};
        }
        return it->second;
    }

    /**
     * @brief 获取指定服务可选endpoint副本
     */
    std::vector<RpcEndpointInfo> selectable(const std::string& service) const {
        std::vector<RpcEndpointInfo> result;
        auto it = m_snapshot.by_service.find(service);
        if (it == m_snapshot.by_service.end()) {
            return result;
        }
        result.reserve(it->second.size());
        for (const auto& endpoint : it->second) {
            if (endpoint.selectable()) {
                result.push_back(endpoint);
            }
        }
        return result;
    }

    /**
     * @brief 在 owner 上原地应用endpoint变更，不复制无关服务。
     */
    void apply(const RpcEndpointEvent& event) {
        if (event.type == RpcEndpointEventType::Remove) {
            removeFrom(m_snapshot, event.service, event.instance_id);
        } else {
            upsertInto(m_snapshot, event.endpoint);
        }
    }

private:
    RpcEndpointCache(const RpcEndpointCache&) = delete;
    RpcEndpointCache& operator=(const RpcEndpointCache&) = delete;

    static void upsertInto(RpcEndpointSnapshot& snapshot, const RpcEndpointInfo& endpoint) {
        auto& endpoints = snapshot.by_service[endpoint.service];
        auto it = std::ranges::find_if(endpoints, [&](const RpcEndpointInfo& item) {
            return item.instance_id == endpoint.instance_id;
        });
        if (it == endpoints.end()) {
            endpoints.push_back(endpoint);
        } else {
            *it = endpoint;
        }
    }

    static void removeFrom(RpcEndpointSnapshot& snapshot,
                           const std::string& service,
                           const std::string& instance_id) {
        auto it = snapshot.by_service.find(service);
        if (it == snapshot.by_service.end()) {
            return;
        }
        auto& endpoints = it->second;
        std::erase_if(endpoints, [&](const RpcEndpointInfo& item) {
            return item.instance_id == instance_id;
        });
        if (endpoints.empty()) {
            // 已完成本次删除，无需继续遍历返回的后继位置。
            [[maybe_unused]] auto next = snapshot.by_service.erase(it);
        }
    }

    RpcEndpointSnapshot m_snapshot;  ///< 缓存独占数据，旧快照由调用方按值拥有
};

} // namespace galay::rpc

#endif // GALAY_RPC_ENDPOINT_CACHE_H
