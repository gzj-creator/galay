/**
 * @file t189_accept_completion_integration.cc
 * @brief accept ready/close/timeout 的 owner-thread 确定性集成门禁。
 *
 * 手动驱动真实 EpollScheduler，注册完成后才注入候选事件；不启动 worker，
 * 不用 sleep 推测挂起。默认保留 controller 到恢复后，以便一次报告全部合约
 * 失败；--release-controller 则在完成派发后立即释放它，供 ASan 验证 UAF。
 * 同一测试在旧路径失败，迁移后必须通过，不能用 WILL_FAIL 掩盖门禁。
 */
#include <galay/cpp/galay-kernel/core/awaitable.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace galay::kernel {
struct EpollReactorTestAccess {
    static epoll_event event(EpollReactor& reactor, int fd) {
        epoll_event result{};
        result.events = EPOLLIN;
        result.data.ptr = reactor.m_registration_entries.at(fd).get();
        return result;
    }
    static void deliver(EpollReactor& reactor, epoll_event event) { reactor.processEvent(event); }
    static void generation(EpollReactor& reactor, uint32_t value) {
        reactor.m_next_accept_generation = value;
    }
};
}

namespace {

using AcceptResult = std::expected<GHandle, IOError>;

/** Synchronous test owner; the production queue and reactor remain unchanged. */
class ManualScheduler final : public EpollScheduler {
public:
    bool initialize() {
        const auto started = m_reactor.start();
        if (!started || !m_worker.reopenResumeAdmission()) {
            return false;
        }
        m_threadId = std::this_thread::get_id();
        return true;
    }

    bool flush() { return m_reactor.flushPendingChanges() == 0; }

    bool pollOnce() {
        m_reactor.poll(0, m_wake_coordinator);
        return !lastError();
    }
    GHandle pollHandle() const { return m_reactor.getPollHandle(); }
    epoll_event event(int fd) { return EpollReactorTestAccess::event(m_reactor, fd); }
    void deliver(epoll_event event) { EpollReactorTestAccess::deliver(m_reactor, event); }
    void generation(uint32_t value) { EpollReactorTestAccess::generation(m_reactor, value); }
    size_t timers() const { return m_timer_manager.size(); }
    void tick() { m_timer_manager.tick(); }

