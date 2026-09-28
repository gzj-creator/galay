/** @file b41_accept_completion_lifecycle.cc
 *  @brief 有界真实 loopback accept：先停泊、再连接、再完成；含预热与延迟分位。
 *  B3 connect-only 的一秒采样窗口不能测量短 accept 吞吐，本入口计时到实际恢复。
 *  同步客户端和清理只运行于 benchmark driver，协程只执行异步 accept。
 */
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>
#include <unistd.h>

using namespace galay::kernel;
using Clock = std::chrono::steady_clock;

#if defined(USE_EPOLL) || defined(USE_IOURING)
namespace {
class Owner final : public IOScheduler {
public:
    bool initialize() {
        if (!m_reactor.start() || !m_worker.reopenResumeAdmission()) { return false; }
        m_threadId = std::this_thread::get_id();
        return true;
    }
    bool dispatch() {
        const auto ran = m_core.runReadyPass(
            [this](TaskRef& task) { resume(task); },
            [this](size_t n) { m_wake_coordinator.onRemoteCollected(n); });
        (void)ran; // An empty nonblocking poll is progress-neutral, not an error.
#ifdef USE_EPOLL
        return m_reactor.flushPendingChanges() == 0;
#else
        return !lastError();
#endif
    }
    bool poll() {
        m_reactor.poll(0, m_wake_coordinator);
        return !lastError();
    }
    bool registered(const IOController& controller) const {
#ifdef USE_EPOLL
        return controller.m_registration_owner_slot != nullptr;
#else
        return controller.m_accept_multishot_armed;
#endif
    }
};

struct Batch {
    std::vector<int> accepted;
    std::vector<Clock::time_point> starts;
    std::vector<double> latencies;
    size_t completed = 0;
    size_t errors = 0;
};

Task<void> acceptBatch(IOController& controller, Batch& batch) {
    for (size_t i = 0; i != batch.accepted.size(); ++i) {
        Host peer;
        auto result = co_await AcceptAwaitable(&controller, &peer);
        if (!result) { ++batch.errors; co_return; }
        batch.accepted[i] = result->fd;
        batch.latencies[i] = std::chrono::duration<double, std::micro>(Clock::now() - batch.starts[i]).count();
        ++batch.completed;
    }
}

bool measure(size_t width, size_t rounds) {
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (listener < 0) { return false; }
    const auto closeFd = [](int fd) { return fd < 0 || ::close(fd) == 0; };
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 256) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        if (!closeFd(listener)) { std::cerr << "listener cleanup failed\n"; }
        return false;
    }
    Owner owner;
    if (!owner.initialize()) {
        if (!closeFd(listener)) { std::cerr << "listener cleanup failed\n"; }
        return false;
    }
    IOController controller(GHandle{.fd = listener});
    Batch batch{std::vector<int>(width, -1), std::vector<Clock::time_point>(width),
                std::vector<double>(width)};
    std::vector<int> clients(width, -1);
    std::vector<double> samples;
    samples.reserve(rounds * width);
    double elapsed = 0;
    size_t errors = 0;
    constexpr size_t warmup = 16;
    for (size_t round = 0; round != rounds + warmup; ++round) {
        batch.completed = 0;
        batch.errors = 0;
        if (!scheduleTask(owner, acceptBatch(controller, batch)) || !owner.dispatch() ||
            batch.completed != 0 || !owner.registered(controller)) { ++errors; break; }
        for (auto& client : clients) {
            client = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (client < 0) { ++errors; }
        }
        const auto begin = Clock::now();
        for (size_t i = 0; i != width; ++i) {
            batch.starts[i] = Clock::now();
            if (::connect(clients[i], reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 &&
                errno != EINPROGRESS) { ++errors; }
        }
        // 只在同步 driver poll；硬上限用于报告 OS/setup 故障，不放入协程。
        for (unsigned pass = 0; batch.completed != width && batch.errors == 0 && pass != 10000; ++pass) {
            if (!owner.poll()) { ++errors; break; }
            if (!owner.dispatch()) { ++errors; break; }
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        errors += batch.errors + (batch.completed != width);
        if (round >= warmup) {
            elapsed += seconds;
            samples.insert(samples.end(), batch.latencies.begin(), batch.latencies.end());
        }
        for (size_t i = 0; i != width; ++i) {
            errors += !closeFd(std::exchange(batch.accepted[i], -1));
            errors += !closeFd(std::exchange(clients[i], -1));
        }
        if (errors != 0) { break; }
    }
    if (owner.addClose(&controller) != 0) { ++errors; }
    // Error paths can still have a suspended waiter. Drain its close result
    // while controller/batch storage is alive, including on the before build.
    if (!owner.dispatch()) { ++errors; }
#ifdef USE_IOURING
    // Submit the prepared cancel/close before tearing down this manual owner.
    // This is benchmark cleanup, not the Runtime physical-drain acceptance gate.
    if (!owner.poll()) { ++errors; }
#endif
    owner.stop();
    if (samples.empty()) { return false; }
    std::sort(samples.begin(), samples.end());
    std::cout << std::fixed << std::setprecision(3)
              << "B41 backend="
#ifdef USE_EPOLL
              << "epoll"
#else
              << "io_uring"
#endif
              << " batch=" << width << " operations=" << samples.size()
              << " warmup_batches=" << warmup << " accepts_per_s=" << samples.size() / elapsed
              << " p50_us=" << samples[samples.size() / 2]
              << " p99_us=" << samples[(samples.size() - 1) * 99 / 100]
              << " errors=" << errors << " awaiter_bytes=" << sizeof(AcceptAwaitable) << '\n';
    return errors == 0;
}
} // namespace
#endif

int main() {
#if defined(USE_EPOLL) || defined(USE_IOURING)
    const bool single = measure(1, 2048);
    const bool burst = measure(64, 128);
    return single && burst ? 0 : 1;
#else
    std::cout << "B41 SKIP (requires epoll or io_uring)\n";
    return 77;
#endif
}
