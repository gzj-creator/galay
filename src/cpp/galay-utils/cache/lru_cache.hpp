/**
 * @file lru_cache.hpp
 * @brief 通用 LRU 缓存
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供非线程安全的泛型 LRU 缓存，支持容量惰性淘汰、TTL 惰性淘汰、
 *          自定义哈希/比较器、测试时钟和淘汰回调。
 */

#ifndef GALAY_UTILS_CACHE_LRU_CACHE_HPP
#define GALAY_UTILS_CACHE_LRU_CACHE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <queue>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace galay::utils {

/**
 * @brief 非线程安全的泛型 LRU 缓存
 * @details
 * - 使用最近访问顺序淘汰数据，最新访问的元素位于缓存头部。
 * - 容量淘汰和时间淘汰都是惰性的，只在调用缓存 API 时执行。
 * - 不创建后台线程，不使用定时器，不在内部使用 mutex。
 *
 * @warning 本类不提供线程安全保证。多线程或跨协程并发访问同一个实例时，
 *          调用方必须在外部同步；本类不会隐藏任何可能阻塞协程的锁。
 *
 * @tparam Key 键类型
 * @tparam Value 值类型
 * @tparam Hash 键哈希函数
 * @tparam KeyEqual 键相等比较函数
 * @tparam Clock TTL 使用的时钟类型，默认使用 std::chrono::steady_clock
 * @tparam EnableStats 是否启用运行统计；默认关闭以避免热路径计数开销
 */
template<typename Key,
         typename Value,
         typename Hash = std::hash<Key>,
         typename KeyEqual = std::equal_to<Key>,
         typename Clock = std::chrono::steady_clock,
         bool EnableStats = false>
class LruCache {
public:
    using key_type = Key; ///< 键类型
    using mapped_type = Value; ///< 值类型
    using size_type = std::size_t; ///< 容量和数量类型
    using clock_type = Clock; ///< 时钟类型
    using duration = typename Clock::duration; ///< TTL 时长类型
    using time_point = typename Clock::time_point; ///< 过期时间点类型

    /**
     * @brief 淘汰原因
     */
    enum class EvictReason {
        Capacity, ///< 因容量限制被淘汰
        Expired, ///< 因 TTL 到期被淘汰
        Removed, ///< 因 remove() 被移除
        Cleared ///< 因 clear() 被清空
    };

    /**
     * @brief TTL 过期刷新策略
     */
    enum class ExpirationPolicy {
        ExpireAfterWrite, ///< 写入后按固定过期时间淘汰
        ExpireAfterAccess ///< 每次 get() 命中后刷新过期时间
    };

    /**
     * @brief 缓存运行统计
     * @details 统计项按条目事件计数；clear() 会按被清除条目数累计 clears。
     *          只有 EnableStats=true 时才会收集；默认关闭时 stats() 始终返回零值快照。
     */
    struct Stats {
        std::uint64_t hits = 0; ///< 成功命中的读取次数
        std::uint64_t misses = 0; ///< 未命中的读取次数
        std::uint64_t inserts = 0; ///< 新插入条目次数
        std::uint64_t updates = 0; ///< 更新已有条目次数
        std::uint64_t capacityEvictions = 0; ///< 容量淘汰条目数
        std::uint64_t expiredEvictions = 0; ///< TTL 过期淘汰条目数
        std::uint64_t removes = 0; ///< remove() 移除条目数
        std::uint64_t clears = 0; ///< clear() 清除条目数
    };

    /**
     * @brief 淘汰回调类型
     * @details 参数依次为被淘汰的 key、value 和淘汰原因。回调中不要重入操作同一个缓存实例。
     */
    using EvictCallback = std::function<void(const Key&, const Value&, EvictReason)>;

    /**
     * @brief 判断当前缓存类型是否启用统计收集
     * @return EnableStats 模板参数值
     */
    static constexpr bool statsEnabled() noexcept {
        return EnableStats;
    }

    /**
     * @brief 构造 LRU 缓存
     * @param capacity 最大容量，0 表示不保存任何元素
     * @param defaultTtl 默认 TTL；为 std::nullopt 时元素默认不过期
     * @param onEvict 可选淘汰回调
     */
    explicit LruCache(size_type capacity = 0,
                      std::optional<duration> defaultTtl = std::nullopt,
                      EvictCallback onEvict = nullptr,
                      ExpirationPolicy expirationPolicy = ExpirationPolicy::ExpireAfterWrite)
        : m_capacity(capacity)
        , m_defaultTtl(defaultTtl)
        , m_onEvict(std::move(onEvict))
        , m_expirationPolicy(expirationPolicy) {}

