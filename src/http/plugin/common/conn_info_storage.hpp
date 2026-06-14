#ifndef GALAY_HTTP_PLUGIN_CONN_INFO_STORAGE_HPP
#define GALAY_HTTP_PLUGIN_CONN_INFO_STORAGE_HPP

#include "kernel/common/host.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <netinet/in.h>
#include <optional>
#include <unordered_map>
#include <utility>

namespace galay::http::plugin {

/**
 * @brief HTTP 接入插件使用的单 host 连接元数据。
 * @details 存储器拥有这些值。blacklist 插件使用这些字段记录同一客户端 IP 的
 *          访问压力、临时封禁截止时间，以及计数窗口/衰减时间。
 */
struct ConnInfo {
    std::size_t ip_conn_times = 0; ///< 当前地址键已记录的连接尝试次数。
    std::chrono::steady_clock::time_point first_access_at{}; ///< 固定间隔策略的当前窗口起点。
    std::chrono::steady_clock::time_point blocked_until{};   ///< 临时封禁截止时间；默认值表示未封禁。
    std::chrono::steady_clock::time_point last_decay_at{};   ///< 衰减计数策略的上次衰减基准时间。
};

/**
 * @brief 按远端地址保存 ConnInfo 的轻量存储器。
 * @details 该类型本身非线程安全；多线程或多协程共享时必须由调用方加锁。
 *          默认 key 包含 IP 与端口，保持完整 host 语义。blacklist 等需要按 IP
 *          聚合的插件可传 include_port=false，忽略 TCP 临时源端口。
 */
class ConnInfoStorage {
private:
    struct HostKey {
        sa_family_t family = AF_UNSPEC;
        in_port_t port = 0;
        std::uint32_t scope_id = 0;
        std::array<std::uint8_t, 16> address{};

        static HostKey fromHost(const kernel::Host& host, bool include_port = true) noexcept {
            HostKey key;
            if (host.isIPv4()) {
                const auto* addr4 = reinterpret_cast<const sockaddr_in*>(host.sockAddr());
                key.family = AF_INET;
                key.port = include_port ? addr4->sin_port : 0;
                std::memcpy(key.address.data(), &addr4->sin_addr, sizeof(addr4->sin_addr));
            } else {
                const auto* addr6 = reinterpret_cast<const sockaddr_in6*>(host.sockAddr());
                key.family = AF_INET6;
                key.port = include_port ? addr6->sin6_port : 0;
                key.scope_id = addr6->sin6_scope_id;
                std::memcpy(key.address.data(), &addr6->sin6_addr, sizeof(addr6->sin6_addr));
            }
            return key;
        }

        bool operator==(const HostKey& other) const noexcept = default;
    };

    struct HostKeyHash {
        std::size_t operator()(const HostKey& key) const noexcept {
            std::size_t hash = 1469598103934665603ull;
            mixValue(hash, key.family);
            mixValue(hash, key.port);
            mixValue(hash, key.scope_id);
            for (std::uint8_t byte : key.address) {
                mixByte(hash, byte);
            }
            return hash;
        }

    private:
        static void mixByte(std::size_t& hash, std::uint8_t byte) noexcept {
            hash ^= byte;
            hash *= 1099511628211ull;
        }

        template <typename T>
        static void mixValue(std::size_t& hash, const T& value) noexcept {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                mixByte(hash, bytes[i]);
            }
        }
    };

public:
    ConnInfoStorage() = default;

    ConnInfoStorage(const ConnInfoStorage&) = delete;
    ConnInfoStorage& operator=(const ConnInfoStorage&) = delete;
    
    /**
     * @brief 查询连接信息引用；不存在时不创建记录。
     * @param host 连接对端地址。
     * @param include_port true 时地址 key 包含端口；false 时只按 IP/协议族聚合。
     * @return 找到时返回连接信息引用；不存在时返回 std::nullopt。
     * @note 返回的引用在对应元素删除或 clear 后失效。
     */
    std::optional<std::reference_wrapper<ConnInfo>> getConnInfoRef(const kernel::Host& host,
                                                                   bool include_port = true) {
        auto iter = m_conn_info.find(HostKey::fromHost(host, include_port));
        if (iter == m_conn_info.end()) {
            return std::nullopt;
        }
        return std::ref(iter->second);
    }

