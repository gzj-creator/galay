/**
 * @file bench_ssl_client.cc
 * @brief SSL 客户端性能测试
 */

#include "galay-ssl/async/ssl_socket.h"
#include "galay-ssl/ssl/ssl_context.h"
#include "ssl_stats.h"
#include <galay-kernel/core/task.h>
#include <iostream>
#include <atomic>
#include <csignal>
#include <algorithm>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <string>

#ifdef USE_KQUEUE
#include <galay-kernel/core/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#elif defined(USE_IOURING)
#include <galay-kernel/core/io_uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include <galay-kernel/core/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#endif

using namespace galay::ssl;
using namespace galay::kernel;

namespace {

std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_requests{0};
std::atomic<uint64_t> g_bytes_sent{0};
std::atomic<uint64_t> g_bytes_recv{0};
std::atomic<uint64_t> g_errors{0};
std::atomic<uint64_t> g_connections_done{0};
std::atomic<uint64_t> g_connect_fail{0};
std::atomic<uint64_t> g_handshake_fail{0};
std::atomic<uint64_t> g_send_fail{0};
std::atomic<uint64_t> g_recv_fail{0};
std::atomic<uint64_t> g_peer_closed{0};

void configureBenchmarkTlsContext(SslContext& ctx) {
    ctx.disableSessionCache();
    ctx.setSessionTimeout(0);
    ctx.disableSessionTickets();
}

} // namespace

struct ThreadMetrics {
    uint64_t requests = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_recv = 0;
    uint64_t errors = 0;
    uint64_t connections_done = 0;
    uint64_t connect_fail = 0;
    uint64_t handshake_fail = 0;
    uint64_t send_fail = 0;
    uint64_t recv_fail = 0;
    uint64_t peer_closed = 0;
};

void mergeThreadMetrics(const ThreadMetrics& m) {
    g_requests.fetch_add(m.requests, std::memory_order_relaxed);
    g_bytes_sent.fetch_add(m.bytes_sent, std::memory_order_relaxed);
    g_bytes_recv.fetch_add(m.bytes_recv, std::memory_order_relaxed);
    g_errors.fetch_add(m.errors, std::memory_order_relaxed);
    g_connections_done.fetch_add(m.connections_done, std::memory_order_relaxed);
    g_connect_fail.fetch_add(m.connect_fail, std::memory_order_relaxed);
    g_handshake_fail.fetch_add(m.handshake_fail, std::memory_order_relaxed);
    g_send_fail.fetch_add(m.send_fail, std::memory_order_relaxed);
    g_recv_fail.fetch_add(m.recv_fail, std::memory_order_relaxed);
    g_peer_closed.fetch_add(m.peer_closed, std::memory_order_relaxed);
}

void signalHandler(int) {
    g_running = false;
}

