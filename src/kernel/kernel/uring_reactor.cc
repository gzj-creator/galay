/**
 * @file uring_reactor.cc
 * @brief Linux io_uring reactor 实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用 Linux io_uring 实现 IO 事件注册、multishot accept/recv
 * （配合 provided buffer ring）、send_zc 门控、sequence SQE 提交和 CQE 处理。
 */

#include "uring_reactor.h"

#ifdef USE_IOURING

#include "kernel/kernel/awaitable.h"

#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <expected>
#include <memory>
#include <stdexcept>
#include <vector>

namespace galay::kernel {

namespace {

constexpr int kImmediateReady = 1;

inline auto wakeToken() -> void* {
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

inline int waitForIoUringCompletion(struct io_uring* ring,
                                    struct io_uring_cqe** cqe,
                                    struct __kernel_timespec* timeout) {
    if (io_uring_sq_ready(ring) == 0) {
        return io_uring_wait_cqe_timeout(ring, cqe, timeout);
    }

    const int ret = io_uring_submit_and_wait_timeout(ring, cqe, 1, timeout, nullptr);
    if (ret == -EBUSY) {
        return io_uring_wait_cqe_timeout(ring, cqe, timeout);
    }
    return ret;
}

inline auto negativeRetOrErrno(int ret) -> uint32_t {
    return (ret < 0 && ret != -1)
        ? static_cast<uint32_t>(-ret)
        : static_cast<uint32_t>(errno);
}

inline auto systemCodeFromError(const IOError& error) -> uint32_t {
    return static_cast<uint32_t>(error.code() >> 32);
}

inline auto ioErrorCodeFromError(const IOError& error) -> IOErrorCode {
    return static_cast<IOErrorCode>(error.code() & 0xffffffffu);
}

inline bool resolveSequenceSlot(IOEventType type, IOController::Index& slot) {
    if (detail::sequenceEventUsesSlot(type, IOController::READ)) {
        slot = IOController::READ;
        return true;
    }
    if (detail::sequenceEventUsesSlot(type, IOController::WRITE)) {
        slot = IOController::WRITE;
        return true;
    }
    return false;
}

struct TokenRecycleGuard {
    SqeRequestToken* token = nullptr;

    ~TokenRecycleGuard() {
        if (token != nullptr) {
            token->recycle();
        }
    }
};

struct RecvBufferPool {
    RecvBufferPool(struct io_uring* target_ring,
                   uint16_t entries,
                   uint16_t bgid,
                   size_t buf_size)
        : ring(target_ring)
        , ring_entries(entries)
        , buffer_group(bgid)
        , buffer_size(buf_size)
        , mask(io_uring_buf_ring_mask(entries)) {
        int ret = 0;
        buf_ring = io_uring_setup_buf_ring(ring, ring_entries, buffer_group, 0, &ret);
        if (buf_ring == nullptr || ret < 0) {
            throw std::runtime_error("Failed to set up io_uring recv buffer ring");
        }

        io_uring_buf_ring_init(buf_ring);
        buffers.reserve(ring_entries);
        for (uint16_t bid = 0; bid < ring_entries; ++bid) {
            auto storage = std::make_unique<char[]>(buffer_size);
            io_uring_buf_ring_add(buf_ring,
                                  storage.get(),
                                  static_cast<unsigned>(buffer_size),
                                  bid,
                                  mask,
                                  bid);
            buffers.push_back(std::move(storage));
        }
        io_uring_buf_ring_advance(buf_ring, ring_entries);
    }

    ~RecvBufferPool() = default;

    char* data(uint16_t bid) const noexcept {
        if (bid >= buffers.size()) {
            return nullptr;
        }
        return buffers[bid].get();
    }

    void recycle(uint16_t bid) noexcept {
        if (!active || buf_ring == nullptr || bid >= buffers.size()) {
            return;
        }
        io_uring_buf_ring_add(buf_ring,
                              buffers[bid].get(),
                              static_cast<unsigned>(buffer_size),
                              bid,
                              mask,
                              0);
        io_uring_buf_ring_advance(buf_ring, 1);
    }

    void shutdown() noexcept {
        active = false;
        if (buf_ring != nullptr) {
            (void)io_uring_free_buf_ring(ring, buf_ring, ring_entries, buffer_group);
            buf_ring = nullptr;
        }
    }

    struct io_uring* ring = nullptr;
    struct io_uring_buf_ring* buf_ring = nullptr;
    uint16_t ring_entries = 0;
    uint16_t buffer_group = 0;
    size_t buffer_size = 0;
    int mask = 0;
    bool active = true;
    std::vector<std::unique_ptr<char[]>> buffers;
};

inline void recycleRecvBuffer(const std::shared_ptr<void>& owner, uint16_t bid) noexcept {
    if (!owner) {
        return;
    }
    static_cast<RecvBufferPool*>(owner.get())->recycle(bid);
}

inline auto recvBufferPool(const std::shared_ptr<void>& owner) -> RecvBufferPool* {
    return static_cast<RecvBufferPool*>(owner.get());
}

inline auto cqeBufferId(const struct io_uring_cqe* cqe) -> uint16_t {
    return static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
}

inline bool tryImmediateReadv(int fd, ReadvIOContext* ctx, int& res) {
    if (ctx == nullptr || ctx->m_iovecs.empty()) {
        res = 0;
        return true;
    }

    ssize_t n = 0;
    if (ctx->m_iovecs.size() == 1) {
        const auto& iov = ctx->m_iovecs[0];
        n = ::recv(fd, iov.iov_base, iov.iov_len, MSG_DONTWAIT);
    } else {
        msghdr msg = ctx->m_msg;
        n = ::recvmsg(fd, &msg, MSG_DONTWAIT);
    }

    if (n >= 0) {
        res = static_cast<int>(n);
        return true;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return false;
    }
    res = -errno;
    return true;
}

}  // namespace

IOUringReactor::IOUringReactor(int queue_depth, std::atomic<uint64_t>& last_error_code)
    : m_queue_depth(queue_depth)
    , m_event_fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    , m_last_error_code(last_error_code) {
    if (m_event_fd == -1) {
        throw std::runtime_error("Failed to create eventfd");
    }

    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_COOP_TASKRUN;
    params.sq_thread_idle = 1000;

    if (io_uring_queue_init_params(m_queue_depth, &m_ring, &params) < 0) {
        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_COOP_TASKRUN;
        if (io_uring_queue_init_params(m_queue_depth, &m_ring, &params) < 0) {
            std::memset(&params, 0, sizeof(params));
            if (io_uring_queue_init_params(m_queue_depth, &m_ring, &params) < 0) {
                close(m_event_fd);
                throw std::runtime_error("Failed to initialize io_uring");
            }
        }
    }

    m_recv_buffer_pool = std::static_pointer_cast<void>(
        std::make_shared<RecvBufferPool>(&m_ring,
                                         kRecvBufferCount,
                                         kRecvBufferGroup,
                                         kRecvBufferSize));

    if (io_uring_probe* probe = io_uring_get_probe_ring(&m_ring); probe != nullptr) {
        m_send_zc_supported = io_uring_opcode_supported(probe, IORING_OP_SEND_ZC) != 0;
        io_uring_free_probe(probe);
    }
}

IOUringReactor::~IOUringReactor() {
    if (m_recv_buffer_pool) {
        recvBufferPool(m_recv_buffer_pool)->shutdown();
    }
    io_uring_queue_exit(&m_ring);
    if (m_event_fd != -1) {
        close(m_event_fd);
    }
}

void IOUringReactor::notify() {
    uint64_t val = 1;
    if (write(m_event_fd, &val, sizeof(val)) < 0) {
        detail::storeBackendError(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
    }
}

int IOUringReactor::wakeReadFdForTest() const {
    return m_event_fd;
}

bool IOUringReactor::shouldUseSendZc(size_t length) const noexcept {
    return m_send_zc_supported && length >= kSendZcThreshold;
}

void IOUringReactor::prepareSendSqe(struct io_uring_sqe* sqe,
                                    SqeRequestToken* token,
                                    int fd,
                                    const void* buffer,
                                    size_t length,
                                    int flags) {
    if (token != nullptr) {
        token->notify_expected = false;
        token->notify_received = false;
        token->result_completed = false;
    }

    if (shouldUseSendZc(length)) {
        io_uring_prep_send_zc(sqe,
                              fd,
                              buffer,
                              length,
                              flags,
                              IORING_SEND_ZC_REPORT_USAGE);
        if (token != nullptr) {
            token->notify_expected = true;
        }
        return;
    }

    io_uring_prep_send(sqe, fd, buffer, length, flags);
}

int IOUringReactor::addAccept(IOController* controller) {
    auto* awaitable = controller->getAwaitable<AcceptAwaitable>();
    if (awaitable == nullptr) return -1;
    if (controller->tryConsumeAcceptedHandle(awaitable->m_host, awaitable->m_result)) {
        return kImmediateReady;
    }
    if (controller->m_accept_multishot_armed) {
        return 0;
    }
    return submitMultishotAccept(controller);
}

int IOUringReactor::submitMultishotAccept(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -EINVAL;
    }

    // 每次重新挂载 multishot 请求都切换一次 request epoch，
    // 让旧 request 的晚到 CQE 无法误命中新 token。
    controller->advanceSqeGeneration(IOController::READ);
    auto* token = controller->makeSqeRequest(IOController::READ);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    io_uring_prep_multishot_accept(sqe,
                                   controller->m_handle.fd,
                                   nullptr,
                                   nullptr,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data(sqe, token);
    token->persistent = true;
    controller->m_accept_multishot_token = token;
    controller->m_accept_multishot_armed = true;
    return 0;
}

int IOUringReactor::addConnect(IOController* controller) {
    auto* awaitable = controller->getAwaitable<ConnectAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::WRITE);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    io_uring_prep_connect(sqe,
                          controller->m_handle.fd,
                          awaitable->m_host.sockAddr(),
                          *awaitable->m_host.addrLen());
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addRecv(IOController* controller) {
    auto* awaitable = controller->getAwaitable<RecvAwaitable>();
    if (awaitable == nullptr) return -1;
    if (controller->tryConsumeReadyRecv(awaitable->m_buffer,
                                        awaitable->m_length,
                                        awaitable->m_result)) {
        return kImmediateReady;
    }
    if (controller->m_recv_multishot_armed) {
        return 0;
    }
    return submitMultishotRecv(controller);
}

int IOUringReactor::submitMultishotRecv(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -EINVAL;
    }
    if (!m_recv_buffer_pool) {
        return -EINVAL;
    }

    // awaitable 切换不应使持久 recv token 失效，但每次真正重挂
    // multishot recv SQE 时必须推进 request epoch 以隔离旧 CQE。
    controller->advanceSqeGeneration(IOController::READ);
    auto* token = controller->makeSqeRequest(IOController::READ);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    io_uring_prep_recv_multishot(sqe, controller->m_handle.fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = kRecvBufferGroup;
    io_uring_sqe_set_data(sqe, token);
    token->persistent = true;
    controller->m_recv_multishot_token = token;
    controller->m_recv_multishot_armed = true;
    return 0;
}

int IOUringReactor::addSend(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::WRITE);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    prepareSendSqe(sqe,
                   token,
                   controller->m_handle.fd,
                   awaitable->m_buffer,
                   awaitable->m_length,
                   0);
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addReadv(IOController* controller) {
    auto* awaitable = controller->getAwaitable<ReadvAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::READ);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    if (awaitable->m_iovecs.size() == 1) {
        const auto& iov = awaitable->m_iovecs[0];
        io_uring_prep_recv(sqe,
                           controller->m_handle.fd,
                           iov.iov_base,
                           static_cast<unsigned>(iov.iov_len),
                           0);
    } else {
        io_uring_prep_recvmsg(sqe, controller->m_handle.fd, &awaitable->m_msg, 0);
    }
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addWritev(IOController* controller) {
    auto* awaitable = controller->getAwaitable<WritevAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::WRITE);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    if (awaitable->m_iovecs.size() == 1) {
        const auto& iov = awaitable->m_iovecs[0];
        io_uring_prep_send(sqe,
                           controller->m_handle.fd,
                           iov.iov_base,
                           static_cast<unsigned>(iov.iov_len),
                           0);
    } else {
        io_uring_prep_sendmsg(sqe, controller->m_handle.fd, &awaitable->m_msg, 0);
    }
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addSendFile(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendFileAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::WRITE);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    io_uring_prep_poll_add(sqe, controller->m_handle.fd, POLLOUT);
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addClose(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }

    const int fd = controller->m_handle.fd;

    struct io_uring_sqe* cancel_sqe = io_uring_get_sqe(&m_ring);
    if (cancel_sqe) {
        io_uring_prep_cancel_fd(cancel_sqe, fd, 0);
        io_uring_sqe_set_data(cancel_sqe, nullptr);
    }

    struct io_uring_sqe* close_sqe = io_uring_get_sqe(&m_ring);
    if (!close_sqe) {
        close(fd);
    } else {
        io_uring_prep_close(close_sqe, fd);
        io_uring_sqe_set_data(close_sqe, nullptr);
    }

    controller->m_type = IOEventType::INVALID;
    controller->m_awaitable[IOController::READ] = nullptr;
    controller->m_awaitable[IOController::WRITE] = nullptr;
    controller->invalidateSqeRequests();
    controller->m_handle = GHandle::invalid();
    return 0;
}

int IOUringReactor::addFileRead(IOController* controller) {
    auto* awaitable = controller->getAwaitable<FileReadAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::READ);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    io_uring_prep_read(sqe,
                       controller->m_handle.fd,
                       awaitable->m_buffer,
                       awaitable->m_length,
                       awaitable->m_offset);
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addFileWrite(IOController* controller) {
    auto* awaitable = controller->getAwaitable<FileWriteAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::WRITE);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    io_uring_prep_write(sqe,
                        controller->m_handle.fd,
                        awaitable->m_buffer,
                        awaitable->m_length,
                        awaitable->m_offset);
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addRecvFrom(IOController* controller) {
    auto* awaitable = controller->getAwaitable<RecvFromAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::READ);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    std::memset(&awaitable->m_msg, 0, sizeof(awaitable->m_msg));
    std::memset(&awaitable->m_addr, 0, sizeof(awaitable->m_addr));

