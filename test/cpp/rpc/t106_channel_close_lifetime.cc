#include <galay/cpp/galay-rpc/kernel/rpc_conn.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#define private public
#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>
#undef private

using namespace galay::kernel;
using namespace galay::rpc;

namespace {
struct TestState {
    bool controller_alive = false;
    std::optional<JoinHandle<void>> reader;
};

Task<void> finish_pending_reader(RpcChannel* channel, TestState* state)
{
    co_await sleep(std::chrono::milliseconds(50));
    state->controller_alive = channel->socket().controller() != nullptr;
    channel->m_active_loops.store(0, std::memory_order_release);
}

Task<bool> check_close_order(RpcChannel* channel, TestState* state)
{
    auto runtime = RuntimeHandle::try_current();
    if (!runtime) {
        co_return false;
    }
    channel->m_active_loops.store(1, std::memory_order_release);
    auto reader = runtime->spawn_io(finish_pending_reader(channel, state));
    if (!reader) {
        channel->m_active_loops.store(0, std::memory_order_release);
        co_return false;
    }
    state->reader.emplace(std::move(*reader));
    auto closed = co_await channel->close();
    co_return closed && *closed && state->controller_alive &&
        channel->socket().controller() == nullptr;
}
}

int main()
{
    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    RpcChannel channel;
    channel.m_socket = std::make_unique<galay::async::AsyncTcpSocket>(IPType::IPV4);
    if (channel.socket().handle() == GHandle::invalid()) {
        std::cerr << "socket creation failed\n";
        return 1;
    }
    TestState state;
    auto result = runtime.block_on_io(check_close_order(&channel, &state));
    const bool reader_finished = state.reader && state.reader->join().has_value();
    runtime.stop();
    if (!result || !*result || !reader_finished) {
        std::cerr << "channel close released IOController before pending reader finished\n";
        return 1;
    }
    return 0;
}
