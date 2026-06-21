/**
 * @file rpc_tracing.h
 * @brief RPC trace metadata helper
 * @author galay-rpc
 * @version 1.0.0
 */

#ifndef GALAY_RPC_TRACING_H
#define GALAY_RPC_TRACING_H

#include "rpc_metadata.h"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace galay::rpc
{

constexpr std::string_view kRpcTraceparentKey = "traceparent";

inline std::expected<void, RpcError> setTraceparent(RpcMetadata& metadata, std::string_view value)
{
    if (value.empty()) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                        "traceparent is empty"));
    }
    return metadata.insert(kRpcTraceparentKey, value);
}

inline std::optional<std::string_view> traceparent(const RpcMetadata& metadata)
{
    return metadata.get(kRpcTraceparentKey);
}

} // namespace galay::rpc

#endif // GALAY_RPC_TRACING_H
