/** @brief 配置变体共享安全的借用分派，并能恢复任务、注册 IO 和处理超时。 */
#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/scheduler_dispatch.hpp>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <type_traits>
#include <unistd.h>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

#if defined(USE_IOURING)
template <typename Config> using ConfiguredScheduler = IOUringSchedulerT<Config>;
#elif defined(USE_KQUEUE)
template <typename Config> using ConfiguredScheduler = KqueueSchedulerT<Config>;
#else
template <typename Config> using ConfiguredScheduler = EpollSchedulerT<Config>;
#endif

using CustomConfig = IOSchedulerConfig<17, 3, 128>;

// 所有可被 Scheduler* 借用的 IO 配置，必须包含分派入口使用的真实基类。
static_assert(std::is_base_of_v<IOSchedulerBackend, ConfiguredScheduler<DefaultIOSchedulerConfig>>);
static_assert(std::is_base_of_v<IOSchedulerBackend, ConfiguredScheduler<HighPerformanceIOSchedulerConfig>>);
static_assert(std::is_base_of_v<IOSchedulerBackend, ConfiguredScheduler<LowLatencyIOSchedulerConfig>>);
static_assert(std::is_base_of_v<IOSchedulerBackend, ConfiguredScheduler<CustomConfig>>);
static_assert(!std::is_polymorphic_v<IOSchedulerBackend>);
static_assert(!std::is_destructible_v<IOSchedulerBackend>);

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "[T192] " << message << '\n';
        std::abort();
    }
}

void wait_for(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!flag.load(std::memory_order_acquire)) {
        require(std::chrono::steady_clock::now() < deadline, "task progress deadline");
        std::this_thread::yield();
    }
}

template <typename Config>
struct SchedulerProbe final : ConfiguredScheduler<Config> {
    int batch_budget() const { return this->m_batch_size; }
};

struct State {
    explicit State(int fd) : socket(GHandle{.fd = fd}) {}
    galay::async::AsyncTcpSocket socket;
    Waker waker;
    std::atomic<bool> parked{false};
    std::atomic<bool> receiving{false};
    std::atomic<bool> done{false};
    std::thread::id resumed_thread;
};

struct Park {
    State* state;
    bool await_ready() const noexcept { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        state->waker = Waker(handle);
        state->parked.store(true, std::memory_order_release);
        return true;
    }
    void await_resume() const noexcept {}
};

Task<int> answer() { co_return 42; }

Task<void> exercise(State* state) {
    const auto child = co_await answer();
    require(child && *child == 42, "child task continuation");
    co_await Park{state};
    state->resumed_thread = std::this_thread::get_id();

    char byte = 0;
    state->receiving.store(true, std::memory_order_release);
    const auto received = co_await state->socket.recv(&byte, 1).timeout(2s);
    require(received && *received == 1 && byte == 'x', "IO completion");
    const auto timed_out = co_await state->socket.recv(&byte, 1).timeout(5ms);
    require(!timed_out && IOError::contains(timed_out.error().code(), kTimeout),
            "IO timeout through configured scheduler");
    const auto closed = co_await state->socket.close();
    require(closed.has_value(), "close through configured scheduler");
    state->done.store(true, std::memory_order_release);
}

template <typename Config>
void check_config(const char* name) {
    SchedulerProbe<Config> scheduler;
    require(scheduler.batch_budget() == Config::kBatchSize, "configured batch budget");
    Scheduler* borrowed = &scheduler;
    const auto started = borrowed->start();
    require(started.has_value(), "start through borrowed scheduler");
    require(!borrowed->schedule(TaskRef{}), "reject invalid task");
    require(!borrowed->schedule_resume(TaskRef{}), "reject invalid resume");

    int fds[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    for (int fd : fds) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        require(flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "nonblocking socket");
    }
    State state(fds[0]);
    require(schedule_task_deferred(borrowed, exercise(&state)), "submit through borrowed scheduler");
    wait_for(state.parked);
    state.waker.wake_up();
    wait_for(state.receiving);
    require(::send(fds[1], "x", 1, 0) == 1, "send peer byte");
    wait_for(state.done);
    borrowed->stop();
    require(state.resumed_thread == scheduler.thread_id(), "resume on owner thread");
    require(::close(fds[1]) == 0, "close peer");
    const auto restarted = borrowed->start();
    require(restarted.has_value(), "restart through borrowed scheduler");
    borrowed->stop();
    std::cout << "[T192] " << name << " PASS\n";
}

} // namespace

int main() {
#ifdef USE_IOURING
    IOScheduler probe;
    const auto available = probe.start();
    if (!available) {
        const auto error = static_cast<uint32_t>(available.error().code() >> 32);
        if (error == EPERM || error == ENOSYS || error == EOPNOTSUPP) {
            std::cout << "T192 SKIP: io_uring unavailable, errno=" << error << '\n';
            return 77;
        }
        require(false, "io_uring initialization");
    }
    probe.stop();
#endif
    check_config<DefaultIOSchedulerConfig>("default");
    check_config<HighPerformanceIOSchedulerConfig>("high performance");
    check_config<LowLatencyIOSchedulerConfig>("low latency");
    check_config<CustomConfig>("custom");
}
