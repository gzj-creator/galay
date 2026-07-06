/**
 * @file mysql_error.h
 * @brief MySQL错误类型与错误类定义
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 定义了MySQL客户端可能遇到的各种错误类型枚举和错误信息封装类。
 */

#ifndef GALAY_MYSQL_ERROR_H
#define GALAY_MYSQL_ERROR_H

#include <string>
#include <cstdint>

namespace galay::mysql
{

/**
 * @brief MySQL错误类型枚举
 */
enum MysqlErrorType
{
    MYSQL_ERROR_SUCCESS,          ///< 成功
    MYSQL_ERROR_CONNECTION,       ///< 连接错误
    MYSQL_ERROR_AUTH,             ///< 认证错误
    MYSQL_ERROR_QUERY,            ///< 查询错误
    MYSQL_ERROR_PROTOCOL,         ///< 协议错误
    MYSQL_ERROR_TIMEOUT,          ///< 超时错误
    MYSQL_ERROR_SEND,             ///< 发送错误
    MYSQL_ERROR_RECV,             ///< 接收错误
    MYSQL_ERROR_CONNECTION_CLOSED,///< 连接已关闭
    MYSQL_ERROR_PREPARED_STMT,    ///< 预处理语句错误
    MYSQL_ERROR_TRANSACTION,      ///< 事务错误
    MYSQL_ERROR_SERVER,           ///< 服务器返回错误
    MYSQL_ERROR_INTERNAL,         ///< 内部错误
    MYSQL_ERROR_BUFFER_OVERFLOW,  ///< 缓冲区溢出
    MYSQL_ERROR_INVALID_PARAM,    ///< 无效参数
};

/**
 * @brief MySQL错误类
 * @details 封装错误类型、服务器错误码和附加消息，用于统一错误处理。
 */
class MysqlError
{
public:
    /**
     * @brief 构造指定类型的错误
     * @param type 错误类型
     */
    MysqlError(MysqlErrorType type);

    /**
     * @brief 构造带附加消息的错误
     * @param type 错误类型
     * @param extra_msg 附加消息
     */
    MysqlError(MysqlErrorType type, std::string extra_msg);

    /**
     * @brief 构造服务器返回的错误
     * @param type 错误类型
     * @param server_errno 服务器错误码
     * @param server_msg 服务器错误消息
     */
    MysqlError(MysqlErrorType type, uint16_t server_errno, std::string server_msg);

    /**
     * @brief 获取错误类型
     * @return 错误类型枚举值
     */
    MysqlErrorType type() const;

    /**
     * @brief 获取错误描述消息
     * @return 错误描述字符串
     */
    std::string message() const;

    /**
     * @brief 获取服务器错误码
     * @return 服务器错误码，非服务器错误时返回0
     */
    uint16_t serverErrno() const;

private:
    std::string m_extra_msg;        ///< 附加错误消息
    MysqlErrorType m_type;          ///< 错误类型
    uint16_t m_server_errno = 0;    ///< 服务器错误码
};

} // namespace galay::mysql

#endif // GALAY_MYSQL_ERROR_H
