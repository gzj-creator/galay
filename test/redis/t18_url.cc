#include "galay-redis/async/redis_client.h"

#include <galay-kernel/core/runtime.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

using namespace galay::redis;
using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

struct TestState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool success = false;
    std::string message;
};

void finish(TestState& state, bool success, std::string message)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.done = true;
    state.success = success;
    state.message = std::move(message);
    state.cv.notify_one();
}

std::optional<bool> parseBoolEnv(const char* value)
{
    if (value == nullptr) return std::nullopt;
    std::string text(value);
    if (text == "1" || text == "true" || text == "TRUE") return true;
    if (text == "0" || text == "false" || text == "FALSE") return false;
    return std::nullopt;
}

Task<void> runLocalhostRedissSmoke(IOScheduler* scheduler, TestState* state)
{
    RedissClientConfig tls_config;
    if (const char* ca_path = std::getenv("GALAY_REDIS_TLS_CA")) {
        tls_config.ca_path = ca_path;
    }
    if (const auto verify_peer = parseBoolEnv(std::getenv("GALAY_REDIS_TLS_VERIFY_PEER"))) {
        tls_config.verify_peer = *verify_peer;
    }
    if (const char* server_name = std::getenv("GALAY_REDIS_TLS_SERVER_NAME")) {
        tls_config.server_name = server_name;
    }

    auto client = RedissClientBuilder()
                      .scheduler(scheduler)
                      .tlsConfig(tls_config)
                      .build();

    const char* url = std::getenv("GALAY_REDIS_TLS_LOCALHOST_URL");
    if (url == nullptr || std::string(url).empty()) {
        finish(*state, true, "SKIP: GALAY_REDIS_TLS_LOCALHOST_URL not set");
        co_return;
    }

    RedisCommandBuilder builder;

    auto connect_result = co_await client.connect(url).timeout(5s);
    if (!connect_result) {
        finish(*state, false, "connect failed: " + connect_result.error().message());
        co_return;
    }

    auto ping_result = co_await client.command(builder.ping()).timeout(5s);
    if (!ping_result) {
        finish(*state, false, "ping failed: " + ping_result.error().message());
        co_return;
    }
    if (!ping_result.value().has_value()) {
        finish(*state, false, "ping returned empty response");
        co_return;
    }

    auto close_result = co_await client.close();
    if (!close_result) {
        finish(*state, false, "close failed: " + close_result.error().message());
        co_return;
    }

    finish(*state, true, "PASS");
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "Failed to get IO scheduler\n";
        runtime.stop();
        return 1;
    }

    TestState state;
    scheduleTask(scheduler, runLocalhostRedissSmoke(scheduler, &state));

    std::unique_lock<std::mutex> lock(state.mutex);
    const bool done = state.cv.wait_for(lock, 10s, [&state]() { return state.done; });
    runtime.stop();

    if (!done) {
        std::cerr << "T18 timed out\n";
        return 1;
    }

    std::cout << state.message << "\n";
    return state.success ? 0 : 1;
}
