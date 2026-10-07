#include "common/example_common.h"
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <cstdlib>
#include <iostream>
#include <string>

import galay.http;

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http;
using namespace galay::kernel;

Task<bool> run_https_client(const std::string& url) {
    HttpsClient client(HttpsClientBuilder()
        .verify_peer(false)
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

    auto session_result = client.get_session();
    if (!session_result) {
        (void)co_await client.close();
        co_return false;
    }
    auto& session = *session_result.value();
    auto& writer = session.get_writer();
    auto& reader = session.get_reader();

    auto request = Http1_1RequestBuilder::get("/")
        .host("localhost")
        .connection("close")
        .build_move();

    while (true) {
        auto send_result = co_await writer.send_request(request);
        if (!send_result) {
            std::cerr << "Send failed: " << send_result.error().message() << "\n";
            (void)co_await client.close();
            co_return false;
        }
        if (send_result.value()) {
            break;
        }
    }

    HttpResponse response;
    while (true) {
        auto recv_result = co_await reader.get_response(response);
        if (!recv_result) {
            std::cerr << "Recv failed: " << recv_result.error().message() << "\n";
            (void)co_await client.close();
            co_return false;
        }
        if (recv_result.value()) {
            break;
        }
    }

    std::cout << "Status: " << static_cast<int>(response.header().code()) << "\n";
    std::cout << "Body: " << response.get_body_str() << "\n";
    (void)co_await client.close();
    co_return true;
}

int main(int argc, char* argv[]) {
    std::string url =
        "https://127.0.0.1:" + std::to_string(galay::http::example::kDefaultHttpsEchoPort) + "/";
    if (argc > 1) {
        url = argv[1];
    }

    try {
        Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
        runtime.start();
        auto join = runtime.spawn_io(run_https_client(url));
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
