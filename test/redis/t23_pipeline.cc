#include "galay-redis/async/redis_client.h"
#include <galay-kernel/core/runtime.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
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

Task<void> runBorrowedBatchSmoke(IOScheduler* scheduler, TestState* state)
{
    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder builder;
    const std::string key1 = "galay:plain-fastpath:pipeline:1";
    const std::string key2 = "galay:plain-fastpath:pipeline:2";
    const std::string value1 = "pipeline-value-1";
    const std::string value2 = "pipeline-value-2";

    auto connect_result = co_await client.connect("127.0.0.1", 6379).timeout(5s);
    if (!connect_result) {
        finish(*state, false, "connect failed (need local redis on 127.0.0.1:6379): "
                                  + connect_result.error().message());
        co_return;
    }

    builder.append("SET", std::array<std::string_view, 2>{key1, value1});
    builder.append("SET", std::array<std::string_view, 2>{key2, value2});
    builder.append("GET", std::array<std::string_view, 1>{key1});
    builder.append("GET", std::array<std::string_view, 1>{key2});

    auto result = co_await client.batchBorrowed(builder.encoded(), builder.size()).timeout(5s);
    if (!result || !result.value() || result.value()->size() != 4) {
        finish(*state, false, "borrowed pipeline failed");
        co_return;
    }
    if (!(*result.value())[0].isStatus() || (*result.value())[0].toStatus() != "OK") {
        finish(*state, false, "borrowed pipeline SET #1 mismatch");
        co_return;
    }
    if (!(*result.value())[1].isStatus() || (*result.value())[1].toStatus() != "OK") {
        finish(*state, false, "borrowed pipeline SET #2 mismatch");
        co_return;
    }
    if (!(*result.value())[2].isString() || (*result.value())[2].toString() != value1) {
        finish(*state, false, "borrowed pipeline GET #1 mismatch");
        co_return;
    }
    if (!(*result.value())[3].isString() || (*result.value())[3].toString() != value2) {
        finish(*state, false, "borrowed pipeline GET #2 mismatch");
        co_return;
    }

    (void)co_await client.close();
    finish(*state, true, "PASS");
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "Failed to get IO scheduler\n";
        runtime.stop();
        return 1;
    }

    TestState state;
    scheduleTask(scheduler, runBorrowedBatchSmoke(scheduler, &state));

    std::unique_lock<std::mutex> lock(state.mutex);
    const bool done = state.cv.wait_for(lock, 10s, [&state]() { return state.done; });
    runtime.stop();

    if (!done) {
        std::cerr << "T23 timed out\n";
        return 1;
    }

    std::cout << state.message << "\n";
    return state.success ? 0 : 1;
}
