/**
 * @file t138_c_coro_tcp_timer_cleanup.cc
 * @brief 验证 C coroutine TCP bridge 在 timeout timer 注册失败后清理 reactor 状态。
 */

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/c/galay-bridge-c/coro-c/c_coro_tcp_bridge.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/timer_scheduler.h>

#include "test/cpp/common/sched_access.h"

#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

#ifdef USE_KQUEUE
#include <sys/event.h>
#endif

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::async;
using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

struct ManualWaitState {
    std::atomic<int> completed{0};
    std::atomic<int> released{0};
    GalayCoreCoroIOResult result{GalayCoreCoroIOResultError, 0, 0, 0, nullptr};
};

GalayCoreCoroIOResult manualWait(void* ctx, int64_t)
{
    auto* state = static_cast<ManualWaitState*>(ctx);
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < deadline) {
        if (state->completed.load(std::memory_order_acquire) != 0) {
            return state->result;
        }
        std::this_thread::sleep_for(1ms);
    }
    return GalayCoreCoroIOResult{GalayCoreCoroIOResultTimeout, 0, 0, 0, nullptr};
}

GalayCoreCoroIOResult completeUserData(void* user_data, GalayCoreCoroIOResult result)
{
    auto* state = static_cast<ManualWaitState*>(user_data);
    state->result = result;
    state->completed.fetch_add(1, std::memory_order_release);
    return GalayCoreCoroIOResult{GalayCoreCoroIOResultOk, 0, 0, 0, nullptr};
}

GalayCoreCoroIOResult releaseUserData(void* user_data)
{
    auto* state = static_cast<ManualWaitState*>(user_data);
    state->released.fetch_add(1, std::memory_order_release);
    return GalayCoreCoroIOResult{GalayCoreCoroIOResultOk, 0, 0, 0, nullptr};
}

GalayCoreCoroWaitOps makeWaitOps(ManualWaitState* state)
{
    return GalayCoreCoroWaitOps{
        manualWait,
        completeUserData,
        releaseUserData,
        state,
    };
}

GalayCoreTcpSocket* toCoreSocket(TcpSocket* socket)
{
    return reinterpret_cast<GalayCoreTcpSocket*>(socket);
}

GalayCoreIOScheduler* toCoreScheduler(Scheduler* scheduler)
{
    return reinterpret_cast<GalayCoreIOScheduler*>(scheduler);
}

