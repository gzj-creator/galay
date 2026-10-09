#include <galay/cpp/galay-http/client/http_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;
using namespace std::chrono_literals;

namespace {

constexpr std::array kMethods{HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT,
    HttpMethod::DELETE, HttpMethod::HEAD, HttpMethod::OPTIONS, HttpMethod::PATCH,
    HttpMethod::TRACE, HttpMethod::CONNECT};

struct TestState {
    std::atomic<bool> done{false};
    size_t passed = 0;
    size_t failed = 0;
};

auto make_method_request(HttpSession& session, HttpMethod method)
{
    switch (method) {
    case HttpMethod::GET: return session.get("/api/data");
    case HttpMethod::POST: return session.post("/api/data", R"({"value":123})", "application/json");
    case HttpMethod::PUT: return session.put("/api/data/1", R"({"value":456})", "application/json");
    case HttpMethod::DELETE: return session.del("/api/data/1");
    case HttpMethod::HEAD: return session.head("/api/data");
    case HttpMethod::OPTIONS: return session.options("/api/data");
    case HttpMethod::PATCH: return session.patch("/api/data/1", R"({"value":789})", "application/json");
    case HttpMethod::TRACE: return session.trace("/api/data");
    case HttpMethod::CONNECT: return session.tunnel("example.com:443");
    default: std::abort();
    }
}

Task<void> run_methods(TestState* state)
{
    for (const auto method : kMethods) {
        const std::string name = http_method_to_string(method);
        AsyncTcpSocket socket(IPType::IPV4);
        const auto nonblocking = socket.option().handle_non_block();
        if (!nonblocking) {
            ++state->failed;
            std::cerr << name << " nonblocking setup failed\n";
            const auto closed = co_await socket.close();
            if (!closed) ++state->failed;
            continue;
        }
        const auto connected = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", 8080)).timeout(1s);
        if (!connected) {
            ++state->failed;
            std::cerr << name << " connect failed: " << connected.error().message() << '\n';
            const auto closed = co_await socket.close();
            if (!closed) ++state->failed;
            continue;
        }
        HttpClient client(std::move(socket), HttpClientBuilder().build_config());
        const auto session_result = client.get_session();
        bool passed = false;
        if (!session_result) {
            std::cerr << name << " session failed: " << session_result.error().message() << '\n';
        } else {
            auto& session = **session_result;
            auto response = co_await make_method_request(session, method).timeout(1s);
            if (!response) {
                std::cerr << name << " request failed: " << response.error().message() << '\n';
            } else if (!response->has_value()) {
                std::cerr << name << " returned no response\n";
            } else {
                const int status = static_cast<int>((**response).header().code());
                passed = status >= 200 && status < 300 &&
                    (method != HttpMethod::HEAD || (**response).body_str().empty());
                if (!passed) std::cerr << name << " invalid response, status=" << status << '\n';
            }
        }
        const auto closed = co_await client.close();
        if (!closed) {
            std::cerr << name << " close failed: " << closed.error().message() << '\n';
            passed = false;
        }
        if (passed) {
            ++state->passed;
            std::cout << name << " passed\n";
        } else {
            ++state->failed;
        }
    }
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    Runtime runtime;
    const auto started = runtime.start();
    if (!started) {
        std::cerr << "Runtime start failed\n";
        return 1;
    }
    auto* scheduler = runtime.get_next_io_scheduler();
    TestState state;
    if (!scheduler || !schedule_task(scheduler, run_methods(&state))) {
        runtime.stop();
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!state.done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool completed = state.done.load(std::memory_order_acquire);
    runtime.stop();
    if (!completed) {
        std::cerr << "HTTP methods did not complete before deadline\n";
        return 1;
    }
    std::cout << "HTTP methods passed=" << state.passed << " failed=" << state.failed << '\n';
    return state.failed == 0 && state.passed == kMethods.size() ? 0 : 1;
}
