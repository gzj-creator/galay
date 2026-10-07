/**
 * @file bench_ssl_server.cc
 * @brief SSL 服务端性能测试
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <iostream>
#include <atomic>
#include <csignal>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <limits>
#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#endif

using namespace galay::ssl;
using namespace galay::kernel;

namespace {

std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_connections{0};
std::atomic<uint64_t> g_bytes_recv{0};
std::atomic<uint64_t> g_bytes_sent{0};

struct ServerStartupState {
    std::atomic<int> ready{0};
    std::atomic<int> failed{0};
};

bool configure_benchmark_tls_context(SslContext& ctx) {
    ctx.disable_session_cache();
    ctx.set_session_timeout(0);
    ctx.disable_session_tickets();
    return ctx.set_ciphersuites("TLS_AES_128_GCM_SHA256").has_value();
}

std::unique_ptr<SslContext> create_benchmark_server_context(const std::string& certFile,
                                                         const std::string& keyFile) {
    auto ctx = std::make_unique<SslContext>(SslMethod::TLS_1_3_Server);
    if (!ctx->is_valid()) {
        return nullptr;
    }

    if (!configure_benchmark_tls_context(*ctx)) {
        return nullptr;
    }

    auto certResult = ctx->load_certificate(certFile);
    if (!certResult) {
        return nullptr;
    }

    auto keyResult = ctx->load_private_key(keyFile);
    if (!keyResult) {
        return nullptr;
    }

    return ctx;
}

bool parse_int(const char* text, int minValue, int* value) {
    int parsed = 0;
    const char* end = text + std::char_traits<char>::length(text);
    auto result = std::from_chars(text, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed < minValue) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parse_port(const char* text, uint16_t* value) {
    int parsed = 0;
    if (!parse_int(text, 1, &parsed) || parsed > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    *value = static_cast<uint16_t>(parsed);
    return true;
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " <port> <cert_file> <key_file> [backlog] [worker_count]\n";
    std::cerr << "LONG_RUNNING: stop with SIGINT or SIGTERM after the matching client finishes.\n";
}

} // namespace

void log_errno(const char* prefix) {
    std::cerr << prefix << ": errno=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
}

void signal_handler(int) {
    g_running = false;
}

Task<void> handle_client(SslContext* ctx, GHandle handle) {
    SslSocket client(ctx, handle);
    client.option().handle_non_block();

    auto handshakeResult = co_await client.handshake();
    if (!handshakeResult) {
        co_await client.close();
        co_return;
    }

    g_connections++;

    char buffer[64 * 1024];
    while (g_running) {
        auto recvResult = co_await client.recv(buffer, sizeof(buffer));
        if (!recvResult) {
            break;
        }

        auto& bytes = recvResult.value();
        if (bytes.size() == 0) {
            break;  // 对端关闭
        }

        g_bytes_recv += bytes.size();

        // Echo 回去
        auto sendResult = co_await client.send(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!sendResult) {
            break;
        }
        g_bytes_sent += sendResult.value();
    }

    co_await client.shutdown();
    co_await client.close();
}

Task<void> ssl_server(IOScheduler* scheduler,
                     SslContext* ctx,
                     uint16_t port,
                     int backlog,
                     int workerIndex,
                     int workerCount,
                     ServerStartupState* startup) {
    SslSocket listener(ctx);

    if (!listener.is_valid()) {
        startup->failed.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    listener.option().handle_reuse_addr();
    if (workerCount > 1) {
        listener.option().handle_reuse_port();
    }
    listener.option().handle_non_block();

    auto bindResult = listener.bind(Host(IPType::IPV4, "0.0.0.0", port));
    if (!bindResult) {
        log_errno("bind failed");
        startup->failed.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    auto listenResult = listener.listen(backlog);
    if (!listenResult) {
        log_errno("listen failed");
        startup->failed.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    std::cout << "SSL Server worker " << (workerIndex + 1) << "/" << workerCount
              << " listening on port " << port << std::endl;
    startup->ready.fetch_add(1, std::memory_order_relaxed);

    while (g_running) {
        Host clientHost;
        auto acceptResult = co_await listener.accept(&clientHost);
        if (!acceptResult) {
            log_errno("accept failed");
            continue;
        }
        if (!schedule_task(scheduler, handle_client(ctx, acceptResult.value()))) {
            std::cerr << "spawn failed for client handler" << std::endl;
        }
    }

    co_await listener.close();
}

int main(int argc, char* argv[]) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    uint16_t port = 0;
    if (!parse_port(argv[1], &port)) {
        print_usage(argv[0]);
        return 1;
    }
    std::string certFile = argv[2];
    std::string keyFile = argv[3];
    int backlog = 4096;
    if (argc >= 5) {
        if (!parse_int(argv[4], 128, &backlog)) {
            print_usage(argv[0]);
            return 1;
        }
    }
    int workerCount = 1;
    if (argc >= 6) {
        if (!parse_int(argv[5], 1, &workerCount)) {
            print_usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    struct BenchWorker {
        std::unique_ptr<SslContext> ctx;
        std::unique_ptr<TestScheduler> scheduler;
    };

    std::vector<BenchWorker> workers;
    workers.reserve(workerCount);

    for (int i = 0; i < workerCount; ++i) {
        auto ctx = create_benchmark_server_context(certFile, keyFile);
        if (!ctx) {
            return 1;
        }

        workers.push_back(BenchWorker{
            .ctx = std::move(ctx),
            .scheduler = std::make_unique<TestScheduler>(),
        });
    }

    std::cout << "Starting SSL benchmark server on port " << port
              << " with " << workerCount << " worker(s)" << std::endl;

    ServerStartupState startup;
    int exit_code = 0;
    for (int i = 0; i < workerCount; ++i) {
        workers[static_cast<size_t>(i)].scheduler->start();
        if (!schedule_task(*workers[static_cast<size_t>(i)].scheduler,
                          ssl_server(workers[static_cast<size_t>(i)].scheduler.get(),
                                    workers[static_cast<size_t>(i)].ctx.get(),
                                    port,
                                    backlog,
                                    i,
                                    workerCount,
                                    &startup))) {
            startup.failed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const auto startupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (startup.ready.load(std::memory_order_relaxed) +
               startup.failed.load(std::memory_order_relaxed) < workerCount &&
           std::chrono::steady_clock::now() < startupDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (startup.failed.load(std::memory_order_relaxed) != 0 ||
        startup.ready.load(std::memory_order_relaxed) != workerCount) {
        std::cerr << "[FAIL] SSL benchmark server failed to start all workers"
                  << " ready=" << startup.ready.load(std::memory_order_relaxed)
                  << " failed=" << startup.failed.load(std::memory_order_relaxed)
                  << " expected=" << workerCount << std::endl;
        g_running = false;
        exit_code = 2;
    }

    while (exit_code == 0 && g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    for (auto it = workers.rbegin(); it != workers.rend(); ++it) {
        it->scheduler->stop();
    }

    std::cout << "\nFinal stats:" << std::endl;
    std::cout << "Total connections: " << g_connections << std::endl;
    std::cout << "Total bytes received: " << g_bytes_recv << std::endl;
    std::cout << "Total bytes sent: " << g_bytes_sent << std::endl;

    return exit_code;
}
