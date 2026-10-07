/**
 * @file t3_client.cpp
 * @brief RPC客户端测试
 */

#include "result_writer.h"
#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>

using namespace galay::rpc;
using namespace galay::kernel;

test::TestResultWriter* g_writer = nullptr;

Task<void> test_echo_call(RpcClient& client) {
    std::string payload = "Hello, RPC!";

    while (true) {
        auto result = co_await client.call("EchoService", "echo", payload);
        if (!result) {
            if (g_writer) {
                g_writer->write_test_case("Echo call", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            if (g_writer) {
                g_writer->write_test_case("Echo call",
                    response.is_ok() &&
                    std::string(response.payload().data(), response.payload().size()) == payload);
            }
            break;
        }
    }
    co_return;
}

Task<void> test_uppercase_call(RpcClient& client) {
    std::string payload = "hello world";

    while (true) {
        auto result = co_await client.call("EchoService", "uppercase", payload);
        if (!result) {
            if (g_writer) {
                g_writer->write_test_case("Uppercase call", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            if (g_writer) {
                g_writer->write_test_case("Uppercase call",
                    response.is_ok() &&
                    std::string(response.payload().data(), response.payload().size()) == "HELLO WORLD");
            }
            break;
        }
    }
    co_return;
}

Task<void> test_add_call(RpcClient& client) {
    int32_t a = 100, b = 200;
    char payload[8];
    std::memcpy(payload, &a, 4);
    std::memcpy(payload + 4, &b, 4);

    while (true) {
        auto result = co_await client.call("CalcService", "add", payload, 8);
        if (!result) {
            if (g_writer) {
                g_writer->write_test_case("Add call", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            bool success = false;
            if (response.is_ok() && response.payload().size() >= 4) {
                int32_t sum;
                std::memcpy(&sum, response.payload().data(), 4);
                success = (sum == 300);
            }
            if (g_writer) {
                g_writer->write_test_case("Add call (100 + 200 = 300)", success);
            }
            break;
        }
    }
    co_return;
}

Task<void> test_service_not_found(RpcClient& client) {
    while (true) {
        auto result = co_await client.call("NonExistentService", "method");
        if (!result) {
            if (g_writer) {
                g_writer->write_test_case("Service not found", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            if (g_writer) {
                g_writer->write_test_case("Service not found",
                    response.error_code() == RpcErrorCode::SERVICE_NOT_FOUND);
            }
            break;
        }
    }
    co_return;
}

Task<void> test_method_not_found(RpcClient& client) {
    while (true) {
        auto result = co_await client.call("EchoService", "nonExistentMethod");
        if (!result) {
            if (g_writer) {
                g_writer->write_test_case("Method not found", false, std::string(result.error().message()));
            }
            co_return;
        }
        if (result.value()) {
            const auto& response = result.value().value().value();
            if (g_writer) {
                g_writer->write_test_case("Method not found",
                    response.error_code() == RpcErrorCode::METHOD_NOT_FOUND);
            }
            break;
        }
    }
    co_return;
}

Task<void> run_all_tests(const std::string& host, uint16_t port, std::atomic<bool>* done) {
    RpcClient client;

    auto connect_result = co_await client.connect(host, port);

    if (!connect_result) {
        std::cerr << "Failed to connect to server: "
                  << connect_result.error().message() << "\n";
        if (g_writer) {
            g_writer->write_test_case("Connect to server", false, std::string(connect_result.error().message()));
        }
        done->store(true, std::memory_order_release);
        co_return;
    }

    if (g_writer) {
        g_writer->write_test_case("Connect to server", true);
    }

    std::cout << "Connected to server, running tests...\n";

    co_await test_echo_call(client);
    co_await test_uppercase_call(client);
    co_await test_add_call(client);
    co_await test_service_not_found(client);
    co_await test_method_not_found(client);

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

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(1).build();
    runtime.start();

    std::atomic<bool> done{false};
    auto* scheduler = runtime.get_next_io_scheduler();
    if (!schedule_task(scheduler, run_all_tests(host, port, &done))) {
        writer.write_test_case("Schedule test task", false, "Failed to schedule task on IO scheduler");
    } else {
        for (int i = 0; i < 50 && !done.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done.load(std::memory_order_acquire)) {
            writer.write_test_case("Test completion", false, "Timed out waiting for test task");
        }
    }

    runtime.stop();

    writer.write_summary();

    std::cout << "Results: Passed=" << writer.passed()
              << ", Failed=" << writer.failed() << "\n";

    return writer.failed() > 0 ? 1 : 0;
}
