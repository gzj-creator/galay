/**
 * @file rpc_tls.h
 * @brief RPC TLS可选能力占位
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 默认不向RPC目标引入SSL硬依赖；启用SSL构建时可在此适配SslSocket。
 */

#ifndef GALAY_RPC_TLS_H
#define GALAY_RPC_TLS_H

namespace galay::rpc
{

struct RpcTlsConfig {
    bool enabled = false;
};

inline bool rpcTlsCompiled()
{
#if defined(GALAY_ENABLE_SSL) || defined(GALAY_RPC_ENABLE_TLS)
    return true;
#else
    return false;
#endif
}

} // namespace galay::rpc

#endif // GALAY_RPC_TLS_H
