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

void signal_handler(int) {
    g_running = false;
}

Task<void> handle_wss_connection(galay::ssl::SslSocket& socket) {
    WsFrame welcome_frame = WsFrameParser::create_text_frame("Welcome to import WSS server!");
    std::string welcome_data = WsFrameParser::to_bytes(welcome_frame, false);

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

            auto parse_result = WsFrameParser::from_io_vec(iovecs, frame, true);
            if (!parse_result) {
                if (parse_result.error().code() == kWsIncomplete) {
                    break;
                }
                (void)co_await socket.close();
                co_return;
            }

            accumulated.erase(0, parse_result.value());

            if (frame.header.opcode == WsOpcode::Close) {
                WsFrame close_frame = WsFrameParser::create_close_frame(WsCloseCode::Normal);
                std::string close_data = WsFrameParser::to_bytes(close_frame, false);
                (void)co_await socket.send(close_data.data(), close_data.size());
                (void)co_await socket.close();
                co_return;
            }

            if (frame.header.opcode == WsOpcode::Ping) {
                WsFrame pong_frame = WsFrameParser::create_pong_frame(frame.payload);
                std::string pong_data = WsFrameParser::to_bytes(pong_frame, false);
                (void)co_await socket.send(pong_data.data(), pong_data.size());
                continue;
            }

            if (frame.header.opcode == WsOpcode::Text || frame.header.opcode == WsOpcode::Binary) {
                std::string echo_data = WsFrameParser::to_bytes(
                    WsFrameParser::create_text_frame("Echo: " + frame.payload),
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

Task<void> https_handler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.get_reader();
    HttpRequest request;

    while (true) {
        auto read_result = co_await reader.get_request(request);
        if (!read_result) {
            (void)co_await conn.close();
            co_return;
        }
        if (read_result.value()) {
            break;
        }
    }

    if (request.header().uri() == "/ws" || request.header().uri().starts_with("/ws?")) {
        auto upgrade_result = WsUpgrade::handle_upgrade(request);
        auto writer = conn.get_writer();
        while (true) {
            auto send_result = co_await writer.send_response(upgrade_result.response);
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

        auto& socket = conn.get_socket();
        co_await handle_wss_connection(socket);
        co_return;
    }

    auto response = Http1_1ResponseBuilder::ok()
        .html(
            "<html><body>"
            "<h1>Import WSS Server</h1>"
            "<p>Connect to <code>wss://127.0.0.1:8443/ws</code>.</p>"
            "</body></html>")
        .build();
    auto writer = conn.get_writer();
    while (true) {
        auto send_result = co_await writer.send_response(response);
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

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        HttpsServer server(HttpsServerBuilder<>()
            .host("0.0.0.0")
            .port(port)
            .cert_path(cert_path)
            .key_path(key_path)
            .io_scheduler_count(2)
            .build_config());
        std::cout << "Import WSS server: wss://127.0.0.1:" << port << "/ws\n";
        server.start(https_handler);

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