    size_t dispatch() {
        size_t count = 0;
        // Bounded so a broken completion loop reports failure instead of hanging.
        for (unsigned pass = 0; pass != 8 && m_core.hasPendingWork(); ++pass) {
            const auto ran = m_core.runReadyPass(
                [this, &count](TaskRef& task) { ++count; resume(task); },
                [this](size_t drained) { m_wake_coordinator.onRemoteCollected(drained); });
            if (ran == 0) {
                break;
            }
        }
        return count;
    }
};

/** Owns only descriptors created by the synchronous test driver. */
class TestFd final {
public:
    explicit TestFd(int fd = -1) noexcept : m_fd(fd) {}
    TestFd(TestFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    TestFd& operator=(TestFd&&) = delete;
    ~TestFd() {
        if (!close()) {
            std::cerr << "[T189] descriptor cleanup failed, errno=" << errno << '\n';
        }
    }
    int get() const noexcept { return m_fd; }
    int release() noexcept { return std::exchange(m_fd, -1); }
    bool close() noexcept {
        const int fd = release();
        return fd < 0 || ::close(fd) == 0;
    }
private:
    TestFd(const TestFd&) = delete;
    TestFd& operator=(const TestFd&) = delete;
    int m_fd;
};

struct Trace {
    AcceptAwaitable* awaitable = nullptr; // Valid only while the task is suspended.
    std::shared_ptr<AcceptTimeoutTimer> timer; // Pins the detached timer through a late callback.
    Host peer;
    std::optional<AcceptResult> result;
    unsigned resumes = 0;
    unsigned scope_destroys = 0;
    bool null_peer = false;
};

/** Combined with TaskState::m_handle == nullptr to observe frame completion. */
class FrameProbe final {
public:
    explicit FrameProbe(Trace& trace) noexcept : m_trace(trace) {}
    ~FrameProbe() { ++m_trace.scope_destroys; }
    FrameProbe(FrameProbe&&) = delete;
    FrameProbe& operator=(FrameProbe&&) = delete;
private:
    FrameProbe(const FrameProbe&) = delete;
    FrameProbe& operator=(const FrameProbe&) = delete;
    Trace& m_trace;
};

Task<void> acceptOnce(IOController* controller, Trace* trace, bool timed) {
    FrameProbe frame(*trace);
    if (timed) {
        auto awaitable = AcceptAwaitable(controller, trace->null_peer ? nullptr : &trace->peer).timeout(1h);
        awaitable.ensureTimer();
        trace->awaitable = &awaitable;
        trace->timer = awaitable.m_timer;
        trace->result.emplace(co_await awaitable);
    } else {
        AcceptAwaitable awaitable(controller, trace->null_peer ? nullptr : &trace->peer);
        trace->awaitable = &awaitable;
        trace->result.emplace(co_await awaitable);
    }
    trace->awaitable = nullptr;
    ++trace->resumes;
}

enum class Event { kReady, kClose, kTimeout };
struct Scenario {
    std::string_view name;
    Event first;
    Event second;
    bool success;
    IOErrorCode error;
};

constexpr std::array kScenarios{
    Scenario{"ready-close", Event::kReady, Event::kClose, true, kNotReady},
    Scenario{"close-ready", Event::kClose, Event::kReady, false, kClosed},
    Scenario{"timeout-close", Event::kTimeout, Event::kClose, false, kTimeout},
    Scenario{"close-timeout", Event::kClose, Event::kTimeout, false, kClosed},
    Scenario{"timeout-ready", Event::kTimeout, Event::kReady, false, kTimeout},
    Scenario{"ready-timeout", Event::kReady, Event::kTimeout, true, kNotReady},
};

bool connectClient(int fd, const sockaddr_in& address) {
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        return true;
    }
    if (errno != EINPROGRESS) {
        return false;
    }
    // This is a bounded synchronous setup barrier, outside any Task call chain.
    pollfd event{fd, POLLOUT, 0};
    int ready;
    do { ready = ::poll(&event, 1, 1000); } while (ready < 0 && errno == EINTR);
    int error = 0;
    socklen_t size = sizeof(error);
    return ready == 1 && (event.revents & POLLOUT) != 0 &&
           ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
}

bool runScenario(const Scenario& scenario, bool release_controller) {
    bool ok = true;
    const auto check = [&](bool value, std::string_view contract) {
        if (!value) {
            std::cerr << "[T189] " << scenario.name << ": " << contract << '\n';
            ok = false;
        }
        return value;
    };

    TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (!check(listener.get() >= 0 &&
               ::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
               ::listen(listener.get(), 16) == 0 &&
               ::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0,
               "listener setup")) {
        return false;
    }

    ManualScheduler scheduler;
    if (!check(scheduler.initialize(), "manual owner initialization")) {
        return false;
    }
    auto controller = std::make_unique<IOController>(GHandle{.fd = listener.get()});
    Trace trace;
    const bool timed = scenario.first == Event::kTimeout || scenario.second == Event::kTimeout;
    auto task = acceptOnce(controller.get(), &trace, timed);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    if (!check(scheduleTask(scheduler, std::move(task)), "task submission") ||
        !check(scheduler.dispatch() == 1 && trace.resumes == 0, "one initial suspension") ||
        !check(scheduler.flush(), "register accept in epoll") ||
        !check(trace.awaitable && controller->m_awaitable[IOController::READ] == trace.awaitable &&
               controller->m_registration_owner_slot && controller->m_registered_events != 0,
               "accept registration barrier")) {
        return false;
    }

    // The reactor owns this stable slot even after controller is freed. It is
    // intentionally observed, not modified or used as a fake operation counter.
    IOController** registration = controller->m_registration_owner_slot;
    bool attached = true;
    unsigned observed_detaches = 0;
    const auto observeDetach = [&] {
        const bool now_attached = *registration != nullptr;
        if (attached && !now_attached) {
            ++observed_detaches;
        }
        attached = now_attached;
    };

    TestFd client(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    // For close->ready, queue real TCP readiness before close, but dispatch the
    // reactor only afterwards. This covers kernel-queued readiness, not replay
    // of an already copied epoll batch (that needs an adapter-level key test).
    if (scenario.first == Event::kReady || scenario.second == Event::kReady) {
        if (!check(client.get() >= 0 && connectClient(client.get(), address), "client connect barrier")) {
            return false;
        }
    }

    const auto deliver = [&](Event event) {
        switch (event) {
        case Event::kReady:
            check(scheduler.pollOnce(), "ready dispatch");
            break;
        case Event::kClose:
            if (check(scheduler.addClose(controller.get()) == 0, "listener close")) {
                const int transferred = listener.release();
                check(transferred >= 0, "listener fd closed once");
            }
            break;
        case Event::kTimeout:
            trace.timer->handleTimeout(); // Deterministic owner-thread delivery.
            break;
        }
        observeDetach();
    };

    deliver(scenario.first);
    check(trace.resumes == 0 && trace.scope_destroys == 0, "completion must queue, not resume inline");
    check(controller->m_awaitable[IOController::READ] == nullptr && !attached,
          "winner must detach registration before resume admission");
    check(trace.awaitable->m_controller == nullptr, "winner must end the awaiter's controller borrow");
    check(trace.awaitable->m_operation->state().physicalReferenceCount() == 0 &&
          trace.awaitable->m_operation->state().phase() == OperationPhase::kResumeIssued,
          "physical drain precedes resume admission");
    const auto key = trace.awaitable->m_operation->state().key();
    check(key.isValid(), "submitted operation has a valid key");
    deliver(scenario.second);
    check(observed_detaches == 1 && !attached, "one observed registration detach; loser cannot reattach");

    if (release_controller) {
        controller.reset(); // ASan must permit the following await_resume.
    }
    const size_t resume_dispatches = scheduler.dispatch();
    check(resume_dispatches == 1 && trace.resumes == 1 && trace.scope_destroys == 1 &&
          keeper.state()->m_handle == nullptr, "exactly one resume and completed frame destruction");
    if (check(trace.result.has_value(), "accept result delivered")) {
        check(scenario.success ? trace.result->has_value()
                               : !*trace.result && IOError::contains(trace.result->error().code(), scenario.error),
              "first completion candidate fixes the result");
        if (*trace.result) {
            TestFd accepted(trace.result->value().fd);
            check(accepted.close(), "accepted fd cleanup");
        }
    }
    if (trace.timer) {
        trace.timer->handleTimeout(); // Late callback after frame destruction.
        trace.timer.reset();
    }
    check(scheduler.pollOnce() && scheduler.dispatch() == 0, "late events cannot resume the task again");
    check(trace.resumes == 1 && trace.scope_destroys == 1, "late events cannot destroy the frame again");
    check(client.close() && listener.close(), "driver descriptor cleanup");
    std::cout << "T189 " << scenario.name << (ok ? " PASS" : " FAIL")
              << " resumes=" << trace.resumes << " frame_scopes=" << trace.scope_destroys
              << " observed_detaches=" << observed_detaches << '\n';
    return ok;
}

bool runBoundaries() {
    ManualScheduler scheduler;
    if (!scheduler.initialize()) { return false; }
    TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (listener.get() < 0 ||
        ::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), 16) != 0 ||
        ::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return false;
    }
    auto controller = std::make_unique<IOController>(GHandle{.fd = listener.get()});
    Trace first;
    auto first_task = acceptOnce(controller.get(), &first, true);
    if (!scheduleTask(scheduler, std::move(first_task)) || scheduler.dispatch() != 1 || !scheduler.flush()) {
        return false;
    }
    const auto first_key = first.awaitable->m_operation->state().key();
    const auto old_event = scheduler.event(listener.get());
    Trace duplicate;
    if (!scheduleTask(scheduler, acceptOnce(controller.get(), &duplicate, false)) ||
        scheduler.dispatch() != 1 || !duplicate.result || *duplicate.result ||
        !IOError::contains(duplicate.result->error().code(), kNotReady) ||
        controller->m_awaitable[IOController::READ] != first.awaitable) {
        return false;
    }
    first.timer->handleTimeout();
    if (scheduler.dispatch() != 1 || first.resumes != 1) { return false; }
    Trace next;
    if (!scheduleTask(scheduler, acceptOnce(controller.get(), &next, false)) ||
        scheduler.dispatch() != 1 || !scheduler.flush()) { return false; }
    const auto next_key = next.awaitable->m_operation->state().key();
    TestFd next_client(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!connectClient(next_client.get(), address)) { return false; }
    first.timer->handleTimeout(); // 旧 timer 不能取消同资源上的新 operation。
    scheduler.deliver(old_event); // 已拷贝的旧 event 不能访问新 operation。
    if (first_key == next_key || next.resumes != 0 || scheduler.dispatch() != 0) { return false; }
    // IOController 的既有移动契约必须包含 pending accept；释放旧地址后才关闭。
    auto moved = std::make_unique<IOController>(std::move(*controller));
    controller.reset();
    controller = std::move(moved);
    if (scheduler.addClose(controller.get()) != 0) { return false; }
    const int closed_fd = listener.release();
    if (closed_fd < 0) { return false; }
    controller.reset();
    if (scheduler.dispatch() != 1 || next.resumes != 1 || next.scope_destroys != 1) { return false; }
    scheduler.deliver(old_event); // controller/frame 释放后同一 event 仍可安全丢弃。
    Trace invalid;
    if (!scheduleTask(scheduler, acceptOnce(nullptr, &invalid, false)) || scheduler.dispatch() != 1 ||
        !invalid.result || *invalid.result || !IOError::contains(invalid.result->error().code(), kClosed)) {
        return false;
    }

