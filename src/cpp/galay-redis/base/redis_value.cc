#include "redis_value.h"

namespace galay::redis
{
    // RedisValue实现
    RedisValue::RedisValue()
        : m_reply()
    {
    }

    RedisValue::RedisValue(protocol::RedisReply reply)
        : m_reply(std::move(reply))
    {
    }

    RedisValue::RedisValue(RedisValue&& other) noexcept
        : m_reply(std::move(other.m_reply))
        , m_cached_array(std::move(other.m_cached_array))
        , m_cached_map(std::move(other.m_cached_map))
        , m_array_cached(other.m_array_cached)
        , m_map_cached(other.m_map_cached)
    {
        other.m_array_cached = false;
        other.m_map_cached = false;
    }

    RedisValue& RedisValue::operator=(RedisValue&& other) noexcept
    {
        if (this != &other) {
            m_reply = std::move(other.m_reply);
            m_cached_array = std::move(other.m_cached_array);
            m_cached_map = std::move(other.m_cached_map);
            m_array_cached = other.m_array_cached;
            m_map_cached = other.m_map_cached;
            other.m_array_cached = false;
            other.m_map_cached = false;
        }
        return *this;
    }

    RedisValue RedisValue::clone() const
    {
        return RedisValue(m_reply.clone());
    }

    // 静态工厂方法：创建错误类型的RedisValue
    RedisValue RedisValue::from_error(const std::string& error_msg)
    {
        protocol::RedisReply reply(protocol::RespType::Error, error_msg);
        return RedisValue(std::move(reply));
    }

    bool RedisValue::is_null() const
    {
        return m_reply.is_null();
    }

    bool RedisValue::is_status() const
    {
        return m_reply.is_simple_string();
    }

    std::string RedisValue::to_status() const
    {
        return m_reply.as_string();
    }

    bool RedisValue::is_error() const
    {
        return m_reply.is_error();
    }

    std::string RedisValue::to_error() const
    {
        return m_reply.as_string();
    }

    bool RedisValue::is_integer() const
    {
        return m_reply.is_integer();
    }

    int64_t RedisValue::to_integer() const
    {
        return m_reply.as_integer();
    }

    bool RedisValue::is_string() const
    {
        return m_reply.is_bulk_string();
    }

    std::string RedisValue::to_string() const
    {
        return m_reply.as_string();
    }

    bool RedisValue::is_array() const
    {
        return m_reply.is_array();
    }

    std::vector<RedisValue> RedisValue::to_array() const
    {
        if (!m_array_cached) {
            if (!m_cached_array) {
                m_cached_array = std::make_unique<std::vector<RedisValue>>();
            }
            auto& cache = *m_cached_array;
            cache.clear();
            if (m_reply.is_array()) {
                const auto& arr = m_reply.as_array();
                cache.reserve(arr.size());
                for (const auto& elem : arr) {
                    cache.emplace_back(elem.clone());
                }
            }
            m_array_cached = true;
        }
        // 返回拷贝，保持接口不变
        std::vector<RedisValue> result;
        if (!m_cached_array) {
            return result;
        }

        result.reserve(m_cached_array->size());
        for (const auto& elem : *m_cached_array) {
            result.emplace_back(elem.clone());
        }
        return result;
    }

    bool RedisValue::is_double() const
    {
        return m_reply.is_double();
    }

    double RedisValue::to_double() const
    {
        return m_reply.as_double();
    }

    bool RedisValue::is_bool() const
    {
        return m_reply.is_boolean();
    }

    bool RedisValue::to_bool() const
    {
        return m_reply.as_boolean();
    }

    bool RedisValue::is_map() const
    {
        return m_reply.is_map();
    }

    std::map<std::string, RedisValue> RedisValue::to_map() const
    {
        if (!m_map_cached) {
            if (!m_cached_map) {
                m_cached_map = std::make_unique<std::map<std::string, RedisValue>>();
            }
            auto& cache = *m_cached_map;
            cache.clear();
            if (m_reply.is_map()) {
                const auto& map_data = m_reply.as_map();
                for (const auto& [key, value] : map_data) {
                    cache.emplace(
                        key.as_string(),
                        RedisValue(value.clone())
                    );
                }
            }
            m_map_cached = true;
        }
        // 返回拷贝，保持接口不变
        std::map<std::string, RedisValue> result;
        if (!m_cached_map) {
            return result;
        }

        for (const auto& [key, value] : *m_cached_map) {
            result.emplace(key, value.clone());
        }
        return result;
    }

    bool RedisValue::is_set() const
    {
        return m_reply.is_set();
    }

    std::vector<RedisValue> RedisValue::to_set() const
    {
        std::vector<RedisValue> result;
        if (m_reply.is_set()) {
            const auto& set_data = m_reply.as_array();  // Set uses array internally
            result.reserve(set_data.size());
            for (const auto& elem : set_data) {
                result.push_back(RedisValue(elem.clone()));
            }
        }
        return result;
    }

    bool RedisValue::is_attr() const
    {
        return false;  // 暂未实现
    }

    bool RedisValue::is_push() const
    {
        return m_reply.is_push();
    }

    std::vector<RedisValue> RedisValue::to_push() const
    {
        std::vector<RedisValue> result;
        if (m_reply.is_push()) {
            const auto& push_data = m_reply.as_array();
            result.reserve(push_data.size());
            for (const auto& elem : push_data) {
                result.push_back(RedisValue(elem.clone()));
            }
        }
        return result;
    }

    bool RedisValue::is_big_number() const
    {
        return false;  // 暂未实现
    }

    std::string RedisValue::to_big_number() const
    {
        return "";  // 暂未实现
    }

    bool RedisValue::is_verb() const
    {
        return false;  // 暂未实现
    }

    std::string RedisValue::to_verb() const
    {
        return "";  // 暂未实现
    }

    // RedisAsyncValue实现
    RedisAsyncValue::RedisAsyncValue()
        : RedisValue()
    {
    }

    RedisAsyncValue::RedisAsyncValue(protocol::RedisReply reply)
        : RedisValue(std::move(reply))
    {
    }

    RedisAsyncValue::RedisAsyncValue(RedisAsyncValue&& other) noexcept
        : RedisValue(std::move(other))
    {
    }

    RedisAsyncValue& RedisAsyncValue::operator=(RedisAsyncValue&& other) noexcept
    {
        RedisValue::operator=(std::move(other));
        return *this;
    }

    RedisAsyncValue RedisAsyncValue::clone() const
    {
        return RedisAsyncValue(m_reply.clone());
    }
}
