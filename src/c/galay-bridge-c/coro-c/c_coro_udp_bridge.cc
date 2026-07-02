#include "c_coro_udp_bridge.h"

#include <galay/cpp/galay-kernel/async/udp_socket.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace
{

using galay::async::UdpSocket;
using galay::kernel::IOController;
using galay::kernel::IOError;
using galay::kernel::IOScheduler;
using galay::kernel::RecvFromAwaitable;
using galay::kernel::Scheduler;
using galay::kernel::SendToAwaitable;

using C_IOResult = GalayCoreCoroIOResult;
using C_IOResultCode = GalayCoreCoroIOResultCode;
using C_Host = GalayCoreCoroHost;
using C_IPType = GalayCoreCoroIPType;

constexpr C_IOResultCode C_IOResultOk = GalayCoreCoroIOResultOk;
constexpr C_IOResultCode C_IOResultTimeout = GalayCoreCoroIOResultTimeout;
constexpr C_IOResultCode C_IOResultCancelled = GalayCoreCoroIOResultCancelled;
constexpr C_IOResultCode C_IOResultInvalid = GalayCoreCoroIOResultInvalid;
constexpr C_IOResultCode C_IOResultError = GalayCoreCoroIOResultError;
constexpr C_IPType C_IPTypeIPV4 = GalayCoreCoroIPTypeIPV4;
constexpr C_IPType C_IPTypeIPV6 = GalayCoreCoroIPTypeIPV6;

struct CoroUdpOperationBase;

struct CoroUdpWakeState {
    galay::kernel::detail::ResumeTokenHeader header;
    CoroUdpOperationBase* operation = nullptr;
};

enum class CoroUdpCompletionPhase : uint8_t {
    Pending,
    IoCompleting,
    TimedOut,
    Cancelled,
    Completed,
};

struct CoroUdpCompletionState {
    std::atomic<CoroUdpCompletionPhase> phase{CoroUdpCompletionPhase::Pending};
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
    if (address.empty() || address.size() >= sizeof(out_host->address)) {
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
    return make_result(C_IOResultError, io_error_sys_errno(error));
}

UdpSocket* to_cpp_socket(GalayCoreUdpSocket* socket)
{
    return reinterpret_cast<UdpSocket*>(socket);
}

Scheduler* to_io_scheduler(GalayCoreIOScheduler* scheduler_handle)
{
    auto* scheduler = reinterpret_cast<Scheduler*>(scheduler_handle);
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
    case RECVFROM:
        return IOController::READ;
    case SENDTO:
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

struct CoroUdpOperationInterface {
    virtual ~CoroUdpOperationInterface() = default;
    virtual void cancelFromClose() noexcept = 0;
};

struct CoroUdpOperationBase: public CoroUdpOperationInterface {
    CoroUdpOperationBase(Scheduler* scheduler,
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
        const CoroUdpCompletionPhase phase =
            m_state.phase.load(std::memory_order_acquire);
        if (phase == CoroUdpCompletionPhase::TimedOut ||
            phase == CoroUdpCompletionPhase::Cancelled) {
            m_finished = true;
            releaseUserDataOnly();
            return true;
        }
        if (phase != CoroUdpCompletionPhase::Completed) {
            return true;
        }
        m_finished = true;
        return completeUserData(buildResult());
    }

    C_IOResult immediateResult() noexcept
    {
        m_finished = true;
        return buildResult();
    }

    C_IOResult finishWithoutWait(C_IOResult result) noexcept
    {
        m_finished = true;
        C_IOResult completed = completeAndReleaseUserData(result);
        return merge_cleanup_result(result, completed);
    }

    void cancelFromClose() noexcept override
    {
        if (m_finished) {
            return;
        }
        CoroUdpCompletionPhase expected = CoroUdpCompletionPhase::Pending;
        if (m_state.phase.compare_exchange_strong(expected,
                                                  CoroUdpCompletionPhase::Cancelled,
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
        CoroUdpCompletionPhase expected = CoroUdpCompletionPhase::Pending;
        if (!m_state.phase.compare_exchange_strong(expected,
                                                   CoroUdpCompletionPhase::TimedOut,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            return;
        }
    }

protected:
    template <typename Fn>
    bool guardedHandleComplete(Fn&& fn) noexcept
    {
        CoroUdpCompletionPhase expected = CoroUdpCompletionPhase::Pending;
        if (!m_state.phase.compare_exchange_strong(expected,
                                                   CoroUdpCompletionPhase::IoCompleting,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            return expected == CoroUdpCompletionPhase::Completed;
        }
        const bool completed = fn();
        m_state.phase.store(completed ? CoroUdpCompletionPhase::Completed
                                      : CoroUdpCompletionPhase::Pending,
                            std::memory_order_release);
        return completed;
    }

private:
    virtual C_IOResult buildResult() = 0;

    bool completeUserData(C_IOResult result) noexcept
    {
        const C_IOResult completed = completeAndReleaseUserData(result);
        return completed.code == C_IOResultOk || completed.code == C_IOResultInvalid;
    }

    C_IOResult completeUserDataNoRelease(C_IOResult result) noexcept
    {
        std::lock_guard<std::mutex> lock(m_state.user_data_mutex);
        if (m_state.user_data == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        return m_state.wait_ops.complete_user_data(m_state.user_data, result);
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
        auto* wake_state = static_cast<CoroUdpWakeState*>(state);
        return wake_state != nullptr && wake_state->operation != nullptr
            ? wake_state->operation->scheduler()
            : nullptr;
    }

    static bool wake_request(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroUdpWakeState*>(state);
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
    CoroUdpCompletionState m_state;
    CoroUdpWakeState m_wake_state{};
    bool m_finished = false;
    C_IOResult m_last_cleanup_result = make_result(C_IOResultOk);
};

struct CoroRecvFromOperation final: public RecvFromAwaitable, public CoroUdpOperationBase {
    CoroRecvFromOperation(IOController* controller,
                          Scheduler* scheduler,
                          void* user_data,
                          GalayCoreCoroWaitOps wait_ops,
                          char* buffer,
                          size_t length,
                          C_Host* out_from)
        : RecvFromAwaitable(controller, buffer, length, &m_from)
        , CoroUdpOperationBase(scheduler, user_data, wait_ops)
        , m_out_from(out_from)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = RECVFROM;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return RecvFromAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return RecvFromAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        if (!assign_cpp_host_to_c_host(m_from, m_out_from)) {
            return make_result(C_IOResultError);
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }

    galay::kernel::Host m_from;
    C_Host* m_out_from = nullptr;
};

struct CoroSendToOperation final: public SendToAwaitable, public CoroUdpOperationBase {
    CoroSendToOperation(IOController* controller,
                        Scheduler* scheduler,
                        void* user_data,
                        GalayCoreCoroWaitOps wait_ops,
                        const char* buffer,
                        size_t length,
                        const galay::kernel::Host& to)
        : SendToAwaitable(controller, buffer, length, to)
        , CoroUdpOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = SENDTO;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return SendToAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return SendToAwaitable::handleComplete(handle); });
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
    auto* operation = dynamic_cast<CoroUdpOperationInterface*>(base);
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
    return dynamic_cast<CoroUdpOperationInterface*>(base) != nullptr;
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
    auto* operation = dynamic_cast<CoroUdpOperationBase*>(base);
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
    if (operation.hasPendingToken()) {
#ifdef USE_IOURING
        const bool keep_persistent_multishot =
            event == RECVFROM && (result.code == C_IOResultTimeout ||
                                  result.code == C_IOResultCancelled);
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

GalayCoreCoroIOResult galay_core_coro_udp_recvfrom(GalayCoreUdpSocket* socket_handle,
                                                   GalayCoreIOScheduler* scheduler_handle,
                                                   char* buffer,
                                                   size_t length,
                                                   GalayCoreCoroHost* from,
                                                   int64_t timeout_ms,
                                                   void* user_data,
                                                   const GalayCoreCoroWaitOps* wait_ops)
{
    auto* socket = to_cpp_socket(socket_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || (buffer == nullptr && length != 0) ||
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
    CoroRecvFromOperation operation(socket->controller(),
                                    scheduler,
                                    user_data,
                                    *wait_ops,
                                    buffer,
                                    length,
                                    from);
    return perform_registered_io(socket->controller(),
                                 scheduler,
                                 RECVFROM,
                                 timeout_ms,
                                 operation,
                                 static_cast<RecvFromAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_udp_sendto(GalayCoreUdpSocket* socket_handle,
                                                 GalayCoreIOScheduler* scheduler_handle,
                                                 const char* buffer,
                                                 size_t length,
                                                 const GalayCoreCoroHost* to,
                                                 int64_t timeout_ms,
                                                 void* user_data,
                                                 const GalayCoreCoroWaitOps* wait_ops)
{
    auto* socket = to_cpp_socket(socket_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (socket == nullptr || (buffer == nullptr && length != 0) ||
        to == nullptr || !is_valid_c_ip_type(to->type) ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    auto cpp_to = from_c_host_to_cpp_host(*to);
    if (!cpp_to.valid()) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroSendToOperation operation(socket->controller(),
                                  scheduler,
                                  user_data,
                                  *wait_ops,
                                  buffer,
                                  length,
                                  cpp_to);
    return perform_registered_io(socket->controller(),
                                 scheduler,
                                 SENDTO,
                                 timeout_ms,
                                 operation,
                                 static_cast<SendToAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_udp_close(GalayCoreUdpSocket* socket_handle,
                                                GalayCoreIOScheduler* scheduler_handle,
                                                int64_t)
{
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