Task<void> sslClient(SslContext* ctx,
                    const std::string& host, uint16_t port,
                    const std::string& message, int requestCount,
                    int connectRetries,
                    std::atomic<int>* thread_done,
                    ThreadMetrics* metrics,
                    bool statsEnabled) {
    SslSocket socket(ctx);

    if (!socket.isValid()) {
        metrics->errors += 1;
        metrics->connections_done += 1;
        if (thread_done) {
            (*thread_done)++;
        }
        co_return;
    }

    socket.option().handleNonBlock();

    // 设置 SNI
    socket.setHostname(host);

    // 连接（轻量重试，缓解高并发瞬时接入抖动）
    bool connected = false;
    for (int attempt = 0; attempt < connectRetries; ++attempt) {
        auto connectResult = co_await socket.connect(Host(IPType::IPV4, host, port));
        if (connectResult) {
            connected = true;
            break;
        }

        if (attempt + 1 >= connectRetries) {
            break;
        }

        co_await socket.close();
        socket = SslSocket(ctx);
        if (!socket.isValid()) {
            break;
        }
        socket.option().handleNonBlock();
        socket.setHostname(host);
    }

    if (!connected) {
        metrics->errors += 1;
        metrics->connect_fail += 1;
        co_await socket.close();
        metrics->connections_done += 1;
        if (thread_done) {
            (*thread_done)++;
        }
        co_return;
    }

    auto handshakeResult = co_await socket.handshake();
    if (!handshakeResult) {
        metrics->errors += 1;
        metrics->handshake_fail += 1;
        co_await socket.close();
        metrics->connections_done += 1;
        if (thread_done) {
            (*thread_done)++;
        }
        co_return;
    }

    std::vector<char> buffer(std::min<size_t>(64 * 1024, message.size()));

    for (int i = 0; i < requestCount && g_running; i++) {
        // 发送
        auto sendResult = co_await socket.send(message.c_str(), message.size());
        if (!sendResult) {
            metrics->errors += 1;
            metrics->send_fail += 1;
            break;
        }
        metrics->bytes_sent += sendResult.value();
        if (statsEnabled) {
            bench::sslStatsAddSend(sendResult.value());
        }

        // 接收 - echo 是字节流，可能被拆包；按发送长度累计读满
        size_t remaining = message.size();
        int recvLoops = 0;
        bool recvFailed = false;
        // 防止异常情况下无限等待（比如少读/漏读导致 remaining 永远不为 0）
        const int kMaxRecvLoops = 200000;
        while (remaining > 0 && recvLoops++ < kMaxRecvLoops) {
            auto recvLen = std::min(remaining, buffer.size());
            auto recvResult = co_await socket.recv(buffer.data(), recvLen);
            if (!recvResult) {
                recvFailed = true;
                metrics->recv_fail += 1;
                break;
            }
            if (recvResult.value().size() == 0) {
                recvFailed = true;
                metrics->peer_closed += 1;
                break;
            }
            const size_t received = recvResult.value().size();
            metrics->bytes_recv += received;
            if (statsEnabled) {
                bench::sslStatsAddRecv(received);
            }
            remaining -= received;
        }
        if (recvFailed || remaining != 0) {
            metrics->errors += 1;
            break;
        }
        metrics->requests += 1;
    }

    co_await socket.shutdown();
    co_await socket.close();
    metrics->connections_done += 1;
    if (thread_done) {
        (*thread_done)++;
    }
}

