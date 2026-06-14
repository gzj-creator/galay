/**
 * @file redis_session.h
 * @brief Redis 同步会话实现
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 提供同步 Redis 操作的高级接口 RedisSession，
 *          封装了 String、Hash、List、Set、Sorted Set 等常用 Redis 命令。
 */

#ifndef GALAY_REDIS_SYNC_SESSION_H
#define GALAY_REDIS_SYNC_SESSION_H

#include <kernel/kernel/runtime.h>
#include <string>
#include <memory>
#include <kernel/common/defn.hpp>
#include <kernel/kernel/runtime.h>
#include <kernel/kernel/task.h>
#include "redis/base/redis_error.h"
#include "redis/base/redis_config.h"
#include "redis/base/redis_value.h"
#include "redis/base/redis_base.h"
#include "redis/protoc/connection.h"
#include "redis/protoc/redis_protocol.h"

namespace galay::redis
{
    /**
     * @brief Redis 同步会话类
     * @details 提供同步阻塞方式的 Redis 操作接口，封装常用数据结构的操作命令。
     *          当前底层同步连接支持 host/port/timeout；绑定地址和 Unix 域套接字字段为预留配置。
     */
    class RedisSession
    {
    public:

        /**
         * @brief 构造 Redis 会话
         * @param config 连接配置
         */
        RedisSession(RedisSessionConfig config = {});

        /**
         * @brief 使用构造时保存的 RedisSessionConfig 连接到 Redis 服务器
         * @return 成功或错误
         */
        std::expected<void, RedisError> connect();

        /**
         * @brief 通过 URL 连接到 Redis 服务器
         * @param url Redis 连接 URL，格式：redis://user:password@host:port/db_index
         * @return 成功或错误
         */
        std::expected<void, RedisError> connect(const std::string& url);

        /**
         * @brief 连接到 Redis 服务器（用户名+密码）
         * @param ip 服务器地址
         * @param port 服务器端口
         * @param username 用户名
         * @param password 密码
         * @return 成功或错误
         */
        std::expected<void, RedisError> connect(const std::string& ip, int32_t port, const std::string& username, const std::string& password);

        /**
         * @brief 连接到 Redis 服务器（含数据库索引）
         * @param ip 服务器地址
         * @param port 服务器端口
         * @param username 用户名
         * @param password 密码
         * @param db_index 数据库索引
         * @return 成功或错误
         */
        std::expected<void, RedisError> connect(const std::string& ip, int32_t port, const std::string& username, const std::string& password, int32_t db_index);

        /**
         * @brief 连接到 Redis 服务器（完整参数）
         * @param ip 服务器地址
         * @param port 服务器端口
         * @param username 用户名
         * @param password 密码
         * @param db_index 数据库索引
         * @param version RESP 协议版本
         * @return 成功或错误
         */
        std::expected<void, RedisError> connect(const std::string& ip, int32_t port, const std::string& username, const std::string& password, int32_t db_index, int version);

        /**
         * @brief 断开连接
         * @return 成功或错误
         */
        std::expected<void, RedisError> disconnect();

        /**
         * @brief 选择数据库
         * @param db_index 数据库索引
         * @return 状态回复
         */
        std::expected<RedisValue, RedisError> selectDB(int32_t db_index);

        /**
         * @brief 清空当前数据库
         * @return 状态回复
         */
        std::expected<RedisValue, RedisError> flushDB();

        /**
         * @brief 切换 RESP 协议版本
         * @param version 协议版本（2 或 3）
         * @return RESP2 返回 array，RESP3 返回 map
         */
        std::expected<RedisValue, RedisError> switchVersion(int version);

        /**
         * @brief 检查键是否存在
         * @param key 键名
         * @return 整数（1 表示存在，0 表示不存在）
         */
        std::expected<RedisValue, RedisError> exist(const std::string &key);

        /**
         * @brief 获取字符串值
         * @param key 键名
         * @return RedisValue
         */
        std::expected<RedisValue, RedisError> get(const std::string& key);

        /**
         * @brief 设置字符串值
         * @param key 键名
         * @param value 值
         * @return 状态回复
         */
        std::expected<RedisValue, RedisError> set(const std::string& key, const std::string& value);

        /**
         * @brief 批量设置键值对
         * @tparam KV 键值对类型（需满足 KVPair concept）
         * @param pairs 键值对参数
         * @return 状态回复
         */
        template <KVPair... KV>
        std::expected<RedisValue, RedisError> mset(KV... pairs);

