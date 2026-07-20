#ifndef GALAY_HTTP2_DETAILS_H2_CLIENT_AWAITABLE_H
#define GALAY_HTTP2_DETAILS_H2_CLIENT_AWAITABLE_H

/**
 * @file h2_client_awaitable.h
 * @brief HTTP/2 TLS 客户端等待体声明
 * @details 本文件由 h2_client.h 在 galay::http2::detail 命名空间内包含。
 */

/**
 * @brief 在协程首次挂起点捕获当前 Scheduler
 * @note 仅读取 Promise 所属调度器，不阻塞且不转移协程所有权。
 */
class CaptureSchedulerAwaitable
{
public:
    explicit CaptureSchedulerAwaitable(Scheduler** out) noexcept;

    bool await_ready() const noexcept;

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    void await_resume() const noexcept;

private:
    Scheduler** m_out = nullptr;
};

#include "h2_client_awaitable.inl"

#endif // GALAY_HTTP2_DETAILS_H2_CLIENT_AWAITABLE_H
