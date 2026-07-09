#include <galay/cpp/galay-etcd/async/client.h>
#include <galay/cpp/galay-etcd/cluster/etcd_cluster_client.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{

galay::etcd::EtcdProductionConfig makeProduction(int64_t retry_attempts)
{
    galay::etcd::EtcdProductionConfig production;
    production.endpoints = {
        "http://127.0.0.1:2379",
        "http://127.0.0.1:22379",
        "http://127.0.0.1:32379",
    };
    production.endpoint_policy = galay::etcd::EtcdEndpointPolicy::RoundRobin;
    production.retry.attempts = static_cast<size_t>(retry_attempts);
    production.retry.initial_backoff = std::chrono::milliseconds(1);
    production.retry.max_backoff = std::chrono::milliseconds(8);
    production.retry.jitter = false;
    return production;
}

int64_t parseIterations(int argc, char** argv)
{
    constexpr int64_t kDefaultIterations = 100000;
    if (argc < 2) {
        return kDefaultIterations;
    }

    char* end = nullptr;
    const long long parsed = std::strtoll(argv[1], &end, 10);
    if (end == argv[1] || parsed <= 0) {
        return kDefaultIterations;
    }
    return parsed;
}

} // namespace

int main(int argc, char** argv)
{
    const int64_t iterations = parseIterations(argc, argv);
    uint64_t checksum = 0;

    const auto pipeline_begin = std::chrono::steady_clock::now();
    std::vector<galay::etcd::PipelineOp> moved_ops;
    moved_ops.reserve(static_cast<size_t>(iterations));
    for (int64_t i = 0; i < iterations; ++i) {
        auto op = galay::etcd::PipelineOp::Put(
            "key-" + std::to_string(i),
            "value-" + std::to_string(i),
            i);
        auto clone = op.clone();
        checksum += clone.key.size();
        checksum += clone.value.size();
        moved_ops.push_back(std::move(clone));
    }
    const auto pipeline_end = std::chrono::steady_clock::now();

    const auto state_begin = std::chrono::steady_clock::now();
    galay::etcd::EtcdClusterState state(makeProduction(iterations + 4));
    for (int64_t i = 0; i < iterations; ++i) {
        state.recordRequest();
        auto selected = state.selectEndpoint();
        if (!selected.has_value()) {
            std::cerr << "selectEndpoint failed\n";
            return 1;
        }
        if ((i % 5) == 0) {
            state.markFailure(
                *selected,
                galay::etcd::EtcdError(galay::etcd::EtcdErrorType::Connection, "bench"),
                true);
        } else {
            state.markSuccess(*selected);
        }

        auto clone = state.clone();
        auto clone_selected = clone.selectEndpoint();
        if (!clone_selected.has_value()) {
            std::cerr << "clone selectEndpoint failed\n";
            return 1;
        }
        checksum += *clone_selected;
    }
    const auto state_end = std::chrono::steady_clock::now();

    const auto async_begin = std::chrono::steady_clock::now();
    auto async_cluster = galay::etcd::AsyncEtcdClusterClientBuilder()
        .productionConfig(makeProduction(iterations + 4))
        .build();
    auto attempt = async_cluster.beginAttempt().await_resume();
    if (!attempt.has_value()) {
        std::cerr << "beginAttempt failed: " << attempt.error().message() << '\n';
        return 1;
    }
    for (int64_t i = 0; i < iterations; ++i) {
        galay::etcd::EtcdError error(
            (i % 3) == 0 ? galay::etcd::EtcdErrorType::Server
                         : galay::etcd::EtcdErrorType::Connection,
            "bench");
        auto next = async_cluster.nextAttempt(*attempt, std::move(error)).await_resume();
        if (!next.has_value()) {
            std::cerr << "nextAttempt failed: " << next.error().message() << '\n';
            return 1;
        }
        checksum += next->endpoint_index;
        checksum += next->attempt;
        attempt = std::move(next);
    }
    async_cluster.markSuccess(*attempt);
    const auto async_end = std::chrono::steady_clock::now();

    const auto pipeline_us =
        std::chrono::duration_cast<std::chrono::microseconds>(pipeline_end - pipeline_begin).count();
    const auto state_us =
        std::chrono::duration_cast<std::chrono::microseconds>(state_end - state_begin).count();
    const auto async_us =
        std::chrono::duration_cast<std::chrono::microseconds>(async_end - async_begin).count();

    std::cout << "Iterations        : " << iterations << '\n';
    std::cout << "Pipeline clone us : " << pipeline_us << '\n';
    std::cout << "State clone us    : " << state_us << '\n';
    std::cout << "Async retry us    : " << async_us << '\n';
    std::cout << "Moved ops         : " << moved_ops.size() << '\n';
    std::cout << "Checksum          : " << checksum << '\n';
    return 0;
}
