#include "topology_client.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>
#include <unordered_map>

namespace galay::redis
{
    namespace
    {
        std::string valueToString(const RedisValue& value)
        {
            if (value.isString()) {
                return value.toString();
            }
            if (value.isStatus()) {
                return value.toStatus();
            }
            if (value.isInteger()) {
                return std::to_string(value.toInteger());
            }
            return "";
        }

        bool parseInt32(std::string_view input, int32_t* out)
        {
            if (!out || input.empty()) {
                return false;
            }
            int32_t value = 0;
            const auto* begin = input.data();
            const auto* end = begin + input.size();
            const auto result = std::from_chars(begin, end, value);
            if (result.ec != std::errc() || result.ptr != end) {
                return false;
            }
            *out = value;
            return true;
        }

        RedisError ioErrorToRedisError(const galay::kernel::IOError& io_error)
        {
            if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR, io_error.message());
            }
            if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, io_error.message());
            }
            return RedisError(RedisErrorType::REDIS_ERROR_TYPE_NETWORK_ERROR, io_error.message());
        }

        std::optional<std::pair<std::string, int32_t>> parseHostPort(const std::string& host_port)
        {
            if (host_port.empty()) {
                return std::nullopt;
            }

            const auto at_pos = host_port.find('@');
            const auto endpoint = (at_pos == std::string::npos) ? host_port : host_port.substr(0, at_pos);
            const auto colon_pos = endpoint.rfind(':');
            if (colon_pos == std::string::npos || colon_pos == 0 || colon_pos == endpoint.size() - 1) {
                return std::nullopt;
            }

            const std::string host = endpoint.substr(0, colon_pos);
            int32_t port = 0;
            if (!parseInt32(std::string_view(endpoint).substr(colon_pos + 1), &port)) {
                return std::nullopt;
            }
            return std::make_pair(host, port);
        }

        RedisEncodedCommand encodeCommand(std::string_view cmd,
                                                    const std::vector<std::string>& args,
                                                    size_t expected_replies = 1)
        {
            RedisCommandBuilder builder;
            return builder.command(cmd, args, expected_replies);
        }

        RedisEncodedCommand encodeCommand(std::string_view cmd,
                                                    std::initializer_list<std::string> args,
                                                    size_t expected_replies = 1)
        {
            return encodeCommand(cmd, std::vector<std::string>(args), expected_replies);
        }

        RedisConnectOperation connectToAddress(RedisClient* client, const RedisNodeAddress& address)
        {
            RedisConnectOptions options;
            options.username = address.username;
            options.password = address.password;
            options.db_index = address.db_index;
            options.version = address.version;
            return client->connect(address.host, address.port, std::move(options));
        }

#ifdef GALAY_SSL_FEATURE_ENABLED
        std::string buildRedisUrl(std::string_view scheme, const RedisNodeAddress& address)
        {
            std::string url;
            url.reserve(scheme.size() + address.host.size() + 32 + address.username.size() + address.password.size());
            url.append(scheme.data(), scheme.size());
            url.append("://");
            if (!address.username.empty() || !address.password.empty()) {
                url.append(address.username);
                if (!address.password.empty()) {
                    url.push_back(':');
                    url.append(address.password);
                }
                url.push_back('@');
            }

            const bool needs_ipv6_brackets =
                address.host.find(':') != std::string::npos &&
                address.host.find('[') == std::string::npos &&
                address.host.find(']') == std::string::npos;
            if (needs_ipv6_brackets) {
                url.push_back('[');
                url.append(address.host);
                url.push_back(']');
            } else {
                url.append(address.host);
            }

            url.push_back(':');
            url.append(std::to_string(address.port));

            if (address.db_index != 0) {
                url.push_back('/');
                url.append(std::to_string(address.db_index));
            }

            return url;
        }

        detail::RedissConnectOperation connectToAddress(RedissClient* client, const RedisNodeAddress& address)
        {
            return client->connect(buildRedisUrl("rediss", address));
        }
