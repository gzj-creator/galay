#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-mcp/v2/server/http_server.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace galay::mcp::v2 {

struct McpHttpServerTestAccess {
    static kernel::Task<void> subscriptions(McpHttpServer* server,
                                             std::atomic<int>* completed,
                                             std::atomic<int>* held,
                                             std::atomic<bool>* failed)
    {
        for (int i = 0; i != 2001; ++i) {
            auto operation = McpHttpServer::Operation::acquire(server->m_activeSubscriptions);
            if (!operation) break;
            SubscriptionFilter filter;
            filter.toolsListChanged = true;
            auto state = std::make_unique<McpHttpServer::Subscription>(i, std::move(filter));
            auto& subscription = *state;
            auto submitted = server->submit(McpHttpServer::Command(
                McpHttpServer::CommandKind::Register, {}, std::move(state)));
            if (!submitted) {
                failed->store(true, std::memory_order_release);
                break;
            }
            const auto registered = co_await subscription.registered.wait();
            if (!registered || !*registered) failed->store(true, std::memory_order_release);
            if (i == 2000) {
                // Keep one live borrower per worker until stop closes its queue.
                held->fetch_add(1, std::memory_order_release);
                while (true) {
                    const auto event = co_await subscription.events.recv();
                    if (event) continue;
                    if (!kernel::IOError::contains(event.error().code(), kernel::kClosed)) {
                        failed->store(true, std::memory_order_release);
                    }
                    break;
                }
            }
            subscription.finished.store(true, std::memory_order_release);
            server->wakeOwner();
            completed->fetch_add(1, std::memory_order_release);
        }
    }

    static bool run()
    {
        using namespace std::chrono_literals;
        McpHttpServer server("127.0.0.1", 0, 2, 0);
        kernel::Runtime runtime = kernel::RuntimeBuilder()
            .ioSchedulerCount(2).parallelSchedulerCount(0).build();
        if (!runtime.start()) return false;
        bool passed = true;
        for (int cycle = 0; cycle != 3 && passed; ++cycle) {
            std::thread owner([&] { server.start(); });
            const auto deadline = std::chrono::steady_clock::now() + 8s;
            while (!server.isRunning() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            std::atomic<int> completed{0};
            std::atomic<int> held{0};
            std::atomic<bool> failed{false};
            std::atomic<bool> quit{false};
            std::atomic<int> accepted{0};
            auto produce = [&] {
                while (!quit.load(std::memory_order_acquire)) {
                    auto result = server.notifyToolsListChanged();
                    if (result) accepted.fetch_add(1, std::memory_order_relaxed);
                    else if (result.error().code() != McpErrorCode::ConnectionClosed) {
                        failed.store(true, std::memory_order_release);
                    }
                    std::this_thread::sleep_for(50us);
                }
            };
            std::thread firstProducer(produce);
            std::thread secondProducer(produce);
            const bool first = kernel::scheduleTask(runtime.getIOScheduler(0),
                subscriptions(&server, &completed, &held, &failed));
            const bool second = kernel::scheduleTask(runtime.getIOScheduler(1),
                subscriptions(&server, &completed, &held, &failed));
            while (first && second && held.load(std::memory_order_acquire) != 2 &&
                   !failed.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            passed = server.isRunning() && first && second &&
                held.load(std::memory_order_acquire) == 2 &&
                completed.load(std::memory_order_acquire) == 4000 &&
                !failed.load(std::memory_order_acquire) && accepted.load() != 0;
            // Producers keep submitting while two external threads stop the owner.
            std::thread firstStop([&] { server.stop(); });
            std::thread secondStop([&] { server.stop(); });
            firstStop.join();
            secondStop.join();
            quit.store(true, std::memory_order_release);
            firstProducer.join();
            secondProducer.join();
            owner.join();
            const auto rejected = server.notifyToolsListChanged();
            passed = passed && !server.isRunning() && !rejected &&
                completed.load(std::memory_order_acquire) == 4002 &&
                !failed.load(std::memory_order_acquire) &&
                rejected.error().code() == McpErrorCode::ConnectionClosed &&
                server.m_subscriptions == nullptr &&
                server.m_activeSubscriptions.load() == McpHttpServer::kAdmissionClosed;
        }
        runtime.stop();
        return passed;
    }
};

} // namespace galay::mcp::v2

int main()
{
    if (!galay::mcp::v2::McpHttpServerTestAccess::run()) {
        std::cerr << "MCP owner lifecycle stress failed" << std::endl;
        return 1;
    }
    std::cout << "MCP owner lifecycle stress PASS" << std::endl;
}
