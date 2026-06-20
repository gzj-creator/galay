/**
 * @file mvcc.hpp
 * @brief 多版本并发控制（MVCC）实现
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供基于版本号的多版本并发控制机制，支持快照读、事务、
 *          CAS 操作和垃圾回收。使用读写锁保证线程安全。
 */

#ifndef GALAY_UTILS_MVCC_HPP
#define GALAY_UTILS_MVCC_HPP

#include "../common/defn.hpp"
#include <atomic>
#include <shared_mutex>
#include <map>
#include <memory>
#include <optional>
#include <functional>
#include <vector>

namespace galay::utils {

/// 版本号类型
using Version = uint64_t;

/**
 * @brief 带版本号的值
 * @tparam T 值类型
 */
template<typename T>
struct VersionedValue {
    Version version; ///< 版本号
    std::unique_ptr<T> value; ///< 值指针
    bool deleted; ///< 是否已删除

    VersionedValue(Version v, std::unique_ptr<T> val, bool del = false)
        : version(v), value(std::move(val)), deleted(del) {}
};

/**
 * @brief 多版本并发控制容器
 * @details 维护值的多个版本，支持快照读、事务写入和 CAS 操作。
 * @tparam T 值类型
 */
template<typename T>
class Mvcc {
public:
    Mvcc() : m_currentVersion(0) {}

    /**
     * @brief 获取指定版本号的值
     * @param version 目标版本号
     * @return 值指针，版本不存在或已删除时返回 nullptr
     */
    const T* getValue(Version version) const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);

        auto it = m_versions.upper_bound(version);
        if (it == m_versions.begin()) {
            return nullptr;
        }
        --it;

        if (it->second->deleted) {
            return nullptr;
        }

        return it->second->value.get();
    }

    /**
     * @brief 获取当前最新版本的值
     * @return 值指针，无值时返回 nullptr
     */
    const T* getCurrentValue() const {
        return getValue(m_currentVersion.load());
    }

    /**
     * @brief 获取指定版本的值和实际版本号
     * @param version 目标版本号
     * @return 值指针和实际版本号的键值对
     */
    std::pair<const T*, Version> getValueWithVersion(Version version) const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);

        auto it = m_versions.upper_bound(version);
        if (it == m_versions.begin()) {
            return {nullptr, 0};
        }
        --it;

        if (it->second->deleted) {
            return {nullptr, it->first};
        }

        return {it->second->value.get(), it->first};
    }

    /**
     * @brief 存入新值（移动语义）
     * @param value 新值（unique_ptr）
     * @return 新版本号
     */
    Version putValue(std::unique_ptr<T> value) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        Version newVersion = ++m_currentVersion;
        m_versions[newVersion] = std::make_unique<VersionedValue<T>>(
            newVersion, std::move(value), false);

        return newVersion;
    }

    /**
     * @brief 存入新值（拷贝语义）
     * @param value 新值
     * @return 新版本号
     */
    Version putValue(const T& value) {
        return putValue(std::make_unique<T>(value));
    }

    /**
     * @brief 基于当前值更新
     * @param updateFn 更新函数，接收当前值指针，返回新值
     * @return 新版本号
     */
    Version updateValue(std::function<std::unique_ptr<T>(const T*)> updateFn) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        const T* current = nullptr;
        if (!m_versions.empty()) {
            auto it = m_versions.rbegin();
            if (!it->second->deleted) {
                current = it->second->value.get();
            }
        }

        auto newValue = updateFn(current);
        Version newVersion = ++m_currentVersion;
        m_versions[newVersion] = std::make_unique<VersionedValue<T>>(
            newVersion, std::move(newValue), false);

        return newVersion;
    }

    /**
     * @brief 比较并交换（CAS）
     * @param expectedVersion 期望的当前版本号
     * @param newValue 新值
     * @return 成功返回新版本号，失败返回 0
     */
    Version compareAndSwap(Version expectedVersion, std::unique_ptr<T> newValue) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        if (m_currentVersion.load() != expectedVersion) {
            return 0;
        }

        Version newVersion = ++m_currentVersion;
        m_versions[newVersion] = std::make_unique<VersionedValue<T>>(
            newVersion, std::move(newValue), false);

        return newVersion;
    }

    /**
     * @brief 移除指定版本的值
     * @param version 目标版本号
     * @return 成功返回 true
     */
    bool removeValue(Version version) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        auto it = m_versions.find(version);
        if (it == m_versions.end()) {
            return false;
        }

        m_versions.erase(it);
        return true;
    }

    /**
     * @brief 标记当前值为已删除
     * @return 新版本号
     */
    Version deleteValue() {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        Version newVersion = ++m_currentVersion;
        m_versions[newVersion] = std::make_unique<VersionedValue<T>>(
            newVersion, nullptr, true);

        return newVersion;
    }

    /**
     * @brief 检查版本号是否有效
     * @param version 目标版本号
     * @return 存在返回 true
     */
    bool isValid(Version version) const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_versions.find(version) != m_versions.end();
    }

    /**
     * @brief 获取当前版本号
     * @return 当前版本号
     */
    Version currentVersion() const {
        return m_currentVersion.load();
    }

    /**
     * @brief 获取版本总数
     * @return 版本数量
     */
    size_t versionCount() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_versions.size();
    }

    /**
     * @brief 垃圾回收，保留最近 N 个版本
     * @param keepVersions 保留的版本数量
     */
    void gc(size_t keepVersions) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        while (m_versions.size() > keepVersions) {
            m_versions.erase(m_versions.begin());
        }
    }

    /**
     * @brief 垃圾回收，删除早于指定版本的所有版本
     * @param olderThan 版本号阈值
     */
    void gcOlderThan(Version olderThan) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        auto it = m_versions.begin();
        while (it != m_versions.end() && it->first < olderThan) {
            it = m_versions.erase(it);
        }
    }

    /**
     * @brief 获取所有版本号列表
     * @return 版本号向量
     */
    std::vector<Version> getAllVersions() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);

        std::vector<Version> result;
        result.reserve(m_versions.size());
        for (const auto& [ver, _] : m_versions) {
            result.push_back(ver);
        }
        return result;
    }

    void clear() {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_versions.clear();
        m_currentVersion = 0;
    }

