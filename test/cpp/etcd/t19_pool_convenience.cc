/**
 * @file t19_pool_convenience.cc
 * @brief 验证同步与异步 cluster pool 的自动连接及高阶调用接口。
 */

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <galay/cpp/galay-etcd/async/client.h>
#include <galay/cpp/galay-etcd/cluster/etcd_cluster_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

namespace
{

int fail(const std::string& message)
{
    std::cerr << "[FAIL] " << message << '\n';
    return 1;
}

class LoopbackListener
{
public:
    LoopbackListener()
    {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) {
            std::cerr << "[FAIL] socket failed: " << std::strerror(errno) << '\n';
            std::abort();
        }

        int reuse = 1;
        if (::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            std::cerr << "[FAIL] setsockopt(SO_REUSEADDR) failed: "
                      << std::strerror(errno) << '\n';
            std::abort();
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        const int parsed = ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (parsed != 1) {
            std::cerr << "[FAIL] inet_pton failed\n";
            std::abort();
        }
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            std::cerr << "[FAIL] bind failed: " << std::strerror(errno) << '\n';
            std::abort();
        }

        socklen_t address_size = sizeof(address);
        if (::getsockname(
                m_fd,
                reinterpret_cast<sockaddr*>(&address),
                &address_size) != 0) {
            std::cerr << "[FAIL] getsockname failed: " << std::strerror(errno) << '\n';
            std::abort();
        }
        m_port = ntohs(address.sin_port);

        if (::listen(m_fd, 16) != 0) {
            std::cerr << "[FAIL] listen failed: " << std::strerror(errno) << '\n';
            std::abort();
        }
    }

    ~LoopbackListener()
    {
        if (m_fd >= 0 && ::close(m_fd) != 0) {
            std::cerr << "[FAIL] close listener failed: " << std::strerror(errno) << '\n';
        }
    }

    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    [[nodiscard]] uint16_t port() const noexcept
    {
        return m_port;
    }

private:
    int m_fd = -1;
    uint16_t m_port = 0;
};

