#include "etcd_cluster_client.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>
#include <concurrentqueue/moodycamel/concurrentqueue.h>

namespace galay::etcd
{

namespace
{

size_t configuredEndpointCount(const EtcdConfig& config)
{
    if (!config.production.endpoints.empty()) {
        return config.production.endpoints.size();
    }
    return config.endpoint.empty() ? 0 : 1;
}

size_t queueCapacityForConfig(const EtcdConfig& config)
{
    const size_t endpoint_count = configuredEndpointCount(config);
    const size_t per_endpoint = config.production.connections_per_endpoint;
    if (endpoint_count == 0 || per_endpoint == 0 ||
        endpoint_count > std::numeric_limits<size_t>::max() / per_endpoint) {
        return 1;
    }
    return endpoint_count * per_endpoint;
}

} // namespace

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
    case EtcdErrorType::PoolExhausted:
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

namespace details
{

struct EtcdClientPoolState
{
    explicit EtcdClientPoolState(EtcdConfig config)
        : idle_clients(queueCapacityForConfig(config))
    {
        std::vector<std::string> endpoints = config.production.endpoints;
        if (endpoints.empty() && !config.endpoint.empty()) {
            endpoints.push_back(config.endpoint);
        }
        if (endpoints.empty()) {
            init_error.emplace(
                EtcdErrorType::InvalidEndpoint,
                "cluster endpoints are empty");
            return;
        }

        const size_t per_endpoint = config.production.connections_per_endpoint;
        if (per_endpoint == 0) {
            init_error.emplace(
                EtcdErrorType::InvalidParam,
                "connections_per_endpoint must be greater than zero");
            return;
        }
        if (endpoints.size() > std::numeric_limits<size_t>::max() / per_endpoint) {
            init_error.emplace(
                EtcdErrorType::InvalidParam,
                "configured client pool size overflows size_t");
            return;
        }

        const size_t total = endpoints.size() * per_endpoint;
        clients.reserve(total);
        for (const auto& endpoint : endpoints) {
            for (size_t index = 0; index < per_endpoint; ++index) {
                EtcdConfig client_config = config;
                client_config.endpoint = endpoint;
                auto client = std::make_unique<EtcdClient>(std::move(client_config));
                EtcdClient* const client_ptr = client.get();
                clients.push_back(std::move(client));
                const bool enqueued = idle_clients.enqueue(client_ptr);
                if (!enqueued) {
                    queue_failed.store(true, std::memory_order_release);
                    init_error.emplace(
                        EtcdErrorType::Internal,
                        "failed to initialize sync client pool queue");
                    return;
                }
            }
        }
    }

    void release(EtcdClient* client) noexcept
    {
        if (client == nullptr) {
            return;
        }
        const bool enqueued = idle_clients.enqueue(client);
        if (!enqueued) {
            queue_failed.store(true, std::memory_order_release);
            return;
        }
        borrowed_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    alignas(64) moodycamel::ConcurrentQueue<EtcdClient*> idle_clients;
    alignas(64) std::atomic<size_t> borrowed_count{0};
    alignas(64) std::atomic<bool> queue_failed{false};
    std::optional<EtcdError> init_error;
    std::vector<std::unique_ptr<EtcdClient>> clients;
};

} // namespace details

EtcdClientLease::EtcdClientLease(
    std::shared_ptr<details::EtcdClientPoolState> state,
    EtcdClient* client) noexcept
    : m_state(std::move(state))
    , m_client(client)
{
}

EtcdClientLease::EtcdClientLease(EtcdClientLease&& other) noexcept
    : m_state(std::move(other.m_state))
    , m_client(std::exchange(other.m_client, nullptr))
{
}

EtcdClientLease& EtcdClientLease::operator=(EtcdClientLease&& other) noexcept
{
    if (this != &other) {
        release();
        m_state = std::move(other.m_state);
        m_client = std::exchange(other.m_client, nullptr);
    }
    return *this;
}

EtcdClientLease::~EtcdClientLease()
{
    release();
}

EtcdClient* EtcdClientLease::get() const noexcept
{
    return m_client;
}

EtcdClient& EtcdClientLease::operator*() const noexcept
{
    return *m_client;
}

EtcdClient* EtcdClientLease::operator->() const noexcept
{
    return m_client;
}

EtcdClientLease::operator bool() const noexcept
{
    return m_client != nullptr;
}

void EtcdClientLease::release() noexcept
{
    EtcdClient* const client = std::exchange(m_client, nullptr);
    auto state = std::move(m_state);
    if (state != nullptr && client != nullptr) {
        state->release(client);
    }
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

EtcdClusterClientBuilder& EtcdClusterClientBuilder::connectionsPerEndpoint(size_t count)
{
    m_config.production.connections_per_endpoint = count;
    return *this;
}

EtcdClusterClientBuilder& EtcdClusterClientBuilder::config(EtcdConfig config)
{
    m_config = std::move(config);
    return *this;
}

EtcdClusterClient::EtcdClusterClient(EtcdConfig config)
    : m_state(std::make_shared<details::EtcdClientPoolState>(std::move(config)))
{
}

EtcdClientAcquireResult EtcdClusterClient::tryAcquire()
{
    if (m_state == nullptr) {
        return std::unexpected(
            EtcdError(EtcdErrorType::Internal, "sync client pool is moved from"));
    }
    if (m_state->init_error.has_value()) {
        return std::unexpected(*m_state->init_error);
    }
    if (m_state->queue_failed.load(std::memory_order_acquire)) {
        return std::unexpected(
            EtcdError(EtcdErrorType::Internal, "sync client pool queue failed"));
    }

    EtcdClient* client = nullptr;
    const bool dequeued = m_state->idle_clients.try_dequeue(client);
    if (!dequeued || client == nullptr) {
        return std::unexpected(
            EtcdError(EtcdErrorType::PoolExhausted, "no idle sync EtcdClient"));
    }
    m_state->borrowed_count.fetch_add(1, std::memory_order_acq_rel);
    return EtcdClientLease(m_state, client);
}

EtcdClientAcquireResult EtcdClusterClient::acquireConnected()
{
    auto lease = tryAcquire();
    if (!lease.has_value()) {
        return std::unexpected(lease.error());
    }
    if (!lease->get()->connected()) {
        auto connected = lease->get()->connect();
        if (!connected.has_value()) {
            return std::unexpected(connected.error());
        }
    }
    return lease;
}

size_t EtcdClusterClient::size() const noexcept
{
    return m_state == nullptr ? 0 : m_state->clients.size();
}

size_t EtcdClusterClient::idleCount() const noexcept
{
    if (m_state == nullptr) {
        return 0;
    }
    const size_t borrowed = m_state->borrowed_count.load(std::memory_order_acquire);
    return borrowed >= m_state->clients.size()
        ? 0
        : m_state->clients.size() - borrowed;
}

} // namespace galay::etcd
