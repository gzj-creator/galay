/**
 * @file rpc_endpoint_cache.h
 * @brief RPC endpoint快照缓存
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 写入路径复制当前快照并原子发布；读取路径只做atomic shared_ptr load，
 *          不获取写锁，适合托管客户端热路径读取发现快照。
 */

#ifndef GALAY_RPC_ENDPOINT_CACHE_H
#define GALAY_RPC_ENDPOINT_CACHE_H

#include "rpc_endpoint.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
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
    std::unordered_map<std::string, std::vector<RpcEndpointInfo>> by_service;  ///< 按服务分组
};

/**
 * @brief RPC endpoint快照缓存
 */
class RpcEndpointCache {
public:
    RpcEndpointCache()
        : m_snapshot(std::make_shared<const RpcEndpointSnapshot>())
    {
    }

    /**
     * @brief 获取完整快照
     * @return 只读快照shared_ptr；调用方可长期持有，后续写入不会修改该快照
     *
     * @note 读路径不获取互斥锁，只做atomic shared_ptr load。
     */
    std::shared_ptr<const RpcEndpointSnapshot> snapshot() const {
        return m_snapshot.load(std::memory_order_acquire);
    }

    /**
     * @brief 获取指定服务endpoint快照副本
     */
    std::vector<RpcEndpointInfo> snapshot(const std::string& service) const {
        auto current = snapshot();
        auto it = current->by_service.find(service);
        if (it == current->by_service.end()) {
            return {};
        }
        return it->second;
    }

    /**
     * @brief 获取指定服务可选endpoint副本
     */
    std::vector<RpcEndpointInfo> selectable(const std::string& service) const {
        std::vector<RpcEndpointInfo> result;
        for (const auto& endpoint : snapshot(service)) {
            if (endpoint.selectable()) {
                result.push_back(endpoint);
            }
        }
        return result;
    }

    /**
     * @brief 应用endpoint变更并发布新快照
     * @note 写路径使用互斥锁序列化复制/发布，不影响并发读者持有旧快照。
     */
    void apply(const RpcEndpointEvent& event) {
        std::lock_guard<std::mutex> guard(m_write_mutex);
        auto next = std::make_shared<RpcEndpointSnapshot>(*snapshot());
        if (event.type == RpcEndpointEventType::Remove) {
            removeFrom(*next, event.service, event.instance_id);
        } else {
            upsertInto(*next, event.endpoint);
        }
        m_snapshot.store(std::shared_ptr<const RpcEndpointSnapshot>(std::move(next)),
                         std::memory_order_release);
    }

private:
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
            snapshot.by_service.erase(it);
        }
    }

    mutable std::mutex m_write_mutex;
    std::atomic<std::shared_ptr<const RpcEndpointSnapshot>> m_snapshot;
};

} // namespace galay::rpc

#endif // GALAY_RPC_ENDPOINT_CACHE_H
