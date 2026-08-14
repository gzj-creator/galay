#include "c_coro_tcp_bridge.h"
#include "c_coro_operation_base.h"

#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>

#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <sys/uio.h>
#include <utility>

namespace
{

using galay::async::AsyncTcpSocket;
using galay::kernel::AcceptAwaitable;
using galay::kernel::ConnectAwaitable;
using galay::kernel::IOController;
using galay::kernel::ReadvAwaitable;
using galay::kernel::RecvAwaitable;
using galay::kernel::Scheduler;
using galay::kernel::SendFileAwaitable;
using galay::kernel::SendAwaitable;
using galay::kernel::WritevAwaitable;

using galay::bridge::detail::CoroOperationBase;
using galay::bridge::detail::C_IOResult;
using galay::bridge::detail::C_IOResultEof;
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

AsyncTcpSocket* to_cpp_socket(GalayCoreTcpSocket* socket)
{
    return reinterpret_cast<AsyncTcpSocket*>(socket);
}

std::unique_ptr<struct iovec[]> make_platform_iovecs(const galay_iovec_t* iovecs,
                                                     size_t count)
{
    auto platform_iovecs = std::unique_ptr<struct iovec[]>(
        new (std::nothrow) struct iovec[count]);
    if (!platform_iovecs) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        platform_iovecs[i].iov_base = iovecs[i].base;
        platform_iovecs[i].iov_len = iovecs[i].len;
    }
    return platform_iovecs;
}

struct PlatformIovecStorage {
    PlatformIovecStorage(std::unique_ptr<struct iovec[]> iovecs, size_t count) noexcept
        : iovecs(std::move(iovecs))
        , count(count)
    {
    }

    std::span<const struct iovec> span() const noexcept
    {
        return std::span<const struct iovec>(iovecs.get(), count);
    }

    std::unique_ptr<struct iovec[]> iovecs;
    size_t count = 0;
};

