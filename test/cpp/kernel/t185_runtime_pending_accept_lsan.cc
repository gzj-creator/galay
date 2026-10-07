#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <iostream>
#include <atomic>
#include <chrono>
#include <future>
#include <charconv>
#include <string_view>
#include <vector>

using namespace galay::async;
using namespace galay::kernel;

namespace {

struct Trace {
    std::promise<bool> registered;
    unsigned resumes = 0;
    unsigned destroys = 0;
    bool closed = false;
};

struct RegisteredAccept {
    AcceptAwaitable inner;
    IOController* controller;
    Trace& trace;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<Task<void>::promise_type> handle) {
        const bool suspended = inner.await_suspend(handle);
        // 这是 owner 上提交完成的屏障；主线程不读取并发变化的 controller。
        trace.registered.set_value(suspended &&
            controller->m_awaitable[IOController::READ] == &inner);
        return suspended;
    }
    auto await_resume() { return inner.await_resume(); }
};

Task<void> pending_accept(AsyncTcpSocket& listener, Trace& trace)
{
    struct FrameProbe {
        Trace& trace;
        ~FrameProbe() { ++trace.destroys; }
    } frame{trace};
    Host peer;
    auto result = co_await RegisteredAccept{listener.accept(&peer), listener.controller(), trace};
    ++trace.resumes;
    trace.closed = !result && IOError::contains(result.error().code(), kClosed);
    co_return;
}

} // namespace

int main(int argc, char** argv)
{
    unsigned count = 1;
    if (argc > 1) {
        const std::string_view argument(argv[1]);
        const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), count);
        if (argc != 2 || parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() ||
            count == 0 || count > 10000) { return 2; }
    }
    std::vector<AsyncTcpSocket> listeners;
    listeners.reserve(count);
    for (unsigned i = 0; i != count; ++i) {
        auto listener = AsyncTcpSocket::create(IPType::IPV4);
        if (!listener || !listener->option().handle_non_block() ||
            !listener->bind(Host(IPType::IPV4, "127.0.0.1", 0)) || !listener->listen(16)) {
            std::cerr << "listener setup failed at " << i << '\n';
            return 1;
        }
        listeners.push_back(std::move(*listener));
    }

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    auto started = runtime.start();
    if (!started) {
        std::cerr << "runtime start failed\n";
        return 1;
    }

    std::vector<Trace> traces(count);
    std::vector<std::future<bool>> registrations;
    std::vector<TaskRef> keepers;
    registrations.reserve(count);
    keepers.reserve(count);
    for (unsigned i = 0; i != count; ++i) {
        registrations.push_back(traces[i].registered.get_future());
        auto task = pending_accept(listeners[i], traces[i]);
        keepers.push_back(detail::TaskAccess::task_ref(task));
        if (!runtime.spawn_io(std::move(task))) {
            std::cerr << "pending accept submit failed\n";
            runtime.stop();
            return 1;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (auto& registered : registrations) {
        if (registered.wait_until(deadline) != std::future_status::ready || !registered.get()) {
            std::cerr << "accept registration barrier failed\n";
            runtime.stop();
            return 1;
        }
    }
    // 停机必须由 owner 完成已登记 accept，listener 本身仍由调用方持有。
    const auto started_stop = std::chrono::steady_clock::now();
    runtime.stop();
    const auto stop_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_stop).count();
    unsigned resumes = 0, destroys = 0, detached = 0, closed = 0, frames = 0;
    bool ok = true;
    for (unsigned i = 0; i != count; ++i) {
        const bool unbound = listeners[i].controller()->m_awaitable[IOController::READ] == nullptr;
        const bool destroyed = keepers[i].state()->m_handle == nullptr;
        resumes += traces[i].resumes;
        destroys += traces[i].destroys;
        detached += unbound;
        closed += traces[i].closed;
        frames += destroyed;
        ok = ok && traces[i].resumes == 1 && traces[i].destroys == 1 && unbound && destroyed && traces[i].closed;
    }
    std::cout << "T185 pending=" << count << " resumes=" << resumes << " destroys=" << destroys
              << " detached=" << detached << " closed=" << closed << " frames=" << frames
              << " stop_ms=" << stop_ms << '\n';
    return ok ? 0 : 1;
}