std::string endpointFor(uint16_t port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

galay::etcd::EtcdProductionConfig makeProduction(uint16_t port)
{
    galay::etcd::EtcdProductionConfig production;
    production.endpoints = {endpointFor(port)};
    production.connections_per_endpoint = 1;
    return production;
}

int runSyncCases(uint16_t port)
{
    using galay::etcd::EtcdError;
    using galay::etcd::EtcdErrorType;

    auto pool = galay::etcd::EtcdClusterClientBuilder()
        .productionConfig(makeProduction(port))
        .build();

    auto connected = pool.acquireConnected();
    if (!connected.has_value() || !connected->get()->connected()) {
        return fail("sync acquireConnected should return a connected lease");
    }
    if (pool.idleCount() != 0) {
        return fail("sync acquireConnected should borrow one client");
    }
    connected->release();

    auto happy = pool.withClient([](galay::etcd::EtcdClient& client)
        -> std::expected<int, EtcdError> {
        if (!client.connected()) {
            return std::unexpected(
                EtcdError(EtcdErrorType::Internal, "sync callback received disconnected client"));
        }
        return 42;
    });
    if (!happy.has_value() || *happy != 42) {
        return fail("sync withClient should return the callback result");
    }
    if (pool.idleCount() != pool.size()) {
        return fail("sync withClient should return the lease after success");
    }

    auto callback_error = pool.withClient([](galay::etcd::EtcdClient&)
        -> std::expected<int, EtcdError> {
        return std::unexpected(
            EtcdError(EtcdErrorType::Server, "sync callback failed"));
    });
    if (callback_error.has_value() ||
        callback_error.error().type() != EtcdErrorType::Server) {
        return fail("sync withClient should preserve callback errors");
    }
    if (pool.idleCount() != pool.size()) {
        return fail("sync withClient should return the lease after callback error");
    }

    auto held = pool.tryAcquire();
    if (!held.has_value()) {
        return fail("sync pool setup should acquire its only client");
    }
    auto exhausted = pool.withClient([](galay::etcd::EtcdClient&)
        -> std::expected<int, EtcdError> {
        return 7;
    });
    if (exhausted.has_value() ||
        exhausted.error().type() != EtcdErrorType::PoolExhausted) {
        return fail("sync withClient should report PoolExhausted");
    }
    held->release();
    if (pool.idleCount() != pool.size()) {
        return fail("sync pool should recover after exhaustion case");
    }

    return 0;
}

galay::kernel::Task<void> runAsyncCases(
    galay::kernel::IOScheduler* scheduler,
    uint16_t port,
    std::atomic<bool>* done,
    int* exit_code)
{
    using galay::etcd::AsyncEtcdClient;
    using galay::etcd::EtcdBoolResult;
    using galay::etcd::EtcdError;
    using galay::etcd::EtcdErrorType;

    auto finish = [&](int code) {
        *exit_code = code;
        done->store(true, std::memory_order_release);
    };

    auto pool = galay::etcd::AsyncEtcdClusterClientBuilder()
        .scheduler(scheduler)
        .productionConfig(makeProduction(port))
        .build();

    auto connected_task = co_await pool.acquireConnected();
    if (!connected_task.has_value()) {
        finish(fail("async acquireConnected task failed: " +
                    std::string(connected_task.error().message())));
        co_return;
    }
    auto connected = std::move(*connected_task);
    if (!connected.has_value() || !connected->get()->connected()) {
        finish(fail("async acquireConnected should return a connected lease"));
        co_return;
    }
    if (pool.idleCount() != 0) {
        finish(fail("async acquireConnected should borrow one client"));
        co_return;
    }
    connected->release();

    auto happy_task = co_await pool.withClient([](AsyncEtcdClient& client)
        -> galay::kernel::Task<std::expected<int, EtcdError>> {
        if (!client.connected()) {
            co_return std::unexpected(
                EtcdError(EtcdErrorType::Internal, "async callback received disconnected client"));
        }
        co_return 84;
    });
    if (!happy_task.has_value()) {
        finish(fail("async withClient task failed: " +
                    std::string(happy_task.error().message())));
        co_return;
    }
    auto happy = std::move(*happy_task);
    if (!happy.has_value() || *happy != 84) {
        finish(fail("async withClient should return the callback result"));
        co_return;
    }
    if (pool.idleCount() != pool.size()) {
        finish(fail("async withClient should return the lease after success"));
        co_return;
    }

    auto callback_error_task = co_await pool.withClient([](AsyncEtcdClient&)
        -> galay::kernel::Task<std::expected<int, EtcdError>> {
        co_return std::unexpected(
            EtcdError(EtcdErrorType::Server, "async callback failed"));
    });
    if (!callback_error_task.has_value()) {
        finish(fail("async callback-error task failed: " +
                    std::string(callback_error_task.error().message())));
        co_return;
    }
    auto callback_error = std::move(*callback_error_task);
    if (callback_error.has_value() ||
        callback_error.error().type() != EtcdErrorType::Server) {
        finish(fail("async withClient should preserve callback errors"));
        co_return;
    }
    if (pool.idleCount() != pool.size()) {
        finish(fail("async withClient should return the lease after callback error"));
        co_return;
    }

    auto task_error = co_await pool.withClient([](AsyncEtcdClient&)
        -> galay::kernel::Task<EtcdBoolResult> {
        return {};
    });
    if (!task_error.has_value()) {
        finish(fail("outer async withClient task should complete normally"));
        co_return;
    }
    if (task_error->has_value() ||
        task_error->error().type() != EtcdErrorType::Internal ||
        task_error->error().message().find("task state is invalid") == std::string::npos) {
        finish(fail("async withClient should map TaskResultError to Internal with its message"));
        co_return;
    }
    if (pool.idleCount() != pool.size()) {
        finish(fail("async withClient should return the lease after task failure"));
        co_return;
    }

    auto held = pool.tryAcquire();
    if (!held.has_value()) {
        finish(fail("async pool setup should acquire its only client"));
        co_return;
    }
    auto exhausted_task = co_await pool.withClient([](AsyncEtcdClient&)
        -> galay::kernel::Task<std::expected<int, EtcdError>> {
        co_return 9;
    });
    if (!exhausted_task.has_value()) {
        finish(fail("async exhaustion task failed: " +
                    std::string(exhausted_task.error().message())));
        co_return;
    }
    auto exhausted = std::move(*exhausted_task);
    if (exhausted.has_value() ||
        exhausted.error().type() != EtcdErrorType::PoolExhausted) {
        finish(fail("async withClient should report PoolExhausted"));
        co_return;
    }
    held->release();
    if (pool.idleCount() != pool.size()) {
        finish(fail("async pool should recover after exhaustion case"));
        co_return;
    }

    finish(0);
}

} // namespace

int main()
{
    LoopbackListener sync_listener;
    if (const int sync_result = runSyncCases(sync_listener.port()); sync_result != 0) {
        return sync_result;
    }

    LoopbackListener async_listener;
    galay::kernel::Runtime runtime = galay::kernel::RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    auto start_result = runtime.start();
    if (!start_result.has_value()) {
        return fail("runtime start failed");
    }
    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        return fail("runtime did not provide an IO scheduler");
    }

    std::atomic<bool> done{false};
    int exit_code = 1;
    const bool scheduled = galay::kernel::scheduleTask(
        scheduler,
        runAsyncCases(scheduler, async_listener.port(), &done, &exit_code));
    if (!scheduled) {
        runtime.stop();
        return fail("failed to schedule async pool convenience task");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!done.load(std::memory_order_acquire)) {
        exit_code = fail("async pool convenience test timed out");
    }

    runtime.stop();
    if (exit_code == 0) {
        std::cout << "ETCD POOL CONVENIENCE TEST PASSED\n";
    }
    return exit_code;
}
