#include "c_coro_tcp_bridge.h"

#include "../async/tcp_socket.h"
#include "awaitable.h"
#include "io_scheduler.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <utility>

namespace
{

using galay::async::TcpSocket;
using galay::kernel::AcceptAwaitable;
using galay::kernel::ConnectAwaitable;
using galay::kernel::IOController;
using galay::kernel::IOError;
using galay::kernel::IOScheduler;
using galay::kernel::ReadvAwaitable;
using galay::kernel::RecvAwaitable;
using galay::kernel::Scheduler;
using galay::kernel::SendFileAwaitable;
using galay::kernel::SendAwaitable;
using galay::kernel::WritevAwaitable;

using C_IOResult = GalayCoreCoroIOResult;
using C_IOResultCode = GalayCoreCoroIOResultCode;
using C_Host = GalayCoreCoroHost;
using C_IPType = GalayCoreCoroIPType;

constexpr C_IOResultCode C_IOResultOk = GalayCoreCoroIOResultOk;
constexpr C_IOResultCode C_IOResultEof = GalayCoreCoroIOResultEof;
constexpr C_IOResultCode C_IOResultTimeout = GalayCoreCoroIOResultTimeout;
constexpr C_IOResultCode C_IOResultCancelled = GalayCoreCoroIOResultCancelled;
constexpr C_IOResultCode C_IOResultInvalid = GalayCoreCoroIOResultInvalid;
constexpr C_IOResultCode C_IOResultError = GalayCoreCoroIOResultError;
constexpr C_IPType C_IPTypeIPV4 = GalayCoreCoroIPTypeIPV4;
constexpr C_IPType C_IPTypeIPV6 = GalayCoreCoroIPTypeIPV6;

struct CoroTcpOperationBase;

struct CoroTcpWakeState {
    galay::kernel::detail::ResumeTokenHeader header;
    CoroTcpOperationBase* operation = nullptr;
};

enum class CoroTcpCompletionPhase : uint8_t {
    Pending,
    IoCompleting,
    TimedOut,
    Cancelled,
    Completed,
};

struct CoroTcpCompletionState {
    std::atomic<CoroTcpCompletionPhase> phase{CoroTcpCompletionPhase::Pending};
    std::mutex user_data_mutex;
    void* user_data = nullptr;
    GalayCoreCoroWaitOps wait_ops{};
};

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

C_IOResult merge_cleanup_result(C_IOResult primary, C_IOResult cleanup)
{
    return primary.code == C_IOResultOk && cleanup.code != C_IOResultOk
        ? cleanup
        : primary;
}

bool is_valid_c_ip_type(C_IPType ip_type)
{
    return ip_type == C_IPTypeIPV4 || ip_type == C_IPTypeIPV6;
}

std::string from_c_host_address_to_string(const C_Host& host)
{
    const void* end = std::memchr(host.address, '\0', sizeof(host.address));
    const auto length = end == nullptr
        ? sizeof(host.address)
        : static_cast<size_t>(static_cast<const char*>(end) - host.address);
    return std::string(host.address, length);
}

galay::kernel::Host from_c_host_to_cpp_host(const C_Host& host)
{
    return galay::kernel::Host(
        static_cast<galay::kernel::IPType>(host.type),
        from_c_host_address_to_string(host),
        host.port);
}

bool assign_cpp_host_to_c_host(const galay::kernel::Host& host, C_Host* out_host)
{
    if (out_host == nullptr) {
        return true;
    }
    if (!host.valid()) {
        return false;
    }
    const std::string address = host.ip();
    if (address.size() >= sizeof(out_host->address)) {
        return false;
    }
    C_Host converted{};
    converted.type = host.isIPv4() ? C_IPTypeIPV4 : C_IPTypeIPV6;
    std::memcpy(converted.address, address.data(), address.size());
    converted.address[address.size()] = '\0';
    converted.port = host.port();
    *out_host = converted;
    return true;
}

int io_error_sys_errno(const IOError& error)
{
    return static_cast<int>(error.code() >> 32U);
}

C_IOResult from_io_error(const IOError& error)
{
    if (IOError::contains(error.code(), galay::kernel::kParamInvalid) ||
        IOError::contains(error.code(), galay::kernel::kNotRunningOnIOScheduler) ||
        IOError::contains(error.code(), galay::kernel::kNotReady)) {
        return make_result(C_IOResultInvalid, io_error_sys_errno(error));
    }
    if (IOError::contains(error.code(), galay::kernel::kTimeout)) {
        return make_result(C_IOResultTimeout, io_error_sys_errno(error));
    }
    if (IOError::contains(error.code(), galay::kernel::kDisconnectError)) {
        return make_result(C_IOResultEof, io_error_sys_errno(error));
    }
    return make_result(C_IOResultError, io_error_sys_errno(error));
}

TcpSocket* to_cpp_socket(void* socket)
{
    return static_cast<TcpSocket*>(socket);
}

Scheduler* to_io_scheduler(void* scheduler_handle)
{
    auto* scheduler = static_cast<Scheduler*>(scheduler_handle);
    return scheduler != nullptr && scheduler->type() == galay::kernel::kIOScheduler
        ? scheduler
        : nullptr;
}

bool valid_wait_ops(const GalayCoreCoroWaitOps* wait_ops)
{
    return wait_ops != nullptr &&
        wait_ops->wait != nullptr &&
        wait_ops->complete_user_data != nullptr &&
        wait_ops->release_user_data != nullptr;
}

IOController::Index slot_for_event(IOEventType event)
{
    switch (event) {
    case ACCEPT:
    case RECV:
    case READV:
        return IOController::READ;
    case CONNECT:
    case SEND:
    case WRITEV:
    case SENDFILE:
        return IOController::WRITE;
    default:
        return IOController::SIZE;
    }
}

bool timeout_fits_chrono(int64_t timeout_ms)
{
    if (timeout_ms <= 0) {
        return true;
    }
    using MillisecondsRep = std::chrono::milliseconds::rep;
    using NanosecondsRep = std::chrono::nanoseconds::rep;
    constexpr auto max_milliseconds_rep =
        static_cast<int64_t>(std::numeric_limits<MillisecondsRep>::max());
    constexpr auto max_milliseconds_for_nanoseconds =
        static_cast<int64_t>(std::numeric_limits<NanosecondsRep>::max() / 1000000);
    constexpr int64_t max_supported_milliseconds =
        max_milliseconds_rep < max_milliseconds_for_nanoseconds
            ? max_milliseconds_rep
            : max_milliseconds_for_nanoseconds;
    return timeout_ms <= max_supported_milliseconds;
}

struct CoroTcpOperationInterface {
    virtual ~CoroTcpOperationInterface() = default;
    virtual void cancelFromClose() noexcept = 0;
};

struct CoroTcpOperationBase: public CoroTcpOperationInterface {
    CoroTcpOperationBase(Scheduler* scheduler,
                         void* user_data,
                         GalayCoreCoroWaitOps wait_ops)
        : m_scheduler(scheduler)
        , m_wait_ops(wait_ops)
    {
        m_state.user_data = user_data;
        m_state.wait_ops = wait_ops;
        m_wake_state.header.hooks = &kWakeHooks;
        m_wake_state.operation = this;
    }