    /**
     * @brief 构造带默认 TTL 的 LRU 缓存
     * @param capacity 最大容量，0 表示不保存任何元素
     * @param defaultTtl 默认 TTL，支持任意可转换到 Clock::duration 的 chrono duration
     * @param onEvict 可选淘汰回调
     */
    template<typename Rep, typename Period>
    LruCache(size_type capacity,
             std::chrono::duration<Rep, Period> defaultTtl,
             EvictCallback onEvict = nullptr,
             ExpirationPolicy expirationPolicy = ExpirationPolicy::ExpireAfterWrite)
        : LruCache(capacity,
                   std::optional<duration>(std::chrono::duration_cast<duration>(defaultTtl)),
                   std::move(onEvict),
                   expirationPolicy) {}

    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;
    LruCache(LruCache&&) = delete;
    LruCache& operator=(LruCache&&) = delete;

    /**
     * @brief 写入或更新缓存值，使用默认 TTL
     * @param key 缓存键
     * @param value 缓存值
     * @return 写入后元素仍保存在缓存中返回 true，否则返回 false
     */
    template<typename K, typename V>
    bool put(K&& key, V&& value) {
        return putWithExpiration(std::forward<K>(key), std::forward<V>(value), expirationFromTtl(m_defaultTtl));
    }

    /**
     * @brief 写入或更新缓存值，并指定本次写入的 TTL
     * @param key 缓存键
     * @param value 缓存值
     * @param ttl 本条记录的 TTL
     * @return 写入后元素仍保存在缓存中返回 true，否则返回 false
     */
    template<typename K, typename V>
    bool putFor(K&& key, V&& value, duration ttl) {
        return putWithExpiration(std::forward<K>(key), std::forward<V>(value), expirationFromTtl(ttl));
    }

    /**
     * @brief 写入或更新缓存值，并指定绝对过期时间
     * @param key 缓存键
     * @param value 缓存值
     * @param expiresAt 过期时间点
     * @return 写入后元素仍保存在缓存中返回 true，否则返回 false
     */
    template<typename K, typename V>
    bool putUntil(K&& key, V&& value, time_point expiresAt) {
        return putWithExpiration(std::forward<K>(key), std::forward<V>(value), Expiration{expiresAt, std::nullopt});
    }

    /**
     * @brief 原地构造或替换缓存值，使用默认 TTL
     * @param key 缓存键
     * @param args 构造 Value 的参数
     * @return 写入后元素仍保存在缓存中返回 true，否则返回 false
     */
    template<typename K, typename... Args>
    bool emplace(K&& key, Args&&... args) {
        return emplaceWithExpiration(std::forward<K>(key), expirationFromTtl(m_defaultTtl),
                                     std::forward<Args>(args)...);
    }

    /**
     * @brief 原地构造或替换缓存值，并指定本次写入的 TTL
     * @param key 缓存键
     * @param ttl 本条记录的 TTL
     * @param args 构造 Value 的参数
     * @return 写入后元素仍保存在缓存中返回 true，否则返回 false
     */
    template<typename K, typename... Args>
    bool emplaceFor(K&& key, duration ttl, Args&&... args) {
        return emplaceWithExpiration(std::forward<K>(key), expirationFromTtl(ttl),
                                     std::forward<Args>(args)...);
    }

    /**
     * @brief 预留内部哈希表容量
     * @param count 预期条目数量
     * @note 只影响哈希表分配策略，不改变缓存容量上限。
     */
    void reserve(size_type count) {
        m_index.reserve(count);
    }

    /**
     * @brief 设置内部哈希表最大负载因子
     * @param factor 最大负载因子
     */
    void maxLoadFactor(float factor) {
        m_index.max_load_factor(factor);
    }

    /**
     * @brief 获取内部哈希表最大负载因子
     * @return 最大负载因子
     */
    float maxLoadFactor() const {
        return m_index.max_load_factor();
    }

    /**
     * @brief 获取缓存统计快照
     * @return 当前统计值副本；未启用统计时返回零值快照
     */
    Stats stats() const {
        if constexpr (EnableStats) {
            return m_stats;
        } else {
            return Stats{};
        }
    }

    /**
     * @brief 重置缓存统计；未启用统计时为空操作
     */
    void resetStats() {
        if constexpr (EnableStats) {
            m_stats = Stats{};
        }
    }

    /**
     * @brief 惰性清理已过期条目
     * @return 本次实际清理的条目数
     */
    size_type purgeExpired() const {
        return purgeExpiredImpl();
    }

