/**
 * @file t119_connbld.cc
 * @brief 用途：在同一 IO scheduler 下并发压测 `TcpSocket::connect` 与 `AwaitableBuilder::connect` 的混合路径。
 * 关键覆盖点：高并发 paired client/server 建连、传统 connect awaitable 与 sequence/state-machine connect 桥接并发共存。
 * 通过条件：所有建连成功、服务端 accept 计数完整、无 connect 超时/失败。
 */

#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/common/host.hpp"
#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/core/task.h"

#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

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
using galay::async::TcpSocket;
using namespace std::chrono_literals;

namespace {

constexpr uint16_t kPort = 19459;
constexpr int kClients = 512;
constexpr int kRoundsPerClient = 4;
constexpr int kTotalConnections = kClients * kRoundsPerClient;
constexpr auto kConnectTimeout = 4s;
constexpr auto kAcceptTimeout = 4s;

using BuilderResult = std::expected<uint8_t, IOError>;

struct TestState {
    std::atomic<bool> server_ready{false};
    std::atomic<bool> failed{false};
    std::atomic<int> accepted{0};
    std::atomic<int> client_done{0};
    std::atomic<int> plain_success{0};
    std::atomic<int> builder_success{0};
    std::atomic<int> connect_fail{0};
    std::atomic<int> timeout_fail{0};
    std::mutex failure_mu;
    std::string failure;
};

void fail(TestState* state, std::string message) {
    state->failed.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(state->failure_mu);
    if (state->failure.empty()) {
        state->failure = std::move(message);
    }
}

void recordConnectFailure(TestState* state,
                          const char* mode,
                          int round,
                          const IOError& error) {
    if (IOError::contains(error.code(), kTimeout)) {
        state->timeout_fail.fetch_add(1, std::memory_order_relaxed);
    } else {
        state->connect_fail.fetch_add(1, std::memory_order_relaxed);
    }
    fail(state,
         std::string(mode) + " connect round=" + std::to_string(round) +
             " failed: " + error.message());
}

struct BuilderConnectFlow {
    void onConnect(SequenceOps<BuilderResult, 4>& ops, ConnectIOContext& connect_ctx) {
        if (connect_ctx.m_result.has_value()) {
            ops.complete(static_cast<uint8_t>(1));
            return;
        }
        ops.complete(std::unexpected(connect_ctx.m_result.error()));
    }
};

Task<void> runServer(TestState* state) {
    TcpSocket listener(IPType::IPV4);
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

    auto bind_result = listener.bind(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!bind_result) {
        fail(state, "bind failed: " + bind_result.error().message());
        co_return;
    }
    auto listen_result = listener.listen(1024);
    if (!listen_result) {
        fail(state, "listen failed: " + listen_result.error().message());
        co_return;
    }

    state->server_ready.store(true, std::memory_order_release);

    for (int i = 0; i < kTotalConnections; ++i) {
        Host client_host;
        auto accepted = co_await listener.accept(&client_host).timeout(kAcceptTimeout);
        if (!accepted) {
            fail(state, "accept failed at index=" + std::to_string(i) +
                            ": " + accepted.error().message());
            break;
        }
        state->accepted.fetch_add(1, std::memory_order_relaxed);

        TcpSocket peer(std::move(accepted.value()));
        (void)co_await peer.close();
    }

    (void)co_await listener.close();
}

Task<void> runPlainClient(TestState* state) {
    const Host target(IPType::IPV4, "127.0.0.1", kPort);
    for (int round = 0; round < kRoundsPerClient; ++round) {
        TcpSocket socket(IPType::IPV4);
        socket.option().handleNonBlock();

        auto connected = co_await socket.connect(target).timeout(kConnectTimeout);
        if (!connected) {
            recordConnectFailure(state, "plain", round, connected.error());
            (void)co_await socket.close();
            continue;
        }

        state->plain_success.fetch_add(1, std::memory_order_relaxed);
        (void)co_await socket.close();
    }

    state->client_done.fetch_add(1, std::memory_order_relaxed);
}

Task<void> runBuilderClient(TestState* state) {
    const Host target(IPType::IPV4, "127.0.0.1", kPort);
    for (int round = 0; round < kRoundsPerClient; ++round) {
        TcpSocket socket(IPType::IPV4);
        socket.option().handleNonBlock();

        BuilderConnectFlow flow;
        auto awaitable = AwaitableBuilder<BuilderResult, 4, BuilderConnectFlow>(socket.controller(), flow)
                             .connect<&BuilderConnectFlow::onConnect>(target)
                             .build()
                             .timeout(kConnectTimeout);
        auto connected = co_await awaitable;
        if (!connected) {
            recordConnectFailure(state, "builder", round, connected.error());
            (void)co_await socket.close();
            continue;
        }
        if (connected.value() != 1) {
            state->connect_fail.fetch_add(1, std::memory_order_relaxed);
            fail(state, "builder connect returned unexpected value");
            (void)co_await socket.close();
            continue;
        }

        state->builder_success.fetch_add(1, std::memory_order_relaxed);
        (void)co_await socket.close();
    }

    state->client_done.fetch_add(1, std::memory_order_relaxed);
}

void waitForServerReady(TestState& state) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!state.server_ready.load(std::memory_order_acquire)) {
        if (state.failed.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state.failure_mu);
            throw std::runtime_error(state.failure.empty() ? "server setup failed" : state.failure);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("server did not become ready");
        }
        std::this_thread::sleep_for(5ms);
    }
}

