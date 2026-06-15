/**
 * @file b1_rpc.cpp
 * @brief RPC压测服务端
 */

#include "galay-rpc/kernel/rpc_server.h"
#include "galay-rpc/kernel/rpc_service.h"
#include "galay-rpc/utils/runtime_compat.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <cstdlib>

using namespace galay::rpc;
using namespace galay::kernel;

namespace {
constexpr int kDefaultPort = 9000;
constexpr size_t kDefaultBacklog = 1024;
constexpr size_t kDefaultBenchRingBufferSize = 128 * 1024;
}

/**
 * @brief 压测Echo服务
 */
class BenchEchoService : public RpcService {
public:
    BenchEchoService() : RpcService("BenchEchoService") {
        registerMethod("echo", &BenchEchoService::echo);
        registerClientStreamingMethod("echo", &BenchEchoService::echo);
        registerServerStreamingMethod("echo", &BenchEchoService::echo);
        registerBidiStreamingMethod("echo", &BenchEchoService::echo);
    }

    Coroutine echo(RpcContext& ctx) {
        auto& req = ctx.request();
        ctx.setPayload(req.payloadView());
        co_return;
    }
};

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#if defined(SIGPIPE)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    uint16_t port = kDefaultPort;
    size_t io_count = 0;  // 自动
    size_t ring_buffer_size = kDefaultBenchRingBufferSize;

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        io_count = static_cast<size_t>(std::atoi(argv[2]));
    }
    if (argc > 3) {
        ring_buffer_size = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    }

    std::cout << "=== RPC Benchmark Server ===\n";
    std::cout << "Port: " << port << "\n";
    const size_t resolved_io_count = resolveIoSchedulerCount(io_count);
    std::cout << "IO Schedulers: " << (io_count == 0 ? "auto" : std::to_string(io_count)) << "\n";
    if (io_count == 0) {
        std::cout << "Resolved IO Schedulers: " << resolved_io_count << "\n";
    }
    std::cout << "RingBuffer size: " << ring_buffer_size << " bytes\n";

    auto service = std::make_shared<BenchEchoService>();

    auto server = RpcServerBuilder()
        .host("0.0.0.0")
        .port(port)
        .ioSchedulerCount(resolved_io_count)
        .backlog(kDefaultBacklog)
        .ringBufferSize(ring_buffer_size)
        .build();
    server.registerService(service);
    server.start();

    std::cout << "Server started. Press Ctrl+C to stop.\n";

    while (g_running.load() && server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "Server stopped.\n";

    return 0;
}
