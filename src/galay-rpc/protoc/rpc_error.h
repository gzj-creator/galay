/**
 * @file rpc_error.h
 * @brief RPC错误处理
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 定义RPC错误类RpcError，封装错误码和错误消息，
 *          并提供从底层IOError自动映射到RPC错误的工厂方法。
 */

#ifndef GALAY_RPC_ERROR_H
#define GALAY_RPC_ERROR_H

#include "rpc_base.h"
#include "galay-kernel/common/error.h"
#include <string>

namespace galay::rpc
{

/**
 * @brief RPC错误类
 */
/**
 * @brief RPC错误类
 *
 * @details 封装RPC错误码和错误消息，支持从底层IOError自动映射。
 */
class RpcError {
public:
    RpcError() = default;

    /**
     * @brief 从错误码构造，自动生成默认消息
     * @param code RPC错误码
     */
    RpcError(RpcErrorCode code)
        : m_code(code)
        , m_message(rpcErrorCodeToString(code)) {}

    /**
     * @brief 从错误码和自定义消息构造
     * @param code RPC错误码
     * @param message 错误描述消息
     */
    RpcError(RpcErrorCode code, std::string_view message)
        : m_code(code)
        , m_message(message) {}

    /// @brief 获取错误码
    RpcErrorCode code() const { return m_code; }
    /// @brief 获取错误消息
    const std::string& message() const { return m_message; }

    /// @brief 判断是否为成功状态
    bool isOk() const { return m_code == RpcErrorCode::OK; }

    /// @brief 判断是否存在错误（非OK时返回true）
    explicit operator bool() const { return !isOk(); }

    /**
     * @brief 从IOError创建RpcError
     * @param io_error 底层IO错误
     * @param default_code 默认RPC错误码
     * @return 映射后的RpcError
     * @note 自动识别超时和连接断开错误并映射到对应的RPC错误码
     */
    static RpcError from(const kernel::IOError& io_error,
                         RpcErrorCode default_code = RpcErrorCode::INTERNAL_ERROR) {
        if (kernel::IOError::contains(io_error.code(), kernel::kTimeout)) {
            return RpcError(RpcErrorCode::REQUEST_TIMEOUT, io_error.message());
        }
        if (kernel::IOError::contains(io_error.code(), kernel::kDisconnectError)) {
            return RpcError(RpcErrorCode::CONNECTION_CLOSED, io_error.message());
        }
        return RpcError(default_code, io_error.message());
    }

    /// @brief 转换为字符串表示（错误码: 消息）
    std::string toString() const {
        return std::string(rpcErrorCodeToString(m_code)) + ": " + m_message;
    }

private:
    RpcErrorCode m_code = RpcErrorCode::OK;  ///< 错误码
    std::string m_message;                    ///< 错误消息
};

} // namespace galay::rpc

#endif // GALAY_RPC_ERROR_H