    /**
     * @brief 获取缓存值并刷新其 LRU 顺序
     * @param key 缓存键
     * @return 命中返回值指针，未命中或已过期返回 nullptr
     * @note 返回指针在该元素被移除、被替换或缓存被清空前有效。
     */
    Value* get(const Key& key) {
        purgeExpired();

        auto it = m_index.find(key);
        if (it == m_index.end()) {
            recordMiss();
            return nullptr;
        }

        recordHit();
        touch(it->second);
        refreshExpirationAfterAccess(it->second);
        return std::addressof(it->second->value);
    }

    /**
     * @brief 获取缓存值并刷新其 LRU 顺序
     * @param key 缓存键
     * @return 命中返回只读值指针，未命中或已过期返回 nullptr
     * @note const 版本仍会更新缓存的内部访问顺序，这是缓存的逻辑 const 行为。
     */
    const Value* get(const Key& key) const {
        purgeExpired();

        auto it = m_index.find(key);
        if (it == m_index.end()) {
            recordMiss();
            return nullptr;
        }

        recordHit();
        touch(it->second);
        refreshExpirationAfterAccess(it->second);
        return std::addressof(it->second->value);
    }

    /**
     * @brief 查看缓存值但不刷新 LRU 顺序
     * @param key 缓存键
     * @return 命中返回值指针，未命中或已过期返回 nullptr
     */
    Value* peek(const Key& key) {
        purgeExpired();

        auto it = m_index.find(key);
        if (it == m_index.end()) {
            recordMiss();
            return nullptr;
        }
        recordHit();
        return std::addressof(it->second->value);
    }

    /**
     * @brief 查看缓存值但不刷新 LRU 顺序
     * @param key 缓存键
     * @return 命中返回只读值指针，未命中或已过期返回 nullptr
     */
    const Value* peek(const Key& key) const {
        purgeExpired();

        auto it = m_index.find(key);
        if (it == m_index.end()) {
            recordMiss();
            return nullptr;
        }
        recordHit();
        return std::addressof(it->second->value);
    }

    /**
     * @brief 判断键是否存在
     * @param key 缓存键
     * @return 存在且未过期返回 true，否则返回 false
     * @note 该操作会惰性清理过期元素，但不会刷新 LRU 顺序。
     */
    bool contains(const Key& key) const {
        purgeExpired();
        const bool found = m_index.find(key) != m_index.end();
        if (found) {
            recordHit();
        } else {
            recordMiss();
        }
        return found;
    }

    /**
     * @brief 移除指定键
     * @param key 缓存键
     * @return 成功移除返回 true，键不存在或已过期返回 false
     */
    bool remove(const Key& key) {
        purgeExpired();

        auto it = m_index.find(key);
        if (it == m_index.end()) {
            return false;
        }

        eraseEntry(it, EvictReason::Removed);
        return true;
    }

    /**
     * @brief 清空缓存
     * @details 会对当前仍存在的元素触发 Cleared 淘汰回调。
     */
    void clear() {
        while (!m_items.empty()) {
            auto it = m_index.find(m_items.back().key);
            if (it == m_index.end()) {
                m_items.pop_back();
                continue;
            }
            eraseEntry(it, EvictReason::Cleared);
        }

        while (!m_expirations.empty()) {
            m_expirations.pop();
        }
    }

    /**
     * @brief 获取当前缓存元素数量
     * @return 当前未过期元素数量
     * @note 该操作会惰性清理过期元素。
     */
    size_type size() const {
        purgeExpired();
        return m_items.size();
    }

    /**
     * @brief 判断缓存是否为空
     * @return 为空返回 true
     * @note 该操作会惰性清理过期元素。
     */
    bool empty() const {
        return size() == 0;
    }

    /**
     * @brief 获取最大容量
     * @return 最大容量
     */
    size_type capacity() const {
        return m_capacity;
    }

    /**
     * @brief 设置最大容量
     * @param capacity 新容量，0 表示不保存任何元素
     * @details 设置后会在本次 API 调用中惰性清理过期元素并执行容量淘汰。
     */
    void setCapacity(size_type capacity) {
        m_capacity = capacity;
        purgeExpired();
        enforceCapacity();
    }

    /**
     * @brief 获取默认 TTL
     * @return 默认 TTL；std::nullopt 表示默认不过期
     */
    std::optional<duration> defaultTtl() const {
        return m_defaultTtl;
    }

