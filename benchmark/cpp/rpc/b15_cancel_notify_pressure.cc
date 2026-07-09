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

std::expected<size_t, const char*> parseSize(const char* text)
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
    size_t requests = 10000;
    if (argc > 1) {
        auto parsed = parseSize(argv[1]);
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

    std::vector<RpcCancellationSource> sources;
    std::vector<std::shared_ptr<RpcChannelPendingCall>> pendings;
    sources.reserve(requests);
    pendings.reserve(requests);

    size_t register_errors = 0;
    const auto register_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < requests; ++i) {
        RpcCancellationSource source;
        auto pending = std::make_shared<RpcChannelPendingCall>();
        pending->request_id = static_cast<uint32_t>(i + 1);
        auto token = source.token();
        std::weak_ptr<RpcChannelPendingCall> weak_pending = pending;
        auto registration = token.registerCallback([weak_pending]() {
            auto locked = weak_pending.lock();
            if (!locked) {
                return;
            }
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
        if (!registration) {
            ++register_errors;
            continue;
        }
        pending->cancellation_registration = std::move(registration);
        sources.push_back(std::move(source));
        pendings.push_back(std::move(pending));
    }
    const auto register_stop = std::chrono::steady_clock::now();

    const auto notify_start = std::chrono::steady_clock::now();
    for (const auto& source : sources) {
        source.cancel();
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
              << "\nregister_errors=" << register_errors
              << "\ncancelled=" << cancelled
              << "\nregister_us=" << register_us
              << "\nnotify_us=" << notify_us
              << "\ncancelled_per_second=" << cancelled_per_second
              << "\n";

    if (register_errors != 0 || cancelled != pendings.size()) {
        return 1;
    }
    return 0;
}