private:
    mutable std::shared_mutex m_mutex;
    std::atomic<Version> m_currentVersion;
    std::map<Version, std::unique_ptr<VersionedValue<T>>> m_versions;
};

/**
 * @brief 快照类
 * @details 绑定到特定版本号的只读快照，用于一致性读取。
 */
class Snapshot {
public:
    /**
     * @brief 构造快照
     * @param version 快照版本号
     */
    explicit Snapshot(Version version) : m_version(version) {}

    /**
     * @brief 获取快照版本号
     * @return 版本号
     */
    Version version() const { return m_version; }

    /**
     * @brief 通过快照读取 MVCC 容器中的值
     * @tparam T 值类型
     * @param mvcc MVCC 容器引用
     * @return 值指针
     */
    template<typename T>
    const T* read(const Mvcc<T>& mvcc) const {
        return mvcc.getValue(m_version);
    }

private:
    Version m_version;
};

/**
 * @brief 事务类
 * @details 封装 MVCC 事务操作，包括读取、写入和提交。
 * @tparam T 值类型
 */
template<typename T>
class Transaction {
public:
    /**
     * @brief 构造事务
     * @param mvcc MVCC 容器引用
     */
    explicit Transaction(Mvcc<T>& mvcc)
        : m_mvcc(mvcc)
        , m_startVersion(mvcc.currentVersion())
        , m_committed(false) {}

    /**
     * @brief 读取事务开始时的值
     * @return 值指针
     */
    const T* read() const {
        return m_mvcc.getValue(m_startVersion);
    }

    /**
     * @brief 写入待提交的值
     * @param value 新值
     */
    void write(std::unique_ptr<T> value) {
        m_pendingValue = std::move(value);
    }

    /**
     * @brief 提交事务（使用 CAS 保证原子性）
     * @return 成功返回 true
     */
    bool commit() {
        if (m_committed || !m_pendingValue) {
            return false;
        }

        Version newVersion = m_mvcc.compareAndSwap(m_startVersion, std::move(m_pendingValue));
        m_committed = (newVersion != 0);
        return m_committed;
    }

    /**
     * @brief 检查事务是否已提交
     * @return 已提交返回 true
     */
    bool isCommitted() const { return m_committed; }

private:
    Mvcc<T>& m_mvcc;
    Version m_startVersion;
    std::unique_ptr<T> m_pendingValue;
    bool m_committed;
};

} // namespace galay::utils

#endif // GALAY_UTILS_MVCC_HPP