    // 已排队的连接在 await_suspend 中同步完成；nullptr Host 是公开允许值。
    TestFd immediate_listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    // Force the old numeric fd to refer to a different listener, then replay a
    // copied event while that replacement has a real connection ready.
    if (immediate_listener.get() < 0) { return false; }
    if (immediate_listener.get() != closed_fd) {
        if (::dup3(immediate_listener.get(), closed_fd, O_CLOEXEC) != closed_fd ||
            !immediate_listener.close()) { return false; }
    } else if (immediate_listener.release() != closed_fd) { return false; }
    TestFd reused_listener(closed_fd);
    address.sin_port = 0;
    if (::bind(reused_listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(reused_listener.get(), 16) != 0 ||
        ::getsockname(reused_listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return false;
    }
    TestFd client(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    IOController immediate_controller(GHandle{.fd = reused_listener.get()});
    Trace reused;
    if (!scheduleTask(scheduler, acceptOnce(&immediate_controller, &reused, false)) ||
        scheduler.dispatch() != 1 || !scheduler.flush() || !connectClient(client.get(), address)) { return false; }
    scheduler.deliver(old_event);
    if (scheduler.dispatch() != 0 || reused.resumes != 0) { return false; }
    scheduler.deliver(scheduler.event(closed_fd));
    if (scheduler.dispatch() != 1 || !reused.result || !*reused.result) { return false; }
    TestFd reused_accepted(reused.result->value().fd);
    if (!reused_accepted.close() || !client.close()) { return false; }
    TestFd immediate_client(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Trace immediate;
    immediate.null_peer = true;
    if (!connectClient(immediate_client.get(), address) ||
        !scheduleTask(scheduler, acceptOnce(&immediate_controller, &immediate, true)) ||
        scheduler.dispatch() != 1 || !immediate.result || !*immediate.result) { return false; }
    TestFd accepted(immediate.result->value().fd);
    immediate.timer->handleTimeout();
    const bool ok = scheduler.dispatch() == 0 && immediate.resumes == 1 &&
        immediate.scope_destroys == 1 && accepted.close();
    std::cout << "T189 duplicate/key-reuse/fd-reuse/late-timer/invalid/synchronous/null-peer "
              << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}

bool runRegistrationFailure() {
    TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener.get() < 0 ||
        ::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), 16) != 0) { return false; }
    ManualScheduler scheduler;
    if (!scheduler.initialize()) { return false; }
    auto controller = std::make_unique<IOController>(GHandle{.fd = listener.get()});
    Trace trace;
    if (!scheduleTask(scheduler, acceptOnce(controller.get(), &trace, true)) ||
        scheduler.dispatch() != 1 || !trace.awaitable) { return false; }
    // 故意使 deferred EPOLL_CTL_ADD 在 flush 时失败；不能留下永久 pending。
    if (!listener.close() || scheduler.flush()) { return false; }
    controller.reset();
    const bool resumed = scheduler.dispatch() == 1 && trace.resumes == 1 && trace.scope_destroys == 1;
    const bool error = trace.result && !*trace.result &&
        IOError::contains(trace.result->error().code(), kAcceptFailed) &&
        (trace.result->error().code() >> 32) == EBADF;
    if (trace.timer) { trace.timer->handleTimeout(); }
    std::cout << "T189 deferred-registration-failure " << (resumed && error ? "PASS" : "FAIL") << '\n';
    return resumed && error;
}

bool runExpiredTimerDuringSubmit() {
    ManualScheduler scheduler;
    if (!scheduler.initialize()) { return false; }
    TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener.get() < 0 ||
        ::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), 16) != 0) { return false; }
    IOController controller(GHandle{.fd = listener.get()});
    struct Probe {
        detail::ResumeTokenHeader header;
        Scheduler* scheduler;
        unsigned resumes = 0;
    } probe{{}, &scheduler};
    static const detail::ResumeTokenHooks hooks{
        .owner_scheduler = [](void* state) noexcept { return static_cast<Probe*>(state)->scheduler; },
        .request_resume = [](void* state) noexcept {
            ++static_cast<Probe*>(state)->resumes;
            return true;
        },
    };
    probe.header.hooks = &hooks;
    auto operation = AcceptAwaitable(&controller, nullptr).timeout(0ms);
    // The real wheel calls an expired timer inline from push(). Publishing a
    // resume while suspend() still owns the operation would permit frame UAF.
    const bool suspended = operation.suspend(
        Waker(detail::ResumeToken::fromNonOwningCCoroutine(&probe)));
    const auto result = operation.await_resume();
    const bool ok = !suspended && probe.resumes == 0 && !result &&
        IOError::contains(result.error().code(), kTimeout) &&
        controller.m_awaitable[IOController::READ] == nullptr &&
        operation.m_operation->state().physicalReferenceCount() == 0;
    std::cout << "T189 expired-timer-during-submit " << (ok ? "PASS" : "FAIL")
              << " suspended=" << suspended << " resumes=" << probe.resumes << '\n';
    return ok;
}