        /**
         * @brief 批量获取值
         * @tparam Key 键类型（需满足 KeyType concept）
         * @param keys 键名参数
         * @return RedisValue 数组
         */
        template<KeyType... Key>
        std::expected<RedisValue, RedisError> mget(Key... keys);

        /**
         * @brief 删除键
         * @param key 键名
         * @return 被删除的键数量（整数）
         */
        std::expected<RedisValue, RedisError> del(const std::string &key);

        /**
         * @brief 设置带过期时间的键值对（秒）
         * @param key 键名
         * @param seconds 过期时间（秒）
         * @param value 值
         * @return 状态回复
         */
        std::expected<RedisValue, RedisError> setEx(const std::string& key, int64_t seconds, const std::string& value);

        /**
         * @brief 设置带过期时间的键值对（毫秒）
         * @param key 键名
         * @param milliseconds 过期时间（毫秒）
         * @param value 值
         * @return 状态回复
         */
        std::expected<RedisValue, RedisError> psetEx(const std::string& key, int64_t milliseconds, const std::string& value);

        /**
         * @brief 自增
         * @param key 键名
         * @return 自增后的值（整数）
         */
        std::expected<RedisValue, RedisError> incr(const std::string& key);

        /**
         * @brief 按步长自增
         * @param key 键名
         * @param value 增量
         * @return 自增后的值（整数）
         */
        std::expected<RedisValue, RedisError> incrBy(std::string key, int64_t value);

        /**
         * @brief 自减
         * @param key 键名
         * @return 自减后的值（整数）
         */
        std::expected<RedisValue, RedisError> decr(const std::string& key);

        /**
         * @brief 获取哈希字段值
         * @param key 键名
         * @param field 字段名
         * @return RedisValue
         */
        std::expected<RedisValue, RedisError> hget(const std::string& key, const std::string& field);

        /**
         * @brief 设置哈希字段值
         * @param key 键名
         * @param field 字段名
         * @param value 值
         * @return 状态回复
         */
        std::expected<RedisValue, RedisError> hset(const std::string& key, const std::string& field, const std::string& value);

        /**
         * @brief 删除哈希字段
         * @tparam Key 字段名类型（需满足 KeyType concept）
         * @param key 键名
         * @param fields 字段名参数
         * @return 被删除的字段数量（整数）
         */
        template <KeyType... Key>
        std::expected<RedisValue, RedisError> hdel(const std::string& key, Key... fields);

        /**
         * @brief 批量获取哈希字段值
         * @tparam Key 字段名类型（需满足 KeyType concept）
         * @param key 键名
         * @param field 字段名参数
         * @return RedisValue 数组
         */
        template <KeyType... Key>
        std::expected<RedisValue, RedisError> hmget(const std::string& key, Key... field);

        /**
         * @brief 批量设置哈希字段值
         * @tparam KV 键值对类型（需满足 KVPair concept）
         * @param key 键名
         * @param fields 字段键值对参数
         * @return 状态回复
         */
        template<KVPair... KV>
        std::expected<RedisValue, RedisError> hmset(const std::string& key, KV... fields);

        /**
         * @brief 获取哈希所有字段和值
         * @param key 键名
         * @return RedisValue map 或 array
         */
        std::expected<RedisValue, RedisError> hgetAll(const std::string& key);

        /**
         * @brief 哈希字段自增
         * @param key 键名
         * @param field 字段名
         * @param value 增量
         * @return 自增后的值（整数）
         */
        std::expected<RedisValue, RedisError> hincrBy(const std::string& key, std::string field, int64_t value);

        /**
         * @brief 从列表左端推入值
         * @tparam Val 值类型（需满足 ValType concept）
         * @param key 键名
         * @param values 值参数
         * @return 列表长度（整数）
         */
        template <ValType... Val>
        std::expected<RedisValue, RedisError> lpush(const std::string& key, Val... values);

        /**
         * @brief 从列表右端推入值
         * @tparam Val 值类型（需满足 ValType concept）
         * @param key 键名
         * @param values 值参数
         * @return 列表长度（整数）
         */
        template <ValType... Val>
        std::expected<RedisValue, RedisError> rpush(const std::string& key, Val... values);