#endif

        template <typename AwaitResult>
        RedisCommandResult unwrapRedisCommandTask(AwaitResult&& result)
        {
            if (!result) {
                return std::unexpected(RedisError(
                    REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    std::string(result.error().message())));
            }
            return std::move(result.value());
        }
    }

    RedisMasterSlaveClient::RedisMasterSlaveClient(IOScheduler* scheduler, AsyncRedisConfig config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
    {
    }

    RedisClient* RedisMasterSlaveClient::ensureMaster()
    {
        if (!m_master) {
            m_master = std::make_unique<RedisClient>(m_scheduler, m_config);
        }
        return m_master.get();
    }

    RedisConnectOperation RedisMasterSlaveClient::connectMaster(const RedisNodeAddress& master)
    {
        m_master_address = master;
        m_master_connected = false;
        auto* master_client = ensureMaster();
        return connectToAddress(master_client, master);
    }

    RedisConnectOperation RedisMasterSlaveClient::addReplica(const RedisNodeAddress& replica)
    {
        auto client = std::make_unique<RedisClient>(m_scheduler, m_config);
        auto* raw_client = client.get();
        m_replicas.push_back(std::move(client));
        m_replica_addresses.push_back(replica);
        m_replica_connected.push_back(false);
        return connectToAddress(raw_client, replica);
    }

    RedisConnectOperation RedisMasterSlaveClient::addSentinel(const RedisNodeAddress& sentinel)
    {
        NodeHandle node;
        node.address = sentinel;
        node.client = std::make_unique<RedisClient>(m_scheduler, m_config);
        node.connected = false;
        auto* raw_client = node.client.get();
        m_sentinels.push_back(std::move(node));
        return connectToAddress(raw_client, sentinel);
    }

    void RedisMasterSlaveClient::setSentinelMasterName(std::string master_name)
    {
        if (!master_name.empty()) {
            m_sentinel_master_name = std::move(master_name);
        }
    }

    void RedisMasterSlaveClient::setAutoRetryAttempts(size_t attempts) noexcept
    {
        m_auto_retry_attempts = std::max<size_t>(1, attempts);
    }

    Task<RedisCommandResult> RedisMasterSlaveClient::refreshFromSentinel()
    {
        co_return unwrapRedisCommandTask(co_await refreshSentinelTask());
    }

    Task<RedisCommandResult> RedisMasterSlaveClient::execute(
        const std::string& cmd,
        const std::vector<std::string>& args,
        bool prefer_read,
        bool auto_retry)
    {
        const size_t max_attempts = auto_retry ? std::max<size_t>(1, m_auto_retry_attempts) : size_t(1);
        co_return unwrapRedisCommandTask(co_await runAutoTask(prefer_read, cmd, args, max_attempts));
    }

    RedisExchangeOperation RedisMasterSlaveClient::batch(
        std::span<const RedisCommandView> commands,
        bool prefer_read)
    {
        RedisClient* client = prefer_read ? chooseReadClient() : ensureMaster();
        return client->batch(commands);
    }

    RedisClient* RedisMasterSlaveClient::chooseReadClient()
    {
        if (m_replicas.empty()) {
            return ensureMaster();
        }

        const size_t base_index = m_read_cursor % m_replicas.size();
        for (size_t i = 0; i < m_replicas.size(); ++i) {
            const size_t idx = (base_index + i) % m_replicas.size();
            auto* replica = m_replicas[idx].get();
            if (replica) {
                m_read_cursor = idx + 1;
                return replica;
            }
        }

        return ensureMaster();
    }

    RedisClient& RedisMasterSlaveClient::master()
    {
        return *ensureMaster();
    }

    std::optional<std::reference_wrapper<RedisClient>> RedisMasterSlaveClient::replica(size_t index)
    {
        if (index >= m_replicas.size() || !m_replicas[index]) {
            return std::nullopt;
        }
        return *m_replicas[index];
    }

    size_t RedisMasterSlaveClient::replicaCount() const noexcept
    {
        return m_replicas.size();
    }

    bool RedisMasterSlaveClient::isRetryableConnectionError(const RedisError& error) const noexcept
    {
        switch (error.type()) {
            case REDIS_ERROR_TYPE_CONNECTION_ERROR:
            case REDIS_ERROR_TYPE_TIMEOUT_ERROR:
            case REDIS_ERROR_TYPE_SEND_ERROR:
            case REDIS_ERROR_TYPE_RECV_ERROR:
            case REDIS_ERROR_TYPE_NETWORK_ERROR:
            case REDIS_ERROR_TYPE_CONNECTION_CLOSED:
                return true;
            default:
                return false;
        }
    }

    RedisClient* RedisMasterSlaveClient::chooseAvailableSentinel()
    {
        for (auto& sentinel : m_sentinels) {
            if (sentinel.client) {
                return sentinel.client.get();
            }
        }
        return nullptr;
    }

    bool RedisMasterSlaveClient::parseMasterAddressReply(const std::vector<RedisValue>& values,
                                                         RedisNodeAddress* out_addr) const
    {
        if (!out_addr || values.empty() || !values[0].isArray()) {
            return false;
        }

        const auto parts = values[0].toArray();
        if (parts.size() < 2) {
            return false;
        }

        const auto host = valueToString(parts[0]);
        const auto port_str = valueToString(parts[1]);
        int32_t port = 0;
        if (host.empty() || !parseInt32(port_str, &port)) {
            return false;
        }

        *out_addr = m_master_address;
        out_addr->host = host;
        out_addr->port = port;
        return true;
    }

    bool RedisMasterSlaveClient::parseReplicaListReply(const std::vector<RedisValue>& values,
                                                       std::vector<RedisNodeAddress>* replicas) const
    {
        if (!replicas || values.empty() || !values[0].isArray()) {
            return false;
        }

        replicas->clear();
        const auto rows = values[0].toArray();
        for (const auto& row : rows) {
            if (!row.isArray()) {
                continue;
            }

            const auto kvs = row.toArray();
            std::unordered_map<std::string, std::string> fields;
            for (size_t i = 0; i + 1 < kvs.size(); i += 2) {
                const auto key = valueToString(kvs[i]);
                const auto val = valueToString(kvs[i + 1]);
                if (!key.empty()) {
                    fields[key] = val;
                }
            }

            const auto it_ip = fields.find("ip");
            const auto it_port = fields.find("port");
            if (it_ip == fields.end() || it_port == fields.end()) {
                continue;
            }

            auto flags = fields["flags"];
            if (flags.find("s_down") != std::string::npos ||
                flags.find("o_down") != std::string::npos ||
                flags.find("disconnected") != std::string::npos) {
                continue;
            }

            int32_t port = 0;
            if (!parseInt32(it_port->second, &port)) {
                continue;
            }

            RedisNodeAddress replica = m_master_address;
            replica.host = it_ip->second;
            replica.port = port;
            replicas->push_back(std::move(replica));
        }

        return true;
    }

    Task<RedisCommandResult> RedisMasterSlaveClient::refreshSentinelTask()
    {
        if (m_sentinels.empty()) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No available sentinel"));
        }

        NodeHandle* selected = nullptr;
        for (auto& sentinel : m_sentinels) {
            if (!sentinel.client) {
                continue;
            }
            if (!sentinel.connected) {
                auto connect_result = co_await connectToAddress(sentinel.client.get(), sentinel.address);
                if (!connect_result) {
                    continue;
                }
                sentinel.connected = true;
            }
            selected = &sentinel;
            break;
        }

        if (!selected) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No available sentinel"));
        }

        std::vector<std::string> master_query_args;
        master_query_args.reserve(2);
        master_query_args.emplace_back("get-master-addr-by-name");
        master_query_args.push_back(m_sentinel_master_name);
        auto master_reply = co_await selected->client->command(
            encodeCommand("SENTINEL", master_query_args));
        if (!master_reply) {
            co_return std::unexpected(master_reply.error());
        }
        if (!master_reply.value().has_value()) {
            co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                 "Sentinel master reply missing"));
        }

        RedisNodeAddress latest_master = m_master_address;
        if (!parseMasterAddressReply(master_reply.value().value(), &latest_master)) {
            co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                 "Failed to parse sentinel master address"));
        }

        const bool master_changed = latest_master.host != m_master_address.host ||
                                    latest_master.port != m_master_address.port;
        m_master_address = latest_master;
        if (master_changed) {
            m_master_connected = false;
            if (m_master) {
                co_await m_master->close();
            }
            m_master = std::make_unique<RedisClient>(m_scheduler, m_config);
        }

        std::vector<std::string> replicas_query_args;
        replicas_query_args.reserve(2);
        replicas_query_args.emplace_back("replicas");
        replicas_query_args.push_back(m_sentinel_master_name);
        auto replicas_reply = co_await selected->client->command(
            encodeCommand("SENTINEL", replicas_query_args));
        if (replicas_reply && replicas_reply.value().has_value()) {
            std::vector<RedisNodeAddress> parsed_replicas;
            if (parseReplicaListReply(replicas_reply.value().value(), &parsed_replicas)) {
                auto next_replica_addresses = std::move(parsed_replicas);
                std::vector<std::unique_ptr<RedisClient>> next_replicas;
                std::vector<bool> next_replica_connected;
                next_replicas.reserve(next_replica_addresses.size());
                next_replica_connected.reserve(next_replica_addresses.size());
                for (const auto& addr : next_replica_addresses) {
                    auto client = std::make_unique<RedisClient>(m_scheduler, m_config);
                    auto connect_res = co_await connectToAddress(client.get(), addr);
                    next_replica_connected.push_back(connect_res.has_value());
                    next_replicas.push_back(std::move(client));
                }
                m_replica_addresses = std::move(next_replica_addresses);
                m_replica_connected = std::move(next_replica_connected);
                m_replicas = std::move(next_replicas);
            }
        }

        auto& master_values = master_reply.value().value();
        co_return std::move(master_values);
    }

    Task<RedisCommandResult> RedisMasterSlaveClient::runAutoTask(
        bool prefer_read,
        std::string cmd,
        std::vector<std::string> args,
        size_t max_attempts)
    {
        RedisCommandResult final_result = std::unexpected(
            RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "Command execution not started"));

        max_attempts = std::max<size_t>(1, max_attempts);
        for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
            RedisClient* target = prefer_read ? chooseReadClient() : ensureMaster();
            bool is_master_target = (target == ensureMaster());
            size_t replica_index = static_cast<size_t>(-1);
            if (!is_master_target) {
                for (size_t i = 0; i < m_replicas.size(); ++i) {
                    if (m_replicas[i].get() == target) {
                        replica_index = i;
                        break;
                    }
                }
            }

            if (is_master_target && !m_master_connected) {
                auto connect_result = co_await connectToAddress(ensureMaster(), m_master_address);
                if (!connect_result) {
                    final_result = std::unexpected(connect_result.error());
                } else {
                    m_master_connected = true;
                }
            } else if (!is_master_target &&
                       replica_index != static_cast<size_t>(-1) &&
                       replica_index < m_replica_addresses.size() &&
                       replica_index < m_replica_connected.size() &&
                       !m_replica_connected[replica_index]) {
                const auto& addr = m_replica_addresses[replica_index];
                auto connect_result = co_await connectToAddress(target, addr);
                if (!connect_result) {
                    final_result = std::unexpected(connect_result.error());
                } else {
                    m_replica_connected[replica_index] = true;
                }
            }

            auto exec_result = co_await target->command(encodeCommand(cmd, args));
            if (exec_result && exec_result.value().has_value()) {
                auto& exec_values = exec_result.value().value();
                final_result = std::move(exec_values);
                break;
            }

            if (!exec_result) {
                final_result = std::unexpected(exec_result.error());
            } else {
                final_result = std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "Empty response from redis command"));
            }

            if (!final_result.has_value() &&
                isRetryableConnectionError(final_result.error()) &&
                !m_sentinels.empty() &&
                (attempt + 1) < max_attempts) {
                auto refresh_result = co_await refreshFromSentinel();
                if (refresh_result) {
                    continue;
                }
            }
            break;
        }

        co_return final_result;
    }

    RedisClusterClient::RedisClusterClient(IOScheduler* scheduler, AsyncRedisConfig config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
    {
        m_slot_owner.fill(-1);
    }

    RedisConnectOperation RedisClusterClient::addNode(const RedisClusterNodeAddress& node)
    {
        ClusterNode cluster_node;
        cluster_node.address = node;
        cluster_node.client = std::make_unique<RedisClient>(m_scheduler, m_config);
        cluster_node.connected = false;

        auto* raw_client = cluster_node.client.get();
        m_nodes.push_back(std::move(cluster_node));
        const int idx = static_cast<int>(m_nodes.size() - 1);
        for (uint16_t slot = node.slot_start; slot <= node.slot_end; ++slot) {
            m_slot_owner[slot] = idx;
            if (slot == 16383) break;
        }
        m_slot_cache_ready = true;

        return connectToAddress(raw_client, node);
    }

    void RedisClusterClient::setSlotRange(size_t node_index, uint16_t slot_start, uint16_t slot_end)
    {
        if (node_index >= m_nodes.size()) {
            return;
        }
        m_nodes[node_index].address.slot_start = slot_start;
        m_nodes[node_index].address.slot_end = slot_end;

        const int idx = static_cast<int>(node_index);
        for (uint16_t slot = slot_start; slot <= slot_end; ++slot) {
            m_slot_owner[slot] = idx;
            if (slot == 16383) break;
        }
        m_slot_cache_ready = true;
    }

    void RedisClusterClient::setAutoRefreshInterval(std::chrono::milliseconds interval)
    {
        if (interval.count() > 0) {
            m_auto_refresh_interval = interval;
        }
    }

    Task<RedisCommandResult> RedisClusterClient::refreshSlots()
    {
        co_return unwrapRedisCommandTask(co_await refreshSlotsTask());
    }

    Task<RedisCommandResult> RedisClusterClient::execute(
        const std::string& cmd,
        const std::vector<std::string>& args,
        std::string routing_key,
        bool auto_retry)
    {
        if (routing_key.empty() && !args.empty()) {
            routing_key = args.front();
        }
        const bool force_key_routing = !routing_key.empty();
        const size_t max_attempts = auto_retry ? size_t(5) : size_t(1);
        co_return unwrapRedisCommandTask(co_await runAutoTask(routing_key,
                                                                   cmd,
                                                                   args,
                                                                   force_key_routing,
                                                                   auto_retry,
                                                                   max_attempts));
    }

    RedisExchangeOperation RedisClusterClient::batch(
        std::span<const RedisCommandView> commands,
        std::string routing_key)
    {
        if (routing_key.empty() && !commands.empty() && !commands.front().args.empty()) {
            routing_key.assign(commands.front().args.front());
        }

        RedisClient* node = routing_key.empty() ? chooseNodeBySlot(0) : chooseNodeByKey(routing_key);
        if (!node) {
            if (!m_fallback_client) {
                m_fallback_client = std::make_unique<RedisClient>(m_scheduler, m_config);
            }
            return m_fallback_client->batch(commands);
        }

        return node->batch(commands);
    }

    uint16_t RedisClusterClient::keySlot(const std::string& key) const
    {
        auto hash_key = extractHashTag(key);
        return crc16(reinterpret_cast<const uint8_t*>(hash_key.data()), hash_key.size()) % 16384;
    }

    size_t RedisClusterClient::nodeCount() const noexcept
    {
        return m_nodes.size();
    }

    std::optional<std::reference_wrapper<RedisClient>> RedisClusterClient::node(size_t index)
    {
        if (index >= m_nodes.size() || !m_nodes[index].client) {
            return std::nullopt;
        }
        return *m_nodes[index].client;
    }

    std::optional<RedisClusterClient::RedirectInfo> RedisClusterClient::parseRedirect(const RedisValue& value)
    {
        if (!value.isError()) {
            return std::nullopt;
        }

        const auto msg = value.toError();
        if (msg.empty()) {
            return std::nullopt;
        }

        std::vector<std::string> parts;
        size_t start = 0;
        while (start < msg.size()) {
            while (start < msg.size() && std::isspace(static_cast<unsigned char>(msg[start])) != 0) {
                ++start;
            }
            if (start >= msg.size()) {
                break;
            }
            size_t end = start;
            while (end < msg.size() && std::isspace(static_cast<unsigned char>(msg[end])) == 0) {
                ++end;
            }
            parts.push_back(msg.substr(start, end - start));
            start = end;
        }

        if (parts.size() < 3) {
            return std::nullopt;
        }

        RedirectInfo info;
        if (parts[0] == "MOVED") {
            info.type = RedirectInfo::Type::Moved;
        } else if (parts[0] == "ASK") {
            info.type = RedirectInfo::Type::Ask;
        } else {
            return std::nullopt;
        }

        int32_t slot = 0;
        if (!parseInt32(parts[1], &slot) || slot < 0 || slot > 16383) {
            return std::nullopt;
        }
        info.slot = static_cast<uint16_t>(slot);

        auto endpoint = parseHostPort(parts[2]);
        if (!endpoint.has_value()) {
            return std::nullopt;
        }
        info.host = endpoint->first;
        info.port = endpoint->second;
        return info;
    }

    RedisClusterClient::ClusterNode* RedisClusterClient::chooseNodeHandleBySlot(uint16_t slot) noexcept
    {
        if (m_nodes.empty()) {
            return nullptr;
        }

        const int owner = m_slot_owner[slot];
        if (owner >= 0 && static_cast<size_t>(owner) < m_nodes.size()) {
            return &m_nodes[owner];
        }

        for (auto& node : m_nodes) {
            if (slot >= node.address.slot_start && slot <= node.address.slot_end) {
                return &node;
            }
        }

        return &m_nodes.front();
    }

    RedisClusterClient::ClusterNode* RedisClusterClient::chooseNodeHandleByKey(const std::string& key) noexcept
    {
        return chooseNodeHandleBySlot(keySlot(key));
    }

    RedisClient* RedisClusterClient::chooseNodeBySlot(uint16_t slot) noexcept
    {
        auto* node = chooseNodeHandleBySlot(slot);
        return node ? node->client.get() : nullptr;
    }

    RedisClient* RedisClusterClient::chooseNodeByKey(const std::string& key) noexcept
    {
        auto* node = chooseNodeHandleByKey(key);
        return node ? node->client.get() : nullptr;
    }

    RedisClusterClient::ClusterNode* RedisClusterClient::findOrCreateNode(const std::string& host, int32_t port)
    {
        for (auto& node : m_nodes) {
            if (node.address.host == host && node.address.port == port) {
                return &node;
            }
        }

        ClusterNode node;
        node.address.host = host;
        node.address.port = port;
        node.address.slot_start = 0;
        node.address.slot_end = 16383;
        node.client = std::make_unique<RedisClient>(m_scheduler, m_config);
        node.connected = false;
        m_nodes.push_back(std::move(node));
        return &m_nodes.back();
    }

    bool RedisClusterClient::applyClusterSlots(const std::vector<RedisValue>& values, std::string* error_message)
    {
        if (values.empty() || !values[0].isArray()) {
            if (error_message) {
                *error_message = "CLUSTER SLOTS response is not array";
            }
            return false;
        }

        auto new_owner = m_slot_owner;
        new_owner.fill(-1);

        const auto slots_rows = values[0].toArray();
        for (const auto& row : slots_rows) {
            if (!row.isArray()) {
                continue;
            }
            const auto row_values = row.toArray();
            if (row_values.size() < 3 || !row_values[0].isInteger() || !row_values[1].isInteger() ||
                !row_values[2].isArray()) {
                continue;
            }

            const int64_t start = row_values[0].toInteger();
            const int64_t end = row_values[1].toInteger();
            if (start < 0 || end < 0 || start > end || end > 16383) {
                continue;
            }

            const auto master_node = row_values[2].toArray();
            if (master_node.size() < 2) {
                continue;
            }

            const auto host = valueToString(master_node[0]);
            const auto port_string = valueToString(master_node[1]);
            int32_t port = 0;
            if (host.empty() || !parseInt32(port_string, &port)) {
                continue;
            }

            auto* node = findOrCreateNode(host, port);
            if (!node) {
                continue;
            }

            node->address.slot_start = static_cast<uint16_t>(std::min<int64_t>(node->address.slot_start, start));
            node->address.slot_end = static_cast<uint16_t>(std::max<int64_t>(node->address.slot_end, end));

            const int owner = static_cast<int>(node - m_nodes.data());
            for (int64_t slot = start; slot <= end; ++slot) {
                new_owner[slot] = owner;
            }
        }

        m_slot_owner = new_owner;
        m_slot_cache_ready = true;
        m_last_refresh_time = std::chrono::steady_clock::now();
        return true;
    }

    bool RedisClusterClient::shouldAutoRefresh() const noexcept
    {
        if (!m_slot_cache_ready) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        return (now - m_last_refresh_time) >= m_auto_refresh_interval;
    }

    Task<RedisCommandResult> RedisClusterClient::refreshSlotsTask()
    {
        if (m_nodes.empty()) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No cluster node configured"));
        }

        ClusterNode* seed = nullptr;
        for (auto& node : m_nodes) {
            if (node.client) {
                seed = &node;
                break;
            }
        }
        if (!seed) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No valid cluster client"));
        }

        if (!seed->connected) {
            auto connect_result = co_await connectToAddress(seed->client.get(), seed->address);
            if (!connect_result) {
                co_return std::unexpected(connect_result.error());
            }
            seed->connected = true;
        }

        RedisCommandBuilder command_builder;
        auto slots_result = co_await seed->client->command(command_builder.clusterSlots());
        if (!slots_result) {
            co_return std::unexpected(slots_result.error());
        }
        if (!slots_result.value().has_value()) {
            co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                 "CLUSTER SLOTS returned empty payload"));
        }

        std::string parse_error;
        auto& slots_values = slots_result.value().value();
        auto values = std::move(slots_values);
        if (!applyClusterSlots(values, &parse_error)) {
            co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR, parse_error));
        }

        co_return std::move(values);
    }

    Task<RedisCommandResult> RedisClusterClient::runAutoTask(
        std::string routing_key,
        std::string cmd,
        std::vector<std::string> args,
        bool force_key_routing,
        bool allow_auto_refresh,
        size_t max_attempts)
    {
        if (m_nodes.empty()) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No cluster node configured"));
        }

        if (allow_auto_refresh && shouldAutoRefresh()) {
            auto refresh_result = co_await refreshSlots();
            if (!refresh_result) {
                // 刷新失败时，继续使用本地缓存做一次最佳努力路由
            }
        }

        std::vector<std::string> cmd_parts;
        cmd_parts.reserve(1 + args.size());
        cmd_parts.push_back(cmd);
        cmd_parts.insert(cmd_parts.end(), args.begin(), args.end());

        max_attempts = std::max<size_t>(1, max_attempts);
        for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
            ClusterNode* target = nullptr;
            if (force_key_routing && !routing_key.empty()) {
                target = chooseNodeHandleByKey(routing_key);
            } else if (!args.empty()) {
                target = chooseNodeHandleByKey(args.front());
            } else {
                target = chooseNodeHandleBySlot(0);
            }

            if (!target || !target->client) {
                co_return std::unexpected(
                    RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No target cluster node"));
            }

            if (!target->connected) {
                auto connect_result = co_await connectToAddress(target->client.get(), target->address);
                if (!connect_result) {
                    co_return std::unexpected(connect_result.error());
                }
                target->connected = true;
            }

            auto exec_result = co_await target->client->command(encodeCommand(cmd, args));
            if (!exec_result) {
                co_return std::unexpected(exec_result.error());
            }
            if (!exec_result.value().has_value() || exec_result.value()->empty()) {
                co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                     "Cluster command returned empty payload"));
            }

            auto& exec_values = exec_result.value().value();
            auto values = std::move(exec_values);
            const auto redirect = parseRedirect(values.front());
            if (!redirect.has_value()) {
                co_return std::move(values);
            }

            auto* redirect_node = findOrCreateNode(redirect->host, redirect->port);
            if (!redirect_node || !redirect_node->client) {
                co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR,
                                                     "Redirect target node unavailable"));
            }

            if (!redirect_node->connected) {
                auto connect_result = co_await connectToAddress(redirect_node->client.get(),
                                                                redirect_node->address);
                if (!connect_result) {
                    co_return std::unexpected(connect_result.error());
                }
                redirect_node->connected = true;
            }

            if (redirect->type == RedirectInfo::Type::Moved) {
                m_slot_owner[redirect->slot] = static_cast<int>(redirect_node - m_nodes.data());
                if (allow_auto_refresh) {
                    auto refresh_result = co_await refreshSlots();
                    (void)refresh_result;
                }
                continue;
            }

            if (redirect->type == RedirectInfo::Type::Ask) {
                RedisCommandBuilder asking_builder;
                asking_builder.reserve(2, 1 + cmd_parts.size(), 64);
                asking_builder.append("ASKING");

                std::vector<std::string_view> cmd_args;
                cmd_args.reserve(cmd_parts.size() > 1 ? cmd_parts.size() - 1 : 0);
                for (size_t i = 1; i < cmd_parts.size(); ++i) {
                    cmd_args.emplace_back(cmd_parts[i]);
                }
                asking_builder.append(cmd_parts[0], std::span<const std::string_view>(cmd_args));

                auto asking_result = co_await redirect_node->client->batch(asking_builder.commands());
                if (!asking_result) {
                    co_return std::unexpected(asking_result.error());
                }
                if (!asking_result.value().has_value() || asking_result.value()->size() < 2) {
                    co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "ASK redirect response invalid"));
                }

                auto& ask_values_ref = asking_result.value().value();
                auto ask_values = std::move(ask_values_ref);
                std::vector<RedisValue> final_values;
                final_values.push_back(std::move(ask_values[1]));
                const auto chained_redirect = parseRedirect(final_values.front());
                if (chained_redirect.has_value()) {
                    if (chained_redirect->type == RedirectInfo::Type::Moved) {
                        m_slot_owner[chained_redirect->slot] = static_cast<int>(redirect_node - m_nodes.data());
                    }
                    continue;
                }
                co_return std::move(final_values);
            }
        }

        co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_COMMAND_ERROR,
                                             "Exceeded redirect retry limit"));
    }

    uint16_t RedisClusterClient::crc16(const uint8_t* data, size_t len)
    {
        uint16_t crc = 0;
        for (size_t i = 0; i < len; ++i) {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int j = 0; j < 8; ++j) {
                if ((crc & 0x8000) != 0) {
                    crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
                } else {
                    crc = static_cast<uint16_t>(crc << 1);
                }
            }
        }
        return crc;
    }

    std::string RedisClusterClient::extractHashTag(const std::string& key)
    {
        const auto left = key.find('{');
        if (left == std::string::npos) {
            return key;
        }

        const auto right = key.find('}', left + 1);
        if (right == std::string::npos || right == left + 1) {
            return key;
        }

        return key.substr(left + 1, right - left - 1);
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    RedissMasterSlaveClient::RedissMasterSlaveClient(IOScheduler* scheduler,
                                                     AsyncRedisConfig config,
                                                     RedissClientConfig tls_config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
        , m_tls_config(std::move(tls_config))
    {
    }

    RedissClient* RedissMasterSlaveClient::ensureMaster()
    {
        if (!m_master) {
            m_master = std::make_unique<RedissClient>(m_scheduler, m_config, m_tls_config);
        }
        return m_master.get();
    }

    detail::RedissConnectOperation RedissMasterSlaveClient::connectMaster(const RedisNodeAddress& master)
    {
        m_master_address = master;
        m_master_connected = false;
        auto* master_client = ensureMaster();
        return connectToAddress(master_client, master);
    }

    detail::RedissConnectOperation RedissMasterSlaveClient::addReplica(const RedisNodeAddress& replica)
    {
        auto client = std::make_unique<RedissClient>(m_scheduler, m_config, m_tls_config);
        auto* raw_client = client.get();
        m_replicas.push_back(std::move(client));
        m_replica_addresses.push_back(replica);
        m_replica_connected.push_back(false);
        return connectToAddress(raw_client, replica);
    }

    detail::RedissConnectOperation RedissMasterSlaveClient::addSentinel(const RedisNodeAddress& sentinel)
    {
        NodeHandle node;
        node.address = sentinel;
        node.client = std::make_unique<RedissClient>(m_scheduler, m_config, m_tls_config);
        node.connected = false;
        auto* raw_client = node.client.get();
        m_sentinels.push_back(std::move(node));
        return connectToAddress(raw_client, sentinel);
    }

    void RedissMasterSlaveClient::setSentinelMasterName(std::string master_name)
    {
        if (!master_name.empty()) {
            m_sentinel_master_name = std::move(master_name);
        }
    }

    void RedissMasterSlaveClient::setAutoRetryAttempts(size_t attempts) noexcept
    {
        m_auto_retry_attempts = std::max<size_t>(1, attempts);
    }

    Task<RedisCommandResult> RedissMasterSlaveClient::refreshFromSentinel()
    {
        co_return unwrapRedisCommandTask(co_await refreshSentinelTask());
    }

    Task<RedisCommandResult> RedissMasterSlaveClient::execute(
        const std::string& cmd,
        const std::vector<std::string>& args,
        bool prefer_read,
        bool auto_retry)
    {
        const size_t max_attempts = auto_retry ? std::max<size_t>(1, m_auto_retry_attempts) : size_t(1);
        co_return unwrapRedisCommandTask(co_await runAutoTask(prefer_read, cmd, args, max_attempts));
    }

    detail::RedissExchangeOperation RedissMasterSlaveClient::batch(
        std::span<const RedisCommandView> commands,
        bool prefer_read)
    {
        RedissClient* client = prefer_read ? chooseReadClient() : ensureMaster();
        return client->batch(commands);
    }

    RedissClient* RedissMasterSlaveClient::chooseReadClient()
    {
        if (m_replicas.empty()) {
            return ensureMaster();
        }

        const size_t base_index = m_read_cursor % m_replicas.size();
        for (size_t i = 0; i < m_replicas.size(); ++i) {
            const size_t idx = (base_index + i) % m_replicas.size();
            auto* replica = m_replicas[idx].get();
            if (replica) {
                m_read_cursor = idx + 1;
                return replica;
            }
        }

        return ensureMaster();
    }

    RedissClient& RedissMasterSlaveClient::master()
    {
        return *ensureMaster();
    }

    std::optional<std::reference_wrapper<RedissClient>> RedissMasterSlaveClient::replica(size_t index)
    {
        if (index >= m_replicas.size() || !m_replicas[index]) {
            return std::nullopt;
        }
        return *m_replicas[index];
    }

    size_t RedissMasterSlaveClient::replicaCount() const noexcept
    {
        return m_replicas.size();
    }

    bool RedissMasterSlaveClient::isRetryableConnectionError(const RedisError& error) const noexcept
    {
        switch (error.type()) {
            case REDIS_ERROR_TYPE_CONNECTION_ERROR:
            case REDIS_ERROR_TYPE_TIMEOUT_ERROR:
            case REDIS_ERROR_TYPE_SEND_ERROR:
            case REDIS_ERROR_TYPE_RECV_ERROR:
            case REDIS_ERROR_TYPE_NETWORK_ERROR:
            case REDIS_ERROR_TYPE_CONNECTION_CLOSED:
                return true;
            default:
                return false;
        }
    }

    bool RedissMasterSlaveClient::parseMasterAddressReply(const std::vector<RedisValue>& values,
                                                          RedisNodeAddress* out_addr) const
    {
        if (!out_addr || values.empty() || !values[0].isArray()) {
            return false;
        }

        const auto parts = values[0].toArray();
        if (parts.size() < 2) {
            return false;
        }

        const auto host = valueToString(parts[0]);
        const auto port_str = valueToString(parts[1]);
        int32_t port = 0;
        if (host.empty() || !parseInt32(port_str, &port)) {
            return false;
        }

        *out_addr = m_master_address;
        out_addr->host = host;
        out_addr->port = port;
        return true;
    }

    bool RedissMasterSlaveClient::parseReplicaListReply(const std::vector<RedisValue>& values,
                                                        std::vector<RedisNodeAddress>* replicas) const
    {
        if (!replicas || values.empty() || !values[0].isArray()) {
            return false;
        }

        replicas->clear();
        const auto rows = values[0].toArray();
        for (const auto& row : rows) {
            if (!row.isArray()) {
                continue;
            }

            const auto kvs = row.toArray();
            std::unordered_map<std::string, std::string> fields;
            for (size_t i = 0; i + 1 < kvs.size(); i += 2) {
                const auto key = valueToString(kvs[i]);
                const auto val = valueToString(kvs[i + 1]);
                if (!key.empty()) {
                    fields[key] = val;
                }
            }

            const auto it_ip = fields.find("ip");
            const auto it_port = fields.find("port");
            if (it_ip == fields.end() || it_port == fields.end()) {
                continue;
            }

            auto flags = fields["flags"];
            if (flags.find("s_down") != std::string::npos ||
                flags.find("o_down") != std::string::npos ||
                flags.find("disconnected") != std::string::npos) {
                continue;
            }

            int32_t port = 0;
            if (!parseInt32(it_port->second, &port)) {
                continue;
            }

            RedisNodeAddress replica = m_master_address;
            replica.host = it_ip->second;
            replica.port = port;
            replicas->push_back(std::move(replica));
        }

        return true;
    }

    Task<RedisCommandResult> RedissMasterSlaveClient::refreshSentinelTask()
    {
        if (m_sentinels.empty()) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No available TLS sentinel"));
        }

        NodeHandle* selected = nullptr;
        for (auto& sentinel : m_sentinels) {
            if (!sentinel.client) {
                continue;
            }
            if (!sentinel.connected) {
                auto connect_result = co_await connectToAddress(sentinel.client.get(), sentinel.address);
                if (!connect_result) {
                    continue;
                }
                sentinel.connected = true;
            }
            selected = &sentinel;
            break;
        }

        if (!selected) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No available TLS sentinel"));
        }

        std::vector<std::string> master_query_args;
        master_query_args.reserve(2);
        master_query_args.emplace_back("get-master-addr-by-name");
        master_query_args.push_back(m_sentinel_master_name);
        auto master_reply = co_await selected->client->command(
            encodeCommand("SENTINEL", master_query_args));
        if (!master_reply) {
            co_return std::unexpected(master_reply.error());
        }
        if (!master_reply.value().has_value()) {
            co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                 "TLS sentinel master reply missing"));
        }

        RedisNodeAddress latest_master = m_master_address;
        if (!parseMasterAddressReply(master_reply.value().value(), &latest_master)) {
            co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                 "Failed to parse TLS sentinel master address"));
        }

        const bool master_changed = latest_master.host != m_master_address.host ||
                                    latest_master.port != m_master_address.port;
        m_master_address = latest_master;
        if (master_changed) {
            m_master_connected = false;
            if (m_master) {
                co_await m_master->close();
            }
            m_master = std::make_unique<RedissClient>(m_scheduler, m_config, m_tls_config);
        }

        std::vector<std::string> replicas_query_args;
        replicas_query_args.reserve(2);
        replicas_query_args.emplace_back("replicas");
        replicas_query_args.push_back(m_sentinel_master_name);
        auto replicas_reply = co_await selected->client->command(
            encodeCommand("SENTINEL", replicas_query_args));
        if (replicas_reply && replicas_reply.value().has_value()) {
            std::vector<RedisNodeAddress> parsed_replicas;
            if (parseReplicaListReply(replicas_reply.value().value(), &parsed_replicas)) {
                auto next_replica_addresses = std::move(parsed_replicas);
                std::vector<std::unique_ptr<RedissClient>> next_replicas;
                std::vector<bool> next_replica_connected;
                next_replicas.reserve(next_replica_addresses.size());
                next_replica_connected.reserve(next_replica_addresses.size());
                for (const auto& addr : next_replica_addresses) {
                    auto client = std::make_unique<RedissClient>(m_scheduler, m_config, m_tls_config);
                    auto connect_res = co_await connectToAddress(client.get(), addr);
                    next_replica_connected.push_back(connect_res.has_value());
                    next_replicas.push_back(std::move(client));
                }
                m_replica_addresses = std::move(next_replica_addresses);
                m_replica_connected = std::move(next_replica_connected);
                m_replicas = std::move(next_replicas);
            }
        }

        auto& master_values = master_reply.value().value();
        co_return std::move(master_values);
    }

    Task<RedisCommandResult> RedissMasterSlaveClient::runAutoTask(
        bool prefer_read,
        std::string cmd,
        std::vector<std::string> args,
        size_t max_attempts)
    {
        RedisCommandResult final_result = std::unexpected(
            RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "TLS command execution not started"));

        max_attempts = std::max<size_t>(1, max_attempts);
        for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
            RedissClient* target = prefer_read ? chooseReadClient() : ensureMaster();
            bool is_master_target = (target == ensureMaster());
            size_t replica_index = static_cast<size_t>(-1);
            if (!is_master_target) {
                for (size_t i = 0; i < m_replicas.size(); ++i) {
                    if (m_replicas[i].get() == target) {
                        replica_index = i;
                        break;
                    }
                }
            }

            if (is_master_target && !m_master_connected) {
                auto connect_result = co_await connectToAddress(ensureMaster(), m_master_address);
                if (!connect_result) {
                    final_result = std::unexpected(connect_result.error());
                } else {
                    m_master_connected = true;
                }
            } else if (!is_master_target &&
                       replica_index != static_cast<size_t>(-1) &&
                       replica_index < m_replica_addresses.size() &&
                       replica_index < m_replica_connected.size() &&
                       !m_replica_connected[replica_index]) {
                const auto& addr = m_replica_addresses[replica_index];
                auto connect_result = co_await connectToAddress(target, addr);
                if (!connect_result) {
                    final_result = std::unexpected(connect_result.error());
                } else {
                    m_replica_connected[replica_index] = true;
                }
            }

            auto exec_result = co_await target->command(encodeCommand(cmd, args));
            if (exec_result && exec_result.value().has_value()) {
                auto& exec_values = exec_result.value().value();
                final_result = std::move(exec_values);
                break;
            }

            if (!exec_result) {
                final_result = std::unexpected(exec_result.error());
            } else {
                final_result = std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "Empty response from TLS redis command"));
            }

            if (!final_result.has_value() &&
                isRetryableConnectionError(final_result.error()) &&
                !m_sentinels.empty() &&
                (attempt + 1) < max_attempts) {
                auto refresh_result = co_await refreshFromSentinel();
                if (refresh_result) {
                    continue;
                }
            }
            break;
        }

        co_return final_result;
    }

    RedissClusterClient::RedissClusterClient(IOScheduler* scheduler,
                                             AsyncRedisConfig config,
                                             RedissClientConfig tls_config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
        , m_tls_config(std::move(tls_config))
    {
        m_slot_owner.fill(-1);
    }

    detail::RedissConnectOperation RedissClusterClient::addNode(const RedisClusterNodeAddress& node)
    {
        ClusterNode cluster_node;
        cluster_node.address = node;
        cluster_node.client = std::make_unique<RedissClient>(m_scheduler, m_config, m_tls_config);
        cluster_node.connected = false;

        auto* raw_client = cluster_node.client.get();
        m_nodes.push_back(std::move(cluster_node));
        const int idx = static_cast<int>(m_nodes.size() - 1);
        for (uint16_t slot = node.slot_start; slot <= node.slot_end; ++slot) {
            m_slot_owner[slot] = idx;
            if (slot == 16383) break;
        }
        m_slot_cache_ready = true;

        return connectToAddress(raw_client, node);
    }

    void RedissClusterClient::setSlotRange(size_t node_index, uint16_t slot_start, uint16_t slot_end)
    {
        if (node_index >= m_nodes.size()) {
            return;
        }
        m_nodes[node_index].address.slot_start = slot_start;
        m_nodes[node_index].address.slot_end = slot_end;

        const int idx = static_cast<int>(node_index);
        for (uint16_t slot = slot_start; slot <= slot_end; ++slot) {
            m_slot_owner[slot] = idx;
            if (slot == 16383) break;
        }
        m_slot_cache_ready = true;
    }

    void RedissClusterClient::setAutoRefreshInterval(std::chrono::milliseconds interval)
    {
        if (interval.count() > 0) {
            m_auto_refresh_interval = interval;
        }
    }

    Task<RedisCommandResult> RedissClusterClient::refreshSlots()
    {
        co_return unwrapRedisCommandTask(co_await refreshSlotsTask());
    }

    Task<RedisCommandResult> RedissClusterClient::execute(
        const std::string& cmd,
        const std::vector<std::string>& args,
        std::string routing_key,
        bool auto_retry)
    {
        if (routing_key.empty() && !args.empty()) {
            routing_key = args.front();
        }
        const bool force_key_routing = !routing_key.empty();
        const size_t max_attempts = auto_retry ? size_t(5) : size_t(1);
        co_return unwrapRedisCommandTask(co_await runAutoTask(routing_key,
                                                                   cmd,
                                                                   args,
                                                                   force_key_routing,
                                                                   auto_retry,
                                                                   max_attempts));
    }

    detail::RedissExchangeOperation RedissClusterClient::batch(
        std::span<const RedisCommandView> commands,
        std::string routing_key)
    {
        if (routing_key.empty() && !commands.empty() && !commands.front().args.empty()) {
            routing_key.assign(commands.front().args.front());
        }

        RedissClient* node = routing_key.empty() ? chooseNodeBySlot(0) : chooseNodeByKey(routing_key);
        if (!node) {
            if (!m_fallback_client) {
                m_fallback_client = std::make_unique<RedissClient>(m_scheduler, m_config, m_tls_config);
            }
            return m_fallback_client->batch(commands);
        }

        return node->batch(commands);
    }

    uint16_t RedissClusterClient::keySlot(const std::string& key) const
    {
        auto hash_key = extractHashTag(key);
        return crc16(reinterpret_cast<const uint8_t*>(hash_key.data()), hash_key.size()) % 16384;
    }

    size_t RedissClusterClient::nodeCount() const noexcept
    {
        return m_nodes.size();
    }

    std::optional<std::reference_wrapper<RedissClient>> RedissClusterClient::node(size_t index)
    {
        if (index >= m_nodes.size() || !m_nodes[index].client) {
            return std::nullopt;
        }
        return *m_nodes[index].client;
    }

    std::optional<RedissClusterClient::RedirectInfo> RedissClusterClient::parseRedirect(const RedisValue& value)
    {
        if (!value.isError()) {
            return std::nullopt;
        }

        const auto msg = value.toError();
        if (msg.empty()) {
            return std::nullopt;
        }

        std::vector<std::string> parts;
        size_t start = 0;
        while (start < msg.size()) {
            while (start < msg.size() && std::isspace(static_cast<unsigned char>(msg[start])) != 0) {
                ++start;
            }
            if (start >= msg.size()) {
                break;
            }
            size_t end = start;
            while (end < msg.size() && std::isspace(static_cast<unsigned char>(msg[end])) == 0) {
                ++end;
            }
            parts.push_back(msg.substr(start, end - start));
            start = end;
        }

        if (parts.size() < 3) {
            return std::nullopt;
        }

        RedirectInfo info;
        if (parts[0] == "MOVED") {
            info.type = RedirectInfo::Type::Moved;
        } else if (parts[0] == "ASK") {
            info.type = RedirectInfo::Type::Ask;
        } else {
            return std::nullopt;
        }

        int32_t slot = 0;
        if (!parseInt32(parts[1], &slot) || slot < 0 || slot > 16383) {
            return std::nullopt;
        }
        info.slot = static_cast<uint16_t>(slot);

        auto endpoint = parseHostPort(parts[2]);
        if (!endpoint.has_value()) {
            return std::nullopt;
        }
        info.host = endpoint->first;
        info.port = endpoint->second;
        return info;
    }

    RedissClusterClient::ClusterNode* RedissClusterClient::chooseNodeHandleBySlot(uint16_t slot) noexcept
    {
        if (m_nodes.empty()) {
            return nullptr;
        }

        const int owner = m_slot_owner[slot];
        if (owner >= 0 && static_cast<size_t>(owner) < m_nodes.size()) {
            return &m_nodes[owner];
        }

        for (auto& node : m_nodes) {
            if (slot >= node.address.slot_start && slot <= node.address.slot_end) {
                return &node;
            }
        }

        return &m_nodes.front();
    }

    RedissClusterClient::ClusterNode* RedissClusterClient::chooseNodeHandleByKey(const std::string& key) noexcept
    {
        return chooseNodeHandleBySlot(keySlot(key));
    }

    RedissClient* RedissClusterClient::chooseNodeBySlot(uint16_t slot) noexcept
    {
        auto* node = chooseNodeHandleBySlot(slot);
        return node ? node->client.get() : nullptr;
    }

    RedissClient* RedissClusterClient::chooseNodeByKey(const std::string& key) noexcept
    {
        auto* node = chooseNodeHandleByKey(key);
        return node ? node->client.get() : nullptr;
    }

    RedissClusterClient::ClusterNode* RedissClusterClient::findOrCreateNode(const std::string& host, int32_t port)
    {
        for (auto& node : m_nodes) {
            if (node.address.host == host && node.address.port == port) {
                return &node;
            }
        }

        ClusterNode node;
        node.address.host = host;
        node.address.port = port;
        node.address.slot_start = 0;
        node.address.slot_end = 16383;
        node.client = std::make_unique<RedissClient>(m_scheduler, m_config, m_tls_config);
        node.connected = false;
        m_nodes.push_back(std::move(node));
        return &m_nodes.back();
    }

    bool RedissClusterClient::applyClusterSlots(const std::vector<RedisValue>& values, std::string* error_message)
    {
        if (values.empty() || !values[0].isArray()) {
            if (error_message) {
                *error_message = "CLUSTER SLOTS response is not array";
            }
            return false;
        }

        auto new_owner = m_slot_owner;
        new_owner.fill(-1);

        const auto slots_rows = values[0].toArray();
        for (const auto& row : slots_rows) {
            if (!row.isArray()) {
                continue;
            }
            const auto row_values = row.toArray();
            if (row_values.size() < 3 || !row_values[0].isInteger() || !row_values[1].isInteger() ||
                !row_values[2].isArray()) {
                continue;
            }

            const int64_t start = row_values[0].toInteger();
            const int64_t end = row_values[1].toInteger();
            if (start < 0 || end < 0 || start > end || end > 16383) {
                continue;
            }

            const auto master_node = row_values[2].toArray();
            if (master_node.size() < 2) {
                continue;
            }

            const auto host = valueToString(master_node[0]);
            const auto port_string = valueToString(master_node[1]);
            int32_t port = 0;
            if (host.empty() || !parseInt32(port_string, &port)) {
                continue;
            }

            auto* node = findOrCreateNode(host, port);
            if (!node) {
                continue;
            }

            node->address.slot_start = static_cast<uint16_t>(std::min<int64_t>(node->address.slot_start, start));
            node->address.slot_end = static_cast<uint16_t>(std::max<int64_t>(node->address.slot_end, end));

            const int owner = static_cast<int>(node - m_nodes.data());
            for (int64_t slot = start; slot <= end; ++slot) {
                new_owner[slot] = owner;
            }
        }

        m_slot_owner = new_owner;
        m_slot_cache_ready = true;
        m_last_refresh_time = std::chrono::steady_clock::now();
        return true;
    }

    bool RedissClusterClient::shouldAutoRefresh() const noexcept
    {
        if (!m_slot_cache_ready) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        return (now - m_last_refresh_time) >= m_auto_refresh_interval;
    }

    Task<RedisCommandResult> RedissClusterClient::refreshSlotsTask()
    {
        if (m_nodes.empty()) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No TLS cluster node configured"));
        }

        ClusterNode* seed = nullptr;
        for (auto& node : m_nodes) {
            if (node.client) {
                seed = &node;
                break;
            }
        }
        if (!seed) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No valid TLS cluster client"));
        }

        if (!seed->connected) {
            auto connect_result = co_await connectToAddress(seed->client.get(), seed->address);
            if (!connect_result) {
                co_return std::unexpected(connect_result.error());
            }
            seed->connected = true;
        }

        RedisCommandBuilder command_builder;
        auto slots_result = co_await seed->client->command(command_builder.clusterSlots());
        if (!slots_result) {
            co_return std::unexpected(slots_result.error());
        }
        if (!slots_result.value().has_value()) {
            co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                 "TLS CLUSTER SLOTS returned empty payload"));
        }

        std::string parse_error;
        auto& slots_values = slots_result.value().value();
        auto values = std::move(slots_values);
        if (!applyClusterSlots(values, &parse_error)) {
            co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR, parse_error));
        }

        co_return std::move(values);
    }

    Task<RedisCommandResult> RedissClusterClient::runAutoTask(
        std::string routing_key,
        std::string cmd,
        std::vector<std::string> args,
        bool force_key_routing,
        bool allow_auto_refresh,
        size_t max_attempts)
    {
        if (m_nodes.empty()) {
            co_return std::unexpected(
                RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No TLS cluster node configured"));
        }

        if (allow_auto_refresh && shouldAutoRefresh()) {
            auto refresh_result = co_await refreshSlots();
            if (!refresh_result) {
                // Continue with best-effort local routing.
            }
        }

        std::vector<std::string> cmd_parts;
        cmd_parts.reserve(1 + args.size());
        cmd_parts.push_back(cmd);
        cmd_parts.insert(cmd_parts.end(), args.begin(), args.end());

        max_attempts = std::max<size_t>(1, max_attempts);
        for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
            ClusterNode* target = nullptr;
            if (force_key_routing && !routing_key.empty()) {
                target = chooseNodeHandleByKey(routing_key);
            } else if (!args.empty()) {
                target = chooseNodeHandleByKey(args.front());
            } else {
                target = chooseNodeHandleBySlot(0);
            }

            if (!target || !target->client) {
                co_return std::unexpected(
                    RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No target TLS cluster node"));
            }

            if (!target->connected) {
                auto connect_result = co_await connectToAddress(target->client.get(), target->address);
                if (!connect_result) {
                    co_return std::unexpected(connect_result.error());
                }
                target->connected = true;
            }

            auto exec_result = co_await target->client->command(encodeCommand(cmd, args));
            if (!exec_result) {
                co_return std::unexpected(exec_result.error());
            }
            if (!exec_result.value().has_value() || exec_result.value()->empty()) {
                co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                     "TLS cluster command returned empty payload"));
            }

            auto& exec_values = exec_result.value().value();
            auto values = std::move(exec_values);
            const auto redirect = parseRedirect(values.front());
            if (!redirect.has_value()) {
                co_return std::move(values);
            }

            auto* redirect_node = findOrCreateNode(redirect->host, redirect->port);
            if (!redirect_node || !redirect_node->client) {
                co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR,
                                                     "TLS redirect target node unavailable"));
            }

            if (!redirect_node->connected) {
                auto connect_result = co_await connectToAddress(redirect_node->client.get(),
                                                                redirect_node->address);
                if (!connect_result) {
                    co_return std::unexpected(connect_result.error());
                }
                redirect_node->connected = true;
            }

            if (redirect->type == RedirectInfo::Type::Moved) {
                m_slot_owner[redirect->slot] = static_cast<int>(redirect_node - m_nodes.data());
                if (allow_auto_refresh) {
                    auto refresh_result = co_await refreshSlots();
                    (void)refresh_result;
                }
                continue;
            }

            if (redirect->type == RedirectInfo::Type::Ask) {
                RedisCommandBuilder asking_builder;
                asking_builder.reserve(2, 1 + cmd_parts.size(), 64);
                asking_builder.append("ASKING");

                std::vector<std::string_view> cmd_args;
                cmd_args.reserve(cmd_parts.size() > 1 ? cmd_parts.size() - 1 : 0);
                for (size_t i = 1; i < cmd_parts.size(); ++i) {
                    cmd_args.emplace_back(cmd_parts[i]);
                }
                asking_builder.append(cmd_parts[0], std::span<const std::string_view>(cmd_args));

                auto asking_result = co_await redirect_node->client->batch(asking_builder.commands());
                if (!asking_result) {
                    co_return std::unexpected(asking_result.error());
                }
                if (!asking_result.value().has_value() || asking_result.value()->size() < 2) {
                    co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "TLS ASK redirect response invalid"));
                }

                auto& ask_values_ref = asking_result.value().value();
                auto ask_values = std::move(ask_values_ref);
                std::vector<RedisValue> final_values;
                final_values.push_back(std::move(ask_values[1]));
                const auto chained_redirect = parseRedirect(final_values.front());
                if (chained_redirect.has_value()) {
                    if (chained_redirect->type == RedirectInfo::Type::Moved) {
                        m_slot_owner[chained_redirect->slot] = static_cast<int>(redirect_node - m_nodes.data());
                    }
                    continue;
                }
                co_return std::move(final_values);
            }
        }

        co_return std::unexpected(RedisError(REDIS_ERROR_TYPE_COMMAND_ERROR,
                                             "Exceeded TLS redirect retry limit"));
    }

    uint16_t RedissClusterClient::crc16(const uint8_t* data, size_t len)
    {
        uint16_t crc = 0;
        for (size_t i = 0; i < len; ++i) {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int j = 0; j < 8; ++j) {
                if ((crc & 0x8000) != 0) {
                    crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
                } else {
                    crc = static_cast<uint16_t>(crc << 1);
                }
            }
        }
        return crc;
    }

    std::string RedissClusterClient::extractHashTag(const std::string& key)
    {
        const auto left = key.find('{');
        if (left == std::string::npos) {
            return key;
        }

        const auto right = key.find('}', left + 1);
        if (right == std::string::npos || right == left + 1) {
            return key;
        }

        return key.substr(left + 1, right - left - 1);
    }
#endif
} // namespace galay::redis
