/**
 * @file rpc_interceptor.h
 * @brief RPC服务端拦截器
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供服务端请求拦截hook，用于认证、授权等前置检查。
 */

#ifndef GALAY_RPC_INTERCEPTOR_H
#define GALAY_RPC_INTERCEPTOR_H

#include "rpc_call.h"
#include "../protoc/rpc_message.h"

#include <expected>
#include <functional>

namespace galay::rpc
{

using RpcServerInterceptor = std::function<std::expected<void, RpcError>(const RpcRequest&)>;

inline RpcServerInterceptor AllowAllRpcInterceptor()
{
    return [](const RpcRequest&) -> std::expected<void, RpcError> {
        return {};
    };
}

} // namespace galay::rpc

#endif // GALAY_RPC_INTERCEPTOR_H