struct CoroAcceptOperation final:
    public AcceptAwaitable,
    public CoroOperationBase<CoroAcceptOperation> {
    CoroAcceptOperation(IOController* controller,
                        Scheduler* scheduler,
                        void* user_data,
                        GalayCoreCoroWaitOps wait_ops,
                        GalayCoreTcpSocket** out_socket,
                        C_Host* out_peer)
        : AcceptAwaitable(controller, &m_peer)
        , CoroOperationBase(scheduler, user_data, wait_ops)
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

    C_IOResult buildResultImpl() noexcept
    {
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

        AsyncTcpSocket accepted_socket(*m_result);
        auto non_block = accepted_socket.option().handleNonBlock();
        if (!non_block) {
            return from_io_error(non_block.error());
        }

        m_pending_socket.reset(new (std::nothrow) AsyncTcpSocket(std::move(accepted_socket)));
        if (!m_pending_socket) {
            return make_result(C_IOResultError, ENOMEM);
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.value = m_pending_socket->handle().fd;
        result.ptr = m_out_socket;
        return result;
    }

    void commitResultImpl() noexcept
    {
        if (m_out_socket != nullptr && *m_out_socket == nullptr && m_pending_socket) {
            *m_out_socket = reinterpret_cast<GalayCoreTcpSocket*>(m_pending_socket.release());
        }
    }

    void rollbackResultImpl() noexcept
    {
        m_pending_socket.reset();
    }

    galay::kernel::Host m_peer;
    GalayCoreTcpSocket** m_out_socket = nullptr;
    C_Host* m_out_peer = nullptr;
    std::unique_ptr<AsyncTcpSocket> m_pending_socket;
};

struct CoroConnectOperation final:
    public ConnectAwaitable,
    public CoroOperationBase<CoroConnectOperation> {
    CoroConnectOperation(IOController* controller,
                         Scheduler* scheduler,
                         void* user_data,
                         GalayCoreCoroWaitOps wait_ops,
                         const galay::kernel::Host& host)
        : ConnectAwaitable(controller, host)
        , CoroOperationBase(scheduler, user_data, wait_ops)
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

    C_IOResult buildResultImpl() noexcept
    {
        return m_result ? make_result(C_IOResultOk) : from_io_error(m_result.error());
    }
};

struct CoroRecvOperation final:
    public RecvAwaitable,
    public CoroOperationBase<CoroRecvOperation> {
    CoroRecvOperation(IOController* controller,
                      Scheduler* scheduler,
                      void* user_data,
                      GalayCoreCoroWaitOps wait_ops,
                      char* buffer,
                      size_t length)
        : RecvAwaitable(controller, buffer, length)
        , CoroOperationBase(scheduler, user_data, wait_ops)
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

    C_IOResult buildResultImpl() noexcept
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(*m_result == 0 ? C_IOResultEof : C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

struct CoroSendOperation final:
    public SendAwaitable,
    public CoroOperationBase<CoroSendOperation> {
    CoroSendOperation(IOController* controller,
                      Scheduler* scheduler,
                      void* user_data,
                      GalayCoreCoroWaitOps wait_ops,
                      const char* buffer,
                      size_t length)
        : SendAwaitable(controller, buffer, length)
        , CoroOperationBase(scheduler, user_data, wait_ops)
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

struct CoroReadvOperation final:
    private PlatformIovecStorage,
    public ReadvAwaitable,
    public CoroOperationBase<CoroReadvOperation> {
    CoroReadvOperation(IOController* controller,
                       Scheduler* scheduler,
                       void* user_data,
                       GalayCoreCoroWaitOps wait_ops,
                       std::unique_ptr<struct iovec[]> iovecs,
                       size_t count)
        : PlatformIovecStorage(std::move(iovecs), count)
        , ReadvAwaitable(controller, span())
        , CoroOperationBase(scheduler, user_data, wait_ops)
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

    C_IOResult buildResultImpl() noexcept
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(*m_result == 0 ? C_IOResultEof : C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

struct CoroWritevOperation final:
    private PlatformIovecStorage,
    public WritevAwaitable,
    public CoroOperationBase<CoroWritevOperation> {
    CoroWritevOperation(IOController* controller,
                        Scheduler* scheduler,
                        void* user_data,
                        GalayCoreCoroWaitOps wait_ops,
                        std::unique_ptr<struct iovec[]> iovecs,
                        size_t count)
        : PlatformIovecStorage(std::move(iovecs), count)
        , WritevAwaitable(controller, span())
        , CoroOperationBase(scheduler, user_data, wait_ops)
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

struct CoroSendFileOperation final:
    public SendFileAwaitable,
    public CoroOperationBase<CoroSendFileOperation> {
    CoroSendFileOperation(IOController* controller,
                          Scheduler* scheduler,
                          void* user_data,
                          GalayCoreCoroWaitOps wait_ops,
                          int file_fd,
                          off_t offset,
                          size_t count)
        : SendFileAwaitable(controller, file_fd, offset, count)
        , CoroOperationBase(scheduler, user_data, wait_ops)
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

int galay_core_coro_tcp_can_try_immediate_io(GalayCoreTcpSocket* socket_handle,
                                             GalayCoreIOScheduler* scheduler_handle)
{
    auto* socket = to_cpp_socket(socket_handle);
    auto* scheduler = reinterpret_cast<Scheduler*>(scheduler_handle);
    if (socket == nullptr || scheduler == nullptr) {
        return 0;
    }
    IOController* controller = socket->controller();
    const auto* owner = controller->m_owner_scheduler.load(std::memory_order_acquire);
    return controller->m_awaitable[IOController::READ] == nullptr &&
        controller->m_awaitable[IOController::WRITE] == nullptr &&
        controller->m_sequence_owner[IOController::READ] == nullptr &&
        controller->m_sequence_owner[IOController::WRITE] == nullptr &&
        (owner == nullptr || owner == scheduler)
        ? 1
        : 0;
}

GalayCoreCoroIOResult galay_core_coro_tcp_accept(GalayCoreTcpSocket* listener_socket,
                                                 GalayCoreIOScheduler* scheduler_handle,
                                                 GalayCoreTcpSocket** out_socket,
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
    return perform_registered_io<true, true>(socket->controller(),
                                             scheduler,
                                             ACCEPT,
                                             timeout_ms,
                                             operation,
                                             static_cast<AcceptAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_connect(GalayCoreTcpSocket* socket_handle,
                                                  GalayCoreIOScheduler* scheduler_handle,
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
    return perform_registered_io<true, true>(socket->controller(),
                                             scheduler,
                                             CONNECT,
                                             timeout_ms,
                                             operation,
                                             static_cast<ConnectAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_recv(GalayCoreTcpSocket* socket_handle,
                                               GalayCoreIOScheduler* scheduler_handle,
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
    return perform_registered_io<true, true>(socket->controller(),
                                             scheduler,
                                             RECV,
                                             timeout_ms,
                                             operation,
                                             static_cast<RecvAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_send(GalayCoreTcpSocket* socket_handle,
                                               GalayCoreIOScheduler* scheduler_handle,
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
    return perform_registered_io<true, true>(socket->controller(),
                                             scheduler,
                                             SEND,
                                             timeout_ms,
                                             operation,
                                             static_cast<SendAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_readv(GalayCoreTcpSocket* socket_handle,
                                                GalayCoreIOScheduler* scheduler_handle,
                                                const galay_iovec_t* iovecs,
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
    auto platform_iovecs = make_platform_iovecs(iovecs, count);
    if (!platform_iovecs) {
        return make_result(C_IOResultError, ENOMEM);
    }
    CoroReadvOperation operation(
        socket->controller(), scheduler, user_data, *wait_ops, std::move(platform_iovecs), count);
    return perform_registered_io<true, true>(socket->controller(),
                                             scheduler,
                                             READV,
                                             timeout_ms,
                                             operation,
                                             static_cast<ReadvAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_writev(GalayCoreTcpSocket* socket_handle,
                                                 GalayCoreIOScheduler* scheduler_handle,
                                                 const galay_iovec_t* iovecs,
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
    auto platform_iovecs = make_platform_iovecs(iovecs, count);
    if (!platform_iovecs) {
        return make_result(C_IOResultError, ENOMEM);
    }
    CoroWritevOperation operation(
        socket->controller(), scheduler, user_data, *wait_ops, std::move(platform_iovecs), count);
    return perform_registered_io<true, true>(socket->controller(),
                                             scheduler,
                                             WRITEV,
                                             timeout_ms,
                                             operation,
                                             static_cast<WritevAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_sendfile(GalayCoreTcpSocket* socket_handle,
                                                   GalayCoreIOScheduler* scheduler_handle,
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
    return perform_registered_io<true, true>(socket->controller(),
                                             scheduler,
                                             SENDFILE,
                                             timeout_ms,
                                             operation,
                                             static_cast<SendFileAwaitable*>(&operation));
}

GalayCoreCoroIOResult galay_core_coro_tcp_close(GalayCoreTcpSocket* socket_handle,
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
