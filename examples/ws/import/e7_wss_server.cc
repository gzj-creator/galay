#include "common/example_common.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

import galay.http;
import galay.websocket;

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

Task<void> handleWssConnection(galay::ssl::SslSocket& socket) {
    WsFrame welcome_frame = WsFrameParser::createTextFrame("Welcome to import WSS server!");
    std::string welcome_data = WsFrameParser::toBytes(welcome_frame, false);

    size_t sent = 0;
    while (sent < welcome_data.size()) {
        auto send_result = co_await socket.send(welcome_data.data() + sent, welcome_data.size() - sent);
        if (!send_result) {
            co_return;
        }
        sent += send_result.value();
    }

    std::vector<char> buffer(8192);
    std::string accumulated;

    while (true) {
        auto recv_result = co_await socket.recv(buffer.data(), buffer.size());
        if (!recv_result) {
            break;
        }

        const size_t bytes_received = recv_result.value().size();
        if (bytes_received == 0) {
            break;
        }

        accumulated.append(buffer.data(), bytes_received);

        while (!accumulated.empty()) {
            WsFrame frame;
            std::vector<iovec> iovecs;
            iovecs.push_back({const_cast<char*>(accumulated.data()), accumulated.size()});

            auto parse_result = WsFrameParser::fromIOVec(iovecs, frame, true);
            if (!parse_result) {
                if (parse_result.error().code() == kWsIncomplete) {
                    break;
                }
                (void)co_await socket.close();
                co_return;
            }

            accumulated.erase(0, parse_result.value());

            if (frame.header.opcode == WsOpcode::Close) {
                WsFrame close_frame = WsFrameParser::createCloseFrame(WsCloseCode::Normal);
                std::string close_data = WsFrameParser::toBytes(close_frame, false);
                (void)co_await socket.send(close_data.data(), close_data.size());
                (void)co_await socket.close();
                co_return;
            }

            if (frame.header.opcode == WsOpcode::Ping) {
                WsFrame pong_frame = WsFrameParser::createPongFrame(frame.payload);
                std::string pong_data = WsFrameParser::toBytes(pong_frame, false);
                (void)co_await socket.send(pong_data.data(), pong_data.size());
                continue;
            }

            if (frame.header.opcode == WsOpcode::Text || frame.header.opcode == WsOpcode::Binary) {
                std::string echo_data = WsFrameParser::toBytes(
                    WsFrameParser::createTextFrame("Echo: " + frame.payload),
                    false);
                size_t echo_sent = 0;
                while (echo_sent < echo_data.size()) {
                    auto send_result = co_await socket.send(
                        echo_data.data() + echo_sent,
                        echo_data.size() - echo_sent);
                    if (!send_result) {
                        (void)co_await socket.close();
                        co_return;
                    }
                    echo_sent += send_result.value();
                }
            }
        }
    }

    (void)co_await socket.close();
    co_return;
}

Task<void> httpsHandler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.getReader();
    HttpRequest request;

    while (true) {
        auto read_result = co_await reader.getRequest(request);
        if (!read_result) {
            (void)co_await conn.close();
            co_return;
        }
        if (read_result.value()) {
            break;
        }
    }

    if (request.header().uri() == "/ws" || request.header().uri().starts_with("/ws?")) {
        auto upgrade_result = WsUpgrade::handleUpgrade(request);
        auto writer = conn.getWriter();
        while (true) {
            auto send_result = co_await writer.sendResponse(upgrade_result.response);
            if (!send_result) {
                (void)co_await conn.close();
                co_return;
            }
            if (send_result.value()) {
                break;
            }
        }
        if (!upgrade_result.success) {
            (void)co_await conn.close();
            co_return;
        }

        auto& socket = conn.getSocket();
        co_await handleWssConnection(socket);
        co_return;
    }

    auto response = Http1_1ResponseBuilder::ok()
        .html(
            "<html><body>"
            "<h1>Import WSS Server</h1>"
            "<p>Connect to <code>wss://127.0.0.1:8443/ws</code>.</p>"
            "</body></html>")
        .build();
    auto writer = conn.getWriter();
    while (true) {
        auto send_result = co_await writer.sendResponse(response);
        if (!send_result) {
            break;
        }
        if (send_result.value()) {
            break;
        }
    }
    (void)co_await conn.close();
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = galay::http::example::kDefaultWssEchoPort;
    std::string cert_path = "test/test.crt";
    std::string key_path = "test/test.key";

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        cert_path = argv[2];
    }
    if (argc > 3) {
        key_path = argv[3];
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpsServer server(HttpsServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .certPath(cert_path)
            .keyPath(key_path)
            .ioSchedulerCount(2)
            .build());
        std::cout << "Import WSS server: wss://127.0.0.1:" << port << "/ws\n";
        server.start(httpsHandler);

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        server.stop();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
