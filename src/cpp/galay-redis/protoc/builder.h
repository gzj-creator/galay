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
#include <utility>
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
        RedisEncodedCommand() = default; ///< 默认构造
        RedisEncodedCommand(std::string encoded_data, size_t replies = 1)
            : encoded(std::move(encoded_data))
            , expected_replies(replies)
        {
        }
    private:
        RedisEncodedCommand(const RedisEncodedCommand&) = delete; ///< 禁止隐式拷贝
        /**
         * @brief 禁止隐式拷贝赋值
         * @return 该操作已禁用，不可调用
         */
        RedisEncodedCommand& operator=(const RedisEncodedCommand&) = delete;
    public:
        RedisEncodedCommand(RedisEncodedCommand&&) noexcept = default; ///< 移动构造
        /**
         * @brief 移动赋值
         * @return 当前对象引用
         */
        RedisEncodedCommand& operator=(RedisEncodedCommand&&) noexcept = default;

        /**
         * @brief 显式克隆已编码命令
         * @return 独立复制后的命令包
         */
        [[nodiscard]] RedisEncodedCommand clone() const
        {
            return RedisEncodedCommand(encoded, expected_replies);
        }

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
    private:
        RedisCommandBuilder(const RedisCommandBuilder&) = delete;
        RedisCommandBuilder& operator=(const RedisCommandBuilder&) = delete;
    public:
        RedisCommandBuilder(RedisCommandBuilder&& other) noexcept;
        RedisCommandBuilder& operator=(RedisCommandBuilder&& other) noexcept;

        /**
         * @brief 显式克隆命令构建器
         * @return 独立复制后的构建器，内部视图缓存会在首次访问时重建
         */
        [[nodiscard]] RedisCommandBuilder clone() const;

        /**
         * @brief 清空所有已添加的命令
         * @return 无返回值
         */
        void clear() noexcept;

        /**
         * @brief 批量预留空间
         * @param command_count 命令数量
         * @param arg_count 参数数量
         * @param storage_bytes 存储字节数
         * @return 无返回值
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
         * @param cmd 命令名称
         * @param args 调用参数包
         * @param expected_replies 预期响应数量
         * @return 编码后的单条 Redis 命令
         */
        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  std::initializer_list<std::string_view> args,
                                                  size_t expected_replies = 1) const;

        /**
         * @brief 构建单条带参数命令（array 版本）
         * @param cmd 命令名称
         * @param args 调用参数包
         * @param expected_replies 预期响应数量
         * @return 编码后的单条 Redis 命令
         */
        template <size_t N>
        [[nodiscard]] RedisEncodedCommand command(
            std::string_view cmd,
            const std::array<std::string_view, N>& args,
            size_t expected_replies = 1) const;

        /**
         * @brief 构建单条带参数命令（vector 版本）
         * @param cmd 命令名称
         * @param args 调用参数包
         * @param expected_replies 预期响应数量
         * @return 编码后的单条 Redis 命令
         */
        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  const std::vector<std::string>& args,
                                                  size_t expected_replies = 1) const;

        // ======================== 连接命令 ========================
        /**
         * @brief AUTH 命令（仅密码）
         * @param password 密码
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand auth(const std::string& password) const;
        /**
         * @brief AUTH 命令（用户名+密码）
         * @param username 用户名
         * @param password 密码
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand auth(const std::string& username,
                                               const std::string& password) const;
        /**
         * @brief SELECT 命令
         * @param db_index 数据库索引
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand select(int32_t db_index) const;
        /**
         * @brief PING 命令
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand ping() const;
        /**
         * @brief ECHO 命令
         * @param message 消息
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand echo(const std::string& message) const;

        // ======================== 发布订阅 ========================
        /**
         * @brief PUBLISH 命令
         * @param channel 通道对象
         * @param message 消息
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand publish(const std::string& channel,
                                                  const std::string& message) const;
        /**
         * @brief SUBSCRIBE 命令（单频道）
         * @param channel 通道对象
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand subscribe(const std::string& channel) const;
        /**
         * @brief SUBSCRIBE 命令（多频道）
         * @param channels 订阅频道集合
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand subscribe(const std::vector<std::string>& channels) const;
        /**
         * @brief UNSUBSCRIBE 命令（单频道）
         * @param channel 通道对象
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand unsubscribe(const std::string& channel) const;
        /**
         * @brief UNSUBSCRIBE 命令（多频道）
         * @param channels 订阅频道集合
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand unsubscribe(const std::vector<std::string>& channels) const;
        /**
         * @brief PSUBSCRIBE 命令（单模式）
         * @param pattern 匹配模式
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand psubscribe(const std::string& pattern) const;
        /**
         * @brief PSUBSCRIBE 命令（多模式）
         * @param patterns 匹配模式集合
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand psubscribe(const std::vector<std::string>& patterns) const;
        /**
         * @brief PUNSUBSCRIBE 命令（单模式）
         * @param pattern 匹配模式
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand punsubscribe(const std::string& pattern) const;
        /**
         * @brief PUNSUBSCRIBE 命令（多模式）
         * @param patterns 匹配模式集合
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand punsubscribe(const std::vector<std::string>& patterns) const;

        // ======================== 集群/主从命令 ========================
        /**
         * @brief ROLE 命令
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand role() const;
        /**
         * @brief REPLICAOF 命令
         * @param host 目标主机地址
         * @param port 端口号
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand replicaof(const std::string& host, int32_t port) const;
        /**
         * @brief READONLY 命令
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand readonly() const;
        /**
         * @brief READWRITE 命令
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand readwrite() const;
        /**
         * @brief CLUSTER INFO 命令
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand cluster_info() const;
        /**
         * @brief CLUSTER NODES 命令
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand cluster_nodes() const;
        /**
         * @brief CLUSTER SLOTS 命令
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand cluster_slots() const;

        // ======================== String操作 ========================
        /**
         * @brief GET 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand get(const std::string& key) const;
        /**
         * @brief SET 命令
         * @param key 键
         * @param value 待设置或处理的值
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand set(const std::string& key,
                                              const std::string& value) const;
        /**
         * @brief SETEX 命令
         * @param key 键
         * @param seconds 时间，单位为秒
         * @param value 待设置或处理的值
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand setex(const std::string& key,
                                                int64_t seconds,
                                                const std::string& value) const;
        /**
         * @brief DEL 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand del(const std::string& key) const;
        /**
         * @brief EXISTS 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand exists(const std::string& key) const;
        /**
         * @brief INCR 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand incr(const std::string& key) const;
        /**
         * @brief DECR 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand decr(const std::string& key) const;

        // ======================== Hash操作 ========================
        /**
         * @brief HGET 命令
         * @param key 键
         * @param field 哈希字段名
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand hget(const std::string& key,
                                               const std::string& field) const;
        /**
         * @brief HSET 命令
         * @param key 键
         * @param field 哈希字段名
         * @param value 待设置或处理的值
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand hset(const std::string& key,
                                               const std::string& field,
                                               const std::string& value) const;
        /**
         * @brief HDEL 命令
         * @param key 键
         * @param field 哈希字段名
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand hdel(const std::string& key,
                                               const std::string& field) const;
        /**
         * @brief HGETALL 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand hget_all(const std::string& key) const;

        // ======================== List操作 ========================
        /**
         * @brief LPUSH 命令
         * @param key 键
         * @param value 待设置或处理的值
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand lpush(const std::string& key,
                                                const std::string& value) const;
        /**
         * @brief RPUSH 命令
         * @param key 键
         * @param value 待设置或处理的值
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand rpush(const std::string& key,
                                                const std::string& value) const;
        /**
         * @brief LPOP 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand lpop(const std::string& key) const;
        /**
         * @brief RPOP 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand rpop(const std::string& key) const;
        /**
         * @brief LLEN 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand llen(const std::string& key) const;
        /**
         * @brief LRANGE 命令
         * @param key 键
         * @param start 开始位置
         * @param stop 结束位置
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand lrange(const std::string& key,
                                                 int64_t start,
                                                 int64_t stop) const;

        // ======================== Set操作 ========================
        /**
         * @brief SADD 命令
         * @param key 键
         * @param member 成员名称
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand sadd(const std::string& key,
                                               const std::string& member) const;
        /**
         * @brief SREM 命令
         * @param key 键
         * @param member 成员名称
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand srem(const std::string& key,
                                               const std::string& member) const;
        /**
         * @brief SMEMBERS 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand smembers(const std::string& key) const;
        /**
         * @brief SCARD 命令
         * @param key 键
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand scard(const std::string& key) const;

        // ======================== Sorted Set操作 ========================
        /**
         * @brief ZADD 命令
         * @param key 键
         * @param score 成员分数
         * @param member 成员名称
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand zadd(const std::string& key,
                                               double score,
                                               const std::string& member) const;
        /**
         * @brief ZREM 命令
         * @param key 键
         * @param member 成员名称
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand zrem(const std::string& key,
                                               const std::string& member) const;
        /**
         * @brief ZRANGE 命令
         * @param key 键
         * @param start 开始位置
         * @param stop 结束位置
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand zrange(const std::string& key,
                                                 int64_t start,
                                                 int64_t stop) const;
        /**
         * @brief ZSCORE 命令
         * @param key 键
         * @param member 成员名称
         * @return RedisEncodedCommand 操作结果
         */
        [[nodiscard]] RedisEncodedCommand zscore(const std::string& key,
                                                 const std::string& member) const;

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

        /**
         * @brief 规范化期望回复数
         * @param expected_replies 预期响应数量
         * @return size_t 操作结果
         */
        static size_t normalize_expected_replies(size_t expected_replies) noexcept;

        /**
         * @brief 追加字符串到存储池
         * @param value 待设置或处理的值
         * @return Slice 操作结果
         */
        Slice append_to_storage(std::string_view value);
        /**
         * @brief 将切片转换为 string_view
         * @param slice 数据切片
         * @return 处理后的 std::string_view 结果
         */
        [[nodiscard]] std::string_view to_view(Slice slice) const;
        /**
         * @brief 将编码切片转换为 string_view
         * @param slice 数据切片
         * @return 处理后的 std::string_view 结果
         */
        [[nodiscard]] std::string_view to_encoded_view(Slice slice) const;
        /**
         * @brief 按需重建命令视图
         * @return 无返回值
         */
        void rebuild_views_if_needed() const;

        std::string m_encoded;                        ///< 累积的编码数据

        std::string m_storage;                        ///< 字符串存储池
        std::vector<Slice> m_arg_slices;              ///< 参数切片列表
        std::vector<CommandMeta> m_commands;           ///< 命令元数据列表
        mutable std::vector<std::string_view> m_arg_views;         ///< 参数视图缓存
        mutable std::vector<RedisCommandView> m_command_views;     ///< 命令视图缓存
        mutable bool m_views_dirty = true;            ///< 视图脏标志
        protocol::RespEncoder m_encoder;              ///< RESP 编码器
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
