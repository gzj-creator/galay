/**
 * @file builder.h
 * @brief Redis 命令构建器
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 提供 Redis 命令的 RESP 编码和批量构建功能，
 *          支持单条命令构建、Pipeline 批量构建和预编码快速路径。
 */

#ifndef GALAY_REDIS_BUILDER_H
#define GALAY_REDIS_BUILDER_H

#include "redis_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace galay::redis
{
    /**
     * @brief Redis 命令视图结构
     * @details 轻量级命令描述，包含命令名、参数列表和可选的预编码数据
     */
    struct RedisCommandView
    {
        std::string_view command;                          ///< 命令名
        std::span<const std::string_view> args;            ///< 参数列表
        std::string_view encoded;                          ///< 预编码的 RESP 命令字节（用于快速 Pipeline 批量发送）
    };

    /**
     * @brief Redis 已编码命令结构
     * @details 包含编码后的 RESP 命令字符串和期望的回复数量
     */
    struct RedisEncodedCommand
    {
        std::string encoded;            ///< 已编码的 RESP 命令字符串
        size_t expected_replies = 1;    ///< 期望的回复数量
    };

    /**
     * @brief Redis 命令构建器
     * @details 提供命令的编码和批量构建功能，支持连接命令、发布订阅、
     *          String、Hash、List、Set、Sorted Set 等常用操作。
     *          内部使用存储池避免频繁内存分配。
     */
    class RedisCommandBuilder
    {
    public:
        RedisCommandBuilder() = default;

        /**
         * @brief 清空所有已添加的命令
         */
        void clear() noexcept;

        /**
         * @brief 批量预留空间
         * @param command_count 命令数量
         * @param arg_count 参数数量
         * @param storage_bytes 存储字节数
         */
        void reserve(size_t command_count, size_t arg_count, size_t storage_bytes);

        /**
         * @brief 追加无参数命令
         * @param cmd 命令名
         * @return 构建器引用
         */
        RedisCommandBuilder& append(std::string_view cmd);

        /**
         * @brief 追加带参数命令（span 版本）
         * @param cmd 命令名
         * @param args 参数列表
         * @return 构建器引用
         */
        RedisCommandBuilder& append(std::string_view cmd,
                                    std::span<const std::string_view> args);

        /**
         * @brief 追加带参数命令（初始化列表版本）
         * @param cmd 命令名
         * @param args 参数初始化列表
         * @return 构建器引用
         */
        RedisCommandBuilder& append(std::string_view cmd,
                                    std::initializer_list<std::string_view> args);

        /**
         * @brief 追加带参数命令（array 版本）
         * @tparam N 数组大小
         * @param cmd 命令名
         * @param args 参数数组
         * @return 构建器引用
         */
        template <size_t N>
        RedisCommandBuilder& append(std::string_view cmd,
                                    const std::array<std::string_view, N>& args);

        /**
         * @brief 获取已添加的命令视图列表
         * @return 命令视图数组
         */
        [[nodiscard]] std::span<const RedisCommandView> commands() const;

        /**
         * @brief 获取已添加的命令数量
         * @return 命令数量
         */
        [[nodiscard]] size_t size() const noexcept;

        /**
         * @brief 获取所有命令的编码数据
         * @return 编码后的字符串引用
         */
        [[nodiscard]] const std::string& encoded() const noexcept;

        /**
         * @brief 检查是否没有命令
         * @return 无命令返回 true
         */
        [[nodiscard]] bool empty() const noexcept;

        /**
         * @brief 构建已编码命令（保留内部数据）
         * @return 已编码命令结构
         */
        [[nodiscard]] RedisEncodedCommand build() const;

        /**
         * @brief 构建并清空内部数据（移动语义）
         * @return 已编码命令结构
         */
        [[nodiscard]] RedisEncodedCommand release();

        /**
         * @brief 构建单条无参数命令
         * @param cmd 命令名
         * @param expected_replies 期望回复数
         * @return 已编码命令
         */
        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  size_t expected_replies = 1) const;

        /**
         * @brief 构建单条带参数命令（span 版本）
         * @param cmd 命令名
         * @param args 参数列表
         * @param expected_replies 期望回复数
         * @return 已编码命令
         */
        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  std::span<const std::string_view> args,
                                                  size_t expected_replies = 1) const;

        /**
         * @brief 构建单条带参数命令（初始化列表版本）
         */
        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  std::initializer_list<std::string_view> args,
                                                  size_t expected_replies = 1) const;

        /**
         * @brief 构建单条带参数命令（array 版本）
         */
        template <size_t N>
        [[nodiscard]] RedisEncodedCommand command(
            std::string_view cmd,
            const std::array<std::string_view, N>& args,
            size_t expected_replies = 1) const;

        /**
         * @brief 构建单条带参数命令（vector 版本）
         */
        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  const std::vector<std::string>& args,
                                                  size_t expected_replies = 1) const;

        // ======================== 连接命令 ========================
        [[nodiscard]] RedisEncodedCommand auth(const std::string& password) const; ///< AUTH 命令（仅密码）
        [[nodiscard]] RedisEncodedCommand auth(const std::string& username,
                                               const std::string& password) const; ///< AUTH 命令（用户名+密码）
        [[nodiscard]] RedisEncodedCommand select(int32_t db_index) const; ///< SELECT 命令
        [[nodiscard]] RedisEncodedCommand ping() const;  ///< PING 命令
        [[nodiscard]] RedisEncodedCommand echo(const std::string& message) const; ///< ECHO 命令

        // ======================== 发布订阅 ========================
        [[nodiscard]] RedisEncodedCommand publish(const std::string& channel,
                                                  const std::string& message) const; ///< PUBLISH 命令
        [[nodiscard]] RedisEncodedCommand subscribe(const std::string& channel) const; ///< SUBSCRIBE 命令（单频道）
        [[nodiscard]] RedisEncodedCommand subscribe(const std::vector<std::string>& channels) const; ///< SUBSCRIBE 命令（多频道）
        [[nodiscard]] RedisEncodedCommand unsubscribe(const std::string& channel) const; ///< UNSUBSCRIBE 命令（单频道）
        [[nodiscard]] RedisEncodedCommand unsubscribe(const std::vector<std::string>& channels) const; ///< UNSUBSCRIBE 命令（多频道）
        [[nodiscard]] RedisEncodedCommand psubscribe(const std::string& pattern) const; ///< PSUBSCRIBE 命令（单模式）
        [[nodiscard]] RedisEncodedCommand psubscribe(const std::vector<std::string>& patterns) const; ///< PSUBSCRIBE 命令（多模式）
        [[nodiscard]] RedisEncodedCommand punsubscribe(const std::string& pattern) const; ///< PUNSUBSCRIBE 命令（单模式）
        [[nodiscard]] RedisEncodedCommand punsubscribe(const std::vector<std::string>& patterns) const; ///< PUNSUBSCRIBE 命令（多模式）

        // ======================== 集群/主从命令 ========================
        [[nodiscard]] RedisEncodedCommand role() const; ///< ROLE 命令
        [[nodiscard]] RedisEncodedCommand replicaof(const std::string& host, int32_t port) const; ///< REPLICAOF 命令
        [[nodiscard]] RedisEncodedCommand readonly() const; ///< READONLY 命令
        [[nodiscard]] RedisEncodedCommand readwrite() const; ///< READWRITE 命令
        [[nodiscard]] RedisEncodedCommand clusterInfo() const; ///< CLUSTER INFO 命令
        [[nodiscard]] RedisEncodedCommand clusterNodes() const; ///< CLUSTER NODES 命令
        [[nodiscard]] RedisEncodedCommand clusterSlots() const; ///< CLUSTER SLOTS 命令

        // ======================== String操作 ========================
        [[nodiscard]] RedisEncodedCommand get(const std::string& key) const; ///< GET 命令
        [[nodiscard]] RedisEncodedCommand set(const std::string& key,
                                              const std::string& value) const; ///< SET 命令
        [[nodiscard]] RedisEncodedCommand setex(const std::string& key,
                                                int64_t seconds,
                                                const std::string& value) const; ///< SETEX 命令
        [[nodiscard]] RedisEncodedCommand del(const std::string& key) const; ///< DEL 命令
        [[nodiscard]] RedisEncodedCommand exists(const std::string& key) const; ///< EXISTS 命令
        [[nodiscard]] RedisEncodedCommand incr(const std::string& key) const; ///< INCR 命令
        [[nodiscard]] RedisEncodedCommand decr(const std::string& key) const; ///< DECR 命令

        // ======================== Hash操作 ========================
        [[nodiscard]] RedisEncodedCommand hget(const std::string& key,
                                               const std::string& field) const; ///< HGET 命令
        [[nodiscard]] RedisEncodedCommand hset(const std::string& key,
                                               const std::string& field,
                                               const std::string& value) const; ///< HSET 命令
        [[nodiscard]] RedisEncodedCommand hdel(const std::string& key,
                                               const std::string& field) const; ///< HDEL 命令
        [[nodiscard]] RedisEncodedCommand hgetAll(const std::string& key) const; ///< HGETALL 命令

        // ======================== List操作 ========================
        [[nodiscard]] RedisEncodedCommand lpush(const std::string& key,
                                                const std::string& value) const; ///< LPUSH 命令
        [[nodiscard]] RedisEncodedCommand rpush(const std::string& key,
                                                const std::string& value) const; ///< RPUSH 命令
        [[nodiscard]] RedisEncodedCommand lpop(const std::string& key) const; ///< LPOP 命令
        [[nodiscard]] RedisEncodedCommand rpop(const std::string& key) const; ///< RPOP 命令
        [[nodiscard]] RedisEncodedCommand llen(const std::string& key) const; ///< LLEN 命令
        [[nodiscard]] RedisEncodedCommand lrange(const std::string& key,
                                                 int64_t start,
                                                 int64_t stop) const; ///< LRANGE 命令

        // ======================== Set操作 ========================
        [[nodiscard]] RedisEncodedCommand sadd(const std::string& key,
                                               const std::string& member) const; ///< SADD 命令
        [[nodiscard]] RedisEncodedCommand srem(const std::string& key,
                                               const std::string& member) const; ///< SREM 命令
        [[nodiscard]] RedisEncodedCommand smembers(const std::string& key) const; ///< SMEMBERS 命令
        [[nodiscard]] RedisEncodedCommand scard(const std::string& key) const; ///< SCARD 命令

        // ======================== Sorted Set操作 ========================
        [[nodiscard]] RedisEncodedCommand zadd(const std::string& key,
                                               double score,
                                               const std::string& member) const; ///< ZADD 命令
        [[nodiscard]] RedisEncodedCommand zrem(const std::string& key,
                                               const std::string& member) const; ///< ZREM 命令
        [[nodiscard]] RedisEncodedCommand zrange(const std::string& key,
                                                 int64_t start,
                                                 int64_t stop) const; ///< ZRANGE 命令
        [[nodiscard]] RedisEncodedCommand zscore(const std::string& key,
                                                 const std::string& member) const; ///< ZSCORE 命令

    private:
        /**
         * @brief 字符串切片描述
         */
        struct Slice
        {
            size_t offset = 0;  ///< 在存储池中的偏移
            size_t length = 0;  ///< 长度
        };

        /**
         * @brief 命令元数据
         */
        struct CommandMeta
        {
            Slice command;       ///< 命令名切片
            size_t arg_offset = 0;  ///< 参数在 arg_slices 中的起始偏移
            size_t arg_count = 0;   ///< 参数数量
            Slice encoded;       ///< 预编码数据切片
        };

        static size_t normalizeExpectedReplies(size_t expected_replies) noexcept; ///< 规范化期望回复数

        Slice appendToStorage(std::string_view value); ///< 追加字符串到存储池
        [[nodiscard]] std::string_view toView(Slice slice) const; ///< 将切片转换为 string_view
        [[nodiscard]] std::string_view toEncodedView(Slice slice) const; ///< 将编码切片转换为 string_view
        void rebuildViewsIfNeeded() const; ///< 按需重建命令视图

        protocol::RespEncoder m_encoder;              ///< RESP 编码器
        std::string m_encoded;                        ///< 累积的编码数据

        std::string m_storage;                        ///< 字符串存储池
        std::vector<Slice> m_arg_slices;              ///< 参数切片列表
        std::vector<CommandMeta> m_commands;           ///< 命令元数据列表
        mutable std::vector<std::string_view> m_arg_views;         ///< 参数视图缓存
        mutable std::vector<RedisCommandView> m_command_views;     ///< 命令视图缓存
        mutable bool m_views_dirty = true;            ///< 视图脏标志
    };

    template <size_t N>
    RedisCommandBuilder& RedisCommandBuilder::append(
        std::string_view cmd,
        const std::array<std::string_view, N>& args)
    {
        return append(cmd, std::span<const std::string_view>(args));
    }

    template <size_t N>
    RedisEncodedCommand RedisCommandBuilder::command(
        std::string_view cmd,
        const std::array<std::string_view, N>& args,
        size_t expected_replies) const
    {
        return command(cmd, std::span<const std::string_view>(args), expected_replies);
    }
} // namespace galay::redis

#endif // GALAY_REDIS_BUILDER_H
