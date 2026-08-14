#include "c_coro_udp_bridge.h"
#include "c_coro_operation_base.h"

#include <galay/cpp/galay-kernel/async/async_udp.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>

#include <cstring>
#include <string>

namespace
{

using galay::async::AsyncUdpSocket;
using galay::kernel::IOController;
using galay::kernel::RecvFromAwaitable;
using galay::kernel::Scheduler;
using galay::kernel::SendToAwaitable;

using galay::bridge::detail::CoroOperationBase;
using galay::bridge::detail::C_IOResult;
using galay::bridge::detail::C_IOResultError;
using galay::bridge::detail::C_IOResultInvalid;
using galay::bridge::detail::C_IOResultOk;
using galay::bridge::detail::C_IOResultTimeout;
using galay::bridge::detail::from_io_error;
using galay::bridge::detail::make_result;
using galay::bridge::detail::perform_coro_close;
using galay::bridge::detail::perform_registered_io;
using galay::bridge::detail::timeout_fits_chrono;
using galay::bridge::detail::to_io_scheduler;
using galay::bridge::detail::valid_wait_ops;

using C_Host = GalayCoreCoroHost;
using C_IPType = GalayCoreCoroIPType;

constexpr C_IPType C_IPTypeIPV4 = GalayCoreCoroIPTypeIPV4;
constexpr C_IPType C_IPTypeIPV6 = GalayCoreCoroIPTypeIPV6;

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

AsyncUdpSocket* to_cpp_socket(GalayCoreUdpSocket* socket)
{
    return reinterpret_cast<AsyncUdpSocket*>(socket);
}

struct CoroRecvFromOperation final:
    public RecvFromAwaitable,
    public CoroOperationBase<CoroRecvFromOperation> {
    CoroRecvFromOperation(IOController* controller,
                          Scheduler* scheduler,
                          void* user_data,
                          GalayCoreCoroWaitOps wait_ops,
                          char* buffer,
                          size_t length,
                          C_Host* out_from)
        : RecvFromAwaitable(controller, buffer, length, &m_from)
        , CoroOperationBase(scheduler, user_data, wait_ops)
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

    C_IOResult buildResultImpl() noexcept
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

struct CoroSendToOperation final:
    public SendToAwaitable,
    public CoroOperationBase<CoroSendToOperation> {
    CoroSendToOperation(IOController* controller,
                        Scheduler* scheduler,
                        void* user_data,
                        GalayCoreCoroWaitOps wait_ops,
                        const char* buffer,
                        size_t length,
                        const galay::kernel::Host& to)
        : SendToAwaitable(controller, buffer, length, to)
        , CoroOperationBase(scheduler, user_data, wait_ops)
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

    C_IOResult buildResultImpl() noexcept
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

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
    return perform_registered_io<true, false>(socket->controller(),
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
    return perform_registered_io<true, false>(socket->controller(),
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
    return perform_coro_close(socket->controller(), scheduler);
}

} // extern "C"