        /**
         * @brief 获取列表长度
         * @param key 键名
         * @return 列表长度（整数）
         */
        std::expected<RedisValue, RedisError> lLen(const std::string& key);

        /**
         * @brief 获取列表范围内的元素
         * @param key 键名
         * @param start 起始索引
         * @param end 结束索引
         * @return RedisValue 数组
         */
        std::expected<RedisValue, RedisError> lrange(const std::string& key, int64_t start, int64_t end);

        /**
         * @brief 从列表中移除指定数量的值
         * @param key 键名
         * @param value 要移除的值
         * @param count 移除数量
         * @return 实际移除的数量（整数）
         */
        std::expected<RedisValue, RedisError> lrem(const std::string& key, const std::string& value, int64_t count);

        /**
         * @brief 向集合添加成员
         * @tparam Val 值类型（需满足 ValType concept）
         * @param key 键名
         * @param members 成员参数
         * @return 添加的成员数量（整数）
         */
        template <ValType... Val>
        std::expected<RedisValue, RedisError> sadd(const std::string& key, Val... members);

        /**
         * @brief 获取集合所有成员
         * @param key 键名
         * @return RedisValue 数组
         */
        std::expected<RedisValue, RedisError> smembers(const std::string& key);

        /**
         * @brief 从集合移除成员
         * @tparam Val 值类型（需满足 ValType concept）
         * @param key 键名
         * @param members 成员参数
         * @return 移除的成员数量（整数）
         */
        template <ValType... Val>
        std::expected<RedisValue, RedisError> srem(const std::string& key, Val... members);

        /**
         * @brief 计算集合交集
         * @tparam Key 键类型（需满足 KeyType concept）
         * @param keys 键名参数
         * @return RedisValue 数组或集合
         */
        template<KeyType... Key>
        std::expected<RedisValue, RedisError> sinter(Key... keys);

        /**
         * @brief 计算集合并集
         * @tparam Key 键类型（需满足 KeyType concept）
         * @param keys 键名参数
         * @return RedisValue 数组或集合
         */
        template<KeyType... Key>
        std::expected<RedisValue, RedisError> sunion(Key... keys);

        /**
         * @brief 移动集合成员
         * @param source 源集合
         * @param destination 目标集合
         * @param member 要移动的成员
         * @return 整数（1 成功，0 失败）
         */
        std::expected<RedisValue, RedisError> smove(const std::string& source, const std::string& destination, const std::string& member);

        /**
         * @brief 获取集合成员数量
         * @param key 键名
         * @return 成员数量（整数）
         */
        std::expected<RedisValue, RedisError> scard(const std::string& key);

        /**
         * @brief 向有序集合添加成员
         * @tparam KV 分数值类型（需满足 ScoreValType concept）
         * @param key 键名
         * @param values 分数-成员对参数
         * @return 添加的成员数量（整数）
         */
        template <ScoreValType... KV>
        std::expected<RedisValue, RedisError> zadd(const std::string& key, KV... values);

        /**
         * @brief 获取有序集合范围内的成员
         * @param key 键名
         * @param beg 起始索引
         * @param end 结束索引
         * @return RedisValue 数组
         */
        std::expected<RedisValue, RedisError> zrange(const std::string& key, uint32_t beg, uint32_t end);

        /**
         * @brief 获取有序集合成员的分数
         * @param key 键名
         * @param member 成员名
         * @return 字符串或双精度浮点数
         */
        std::expected<RedisValue, RedisError> zscore(const std::string& key, const std::string& member);

        /**
         * @brief 从有序集合移除成员
         * @tparam Key 成员名类型（需满足 KeyType concept）
         * @param key 键名
         * @param members 成员名参数
         * @return 移除的成员数量（整数）
         */
        template <KeyType... Key>
        std::expected<RedisValue, RedisError> zrem(const std::string& key, Key... members);

        /**
         * @brief 执行原始 Redis 命令
         * @param cmd 原始命令字符串
         * @return RedisValue
         */
        std::expected<RedisValue, RedisError> redisCommand(const std::string& cmd);

        ~RedisSession();

    private:
        std::ostringstream m_stream;                            ///< 字符串输出流（用于命令编码）
        std::unique_ptr<protocol::Connection> m_connection;     ///< 底层 TCP 连接
        protocol::RespEncoder m_encoder;                        ///< RESP 编码器
        RedisSessionConfig m_config;                            ///< 连接配置
    };

}


#endif
