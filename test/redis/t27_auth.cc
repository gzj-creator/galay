#include "galay-redis/async/redis_client.h"
#include "galay-redis/sync/redis_session.h"

#include <galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace galay::kernel;
using namespace galay::redis;
using namespace std::chrono_literals;

namespace
{
struct AsyncState
{
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    std::string message;
};

void finish(AsyncState& state, bool ok, std::string message)
{
    state.message = std::move(message);
    state.ok.store(ok, std::memory_order_relaxed);
    state.done.store(true, std::memory_order_release);
}

std::string getenvOrDefault(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return value;
}

bool expectStatus(const std::expected<RedisValue, RedisError>& result,
                  const std::string& expected,
                  const std::string& label)
{
    if (!result) {
        std::cerr << label << " failed: " << result.error().message() << std::endl;
        return false;
    }
    if (!result->isStatus() || result->toStatus() != expected) {
        std::cerr << label << " returned unexpected reply" << std::endl;
        return false;
    }
    return true;
}

bool expectString(const std::expected<RedisValue, RedisError>& result,
                  const std::string& expected,
                  const std::string& label)
{
    if (!result) {
        std::cerr << label << " failed: " << result.error().message() << std::endl;
        return false;
    }
    if (!result->isString() || result->toString() != expected) {
        std::cerr << label << " returned unexpected reply" << std::endl;
        return false;
    }
    return true;
}

bool runSyncCase(const std::string& url,
                 const std::string& wrong_url,
                 const std::string& key,
                 const std::string& value)
{
    RedisSession session;
    auto connected = session.connect(url);
    if (!connected) {
        std::cerr << "sync auth connect failed: " << connected.error().message() << std::endl;
        return false;
    }

    if (!expectStatus(session.set(key, value), "OK", "sync SET")) {
        session.disconnect();
        return false;
    }
    if (!expectString(session.get(key), value, "sync GET")) {
        session.disconnect();
        return false;
    }
    auto deleted = session.del(key);
    if (!deleted) {
        std::cerr << "sync DEL failed: " << deleted.error().message() << std::endl;
        session.disconnect();
        return false;
    }
    session.disconnect();

    RedisSession wrong_session;
    auto wrong_connected = wrong_session.connect(wrong_url);
    if (wrong_connected) {
        std::cerr << "sync wrong password unexpectedly connected" << std::endl;
        wrong_session.disconnect();
        return false;
    }
    if (wrong_connected.error().type() != REDIS_ERROR_TYPE_AUTH_ERROR) {
        std::cerr << "sync wrong password returned unexpected error: "
                  << wrong_connected.error().message() << std::endl;
        return false;
    }
    return true;
}

bool firstStatus(const std::expected<std::optional<std::vector<RedisValue>>, RedisError>& result,
                 const std::string& expected,
                 std::string* error)
{
    if (!result) {
        *error = result.error().message();
        return false;
    }
    if (!result->has_value() || result->value().empty()) {
        *error = "empty reply";
        return false;
    }
    const auto& first = result->value().front();
    if (!first.isStatus() || first.toStatus() != expected) {
        *error = "unexpected status reply";
        return false;
    }
    return true;
}

bool firstString(const std::expected<std::optional<std::vector<RedisValue>>, RedisError>& result,
                 const std::string& expected,
                 std::string* error)
{
    if (!result) {
        *error = result.error().message();
        return false;
    }
    if (!result->has_value() || result->value().empty()) {
        *error = "empty reply";
        return false;
    }
    const auto& first = result->value().front();
    if (!first.isString() || first.toString() != expected) {
        *error = "unexpected string reply";
        return false;
    }
    return true;
}

Task<void> runAsyncCase(IOScheduler* scheduler,
                        AsyncState* state,
                        std::string url,
                        std::string wrong_url,
                        std::string key,
                        std::string value)
{
    RedisCommandBuilder builder;
    auto client = RedisClientBuilder().scheduler(scheduler).build();

    auto connected = co_await client.connect(url).timeout(5s);
    if (!connected) {
        finish(*state, false, "async auth connect failed: " + connected.error().message());
        co_return;
    }

    std::string error;
    if (!firstStatus(co_await client.command(builder.set(key, value)).timeout(5s), "OK", &error)) {
        finish(*state, false, "async SET failed: " + error);
        co_return;
    }
    if (!firstString(co_await client.command(builder.get(key)).timeout(5s), value, &error)) {
        finish(*state, false, "async GET failed: " + error);
        co_return;
    }
    auto del_result = co_await client.command(builder.del(key)).timeout(5s);
    if (!del_result) {
        finish(*state, false, "async DEL failed: " + del_result.error().message());
        co_return;
    }
    (void)co_await client.close();

    auto wrong_client = RedisClientBuilder().scheduler(scheduler).build();
    auto wrong_connected = co_await wrong_client.connect(wrong_url).timeout(5s);
    if (wrong_connected) {
        (void)co_await wrong_client.close();
        finish(*state, false, "async wrong password unexpectedly connected");
        co_return;
    }
    if (wrong_connected.error().type() != REDIS_ERROR_TYPE_AUTH_ERROR) {
        finish(*state,
               false,
               "async wrong password returned unexpected error: " +
                   wrong_connected.error().message());
        co_return;
    }

    finish(*state, true, "PASS");
}
} // namespace

int main()
{
    const std::string url = getenvOrDefault("GALAY_REDIS_AUTH_URL", "");
    const std::string wrong_url = getenvOrDefault("GALAY_REDIS_AUTH_WRONG_URL", "");
    if (url.empty() || wrong_url.empty()) {
        std::cout << "SKIP: set GALAY_REDIS_AUTH_URL and GALAY_REDIS_AUTH_WRONG_URL" << std::endl;
        return 0;
    }

    const std::string key = getenvOrDefault("GALAY_REDIS_AUTH_KEY", "galay:redis:auth:test");
    const std::string value = getenvOrDefault("GALAY_REDIS_AUTH_VALUE", "auth-ok");

    if (!runSyncCase(url, wrong_url, key + ":sync", value)) {
        return 1;
    }

    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        std::cerr << "failed to get IO scheduler" << std::endl;
        runtime.stop();
        return 1;
    }

    AsyncState state;
    scheduleTask(scheduler, runAsyncCase(scheduler,
                                         &state,
                                         url,
                                         wrong_url,
                                         key + ":async",
                                         value));

    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!state.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "async auth test timeout" << std::endl;
        return 1;
    }
    if (!state.ok.load(std::memory_order_relaxed)) {
        std::cerr << state.message << std::endl;
        return 1;
    }

    std::cout << "T27 Redis auth compatibility test OK" << std::endl;
    return 0;
}
