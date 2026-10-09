#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/uring_reactor.h>

#include <atomic>
#include <cassert>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#ifdef USE_IOURING
using namespace galay::kernel;

namespace galay::kernel {
struct IOUringReactorTestAccess {
    static io_uring& ring(IOUringReactor& reactor) { return reactor.m_ring; }
    static void deliver(IOUringReactor& reactor, io_uring_cqe* cqe) {
        reactor.process_completion(cqe);
    }
};
}

struct RecvProbe final : SequenceAwaitableBase {
    RecvProbe(IOController* controller, char* buffer, size_t length)
        : SequenceAwaitableBase(controller, SequenceOwnerDomain::Read)
        , context(buffer, length)
        , task{nullptr, &context, RECV} {}
    IOTask* front() override { return &task; }
    const IOTask* front() const override { return &task; }
    void pop_front() override {}
    bool empty() const override { return false; }
    SequenceProgress prepare_for_submit() override { return SequenceProgress::kNeedWait; }
    SequenceProgress on_active_event(io_uring_cqe*, GHandle) override {
        assert(false && "expired sequence must not receive a completion");
        return SequenceProgress::kCompleted;
    }
    RecvIOContext context;
    IOTask task;
};
#endif

int main()
{
#ifdef USE_IOURING
    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    std::atomic<uint64_t> error{0};
    IOUringReactor reactor(32, error);
    assert(reactor.start());
    IOController controller(GHandle{sockets[0]});
    char buffer[32]{};
    RecvProbe probe(&controller, buffer, sizeof(buffer));
    assert(probe.claim_requested_domain());
    assert(reactor.add_sequence(&controller) == 0);
    auto& ring = IOUringReactorTestAccess::ring(reactor);
    assert(io_uring_submit(&ring) > 0);
    constexpr char payload[] = "timeout must not consume TLS ciphertext";
    assert(::send(sockets[1], payload, sizeof(payload), 0) == sizeof(payload));
    io_uring_cqe* cqe = nullptr;
    __kernel_timespec timeout{1, 0};
    assert(io_uring_wait_cqe_timeout(&ring, &cqe, &timeout) == 0);
    // The deadline wins before the owner dispatches an already queued CQE.
    probe.on_completed();
    IOUringReactorTestAccess::deliver(reactor, cqe);
    io_uring_cqe_seen(&ring, cqe);
    char received[sizeof(payload)]{};
    const auto count = ::recv(sockets[0], received, sizeof(received), MSG_DONTWAIT);
    assert(count == sizeof(payload));
    assert(std::memcmp(received, payload, sizeof(payload)) == 0);
    assert(::close(sockets[0]) == 0);
    assert(::close(sockets[1]) == 0);
#endif
    return 0;
}
