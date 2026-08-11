#include "common/config.h"

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-postgres/async/client.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace
{

struct State
{
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

galay::kernel::Task<void> query(galay::kernel::IOScheduler* scheduler,
                                State* state,
                                postgres_example::DbConfig config)
{
    auto client = galay::postgres::AsyncPostgresClientBuilder().scheduler(scheduler).build();
    auto connected = co_await client.connect(config.host,
                                             config.port,
                                             config.user,
                                             config.password,
                                             config.database);
    if (!connected || !connected->has_value()) {
        state->ok = false;
        state->error = !connected ? connected.error().message() : "connect produced no result";
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto result = co_await client.query("SELECT 1");
    if (!result || !result->has_value()) {
        state->ok = false;
        state->error = !result ? result.error().message() : "query produced no result";
    } else if (result->value().rowCount() != 0) {
        std::cout << "SELECT 1 => " << result->value().row(0).getString(0) << '\n';
    }
    auto closed = co_await client.close();
    if (!closed && state->ok) {
        state->ok = false;
        state->error = closed.error().message();
    }
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    const auto config = postgres_example::loadConfig();
    galay::kernel::Runtime runtime;
    const auto started = runtime.start();
    if (!started) {
        std::cerr << started.error().message() << '\n';
        return 1;
    }
    auto* scheduler = runtime.getNextIOScheduler();
    State state;
    if (scheduler == nullptr ||
        !galay::kernel::scheduleTask(scheduler, query(scheduler, &state, config))) {
        runtime.stop();
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!state.done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    runtime.stop();
    if (!state.done.load(std::memory_order_acquire) || !state.ok) {
        std::cerr << (state.error.empty() ? "async example timed out" : state.error) << '\n';
        return 1;
    }
    return 0;
}