    galay::kernel::Waker makeWaker() noexcept
    {
        return galay::kernel::Waker(
            galay::kernel::detail::ResumeToken::fromCCoroutine(&m_wake_state));
    }

    bool completeFromWake() noexcept
    {
        if (m_finished) {
            return true;
        }
        const CoroTcpCompletionPhase phase =
            m_state.phase.load(std::memory_order_acquire);
        if (phase == CoroTcpCompletionPhase::TimedOut ||
            phase == CoroTcpCompletionPhase::Cancelled) {
            m_finished = true;
            releaseUserDataOnly();
            rollbackResult();
            return true;
        }
        if (phase != CoroTcpCompletionPhase::Completed) {
            return true;
        }
        m_finished = true;
        C_IOResult result = buildResult();
        const bool completed = completeUserData(result);
        if (m_complete_accepted) {
            commitResult();
        } else {
            rollbackResult();
        }
        return completed;
    }

    C_IOResult immediateResult() noexcept
    {
        m_finished = true;
        return buildResult();
    }

    C_IOResult finishWithoutWait(C_IOResult result) noexcept
    {
        m_finished = true;
        const C_IOResult completed = completeAndReleaseUserData(result);
        m_complete_accepted = completed.code == C_IOResultOk && result.code == C_IOResultOk;
        if (m_complete_accepted) {
            commitResult();
        } else {
            rollbackResult();
        }
        return merge_cleanup_result(result, completed);
    }