void runClientThread(const std::string& host, uint16_t port,
                     int connections, int requestsPerConn,
                     size_t payloadBytes, bool statsEnabled,
                     int connectRetries) {
    ThreadMetrics metrics;

    // 创建 SSL 上下文
    SslContext ctx(SslMethod::TLS_1_3_Client);
    if (!ctx.isValid()) {
        metrics.errors += static_cast<uint64_t>(connections);
        metrics.connections_done += static_cast<uint64_t>(connections);
        mergeThreadMetrics(metrics);
        return;
    }

    configureBenchmarkTlsContext(ctx);

    // 加载CA证书，即使不验证（用于建立信任链）
    auto caResult = ctx.loadCACertificate("certs/ca.crt");
    if (!caResult) {
        metrics.errors += static_cast<uint64_t>(connections);
        metrics.connections_done += static_cast<uint64_t>(connections);
        mergeThreadMetrics(metrics);
        return;
    }

    // 不验证服务器证书（测试用）
    ctx.setVerifyMode(SslVerifyMode::None);

    // 创建调度器
    TestScheduler scheduler;
    scheduler.start();

    // 固定 payload，避免构造成本影响压测结果；内容无所谓（echo 只看字节流）
    std::string message(payloadBytes, 'x');
    std::atomic<int> thread_done{0};

    // 启动客户端连接
    for (int i = 0; i < connections; i++) {
        scheduleTask(scheduler, sslClient(&ctx, host, port, message, requestsPerConn,
                                          connectRetries, &thread_done, &metrics, statsEnabled));
    }

    // 等待完成
    while (g_running && thread_done.load() < connections) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    scheduler.stop();
    mergeThreadMetrics(metrics);
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
    int connections = std::stoi(argv[3]);
    int requestsPerConn = std::stoi(argv[4]);
    // 保持默认与历史压测一致（47 字节），大包场景用第 5 个参数显式指定。
    size_t payloadBytes = 47;
    if (argc >= 6) {
        payloadBytes = static_cast<size_t>(std::stoull(argv[5]));
        if (payloadBytes == 0) {
            payloadBytes = 1;
        }
    }
    int threads = 1;
    if (argc >= 7) {
        threads = std::max(1, std::stoi(argv[6]));
    }
    int connectRetries = 3;
    if (argc >= 8) {
        connectRetries = std::max(1, std::stoi(argv[7]));
    }

    // 设置信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN);

    const char* statsEnv = std::getenv("GALAY_SSL_STATS");
    const bool statsEnabled = statsEnv != nullptr && statsEnv[0] != '\0' && std::string(statsEnv) != "0";
    bench::sslStatsSetEnabled(statsEnabled);

    auto startTime = std::chrono::high_resolution_clock::now();

    int baseConns = connections / threads;
    int remainder = connections % threads;
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (int i = 0; i < threads; ++i) {
        int conns = baseConns + (i < remainder ? 1 : 0);
        if (conns == 0) {
            continue;
        }
        workers.emplace_back(runClientThread, host, port, conns, requestsPerConn,
                             payloadBytes, statsEnabled, connectRetries);
    }

    for (auto& t : workers) {
        t.join();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    g_running = false;

    std::cout << "\nBenchmark Results:" << std::endl;
    std::cout << "==================" << std::endl;
    std::cout << "Connections: " << connections << std::endl;
    std::cout << "Requests per connection: " << requestsPerConn << std::endl;
    std::cout << "Payload bytes: " << payloadBytes << std::endl;
    std::cout << "Threads: " << threads << std::endl;
    std::cout << "Total requests: " << g_requests << std::endl;
    std::cout << "Total errors: " << g_errors << std::endl;
    if (g_errors.load() > 0) {
        std::cout << "Error breakdown: "
                  << "connect=" << g_connect_fail.load()
                  << " handshake=" << g_handshake_fail.load()
                  << " send=" << g_send_fail.load()
                  << " recv=" << g_recv_fail.load()
                  << " peer_closed=" << g_peer_closed.load()
                  << std::endl;
    }
    std::cout << "Total bytes sent: " << g_bytes_sent << std::endl;
    std::cout << "Total bytes received: " << g_bytes_recv << std::endl;
    std::cout << "Duration: " << duration.count() << " ms" << std::endl;

    if (duration.count() > 0) {
        double rps = static_cast<double>(g_requests) * 1000.0 / duration.count();
        double throughput = static_cast<double>(g_bytes_sent + g_bytes_recv) / 1024.0 / 1024.0 * 1000.0 / duration.count();
        std::cout << "Requests/sec: " << rps << std::endl;
        std::cout << "Throughput: " << throughput << " MB/s" << std::endl;
    }

    if (statsEnabled) {
        auto stats = bench::sslStatsSnapshot();
        std::cout << "\nSSL IO Stats (Benchmark-side):" << std::endl;
        std::cout << "Send ops: " << stats.send_ops
                  << ", send plain bytes: " << stats.send_plain_bytes << std::endl;
        std::cout << "Recv ops: " << stats.recv_ops
                  << ", recv plain bytes: " << stats.recv_plain_bytes
                  << ", recv chunks: " << stats.recv_chunks << std::endl;
        if (stats.recv_chunks > 0) {
            const double avg_chunk = static_cast<double>(stats.recv_plain_bytes) /
                                     static_cast<double>(stats.recv_chunks);
            std::cout << "Avg recv chunk bytes: " << avg_chunk << std::endl;
        }
    }

    return 0;
}
