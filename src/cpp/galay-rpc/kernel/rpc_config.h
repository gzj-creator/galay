/**
 * @file rpc_config.h
 * @brief RPC配置模型
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 定义RPC服务端、客户端、连接池、重试、治理、发现、流和基准配置。
 */

#ifndef GALAY_RPC_CONFIG_H
#define GALAY_RPC_CONFIG_H

#include "rpc_connection_pool.h"
#include "rpc_policy.h"
#include "rpc_stream.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace galay::rpc
{

enum class RpcDiscoveryKind {
    Local,
    Etcd
};

struct RpcServerRuntimeConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 50051;
    int backlog = 128;
    size_t io_scheduler_count = 0;
    size_t compute_scheduler_count = 0;
};

struct RpcClientRuntimeConfig {
    std::chrono::milliseconds default_timeout{3000};
};

struct RpcDiscoveryConfig {
    RpcDiscoveryKind kind = RpcDiscoveryKind::Local;
    std::string prefix = "/galay/rpc";
};

struct RpcBenchmarkConfig {
    size_t requests = 10000;
    size_t concurrency = 64;
    size_t payload = 128;
};

struct RpcConfig {
    RpcServerRuntimeConfig server;
    RpcClientRuntimeConfig client;
    RpcConnectionPoolConfig pool{.min_connections_per_endpoint = 0,
                                 .max_connections_per_endpoint = 4,
                                 .max_waiters_per_endpoint = 64};
    RpcRetryPolicy retry;
    RpcGovernancePolicy governance;
    RpcDiscoveryConfig discovery;
    RpcStreamLimits stream;
    RpcBenchmarkConfig benchmark;
};

} // namespace galay::rpc

#endif // GALAY_RPC_CONFIG_H
