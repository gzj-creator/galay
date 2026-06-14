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

    // 静态工厂方法：创建错误类型的RedisValue
    RedisValue RedisValue::fromError(const std::string& error_msg)
    {
        protocol::RedisReply reply(protocol::RespType::Error, error_msg);
        return RedisValue(std::move(reply));
    }

    bool RedisValue::isNull() const
    {
        return m_reply.isNull();
    }

    bool RedisValue::isStatus() const
    {
        return m_reply.isSimpleString();
    }

    std::string RedisValue::toStatus() const
    {
        return m_reply.asString();
    }

    bool RedisValue::isError() const
    {
        return m_reply.isError();
    }

    std::string RedisValue::toError() const
    {
        return m_reply.asString();
    }

    bool RedisValue::isInteger() const
    {
        return m_reply.isInteger();
    }

    int64_t RedisValue::toInteger() const
    {
        return m_reply.asInteger();
    }

    bool RedisValue::isString() const
    {
        return m_reply.isBulkString();
    }

    std::string RedisValue::toString() const
    {
        return m_reply.asString();
    }

    bool RedisValue::isArray() const
    {
        return m_reply.isArray();
    }

    std::vector<RedisValue> RedisValue::toArray() const
    {
        if (!m_array_cached) {
            if (!m_cached_array) {
                m_cached_array = std::make_unique<std::vector<RedisValue>>();
            }
            auto& cache = *m_cached_array;
            cache.clear();
            if (m_reply.isArray()) {
                const auto& arr = m_reply.asArray();
                cache.reserve(arr.size());
                for (const auto& elem : arr) {
                    cache.emplace_back(elem);
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
            result.emplace_back(elem.m_reply);
        }
        return result;
    }

    bool RedisValue::isDouble() const
    {
        return m_reply.isDouble();
    }

    double RedisValue::toDouble() const
    {
        return m_reply.asDouble();
    }

    bool RedisValue::isBool() const
    {
        return m_reply.isBoolean();
    }

    bool RedisValue::toBool() const
    {
        return m_reply.asBoolean();
    }

    bool RedisValue::isMap() const
    {
        return m_reply.isMap();
    }

    std::map<std::string, RedisValue> RedisValue::toMap() const
    {
        if (!m_map_cached) {
            if (!m_cached_map) {
                m_cached_map = std::make_unique<std::map<std::string, RedisValue>>();
            }
            auto& cache = *m_cached_map;
            cache.clear();
            if (m_reply.isMap()) {
                const auto& map_data = m_reply.asMap();
                for (const auto& [key, value] : map_data) {
                    cache.emplace(
                        key.asString(),
                        RedisValue(value)
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
            result.emplace(key, RedisValue(value.m_reply));
        }
        return result;
    }

    bool RedisValue::isSet() const
    {
        return m_reply.isSet();
    }

    std::vector<RedisValue> RedisValue::toSet() const
    {
        std::vector<RedisValue> result;
        if (m_reply.isSet()) {
            const auto& set_data = m_reply.asArray();  // Set uses array internally
            result.reserve(set_data.size());
            for (const auto& elem : set_data) {
                // 使用拷贝构造，避免 const_cast
                result.push_back(RedisValue(elem));
            }
        }
        return result;
    }

    bool RedisValue::isAttr() const
    {
        return false;  // 暂未实现
    }

    bool RedisValue::isPush() const
    {
        return m_reply.isPush();
    }

    std::vector<RedisValue> RedisValue::toPush() const
    {
        std::vector<RedisValue> result;
        if (m_reply.isPush()) {
            const auto& push_data = m_reply.asArray();
            result.reserve(push_data.size());
            for (const auto& elem : push_data) {
                // 使用拷贝构造，避免 const_cast
                result.push_back(RedisValue(elem));
            }
        }
        return result;
    }

    bool RedisValue::isBigNumber() const
    {
        return false;  // 暂未实现
    }

    std::string RedisValue::toBigNumber() const
    {
        return "";  // 暂未实现
    }

    bool RedisValue::isVerb() const
    {
        return false;  // 暂未实现
    }

    std::string RedisValue::toVerb() const
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
}
