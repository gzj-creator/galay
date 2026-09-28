/** @file t190_iou_accept_completion_resource.cc
 *  @brief 确定性 accept CQE 资源测试：有效交付、过期结果、terminal 与 arena 回收。
 *  确定性用例只准备 SQE 并注入 CQE；realKernelClose 另外收集真实内核 CQE，
 *  再重排交付顺序。单次操作经 submitAccept/await_resume 接入 typed completion；
 *  完成竞争/frame 边界见 T191；本测试不证明 Runtime 全量 drain。
 */
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/uring_reactor.h>

#include <atomic>
#include <cerrno>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#ifdef USE_IOURING
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace galay::kernel;

namespace galay::kernel {
struct IOUringReactorTestAccess {
    struct Completion {
        SqeRequestHandle* handle;
        int result;
        unsigned flags;
    };
    static void deliver(IOUringReactor& reactor, SqeRequestHandle* handle,
                        int result, unsigned flags) {
        io_uring_cqe cqe{};
        cqe.user_data = reinterpret_cast<uintptr_t>(handle);
        cqe.res = result;
        cqe.flags = flags;
        reactor.processCompletion(&cqe);
    }
    // Bounded synchronous test driver only; no coroutine/executor waits here.
    static std::expected<Completion, int> takeKernelCompletion(IOUringReactor& reactor) {
        io_uring_cqe* cqe = nullptr;
        __kernel_timespec deadline{.tv_sec = 1, .tv_nsec = 0};
        const int ret = io_uring_submit_and_wait_timeout(&reactor.m_ring, &cqe, 1, &deadline, nullptr);
        if (ret < 0) { return std::unexpected(ret); }
        if (cqe == nullptr) { return std::unexpected(-EIO); }
        Completion result{static_cast<SqeRequestHandle*>(io_uring_cqe_get_data(cqe)),
                          cqe->res, cqe->flags};
        io_uring_cqe_seen(&reactor.m_ring, cqe);
        return result;
    }
};
}

namespace {
unsigned failures = 0;

bool check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "[T190] FAIL " << message << '\n';
    }
    return condition;
}

