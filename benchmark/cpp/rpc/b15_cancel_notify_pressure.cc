#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>

#include <charconv>
#include <chrono>
#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace galay::rpc;

namespace {

std::expected<size_t, const char*> parse_size(const char* text)
{
    const size_t length = std::char_traits<char>::length(text);
    const char* end = text + length;
    size_t value = 0;
    const auto [ptr, ec] = std::from_chars(text, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected("invalid size argument");
    }
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    size_t requests = 10000;
    if (argc > 1) {
        auto parsed = parse_size(argv[1]);
        if (!parsed.has_value()) {
            std::cerr << parsed.error() << "\n";
            return 1;
        }
        requests = *parsed;
    }
    if (requests == 0) {
        std::cerr << "requests must be greater than zero\n";
        return 1;
    }

    // Stable source addresses; all registrations live on this benchmark owner.
    auto sources = std::make_unique<RpcCancellationSource[]>(requests);
    std::vector<std::shared_ptr<RpcChannelPendingCall>> pendings;
    std::vector<RpcCancellationRegistration> registrations;
    pendings.reserve(requests);
    registrations.reserve(requests);

    const auto register_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < requests; ++i) {
        auto pending = std::make_shared<RpcChannelPendingCall>();
        pending->request_id = static_cast<uint32_t>(i + 1);
        auto token = sources[i].token();
        auto registration = token.register_callback([locked = pending.get()] {
            if (locked->completed.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            const bool notified = locked->waiter.notify(
                RpcCallResult(std::unexpected(
                    RpcError(RpcErrorCode::CANCELLED, "benchmark cancel"))));
            if (!notified) {
                std::cerr << "duplicate cancel notify for request "
                          << locked->request_id << "\n";
            }
        });
        pendings.push_back(std::move(pending));
        registrations.push_back(std::move(registration));
    }
    const auto register_stop = std::chrono::steady_clock::now();

    const auto notify_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < requests; ++i) {
        sources[i].cancel();
    }
    const auto notify_stop = std::chrono::steady_clock::now();

    size_t cancelled = 0;
    for (const auto& pending : pendings) {
        if (pending->completed.load(std::memory_order_acquire)) {
            ++cancelled;
        }
    }

    const double register_us = std::chrono::duration<double, std::micro>(register_stop - register_start).count();
    const double notify_us = std::chrono::duration<double, std::micro>(notify_stop - notify_start).count();
    const double cancelled_per_second = notify_us > 0.0
        ? static_cast<double>(cancelled) * 1000000.0 / notify_us
        : 0.0;

    std::cout << "RPC cancel notify pressure\nrequests=" << requests
              << "\nregistered=" << pendings.size()
              << "\ncancelled=" << cancelled
              << "\nregister_us=" << register_us
              << "\nnotify_us=" << notify_us
              << "\ncancelled_per_second=" << cancelled_per_second
              << "\n";

    if (cancelled != pendings.size()) {
        return 1;
    }
    return 0;
}
