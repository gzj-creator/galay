#include "common/example_common.h"
#include "kernel/kernel/runtime.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

import galay.websocket;

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::websocket;
using namespace galay::kernel;

Task<bool> runWssClient(const std::string& url, int message_count) {
    constexpr auto kOpTimeout = std::chrono::milliseconds(3000);

    WssClient client(WssClientBuilder()
        .verifyPeer(false)
        .build());

    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
        co_return false;
    }

    auto handshake_result = co_await client.handshake();
    if (!handshake_result) {
        std::cerr << "Handshake failed: " << handshake_result.error().message() << "\n";
        (void)co_await client.close();
        co_return false;
    }

    auto session_result = client.getSession(WsWriterSetting::byClient());
    if (!session_result) {
        std::cerr << "Session failed: " << session_result.error().message() << "\n";
        (void)co_await client.close();
        co_return false;
    }
    auto& session = *session_result.value();
    auto upgrade_result = co_await session.upgrade()().timeout(kOpTimeout);
    if (!upgrade_result || !upgrade_result.value()) {
        std::cerr << "Upgrade failed\n";
        (void)co_await client.close();
        co_return false;
    }

    std::string message;
    WsOpcode opcode{};
    while (true) {
        auto recv_result = co_await session.getMessage(message, opcode).timeout(kOpTimeout);
        if (!recv_result) {
            std::cerr << "Failed to receive welcome message: " << recv_result.error().message() << "\n";
            (void)co_await client.close();
            co_return false;
        }
        if (recv_result.value()) {
            break;
        }
    }

    std::cout << "Welcome: " << message << "\n";

    for (int i = 0; i < message_count; ++i) {
        std::string payload = "Hello from import WSS client #" + std::to_string(i + 1);
        while (true) {
            auto send_result = co_await session.sendText(payload);
            if (!send_result) {
                std::cerr << "Send failed: " << send_result.error().message() << "\n";
                (void)co_await client.close();
                co_return false;
            }
            if (send_result.value()) {
                break;
            }
        }

        while (true) {
            auto recv_result = co_await session.getMessage(message, opcode).timeout(kOpTimeout);
            if (!recv_result) {
                std::cerr << "Receive failed: " << recv_result.error().message() << "\n";
                (void)co_await client.close();
                co_return false;
            }
            if (recv_result.value()) {
                break;
            }
        }

        std::cout << "Echo: " << message << "\n";
    }

    while (true) {
        auto close_result = co_await session.sendClose(WsCloseCode::Normal);
        if (!close_result) {
            break;
        }
        if (close_result.value()) {
            break;
        }
    }

    (void)co_await client.close();
    co_return true;
}

int main(int argc, char* argv[]) {
    std::string url =
        "wss://127.0.0.1:" + std::to_string(galay::http::example::kDefaultWssEchoPort) + "/ws";
    int message_count = 3;

    if (argc > 1) {
        url = argv[1];
    }
    if (argc > 2) {
        message_count = std::atoi(argv[2]);
    }

    try {
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();
        auto join = runtime.spawn(runWssClient(url, message_count));
        bool ok = false;
        if (join) {
            auto result = join->join();
            ok = result && result.value();
        }
        runtime.stop();
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Client error: " << e.what() << "\n";
        return 1;
    }
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
