/**
 * @file redis_error.h
 * @brief Redis 错误类型定义
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 定义 Redis 操作中所有可能的错误码和错误类，
 *          用于 std::expected 返回值中的错误传递。
 */

#ifndef GALAY_REDIS_ERROR_H
#define GALAY_REDIS_ERROR_H

#include "../../galay-kernel/common/error.h"
#include <string>

namespace galay::redis
{
    /**
     * @brief Redis 错误类型枚举
     */
    enum RedisErrorType
    {
        REDIS_ERROR_TYPE_SUCCESS,                    ///< 成功
        REDIS_ERROR_TYPE_URL_INVALID_ERROR,          ///< URL 无效（不符合正则）
        REDIS_ERROR_TYPE_HOST_INVALID_ERROR,         ///< 主机地址无效
        REDIS_ERROR_TYPE_PORT_INVALID_ERROR,         ///< 端口无效
        REDIS_ERROR_TYPE_DB_INDEX_INVALID_ERROR,     ///< 数据库索引无效
        REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR, ///< 地址类型无效
        REDIS_ERROR_TYPE_VERSION_INVALID_ERROR,      ///< RESP 协议版本无效
        REDIS_ERROR_TYPE_CONNECTION_ERROR,           ///< 连接错误
        REDIS_ERROR_TYPE_FREE_REDISOBJ_ERROR,        ///< 释放 redisContext 对象失败

        REDIS_ERROR_TYPE_COMMAND_ERROR,              ///< 命令执行错误
        REDIS_ERROR_TYPE_TIMEOUT_ERROR,              ///< 超时错误
        REDIS_ERROR_TYPE_AUTH_ERROR,                 ///< 认证错误
        REDIS_ERROR_TYPE_INVALID_ERROR,              ///< 参数无效错误
        REDIS_ERROR_TYPE_UNKNOWN_ERROR,              ///< 未知错误
        REDIS_ERROR_TYPE_PARSE_ERROR,                ///< 协议解析错误
        REDIS_ERROR_TYPE_SEND_ERROR,                 ///< 发送数据错误
        REDIS_ERROR_TYPE_RECV_ERROR,                 ///< 接收数据错误
        REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,      ///< 缓冲区溢出
        REDIS_ERROR_TYPE_NETWORK_ERROR,              ///< 网络错误
        REDIS_ERROR_TYPE_CONNECTION_CLOSED,          ///< 连接已关闭
        REDIS_ERROR_TYPE_INTERNAL_ERROR,             ///< 内部错误
    };

    // 为了兼容性，提供别名
    using RedisErrorCode = RedisErrorType; ///< 错误码类型别名

    // 便捷的错误码常量
    constexpr RedisErrorType NetworkError = REDIS_ERROR_TYPE_NETWORK_ERROR;           ///< 网络错误常量
    constexpr RedisErrorType ConnectionClosed = REDIS_ERROR_TYPE_CONNECTION_CLOSED;   ///< 连接关闭常量


    /**
     * @brief Redis 错误类
     * @details 封装 Redis 错误类型和附加消息，支持从 IO 错误转换
     */
    class RedisError
    {
    public:
        /**
         * @brief 从错误类型构造
         * @param type 错误类型
         */
        RedisError(RedisErrorType type);

        /**
         * @brief 从错误类型和附加消息构造
         * @param type 错误类型
         * @param extra_msg 附加错误消息
         */
        RedisError(RedisErrorType type, std::string extra_msg);

        /**
         * @brief 从 IO 错误构造
         * @param io_error 内核 IO 错误
         */
        RedisError(const galay::kernel::IOError& io_error);

        /**
         * @brief 获取错误类型
         * @return 错误类型枚举值
         */
        RedisErrorType type() const;

        /**
         * @brief 获取错误描述消息
         * @return 错误描述字符串
         */
        std::string message() const;

    private:
        RedisErrorType m_type;     ///< 错误类型
        std::string m_extra_msg;   ///< 附加错误消息
    };
}
#endif
