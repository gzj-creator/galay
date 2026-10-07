#include <galay/cpp/galay-rpc/kernel/rpc_call.h>

#include <iostream>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

using namespace galay::rpc;

static_assert(std::is_trivially_copyable_v<RpcCancellationToken>);
static_assert(!std::is_copy_constructible_v<RpcCancellationSource>);
static_assert(!std::is_move_constructible_v<RpcCancellationSource>);
static_assert(std::is_nothrow_move_constructible_v<RpcCancellationRegistration>);
static_assert(!std::is_copy_constructible_v<RpcCancellationRegistration>);

int main()
{
    int calls = 0;
    RpcCancellationToken empty;
    auto inert = empty.register_callback([&] { ++calls; });
    if (empty.cancelled() || calls != 0) return 1;

    RpcCancellationSource source;
    auto token = source.token();
    {
        auto detached = token.register_callback([&] { calls += 100; });
    }
    auto first = token.register_callback([&] { ++calls; });
    auto moved = std::move(first);
    auto second = token.register_callback([&] { calls += 100; });
    second.deactivate();
    second.deactivate();
    auto third = token.register_callback([&] { calls += 10; });
    RpcCancellationRegistration assigned;
    assigned = std::move(third);
    source.cancel();
    source.cancel();
    if (!token.cancelled() || calls != 11) return 2;
    auto late = token.register_callback([&] { ++calls; });
    if (calls != 12) return 3;

    // Callbacks may unregister, recursively cancel, and destroy themselves.
    RpcCancellationSource reentrant;
    auto skipped = reentrant.token().register_callback([&] { calls += 100; });
    std::optional<RpcCancellationRegistration> self;
    self.emplace(reentrant.token().register_callback([&] {
        skipped.deactivate();
        self.reset();
        reentrant.cancel();
        auto nested = reentrant.token().register_callback([&] { ++calls; });
        ++calls;
    }));
    reentrant.cancel();
    if (calls != 14 || self.has_value()) return 4;

    RpcCancellationRegistration survivor;
    {
        RpcCancellationSource local;
        survivor = local.token().register_callback([&] { calls += 100; });
    }
    survivor.deactivate();
    if (calls != 14) return 5;

    RpcCancellationSource reused;
    for (int i = 0; i < 100000; ++i) {
        auto registration = reused.token().register_callback([&] { ++calls; });
    }
    reused.cancel();
    if (calls != 14) return 6;

    // Moving non-head nodes and replacing a live registration repair both links.
    RpcCancellationSource relocation;
    auto tail = relocation.token().register_callback([&] { ++calls; });
    auto head = relocation.token().register_callback([&] { ++calls; });
    auto relocated_tail = std::move(tail);
    head = std::move(relocated_tail);
    relocation.cancel();
    if (calls != 15) return 7;

    // Vector growth relocates active registrations; every live callback runs once.
    RpcCancellationSource fanout;
    std::vector<RpcCancellationRegistration> registrations;
    for (int i = 0; i < 4096; ++i) {
        registrations.push_back(fanout.token().register_callback([&] { ++calls; }));
    }
    for (size_t i = 0; i < registrations.size(); i += 2) {
        registrations[i].deactivate();
    }
    fanout.cancel();
    if (calls != 15 + 2048) return 8;

    std::cout << "RPC cancellation ownership PASS\n";
    return 0;
}