bool runReentrantRegistrationFailure() {
    ManualScheduler scheduler;
    if (!scheduler.initialize()) { return false; }
    auto listener = [] {
        TestFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd.get() < 0 || ::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd.get(), 16) != 0) { return TestFd{}; }
        return fd;
    };
    auto first_fd = listener();
    auto failed_fd = listener();
    if (first_fd.get() < 0 || failed_fd.get() < 0) { return false; }
    auto first = std::make_unique<IOController>(GHandle{.fd = first_fd.get()});
    IOController failed(GHandle{.fd = failed_fd.get()});
    Trace trace;
    if (!scheduleTask(scheduler, acceptOnce(first.get(), &trace, true)) ||
        scheduler.dispatch() != 1 || !scheduler.flush()) { return false; }
    struct Probe {
        detail::ResumeTokenHeader header;
        ManualScheduler* scheduler;
        std::unique_ptr<IOController>* other;
        unsigned resumes = 0;
        bool closed = false;
    } probe{{}, &scheduler, &first};
    static const detail::ResumeTokenHooks hooks{
        .owner_scheduler = [](void* state) noexcept -> Scheduler* {
            return static_cast<Probe*>(state)->scheduler;
        },
        .request_resume = [](void* state) noexcept {
            auto& probe = *static_cast<Probe*>(state);
            ++probe.resumes;
            probe.closed = probe.scheduler->addClose(probe.other->get()) == 0;
            probe.other->reset(); // Another completion may destroy the resource inline.
            return true;
        },
    };
    probe.header.hooks = &hooks;
    AcceptAwaitable operation(&failed, nullptr);
    if (!operation.suspend(Waker(detail::ResumeToken::fromNonOwningCCoroutine(&probe))) ||
        !failed_fd.close()) { return false; }
    // Detaching the first operation flushes the second operation's failed ADD.
    // Its inline recovery closes and frees the first controller during flush.
    trace.timer->handleTimeout();
    // A non-reentrant detach may leave the unrelated ADD for the next flush.
    if (probe.resumes == 0 && scheduler.flush()) { return false; }
    if (!probe.closed || first || probe.resumes != 1) { return false; }
    const int closed = first_fd.release();
    if (closed < 0) { return false; }
    const auto failure = operation.await_resume();
    const bool ok = scheduler.dispatch() == 1 && trace.resumes == 1 &&
        trace.result && !*trace.result &&
        IOError::contains(trace.result->error().code(), kTimeout) &&
        !failure && (failure.error().code() >> 32) == EBADF;
    std::cout << "T189 reentrant-registration-failure " << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}