void waitForClients(TestState& state) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (state.client_done.load(std::memory_order_relaxed) < kClients) {
        if (state.failed.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state.failure_mu);
            throw std::runtime_error(state.failure.empty() ? "connect stress failed" : state.failure);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "connect stress timed out [client_done=" +
                std::to_string(state.client_done.load(std::memory_order_relaxed)) +
                ", accepted=" + std::to_string(state.accepted.load(std::memory_order_relaxed)) +
                ", plain_success=" + std::to_string(state.plain_success.load(std::memory_order_relaxed)) +
                ", builder_success=" + std::to_string(state.builder_success.load(std::memory_order_relaxed)) +
                ", connect_fail=" + std::to_string(state.connect_fail.load(std::memory_order_relaxed)) +
                ", timeout_fail=" + std::to_string(state.timeout_fail.load(std::memory_order_relaxed)) + "]");
        }
        std::this_thread::sleep_for(10ms);
    }
}

}  // namespace

int main() {
    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    if (!scheduleTask(scheduler, runServer(&state))) {
        scheduler.stop();
        std::cerr << "[T119] schedule server failed\n";
        return 1;
    }

    int rc = 0;
    try {
        waitForServerReady(state);
        for (int i = 0; i < kClients; ++i) {
            const bool scheduled = (i % 2 == 0)
                ? scheduleTask(scheduler, runPlainClient(&state))
                : scheduleTask(scheduler, runBuilderClient(&state));
            if (!scheduled) {
                throw std::runtime_error("schedule client failed");
            }
        }
        waitForClients(state);
    } catch (const std::exception& ex) {
        std::cerr << "[T119] " << ex.what() << "\n";
        rc = 1;
    }

    scheduler.stop();

    if (rc != 0) {
        return rc;
    }
    if (state.failed.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(state.failure_mu);
        std::cerr << "[T119] failure: "
                  << (state.failure.empty() ? "connect stress failed" : state.failure)
                  << "\n";
        return 1;
    }

    const int plain_clients = (kClients + 1) / 2;
    const int builder_clients = kClients / 2;
    const int expected_plain_success = plain_clients * kRoundsPerClient;
    const int expected_builder_success = builder_clients * kRoundsPerClient;

    if (state.plain_success.load(std::memory_order_relaxed) != expected_plain_success ||
        state.builder_success.load(std::memory_order_relaxed) != expected_builder_success ||
        state.accepted.load(std::memory_order_relaxed) != kTotalConnections ||
        state.connect_fail.load(std::memory_order_relaxed) != 0 ||
        state.timeout_fail.load(std::memory_order_relaxed) != 0) {
        std::cerr << "[T119] count mismatch [accepted="
                  << state.accepted.load(std::memory_order_relaxed)
                  << ", plain_success=" << state.plain_success.load(std::memory_order_relaxed)
                  << ", builder_success=" << state.builder_success.load(std::memory_order_relaxed)
                  << ", connect_fail=" << state.connect_fail.load(std::memory_order_relaxed)
                  << ", timeout_fail=" << state.timeout_fail.load(std::memory_order_relaxed)
                  << "]\n";
        return 1;
    }

    std::cout << "T119-MixedConnectBuilderStress PASS"
              << " [accepted=" << state.accepted.load(std::memory_order_relaxed)
              << ", plain_success=" << state.plain_success.load(std::memory_order_relaxed)
              << ", builder_success=" << state.builder_success.load(std::memory_order_relaxed)
              << "]\n";
    return 0;
}
