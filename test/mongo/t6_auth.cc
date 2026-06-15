#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <galay-kernel/core/runtime.h>

#include "galay-mongo/async/client.h"
#include "galay-mongo/sync/mongo_client.h"
#include "async_result_helper.h"
#include "config.h"

using namespace galay::kernel;
using namespace galay::mongo;

namespace
{

struct AsyncAuthState
{
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    std::string error;
};

struct AsyncClientConfig
{
    MongoConfig mongo;
    AsyncMongoConfig async;
};

void setFailure(AsyncAuthState* state, std::string message)
{
    state->ok.store(false, std::memory_order_relaxed);
    state->error = std::move(message);
    state->done.store(true, std::memory_order_release);
}

Task<void> runAsyncAuth(IOScheduler* scheduler,
                        AsyncAuthState* state,
                        AsyncClientConfig cfg)
{
    auto client = AsyncMongoClientBuilder().scheduler(scheduler).config(cfg.async).build();

    const std::expected<bool, MongoError> connected =
        mongo_test::unwrapMongoTaskResult(co_await client.connect(cfg.mongo),
                                          MONGO_ERROR_CONNECTION);
    if (!connected) {
        setFailure(state, "async connect failed: " + connected.error().message());
        co_return;
    }

    const std::expected<MongoReply, MongoError> ping =
        mongo_test::unwrapMongoTaskResult(co_await client.ping(cfg.mongo.database),
                                          MONGO_ERROR_COMMAND);
    if (!ping) {
        setFailure(state, "async ping failed: " + ping.error().message());
        co_return;
    }

    co_await client.close();
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    std::cout << "=== T6: Auth Compatibility Tests (Sync + Async) ===" << std::endl;

    const auto test_cfg = mongo_test::loadMongoTestConfig();
    mongo_test::printMongoTestConfig(test_cfg);

    const bool has_user = !test_cfg.username.empty();
    const bool has_password = !test_cfg.password.empty();

    if (!has_user && !has_password) {
        std::cout << "SKIPPED: auth env not provided. Set GALAY_MONGO_USER and GALAY_MONGO_PASSWORD to run this test."
                  << std::endl;
        return 0;
    }

    if (has_user != has_password) {
        std::cerr << "FAIL: GALAY_MONGO_USER and GALAY_MONGO_PASSWORD must be provided together"
                  << std::endl;
        return 1;
    }

    const auto cfg = mongo_test::toMongoConfig(test_cfg);

    MongoClient session;
    auto sync_connected = session.connect(cfg);
    if (!sync_connected) {
        std::cerr << "FAIL: sync connect failed: " << sync_connected.error().message() << std::endl;
        return 1;
    }

    auto sync_ping = session.ping(cfg.database);
    if (!sync_ping) {
        std::cerr << "FAIL: sync ping failed: " << sync_ping.error().message() << std::endl;
        session.close();
        return 1;
    }
    session.close();

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        std::cerr << "FAIL: no scheduler available" << std::endl;
        runtime.stop();
        return 1;
    }

    AsyncAuthState state;
    if (!scheduleTask(scheduler,
                      runAsyncAuth(scheduler,
                                   &state,
                                   AsyncClientConfig{cfg, mongo_test::loadAsyncMongoTestConfig()}))) {
        std::cerr << "FAIL: failed to schedule async auth task" << std::endl;
        runtime.stop();
        return 1;
    }

    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!state.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }

    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "FAIL: async auth timeout" << std::endl;
        return 1;
    }

    if (!state.ok.load(std::memory_order_relaxed)) {
        std::cerr << "FAIL: " << state.error << std::endl;
        return 1;
    }

    std::cout << "T6 auth compatibility test OK" << std::endl;
    return 0;
}
