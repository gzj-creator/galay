/**
 * @file t73_connbridge.cc
 * @brief 用途：验证链式 AwaitableBuilder 的 connect 步骤已经桥接到共享状态机内核。
 * 关键覆盖点：`connect(...).finish(...).build()` 的真实调度、ConnectIOContext 回调结果、
 * 后续本地步骤继续推进，以及连接确实到达监听端。
 * 通过条件：connect handler 收到成功结果、finish 完成最终结果、服务端 accept 到连接。
 */

#include "kernel/common/host.hpp"
#include "kernel/kernel/awaitable.h"
#include "kernel/kernel/task.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef USE_IOURING
#include "kernel/kernel/uring_scheduler.h"
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include "kernel/kernel/epoll_scheduler.h"
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include "kernel/kernel/kqueue_scheduler.h"
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using ConnectResult = std::expected<size_t, IOError>;

struct ConnectFlow {
    void onConnect(SequenceOps<ConnectResult, 4>&, ConnectIOContext& connect_ctx) {
        connect_handler_called = true;
        connect_ok = connect_ctx.m_result.has_value();
    }

    void onFinish(SequenceOps<ConnectResult, 4>& ops) {
        finish_handler_called = true;
        ops.complete((connect_handler_called && connect_ok) ? 1u : 0u);
    }

    bool connect_handler_called = false;
    bool connect_ok = false;
    bool finish_handler_called = false;
};

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
};

bool waitUntil(const std::atomic<bool>& flag,
               std::chrono::milliseconds timeout = 1000ms,
               std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return flag.load(std::memory_order_acquire);
}

int createListenSocket(uint16_t* port_out) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    const int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 8) != 0) {
        close(listen_fd);
        return -1;
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
        close(listen_fd);
        return -1;
    }

    *port_out = ntohs(bound.sin_port);
    return listen_fd;
}

void acceptWithTimeout(int listen_fd,
                       std::atomic<bool>* accepted,
                       std::chrono::milliseconds timeout = 1000ms,
                       std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            accepted->store(true, std::memory_order_release);
            close(client_fd);
            return;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return;
        }
        std::this_thread::sleep_for(step);
    }
}

Task<void> connectBuilderTask(TestState* state, int fd, Host host) {
    IOController controller(GHandle{.fd = fd});
    ConnectFlow flow;
    auto awaitable = AwaitableBuilder<ConnectResult, 4, ConnectFlow>(&controller, flow)
        .connect<&ConnectFlow::onConnect>(host)
        .finish<&ConnectFlow::onFinish>()
        .build();

    auto result = co_await awaitable;
    state->success.store(
        result.has_value() &&
            result.value() == 1 &&
            flow.connect_handler_called &&
            flow.connect_ok &&
            flow.finish_handler_called,
        std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

}  // namespace

int main() {
    uint16_t port = 0;
    int listen_fd = createListenSocket(&port);
    if (listen_fd < 0) {
        std::cerr << "[T73] createListenSocket failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    std::atomic<bool> accepted{false};
    std::thread accept_thread(acceptWithTimeout, listen_fd, &accepted, 1000ms, 2ms);

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        close(listen_fd);
        accept_thread.join();
        std::cerr << "[T73] client socket failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    const int client_flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, client_flags | O_NONBLOCK);

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    scheduleTask(
        scheduler,
        connectBuilderTask(&state, client_fd, Host(IPType::IPV4, "127.0.0.1", port))
    );

    const bool completed = waitUntil(state.done);
    const bool server_accepted = waitUntil(accepted);

    scheduler.stop();
    close(client_fd);
    close(listen_fd);
    accept_thread.join();

    if (!completed) {
        std::cerr << "[T73] builder connect timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T73] builder connect result mismatch\n";
        return 1;
    }
    if (!server_accepted) {
        std::cerr << "[T73] server did not observe the connection\n";
        return 1;
    }

    std::cout << "T73-AwaitableBuilderConnectBridge PASS\n";
    return 0;
}
