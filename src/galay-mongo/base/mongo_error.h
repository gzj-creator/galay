/**
 * @file mongo_error.h
 * @brief MongoDB 错误类型与错误信息封装
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 定义 galay-mongo 的错误体系，包括：
 * - MongoErrorType 枚举：标识错误发生的阶段或原因
 * - MongoError 类：封装错误类型、服务端错误码和描述消息
 */

#ifndef GALAY_MONGO_ERROR_H
#define GALAY_MONGO_ERROR_H

#include <cstdint>
#include <string>

namespace galay::mongo
{

/**
 * @brief 错误类型枚举，标识错误发生的阶段或原因
 */
enum MongoErrorType
{
    MONGO_ERROR_SUCCESS,            ///< 无错误
    MONGO_ERROR_CONNECTION,         ///< TCP 连接失败
    MONGO_ERROR_AUTH,               ///< SCRAM 认证失败
    MONGO_ERROR_COMMAND,            ///< 命令执行失败
    MONGO_ERROR_PROTOCOL,           ///< 协议解析错误
    MONGO_ERROR_TIMEOUT,            ///< 操作超时
    MONGO_ERROR_SEND,               ///< 数据发送失败
    MONGO_ERROR_RECV,               ///< 数据接收失败
    MONGO_ERROR_CONNECTION_CLOSED,  ///< 连接已关闭
    MONGO_ERROR_SERVER,             ///< 服务端返回错误
    MONGO_ERROR_INTERNAL,           ///< 内部逻辑错误
    MONGO_ERROR_BUFFER_OVERFLOW,    ///< 缓冲区溢出
    MONGO_ERROR_INVALID_PARAM,      ///< 参数无效
    MONGO_ERROR_UNSUPPORTED,        ///< 不支持的操作
};

/**
 * @brief MongoDB 错误信息，包含错误类型、服务端错误码和描述消息
 * @details 提供多种构造方式，支持仅错误类型、错误类型+描述、错误类型+服务端错误码+消息
 */
class MongoError
{
public:
    /**
     * @brief 仅指定错误类型
     * @param type 错误类型
     */
    explicit MongoError(MongoErrorType type);

    /**
     * @brief 指定错误类型和附加描述
     * @param type 错误类型
     * @param extra_msg 附加错误描述
     */
    MongoError(MongoErrorType type, std::string extra_msg);

    /**
     * @brief 指定错误类型、服务端错误码和服务端消息
     * @param type 错误类型
     * @param server_code 服务端错误码
     * @param server_msg 服务端错误消息
     */
    MongoError(MongoErrorType type, int32_t server_code, std::string server_msg);

    /**
     * @brief 返回错误类型
     * @return 错误类型枚举值
     */
    MongoErrorType type() const;

    /**
     * @brief 返回服务端错误码
     * @return 服务端错误码；非服务端错误时为 0
     */
    int32_t serverCode() const;

    /**
     * @brief 返回可读的错误描述字符串
     * @return 结合错误类型和服务端信息的描述
     */
    std::string message() const;

private:
    MongoErrorType m_type;              ///< 错误类型
    int32_t m_server_code = 0;          ///< 服务端错误码
    std::string m_extra_msg;            ///< 附加错误描述
};

} // namespace galay::mongo

#endif // GALAY_MONGO_ERROR_H