    /**
     * @brief 查询连接信息引用；不存在时创建一条默认记录。
     * @param host 连接对端地址。
     * @param include_port true 时地址 key 包含端口；false 时只按 IP/协议族聚合。
     * @return 该 host 对应的连接信息引用。
     * @note 返回的引用在对应元素删除或 clear 后失效。
     */
    ConnInfo& getOrCreateConnInfo(const kernel::Host& host, bool include_port = true) {
        auto [iter, inserted] = m_conn_info.try_emplace(HostKey::fromHost(host, include_port));
        (void)inserted;
        return iter->second;
    }

    /**
     * @brief 查询连接信息。
     * @param host 连接对端地址。
     * @param include_port true 时地址 key 包含端口；false 时只按 IP/协议族聚合。
     * @return 找到时返回连接信息副本；不存在时返回 std::nullopt。
     */
    std::optional<ConnInfo> getConnInfo(const kernel::Host& host,
                                        bool include_port = true) const {
        auto iter = m_conn_info.find(HostKey::fromHost(host, include_port));
        if (iter == m_conn_info.end()) {
            return std::nullopt;
        }
        return iter->second;
    }

    /**
     * @brief 新增连接信息。
     * @param host 连接对端地址。
     * @param conn_info 要保存的连接信息。
     * @param include_port true 时地址 key 包含端口；false 时只按 IP/协议族聚合。
     * @return 新增成功返回 true；host 已存在时不覆盖并返回 false。
     */
    bool addConnInfo(const kernel::Host& host,
                     ConnInfo conn_info = ConnInfo{},
                     bool include_port = true) {
        auto [iter, inserted] = m_conn_info.try_emplace(HostKey::fromHost(host, include_port),
                                                        std::move(conn_info));
        (void)iter;
        return inserted;
    }

    /**
     * @brief 修改已有连接信息。
     * @param host 连接对端地址。
     * @param conn_info 新的连接信息。
     * @param include_port true 时地址 key 包含端口；false 时只按 IP/协议族聚合。
     * @return 修改成功返回 true；host 不存在时不创建记录并返回 false。
     */
    bool updateConnInfo(const kernel::Host& host,
                        ConnInfo conn_info,
                        bool include_port = true) {
        auto iter = m_conn_info.find(HostKey::fromHost(host, include_port));
        if (iter == m_conn_info.end()) {
            return false;
        }

        iter->second = std::move(conn_info);
        return true;
    }

    /**
     * @brief 删除连接信息。
     * @param host 连接对端地址。
     * @param include_port true 时地址 key 包含端口；false 时只按 IP/协议族聚合。
     * @return 删除成功返回 true；host 不存在时返回 false。
     */
    bool deleteConnInfo(const kernel::Host& host, bool include_port = true) {
        return m_conn_info.erase(HostKey::fromHost(host, include_port)) > 0;
    }

    /**
     * @brief deleteConnInfo 的同义接口，便于按 remove 语义调用。
     * @param host 连接对端地址。
     * @param include_port true 时地址 key 包含端口；false 时只按 IP/协议族聚合。
     * @return 删除成功返回 true；host 不存在时返回 false。
     */
    bool removeConnInfo(const kernel::Host& host, bool include_port = true) {
        return deleteConnInfo(host, include_port);
    }

    /**
     * @brief 判断 host 是否已有连接信息。
     * @param host 连接对端地址。
     * @param include_port true 时地址 key 包含端口；false 时只按 IP/协议族聚合。
     * @return 已存在返回 true；否则返回 false。
     */
    bool containsConnInfo(const kernel::Host& host, bool include_port = true) const {
        return m_conn_info.contains(HostKey::fromHost(host, include_port));
    }

    /**
     * @brief 清空全部连接信息；此前返回的 ConnInfo 引用或指针全部失效。
     */
    void clearConnInfo() {
        m_conn_info.clear();
    }

    /**
     * @brief 返回当前保存的连接信息数量。
     */
    std::size_t size() const noexcept {
        return m_conn_info.size();
    }

    /**
     * @brief 判断当前是否没有任何连接信息。
     */
    bool empty() const noexcept {
        return m_conn_info.empty();
    }

private:
    std::unordered_map<HostKey, ConnInfo, HostKeyHash> m_conn_info;
};


} // namespace galay::http::plugin

#endif // GALAY_HTTP_PLUGIN_CONN_INFO_STORAGE_HPP
