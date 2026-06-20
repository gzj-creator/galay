#include <galay/cpp/galay-kernel/core/runtime.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

import galay.http;

using namespace galay::http;
using namespace galay::kernel;

Task<bool> sendEchoRequest(const std::string& url, const std::string& message) {
    auto client = HttpClientBuilder().build();
    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        std::cerr << "Failed to connect: " << connect_result.error().message() << "\n";
        co_return false;
    }

    auto socket_result = client.socket();
    if (!socket_result) {
        (void)co_await client.close();
        co_return false;
    }
    HttpSession session(socket_result.value().get());
    const std::string request_path = client.url().path;
    std::map<std::string, std::string> headers{
        {"Host", client.url().host + ":" + std::to_string(client.url().port)},
        {"Connection", "close"}
    };

    auto result = co_await session.post(request_path, message, "text/plain", headers);
    if (!result) {
        std::cerr << "Request failed: " << result.error().message() << "\n";
        co_return false;
    }

    auto response_opt = result.value();
    if (!response_opt.has_value()) {
        std::cerr << "Request incomplete\n";
        co_return false;
    }

    auto& response = response_opt.value();
    std::cout << "Status: " << static_cast<int>(response.header().code())
              << " " << httpStatusCodeToString(response.header().code()) << "\n";
    std::cout << "Body: " << response.getBodyStr() << "\n";
    co_return true;
}

int main(int argc, char* argv[]) {
    std::string url = "http://127.0.0.1:8080/echo";
    std::string message = "Hello from import echo client!";
    if (argc > 1) {
        url = argv[1];
    }
    if (argc > 2) {
        message = argv[2];
    }

    try {
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();
        auto join = runtime.spawn(sendEchoRequest(url, message));
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
