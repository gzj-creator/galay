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
         * @brief 从错误消息创建 RedisValue
         * @param error_msg 错误消息
         * @return 包含错误信息的 RedisValue
         */
        static RedisValue fromError(const std::string& error_msg);

        // RESP2 类型判断和转换
        bool isNull() const;                        ///< 判断是否为 Null
        bool isStatus() const;                      ///< 判断是否为状态回复
        std::string toStatus() const;               ///< 转换为状态字符串
        bool isError() const;                       ///< 判断是否为错误回复
        std::string toError() const;                ///< 转换为错误字符串
        bool isInteger() const;                     ///< 判断是否为整数
        int64_t toInteger() const;                  ///< 转换为整数
        bool isString() const;                      ///< 判断是否为字符串
        std::string toString() const;               ///< 转换为字符串
        bool isArray() const;                       ///< 判断是否为数组

        /**
         * @brief 转换为数组
         * @return RedisValue 数组
         * @note 返回的 vector 生命周期需小于等于 RedisValue 的生命周期
         */
        std::vector<RedisValue> toArray() const;

        // RESP3 类型判断和转换
        bool isDouble() const;                      ///< 判断是否为双精度浮点数（RESP3）
        double toDouble() const;                    ///< 转换为双精度浮点数
        bool isBool() const;                        ///< 判断是否为布尔值（RESP3）
        bool toBool() const;                        ///< 转换为布尔值
        bool isMap() const;                         ///< 判断是否为映射（RESP3）

        /**
         * @brief 转换为映射
         * @return 字符串到 RedisValue 的映射
         * @note 返回的 map 生命周期需小于等于 RedisValue 的生命周期
         */
        std::map<std::string, RedisValue> toMap() const;

        bool isSet() const;                         ///< 判断是否为集合（RESP3）

        /**
         * @brief 转换为集合
         * @return RedisValue 数组（集合元素）
         * @note 返回的 vector 生命周期需小于等于 RedisValue 的生命周期
         */
        std::vector<RedisValue> toSet() const;

        bool isAttr() const;                        ///< 判断是否为属性（RESP3）
        bool isPush() const;                        ///< 判断是否为推送（RESP3）

        /**
         * @brief 转换为推送数组
         * @return RedisValue 数组（推送消息）
         * @note 返回的 vector 生命周期需小于等于 RedisValue 的生命周期
         */
        std::vector<RedisValue> toPush() const;

        bool isBigNumber() const;                   ///< 判断是否为大数字（RESP3）
        std::string toBigNumber() const;            ///< 转换为大数字字符串

        bool isVerb() const;                        ///< 判断是否为原义字符串（RESP3，不转义）
        std::string toVerb() const;                 ///< 转换为原义字符串

        /**
         * @brief 获取底层 RedisReply（const）
         * @return 底层 RedisReply 的 const 引用
         */
        const protocol::RedisReply& getReply() const { return m_reply; }

        /**
         * @brief 获取底层 RedisReply
         * @return 底层 RedisReply 的引用
         */
        protocol::RedisReply& getReply() { return m_reply; }

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
        ~RedisAsyncValue() = default;
    };
}

#endif