    /**
     * @brief 设置默认 TTL
     * @param ttl 默认 TTL；std::nullopt 表示默认不过期
     * @note 只影响后续写入，不改变已有元素的过期时间。
     */
    void setDefaultTtl(std::optional<duration> ttl) {
        m_defaultTtl = ttl;
    }

private:
    struct Entry {
        Key key;
        Value value;
        std::optional<time_point> expiresAt;
        std::optional<duration> ttl;
        std::uint64_t version;
    };

    struct Expiration {
        std::optional<time_point> expiresAt;
        std::optional<duration> ttl;
    };

    struct ExpireNode {
        time_point expiresAt;
        Key key;
        std::uint64_t version;
    };

    struct ExpireCompare {
        bool operator()(const ExpireNode& lhs, const ExpireNode& rhs) const {
            return lhs.expiresAt > rhs.expiresAt;
        }
    };

    using ItemList = std::list<Entry>;
    using ListIterator = typename ItemList::iterator;
    using IndexMap = std::unordered_map<Key, ListIterator, Hash, KeyEqual>;

    Expiration expirationFromTtl(std::optional<duration> ttl) const {
        if (!ttl.has_value()) {
            return Expiration{std::nullopt, std::nullopt};
        }
        return Expiration{Clock::now() + *ttl, ttl};
    }

    Expiration expirationFromTtl(duration ttl) const {
        return Expiration{Clock::now() + ttl, ttl};
    }

    bool isExpired(const std::optional<time_point>& expiresAt) const {
        return expiresAt.has_value() && *expiresAt <= Clock::now();
    }

    template<typename K, typename V>
    bool putWithExpiration(K&& key, V&& value, Expiration expiration) {
        purgeExpired();

        Key normalizedKey(std::forward<K>(key));
        if (isExpired(expiration.expiresAt)) {
            removeExpiredKey(normalizedKey);
            return false;
        }

        if (m_capacity == 0) {
            removeCapacityKey(normalizedKey);
            return false;
        }

        auto indexIt = m_index.find(normalizedKey);
        if (indexIt != m_index.end()) {
            auto itemIt = indexIt->second;
            itemIt->value = std::forward<V>(value);
            updateExpiration(itemIt, expiration);
            touch(itemIt);
            recordUpdate();
            return true;
        }

        m_items.push_front(Entry{
            std::move(normalizedKey),
            Value(std::forward<V>(value)),
            std::nullopt,
            std::nullopt,
            nextVersion()
        });

        auto itemIt = m_items.begin();
        auto [insertedIt, inserted] = m_index.emplace(itemIt->key, itemIt);
        if (!inserted || insertedIt == m_index.end()) {
            m_items.pop_front();
            return false;
        }
        updateExpiration(itemIt, expiration);
        recordInsert();
        enforceCapacity();
        return true;
    }

    template<typename K, typename... Args>
    bool emplaceWithExpiration(K&& key, Expiration expiration, Args&&... args) {
        purgeExpired();

        Key normalizedKey(std::forward<K>(key));
        if (isExpired(expiration.expiresAt)) {
            removeExpiredKey(normalizedKey);
            return false;
        }

        if (m_capacity == 0) {
            removeCapacityKey(normalizedKey);
            return false;
        }

        auto indexIt = m_index.find(normalizedKey);
        if (indexIt != m_index.end()) {
            auto itemIt = indexIt->second;
            itemIt->value = Value(std::forward<Args>(args)...);
            updateExpiration(itemIt, expiration);
            touch(itemIt);
            recordUpdate();
            return true;
        }

        m_items.push_front(Entry{
            std::move(normalizedKey),
            Value(std::forward<Args>(args)...),
            std::nullopt,
            std::nullopt,
            nextVersion()
        });

        auto itemIt = m_items.begin();
        auto [insertedIt, inserted] = m_index.emplace(itemIt->key, itemIt);
        if (!inserted || insertedIt == m_index.end()) {
            m_items.pop_front();
            return false;
        }
        updateExpiration(itemIt, expiration);
        recordInsert();
        enforceCapacity();
        return true;
    }

    std::uint64_t nextVersion() const {
        return ++m_nextVersion;
    }

    void updateExpiration(ListIterator it, const Expiration& expiration) const {
        it->expiresAt = expiration.expiresAt;
        it->ttl = expiration.ttl;
        it->version = nextVersion();

        if (expiration.expiresAt.has_value()) {
            m_expirations.push(ExpireNode{*expiration.expiresAt, it->key, it->version});
        }
    }

