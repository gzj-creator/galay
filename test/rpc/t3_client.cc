/**
 * @file t3_client.cpp
 * @brief RPC客户端测试
 */

#include "result_writer.h"
#include "rpc/kernel/rpc_client.h"
#include "kernel/kernel/runtime.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>

using namespace galay::rpc;
using namespace galay::kernel;

test::TestResultWriter* g_writer = nullptr;

Coroutine testEchoCall(RpcClient& client) {
    std::string payload = "Hello, RPC!";

    while (true) {
        auto result = co_await client.call("EchoService", "echo", payload);
        if (!result) {
            if (g_writer) {
                g_writer->writeTestCase("Echo call", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            if (g_writer) {
                g_writer->writeTestCase("Echo call",
                    response.isOk() &&
                    std::string(response.payload().data(), response.payload().size()) == payload);
            }
            break;
        }
    }
    co_return;
}

Coroutine testUppercaseCall(RpcClient& client) {
    std::string payload = "hello world";

    while (true) {
        auto result = co_await client.call("EchoService", "uppercase", payload);
        if (!result) {
            if (g_writer) {
                g_writer->writeTestCase("Uppercase call", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            if (g_writer) {
                g_writer->writeTestCase("Uppercase call",
                    response.isOk() &&
                    std::string(response.payload().data(), response.payload().size()) == "HELLO WORLD");
            }
            break;
        }
    }
    co_return;
}

Coroutine testAddCall(RpcClient& client) {
    int32_t a = 100, b = 200;
    char payload[8];
    std::memcpy(payload, &a, 4);
    std::memcpy(payload + 4, &b, 4);

    while (true) {
        auto result = co_await client.call("CalcService", "add", payload, 8);
        if (!result) {
            if (g_writer) {
                g_writer->writeTestCase("Add call", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            bool success = false;
            if (response.isOk() && response.payload().size() >= 4) {
                int32_t sum;
                std::memcpy(&sum, response.payload().data(), 4);
                success = (sum == 300);
            }
            if (g_writer) {
                g_writer->writeTestCase("Add call (100 + 200 = 300)", success);
            }
            break;
        }
    }
    co_return;
}

Coroutine testServiceNotFound(RpcClient& client) {
    while (true) {
        auto result = co_await client.call("NonExistentService", "method");
        if (!result) {
            if (g_writer) {
                g_writer->writeTestCase("Service not found", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            if (g_writer) {
                g_writer->writeTestCase("Service not found",
                    response.errorCode() == RpcErrorCode::SERVICE_NOT_FOUND);
            }
            break;
        }
    }
    co_return;
}

Coroutine testMethodNotFound(RpcClient& client) {
    while (true) {
        auto result = co_await client.call("EchoService", "nonExistentMethod");
        if (!result) {
            if (g_writer) {
                g_writer->writeTestCase("Method not found", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            if (g_writer) {
                g_writer->writeTestCase("Method not found",
                    response.errorCode() == RpcErrorCode::METHOD_NOT_FOUND);
            }
            break;
        }
    }
    co_return;
}

Coroutine runAllTests(const std::string& host, uint16_t port, std::atomic<bool>* done) {
    RpcClient client;

    auto connect_result = co_await client.connect(host, port);

    if (!connect_result) {
        std::cerr << "Failed to connect to server: "
                  << connect_result.error().message() << "\n";
        if (g_writer) {
            g_writer->writeTestCase("Connect to server", false, std::string(connect_result.error().message()));
        }
        done->store(true, std::memory_order_release);
        co_return;
    }

    if (g_writer) {
        g_writer->writeTestCase("Connect to server", true);
    }

    std::cout << "Connected to server, running tests...\n";

    co_await testEchoCall(client);
    co_await testUppercaseCall(client);
    co_await testAddCall(client);
    co_await testServiceNotFound(client);
    co_await testMethodNotFound(client);

    co_await client.close();
    std::cout << "Tests completed.\n";
    done->store(true, std::memory_order_release);
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::atoi(argv[2]));
    }

    test::TestResultWriter writer("t3_client.result");
    g_writer = &writer;

    std::cout << "RPC Client Test - Connecting to " << host << ":" << port << "\n";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    std::atomic<bool> done{false};
    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduleTask(scheduler, runAllTests(host, port, &done))) {
        writer.writeTestCase("Schedule test task", false, "Failed to schedule task on IO scheduler");
    } else {
        for (int i = 0; i < 50 && !done.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done.load(std::memory_order_acquire)) {
            writer.writeTestCase("Test completion", false, "Timed out waiting for test task");
        }
    }

    runtime.stop();

    writer.writeSummary();

    std::cout << "Results: Passed=" << writer.passed()
              << ", Failed=" << writer.failed() << "\n";

    return writer.failed() > 0 ? 1 : 0;
}
