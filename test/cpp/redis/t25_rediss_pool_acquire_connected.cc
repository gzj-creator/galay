#include <galay/cpp/galay-redis/async/conn_pool.h>
#include <galay/cpp/galay-redis/async/redis_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using namespace galay::kernel;
using namespace galay::redis;
using namespace std::chrono_literals;

namespace {

struct TestState {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
    bool live_case_skipped = false;
};

struct ParsedRedissUrl {
    std::string host;
    int32_t port = 6380;
};

void fail(TestState* state, std::string message)
{
    state->ok = false;
    state->error = std::move(message);
}

std::optional<bool> parseBoolEnv(const char* value)
{
    if (value == nullptr) {
        return std::nullopt;
    }
    const std::string_view text(value);
    if (text == "1" || text == "true" || text == "TRUE") {
        return true;
    }
    if (text == "0" || text == "false" || text == "FALSE") {
        return false;
    }
    return std::nullopt;
}

bool parsePort(std::string_view text, int32_t* out_port)
{
    if (text.empty() || out_port == nullptr) {
        return false;
    }
    int parsed = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed <= 0 || parsed > 65535) {
        return false;
    }
    *out_port = static_cast<int32_t>(parsed);
    return true;
}

std::optional<ParsedRedissUrl> parseRedissUrl(std::string_view url)
{
    constexpr std::string_view prefix = "rediss://";
    if (!url.starts_with(prefix)) {
        return std::nullopt;
    }

    std::string_view rest = url.substr(prefix.size());
    const size_t slash = rest.find('/');
    if (slash != std::string_view::npos) {
        rest = rest.substr(0, slash);
    }
    const size_t at = rest.rfind('@');
    if (at != std::string_view::npos) {
        rest = rest.substr(at + 1);
    }
    if (rest.empty()) {
        return std::nullopt;
    }

    ParsedRedissUrl parsed;
    if (rest.front() == '[') {
        const size_t close = rest.find(']');
        if (close == std::string_view::npos || close == 1) {
            return std::nullopt;
        }
        parsed.host = std::string(rest.substr(1, close - 1));
        if (close + 1 < rest.size()) {
            if (rest[close + 1] != ':') {
                return std::nullopt;
            }
            if (!parsePort(rest.substr(close + 2), &parsed.port)) {
                return std::nullopt;
            }
        }
        return parsed;
    }

    const size_t colon = rest.rfind(':');
    if (colon != std::string_view::npos) {
        if (rest.find(':') != colon) {
            return std::nullopt;
        }
        parsed.host = std::string(rest.substr(0, colon));
        if (parsed.host.empty() || !parsePort(rest.substr(colon + 1), &parsed.port)) {
            return std::nullopt;
        }
        return parsed;
    }

    parsed.host = std::string(rest);
    return parsed.host.empty() ? std::nullopt : std::optional<ParsedRedissUrl>(std::move(parsed));
}

RedissClientConfig tlsConfigFromEnv()
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
    return tls_config;
}

Task<bool> runClosedPortLeakCase(IOScheduler* scheduler, TestState* state)
{
    RedissConnectionPoolConfig config = RedissConnectionPoolConfig::create("127.0.0.1", 1, 0, 1);
    config.initial_connections = 0;
    config.connect_timeout = 50ms;
    config.acquire_timeout = 200ms;
    config.tls_config.verify_peer = false;

    RedissConnectionPool pool(scheduler, config);
    auto init = co_await pool.initialize().timeout(500ms);
    if (!init) {
        fail(state, "closed-port Rediss pool init failed: " + init.error().message());
        co_return false;
    }

    auto acquired = co_await pool.acquire().timeout(500ms);
    if (acquired) {
        pool.release(acquired.value());
        pool.shutdown();
        fail(state, "closed-port Rediss acquire unexpectedly returned a connection");
        co_return false;
    }

    const auto stats = pool.getStats();
    if (stats.total_connections != 0 ||
        stats.available_connections != 0 ||
        stats.active_connections != 0 ||
        stats.waiting_requests != 0) {
        pool.shutdown();
        fail(state, "closed-port Rediss acquire leaked pool stats");
        co_return false;
    }

    pool.shutdown();
    co_return true;
}

Task<bool> runLiveAcquireConnectedCase(IOScheduler* scheduler, TestState* state)
{
    const char* url_value = std::getenv("GALAY_REDIS_TLS_URL");
    if (url_value == nullptr || std::string_view(url_value).empty()) {
        state->live_case_skipped = true;
        co_return true;
    }

    auto parsed = parseRedissUrl(url_value);
    if (!parsed.has_value()) {
        fail(state, "invalid GALAY_REDIS_TLS_URL");
        co_return false;
    }

    RedissConnectionPoolConfig config = RedissConnectionPoolConfig::create(parsed->host, parsed->port, 0, 1);
    config.initial_connections = 0;
    config.connect_timeout = 5s;
    config.acquire_timeout = 5s;
    config.tls_config = tlsConfigFromEnv();

    RedissConnectionPool pool(scheduler, config);
    auto init = co_await pool.initialize().timeout(5s);
    if (!init) {
        fail(state, "live Rediss pool init failed: " + init.error().message());
        co_return false;
    }

    auto acquired = co_await pool.acquire().timeout(5s);
    if (!acquired) {
        pool.shutdown();
        fail(state, "live Rediss acquire failed: " + acquired.error().message());
        co_return false;
    }

    RedisCommandBuilder builder;
    auto conn = acquired.value();
    auto ping = co_await conn->get()->command(builder.ping()).timeout(5s);
    if (!ping || !ping.value().has_value() || ping.value()->empty()) {
        pool.release(conn);
        pool.shutdown();
        fail(state, "live Rediss PING failed after pool.acquire without reconnect");
        co_return false;
    }

    pool.release(conn);
    pool.shutdown();
    co_return true;
}

Task<void> runRedissPoolAcquireConnected(IOScheduler* scheduler, TestState* state)
{
    if (!co_await runClosedPortLeakCase(scheduler, state)) {
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!co_await runLiveAcquireConnectedCase(scheduler, state)) {
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "Failed to get IO scheduler\n";
        return 1;
    }

    TestState state;
    if (!scheduleTask(scheduler, runRedissPoolAcquireConnected(scheduler, &state))) {
        runtime.stop();
        std::cerr << "failed to schedule Rediss pool acquire test\n";
        return 1;
    }

    for (int i = 0; i < 1000 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(10ms);
    }

    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "T25 timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    if (state.live_case_skipped) {
        std::cout << "T25-RedissPoolAcquireConnected PASS (live TLS case skipped: GALAY_REDIS_TLS_URL not set)\n";
    } else {
        std::cout << "T25-RedissPoolAcquireConnected PASS\n";
    }
    return 0;
}
