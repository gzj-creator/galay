#ifndef GALAY_HTTP2_DETAILS_H2_CLIENT_AWAITABLE_H
#define GALAY_HTTP2_DETAILS_H2_CLIENT_AWAITABLE_H

#include "../../galay-kernel/core/scheduler.hpp"
#include <coroutine>

namespace galay::http2::detail
{

/**
 * @brief 在协程首次挂起点捕获当前 Scheduler
 * @note 仅读取 Promise 所属调度器，不阻塞且不转移协程所有权。
 */
class CaptureSchedulerAwaitable
{
public:
    explicit CaptureSchedulerAwaitable(galay::kernel::Scheduler** out) noexcept;

    bool await_ready() const noexcept;

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    void await_resume() const noexcept;

private:
    galay::kernel::Scheduler** m_out = nullptr;
};

#include "h2_client_awaitable.inl"

} // namespace galay::http2::detail

#endif // GALAY_HTTP2_DETAILS_H2_CLIENT_AWAITABLE_H