    void refreshExpirationAfterAccess(ListIterator it) const {
        if (m_expirationPolicy != ExpirationPolicy::ExpireAfterAccess || !it->ttl.has_value()) {
            return;
        }

        const auto now = Clock::now();
        if (it->expiresAt.has_value()) {
            const duration refreshInterval = *it->ttl / 4;
            const time_point lastRefresh = *it->expiresAt - *it->ttl;
            if (refreshInterval > duration::zero() && now - lastRefresh < refreshInterval) {
                return;
            }
        }

        updateExpiration(it, Expiration{now + *it->ttl, it->ttl});
    }

    void touch(ListIterator it) const {
        if (it != m_items.begin()) {
            m_items.splice(m_items.begin(), m_items, it);
        }
    }

    size_type purgeExpiredImpl() const {
        if (m_expirations.empty()) {
            return 0;
        }

        size_type removed = 0;
        const auto now = Clock::now();

        while (!m_expirations.empty()) {
            const auto& top = m_expirations.top();
            if (top.expiresAt > now) {
                break;
            }

            ExpireNode node = top;
            m_expirations.pop();

            auto indexIt = m_index.find(node.key);
            if (indexIt == m_index.end()) {
                continue;
            }

            auto itemIt = indexIt->second;
            if (!itemIt->expiresAt.has_value() ||
                itemIt->version != node.version ||
                *itemIt->expiresAt != node.expiresAt) {
                continue;
            }

            eraseEntry(indexIt, EvictReason::Expired);
            ++removed;
        }

        return removed;
    }

    void enforceCapacity() const {
        while (m_items.size() > m_capacity) {
            auto last = std::prev(m_items.end());
            auto indexIt = m_index.find(last->key);
            if (indexIt == m_index.end()) {
                auto next = m_items.erase(last);
                if (next != m_items.end()) {
                    continue;
                }
                continue;
            }
            eraseEntry(indexIt, EvictReason::Capacity);
        }
    }

    void removeExpiredKey(const Key& key) {
        auto indexIt = m_index.find(key);
        if (indexIt != m_index.end()) {
            eraseEntry(indexIt, EvictReason::Expired);
        }
    }

    void removeCapacityKey(const Key& key) {
        auto indexIt = m_index.find(key);
        if (indexIt != m_index.end()) {
            eraseEntry(indexIt, EvictReason::Capacity);
        }
    }

    void eraseEntry(typename IndexMap::iterator indexIt, EvictReason reason) const {
        auto itemIt = indexIt->second;
        notifyEvict(itemIt, reason);
        recordEviction(reason);
        auto nextIndex = m_index.erase(indexIt);
        if (nextIndex != m_index.end()) {
            // erase 返回值已显式处理；调用方不依赖后继位置。
        }
        auto nextItem = m_items.erase(itemIt);
        if (nextItem != m_items.end()) {
            // erase 返回值已显式处理；调用方不依赖后继位置。
        }
    }

    void notifyEvict(ListIterator it, EvictReason reason) const {
        if (m_onEvict) {
            m_onEvict(it->key, it->value, reason);
        }
    }

    void recordHit() const {
        if constexpr (EnableStats) {
            ++m_stats.hits;
        }
    }

    void recordMiss() const {
        if constexpr (EnableStats) {
            ++m_stats.misses;
        }
    }

    void recordInsert() const {
        if constexpr (EnableStats) {
            ++m_stats.inserts;
        }
    }

    void recordUpdate() const {
        if constexpr (EnableStats) {
            ++m_stats.updates;
        }
    }

    void recordEviction(EvictReason reason) const {
        if constexpr (EnableStats) {
            switch (reason) {
            case EvictReason::Capacity:
                ++m_stats.capacityEvictions;
                break;
            case EvictReason::Expired:
                ++m_stats.expiredEvictions;
                break;
            case EvictReason::Removed:
                ++m_stats.removes;
                break;
            case EvictReason::Cleared:
                ++m_stats.clears;
                break;
            }
        } else {
            (void)reason;
        }
    }

    struct DisabledStats {};
    using StatsStorage = std::conditional_t<EnableStats, Stats, DisabledStats>;

    mutable ItemList m_items;
    mutable IndexMap m_index;
    mutable std::priority_queue<ExpireNode, std::vector<ExpireNode>, ExpireCompare> m_expirations;
    mutable std::uint64_t m_nextVersion = 0;
    [[no_unique_address]] mutable StatsStorage m_stats;
    size_type m_capacity;
    std::optional<duration> m_defaultTtl;
    EvictCallback m_onEvict;
    ExpirationPolicy m_expirationPolicy;
};

} // namespace galay::utils

#endif // GALAY_UTILS_CACHE_LRU_CACHE_HPP
