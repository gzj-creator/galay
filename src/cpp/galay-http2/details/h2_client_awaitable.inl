#ifndef GALAY_HTTP2_DETAILS_H2_CLIENT_AWAITABLE_INL
#define GALAY_HTTP2_DETAILS_H2_CLIENT_AWAITABLE_INL

inline CaptureSchedulerAwaitable::CaptureSchedulerAwaitable(Scheduler** out) noexcept
    : m_out(out)
{
}

inline bool CaptureSchedulerAwaitable::await_ready() const noexcept
{
    return false;
}

template<typename Promise>
bool CaptureSchedulerAwaitable::await_suspend(std::coroutine_handle<Promise> handle) noexcept
{
    if (m_out != nullptr) {
        *m_out = handle.promise().taskRefView().belongScheduler();
    }
    return false;
}

inline void CaptureSchedulerAwaitable::await_resume() const noexcept
{
}

#endif // GALAY_HTTP2_DETAILS_H2_CLIENT_AWAITABLE_INL
