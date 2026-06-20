/**
 * @file t56_seqawait.cc
 * @brief 用途：验证 AwaitableBuilder 能按预期完成线性多步挂起与恢复。
 * 关键覆盖点：builder 直接声明标准步骤、连续挂起恢复、最终结果汇总。
 * 通过条件：builder 组合流程全部按预期执行，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/common/host.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

using namespace galay::kernel;

namespace {

std::atomic<bool> g_server_ok{false};
std::atomic<bool> g_client_ok{false};

using BuilderResult = std::expected<size_t, IOError>;

struct BuilderFlow {
    void onSend(SequenceOps<BuilderResult, 4>&, SendIOContext& send_ctx) {
        send_ok = send_ctx.m_result.has_value();
    }

    void onRecv(SequenceOps<BuilderResult, 4>& ops, RecvIOContext& recv_ctx) {
        ops.complete(std::move(recv_ctx.m_result));
    }

    bool send_ok = false;
};

Task<void> serverTask(int listen_fd) {
    IOController listen_ctrl(GHandle{.fd = listen_fd});
    Host client_host;
    AcceptAwaitable accept_awaitable(&listen_ctrl, &client_host);
    auto accepted = co_await accept_awaitable;
    if (!accepted) {
        co_return;
    }

    int client_fd = accepted->fd;
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    IOController controller(GHandle{.fd = client_fd});
    char recv_buffer[64]{};
    const std::string greeting = "hello";
    BuilderFlow flow;
    auto exchange = AwaitableBuilder<BuilderResult, 4, BuilderFlow>(&controller, flow)
        .send<&BuilderFlow::onSend>(greeting.c_str(), greeting.size())
        .recv<&BuilderFlow::onRecv>(recv_buffer, sizeof(recv_buffer) - 1)
        .build();

    auto received = co_await exchange;
    if (flow.send_ok && received) {
        const std::string reply(recv_buffer, received.value());
        if (reply == "world") {
            g_server_ok.store(true, std::memory_order_release);
        }
    }

    close(client_fd);
}

Task<void> clientTask(const char* ip, int port) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        co_return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    IOController controller(GHandle{.fd = fd});
    ConnectAwaitable connect_awaitable(&controller, Host(IPType::IPV4, ip, port));
    auto connected = co_await connect_awaitable;
    if (!connected) {
        close(fd);
        co_return;
    }

    char buffer[64]{};
    RecvAwaitable recv_awaitable(&controller, buffer, sizeof(buffer) - 1);
    auto received = co_await recv_awaitable;
    if (!received) {
        close(fd);
        co_return;
    }

    const std::string reply = "world";
    SendAwaitable send_awaitable(&controller, reply.c_str(), reply.size());
    auto sent = co_await send_awaitable;
    if (sent && std::string(buffer, received.value()) == "hello") {
        g_client_ok.store(true, std::memory_order_release);
    }

    close(fd);
}

}  // namespace

int main() {
    const int port = 20063;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "[T56] failed to create listen socket\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[T56] bind failed: " << std::strerror(errno) << '\n';
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 128) < 0) {
        std::cerr << "[T56] listen failed: " << std::strerror(errno) << '\n';
        close(listen_fd);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();
    scheduleTask(scheduler, serverTask(listen_fd));
    scheduleTask(scheduler, clientTask("127.0.0.1", port));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    scheduler.stop();
    close(listen_fd);

    if (!g_server_ok.load(std::memory_order_acquire) ||
        !g_client_ok.load(std::memory_order_acquire)) {
        std::cerr << "[T56] expected AwaitableBuilder exchange to succeed\n";
        return 1;
    }

    std::cout << "T56-SequenceBuilder PASS\n";
    return 0;
}
