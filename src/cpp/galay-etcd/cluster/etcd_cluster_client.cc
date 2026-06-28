#include "etcd_cluster_client.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace galay::etcd
{

EtcdClusterState::EtcdClusterState(EtcdProductionConfig production)
    : m_production(std::move(production))
{
    for (const auto& endpoint : m_production.endpoints) {
        m_snapshots.push_back(EtcdEndpointHealthSnapshot{.endpoint = endpoint});
    }
}

std::optional<size_t> EtcdClusterState::selectEndpoint()
{
    if (m_snapshots.empty()) {
        return std::nullopt;
    }

    if (m_production.endpoint_policy == EtcdEndpointPolicy::StickyLeader) {
        if (const auto leader = selectStickyLeaderEndpoint(); leader.has_value()) {
            return leader;
        }
    }

    if (m_production.endpoint_policy == EtcdEndpointPolicy::RoundRobin) {
        const size_t start = m_round_robin_cursor % m_snapshots.size();
        for (size_t offset = 0; offset < m_snapshots.size(); ++offset) {
            const size_t index = (start + offset) % m_snapshots.size();
            if (m_snapshots[index].state != EtcdEndpointHealthState::Unhealthy) {
                m_round_robin_cursor = (index + 1) % m_snapshots.size();
                return index;
            }
        }
        m_round_robin_cursor = (start + 1) % m_snapshots.size();
        return start;
    }

    for (size_t index = 0; index < m_snapshots.size(); ++index) {
        if (m_snapshots[index].state != EtcdEndpointHealthState::Unhealthy) {
            return index;
        }
    }
    return size_t{0};
}

void EtcdClusterState::recordRequest()
{
    ++m_stats.requests;
}

void EtcdClusterState::recordRetry()
{
    ++m_stats.retries;
}

void EtcdClusterState::markSuccess(size_t index, std::chrono::system_clock::time_point when)
{
    if (index >= m_snapshots.size()) {
        return;
    }

    auto& snapshot = m_snapshots[index];
    snapshot.state = EtcdEndpointHealthState::Healthy;
    snapshot.last_error.reset();
    snapshot.last_success_time = when;
    snapshot.consecutive_failures = 0;

    if (m_production.endpoint_policy == EtcdEndpointPolicy::StickyLeader) {
        m_leader_hint = index;
    }
}

void EtcdClusterState::markFailure(
    size_t index,
    EtcdError error,
    bool endpoint_unhealthy,
    std::chrono::system_clock::time_point when)
{
    if (index >= m_snapshots.size()) {
        return;
    }

    ++m_stats.request_failures;
    auto& snapshot = m_snapshots[index];
    snapshot.last_error = std::move(error);
    snapshot.last_failure_time = when;
    ++snapshot.consecutive_failures;

    if (!endpoint_unhealthy) {
        return;
    }

    if (snapshot.state != EtcdEndpointHealthState::Unhealthy && hasAlternativeEndpoint(index)) {
        ++m_stats.endpoint_switches;
    }
    snapshot.state = EtcdEndpointHealthState::Unhealthy;
    if (m_leader_hint == index) {
        m_leader_hint.reset();
    }
}

std::vector<size_t> EtcdClusterState::collectDueProbes(std::chrono::system_clock::time_point now)
{
    std::vector<size_t> due;
    due.reserve(m_snapshots.size());

    for (size_t index = 0; index < m_snapshots.size(); ++index) {
        auto& snapshot = m_snapshots[index];
        if (snapshot.state != EtcdEndpointHealthState::Unhealthy) {
            continue;
        }

        const auto throttle_anchor = snapshot.last_probe_time.value_or(
            snapshot.last_failure_time.value_or(std::chrono::system_clock::time_point{}));
        if (throttle_anchor + m_production.health_interval > now) {
            continue;
        }

        snapshot.last_probe_time = now;
        due.push_back(index);
    }

    return due;
}

void EtcdClusterState::markProbeSuccess(size_t index, std::chrono::system_clock::time_point when)
{
    if (index >= m_snapshots.size()) {
        return;
    }

    m_snapshots[index].last_probe_time = when;
    markSuccess(index, when);
}

void EtcdClusterState::markProbeFailure(
    size_t index,
    EtcdError error,
    std::chrono::system_clock::time_point when)
{
    if (index >= m_snapshots.size()) {
        return;
    }

    auto& snapshot = m_snapshots[index];
    snapshot.last_probe_time = when;
    snapshot.last_error = std::move(error);
    snapshot.last_failure_time = when;
    ++snapshot.consecutive_failures;
    snapshot.state = EtcdEndpointHealthState::Unhealthy;
    if (m_leader_hint == index) {
        m_leader_hint.reset();
    }
}

EtcdRetryDecision EtcdClusterState::classifyRetry(const EtcdError& error, size_t attempt) const
{
    if (attempt + 1 >= maxAttempts()) {
        return EtcdRetryDecision::FailFast;
    }

    switch (error.type()) {
    case EtcdErrorType::Connection:
    case EtcdErrorType::Timeout:
    case EtcdErrorType::Send:
    case EtcdErrorType::Recv:
    case EtcdErrorType::NotConnected:
        return EtcdRetryDecision::RetryNextEndpoint;
    case EtcdErrorType::Http:
    case EtcdErrorType::Server:
        return EtcdRetryDecision::RetrySameEndpoint;
    case EtcdErrorType::Success:
    case EtcdErrorType::InvalidEndpoint:
    case EtcdErrorType::InvalidParam:
    case EtcdErrorType::Parse:
    case EtcdErrorType::Internal:
        return EtcdRetryDecision::FailFast;
    }
    return EtcdRetryDecision::FailFast;
}

std::chrono::milliseconds EtcdClusterState::backoffForAttempt(size_t attempt) const
{
    auto backoff = m_production.retry.initial_backoff;
    for (size_t i = 0; i < attempt; ++i) {
        if (backoff >= m_production.retry.max_backoff) {
            return m_production.retry.max_backoff;
        }
        backoff *= 2;
    }
    return std::min(backoff, m_production.retry.max_backoff);
}

const std::vector<EtcdEndpointHealthSnapshot>& EtcdClusterState::getEndpointSnapshots() const
{
    return m_snapshots;
}

EtcdClientStats EtcdClusterState::getStats() const
{
    return m_stats;
}

size_t EtcdClusterState::maxAttempts() const
{
    return std::max<size_t>(1, m_production.retry.attempts);
}

std::optional<size_t> EtcdClusterState::selectStickyLeaderEndpoint() const
{
    if (m_leader_hint.has_value() && *m_leader_hint < m_snapshots.size() &&
        m_snapshots[*m_leader_hint].state != EtcdEndpointHealthState::Unhealthy) {
        return m_leader_hint;
    }

    for (size_t index = 0; index < m_snapshots.size(); ++index) {
        if (m_snapshots[index].state != EtcdEndpointHealthState::Unhealthy) {
            return index;
        }
    }

    if (m_leader_hint.has_value() && *m_leader_hint < m_snapshots.size()) {
        return m_leader_hint;
    }

    return size_t{0};
}

bool EtcdClusterState::hasAlternativeEndpoint(size_t excluded_index) const
{
    for (size_t index = 0; index < m_snapshots.size(); ++index) {
        if (index == excluded_index) {
            continue;
        }
        if (m_snapshots[index].state != EtcdEndpointHealthState::Unhealthy) {
            return true;
        }
    }
    return false;
}

EtcdClusterClientBuilder& EtcdClusterClientBuilder::endpoint(std::string endpoint)
{
    m_config.endpoint = std::move(endpoint);
    return *this;
}

EtcdClusterClientBuilder& EtcdClusterClientBuilder::apiPrefix(std::string prefix)
{
    m_config.api_prefix = std::move(prefix);
    return *this;
}

EtcdClusterClientBuilder& EtcdClusterClientBuilder::requestTimeout(std::chrono::milliseconds timeout)
{
    m_config.request_timeout = timeout;
    return *this;
}

EtcdClusterClientBuilder& EtcdClusterClientBuilder::productionConfig(EtcdProductionConfig config)
{
    if (!config.endpoints.empty()) {
        m_config.endpoint = config.endpoints.front();
    }
    m_config.production = std::move(config);
    return *this;
}

EtcdClusterClientBuilder& EtcdClusterClientBuilder::config(EtcdConfig config)
{
    m_config = std::move(config);
    return *this;
}

EtcdClusterClient::EtcdClusterClient(EtcdConfig config)
    : m_config(std::move(config))
    , m_state([this] {
        EtcdProductionConfig production = m_config.production;
        if (production.endpoints.empty() && !m_config.endpoint.empty()) {
            production.endpoints.push_back(m_config.endpoint);
        }
        return production;
    }())
{
}

EtcdBoolResult EtcdClusterClient::put(
    const std::string& key,
    const std::string& value,
    std::optional<int64_t> lease_id)
{
    return execute<EtcdBoolResult>([&](EtcdClient& client) {
        return client.put(key, value, lease_id);
    });
}

EtcdGetResult EtcdClusterClient::get(
    const std::string& key,
    bool prefix,
    std::optional<int64_t> limit)
{
    return execute<EtcdGetResult>([&](EtcdClient& client) {
        return client.get(key, prefix, limit);
    });
}

EtcdDeleteResult EtcdClusterClient::del(const std::string& key, bool prefix)
{
    return execute<EtcdDeleteResult>([&](EtcdClient& client) {
        return client.del(key, prefix);
    });
}

EtcdLeaseGrantResult EtcdClusterClient::grantLease(int64_t ttl_seconds)
{
    return execute<EtcdLeaseGrantResult>([&](EtcdClient& client) {
        return client.grantLease(ttl_seconds);
    });
}

EtcdLeaseGrantResult EtcdClusterClient::keepAliveOnce(int64_t lease_id)
{
    return execute<EtcdLeaseGrantResult>([&](EtcdClient& client) {
        return client.keepAliveOnce(lease_id);
    });
}

EtcdPipelineResult EtcdClusterClient::pipeline(std::span<const PipelineOp> operations)
{
    return execute<EtcdPipelineResult>([&](EtcdClient& client) {
        return client.pipeline(operations);
    });
}

EtcdPipelineResult EtcdClusterClient::pipeline(std::vector<PipelineOp> operations)
{
    return pipeline(std::span<const PipelineOp>(operations.data(), operations.size()));
}

const std::vector<EtcdEndpointHealthSnapshot>& EtcdClusterClient::getEndpointSnapshots() const
{
    return m_state.getEndpointSnapshots();
}

EtcdClientStats EtcdClusterClient::getStats() const
{
    return m_state.getStats();
}

void EtcdClusterClient::runDueHealthProbes()
{
    for (const size_t index : m_state.collectDueProbes()) {
        EtcdClient client(configForEndpoint(index));
        auto connect_result = client.connect();
        if (connect_result.has_value()) {
            auto close_result = client.close();
            if (close_result.has_value()) {
                m_state.markProbeSuccess(index);
            } else {
                m_state.markProbeFailure(index, close_result.error());
            }
            continue;
        }
        m_state.markProbeFailure(index, connect_result.error());
    }
}

EtcdConfig EtcdClusterClient::configForEndpoint(size_t index) const
{
    EtcdConfig config = m_config;
    const auto& snapshots = m_state.getEndpointSnapshots();
    if (index < snapshots.size()) {
        config.endpoint = snapshots[index].endpoint;
    }
    return config;
}

} // namespace galay::etcd
