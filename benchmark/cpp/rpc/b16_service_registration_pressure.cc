#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/streamsvc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <string>

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<size_t> g_allocation_count{0};

} // namespace

void* operator new(std::size_t size)
{
    if (g_count_allocations.load(std::memory_order_relaxed)) {
        g_allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* ptr = std::malloc(size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

using namespace galay::rpc;

namespace {

class RegistrationBenchService final : public RpcService {
public:
    explicit RegistrationBenchService(std::string_view name)
        : RpcService(name) {}
};

struct Services {
    std::array<std::string, RpcServer::kMaxRegisteredServices> names;
    std::array<std::optional<RegistrationBenchService>, RpcServer::kMaxRegisteredServices> values;

    Services()
    {
        for (size_t i = 0; i < values.size(); ++i) {
            names[i] = "registration-bench-service-" + std::to_string(i);
            values[i].emplace(names[i]);
        }
    }
};

template<typename Server>
bool registerAll(Server& server, Services& services)
{
    for (auto& service : services.values) {
        auto registered = server.registerService(service.value());
        if (!registered.has_value()) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    size_t iterations = 10000;
    if (argc > 1) {
        iterations = std::max<size_t>(1, std::strtoull(argv[1], nullptr, 10));
    }

    Services services;
    RpcServerBuilder unary_builder;
    RpcStreamServerBuilder stream_builder;
    size_t errors = 0;
    size_t allocations = 0;
    const auto begin = std::chrono::steady_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        RpcServer unary_server = unary_builder.build();
        g_allocation_count.store(0, std::memory_order_relaxed);
        g_count_allocations.store(true, std::memory_order_release);
        const bool unary_ok = registerAll(unary_server, services);
        g_count_allocations.store(false, std::memory_order_release);
        allocations += g_allocation_count.load(std::memory_order_relaxed);
        if (!unary_ok) {
            ++errors;
        }

        RpcStreamServer stream_server = stream_builder.build();
        g_allocation_count.store(0, std::memory_order_relaxed);
        g_count_allocations.store(true, std::memory_order_release);
        const bool stream_ok = registerAll(stream_server, services);
        g_count_allocations.store(false, std::memory_order_release);
        allocations += g_allocation_count.load(std::memory_order_relaxed);
        if (!stream_ok) {
            ++errors;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::max(
        0.000001,
        std::chrono::duration<double>(end - begin).count());
    const size_t registrations = iterations *
        RpcServer::kMaxRegisteredServices * 2;

    std::cout << "rpc service registration pressure"
              << " iterations=" << iterations
              << " registrations=" << registrations
              << " allocations=" << allocations
              << " errors=" << errors
              << " registrations_per_s="
              << static_cast<double>(registrations) / seconds
              << "\n";
    return errors == 0 && allocations == 0 ? 0 : 1;
}
