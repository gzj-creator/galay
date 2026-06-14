/**
 * @file t118_seqxchg.cc
 * @brief 用途：锁定同一 IO scheduler 下多连接 builder sequence 的 send/recv 交换必须全部完成。
 * 关键覆盖点：`AwaitableBuilder` 线性 sequence 在并发 fanout 场景下的读写推进与完成收口。
 * 通过条件：服务端与客户端 sequence 全部完成，无超时。
 */

#include "kernel/kernel/task.h"
#include "kernel/kernel/io_scheduler.hpp"
#include "kernel/kernel/awaitable.h"
#include "kernel/common/host.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
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

namespace {

constexpr uint16_t kPort = 19450;
constexpr int kConnections = 16;

using BuilderResult = std::expected<size_t, IOError>;

struct TestState {
    std::atomic<bool> server_ready{false};
    std::atomic<int> accepted{0};
    std::atomic<int> connected{0};
    std::atomic<int> server_done{0};
    std::atomic<int> client_done{0};
    std::atomic<bool> failed{false};
    std::mutex failure_mu;
    std::string failure;
};

struct SendThenRecvFlow {
    void onSend(SequenceOps<BuilderResult, 4>&, SendIOContext& ctx) {
        send_ok = ctx.m_result.has_value();
    }

    void onRecv(SequenceOps<BuilderResult, 4>& ops, RecvIOContext& ctx) {
        recv_ok = ctx.m_result.has_value();
        ops.complete(std::move(ctx.m_result));
    }

    bool send_ok = false;
    bool recv_ok = false;
};

struct RecvThenSendFlow {
    explicit RecvThenSendFlow(const char* reply)
        : reply(reply) {}

    void onRecv(SequenceOps<BuilderResult, 4>&, RecvIOContext& ctx) {
        recv_ok = ctx.m_result.has_value();
    }

    void onSend(SequenceOps<BuilderResult, 4>& ops, SendIOContext& ctx) {
        send_ok = ctx.m_result.has_value();
        ops.complete(send_ok ? static_cast<size_t>(1) : static_cast<size_t>(0));
    }

    const char* reply = nullptr;
    bool recv_ok = false;
    bool send_ok = false;
};

void fail(TestState* state, std::string message)
{
    state->failed.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(state->failure_mu);
    if (state->failure.empty()) {
        state->failure = std::move(message);
    }
}

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Task<void> handleAcceptedClient(GHandle handle, TestState* state, int id)
{
    int client_fd = handle.fd;
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    IOController controller(GHandle{.fd = client_fd});
    char recv_buffer[32]{};
    std::string reply(1, static_cast<char>('a' + (id % 23)));
    RecvThenSendFlow flow(reply.c_str());
    auto exchange = AwaitableBuilder<BuilderResult, 4, RecvThenSendFlow>(&controller, flow)
        .recv<&RecvThenSendFlow::onRecv>(recv_buffer, sizeof(recv_buffer))
        .send<&RecvThenSendFlow::onSend>(reply.c_str(), reply.size())
        .build();

    auto result = co_await exchange;
    if (!result || !flow.recv_ok || !flow.send_ok) {
        fail(state, "server exchange failed");
    }

    ::close(client_fd);
    state->server_done.fetch_add(1, std::memory_order_relaxed);
}

Task<void> runServer(IOScheduler* scheduler, int listen_fd, TestState* state)
{
    IOController listen_ctrl(GHandle{.fd = listen_fd});
    state->server_ready.store(true, std::memory_order_release);

    for (int i = 0; i < kConnections; ++i) {
        Host client_host;
        AcceptAwaitable accept_awaitable(&listen_ctrl, &client_host);
        auto accepted = co_await accept_awaitable;
        if (!accepted) {
            fail(state, "accept failed");
            break;
        }
        state->accepted.fetch_add(1, std::memory_order_relaxed);
        if (!scheduleTask(scheduler, handleAcceptedClient(*accepted, state, i))) {
            fail(state, "schedule accepted client failed");
            break;
        }
    }
}

Task<void> runClient(TestState* state, int id)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail(state, "client socket failed");
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    IOController controller(GHandle{.fd = fd});
    auto connected = co_await ConnectAwaitable(&controller, Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connected) {
        fail(state, "connect failed");
        ::close(fd);
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    state->connected.fetch_add(1, std::memory_order_relaxed);

    std::string greeting(1, static_cast<char>('A' + (id % 23)));
    char recv_buffer[32]{};
    SendThenRecvFlow flow;
    auto exchange = AwaitableBuilder<BuilderResult, 4, SendThenRecvFlow>(&controller, flow)
        .send<&SendThenRecvFlow::onSend>(greeting.c_str(), greeting.size())
        .recv<&SendThenRecvFlow::onRecv>(recv_buffer, sizeof(recv_buffer))
        .build();

    auto result = co_await exchange;
    if (!result || !flow.send_ok || !flow.recv_ok || result.value() != 1) {
        fail(state, "client exchange failed");
    }

    ::close(fd);
    state->client_done.fetch_add(1, std::memory_order_relaxed);
}

void waitFor(std::atomic<bool>& flag, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void waitForCompletion(TestState& state)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (state.client_done.load(std::memory_order_relaxed) < kConnections ||
           state.server_done.load(std::memory_order_relaxed) < kConnections) {
        if (state.failed.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state.failure_mu);
            throw std::runtime_error(state.failure.empty() ? "sequence fanout failed" : state.failure);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "sequence fanout timed out [accepted=" +
                std::to_string(state.accepted.load(std::memory_order_relaxed)) +
                ", connected=" + std::to_string(state.connected.load(std::memory_order_relaxed)) +
                ", client_done=" + std::to_string(state.client_done.load(std::memory_order_relaxed)) +
                ", server_done=" + std::to_string(state.server_done.load(std::memory_order_relaxed)) + "]"
            );
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

int main()
{
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "[T118] create listen socket failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(kPort);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[T118] bind failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        return 1;
    }
    if (::listen(listen_fd, 128) != 0) {
        std::cerr << "[T118] listen failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    expect(scheduleTask(scheduler, runServer(&scheduler, listen_fd, &state)), "schedule server failed");
    waitFor(state.server_ready, "server did not become ready");
    for (int i = 0; i < kConnections; ++i) {
        expect(scheduleTask(scheduler, runClient(&state, i)), "schedule client failed");
    }

    int rc = 0;
    try {
        waitForCompletion(state);
    } catch (const std::exception& ex) {
        std::cerr << "[T118] " << ex.what() << "\n";
        rc = 1;
    }

    scheduler.stop();
    ::close(listen_fd);

    if (rc != 0) {
        return rc;
    }

    std::cout << "T118-SequenceFanoutExchange PASS\n";
    return 0;
}