bool runInlineRecoveryAndResultOwnership() {
    for (bool consume : {false, true}) {
        ManualScheduler scheduler;
        if (!scheduler.initialize()) { return false; }
        TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (listener.get() < 0 ||
            ::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener.get(), 16) != 0 ||
            ::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) { return false; }
        auto controller = std::make_unique<IOController>(GHandle{.fd = listener.get()});
        auto operation = std::make_unique<AcceptAwaitable>(controller.get(), nullptr);
        struct Probe {
            detail::ResumeTokenHeader header;
            ManualScheduler* scheduler;
            std::unique_ptr<AcceptAwaitable>* operation;
            std::unique_ptr<IOController>* controller;
            int accepted = -1;
            unsigned resumes = 0;
            bool consume;
            bool closed = false;
        } probe{{}, &scheduler, &operation, &controller, -1, 0, consume};
        static const detail::ResumeTokenHooks hooks{
            .owner_scheduler = [](void* state) noexcept -> Scheduler* { return static_cast<Probe*>(state)->scheduler; },
            .request_resume = [](void* state) noexcept {
                auto& probe = *static_cast<Probe*>(state);
                ++probe.resumes;
                if (probe.consume) {
                    auto result = (*probe.operation)->await_resume();
                    if (result) { probe.accepted = result->fd; }
                }
                probe.operation->reset(); // Unconsumed success must close its fd.
                probe.closed = probe.scheduler->addClose(probe.controller->get()) == 0;
                probe.controller->reset();
                return true;
            },
        };
        probe.header.hooks = &hooks;
        if (!operation->suspend(Waker(detail::ResumeToken::fromNonOwningCCoroutine(&probe))) ||
            !scheduler.flush()) { return false; }
        const auto stale = scheduler.event(listener.get());
        TestFd client(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!connectClient(client.get(), address)) { return false; }
        scheduler.deliver(stale);
        if (!probe.closed || operation || controller || probe.resumes != 1) { return false; }
        const int closed = listener.release();
        if (closed < 0) { return false; }
        scheduler.deliver(stale);
        if (consume) {
            TestFd accepted(probe.accepted);
            if (accepted.get() < 0 || !accepted.close()) { return false; }
        }
        // Both consumed and discarded results must leave the peer at EOF.
        pollfd event{client.get(), POLLIN, 0};
        char byte = 0;
        if (::poll(&event, 1, 1000) != 1 || ::recv(client.get(), &byte, 1, 0) != 0) { return false; }
    }
    std::cout << "T189 inline-recovery/result-fd-ownership PASS\n";
    return true;
}

