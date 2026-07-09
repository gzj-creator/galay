/**
 * @file route_params.h
 * @brief HTTP 路由参数小容器
 * @author galay-http
 * @version 1.0.0
 *
 * @details 为动态路由热路径提供小型连续存储，避免每次匹配都构造 std::map 节点。
 */

#ifndef GALAY_HTTP_ROUTE_PARAMS_H
#define GALAY_HTTP_ROUTE_PARAMS_H

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::http
{

/**
 * @brief 路由参数小容器
 * @details 常见路由参数数量很少，前 8 个参数保存在对象内联数组中；
 *          超出后才使用 overflow vector。该类型拥有参数名和值，不保存外部
 *          path 的 string_view，因而可安全从 HttpRouter::findHandler 返回。
 * @note 该类型不是线程安全容器；应只在单个请求处理路径内移动和读取。
 */
class RouteParams
{
public:
    using Entry = std::pair<std::string, std::string>;
    static constexpr size_t kInlineCapacity = 8;

    RouteParams() = default;
    RouteParams(RouteParams&&) noexcept = default;
    RouteParams& operator=(RouteParams&&) noexcept = default;
    ~RouteParams() = default;

    /**
     * @brief 显式复制路由参数
     * @return 独立副本
     */
    RouteParams clone() const
    {
        RouteParams copy;
        for (size_t i = 0; i < m_inline_size; ++i) {
            const bool inserted = copy.emplace(m_inline[i].first, m_inline[i].second);
            if (!inserted) {
                copy.clear();
                return copy;
            }
        }
        for (const auto& entry : m_overflow) {
            const bool inserted = copy.emplace(entry.first, entry.second);
            if (!inserted) {
                copy.clear();
                return copy;
            }
        }
        return copy;
    }

    /**
     * @brief 清空参数但保留已分配的 overflow 容量以便复用。
     */
    void clear() noexcept
    {
        for (size_t i = 0; i < m_inline_size; ++i) {
            m_inline[i].first.clear();
            m_inline[i].second.clear();
        }
        m_inline_size = 0;
        m_overflow.clear();
    }

    /**
     * @brief 参数是否为空
     * @return 无参数返回 true
     */
    bool empty() const noexcept
    {
        return size() == 0;
    }

    /**
     * @brief 参数数量
     * @return 当前参数数量
     */
    size_t size() const noexcept
    {
        return m_inline_size + m_overflow.size();
    }

    /**
     * @brief 插入或更新参数
     * @param name 参数名
     * @param value 参数值
     * @return 当前实现除分配异常外总返回 true
     */
    bool emplace(std::string_view name, std::string_view value)
    {
        if (Entry* existing = findMutable(name); existing != nullptr) {
            assignString(existing->second, value);
            return true;
        }

        if (m_inline_size < kInlineCapacity) {
            Entry& entry = m_inline[m_inline_size];
            assignString(entry.first, name);
            assignString(entry.second, value);
            ++m_inline_size;
            return true;
        }

        Entry next;
        assignString(next.first, name);
        assignString(next.second, value);
        Entry& inserted = m_overflow.emplace_back(std::move(next));
        return inserted.first.size() == name.size();
    }

    /**
     * @brief 查找参数值
     * @param name 参数名
     * @return 参数值指针；不存在返回 nullptr
     */
    const std::string* find(std::string_view name) const noexcept
    {
        for (size_t i = 0; i < m_inline_size; ++i) {
            if (m_inline[i].first == name) {
                return &m_inline[i].second;
            }
        }
        for (const auto& entry : m_overflow) {
            if (entry.first == name) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    /**
     * @brief 检查参数是否存在
     * @param name 参数名
     * @return 存在返回 true
     */
    bool contains(std::string_view name) const noexcept
    {
        return find(name) != nullptr;
    }

    /**
     * @brief map 兼容访问；不存在时插入空值。
     * @param name 参数名
     * @return 参数值引用
     */
    std::string& operator[](std::string_view name)
    {
        if (Entry* existing = findMutable(name); existing != nullptr) {
            return existing->second;
        }

        if (m_inline_size < kInlineCapacity) {
            Entry& entry = m_inline[m_inline_size];
            assignString(entry.first, name);
            entry.second.clear();
            ++m_inline_size;
            return entry.second;
        }

        Entry next;
        assignString(next.first, name);
        Entry& inserted = m_overflow.emplace_back(std::move(next));
        return inserted.second;
    }

    /**
     * @brief 物化为 std::map，兼容旧 routeParams() API。
     * @return 参数 map 副本
     */
    std::map<std::string, std::string> toMap() const
    {
        std::map<std::string, std::string> params;
        for (size_t i = 0; i < m_inline_size; ++i) {
            upsertMap(params, m_inline[i]);
        }
        for (const auto& entry : m_overflow) {
            upsertMap(params, entry);
        }
        return params;
    }

private:
    RouteParams(const RouteParams&) = delete;
    RouteParams& operator=(const RouteParams&) = delete;

    static void assignString(std::string& target, std::string_view value)
    {
        std::string next(value);
        target.swap(next);
    }

    static void upsertMap(std::map<std::string, std::string>& params, const Entry& entry)
    {
        auto [it, inserted] = params.emplace(entry.first, entry.second);
        if (!inserted) {
            std::string next_value(entry.second);
            it->second.swap(next_value);
        }
    }

    Entry* findMutable(std::string_view name) noexcept
    {
        for (size_t i = 0; i < m_inline_size; ++i) {
            if (m_inline[i].first == name) {
                return &m_inline[i];
            }
        }
        for (auto& entry : m_overflow) {
            if (entry.first == name) {
                return &entry;
            }
        }
        return nullptr;
    }

    std::array<Entry, kInlineCapacity> m_inline;
    std::vector<Entry> m_overflow;
    size_t m_inline_size = 0;
};

} // namespace galay::http

#endif
