#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/uring_reactor.h>

#include <atomic>
#include <cerrno>
#include <iostream>
#include <string_view>

#ifdef USE_IOURING
using namespace galay::kernel;

namespace galay::kernel {
struct IOUringReactorTestAccess {
    static void deliver(IOUringReactor& reactor, SqeRequestHandle* handle,
                        int result, unsigned flags) {
        io_uring_cqe cqe{};
        cqe.user_data = reinterpret_cast<uintptr_t>(handle);
        cqe.res = result;
        cqe.flags = flags;
        reactor.process_completion(&cqe);
    }
};
}

namespace {
unsigned failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

struct Probe {
    detail::ResumeTokenHeader header;
    unsigned wakes = 0;
};

const detail::ResumeTokenHooks kHooks{
    .owner_scheduler = [](void*) noexcept -> Scheduler* { return nullptr; },
    .request_resume = [](void* state) noexcept {
        ++static_cast<Probe*>(state)->wakes;
        return true;
    },
};

void completion_order(bool notification_first, bool invalidate) {
    std::atomic<uint64_t> error{0};
    IOUringReactor reactor(32, error);
    IOController controller(GHandle{.fd = -1});
    char buffer[4096]{};
    SendAwaitable awaitable(&controller, buffer, sizeof(buffer));
    awaitable.m_sqe_type = SEND;
    Probe probe{};
    probe.header.hooks = &kHooks;
    awaitable.m_waker = Waker(detail::ResumeToken::from_non_owning_c_coroutine(&probe));
    check(controller.fill_awaitable(SEND, &awaitable), "register send");
    auto* handle = controller.make_sqe_request(IOController::WRITE);
    check(handle != nullptr, "allocate handle");
    if (!handle) { return; }
    handle->notify_expected = true;
    if (notification_first) {
        IOUringReactorTestAccess::deliver(reactor, handle, -1, IORING_CQE_F_NOTIF);
    } else {
        IOUringReactorTestAccess::deliver(reactor, handle, 2048, IORING_CQE_F_MORE);
    }
    check(probe.wakes == 0, "borrowed buffer remains pinned until both CQEs");
    check(handle->arena != nullptr, "handle survives first CQE");
    if (invalidate) { controller.advance_sqe_generation(IOController::WRITE); }
    if (notification_first) {
        IOUringReactorTestAccess::deliver(reactor, handle, 2048, IORING_CQE_F_MORE);
    } else {
        // Notification res reports zero-copy usage, never the send byte count.
        IOUringReactorTestAccess::deliver(reactor, handle, -1, IORING_CQE_F_NOTIF);
    }
    check(probe.wakes == (invalidate ? 0u : 1u), "deliver exactly once to current owner");
    if (!invalidate) {
        check(awaitable.m_result && *awaitable.m_result == 2048, "preserve partial send result");
    }
    check(handle->arena == nullptr, "recycle after both CQEs");
}

void terminal_result(int result) {
    std::atomic<uint64_t> error{0};
    IOUringReactor reactor(32, error);
    IOController controller(GHandle{.fd = -1});
    char buffer[4096]{};
    SendAwaitable awaitable(&controller, buffer, sizeof(buffer));
    awaitable.m_sqe_type = SEND;
    Probe probe{};
    probe.header.hooks = &kHooks;
    awaitable.m_waker = Waker(detail::ResumeToken::from_non_owning_c_coroutine(&probe));
    check(controller.fill_awaitable(SEND, &awaitable), "register terminal send");
    auto* handle = controller.make_sqe_request(IOController::WRITE);
    check(handle != nullptr, "allocate terminal handle");
    if (!handle) { return; }
    handle->notify_expected = true;
    IOUringReactorTestAccess::deliver(reactor, handle, result, 0);
    check(probe.wakes == 1, "terminal CQE without MORE completes immediately");
    check(handle->arena == nullptr, "no notification means no retained arena cycle");
    if (result < 0) {
        check(!awaitable.m_result, "preserve send error");
    } else {
        check(awaitable.m_result && *awaitable.m_result == static_cast<size_t>(result),
              "preserve terminal byte count");
    }
}
}
#endif

int main() {
#ifdef USE_IOURING
    completion_order(false, false);
    completion_order(true, false);
    completion_order(false, true);
    completion_order(true, true);
    terminal_result(-EPIPE);
    terminal_result(2048);
    return failures == 0 ? 0 : 1;
#else
    std::cout << "SKIP: requires io_uring\n";
    return 0;
#endif
}
