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

Task<void> runBorrowedCommandSmoke(IOScheduler* scheduler, TestState* state)
{
    auto client = RedisClientBuilder().scheduler(scheduler).build();
    protocol::RespEncoder encoder;
    std::string set_encoded;
    std::string get_encoded;
    const std::string key = "galay:plain-fastpath:setget";
    const std::string value = "value-plain-fastpath";

    auto connect_result = co_await client.connect("127.0.0.1", 6379).timeout(5s);
    if (!connect_result) {
        finish(*state, false, "connect failed (need local redis on 127.0.0.1:6379): "
                                  + connect_result.error().message());
        co_return;
    }

    const std::array<std::string_view, 2> set_args{key, value};
    set_encoded.clear();
    set_encoded.reserve(encoder.estimateCommandBytes("SET", set_args));
    encoder.append(set_encoded, "SET", set_args);
    RedisBorrowedCommand set_packet(set_encoded, 1);
    auto set_result = co_await client.commandBorrowed(set_packet).timeout(5s);
    if (!set_result || !set_result.value() || set_result.value()->empty()) {
        finish(*state, false, "borrowed SET failed");
        co_return;
    }
    if (!(*set_result.value())[0].isStatus() ||
        (*set_result.value())[0].toStatus() != "OK") {
        finish(*state, false, "borrowed SET returned unexpected reply");
        co_return;
    }

    const std::array<std::string_view, 1> get_args{key};
    get_encoded.clear();
    get_encoded.reserve(encoder.estimateCommandBytes("GET", get_args));
    encoder.append(get_encoded, "GET", get_args);
    RedisBorrowedCommand get_packet(get_encoded, 1);
    auto get_result = co_await client.commandBorrowed(get_packet).timeout(5s);
    if (!get_result || !get_result.value() || get_result.value()->empty()) {
        finish(*state, false, "borrowed GET failed");
        co_return;
    }
    if (!(*get_result.value())[0].isString() ||
        (*get_result.value())[0].toString() != value) {
        finish(*state, false, "borrowed GET returned wrong value");
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
    scheduleTask(scheduler, runBorrowedCommandSmoke(scheduler, &state));

    std::unique_lock<std::mutex> lock(state.mutex);
    const bool done = state.cv.wait_for(lock, 10s, [&state]() { return state.done; });
    runtime.stop();

    if (!done) {
        std::cerr << "T22 timed out\n";
        return 1;
    }

    std::cout << state.message << "\n";
    return state.success ? 0 : 1;
}
