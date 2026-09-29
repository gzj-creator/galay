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
#include <ctime>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <netinet/tcp.h>
#include <numeric>
#include <string_view>
#include <poll.h>
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
    bool latency = true;
};

Task<void> acceptBatch(IOController& controller, Batch& batch) {
    for (size_t i = 0; i != batch.accepted.size(); ++i) {
        Host peer;
        auto result = co_await AcceptAwaitable(&controller, &peer);
        if (!result) {
            std::cerr << "B41 accept error=" << result.error().code() << '\n';
            ++batch.errors;
            co_return;
        }
        batch.accepted[i] = result->fd;
        if (batch.latency) {
            batch.latencies[i] = std::chrono::duration<double, std::micro>(Clock::now() - batch.starts[i]).count();
        }
        ++batch.completed;
    }
}

// Failure-only observations: SO_ERROR=0 alone does not prove a completed
// nonblocking connect. SYN_SENT plus an empty listener is not a lost wakeup.
void diagnoseTimeout(int listener, const std::vector<int>& clients, bool registered) {
    pollfd ready{listener, POLLIN, 0};
    const int polled = ::poll(&ready, 1, 0);
    std::cerr << "B41 diagnostic registered=" << registered
              << " listener_poll=" << polled << " revents=" << ready.revents;
    if (polled < 0) { std::cerr << " errno=" << errno; }
    std::cerr << '\n';
    for (size_t i = 0; i != clients.size(); ++i) {
        tcp_info info{};
        socklen_t size = sizeof(info);
        if (::getsockopt(clients[i], IPPROTO_TCP, TCP_INFO, &info, &size) != 0) {
            std::cerr << "B41 client=" << i << " TCP_INFO errno=" << errno << '\n';
            continue;
        }
        int error = 0;
        size = sizeof(error);
        if (::getsockopt(clients[i], SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
            std::cerr << "B41 client=" << i << " SO_ERROR errno=" << errno << '\n';
            continue;
        }
        if (info.tcpi_state != TCP_ESTABLISHED || error != 0) {
            std::cerr << "B41 client=" << i << " tcp_state=" << unsigned(info.tcpi_state)
                      << " retransmits=" << unsigned(info.tcpi_retransmits)
                      << " so_error=" << error << '\n';
        }
    }
    for (const char* name : {"nf_conntrack_count", "nf_conntrack_max"}) {
        std::ifstream input(std::string("/proc/sys/net/netfilter/") + name);
        unsigned long value = 0;
        if (input >> value) { std::cerr << "B41 " << name << '=' << value << '\n'; }
        else { std::cerr << "B41 " << name << " unavailable\n"; }
    }
}

bool measure(size_t width, size_t rounds, bool components = false, bool native = false, bool latency = true) {
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
    batch.latency = latency;
    std::vector<int> clients(width, -1);
    std::vector<double> samples;
    samples.reserve(rounds * width);
    double elapsed = 0;
    double cpu_ns = 0;
    double connect_ns = 0, confirm_ns = 0, accept_ns = 0;
    size_t errors = 0;
    constexpr size_t warmup = 16;
    for (size_t round = 0; round != rounds + warmup; ++round) {
        batch.completed = 0;
        batch.errors = 0;
        if (!components && (!scheduleTask(owner, acceptBatch(controller, batch)) || !owner.dispatch() ||
            batch.completed != 0 || !owner.registered(controller))) { ++errors; break; }
        for (auto& client : clients) {
            client = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (client < 0) {
                std::cerr << "B41 socket errno=" << errno << '\n';
                ++errors;
            }
        }
        const auto cpu_begin = std::clock();
        const auto begin = Clock::now();
        for (size_t i = 0; i != width; ++i) {
            if (latency) { batch.starts[i] = Clock::now(); }
            if (::connect(clients[i], reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 &&
                errno != EINPROGRESS) {
                std::cerr << "B41 connect errno=" << errno << '\n';
                ++errors;
            }
        }
        // Diagnostic-only phase boundaries. Default B41 retains its parked path.
        auto connected = begin, confirmed = begin;
        if (components) {
            connected = Clock::now();
            // Confirm all client handshakes and listener readability BEFORE the
            // ready-accept timer; this setup cost is reported separately.
            const auto deadline = begin + std::chrono::seconds(2);
            for (const int client : clients) {
                bool established = false;
                do {
                    tcp_info info{};
                    socklen_t size = sizeof(info);
                    if (::getsockopt(client, IPPROTO_TCP, TCP_INFO, &info, &size) != 0) {
                        ++errors; break;
                    }
                    established = info.tcpi_state == TCP_ESTABLISHED;
                } while (!established && Clock::now() < deadline);
                if (!established) { ++errors; }
            }
            pollfd ready{listener, POLLIN, 0};
            if (::poll(&ready, 1, 0) != 1 || !(ready.revents & POLLIN)) { ++errors; }
            confirmed = Clock::now();
            if (errors == 0) {
                if (native) {
                    // Independent socket control: no Galay scheduling or awaiter.
                    for (size_t i = 0; i != width; ++i) {
                        sockaddr_storage peer{};
                        socklen_t size = sizeof(peer);
                        const int accepted = ::accept4(listener, reinterpret_cast<sockaddr*>(&peer), &size,
                                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if (accepted < 0) { ++errors; break; }
                        batch.accepted[i] = accepted;
                        ++batch.completed;
                        if (latency) {
                            batch.latencies[i] = std::chrono::duration<double, std::micro>(Clock::now() - batch.starts[i]).count();
                        }
                    }
                } else if (!scheduleTask(owner, acceptBatch(controller, batch)) || !owner.dispatch()) {
                    ++errors;
                }
            }
        }
        // 只在同步 driver poll。以时间限制 setup 故障，不能把空 poll 次数
        // 当成超时：本机 10000 次仅约 5 ms，会提前中断尚未完成的连接。
        const auto deadline = begin + std::chrono::seconds(2);
        unsigned passes = 0;
        for (; !components && batch.completed != width && batch.errors == 0 && Clock::now() < deadline; ++passes) {
            if (!owner.poll()) { std::cerr << "B41 poll failed\n"; ++errors; break; }
            if (!owner.dispatch()) { std::cerr << "B41 dispatch failed\n"; ++errors; break; }
        }
        const auto finished = Clock::now();
        const auto cpu_end = std::clock();
        if (cpu_begin == std::clock_t(-1) || cpu_end == std::clock_t(-1)) { ++errors; }
        const double seconds = std::chrono::duration<double>(finished - begin).count();
        errors += batch.errors + (batch.completed != width);
        if (errors != 0) {
            std::cerr << "B41 incomplete batch=" << width << " round=" << round
                      << " completed=" << batch.completed << " passes=" << passes
                      << " seconds=" << seconds << '\n';
            diagnoseTimeout(listener, clients, owner.registered(controller));
        }
        if (round >= warmup) {
            elapsed += seconds;
            cpu_ns += 1e9 * static_cast<double>(cpu_end - cpu_begin) / CLOCKS_PER_SEC;
            if (components) {
                connect_ns += std::chrono::duration<double, std::nano>(connected - begin).count();
                confirm_ns += std::chrono::duration<double, std::nano>(confirmed - connected).count();
                accept_ns += std::chrono::duration<double, std::nano>(finished - confirmed).count();
            }
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
    if (components) {
        std::cout << std::fixed << std::setprecision(3)
                  << "B41Components implementation=" << (native ? "socket" : "galay")
                  << " batch=" << width << " operations=" << samples.size()
                  << " connect_submit_ns_per_op=" << connect_ns / samples.size()
                  << " ready_confirmation_ns_per_op=" << confirm_ns / samples.size()
                  << " ready_accept_ns_per_op=" << accept_ns / samples.size()
                  << " lifecycle_ns_per_op=" << elapsed * 1e9 / samples.size()
                  << " latency_observation=" << latency << " errors=" << errors << '\n';
        return errors == 0;
    }
    std::cout << std::fixed << std::setprecision(3)
              << "B41 backend="
#ifdef USE_EPOLL
              << "epoll"
#else
              << "io_uring"
#endif
              << " batch=" << width << " operations=" << samples.size()
              << " warmup_batches=" << warmup << " accepts_per_s=" << samples.size() / elapsed
              << " cpu_ns_per_op=" << cpu_ns / samples.size()
              << " p50_us=" << samples[samples.size() / 2]
              << " p99_us=" << samples[(samples.size() - 1) * 99 / 100]
              << " errors=" << errors << " awaiter_bytes=" << sizeof(AcceptAwaitable) << '\n';
    return errors == 0;
}
} // namespace
#endif

int main(int argc, char** argv) {
#if defined(USE_EPOLL) || defined(USE_IOURING)
    const bool components = argc > 1 && std::string_view(argv[1]) == "--components";
    const bool native = components && argc > 2 && std::string_view(argv[2]) == "socket";
    const bool latency = !(argc > 3 && std::string_view(argv[3]) == "--no-latency");
    if ((argc != 1 && !components) || argc > 4 ||
        (argc > 2 && !native && std::string_view(argv[2]) != "galay") ||
        (argc > 3 && latency)) { return 1; }
    const bool single = measure(1, 2048, components, native, latency);
    const bool burst = measure(64, 128, components, native, latency);
    return single && burst ? 0 : 1;
#else
    std::cout << "B41 SKIP (requires epoll or io_uring)\n";
    return 77;
#endif
}
