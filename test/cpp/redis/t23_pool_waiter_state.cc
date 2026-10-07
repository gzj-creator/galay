#include <galay/cpp/galay-redis/async/conn_pool_waiter_state.h>
#include <atomic>
#include <iostream>

using galay::redis::detail::PoolWaiterState;
using galay::redis::detail::try_complete_waiter;

namespace
{

bool expect_state(const char* scenario,
                 const std::atomic<PoolWaiterState>& state,
                 PoolWaiterState expected)
{
    const auto actual = state.load(std::memory_order_acquire);
    if (actual != expected) {
        std::cerr << scenario << " ended in unexpected waiter state\n";
        return false;
    }
    return true;
}

bool release_wins_before_timeout()
{
    std::atomic<PoolWaiterState> state{PoolWaiterState::Waiting};
    if (!try_complete_waiter(state, PoolWaiterState::Completed)) {
        std::cerr << "release should complete a waiting waiter\n";
        return false;
    }
    if (try_complete_waiter(state, PoolWaiterState::TimedOut)) {
        std::cerr << "timeout must not overwrite a completed waiter\n";
        return false;
    }
    return expect_state("release before timeout", state, PoolWaiterState::Completed);
}

bool timeout_wins_before_release()
{
    std::atomic<PoolWaiterState> state{PoolWaiterState::Waiting};
    if (!try_complete_waiter(state, PoolWaiterState::TimedOut)) {
        std::cerr << "timeout should complete a waiting waiter\n";
        return false;
    }
    if (try_complete_waiter(state, PoolWaiterState::Completed)) {
        std::cerr << "release must not overwrite a timed-out waiter\n";
        return false;
    }
    return expect_state("timeout before release", state, PoolWaiterState::TimedOut);
}

bool duplicate_callbacks_are_idempotent()
{
    std::atomic<PoolWaiterState> completed{PoolWaiterState::Waiting};
    if (!try_complete_waiter(completed, PoolWaiterState::Completed)) {
        std::cerr << "first release should win\n";
        return false;
    }
    if (try_complete_waiter(completed, PoolWaiterState::Completed)) {
        std::cerr << "duplicate release should be ignored\n";
        return false;
    }

    std::atomic<PoolWaiterState> timed_out{PoolWaiterState::Waiting};
    if (!try_complete_waiter(timed_out, PoolWaiterState::TimedOut)) {
        std::cerr << "first timeout should win\n";
        return false;
    }
    if (try_complete_waiter(timed_out, PoolWaiterState::TimedOut)) {
        std::cerr << "duplicate timeout should be ignored\n";
        return false;
    }

    return expect_state("duplicate release", completed, PoolWaiterState::Completed) &&
           expect_state("duplicate timeout", timed_out, PoolWaiterState::TimedOut);
}

bool cancelled_waiter_cannot_be_completed()
{
    std::atomic<PoolWaiterState> state{PoolWaiterState::Waiting};
    if (!try_complete_waiter(state, PoolWaiterState::Cancelled)) {
        std::cerr << "cancel should complete a waiting waiter\n";
        return false;
    }
    if (try_complete_waiter(state, PoolWaiterState::Completed)) {
        std::cerr << "release must not overwrite a cancelled waiter\n";
        return false;
    }
    if (try_complete_waiter(state, PoolWaiterState::TimedOut)) {
        std::cerr << "timeout must not overwrite a cancelled waiter\n";
        return false;
    }
    return expect_state("cancelled waiter", state, PoolWaiterState::Cancelled);
}

} // namespace

int main()
{
    if (!release_wins_before_timeout() ||
        !timeout_wins_before_release() ||
        !duplicate_callbacks_are_idempotent() ||
        !cancelled_waiter_cannot_be_completed()) {
        return 1;
    }

    std::cout << "T23-RedisPoolWaiterState PASS\n";
    return 0;
}