bool runGenerationExhaustionAndWheel() {
    ManualScheduler scheduler;
    if (!scheduler.initialize()) { return false; }
    TestFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener.get() < 0 ||
        ::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), 16) != 0) { return false; }
    IOController controller(GHandle{.fd = listener.get()});
    scheduler.generation(UINT32_MAX);
    Trace last;
    // Exercise the real wheel with a positive delay; bounded synchronous driver
    // progress is a watchdog, never a scheduling assumption in the coroutine.
    auto timed = [](IOController* controller, Trace* trace) -> Task<void> {
        auto operation = AcceptAwaitable(controller, nullptr).timeout(1ms);
        trace->awaitable = &operation;
        trace->result.emplace(co_await operation);
        trace->awaitable = nullptr;
        ++trace->resumes;
    };
    if (!scheduleTask(scheduler, timed(&controller, &last)) || scheduler.dispatch() != 1 ||
        !scheduler.flush() || !last.awaitable ||
        last.awaitable->m_operation->state().key().generation != UINT32_MAX || scheduler.timers() != 1) { return false; }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (scheduler.timers() != 0 && std::chrono::steady_clock::now() < deadline) {
        scheduler.tick();
        std::this_thread::yield();
    }
    if (scheduler.timers() != 0 || scheduler.dispatch() != 1 || last.resumes != 1 ||
        !last.result || *last.result || !IOError::contains(last.result->error().code(), kTimeout)) { return false; }
    Trace exhausted;
    if (!scheduleTask(scheduler, acceptOnce(&controller, &exhausted, false)) || scheduler.dispatch() != 1 ||
        !exhausted.result || *exhausted.result ||
        (exhausted.result->error().code() >> 32) != EOVERFLOW || controller.m_registration_owner_slot) { return false; }
    std::cout << "T189 generation-exhaustion/real-wheel PASS\n";
    return true;
}