    awaitable->m_iov.iov_base = awaitable->m_buffer;
    awaitable->m_iov.iov_len = awaitable->m_length;
    awaitable->m_msg.msg_iov = &awaitable->m_iov;
    awaitable->m_msg.msg_iovlen = 1;
    awaitable->m_msg.msg_name = &awaitable->m_addr;
    awaitable->m_msg.msg_namelen = sizeof(awaitable->m_addr);

    io_uring_prep_recvmsg(sqe, controller->m_handle.fd, &awaitable->m_msg, 0);
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addSendTo(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendToAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::WRITE);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    std::memset(&awaitable->m_msg, 0, sizeof(awaitable->m_msg));
    awaitable->m_iov.iov_base = const_cast<char*>(awaitable->m_buffer);
    awaitable->m_iov.iov_len = awaitable->m_length;
    awaitable->m_msg.msg_iov = &awaitable->m_iov;
    awaitable->m_msg.msg_iovlen = 1;
    awaitable->m_msg.msg_name = const_cast<sockaddr*>(awaitable->m_to.sockAddr());
    awaitable->m_msg.msg_namelen = *awaitable->m_to.addrLen();

    io_uring_prep_sendmsg(sqe, controller->m_handle.fd, &awaitable->m_msg, 0);
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addFileWatch(IOController* controller) {
    auto* awaitable = controller->getAwaitable<FileWatchAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* token = controller->makeSqeRequest(IOController::READ);
    if (token == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    io_uring_prep_read(sqe,
                       controller->m_handle.fd,
                       awaitable->m_buffer,
                       awaitable->m_buffer_size,
                       0);
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::addSequence(IOController* controller) {
    if (controller == nullptr) {
        return -1;
    }

    const auto submit_owner = [this, controller](SequenceAwaitableBase* owner) -> int {
        if (owner == nullptr) {
            return 0;
        }

        auto* task = owner->front();
        if (task == nullptr) {
            return 0;
        }
        if (task->context == nullptr) {
            return -EINVAL;
        }

        const IOEventType type = owner->resolveTaskEventType(*task);
        IOController::Index slot = IOController::READ;
        if (!resolveSequenceSlot(type, slot)) {
            return -EINVAL;
        }
        if (controller->m_awaitable[slot] == owner) {
            return 0;
        }
        if (controller->m_awaitable[slot] != nullptr) {
            return -EBUSY;
        }
        const int ret = submitSequenceSqe(slot, type, task->context, controller, owner);
        if (ret == kImmediateReady) {
            owner->m_waker.wakeUp();
            return 0;
        }
        return ret;
    };

    auto* read_owner = controller->m_sequence_owner[IOController::READ];
    if (const int ret = submit_owner(read_owner); ret != 0) {
        return ret;
    }

    auto* write_owner = controller->m_sequence_owner[IOController::WRITE];
    if (write_owner != read_owner) {
        if (const int ret = submit_owner(write_owner); ret != 0) {
            return ret;
        }
    }

    return 0;
}

int IOUringReactor::submitSequenceSqe(IOController::Index slot,
                                      IOEventType type,
                                      IOContextBase* ctx,
                                      IOController* controller,
                                      SequenceAwaitableBase* owner) {
    if (type == READV) {
        // Sequence READV is readiness-driven here; completion is produced by a
        // nonblocking socket read so staged sequence progress cannot miss ready bytes.
        auto* readv_ctx = static_cast<ReadvIOContext*>(ctx);
        int immediate_res = 0;
        if (tryImmediateReadv(controller->m_handle.fd, readv_ctx, immediate_res)) {
            io_uring_cqe ready_cqe{};
            ready_cqe.res = immediate_res;
            const auto progress = owner->onActiveEvent(&ready_cqe, controller->m_handle);
            if (progress == SequenceProgress::kCompleted) {
                return kImmediateReady;
            }
            return addSequence(controller);
        }
    }

    auto* token = controller->makeSqeRequest(slot);
    if (token == nullptr) {
        return -ENOMEM;
    }

    auto* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        token->recycle();
        return -EAGAIN;
    }

    switch (type) {
    case RECV: {
        auto* c = static_cast<RecvIOContext*>(ctx);
        io_uring_prep_recv(sqe, controller->m_handle.fd, c->m_buffer, c->m_length, 0);
        break;
    }
    case SEND: {
        auto* c = static_cast<SendIOContext*>(ctx);
        prepareSendSqe(sqe, token, controller->m_handle.fd, c->m_buffer, c->m_length, 0);
        break;
    }
    case ACCEPT: {
        auto* c = static_cast<AcceptIOContext*>(ctx);
        io_uring_prep_accept(sqe,
                             controller->m_handle.fd,
                             c->m_host->sockAddr(),
                             c->m_host->addrLen(),
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        break;
    }
    case CONNECT: {
        auto* c = static_cast<ConnectIOContext*>(ctx);
        io_uring_prep_connect(sqe,
                              controller->m_handle.fd,
                              c->m_host.sockAddr(),
                              *c->m_host.addrLen());
        break;
    }
    case READV: {
        io_uring_prep_poll_add(sqe, controller->m_handle.fd, POLLIN);
        break;
    }
    case WRITEV: {
        auto* c = static_cast<WritevIOContext*>(ctx);
        if (c->m_iovecs.size() == 1) {
            const auto& iov = c->m_iovecs[0];
            io_uring_prep_send(sqe,
                               controller->m_handle.fd,
                               iov.iov_base,
                               static_cast<unsigned>(iov.iov_len),
                               0);
        } else {
            io_uring_prep_sendmsg(sqe, controller->m_handle.fd, &c->m_msg, 0);
        }
        break;
    }
    case FILEREAD: {
        auto* c = static_cast<FileReadIOContext*>(ctx);
        io_uring_prep_read(sqe, controller->m_handle.fd, c->m_buffer, c->m_length, c->m_offset);
        break;
    }
    case FILEWRITE: {
        auto* c = static_cast<FileWriteIOContext*>(ctx);
        io_uring_prep_write(sqe, controller->m_handle.fd, c->m_buffer, c->m_length, c->m_offset);
        break;
    }
    case RECVFROM: {
        auto* c = static_cast<RecvFromIOContext*>(ctx);
        std::memset(&c->m_msg, 0, sizeof(c->m_msg));
        std::memset(&c->m_addr, 0, sizeof(c->m_addr));
        c->m_iov.iov_base = c->m_buffer;
        c->m_iov.iov_len = c->m_length;
        c->m_msg.msg_iov = &c->m_iov;
        c->m_msg.msg_iovlen = 1;
        c->m_msg.msg_name = &c->m_addr;
        c->m_msg.msg_namelen = sizeof(c->m_addr);
        io_uring_prep_recvmsg(sqe, controller->m_handle.fd, &c->m_msg, 0);
        break;
    }
    case SENDTO: {
        auto* c = static_cast<SendToIOContext*>(ctx);
        std::memset(&c->m_msg, 0, sizeof(c->m_msg));
        c->m_iov.iov_base = const_cast<char*>(c->m_buffer);
        c->m_iov.iov_len = c->m_length;
        c->m_msg.msg_iov = &c->m_iov;
        c->m_msg.msg_iovlen = 1;
        c->m_msg.msg_name = const_cast<sockaddr*>(c->m_to.sockAddr());
        c->m_msg.msg_namelen = *c->m_to.addrLen();
        io_uring_prep_sendmsg(sqe, controller->m_handle.fd, &c->m_msg, 0);
        break;
    }
    case SENDFILE:
        io_uring_prep_poll_add(sqe, controller->m_handle.fd, POLLOUT);
        break;
    case FILEWATCH: {
        auto* c = static_cast<FileWatchIOContext*>(ctx);
        io_uring_prep_read(sqe, controller->m_handle.fd, c->m_buffer, c->m_buffer_size, 0);
        break;
    }
    default:
        token->recycle();
        return -EINVAL;
    }

    owner->m_sqe_type = SEQUENCE;
    controller->m_awaitable[slot] = owner;
    io_uring_sqe_set_data(sqe, token);
    return 0;
}

int IOUringReactor::remove(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        return -EAGAIN;
    }

    io_uring_prep_cancel_fd(sqe, controller->m_handle.fd, 0);
    io_uring_sqe_set_data(sqe, nullptr);
    return 0;
}

void IOUringReactor::ensureWakeReadArmed() {
    if (m_wake_read_armed) {
        return;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        return;
    }

    io_uring_prep_read(sqe, m_event_fd, &m_eventfd_buf, sizeof(m_eventfd_buf), 0);
    io_uring_sqe_set_data(sqe, wakeToken());
    m_wake_read_armed = true;
}

void IOUringReactor::poll(uint64_t timeout_ns, WakeCoordinator& wake_coordinator) {
    ensureWakeReadArmed();

    struct io_uring_cqe* cqe = nullptr;
    struct __kernel_timespec timeout;
    timeout.tv_sec = static_cast<__kernel_time64_t>(timeout_ns / 1000000000ULL);
    timeout.tv_nsec = timeout_ns % 1000000000ULL;

    const int ret = waitForIoUringCompletion(&m_ring, &cqe, &timeout);
    if (ret < 0) {
        if (ret == -EINTR || ret == -ETIME) {
            return;
        }
        detail::storeBackendError(
            m_last_error_code, kNotReady, static_cast<uint32_t>(-ret));
        return;
    }

    unsigned head = 0;
    unsigned count = 0;
    bool wake_triggered = false;

    io_uring_for_each_cqe(&m_ring, head, cqe) {
        void* user_data = io_uring_cqe_get_data(cqe);
        if (user_data == wakeToken()) {
            wake_triggered = true;
            m_wake_read_armed = false;
        } else if (user_data != nullptr) {
            processCompletion(cqe);
        }
        ++count;
    }

    if (count > 0) {
        io_uring_cq_advance(&m_ring, count);
    }

    if (wake_triggered) {
        wake_coordinator.cancelPendingWake();
        ensureWakeReadArmed();
    }
}

void IOUringReactor::processCompletion(struct io_uring_cqe* cqe) {
    void* data = io_uring_cqe_get_data(cqe);
    if (!data) {
        return;
    }

    auto* token = static_cast<SqeRequestToken*>(data);
    TokenRecycleGuard recycle_guard{token};
    const bool notification = (cqe->flags & IORING_CQE_F_NOTIF) != 0;
    const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    if (notification) {
        token->notify_received = true;
        if (!token->result_completed) {
            recycle_guard.token = nullptr;
        }
    } else if (token->persistent) {
        if (more) {
            recycle_guard.token = nullptr;
        } else {
            token->persistent = false;
        }
    } else if (token->notify_expected) {
        token->result_completed = true;
        if (!token->notify_received) {
            recycle_guard.token = nullptr;
        }
    }
    if (!token->state) {
        return;
    }
    if (token->state->generation.load(std::memory_order_acquire) != token->generation) {
        return;
    }

    auto* controller = token->state->owner.load(std::memory_order_acquire);
    if (!controller) {
        return;
    }

    if (notification) {
        return;
    }

    const auto slot = static_cast<IOController::Index>(token->state->slot);
    auto* base = static_cast<AwaitableBase*>(controller->m_awaitable[slot]);
    if (!base) {
        if (slot == IOController::READ) {
            if (controller->m_recv_multishot_armed) {
                processRecvCompletion(controller, nullptr, cqe);
            } else if (controller->m_accept_multishot_armed) {
                processAcceptCompletion(controller, nullptr, cqe);
            }
        }
        return;
    }

    switch (base->m_sqe_type) {
    case ACCEPT: {
        auto* awaitable = static_cast<AcceptAwaitable*>(base);
        processAcceptCompletion(controller, awaitable, cqe);
        break;
    }
    case CONNECT: {
        auto* awaitable = static_cast<ConnectAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addConnect(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kConnectFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case RECV: {
        auto* awaitable = static_cast<RecvAwaitable*>(base);
        processRecvCompletion(controller, awaitable, cqe);
        break;
    }
    case SEND: {
        auto* awaitable = static_cast<SendAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addSend(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kSendFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case READV: {
        auto* awaitable = static_cast<ReadvAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addReadv(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kRecvFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case WRITEV: {
        auto* awaitable = static_cast<WritevAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addWritev(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kSendFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case FILEREAD: {
        auto* awaitable = static_cast<FileReadAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addFileRead(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kReadFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case FILEWRITE: {
        auto* awaitable = static_cast<FileWriteAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addFileWrite(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kWriteFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case RECVFROM: {
        auto* awaitable = static_cast<RecvFromAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addRecvFrom(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kRecvFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case SENDTO: {
        auto* awaitable = static_cast<SendToAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addSendTo(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kSendFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case FILEWATCH: {
        auto* awaitable = static_cast<FileWatchAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addFileWatch(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kReadFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case SENDFILE: {
        auto* awaitable = static_cast<SendFileAwaitable*>(base);
        if (awaitable->handleComplete(cqe, controller->m_handle)) {
            awaitable->m_waker.wakeUp();
        } else {
            const int ret = addSendFile(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kSendFailed, negativeRetOrErrno(ret)));
                awaitable->m_waker.wakeUp();
            }
        }
        break;
    }
    case SEQUENCE: {
        auto* sequence = static_cast<SequenceAwaitableBase*>(base);
        const auto event_type = sequence->activeEventType();
        controller->m_awaitable[slot] = nullptr;
        controller->advanceSqeGeneration(slot);

        SequenceProgress progress = SequenceProgress::kNeedWait;
        if (slot == IOController::READ && event_type == READV) {
            io_uring_cqe ready_cqe = *cqe;
            bool deliver = cqe->res < 0;
            if (cqe->res >= 0) {
                auto* task = sequence->front();
                auto* readv_ctx = task != nullptr
                    ? static_cast<ReadvIOContext*>(task->context)
                    : nullptr;
                int immediate_res = 0;
                if (tryImmediateReadv(controller->m_handle.fd, readv_ctx, immediate_res)) {
                    ready_cqe.res = immediate_res;
                    deliver = true;
                }
            }
            if (deliver) {
                progress = sequence->onActiveEvent(&ready_cqe, controller->m_handle);
            }
        } else {
            progress = sequence->onActiveEvent(cqe, controller->m_handle);
        }
        if (progress == SequenceProgress::kCompleted) {
            sequence->m_waker.wakeUp();
        } else {
            const int ret = addSequence(controller);
            if (ret < 0) {
                detail::storeBackendError(
                    m_last_error_code, kNotReady, negativeRetOrErrno(ret));
                sequence->m_waker.wakeUp();
            }
        }
        break;
    }
    default:
        break;
    }
}

void IOUringReactor::processAcceptCompletion(IOController* controller,
                                             AcceptAwaitable* awaitable,
                                             struct io_uring_cqe* cqe) {
    if (controller == nullptr) {
        return;
    }

    const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    auto result = io::handleAccept(cqe);

    if (result) {
        controller->enqueueAcceptedHandle(*result);
        if (awaitable != nullptr && !controller->m_accept_result_assigned &&
            controller->tryConsumeAcceptedHandle(awaitable->m_host, awaitable->m_result)) {
            controller->m_accept_result_assigned = true;
            awaitable->m_waker.wakeUp();
        }
    } else if (!IOError::contains(result.error().code(), kNotReady)) {
        if (awaitable != nullptr && !controller->m_accept_result_assigned) {
            awaitable->m_result = std::unexpected(result.error());
            controller->m_accept_result_assigned = true;
            awaitable->m_waker.wakeUp();
        } else {
            detail::storeBackendError(
                m_last_error_code,
                ioErrorCodeFromError(result.error()),
                systemCodeFromError(result.error()));
        }
    }

    if (more) {
        return;
    }

    controller->m_accept_multishot_token = nullptr;
    controller->m_accept_multishot_armed = false;
    if (controller->m_handle == GHandle::invalid()) {
        return;
    }

    const int ret = submitMultishotAccept(controller);
    if (ret >= 0) {
        return;
    }

    if (awaitable != nullptr && !controller->m_accept_result_assigned) {
        awaitable->m_result =
            std::unexpected(IOError(kAcceptFailed, negativeRetOrErrno(ret)));
        controller->m_accept_result_assigned = true;
        awaitable->m_waker.wakeUp();
        return;
    }

    detail::storeBackendError(
        m_last_error_code, kAcceptFailed, negativeRetOrErrno(ret));
}

void IOUringReactor::processRecvCompletion(IOController* controller,
                                           RecvAwaitable* awaitable,
                                           struct io_uring_cqe* cqe) {
    if (controller == nullptr) {
        return;
    }

    const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    const bool buffer_exhausted = (-cqe->res == ENOBUFS);
    const bool transient =
        (-cqe->res == EAGAIN || -cqe->res == EWOULDBLOCK || -cqe->res == EINTR);

    if (cqe->res > 0) {
        if ((cqe->flags & IORING_CQE_F_BUFFER) == 0) {
            ReadyRecvChunk chunk;
            chunk.kind = ReadyRecvChunk::Kind::Error;
            chunk.result = std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(EINVAL)));
            controller->enqueueReadyRecv(std::move(chunk));
        } else {
            const uint16_t bid = cqeBufferId(cqe);
            auto* pool = recvBufferPool(m_recv_buffer_pool);
            ReadyRecvChunk chunk;
            chunk.owner = m_recv_buffer_pool;
            chunk.data = pool != nullptr ? pool->data(bid) : nullptr;
            chunk.bid = bid;
            chunk.length = static_cast<size_t>(cqe->res);
            chunk.kind = ReadyRecvChunk::Kind::Buffer;
            chunk.recycle = recycleRecvBuffer;
            if (chunk.data == nullptr) {
                chunk.release();
                chunk.kind = ReadyRecvChunk::Kind::Error;
                chunk.result = std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(EINVAL)));
            }
            controller->enqueueReadyRecv(std::move(chunk));
        }
    } else if (cqe->res == 0) {
        ReadyRecvChunk chunk;
        chunk.kind = ReadyRecvChunk::Kind::Eof;
        chunk.result = static_cast<size_t>(0);
        controller->enqueueReadyRecv(std::move(chunk));
    } else if (!transient && !buffer_exhausted) {
        ReadyRecvChunk chunk;
        chunk.kind = ReadyRecvChunk::Kind::Error;
        chunk.result = std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(-cqe->res)));
        controller->enqueueReadyRecv(std::move(chunk));
    }

    if (awaitable != nullptr && !controller->m_recv_result_assigned) {
        bool should_deliver = false;
        if (buffer_exhausted) {
            should_deliver = controller->tryConsumeReadyRecv(awaitable->m_buffer,
                                                             awaitable->m_length,
                                                             awaitable->m_result);
        } else if (awaitable->handleComplete(cqe, controller->m_handle)) {
            should_deliver = controller->tryConsumeReadyRecv(awaitable->m_buffer,
                                                             awaitable->m_length,
                                                             awaitable->m_result);
        }

        if (should_deliver) {
            controller->m_recv_result_assigned = true;
            awaitable->m_waker.wakeUp();
        }
    }

    if (more) {
        return;
    }

    controller->m_recv_multishot_token = nullptr;
    controller->m_recv_multishot_armed = false;
}

}  // namespace galay::kernel

#endif  // USE_IOURING
