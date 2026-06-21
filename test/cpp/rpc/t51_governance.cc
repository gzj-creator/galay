#include <galay/cpp/galay-rpc/kernel/rpc_policy.h>

#include <chrono>
#include <expected>
#include <iostream>

using namespace galay::rpc;

namespace {

int expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

} // namespace

int main()
{
    RpcGovernancePolicy policy;
    policy.rate_limit.enabled = true;
    policy.rate_limit.capacity = 1;
    policy.rate_limit.refill_per_second = 0.0;
    policy.circuit_breaker.enabled = true;
    policy.circuit_breaker.failure_threshold = 2;
    policy.circuit_breaker.success_threshold = 1;
    policy.circuit_breaker.half_open_max_requests = 1;
    policy.circuit_breaker.reset_timeout = std::chrono::seconds(0);

    RpcGovernanceController controller(policy);

    auto first = controller.tryAcquire();
    if (auto rc = expect(first.has_value(), "first limiter permit was rejected")) {
        return rc;
    }
    auto limited = controller.tryAcquire();
    if (auto rc = expect(!limited.has_value() &&
                             limited.error().code() == RpcErrorCode::RATE_LIMITED,
                         "limiter rejection did not return RATE_LIMITED")) {
        return rc;
    }

    controller.release();
    auto after_release = controller.tryAcquire();
    if (auto rc = expect(after_release.has_value(), "released limiter permit was not reusable")) {
        return rc;
    }

    controller.onFailure();
    controller.onFailure();
    controller.release();

    auto open = controller.tryAcquire();
    if (auto rc = expect(!open.has_value() &&
                             open.error().code() == RpcErrorCode::CIRCUIT_OPEN,
                         "open breaker did not return CIRCUIT_OPEN")) {
        return rc;
    }

    auto probe = controller.tryAcquire();
    if (auto rc = expect(probe.has_value(), "half-open probe was not allowed after reset timeout")) {
        return rc;
    }
    auto second_probe = controller.tryAcquire();
    if (auto rc = expect(!second_probe.has_value() &&
                             second_probe.error().code() == RpcErrorCode::CIRCUIT_OPEN,
                         "half-open allowed more than one probe")) {
        return rc;
    }

    controller.onSuccess();
    controller.release();
    auto recovered = controller.tryAcquire();
    if (auto rc = expect(recovered.has_value(), "successful half-open probe did not close breaker")) {
        return rc;
    }

    std::cout << "RPC governance PASS\n";
    return 0;
}
