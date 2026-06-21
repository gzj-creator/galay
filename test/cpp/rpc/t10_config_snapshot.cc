#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_config.h>

#include <cstdint>
#include <iostream>
#include <string>

using namespace galay::rpc;

namespace {

void test_default_config_validates(test::TestResultWriter& writer)
{
    RpcRuntimeConfig config;
    auto result = validateRpcRuntimeConfig(config);

    writer.writeTestCase("default config validates", result.has_value());
}

void test_static_provider_create_validates(test::TestResultWriter& writer)
{
    RpcRuntimeConfig config;
    config.client.max_outstanding_requests = 64;
    const auto provider = StaticRpcConfigProvider::create(config);

    RpcRuntimeConfig invalid;
    invalid.client.max_outstanding_requests = 0;
    const auto rejected = StaticRpcConfigProvider::create(invalid);

    const bool passed = provider.has_value() &&
                        provider->snapshot() != nullptr &&
                        provider->snapshot()->version == 1 &&
                        provider->snapshot()->client.max_outstanding_requests == 64 &&
                        !rejected.has_value();

    writer.writeTestCase("static provider create validates config", passed);
}

void test_invalid_publish_keeps_old_snapshot(test::TestResultWriter& writer)
{
    InMemoryRpcConfigProvider provider;
    const auto before = provider.snapshot();

    RpcRuntimeConfig invalid_outstanding = *before;
    invalid_outstanding.client.max_outstanding_requests = 0;
    const auto bad_outstanding = provider.publish(invalid_outstanding);

    RpcRuntimeConfig invalid_message_size = *provider.snapshot();
    invalid_message_size.client.max_message_size = 0;
    const auto bad_message_size = provider.publish(invalid_message_size);

    RpcRuntimeConfig invalid_timeout = *provider.snapshot();
    invalid_timeout.deadline.default_timeout_ms = -2;
    const auto bad_timeout = provider.publish(invalid_timeout);

    const auto after = provider.snapshot();
    const bool passed = !bad_outstanding.has_value() &&
                        !bad_message_size.has_value() &&
                        !bad_timeout.has_value() &&
                        before == after &&
                        after->version == before->version &&
                        after->client.max_outstanding_requests != 0 &&
                        after->client.max_message_size != 0;

    writer.writeTestCase("invalid publish rejects and keeps old snapshot", passed);
}

void test_publish_increments_version_and_next_read_sees_value(test::TestResultWriter& writer)
{
    InMemoryRpcConfigProvider provider;
    const auto before = provider.snapshot();

    RpcRuntimeConfig config = *before;
    config.client.max_outstanding_requests = before->client.max_outstanding_requests + 7;
    const auto published = provider.publish(config);
    const auto after = provider.snapshot();

    const bool passed = published.has_value() &&
                        after != nullptr &&
                        after->version > before->version &&
                        published.value()->version == after->version &&
                        after->client.max_outstanding_requests ==
                            before->client.max_outstanding_requests + 7;

    writer.writeTestCase("publish increments version and read sees new value", passed);
}

void test_route_policy_lookup(test::TestResultWriter& writer)
{
    RpcRuntimeConfig config;
    config.default_route_policy.timeout_ms = 250;
    config.default_route_policy.max_outstanding_requests = 16;

    RpcRoutePolicy policy;
    policy.timeout_ms = 42;
    policy.max_outstanding_requests = 3;
    config.route_policies.emplace(
        RpcRouteKey{"EchoService", "Echo", RpcCallMode::UNARY},
        policy);

    const auto exact = findRpcRoutePolicy(config, "EchoService", "Echo", RpcCallMode::UNARY);
    const auto missing = findRpcRoutePolicy(config, "EchoService", "Echo", RpcCallMode::BIDI_STREAMING);

    const bool passed = exact.timeout_ms == 42 &&
                        exact.max_outstanding_requests == 3 &&
                        missing.timeout_ms == 250 &&
                        missing.max_outstanding_requests == 16;

    writer.writeTestCase("route policy lookup falls back to default", passed);
}

void test_repeated_snapshot_reads_smoke(test::TestResultWriter& writer)
{
    InMemoryRpcConfigProvider provider;
    bool passed = true;
    std::uint64_t last_version = 0;

    for (int i = 0; i < 10000; ++i) {
        const auto snapshot = provider.snapshot();
        if (snapshot == nullptr || snapshot->version < last_version) {
            passed = false;
            break;
        }
        last_version = snapshot->version;
    }

    writer.writeTestCase("repeated snapshot reads are non-empty", passed);
}

} // namespace

int main()
{
    test::TestResultWriter writer("t10_config_snapshot.result");
    std::cout << "Running RPC config snapshot tests...\n";

    test_default_config_validates(writer);
    test_static_provider_create_validates(writer);
    test_invalid_publish_keeps_old_snapshot(writer);
    test_publish_increments_version_and_next_read_sees_value(writer);
    test_route_policy_lookup(writer);
    test_repeated_snapshot_reads_smoke(writer);

    writer.writeSummary();
    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";
    return writer.failed() > 0 ? 1 : 0;
}
