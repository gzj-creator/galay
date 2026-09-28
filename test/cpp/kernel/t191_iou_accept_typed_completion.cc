/** @file t191_iou_accept_typed_completion.cc
 * @brief 单次 accept typed completion 的 owner 顺序、frame 与资源生命周期边界。
 * 同步 driver 准备 SQE/注入 CQE；不启动 worker，不依赖 sleep 或运行时 drain。
 */
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <array>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#include <fcntl.h>
#include <unistd.h>
using namespace galay::kernel;
using namespace std::chrono_literals;
namespace galay::kernel {
struct IOUringReactorTestAccess {
    static void deliver(IOUringReactor& reactor, SqeRequestHandle* handle, int result, unsigned flags) {
        io_uring_cqe cqe{};
        cqe.user_data = reinterpret_cast<uintptr_t>(handle);
        cqe.res = result;
        cqe.flags = flags;
        reactor.processCompletion(&cqe);
    }
    static unsigned fillSubmissionQueue(IOUringReactor& reactor) {
        unsigned count = 0;
        while (auto* sqe = io_uring_get_sqe(&reactor.m_ring)) {
            io_uring_prep_nop(sqe);
            io_uring_sqe_set_data(sqe, nullptr);
            ++count;
        }
        return count;
    }
    static void generation(IOUringReactor& reactor, uint32_t generation) {
        reactor.m_next_accept_generation = generation;
    }
};
}
namespace {
unsigned failures = 0;
bool check(bool value, const char* contract) {
    if (!value) { ++failures; std::cerr << "[T191] FAIL " << contract << '\n'; }
    return value;
}
class Fd final {
public:
    explicit Fd(int fd = -1) noexcept : m_fd(fd) {}
    Fd(Fd&& other) noexcept : m_fd(other.release()) {}
    Fd& operator=(Fd&&) = delete;
    ~Fd() { if (m_fd >= 0) { check(::close(m_fd) == 0, "driver fd cleanup"); } }
    int get() const noexcept { return m_fd; }
    int release() noexcept { return std::exchange(m_fd, -1); }
private:
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int m_fd;
};
bool closed(int fd) { errno = 0; return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF; }
class Owner final : public IOUringScheduler {
public:
    Owner() : IOUringScheduler(32) {}
    bool initialize() {
        if (!m_reactor.start() || !m_worker.reopenResumeAdmission()) { return false; }
        m_threadId = std::this_thread::get_id();
        return true;
    }
    IOUringReactor& reactor() { return m_reactor; }
    void tick() { m_timer_manager.tick(); }
    size_t dispatch() {
        size_t count = 0;
        for (unsigned pass = 0; pass != 8 && m_core.hasPendingWork(); ++pass) {
            const auto ran = m_core.runReadyPass(
                [this, &count](TaskRef& task) { ++count; resume(task); },
                [this](size_t n) { m_wake_coordinator.onRemoteCollected(n); });
            if (ran == 0) { break; }
        }
        return count;
    }
};
struct Probe {
    detail::ResumeTokenHeader header;
    Owner* owner;
    void* context = nullptr;
    void (*callback)(void*) noexcept = nullptr;
    unsigned wakes = 0;
};
Waker waker(Probe& probe) {
    static const detail::ResumeTokenHooks hooks{
        .owner_scheduler = [](void* p) noexcept -> Scheduler* { return static_cast<Probe*>(p)->owner; },
        .request_resume = [](void* p) noexcept {
            auto& probe = *static_cast<Probe*>(p);
            ++probe.wakes;
            if (probe.callback) { probe.callback(probe.context); }
            return true;
        },
    };
    probe.header.hooks = &hooks;
    return Waker(detail::ResumeToken::fromNonOwningCCoroutine(&probe));
}
void drained(const AcceptAwaitable& op, const IOController& controller) {
    check(controller.m_awaitable[IOController::READ] == nullptr && op.m_controller == nullptr &&
          op.m_registration_state == nullptr, "slot and controller borrow detached");
    check(op.m_operation->state().physicalReferenceCount() == 0 &&
          op.m_operation->state().phase() == OperationPhase::kResumeIssued,
          "frame refs drained before resume issued");
    check(!op.m_timer || op.m_timer->cancelled(), "timer detached before resume");
}
enum class Event { Ready, Timeout, Close };
void ordering(Event first, Event second) {
    const auto before = failures;
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && owner.initialize(), "ordering setup")) { return; }
    auto controller = std::make_unique<IOController>(GHandle{listener.get()});
    auto op = AcceptAwaitable(controller.get(), nullptr).timeout(1h);
    Probe probe{{}, &owner};
    if (!check(op.suspend(waker(probe)), "ordering parked")) { return; }
    auto timer = op.m_timer;
    auto* handle = controller->m_accept_multishot_handle;
    int accepted = -1;
    const auto deliver = [&](Event event) {
        if (event == Event::Ready) {
            accepted = ::dup(listener.get());
            if (check(accepted >= 0, "ordering accepted fd")) {
                IOUringReactorTestAccess::deliver(owner.reactor(), handle, accepted, IORING_CQE_F_MORE);
            }
        } else if (event == Event::Timeout) { timer->handleTimeout(); }
        else { check(owner.addClose(controller.get()) == 0, "ordering close"); }
    };
    deliver(first);
    drained(op, *controller);
    check(probe.wakes == 1 && handle->persistent && handle->arena,
          "one logical wake precedes original terminal");
    deliver(second);
    check(probe.wakes == 1, "loser cannot issue second resume");
    controller.reset();
    const auto result = op.await_resume();
    if (first == Event::Ready) {
        check(result && result->fd == accepted && !closed(accepted), "ready result survives controller destruction");
        if (result) { check(::close(result->fd) == 0, "consumed result cleanup"); }
    } else {
        check(!result && IOError::contains(result.error().code(),
              first == Event::Timeout ? kTimeout : kClosed), "first error remains selected");
        if (accepted >= 0) { check(closed(accepted), "losing success cached or stale fd reclaimed"); }
    }
    timer->handleTimeout();
    IOUringReactorTestAccess::deliver(owner.reactor(), nullptr, 0, 0);
    check(handle->arena && handle->persistent, "cancel ack is not original terminal");
    IOUringReactorTestAccess::deliver(owner.reactor(), handle, -ECANCELED, 0);
    check(!handle->arena && !handle->state && probe.wakes == 1, "terminal recycle independent of logical resume");
    std::cout << "T191 ordering=" << static_cast<int>(first) << ',' << static_cast<int>(second)
              << " failures=" << failures-before << '\n';
}
void staleEntrypoints() {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && owner.initialize(), "stale setup")) { return; }
    IOController controller(GHandle{listener.get()});
    auto first = AcceptAwaitable(&controller, nullptr).timeout(1h);
    Probe first_probe{{}, &owner};
    if (!check(first.suspend(waker(first_probe)), "stale first parked")) { return; }
    const auto old_key = first.m_operation->state().key();
    auto timer = first.m_timer;
    auto* old = controller.m_accept_multishot_handle;
    timer->handleTimeout();
    check(first_probe.wakes == 1 && old->persistent, "timeout retains stream");
    // Valid CQEs still belong to the resource, even after the user timeout.
    const int cached = ::dup(listener.get());
    if (!check(cached >= 0, "timeout cache fd")) { return; }
    IOUringReactorTestAccess::deliver(owner.reactor(), old, cached, IORING_CQE_F_MORE);
    Probe cached_probe{{}, &owner};
    AcceptAwaitable consume(&controller, nullptr);
    check(!consume.suspend(waker(cached_probe)), "post-timeout cache consumed synchronously");
    const auto cached_result = consume.await_resume();
    check(cached_result && cached_result->fd == cached && cached_probe.wakes == 0, "resource cache ownership transferred");
    if (cached_result) { check(::close(cached_result->fd) == 0, "cache cleanup"); }
    controller.invalidateSqeRequests(); // Deterministic resource epoch boundary.
    auto next = AcceptAwaitable(&controller, nullptr).timeout(1h);
    Probe next_probe{{}, &owner};
    if (!check(next.suspend(waker(next_probe)), "new epoch parked")) { return; }
    auto* current = controller.m_accept_multishot_handle;
    check(current != old && next.m_operation->state().key() != old_key, "new request and operation identities");
    timer->handleTimeout();
    const int late = ::dup(listener.get());
    if (!check(late >= 0, "stale fd")) { return; }
    IOUringReactorTestAccess::deliver(owner.reactor(), old, late, IORING_CQE_F_MORE);
    check(closed(late) && next_probe.wakes == 0 && !next.m_operation->state().completionReason(),
          "old timer and request cannot touch new operation");
    IOUringReactorTestAccess::deliver(owner.reactor(), old, -ECANCELED, 0);
    check(!old->arena && current->arena && current->persistent, "old terminal leaves current request alive");
    next.m_timer->handleTimeout();
    drained(next, controller);
    controller.invalidateSqeRequests();
    IOUringReactorTestAccess::deliver(owner.reactor(), current, -ECANCELED, 0);
    check(next_probe.wakes == 1, "new operation finishes once");
}
void submissionBoundaries() {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && owner.initialize(), "submission setup")) { return; }
    IOController controller(GHandle{listener.get()});
    Probe probe{{}, &owner};
    auto expired = AcceptAwaitable(&controller, nullptr).timeout(0ms);
    check(!expired.suspend(waker(probe)) && probe.wakes == 0, "expired push never resumes during suspend");
    const auto expired_result = expired.await_resume();
    check(!expired_result && IOError::contains(expired_result.error().code(), kTimeout), "expired push selects timeout");
    drained(expired, controller);
    auto* handle = controller.m_accept_multishot_handle;
    controller.invalidateSqeRequests();
    IOUringReactorTestAccess::deliver(owner.reactor(), handle, -ECANCELED, 0);
    check(IOUringReactorTestAccess::fillSubmissionQueue(owner.reactor()) > 0, "fill SQ deterministically");
    auto rejected = AcceptAwaitable(&controller, nullptr).timeout(1h);
    check(!rejected.suspend(waker(probe)) && probe.wakes == 0, "SQ exhaustion completes synchronously");
    drained(rejected, controller);
    rejected.m_timer->handleTimeout();
    check(owner.addClose(&controller) == 0, "close after failed submission");
    const int closed_listener = listener.release(); // Full SQ: addClose owns synchronous close.
    check(closed(closed_listener), "submission failure close ownership");
    const auto rejected_result = rejected.await_resume();
    check(!rejected_result && IOError::contains(rejected_result.error().code(), kAcceptFailed) &&
          (rejected_result.error().code() >> 32) == EAGAIN, "SQ error preserved across timeout and close");
}
void realTimer() {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && owner.initialize(), "real timer setup")) { return; }
    IOController controller(GHandle{listener.get()});
    Probe probe{{}, &owner};
    auto op = AcceptAwaitable(&controller, nullptr).timeout(1ms);
    if (!check(op.suspend(waker(probe)), "positive timer parked")) { return; }
    auto* handle = controller.m_accept_multishot_handle;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    // Bounded synchronous driver only; the coroutine/production path never polls.
    while (probe.wakes == 0 && std::chrono::steady_clock::now() < deadline) {
        owner.tick();
        std::this_thread::yield();
    }
    check(probe.wakes == 1, "real wheel dispatches positive timeout");
    drained(op, controller);
    const auto result = op.await_resume();
    check(!result && IOError::contains(result.error().code(), kTimeout), "real wheel typed timeout");
    controller.invalidateSqeRequests();
    IOUringReactorTestAccess::deliver(owner.reactor(), handle, -ECANCELED, 0);
}
void stopAdmission() {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Fd next_listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && next_listener.get() >= 0 && owner.initialize(), "stop setup")) { return; }
    IOController controller(GHandle{listener.get()});
    IOController next_controller(GHandle{next_listener.get()});
    Probe probe{{}, &owner};
    auto op = AcceptAwaitable(&controller, nullptr).timeout(1h);
    if (!check(op.suspend(waker(probe)), "stop parked")) { return; }
    auto* old_handle = controller.m_accept_multishot_handle;
    owner.reactor().stopAccepts();
    drained(op, controller);
    check(probe.wakes == 1 && op.m_operation->state().completionReason() == CompletionReason::kRuntimeStopped,
          "owner stop selects typed runtime result");
    Probe next_probe{{}, &owner};
    AcceptAwaitable rejected(&next_controller, nullptr);
    check(!rejected.suspend(waker(next_probe)), "stopped owner rejects new frame");
    const auto rejected_result = rejected.await_resume();
    check(!rejected_result && IOError::contains(rejected_result.error().code(), kClosed), "stopped owner returns closed");
    IOUringReactorTestAccess::deliver(owner.reactor(), old_handle, -ECANCELED, 0);
    check(owner.reactor().start().has_value(), "explicit owner restart");
    auto restarted = AcceptAwaitable(&next_controller, nullptr).timeout(1h);
    if (!check(restarted.suspend(waker(next_probe)), "restart reopens accept admission")) { return; }
    auto* next_handle = next_controller.m_accept_multishot_handle;
    restarted.m_timer->handleTimeout();
    drained(restarted, next_controller);
    next_controller.invalidateSqeRequests();
    IOUringReactorTestAccess::deliver(owner.reactor(), next_handle, -ECANCELED, 0);
}
void terminalSubmissionFailure(int completion) {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && owner.initialize(), "terminal submission setup")) { return; }
    auto controller = std::make_unique<IOController>(GHandle{listener.get()});
    Probe probe{{}, &owner};
    auto op = AcceptAwaitable(controller.get(), nullptr).timeout(1h);
    if (!check(op.suspend(waker(probe)), "terminal submission parked")) { return; }
    auto* handle = controller->m_accept_multishot_handle;
    check(IOUringReactorTestAccess::fillSubmissionQueue(owner.reactor()) > 0, "fill SQ before terminal");
    const int ready = completion == 0 ? ::dup(listener.get()) : completion;
    if (completion == 0 && !check(ready >= 0, "terminal ready fd")) { return; }
    IOUringReactorTestAccess::deliver(owner.reactor(), handle, ready, 0);
    drained(op, *controller);
    check(!handle->arena && !controller->m_accept_multishot_armed && probe.wakes == 1,
          "terminal rearm failure still completes once");
    op.m_timer->handleTimeout();
    controller.reset();
    const auto result = op.await_resume();
    if (completion == 0) {
        check(result && result->fd == ready, "rearm failure cannot overwrite terminal success");
        if (result) { check(::close(result->fd) == 0, "terminal result cleanup"); }
    } else {
        check(!result && IOError::contains(result.error().code(), kAcceptFailed) &&
              (result.error().code() >> 32) == static_cast<unsigned>(completion == -EINTR ? EAGAIN : -completion),
              "original error or rearm failure keeps original errno");
    }
}
void peerFailure() {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && owner.initialize(), "peer failure setup")) { return; }
    IOController controller(GHandle{listener.get()});
    Host peer;
    Probe probe{{}, &owner};
    AcceptAwaitable op(&controller, &peer);
    if (!check(op.suspend(waker(probe)), "peer failure parked")) { return; }
    auto* handle = controller.m_accept_multishot_handle;
    const int unconnected = ::dup(listener.get());
    if (!check(unconnected >= 0, "unconnected accepted fd")) { return; }
    IOUringReactorTestAccess::deliver(owner.reactor(), handle, unconnected, IORING_CQE_F_MORE);
    const auto result = op.await_resume();
    check(!result && (result.error().code() >> 32) == ENOTCONN && closed(unconnected),
          "peer lookup failure retains errno and reclaims owned fd");
    controller.invalidateSqeRequests();
    IOUringReactorTestAccess::deliver(owner.reactor(), handle, -ECANCELED, 0);
}
void admissionBoundaries() {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && owner.initialize(), "admission setup")) { return; }
    IOController controller(GHandle{listener.get()});
    Probe probe{{}, &owner};
    auto op = AcceptAwaitable(&controller, nullptr).timeout(1h);
    if (!check(op.suspend(waker(probe)), "original parked")) { return; }
    Probe other_probe{{}, &owner};
    AcceptAwaitable duplicate(&controller, nullptr);
    check(!duplicate.suspend(waker(other_probe)), "duplicate rejected");
    const auto rejected = duplicate.await_resume();
    check(!rejected && (rejected.error().code() >> 32) == EBUSY && other_probe.wakes == 0 &&
          controller.m_awaitable[IOController::READ] == &op, "duplicate does not detach original");
    // Move the resource while the operation is parked: stable state resolves its new owner.
    auto moved = std::make_unique<IOController>(std::move(controller));
    op.m_timer->handleTimeout();
    drained(op, *moved);
    auto* handle = moved->m_accept_multishot_handle;
    moved.reset();
    IOUringReactorTestAccess::deliver(owner.reactor(), handle, -ECANCELED, 0);
    const auto result = op.await_resume();
    check(!result && IOError::contains(result.error().code(), kTimeout) && probe.wakes == 1,
          "moved then freed controller is not borrowed by resume");
    AcceptAwaitable null_controller(nullptr, nullptr);
    check(!null_controller.suspend(waker(other_probe)), "null controller completes synchronously");
    const auto null_result = null_controller.await_resume();
    check(!null_result && IOError::contains(null_result.error().code(), kClosed), "null controller typed error");
    IOController valid(GHandle{listener.get()});
    IOUringReactorTestAccess::generation(owner.reactor(), UINT32_MAX);
    auto last = AcceptAwaitable(&valid, nullptr).timeout(0ms);
    check(!last.suspend(waker(other_probe)), "last key consumed synchronously");
    AcceptAwaitable overflow(&valid, nullptr);
    check(!overflow.suspend(waker(other_probe)), "key overflow rejected");
    const auto overflow_result = overflow.await_resume();
    check(!overflow_result && (overflow_result.error().code() >> 32) == EOVERFLOW,
          "key overflow does not publish duplicate identity");
}
struct FrameTrace {
    AcceptAwaitable* op = nullptr;
    std::shared_ptr<AcceptTimeoutTimer> timer;
    std::optional<std::expected<GHandle, IOError>> result;
    unsigned resumes = 0;
    unsigned destroys = 0;
};
Task<void> acceptFrame(IOController* controller, FrameTrace& trace) {
    struct Scope { FrameTrace& trace; ~Scope() { ++trace.destroys; } } scope{trace};
    auto op = AcceptAwaitable(controller, nullptr).timeout(1h);
    op.ensureTimer();
    trace.op = &op;
    trace.timer = op.m_timer;
    trace.result.emplace(co_await op);
    trace.op = nullptr;
    ++trace.resumes;
}
void frameBoundary(Event event) {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && owner.initialize(), "frame setup")) { return; }
    auto controller = std::make_unique<IOController>(GHandle{listener.get()});
    FrameTrace trace;
    auto task = acceptFrame(controller.get(), trace);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    if (!check(scheduleTask(owner, std::move(task)) && owner.dispatch() == 1 && trace.op,
               "real coroutine registered")) { return; }
    auto* handle = controller->m_accept_multishot_handle;
    if (event == Event::Ready) {
        const int accepted = ::dup(listener.get());
        if (!check(accepted >= 0, "frame fd")) { return; }
        IOUringReactorTestAccess::deliver(owner.reactor(), handle, accepted, IORING_CQE_F_MORE);
    } else if (event == Event::Timeout) { trace.timer->handleTimeout(); }
    else { check(owner.addClose(controller.get()) == 0, "frame close"); }
    drained(*trace.op, *controller);
    controller.reset();
    check(owner.dispatch() == 1 && trace.resumes == 1 && trace.destroys == 1 &&
          keeper.state()->m_handle == nullptr, "one resume and completed coroutine frame destruction");
    if (check(trace.result.has_value(), "real coroutine result")) {
        if (event == Event::Ready) {
            check(trace.result->has_value(), "frame success survives destroyed controller");
            if (*trace.result) { check(::close(trace.result->value().fd) == 0, "frame result cleanup"); }
        } else {
            check(!*trace.result && IOError::contains(trace.result->error().code(),
                  event == Event::Timeout ? kTimeout : kClosed), "frame typed error");
        }
    }
    trace.timer->handleTimeout();
    IOUringReactorTestAccess::deliver(owner.reactor(), handle, -ECANCELED, 0);
    check(owner.dispatch() == 0 && !handle->arena, "late sources cannot resume destroyed frame");
}
void inlineDestruction(Event event, bool more, bool consume) {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Owner owner;
    if (!check(listener.get() >= 0 && owner.initialize(), "inline setup")) { return; }
    auto controller = std::make_unique<IOController>(GHandle{listener.get()});
    auto op = std::make_unique<AcceptAwaitable>(AcceptAwaitable(controller.get(), nullptr).timeout(1h));
    Probe probe{{}, &owner};
    struct Context {
        std::unique_ptr<IOController>& controller;
        std::unique_ptr<AcceptAwaitable>& op;
        bool consume;
    } context{controller, op, consume};
    probe.context = &context;
    probe.callback = [](void* p) noexcept {
        auto& ctx = *static_cast<Context*>(p);
        drained(*ctx.op, *ctx.controller);
        // Published OperationCompletion has a fixed address. Move its owning
        // pointer; also move the actual controller, then destroy both inline.
        auto owned_op = std::move(ctx.op);
        IOController moved(std::move(*ctx.controller));
        ctx.controller.reset();
        if (ctx.consume) {
            const auto result = owned_op->await_resume();
            if (result) { check(::close(result->fd) == 0, "inline consumed fd"); }
        }
        owned_op.reset();
    };
    if (!check(op->suspend(waker(probe)), "inline parked")) { return; }
    auto timer = op->m_timer;
    auto* handle = controller->m_accept_multishot_handle;
    int accepted = -1;
    if (event == Event::Ready) {
        accepted = ::dup(listener.get());
        if (!check(accepted >= 0, "inline fd")) { return; }
        IOUringReactorTestAccess::deliver(owner.reactor(), handle, accepted, more ? IORING_CQE_F_MORE : 0);
    } else if (event == Event::Timeout) { timer->handleTimeout(); }
    else { check(owner.addClose(controller.get()) == 0, "inline close"); }
    check(!controller && !op && probe.wakes == 1, "inline callback releases both owners");
    if (accepted >= 0) { check(closed(accepted), "inline consumed or abandoned success reclaimed"); }
    timer->handleTimeout();
    if (event != Event::Ready || more) {
        check(handle->arena && handle->persistent, "inline callback preserves original MORE request");
        IOUringReactorTestAccess::deliver(owner.reactor(), handle, -ECANCELED, 0);
    }
    check(!handle->arena && !handle->state && probe.wakes == 1, "inline original terminal reclaim");
}
}
#endif
int main() {
#ifdef USE_IOURING
    for (auto first : {Event::Ready, Event::Timeout, Event::Close}) {
        for (auto second : {Event::Ready, Event::Timeout, Event::Close}) {
            if (first != second) { ordering(first, second); }
        }
        frameBoundary(first);
        inlineDestruction(first, true, false);
    }
    inlineDestruction(Event::Ready, false, false);
    inlineDestruction(Event::Ready, true, true);
    staleEntrypoints();
    submissionBoundaries();
    stopAdmission();
    realTimer();
    terminalSubmissionFailure(0);
    terminalSubmissionFailure(-EBADF);
    terminalSubmissionFailure(-EINTR);
    peerFailure();
    admissionBoundaries();
    std::cout << "T191 backend=io_uring failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
#else
    std::cout << "T191 SKIP (requires io_uring)\n";
    return 77;
#endif
}
