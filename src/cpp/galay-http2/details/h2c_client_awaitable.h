#ifndef GALAY_HTTP2_DETAILS_H2C_CLIENT_AWAITABLE_H
#define GALAY_HTTP2_DETAILS_H2C_CLIENT_AWAITABLE_H

/**
 * @file h2c_client_awaitable.h
 * @brief H2c 客户端升级等待体声明
 */

#include "../client/h2c_client.h"

namespace galay::http2
{

/**
 * @brief H2c 升级 awaitable 包装器
 * @details 通过状态机挂起协程，并在 await_resume() 时完成 transport 接管。
 * @note 所有连接、超时和协议错误均以 std::expected 显式返回，不阻塞调用线程。
 */
template<RingBufferBackendStrategy Strategy>
class H2cUpgradeAwaitable
    : public SequenceAwaitableBase
    , public TimeoutSupport<H2cUpgradeAwaitable<Strategy>>
{
public:
    using ResultType = std::expected<bool, Http2Error>;
    using result_type = ResultType;

    H2cUpgradeAwaitable(const H2cUpgradeAwaitable&) = delete;
    H2cUpgradeAwaitable& operator=(const H2cUpgradeAwaitable&) = delete;
    H2cUpgradeAwaitable(H2cUpgradeAwaitable&&) noexcept = default;
    H2cUpgradeAwaitable& operator=(H2cUpgradeAwaitable&&) noexcept = default;

    H2cUpgradeAwaitable(H2cClient<Strategy>& client, const std::string& path);
    ~H2cUpgradeAwaitable();

    bool await_ready() const noexcept;

    template <typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle);

    ResultType await_resume();
    void markTimeout();

    IOTask* front() override;
    const IOTask* front() const override;
    void popFront() override;
    bool empty() const override;

#ifdef USE_IOURING
    SequenceProgress prepareForSubmit() override;
    SequenceProgress onActiveEvent(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    SequenceProgress prepareForSubmit(GHandle handle) override;
    SequenceProgress onActiveEvent(GHandle handle) override;
#endif

    std::expected<bool, IOError> m_result{true};

private:
    using InnerOperation = galay::kernel::StateMachineAwaitable<H2cUpgradeMachine<Strategy>>;

    static void discardTransport(H2cClient<Strategy>& client);
    static bool finalizeTransport(H2cClient<Strategy>& client, Scheduler* scheduler);
    static Http2Error translateIoError(const IOError& error);

    ResultType resumeInner();
    void cleanupInnerIfArmed();

    H2cClient<Strategy>* m_client = nullptr;
    std::unique_ptr<InnerOperation> m_inner_operation;
    std::optional<Http2Error> m_error;
    Scheduler* m_scheduler = nullptr;
    bool m_ready = false;
    bool m_inner_armed = false;
    bool m_inner_completed = false;
};

} // namespace galay::http2

#include "h2c_client_awaitable.inl"

#endif // GALAY_HTTP2_DETAILS_H2C_CLIENT_AWAITABLE_H