    void retireToken() noexcept
    {
        m_finished = true;
        releaseUserDataOnly();
    }

    void cancelFromClose() noexcept override
    {
        if (m_finished) {
            return;
        }
        CoroTcpCompletionPhase expected = CoroTcpCompletionPhase::Pending;
        if (m_state.phase.compare_exchange_strong(expected,
                                                  CoroTcpCompletionPhase::Cancelled,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            m_last_cleanup_result =
                completeUserDataNoRelease(make_result(C_IOResultCancelled));
        }
    }

    Scheduler* scheduler() const noexcept { return m_scheduler; }
    bool hasPendingToken() noexcept
    {
        std::lock_guard<std::mutex> lock(m_state.user_data_mutex);
        return m_state.user_data != nullptr;
    }

    C_IOResult wait(int64_t timeout_ms) noexcept
    {
        return m_wait_ops.wait != nullptr
            ? m_wait_ops.wait(m_wait_ops.ctx, timeout_ms)
            : make_result(C_IOResultInvalid);
    }

    void markWaitTimeout() noexcept
    {
        CoroTcpCompletionPhase expected = CoroTcpCompletionPhase::Pending;
        if (!m_state.phase.compare_exchange_strong(expected,
                                                   CoroTcpCompletionPhase::TimedOut,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            return;
        }
    }

protected:
    template <typename Fn>
    bool guardedHandleComplete(Fn&& fn) noexcept
    {
        CoroTcpCompletionPhase expected = CoroTcpCompletionPhase::Pending;
        if (!m_state.phase.compare_exchange_strong(expected,
                                                   CoroTcpCompletionPhase::IoCompleting,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            if (expected == CoroTcpCompletionPhase::TimedOut ||
                expected == CoroTcpCompletionPhase::Cancelled) {
                return false;
            }
            return expected == CoroTcpCompletionPhase::Completed;
        }

        bool completed = fn();

        if (completed) {
            m_state.phase.store(CoroTcpCompletionPhase::Completed,
                                std::memory_order_release);
            return true;
        }

        m_state.phase.store(CoroTcpCompletionPhase::Pending,
                            std::memory_order_release);
        return false;
    }

private:
    virtual C_IOResult buildResult() = 0;
    virtual void commitResult() noexcept {}
    virtual void rollbackResult() noexcept {}

    bool completeUserData(C_IOResult result) noexcept
    {
        m_complete_accepted = false;
        const C_IOResult completed = completeAndReleaseUserData(result);
        if (completed.code == C_IOResultInvalid) {
            return true;
        }
        m_complete_accepted = completed.code == C_IOResultOk;
        return completed.code == C_IOResultOk || completed.code == C_IOResultInvalid;
    }

    C_IOResult completeUserDataNoRelease(C_IOResult result) noexcept
    {
        return completeUserDataNoRelease(&m_state, result);
    }

    static C_IOResult completeUserDataNoRelease(CoroTcpCompletionState* state,
                                                C_IOResult result) noexcept
    {
        if (state == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        std::lock_guard<std::mutex> lock(state->user_data_mutex);
        if (state->user_data == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        return state->wait_ops.complete_user_data(state->user_data, result);
    }

    C_IOResult completeAndReleaseUserData(C_IOResult result) noexcept
    {
        void* user_data = nullptr;
        C_IOResult completed = make_result(C_IOResultInvalid);
        {
            std::lock_guard<std::mutex> lock(m_state.user_data_mutex);
            user_data = std::exchange(m_state.user_data, nullptr);
            if (user_data != nullptr) {
                completed = m_wait_ops.complete_user_data(user_data, result);
            }
        }
        if (user_data != nullptr) {
            C_IOResult released = m_wait_ops.release_user_data(user_data);
            completed = merge_cleanup_result(completed, released);
        }
        return completed;
    }

    void releaseUserDataOnly() noexcept
    {
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_state.user_data_mutex);
            user_data = std::exchange(m_state.user_data, nullptr);
        }
        if (user_data != nullptr) {
            m_last_cleanup_result = m_wait_ops.release_user_data(user_data);
        }
    }

    static Scheduler* wake_owner(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroTcpWakeState*>(state);
        return wake_state != nullptr && wake_state->operation != nullptr
            ? wake_state->operation->scheduler()
            : nullptr;
    }

    static bool wake_request(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroTcpWakeState*>(state);
        return wake_state != nullptr && wake_state->operation != nullptr &&
            wake_state->operation->completeFromWake();
    }

    static void wake_retain(void*) noexcept {}
    static void wake_release(void*) noexcept {}

    inline static const galay::kernel::detail::ResumeTokenHooks kWakeHooks{
        .owner_scheduler = wake_owner,
        .request_resume = wake_request,
        .retain = wake_retain,
        .release = wake_release,
    };

    Scheduler* m_scheduler = nullptr;
    GalayCoreCoroWaitOps m_wait_ops{};
    CoroTcpCompletionState m_state;
    CoroTcpWakeState m_wake_state{};
    bool m_finished = false;
    bool m_complete_accepted = false;
    C_IOResult m_last_cleanup_result = make_result(C_IOResultOk);
};

struct CoroAcceptOperation final: public AcceptAwaitable, public CoroTcpOperationBase {
    CoroAcceptOperation(IOController* controller,
                        Scheduler* scheduler,
                        void* user_data,
                        GalayCoreCoroWaitOps wait_ops,
                        void** out_socket,
                        C_Host* out_peer)
        : AcceptAwaitable(controller, &m_peer)
        , CoroTcpOperationBase(scheduler, user_data, wait_ops)
        , m_out_socket(out_socket)
        , m_out_peer(out_peer)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = ACCEPT;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return AcceptAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return AcceptAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
#ifdef USE_IOURING
        m_controller->m_accept_result_assigned = false;
#endif
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        if (!assign_cpp_host_to_c_host(m_peer, m_out_peer)) {
            if (*m_result != GHandle::invalid()) {
                if (galay_close((*m_result).fd) != 0) {
                    return make_result(C_IOResultError, errno);
                }
            }
            return make_result(C_IOResultError);
        }
        if (m_out_socket == nullptr || *m_out_socket != nullptr) {
            if (*m_result != GHandle::invalid()) {
                if (galay_close((*m_result).fd) != 0) {
                    return make_result(C_IOResultError, errno);
                }
            }
            return make_result(C_IOResultInvalid);
        }

        TcpSocket accepted_socket(*m_result);
        auto non_block = accepted_socket.option().handleNonBlock();
        if (!non_block) {
            return from_io_error(non_block.error());
        }

        m_pending_socket.reset(new (std::nothrow) TcpSocket(std::move(accepted_socket)));
        if (!m_pending_socket) {
            return make_result(C_IOResultError, ENOMEM);
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.value = m_pending_socket->handle().fd;
        result.ptr = m_out_socket;
        return result;
    }

    void commitResult() noexcept override
    {
        if (m_out_socket != nullptr && *m_out_socket == nullptr && m_pending_socket) {
            *m_out_socket = m_pending_socket.release();
        }
    }

    void rollbackResult() noexcept override
    {
        m_pending_socket.reset();
    }

    galay::kernel::Host m_peer;
    void** m_out_socket = nullptr;
    C_Host* m_out_peer = nullptr;
    std::unique_ptr<TcpSocket> m_pending_socket;
};

struct CoroConnectOperation final: public ConnectAwaitable, public CoroTcpOperationBase {
    CoroConnectOperation(IOController* controller,
                         Scheduler* scheduler,
                         void* user_data,
                         GalayCoreCoroWaitOps wait_ops,
                         const galay::kernel::Host& host)
        : ConnectAwaitable(controller, host)
        , CoroTcpOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = CONNECT;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return ConnectAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return ConnectAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
        return m_result ? make_result(C_IOResultOk) : from_io_error(m_result.error());
    }
};

struct CoroRecvOperation final: public RecvAwaitable, public CoroTcpOperationBase {
    CoroRecvOperation(IOController* controller,
                      Scheduler* scheduler,
                      void* user_data,
                      GalayCoreCoroWaitOps wait_ops,
                      char* buffer,
                      size_t length)
        : RecvAwaitable(controller, buffer, length)
        , CoroTcpOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = RECV;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return RecvAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return RecvAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
#ifdef USE_IOURING
        m_controller->m_recv_result_assigned = false;
#endif
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(*m_result == 0 ? C_IOResultEof : C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

struct CoroSendOperation final: public SendAwaitable, public CoroTcpOperationBase {
    CoroSendOperation(IOController* controller,
                      Scheduler* scheduler,
                      void* user_data,
                      GalayCoreCoroWaitOps wait_ops,
                      const char* buffer,
                      size_t length)
        : SendAwaitable(controller, buffer, length)
        , CoroTcpOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = SEND;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return SendAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return SendAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

struct CoroReadvOperation final: public ReadvAwaitable, public CoroTcpOperationBase {
    CoroReadvOperation(IOController* controller,
                       Scheduler* scheduler,
                       void* user_data,
                       GalayCoreCoroWaitOps wait_ops,
                       const struct iovec* iovecs,
                       size_t count)
        : ReadvAwaitable(controller, std::span<const struct iovec>(iovecs, count))
        , CoroTcpOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = READV;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return ReadvAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return ReadvAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(*m_result == 0 ? C_IOResultEof : C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

struct CoroWritevOperation final: public WritevAwaitable, public CoroTcpOperationBase {
    CoroWritevOperation(IOController* controller,
                        Scheduler* scheduler,
                        void* user_data,
                        GalayCoreCoroWaitOps wait_ops,
                        const struct iovec* iovecs,
                        size_t count)
        : WritevAwaitable(controller, std::span<const struct iovec>(iovecs, count))
        , CoroTcpOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = WRITEV;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return WritevAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return WritevAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

struct CoroSendFileOperation final: public SendFileAwaitable, public CoroTcpOperationBase {
    CoroSendFileOperation(IOController* controller,
                          Scheduler* scheduler,
                          void* user_data,
                          GalayCoreCoroWaitOps wait_ops,
                          int file_fd,
                          off_t offset,
                          size_t count)
        : SendFileAwaitable(controller, file_fd, offset, count)
        , CoroTcpOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = SENDFILE;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return SendFileAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return SendFileAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

void cancel_coro_operation(void* awaitable)
{
    if (awaitable == nullptr) {
        return;
    }
    auto* base = static_cast<galay::kernel::AwaitableBase*>(awaitable);
    auto* operation = dynamic_cast<CoroTcpOperationInterface*>(base);
    if (operation != nullptr) {
        operation->cancelFromClose();
    }
}

bool is_direct_coro_operation(void* awaitable)
{
    if (awaitable == nullptr) {
        return false;
    }
    auto* base = static_cast<galay::kernel::AwaitableBase*>(awaitable);
    return dynamic_cast<CoroTcpOperationInterface*>(base) != nullptr;
}

bool has_non_direct_pending_operation(IOController* controller)
{
    if (controller == nullptr) {
        return true;
    }
    if (controller->m_sequence_owner[IOController::READ] != nullptr ||
        controller->m_sequence_owner[IOController::WRITE] != nullptr) {
        return true;
    }
    void* read = controller->m_awaitable[IOController::READ];
    if (read != nullptr && !is_direct_coro_operation(read)) {
        return true;
    }
    void* write = controller->m_awaitable[IOController::WRITE];
    return write != nullptr && !is_direct_coro_operation(write);
}

bool has_live_direct_pending_operation(IOController* controller)
{
    if (controller == nullptr) {
        return false;
    }
    return is_direct_coro_operation(controller->m_awaitable[IOController::READ]) ||
        is_direct_coro_operation(controller->m_awaitable[IOController::WRITE]);
}

void clear_controller_owner_if_no_live_direct_operation(IOController* controller) noexcept
{
    if (controller != nullptr && !has_live_direct_pending_operation(controller)) {
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
    }
}

bool validate_controller_owner(IOController* controller, Scheduler* scheduler)
{
    if (controller == nullptr || scheduler == nullptr) {
        return false;
    }
    Scheduler* expected = nullptr;
    if (controller->m_owner_scheduler.compare_exchange_strong(
            expected,
            scheduler,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return true;
    }
    return expected == scheduler ||
        controller->m_owner_scheduler.load(std::memory_order_acquire) == scheduler;
}

void cancel_coro_operations(IOController* controller)
{
    cancel_coro_operation(controller->m_awaitable[IOController::READ]);
    cancel_coro_operation(controller->m_awaitable[IOController::WRITE]);
}

Scheduler* pending_coro_operation_scheduler(void* awaitable)
{
    if (awaitable == nullptr) {
        return nullptr;
    }
    auto* base = static_cast<galay::kernel::AwaitableBase*>(awaitable);
    auto* operation = dynamic_cast<CoroTcpOperationBase*>(base);
    return operation != nullptr ? operation->scheduler() : nullptr;
}

template <typename Operation, typename Awaitable>
C_IOResult perform_registered_io(IOController* controller,
                                 Scheduler* scheduler,
                                 IOEventType event,
                                 int64_t timeout_ms,
                                 Operation& operation,
                                 Awaitable* awaitable)
{
    const IOController::Index slot = slot_for_event(event);
    const IOController::Index other_slot =
        slot == IOController::READ ? IOController::WRITE : IOController::READ;
    if (controller == nullptr || scheduler == nullptr || slot == IOController::SIZE ||
        controller->m_awaitable[slot] != nullptr ||
        controller->m_sequence_owner[slot] != nullptr ||
        controller->m_awaitable[other_slot] != nullptr ||
        controller->m_sequence_owner[other_slot] != nullptr) {
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }
    if (!validate_controller_owner(controller, scheduler)) {
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }

    if (!controller->fillAwaitable(event, awaitable)) {
        clear_controller_owner_if_no_live_direct_operation(controller);
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }

    const int registered = galay::kernel::detail::registerIOSchedulerEvent(
        scheduler, event, controller);
    if (registered == 1) {
        controller->removeAwaitable(event);
        C_IOResult result = operation.immediateResult();
        result = operation.finishWithoutWait(result);
        clear_controller_owner_if_no_live_direct_operation(controller);
        return result;
    }
    if (registered < 0) {
        controller->removeAwaitable(event);
        C_IOResult result = operation.finishWithoutWait(
            make_result(C_IOResultError, galay::kernel::detail::normalizeAwaitableErrno(registered)));
        clear_controller_owner_if_no_live_direct_operation(controller);
        return result;
    }

    C_IOResult result = operation.wait(timeout_ms);
    if (result.code == C_IOResultTimeout) {
        operation.markWaitTimeout();
    }
    if (result.code == C_IOResultTimeout && event == CONNECT) {
        if (operation.hasPendingToken()) {
            result = operation.finishWithoutWait(result);
        }
        const int closed = galay::kernel::detail::registerIOSchedulerClose(scheduler, controller);
        if (closed == 0) {
            controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
            return result;
        }
        return make_result(C_IOResultError,
                           galay::kernel::detail::normalizeAwaitableErrno(closed));
    }
    if (operation.hasPendingToken()) {
#ifdef USE_IOURING
        const bool keep_persistent_multishot =
            (event == ACCEPT || event == RECV) &&
            (result.code == C_IOResultTimeout || result.code == C_IOResultCancelled);
        if (!keep_persistent_multishot) {
            const int removed = static_cast<IOScheduler*>(scheduler)->remove(controller);
            if (removed < 0) {
                result = merge_cleanup_result(
                    result,
                    make_result(C_IOResultError,
                                galay::kernel::detail::normalizeAwaitableErrno(removed)));
            }
        }
#else
        const int removed = static_cast<IOScheduler*>(scheduler)->remove(controller);
        if (removed < 0) {
            result = merge_cleanup_result(
                result,
                make_result(C_IOResultError,
                            galay::kernel::detail::normalizeAwaitableErrno(removed)));
        }
#endif
        result = operation.finishWithoutWait(result);
    }
    if (controller->m_awaitable[slot] == awaitable) {
        controller->removeAwaitable(event);
    }
    clear_controller_owner_if_no_live_direct_operation(controller);
    return result;
}

} // namespace

extern "C" {

GalayCoreCoroIOResult galay_core_coro_tcp_accept(void* listener_socket,
                                                 void* scheduler_handle,
                                                 void** out_socket,
                                                 GalayCoreCoroHost* out_peer,
                                                 int64_t timeout_ms,
                                                 void* user_data,
                                                 const GalayCoreCoroWaitOps* wait_ops)
{
    auto* socket = to_cpp_socket(listener_socket);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || out_socket == nullptr || *out_socket != nullptr ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroAcceptOperation operation(
        socket->controller(), scheduler, user_data, *wait_ops, out_socket, out_peer);
    return perform_registered_io(socket->controller(),
                                 scheduler,
                                 ACCEPT,
                                 timeout_ms,
                                 operation,
                                 static_cast<AcceptAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_connect(void* socket_handle,
                                                  void* scheduler_handle,
                                                  const GalayCoreCoroHost* host,
                                                  int64_t timeout_ms,
                                                  void* user_data,
                                                  const GalayCoreCoroWaitOps* wait_ops)
{
    auto* socket = to_cpp_socket(socket_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || host == nullptr || !is_valid_c_ip_type(host->type) ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    auto cpp_host = from_c_host_to_cpp_host(*host);
    if (!cpp_host.valid()) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    auto non_block = socket->option().handleNonBlock();
    if (!non_block) {
        return from_io_error(non_block.error());
    }
    CoroConnectOperation operation(
        socket->controller(), scheduler, user_data, *wait_ops, cpp_host);
    return perform_registered_io(socket->controller(),
                                 scheduler,
                                 CONNECT,
                                 timeout_ms,
                                 operation,
                                 static_cast<ConnectAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_recv(void* socket_handle,
                                               void* scheduler_handle,
                                               char* buffer,
                                               size_t length,
                                               int64_t timeout_ms,
                                               void* user_data,
                                               const GalayCoreCoroWaitOps* wait_ops)
{
    auto* socket = to_cpp_socket(socket_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || buffer == nullptr || length == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroRecvOperation operation(
        socket->controller(), scheduler, user_data, *wait_ops, buffer, length);
    return perform_registered_io(socket->controller(),
                                 scheduler,
                                 RECV,
                                 timeout_ms,
                                 operation,
                                 static_cast<RecvAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_send(void* socket_handle,
                                               void* scheduler_handle,
                                               const char* buffer,
                                               size_t length,
                                               int64_t timeout_ms,
                                               void* user_data,
                                               const GalayCoreCoroWaitOps* wait_ops)
{
    auto* socket = to_cpp_socket(socket_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || buffer == nullptr || length == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroSendOperation operation(
        socket->controller(), scheduler, user_data, *wait_ops, buffer, length);
    return perform_registered_io(socket->controller(),
                                 scheduler,
                                 SEND,
                                 timeout_ms,
                                 operation,
                                 static_cast<SendAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_readv(void* socket_handle,
                                                void* scheduler_handle,
                                                const struct iovec* iovecs,
                                                size_t count,
                                                int64_t timeout_ms,
                                                void* user_data,
                                                const GalayCoreCoroWaitOps* wait_ops)
{
    auto* socket = to_cpp_socket(socket_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || iovecs == nullptr || count == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroReadvOperation operation(
        socket->controller(), scheduler, user_data, *wait_ops, iovecs, count);
    return perform_registered_io(socket->controller(),
                                 scheduler,
                                 READV,
                                 timeout_ms,
                                 operation,
                                 static_cast<ReadvAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_writev(void* socket_handle,
                                                 void* scheduler_handle,
                                                 const struct iovec* iovecs,
                                                 size_t count,
                                                 int64_t timeout_ms,
                                                 void* user_data,
                                                 const GalayCoreCoroWaitOps* wait_ops)
{
    auto* socket = to_cpp_socket(socket_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || iovecs == nullptr || count == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroWritevOperation operation(
        socket->controller(), scheduler, user_data, *wait_ops, iovecs, count);
    return perform_registered_io(socket->controller(),
                                 scheduler,
                                 WRITEV,
                                 timeout_ms,
                                 operation,
                                 static_cast<WritevAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_sendfile(void* socket_handle,
                                                   void* scheduler_handle,
                                                   int file_fd,
                                                   int64_t offset,
                                                   size_t count,
                                                   int64_t timeout_ms,
                                                   void* user_data,
                                                   const GalayCoreCoroWaitOps* wait_ops)
{
    auto* socket = to_cpp_socket(socket_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || file_fd < 0 || offset < 0 || count == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroSendFileOperation operation(socket->controller(),
                                    scheduler,
                                    user_data,
                                    *wait_ops,
                                    file_fd,
                                    static_cast<off_t>(offset),
                                    count);
    return perform_registered_io(socket->controller(),
                                 scheduler,
                                 SENDFILE,
                                 timeout_ms,
                                 operation,
                                 static_cast<SendFileAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_close(void* socket_handle,
                                                void* scheduler_handle,
                                                int64_t timeout_ms)
{
    (void)timeout_ms;
    auto* socket = to_cpp_socket(socket_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || scheduler == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    IOController* controller = socket->controller();
    if (has_non_direct_pending_operation(controller)) {
        return make_result(C_IOResultInvalid);
    }
    if (!validate_controller_owner(controller, scheduler)) {
        return make_result(C_IOResultInvalid);
    }
    Scheduler* pending_scheduler =
        pending_coro_operation_scheduler(controller->m_awaitable[IOController::READ]);
    if (pending_scheduler == nullptr) {
        pending_scheduler =
            pending_coro_operation_scheduler(controller->m_awaitable[IOController::WRITE]);
    }
    if (pending_scheduler != nullptr && pending_scheduler != scheduler) {
        return make_result(C_IOResultInvalid);
    }
    cancel_coro_operations(controller);
    const int closed = galay::kernel::detail::registerIOSchedulerClose(scheduler, controller);
    if (closed == 0) {
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
        return make_result(C_IOResultOk);
    }
    return make_result(C_IOResultError, galay::kernel::detail::normalizeAwaitableErrno(closed));
}

} // extern "C"