class TestFd final {
public:
    explicit TestFd(int fd = -1) noexcept : m_fd(fd) {}
    TestFd(TestFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    TestFd& operator=(TestFd&&) = delete;
    ~TestFd() {
        if (m_fd >= 0) {
            check(::close(m_fd) == 0, "test descriptor cleanup");
        }
    }
    int get() const noexcept { return m_fd; }
    int release() noexcept { return std::exchange(m_fd, -1); }
private:
    TestFd(const TestFd&) = delete;
    TestFd& operator=(const TestFd&) = delete;
    int m_fd;
};

struct Probe {
    detail::ResumeTokenHeader header;
    void* context = nullptr;
    void (*on_wake)(void*) noexcept = nullptr;
    unsigned wakes = 0;
};

const detail::ResumeTokenHooks kHooks{
    .owner_scheduler = [](void*) noexcept -> Scheduler* { return nullptr; },
    .request_resume = [](void* state) noexcept {
        auto& probe = *static_cast<Probe*>(state);
        ++probe.wakes;
        if (probe.on_wake != nullptr) { probe.on_wake(probe.context); }
        return true;
    },
};

bool bind(IOUringReactor& reactor, AcceptAwaitable& awaitable, Probe& probe) {
    probe.header.hooks = &kHooks;
    return reactor.submitAccept(awaitable,
        Waker(detail::ResumeToken::fromNonOwningCCoroutine(&probe)));
}

bool isClosed(int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

// On a red run the driver owns cleanup of leaked descriptors, so subsequent
// scenarios can run and fd leaks are diagnosed independently of LSan.
void expectClosed(int fd) {
    if (!check(isClosed(fd), "late accept fd must be closed")) {
        check(::close(fd) == 0, "red-run leaked fd cleanup");
    }
}

// Migration red test: a logical success must release frame entry points and
// own an unconsumed descriptor independently of the persistent resource.
void logicalCompletionBoundary(bool release_controller) {
    std::atomic<uint64_t> error{0};
    TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    IOUringReactor reactor(32, error);
    if (!check(listener.get() >= 0 && reactor.start().has_value(), "boundary setup")) { return; }
    auto controller = std::make_unique<IOController>(GHandle{listener.get()});
    Probe probe{};
    auto awaitable = std::make_unique<AcceptAwaitable>(controller.get(), nullptr);
    if (!check(bind(reactor, *awaitable, probe), "boundary prepare")) { return; }
    auto* handle = controller->m_accept_multishot_handle;
    const int accepted = ::dup(listener.get());
    if (!check(accepted >= 0, "boundary fd")) { return; }
    IOUringReactorTestAccess::deliver(reactor, handle, accepted, IORING_CQE_F_MORE);
    check(probe.wakes == 1, "logical success resumes before original terminal");
    check(controller->m_awaitable[IOController::READ] == nullptr,
          "logical success detaches controller slot before resume");
    check(awaitable->m_controller == nullptr, "logical success ends controller borrow");
    check(handle->arena && handle->persistent, "logical success retains persistent SQE");
    if (release_controller) {
        controller.reset();
        auto result = awaitable->await_resume();
        check(result && result->fd == accepted, "await_resume after controller destruction");
        if (result) { check(::close(result->fd) == 0, "consumed result cleanup"); }
    } else {
        awaitable.reset();
        expectClosed(accepted);
        controller.reset();
    }
    IOUringReactorTestAccess::deliver(reactor, handle, -ECANCELED, 0);
    check(!handle->arena && !handle->state, "boundary original terminal reclaims request");
}

void liveDelivery() {
    std::atomic<uint64_t> error{0};
    TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    IOUringReactor reactor(32, error);
    if (!check(listener.get() >= 0 && reactor.start().has_value(), "live setup")) { return; }
    IOController controller(GHandle{listener.get()});
    Probe probe{};
    AcceptAwaitable awaitable(&controller, nullptr);
    if (!check(bind(reactor, awaitable, probe), "prepare accept SQE")) { return; }
    auto* handle = controller.m_accept_multishot_handle;
    TestFd first(::dup(listener.get()));
    TestFd second(::dup(listener.get()));
    if (!check(first.get() >= 0 && second.get() >= 0, "live fd setup")) { return; }
    IOUringReactorTestAccess::deliver(reactor, handle, first.get(), IORING_CQE_F_MORE);
    check(probe.wakes == 1, "first success wakes once");
    check(awaitable.m_operation->state().completionReason() == CompletionReason::kReady, "first success delivered");
    check(handle->arena && handle->persistent, "MORE retains request");
    const int queued = second.release();
    IOUringReactorTestAccess::deliver(reactor, handle, queued, IORING_CQE_F_MORE);
    const auto result = awaitable.await_resume();
    check(probe.wakes == 1 && result && result->fd == first.get(),
          "second success preserves first result and wake count");
    Probe next_probe{};
    AcceptAwaitable next(&controller, nullptr);
    check(!bind(reactor, next, next_probe), "cached accept completes synchronously");
    const auto cached = next.await_resume();
    check(cached && cached->fd == queued && next_probe.wakes == 0,
          "second success is consumed by next typed operation");
    TestFd consumed(queued);
    // Invalidate before terminal so the synthetic stream does not auto-rearm.
    controller.advanceSqeGeneration(IOController::READ);
    IOUringReactorTestAccess::deliver(reactor, handle, -ECANCELED, 0);
    check(!handle->arena && !handle->state, "terminal recycles request");
    check(probe.wakes == 1 && error.load() == 0, "terminal does not wake again");
}

enum class Invalidation { Generation, OwnerNull, Destroyed };

void lateSuccess(Invalidation invalidation, bool terminal_success) {
    const auto before = failures;
    std::weak_ptr<SqeHandleArena> lifetime;
    {
        std::atomic<uint64_t> error{0};
        TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        IOUringReactor reactor(32, error);
        if (!check(listener.get() >= 0 && reactor.start().has_value(), "late setup")) { return; }
        auto controller = std::make_unique<IOController>(GHandle{listener.get()});
        Probe probe{};
        AcceptAwaitable awaitable(controller.get(), nullptr);
        if (!check(bind(reactor, awaitable, probe), "prepare late request")) { return; }
        auto* handle = controller->m_accept_multishot_handle;
        lifetime = handle->arena;
        if (invalidation == Invalidation::Generation) {
            controller->advanceSqeGeneration(IOController::READ);
        } else if (invalidation == Invalidation::OwnerNull) {
            // Isolate the owner-null branch without also changing generation.
            handle->state->owner.store(nullptr, std::memory_order_release);
        } else {
            controller.reset();
        }
        for (unsigned i = 0; i != 3; ++i) {
            const int accepted = ::dup(listener.get());
            if (!check(accepted >= 0, "late fd setup")) { break; }
            IOUringReactorTestAccess::deliver(reactor, handle, accepted, IORING_CQE_F_MORE);
            expectClosed(accepted);
            check(handle->arena && handle->persistent, "late MORE retains physical request");
        }
        const int terminal = terminal_success ? ::dup(listener.get()) : -ECANCELED;
        if (terminal_success && !check(terminal >= 0, "terminal fd setup")) { return; }
        IOUringReactorTestAccess::deliver(reactor, handle, terminal, 0);
        if (terminal_success) { expectClosed(terminal); }
        check(!handle->arena && !handle->state && !handle->persistent, "late terminal recycles once");
        check(handle->multishot_type == IOEventType::INVALID, "recycle clears result identity");
        check(probe.wakes == 0 && !awaitable.m_operation->state().completionReason(),
              "stale CQEs do not touch awaitable");
        check(error.load() == 0, "late cleanup has no backend error");
    }
    check(lifetime.expired(), "terminal releases arena self-reference");
    std::cout << "T190 late invalidation=" << static_cast<unsigned>(invalidation)
              << " terminal_success=" << terminal_success
              << " failures=" << failures - before << '\n';
}

void staleRecvIsNotDescriptor() {
    std::atomic<uint64_t> error{0};
    IOUringReactor reactor(32, error);
    TestFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!check(socket.get() >= 0 && reactor.start().has_value(), "recv setup")) { return; }
    IOController controller(GHandle{socket.get()});
    char buffer[64]{};
    RecvAwaitable awaitable(&controller, buffer, sizeof(buffer));
    if (!check(controller.fillAwaitable(RECV, &awaitable) && reactor.addRecv(&controller) == 0,
               "prepare persistent recv")) { return; }
    auto* handle = controller.m_recv_multishot_handle;
    check(handle->multishot_type == RECV, "recv identity survives owner invalidation");
    controller.advanceSqeGeneration(IOController::READ);
    IOUringReactorTestAccess::deliver(reactor, handle, socket.get(), IORING_CQE_F_MORE);
    check(::fcntl(socket.get(), F_GETFD) >= 0, "recv byte count must not close matching fd");
    IOUringReactorTestAccess::deliver(reactor, handle, -ECANCELED, 0);
    check(!handle->arena && handle->multishot_type == IOEventType::INVALID,
          "recv terminal clears identity");
    auto* reused = controller.makeSqeRequest(IOController::READ);
    if (check(reused != nullptr, "reacquire recycled request")) {
        check(reused == handle && reused->multishot_type == IOEventType::INVALID,
              "recycled request has no stale identity");
        reused->recycle();
    }
    check(error.load() == 0, "stale recv leaves backend error unchanged");
}

void closeOrdering(bool ready_first) {
    std::atomic<uint64_t> error{0};
    TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    IOUringReactor reactor(32, error);
    if (!check(listener.get() >= 0 && reactor.start().has_value(), "close setup")) { return; }
    IOController controller(GHandle{listener.get()});
    Probe probe{};
    AcceptAwaitable awaitable(&controller, nullptr);
    if (!check(bind(reactor, awaitable, probe), "prepare close request")) { return; }
    auto* handle = controller.m_accept_multishot_handle;
    TestFd first(::dup(listener.get()));
    if (!check(first.get() >= 0, "close first fd setup")) { return; }
    if (ready_first) {
        IOUringReactorTestAccess::deliver(reactor, handle, first.get(), IORING_CQE_F_MORE);
    }
    check(reactor.addClose(&controller) == 0, "logical close");
    check(reactor.addClose(&controller) == 0, "duplicate close");
    // No SQEs are submitted in this test: listener remains driver-owned.
    const int late = ::dup(listener.get());
    if (!check(late >= 0, "close late fd setup")) { return; }
    IOUringReactorTestAccess::deliver(reactor, handle, late, IORING_CQE_F_MORE);
    expectClosed(late);
    // cancel acknowledgement has no original request identity; it must not
    // release that request before its own terminal CQE arrives.
    IOUringReactorTestAccess::deliver(reactor, nullptr, 0, 0);
    check(handle->arena && handle->persistent, "cancel ack does not release original request");
    IOUringReactorTestAccess::deliver(reactor, handle, -ECANCELED, 0);
    IOUringReactorTestAccess::deliver(reactor, nullptr, -ENOENT, 0);
    check(probe.wakes == 1, "ready/close selects exactly one wake");
    const auto result = awaitable.await_resume();
    if (ready_first) {
        check(result && result->fd == first.get(),
              "close cannot overwrite selected success");
    } else {
        check(!result && IOError::contains(result.error().code(), kClosed),
              "late success cannot overwrite selected close");
    }
    check(!handle->arena && error.load() == 0, "close terminal recycles request without error");
}

void inlineOwnerDestruction(bool more) {
    std::atomic<uint64_t> error{0};
    TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    IOUringReactor reactor(32, error);
    if (!check(listener.get() >= 0 && reactor.start().has_value(), "inline setup")) { return; }
    auto controller = std::make_unique<IOController>(GHandle{listener.get()});
    Probe probe{};
    auto awaitable = std::make_unique<AcceptAwaitable>(controller.get(), nullptr);
    if (!check(bind(reactor, *awaitable, probe), "prepare inline request")) { return; }
    auto* handle = controller->m_accept_multishot_handle;
    TestFd accepted(::dup(listener.get()));
    if (!check(accepted.get() >= 0, "inline fd setup")) { return; }
    struct Owners {
        std::unique_ptr<IOController>& controller;
        std::unique_ptr<AcceptAwaitable>& awaitable;
        int delivered = -1;
    } owners{controller, awaitable};
    probe.context = &owners;
    probe.on_wake = [](void* context) noexcept {
        auto& owners = *static_cast<Owners*>(context);
        const auto result = owners.awaitable->await_resume();
        if (result) { owners.delivered = result->fd; }
        owners.awaitable.reset();
        owners.controller.reset();
    };
    // A successful final CQE must finish its resource bookkeeping before the
    // C resume hook can destroy the listener's controller inline.
    IOUringReactorTestAccess::deliver(reactor, handle, accepted.get(), more ? IORING_CQE_F_MORE : 0);
    check(!controller && !awaitable && probe.wakes == 1, "inline wake releases both owners once");
    check(owners.delivered == accepted.get(), "inline release preserves transferred fd");
    if (more) {
        check(handle->arena && handle->persistent, "inline MORE preserves physical lifetime");
        IOUringReactorTestAccess::deliver(reactor, handle, -ECANCELED, 0);
    }
    check(!handle->arena && error.load() == 0, "inline terminal recycles old request");
}

void realKernelClose(bool acknowledgements_first) {
    const auto before = failures;
    std::weak_ptr<SqeHandleArena> lifetime;
    {
        std::atomic<uint64_t> error{0};
        TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        TestFd client(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        IOUringReactor reactor(32, error);
        if (!check(listener.get() >= 0 && client.get() >= 0 && reactor.start().has_value(),
                   "real kernel setup")) { return; }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (!check(::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), length) == 0 &&
                   ::listen(listener.get(), 8) == 0 &&
                   ::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0,
                   "real listener setup")) { return; }
        auto controller = std::make_unique<IOController>(GHandle{listener.get()});
        Probe probe{};
        auto awaitable = std::make_unique<AcceptAwaitable>(controller.get(), nullptr);
        if (!check(bind(reactor, *awaitable, probe), "real prepare accept")) { return; }
        auto* handle = controller->m_accept_multishot_handle;
        lifetime = handle->arena;
        const int connected = ::connect(client.get(), reinterpret_cast<sockaddr*>(&address), length);
        if (!check(connected == 0 || errno == EINPROGRESS, "real connect")) { return; }
        auto ready = IOUringReactorTestAccess::takeKernelCompletion(reactor);
        if (!check(ready && ready->handle == handle && ready->result >= 0 &&
                   (ready->flags & IORING_CQE_F_MORE) != 0, "real success CQE with MORE")) {
            if (ready && ready->handle == handle && ready->result >= 0) {
                check(::close(ready->result) == 0, "unexpected ready cleanup");
            }
            return;
        }
        TestFd accepted(ready->result);
        check(reactor.addClose(controller.get()) == 0, "real owner close");
        const int listener_fd = listener.release(); // Ownership now belongs to the prepared CLOSE SQE.
        const auto result = awaitable->await_resume();
        check(probe.wakes == 1 && !result &&
              IOError::contains(result.error().code(), kClosed), "real close selects result once");
        awaitable.reset();
        controller.reset(); // Both borrowed addresses are gone before processing any CQE.

        std::vector<IOUringReactorTestAccess::Completion> acknowledgements;
        IOUringReactorTestAccess::Completion terminal{};
        bool original_done = false;
        for (unsigned i = 0; i != 8 && (!original_done || acknowledgements.size() != 2); ++i) {
            const auto cqe = IOUringReactorTestAccess::takeKernelCompletion(reactor);
            if (!check(cqe.has_value(), "real cancel/close CQE within deadline")) { return; }
            if (cqe->handle == handle) {
                if (!check(!original_done && cqe->result == -ECANCELED &&
                           (cqe->flags & IORING_CQE_F_MORE) == 0, "real original terminal")) { return; }
                terminal = *cqe;
                original_done = true;
            } else if (check(cqe->handle == nullptr && cqe->result == 0,
                             "real cancel/close acknowledgement")) {
                acknowledgements.push_back(*cqe);
            }
        }
        if (!check(original_done && acknowledgements.size() == 2, "real three-CQE drain")) { return; }
        const auto deliver_acks = [&] {
            for (const auto& cqe : acknowledgements) {
                IOUringReactorTestAccess::deliver(reactor, cqe.handle, cqe.result, cqe.flags);
            }
        };
        if (acknowledgements_first) { deliver_acks(); }
        check(handle->arena && handle->persistent, "real ack retains original request");
        const int late_fd = accepted.release();
        IOUringReactorTestAccess::deliver(reactor, handle, late_fd, ready->flags);
        expectClosed(late_fd);
        check(handle->arena && handle->persistent, "real late MORE retains original request");
        IOUringReactorTestAccess::deliver(reactor, terminal.handle, terminal.result, terminal.flags);
        if (!acknowledgements_first) { deliver_acks(); }
        check(!handle->arena && !handle->state && probe.wakes == 1, "real terminal releases exactly once");
        check(isClosed(listener_fd) && error.load() == 0, "real listener close observed");
    }
    check(lifetime.expired(), "real CQE drain releases arena at reactor teardown");
    std::cout << "T190 real_kernel acknowledgements_first=" << acknowledgements_first
              << " failures=" << failures - before << '\n';
}
}
#endif

int main(int argc, char** argv) {
#ifdef USE_IOURING
    if (argc == 2) {
        logicalCompletionBoundary(std::string_view(argv[1]) == "--release-controller");
        return failures == 0 ? 0 : 1;
    }
    logicalCompletionBoundary(false);
    logicalCompletionBoundary(true);
    liveDelivery();
    staleRecvIsNotDescriptor();
    closeOrdering(false);
    closeOrdering(true);
    inlineOwnerDestruction(false);
    inlineOwnerDestruction(true);
    realKernelClose(false);
    realKernelClose(true);
    for (const auto mode : {Invalidation::Generation, Invalidation::OwnerNull, Invalidation::Destroyed}) {
        lateSuccess(mode, false);
        lateSuccess(mode, true);
    }
    std::cout << "T190 backend=io_uring failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
#else
    std::cout << "T190 SKIP (requires io_uring)\n";
    return 77;
#endif
}
