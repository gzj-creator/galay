/**
 * @file rpc_metrics.h
 * @brief RPC观测指标hook
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供轻量指标回调，不引入外部metrics依赖。
 */

#ifndef GALAY_RPC_METRICS_H
#define GALAY_RPC_METRICS_H

#include "../protoc/rpc_error.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace galay::rpc
{

struct RpcMetricEvent {
    std::string service;
    std::string method;
    RpcErrorCode status = RpcErrorCode::OK;
    std::chrono::nanoseconds latency{0};
    size_t pending_calls = 0;
};

using RpcMetricCallback = std::function<void(const RpcMetricEvent&)>;

class RpcMetricsSink {
public:
    explicit RpcMetricsSink(RpcMetricCallback callback = {})
        : m_callback(std::move(callback))
    {
    }

    void emit(const RpcMetricEvent& event) const {
        if (m_callback) {
            m_callback(event);
        }
    }

private:
    RpcMetricCallback m_callback;
};

class RpcPendingGauge {
public:
    void increment() { m_value.fetch_add(1, std::memory_order_relaxed); }
    void decrement() {
        auto current = m_value.load(std::memory_order_relaxed);
        while (current > 0 &&
               !m_value.compare_exchange_weak(current, current - 1,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
        }
    }
    size_t value() const { return m_value.load(std::memory_order_relaxed); }

private:
    std::atomic<size_t> m_value{0};
};

} // namespace galay::rpc

#endif // GALAY_RPC_METRICS_H