int connectPosixClient(uint16_t port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool createLoopbackPair(TcpSocket* listener, TcpSocket* accepted, int* client_fd)
{
    auto bound = listener->bind(Host(IPType::IPV4, "127.0.0.1", 0));
    if (!bound.has_value()) {
        std::cerr << "[T138] bind failed\n";
        return false;
    }
    auto listening = listener->listen(8);
    if (!listening.has_value()) {
        std::cerr << "[T138] listen failed\n";
        return false;
    }

    sockaddr_in local{};
    socklen_t local_len = sizeof(local);
    if (::getsockname(listener->handle().fd,
                      reinterpret_cast<sockaddr*>(&local),
                      &local_len) != 0) {
        std::cerr << "[T138] getsockname failed\n";
        return false;
    }

    *client_fd = connectPosixClient(ntohs(local.sin_port));
    if (*client_fd < 0) {
        std::cerr << "[T138] posix client connect failed\n";
        return false;
    }

    int server_fd = ::accept(listener->handle().fd, nullptr, nullptr);
    if (server_fd < 0) {
        std::cerr << "[T138] accept failed\n";
        return false;
    }
    *accepted = TcpSocket(GHandle{.fd = server_fd});
    auto non_block = accepted->option().handleNonBlock();
    if (!non_block.has_value()) {
        std::cerr << "[T138] server nonblock failed\n";
        return false;
    }
    return true;
}

#ifdef USE_KQUEUE
bool verifyBackendRegistrationWasRemoved(Scheduler* scheduler, TcpSocket& socket)
{
    auto* kqueue_scheduler = dynamic_cast<KqueueScheduler*>(scheduler);
    if (kqueue_scheduler == nullptr) {
        return false;
    }

    struct kevent delete_event;
    EV_SET(&delete_event, socket.handle().fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    errno = 0;
    const int deleted = kevent(SchedulerTestAccess::kqueueFd(*kqueue_scheduler),
                               &delete_event,
                               1,
                               nullptr,
                               0,
                               nullptr);
    if (deleted == -1 && errno == ENOENT) {
        return true;
    }
    std::cerr << "[T138] timer failure left a kqueue read registration"
              << ", delete_result=" << deleted
              << ", errno=" << errno
              << "\n";
    return false;
}
#elif defined(USE_EPOLL)
bool verifyBackendRegistrationWasRemoved(Scheduler* scheduler, TcpSocket& socket)
{
    auto* epoll_scheduler = dynamic_cast<EpollScheduler*>(scheduler);
    if (epoll_scheduler == nullptr) {
        return false;
    }
    (void)SchedulerTestAccess::flushReactor(*epoll_scheduler);
    if (socket.controller()->m_registered_events == 0) {
        return true;
    }
    std::cerr << "[T138] timer failure left epoll events registered: "
              << socket.controller()->m_registered_events << "\n";
    return false;
}
#else
bool verifyBackendRegistrationWasRemoved(Scheduler*, TcpSocket&)
{
    return true;
}
#endif

bool verifyTimerAddFailureLeavesSocketReusable()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    auto started = runtime.start();
    if (!started.has_value()) {
        std::cerr << "[T138] runtime start failed\n";
        return false;
    }
    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        std::cerr << "[T138] missing IO scheduler\n";
        runtime.stop();
        return false;
    }

    TcpSocket listener;
    TcpSocket server;
    int client_fd = -1;
    if (!createLoopbackPair(&listener, &server, &client_fd)) {
        runtime.stop();
        if (client_fd >= 0) {
            ::close(client_fd);
        }
        return false;
    }

    TimerScheduler::getInstance()->stop();

    char failed_buffer = 0;
    ManualWaitState failed_wait;
    GalayCoreCoroWaitOps failed_ops = makeWaitOps(&failed_wait);
    GalayCoreCoroIOResult failed = galay_core_coro_tcp_recv(toCoreSocket(&server),
                                                            toCoreScheduler(scheduler),
                                                            &failed_buffer,
                                                            1,
                                                            1000,
                                                            &failed_wait,
                                                            &failed_ops);
    if (failed.code != GalayCoreCoroIOResultError ||
        failed_wait.completed.load(std::memory_order_acquire) != 1 ||
        failed_wait.released.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T138] timer add failure should complete and release user_data"
                  << ", code=" << static_cast<int>(failed.code)
                  << ", completed=" << failed_wait.completed.load(std::memory_order_acquire)
                  << ", released=" << failed_wait.released.load(std::memory_order_acquire)
                  << "\n";
        ::close(client_fd);
        runtime.stop();
        return false;
    }

    if (!verifyBackendRegistrationWasRemoved(scheduler, server)) {
        ::close(client_fd);
        runtime.stop();
        return false;
    }

    if (::write(client_fd, "x", 1) != 1) {
        std::cerr << "[T138] client write failed\n";
        ::close(client_fd);
        runtime.stop();
        return false;
    }

    char ok_buffer = 0;
    ManualWaitState ok_wait;
    GalayCoreCoroWaitOps ok_ops = makeWaitOps(&ok_wait);
    GalayCoreCoroIOResult ok = galay_core_coro_tcp_recv(toCoreSocket(&server),
                                                        toCoreScheduler(scheduler),
                                                        &ok_buffer,
                                                        1,
                                                        -1,
                                                        &ok_wait,
                                                        &ok_ops);
    if (ok.code != GalayCoreCoroIOResultOk || ok.bytes != 1 || ok_buffer != 'x' ||
        ok_wait.completed.load(std::memory_order_acquire) != 1 ||
        ok_wait.released.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T138] socket should be reusable after timer failure"
                  << ", code=" << static_cast<int>(ok.code)
                  << ", bytes=" << ok.bytes
                  << ", byte=" << ok_buffer
                  << ", completed=" << ok_wait.completed.load(std::memory_order_acquire)
                  << ", released=" << ok_wait.released.load(std::memory_order_acquire)
                  << "\n";
        ::close(client_fd);
        runtime.stop();
        return false;
    }

    if (server.controller()->m_awaitable[IOController::READ] != nullptr ||
        server.controller()->m_awaitable[IOController::WRITE] != nullptr ||
        server.controller()->m_owner_scheduler.load(std::memory_order_acquire) != nullptr) {
        std::cerr << "[T138] controller state should be clear after reuse\n";
        ::close(client_fd);
        runtime.stop();
        return false;
    }

    ::close(client_fd);
    runtime.stop();
    return true;
}

} // namespace

int main()
{
    if (!verifyTimerAddFailureLeavesSocketReusable()) {
        TimerScheduler::getInstance()->start();
        return 1;
    }
    TimerScheduler::getInstance()->start();
    return 0;
}