bool runSubmitDoesNotDispatchOtherOperations() {
    ManualScheduler scheduler;
    if (!scheduler.initialize()) { return false; }
    std::array<std::unique_ptr<IOController>, 32> controllers;
    std::array<std::unique_ptr<AcceptAwaitable>, 32> operations;
    for (unsigned i = 0; i != controllers.size(); ++i) {
        TestFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd.get() < 0 || ::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd.get(), 16) != 0) { return false; }
        controllers[i] = std::make_unique<IOController>(GHandle{.fd = fd.release()});
        operations[i] = std::make_unique<AcceptAwaitable>(controllers[i].get(), nullptr);
    }
    struct Probe {
        detail::ResumeTokenHeader header;
        ManualScheduler* scheduler;
        std::unique_ptr<IOController>* target;
        unsigned resumes = 0;
        bool closed = false;
    } probe{{}, &scheduler, &controllers.back()};
    static const detail::ResumeTokenHooks hooks{
        .owner_scheduler = [](void* state) noexcept -> Scheduler* { return static_cast<Probe*>(state)->scheduler; },
        .request_resume = [](void* state) noexcept {
            auto& probe = *static_cast<Probe*>(state);
            ++probe.resumes;
            probe.closed = probe.scheduler->addClose(probe.target->get()) == 0;
            probe.target->reset();
            return true;
        },
    };
    probe.header.hooks = &hooks;
    if (!operations[0]->suspend(Waker(detail::ResumeToken::fromNonOwningCCoroutine(&probe)))) { return false; }
    // Invalidate the first pending ADD. The 32nd accept must not flush it while
    // still submitting: its recovery can close/free the currently used resource.
    if (::close(controllers[0]->m_handle.fd) != 0) { return false; }
    bool all_suspended = true;
    for (unsigned i = 1; i != controllers.size(); ++i) {
        all_suspended = scheduler.submitAccept(*operations[i], Waker{}) && all_suspended;
    }
    const bool deferred = probe.resumes == 0;
    if (deferred && scheduler.flush()) { return false; }
    const auto result = operations.back()->await_resume();
    bool ok = all_suspended && deferred && probe.closed && probe.resumes == 1 && !result &&
        IOError::contains(result.error().code(), kClosed);
    controllers[0]->m_handle = GHandle::invalid();
    for (unsigned i = 1; i + 1 != controllers.size(); ++i) {
        ok = scheduler.addClose(controllers[i].get()) == 0 && ok;
    }
    std::cout << "T189 submit-does-not-dispatch-other-operations " << (ok ? "PASS" : "FAIL")
              << " all_suspended=" << all_suspended << " deferred=" << deferred << '\n';
    return ok;
}

} // namespace
#endif

int main(int argc, char** argv) {
#ifndef USE_EPOLL
    std::cout << "T189-AcceptCompletionIntegration SKIP (requires epoll)\n";
    return 0;
#else
    std::string_view selected;
    bool release_controller = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--release-controller") {
            release_controller = true;
        } else {
            selected = argv[index];
        }
    }
    bool ok = true;
    unsigned ran = 0;
    for (const auto& scenario : kScenarios) {
        if (selected.empty() || selected == scenario.name) {
            ok = runScenario(scenario, release_controller) && ok;
            ++ran;
        }
    }
    if (ran == 0) {
        std::cerr << "[T189] unknown scenario\n";
        return 2;
    }
    if (selected.empty()) {
        ok = runBoundaries() && ok;
        ok = runRegistrationFailure() && ok;
        ok = runExpiredTimerDuringSubmit() && ok;
        ok = runReentrantRegistrationFailure() && ok;
        ok = runInlineRecoveryAndResultOwnership() && ok;
        ok = runGenerationExhaustionAndWheel() && ok;
        ok = runSubmitDoesNotDispatchOtherOperations() && ok;
    }
    return ok ? 0 : 1;
#endif
}
