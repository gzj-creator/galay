#include <galay/cpp/galay-rpc/kernel/rpc_policy.h>

#include <chrono>
#include <expected>
#include <iostream>
#include <optional>

using namespace galay::rpc;

namespace {

using RetryResult = std::expected<int, RpcError>;

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
    RpcRetryPolicy policy;
    policy.max_attempts = 3;
    policy.initial_backoff = std::chrono::milliseconds(0);
    policy.max_backoff = std::chrono::milliseconds(0);
    policy.jitter_ratio = 0.0;
    policy.retryable_errors = {RpcErrorCode::UNAVAILABLE};

    RpcCallOptions idempotent;
    idempotent.idempotent(true);

    int attempts = 0;
    auto succeeds_on_second = RpcRetryController::runSync<RetryResult>(
        policy,
        idempotent,
        [&]() -> RetryResult {
            ++attempts;
            if (attempts == 1) {
                return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE, "temporary"));
            }
            return 42;
        });
    if (auto rc = expect(succeeds_on_second.has_value() && succeeds_on_second.value() == 42,
                         "retryable unavailable did not succeed on second attempt")) {
        return rc;
    }
    if (auto rc = expect(attempts == 2, "retry attempt count was not exact for success path")) {
        return rc;
    }

    RpcCallOptions non_idempotent;
    attempts = 0;
    auto non_idempotent_result = RpcRetryController::runSync<RetryResult>(
        policy,
        non_idempotent,
        [&]() -> RetryResult {
            ++attempts;
            return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE, "temporary"));
        });
    if (auto rc = expect(!non_idempotent_result.has_value() &&
                             non_idempotent_result.error().code() == RpcErrorCode::UNAVAILABLE,
                         "non-idempotent retry returned wrong error")) {
        return rc;
    }
    if (auto rc = expect(attempts == 1, "non-idempotent call retried by default")) {
        return rc;
    }

    RpcCallOptions deadline_options;
    deadline_options.idempotent(true).deadline(RpcClock::now());
    attempts = 0;
    auto deadline_result = RpcRetryController::runSync<RetryResult>(
        policy,
        deadline_options,
        [&]() -> RetryResult {
            ++attempts;
            return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE, "temporary"));
        });
    if (auto rc = expect(!deadline_result.has_value() &&
                             deadline_result.error().code() == RpcErrorCode::DEADLINE_EXCEEDED,
                         "expired deadline did not stop retry loop")) {
        return rc;
    }
    if (auto rc = expect(attempts == 0, "operation ran despite expired deadline")) {
        return rc;
    }

    RpcCallOptions override_attempts;
    override_attempts.idempotent(true).maxAttempts(2);
    attempts = 0;
    auto exact_attempts = RpcRetryController::runSync<RetryResult>(
        policy,
        override_attempts,
        [&]() -> RetryResult {
            ++attempts;
            return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE, "temporary"));
        });
    if (auto rc = expect(!exact_attempts.has_value() &&
                             exact_attempts.error().code() == RpcErrorCode::UNAVAILABLE,
                         "retry exhaustion returned wrong error")) {
        return rc;
    }
    if (auto rc = expect(attempts == 2, "max attempts override was not exact")) {
        return rc;
    }

    std::cout << "RPC retry policy PASS\n";
    return 0;
}
