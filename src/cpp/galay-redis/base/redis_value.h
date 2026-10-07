/**
 * @file redis_value.h
 * @brief Redis 值类型封装
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 定义 RedisValue 和 RedisAsyncValue 类，对 RESP 协议回复进行高级封装，
 *          支持 RESP2 和 RESP3 的所有数据类型，并提供类型判断和值转换方法。
 */

#ifndef GALAY_REDIS_VALUE_H
#define GALAY_REDIS_VALUE_H

#include "../protoc/redis_protocol.h"
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace galay::redis
{
    /**
     * @brief Redis 值类，基于 protocol::RedisReply 实现
     * @details 封装 RESP 协议回复，提供 RESP2/RESP3 类型判断和值转换接口。
     *          数组和 Map 转换结果会被缓存以避免重复转换。
     * @note 禁止拷贝，仅支持移动语义
     */
    class RedisValue
    {
    public:
        RedisValue();                               ///< 默认构造
        explicit RedisValue(protocol::RedisReply reply); ///< 从 RedisReply 构造
        RedisValue(RedisValue&& other) noexcept;     ///< 移动构造
        RedisValue& operator=(RedisValue&& other) noexcept; ///< 移动赋值
        RedisValue(const RedisValue&) = delete;      ///< 禁止拷贝
        RedisValue& operator=(const RedisValue&) = delete; ///< 禁止拷贝赋值

        /**
         * @brief 显式克隆 RedisValue
         * @return 独立复制后的 RedisValue，转换缓存会按需重建
         */
        [[nodiscard]] RedisValue clone() const;

        /**
         * @brief 从错误消息创建 RedisValue
         * @param error_msg 错误消息
         * @return 包含错误信息的 RedisValue
         */
        static RedisValue from_error(const std::string& error_msg);

        // RESP2 类型判断和转换
        bool is_null() const;                        ///< 判断是否为 Null
        bool is_status() const;                      ///< 判断是否为状态回复
        std::string to_status() const;               ///< 转换为状态字符串
        bool is_error() const;                       ///< 判断是否为错误回复
        std::string to_error() const;                ///< 转换为错误字符串
        bool is_integer() const;                     ///< 判断是否为整数
        int64_t to_integer() const;                  ///< 转换为整数
        bool is_string() const;                      ///< 判断是否为字符串
        std::string to_string() const;               ///< 转换为字符串
        bool is_array() const;                       ///< 判断是否为数组

        /**
         * @brief 转换为数组
         * @return RedisValue 数组
         * @note 返回的 vector 生命周期需小于等于 RedisValue 的生命周期
         */
        std::vector<RedisValue> to_array() const;

        // RESP3 类型判断和转换
        bool is_double() const;                      ///< 判断是否为双精度浮点数（RESP3）
        double to_double() const;                    ///< 转换为双精度浮点数
        bool is_bool() const;                        ///< 判断是否为布尔值（RESP3）
        bool to_bool() const;                        ///< 转换为布尔值
        bool is_map() const;                         ///< 判断是否为映射（RESP3）

        /**
         * @brief 转换为映射
         * @return 字符串到 RedisValue 的映射
         * @note 返回的 map 生命周期需小于等于 RedisValue 的生命周期
         */
        std::map<std::string, RedisValue> to_map() const;

        bool is_set() const;                         ///< 判断是否为集合（RESP3）

        /**
         * @brief 转换为集合
         * @return RedisValue 数组（集合元素）
         * @note 返回的 vector 生命周期需小于等于 RedisValue 的生命周期
         */
        std::vector<RedisValue> to_set() const;

        bool is_attr() const;                        ///< 判断是否为属性（RESP3）
        bool is_push() const;                        ///< 判断是否为推送（RESP3）

        /**
         * @brief 转换为推送数组
         * @return RedisValue 数组（推送消息）
         * @note 返回的 vector 生命周期需小于等于 RedisValue 的生命周期
         */
        std::vector<RedisValue> to_push() const;

        bool is_big_number() const;                   ///< 判断是否为大数字（RESP3）
        std::string to_big_number() const;            ///< 转换为大数字字符串

        bool is_verb() const;                        ///< 判断是否为原义字符串（RESP3，不转义）
        std::string to_verb() const;                 ///< 转换为原义字符串

        /**
         * @brief 获取底层 RedisReply（const）
         * @return 底层 RedisReply 的 const 引用
         */
        const protocol::RedisReply& get_reply() const { return m_reply; }

        /**
         * @brief 获取底层 RedisReply
         * @return 底层 RedisReply 的引用
         */
        protocol::RedisReply& get_reply() { return m_reply; }

        virtual ~RedisValue() = default;

    protected:
        protocol::RedisReply m_reply; ///< 底层协议回复对象

    private:
        // 缓存转换后的数组和 map
        mutable std::unique_ptr<std::vector<RedisValue>> m_cached_array;         ///< 缓存的数组转换结果
        mutable std::unique_ptr<std::map<std::string, RedisValue>> m_cached_map; ///< 缓存的映射转换结果
        mutable bool m_array_cached = false;  ///< 数组缓存是否有效
        mutable bool m_map_cached = false;    ///< 映射缓存是否有效
    };

    /**
     * @brief 异步 Redis 值类
     * @details 继承自 RedisValue，专用于异步场景的值类型，
     *          提供与 RedisValue 相同的接口但语义上区分同步和异步使用场景
     */
    class RedisAsyncValue: public RedisValue
    {
    public:
        RedisAsyncValue();                               ///< 默认构造
        explicit RedisAsyncValue(protocol::RedisReply reply); ///< 从 RedisReply 构造
        RedisAsyncValue(RedisAsyncValue&& other) noexcept;     ///< 移动构造
        RedisAsyncValue& operator=(RedisAsyncValue&& other) noexcept; ///< 移动赋值
        RedisAsyncValue(const RedisAsyncValue&) = delete;      ///< 禁止拷贝
        RedisAsyncValue& operator=(const RedisAsyncValue&) = delete; ///< 禁止拷贝赋值
        [[nodiscard]] RedisAsyncValue clone() const;           ///< 显式克隆异步值
        ~RedisAsyncValue() = default;
    };
}

#endif
