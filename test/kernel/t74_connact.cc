/**
 * @file t74_connact.cc
 * @brief 用途：验证自定义状态机可以通过 waitConnect/onConnect 直接建模连接阶段。
 * 关键覆盖点：`MachineAction::waitConnect(...)`、`onConnect(...)`、状态机完成收口。
 * 通过条件：状态机连接成功、服务端 accept 到连接、最终结果正确。
 */

#include "galay-kernel/common/host.hpp"
#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/core/task.h"
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
#include "galay-kernel/core/uring_scheduler.h"
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include "galay-kernel/core/epoll_scheduler.h"
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include "galay-kernel/core/kqueue_scheduler.h"
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using ConnectResult = std::expected<size_t, IOError>;

struct ConnectMachine {
    using result_type = ConnectResult;

    explicit ConnectMachine(Host host)
        : m_host(std::move(host)) {}

    MachineAction<result_type> advance() {
        switch (m_state) {
        case State::kInit:
            m_state = State::kWaiting;
            return MachineAction<result_type>::waitConnect(m_host);
        case State::kWaiting:
            return MachineAction<result_type>::continue_();
        case State::kDone:
            return MachineAction<result_type>::complete(m_connect_ok ? 1u : 0u);
        }
        return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
    }

    void onRead(std::expected<size_t, IOError>) {}

    void onWrite(std::expected<size_t, IOError>) {}

    void onConnect(std::expected<void, IOError> result) {
        m_connect_ok = result.has_value();
        m_state = State::kDone;
    }

private:
    enum class State {
        kInit,
        kWaiting,
        kDone,
    };

    Host m_host;
    State m_state = State::kInit;
    bool m_connect_ok = false;
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

Task<void> stateMachineConnectTask(TestState* state, int fd, Host host) {
    IOController controller(GHandle{.fd = fd});
    auto awaitable = AwaitableBuilder<ConnectResult>::fromStateMachine(
        &controller,
        ConnectMachine(std::move(host))
    ).build();

    auto result = co_await awaitable;
    state->success.store(result.has_value() && result.value() == 1, std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

}  // namespace

int main() {
    uint16_t port = 0;
    int listen_fd = createListenSocket(&port);
    if (listen_fd < 0) {
        std::cerr << "[T74] createListenSocket failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    std::atomic<bool> accepted{false};
    std::thread accept_thread(acceptWithTimeout, listen_fd, &accepted, 1000ms, 2ms);

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        close(listen_fd);
        accept_thread.join();
        std::cerr << "[T74] client socket failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    const int client_flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, client_flags | O_NONBLOCK);

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    scheduleTask(
        scheduler,
        stateMachineConnectTask(&state, client_fd, Host(IPType::IPV4, "127.0.0.1", port))
    );

    const bool completed = waitUntil(state.done);
    const bool server_accepted = waitUntil(accepted);

    scheduler.stop();
    close(client_fd);
    close(listen_fd);
    accept_thread.join();

    if (!completed) {
        std::cerr << "[T74] state machine connect timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T74] state machine connect result mismatch\n";
        return 1;
    }
    if (!server_accepted) {
        std::cerr << "[T74] server did not observe the connection\n";
        return 1;
    }

    std::cout << "T74-StateMachineConnectAction PASS\n";
    return 0;
}
