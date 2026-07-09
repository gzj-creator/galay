#include "builder.h"

namespace galay::redis
{
    RedisCommandBuilder::RedisCommandBuilder(RedisCommandBuilder&& other) noexcept
        : m_encoded(std::move(other.m_encoded))
        , m_storage(std::move(other.m_storage))
        , m_arg_slices(std::move(other.m_arg_slices))
        , m_commands(std::move(other.m_commands))
        , m_views_dirty(true)
    {
        other.m_arg_views.clear();
        other.m_command_views.clear();
        other.m_views_dirty = true;
    }

    RedisCommandBuilder& RedisCommandBuilder::operator=(RedisCommandBuilder&& other) noexcept
    {
        if (this != &other) {
            m_encoded = std::move(other.m_encoded);
            m_storage = std::move(other.m_storage);
            m_arg_slices = std::move(other.m_arg_slices);
            m_commands = std::move(other.m_commands);
            m_arg_views.clear();
            m_command_views.clear();
            m_views_dirty = true;
            other.m_arg_views.clear();
            other.m_command_views.clear();
            other.m_views_dirty = true;
        }
        return *this;
    }

    RedisCommandBuilder RedisCommandBuilder::clone() const
    {
        RedisCommandBuilder cloned;
        cloned.m_encoded = m_encoded;
        cloned.m_storage = m_storage;
        cloned.m_arg_slices = m_arg_slices;
        cloned.m_commands = m_commands;
        cloned.m_views_dirty = true;
        return cloned;
    }

    void RedisCommandBuilder::clear() noexcept
    {
        m_encoded.clear();

        m_storage.clear();
        m_arg_slices.clear();
        m_commands.clear();
        m_arg_views.clear();
        m_command_views.clear();
        m_views_dirty = true;
    }

    void RedisCommandBuilder::reserve(size_t command_count, size_t arg_count, size_t storage_bytes)
    {
        m_encoded.reserve(storage_bytes);
        m_commands.reserve(command_count);
        m_command_views.reserve(command_count);
        m_arg_slices.reserve(arg_count);
        m_arg_views.reserve(arg_count);
        m_storage.reserve(storage_bytes);
    }

    RedisCommandBuilder& RedisCommandBuilder::append(std::string_view cmd)
    {
        return append(cmd, std::span<const std::string_view>());
    }

    RedisCommandBuilder& RedisCommandBuilder::append(
        std::string_view cmd,
        std::span<const std::string_view> args)
    {
        const auto command_slice = appendToStorage(cmd);
        const size_t arg_offset = m_arg_slices.size();
        for (const auto arg : args) {
            m_arg_slices.push_back(appendToStorage(arg));
        }
        const size_t encoded_offset = m_encoded.size();
        m_encoder.appendCommandFast(m_encoded, cmd, args);
        const Slice encoded_slice{encoded_offset, m_encoded.size() - encoded_offset};
        m_commands.push_back(CommandMeta{command_slice, arg_offset, args.size(), encoded_slice});
        m_views_dirty = true;
        return *this;
    }

    RedisCommandBuilder& RedisCommandBuilder::append(
        std::string_view cmd,
        std::initializer_list<std::string_view> args)
    {
        return append(cmd, std::span<const std::string_view>(args.begin(), args.size()));
    }

    std::span<const RedisCommandView> RedisCommandBuilder::commands() const
    {
        rebuildViewsIfNeeded();
        return std::span<const RedisCommandView>(m_command_views);
    }

    size_t RedisCommandBuilder::size() const noexcept
    {
        return m_commands.size();
    }

    const std::string& RedisCommandBuilder::encoded() const noexcept
    {
        return m_encoded;
    }

    bool RedisCommandBuilder::empty() const noexcept
    {
        return m_commands.empty();
    }

    RedisEncodedCommand RedisCommandBuilder::build() const
    {
        const size_t replies = m_commands.empty() ? 1 : m_commands.size();
        RedisEncodedCommand out;
        out.encoded = m_encoded;
        out.expected_replies = replies;
        return out;
    }

    RedisEncodedCommand RedisCommandBuilder::release()
    {
        const size_t replies = m_commands.empty() ? 1 : m_commands.size();
        RedisEncodedCommand out;
        out.encoded = std::move(m_encoded);
        out.expected_replies = replies;
        m_encoded.clear();
        return out;
    }

    size_t RedisCommandBuilder::normalizeExpectedReplies(size_t expected_replies) noexcept
    {
        return expected_replies == 0 ? 1 : expected_replies;
    }

    RedisCommandBuilder::Slice RedisCommandBuilder::appendToStorage(std::string_view value)
    {
        const Slice slice{m_storage.size(), value.size()};
        if (!value.empty()) {
            m_storage.append(value.data(), value.size());
        }
        return slice;
    }

    std::string_view RedisCommandBuilder::toView(Slice slice) const
    {
        if (slice.length == 0) {
            return {};
        }
        return std::string_view(m_storage.data() + slice.offset, slice.length);
    }

    std::string_view RedisCommandBuilder::toEncodedView(Slice slice) const
    {
        if (slice.length == 0) {
            return {};
        }
        return std::string_view(m_encoded.data() + slice.offset, slice.length);
    }

    void RedisCommandBuilder::rebuildViewsIfNeeded() const
    {
        if (!m_views_dirty) {
            return;
        }

        m_arg_views.resize(m_arg_slices.size());
        for (size_t i = 0; i < m_arg_slices.size(); ++i) {
            m_arg_views[i] = toView(m_arg_slices[i]);
        }

        m_command_views.resize(m_commands.size());
        for (size_t i = 0; i < m_commands.size(); ++i) {
            const auto& meta = m_commands[i];
            const std::string_view* arg_ptr = nullptr;
            if (meta.arg_count > 0) {
                arg_ptr = m_arg_views.data() + meta.arg_offset;
            }
            m_command_views[i] = RedisCommandView{
                .command = toView(meta.command),
                .args = std::span<const std::string_view>(arg_ptr, meta.arg_count),
                .encoded = toEncodedView(meta.encoded)};
        }

        m_views_dirty = false;
    }

    RedisEncodedCommand RedisCommandBuilder::command(std::string_view cmd, size_t expected_replies) const
    {
        return command(cmd, std::span<const std::string_view>(), expected_replies);
    }

    RedisEncodedCommand RedisCommandBuilder::command(
        std::string_view cmd,
        std::span<const std::string_view> args,
        size_t expected_replies) const
    {
        RedisEncodedCommand result;
        result.expected_replies = normalizeExpectedReplies(expected_replies);
        result.encoded.reserve(m_encoder.estimateCommandBytes(cmd, args));
        m_encoder.appendCommandFast(result.encoded, cmd, args);
        return result;
    }

    RedisEncodedCommand RedisCommandBuilder::command(
        std::string_view cmd,
        std::initializer_list<std::string_view> args,
        size_t expected_replies) const
    {
        RedisEncodedCommand result;
        result.expected_replies = normalizeExpectedReplies(expected_replies);
        result.encoded.reserve(m_encoder.estimateCommandBytes(cmd, args));
        m_encoder.appendCommandFast(result.encoded, cmd, args);
        return result;
    }

    RedisEncodedCommand RedisCommandBuilder::command(
        std::string_view cmd,
        const std::vector<std::string>& args,
        size_t expected_replies) const
    {
        RedisEncodedCommand result;
        result.expected_replies = normalizeExpectedReplies(expected_replies);
        result.encoded.reserve(m_encoder.estimateCommandBytes(cmd, args));
        m_encoder.appendCommandFast(result.encoded, cmd, args);
        return result;
    }

    RedisEncodedCommand RedisCommandBuilder::auth(const std::string& password) const
    {
        return command("AUTH", std::array<std::string_view, 1>{password});
    }

    RedisEncodedCommand RedisCommandBuilder::auth(const std::string& username,
                                                   const std::string& password) const
    {
        return command("AUTH", std::array<std::string_view, 2>{username, password});
    }

    RedisEncodedCommand RedisCommandBuilder::select(int32_t db_index) const
    {
        return command("SELECT", {std::to_string(db_index)});
    }

    RedisEncodedCommand RedisCommandBuilder::ping() const
    {
        return command("PING");
    }

    RedisEncodedCommand RedisCommandBuilder::echo(const std::string& message) const
    {
        return command("ECHO", std::array<std::string_view, 1>{message});
    }

    RedisEncodedCommand RedisCommandBuilder::publish(const std::string& channel,
                                                      const std::string& message) const
    {
        return command("PUBLISH", std::array<std::string_view, 2>{channel, message});
    }

    RedisEncodedCommand RedisCommandBuilder::subscribe(const std::string& channel) const
    {
        return command("SUBSCRIBE", std::array<std::string_view, 1>{channel});
    }

    RedisEncodedCommand RedisCommandBuilder::subscribe(const std::vector<std::string>& channels) const
    {
        return command("SUBSCRIBE", channels, channels.empty() ? 1 : channels.size());
    }

    RedisEncodedCommand RedisCommandBuilder::unsubscribe(const std::string& channel) const
    {
        return command("UNSUBSCRIBE", std::array<std::string_view, 1>{channel});
    }

    RedisEncodedCommand RedisCommandBuilder::unsubscribe(const std::vector<std::string>& channels) const
    {
        return command("UNSUBSCRIBE", channels, channels.empty() ? 1 : channels.size());
    }

    RedisEncodedCommand RedisCommandBuilder::psubscribe(const std::string& pattern) const
    {
        return command("PSUBSCRIBE", std::array<std::string_view, 1>{pattern});
    }

    RedisEncodedCommand RedisCommandBuilder::psubscribe(const std::vector<std::string>& patterns) const
    {
        return command("PSUBSCRIBE", patterns, patterns.empty() ? 1 : patterns.size());
    }

    RedisEncodedCommand RedisCommandBuilder::punsubscribe(const std::string& pattern) const
    {
        return command("PUNSUBSCRIBE", std::array<std::string_view, 1>{pattern});
    }

    RedisEncodedCommand RedisCommandBuilder::punsubscribe(const std::vector<std::string>& patterns) const
    {
        return command("PUNSUBSCRIBE", patterns, patterns.empty() ? 1 : patterns.size());
    }

    RedisEncodedCommand RedisCommandBuilder::role() const
    {
        return command("ROLE");
    }

    RedisEncodedCommand RedisCommandBuilder::replicaof(const std::string& host, int32_t port) const
    {
        return command("REPLICAOF", {host, std::to_string(port)});
    }

    RedisEncodedCommand RedisCommandBuilder::readonly() const
    {
        return command("READONLY");
    }

    RedisEncodedCommand RedisCommandBuilder::readwrite() const
    {
        return command("READWRITE");
    }

    RedisEncodedCommand RedisCommandBuilder::clusterInfo() const
    {
        return command("CLUSTER", {"INFO"});
    }

    RedisEncodedCommand RedisCommandBuilder::clusterNodes() const
    {
        return command("CLUSTER", {"NODES"});
    }

    RedisEncodedCommand RedisCommandBuilder::clusterSlots() const
    {
        return command("CLUSTER", {"SLOTS"});
    }

    RedisEncodedCommand RedisCommandBuilder::get(const std::string& key) const
    {
        return command("GET", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::set(const std::string& key, const std::string& value) const
    {
        return command("SET", std::array<std::string_view, 2>{key, value});
    }

    RedisEncodedCommand RedisCommandBuilder::setex(const std::string& key,
                                                    int64_t seconds,
                                                    const std::string& value) const
    {
        return command("SETEX", {key, std::to_string(seconds), value});
    }

    RedisEncodedCommand RedisCommandBuilder::del(const std::string& key) const
    {
        return command("DEL", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::exists(const std::string& key) const
    {
        return command("EXISTS", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::incr(const std::string& key) const
    {
        return command("INCR", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::decr(const std::string& key) const
    {
        return command("DECR", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::hget(const std::string& key, const std::string& field) const
    {
        return command("HGET", std::array<std::string_view, 2>{key, field});
    }

    RedisEncodedCommand RedisCommandBuilder::hset(const std::string& key,
                                                   const std::string& field,
                                                   const std::string& value) const
    {
        return command("HSET", std::array<std::string_view, 3>{key, field, value});
    }

    RedisEncodedCommand RedisCommandBuilder::hdel(const std::string& key, const std::string& field) const
    {
        return command("HDEL", std::array<std::string_view, 2>{key, field});
    }

    RedisEncodedCommand RedisCommandBuilder::hgetAll(const std::string& key) const
    {
        return command("HGETALL", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::lpush(const std::string& key, const std::string& value) const
    {
        return command("LPUSH", std::array<std::string_view, 2>{key, value});
    }

    RedisEncodedCommand RedisCommandBuilder::rpush(const std::string& key, const std::string& value) const
    {
        return command("RPUSH", std::array<std::string_view, 2>{key, value});
    }

    RedisEncodedCommand RedisCommandBuilder::lpop(const std::string& key) const
    {
        return command("LPOP", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::rpop(const std::string& key) const
    {
        return command("RPOP", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::llen(const std::string& key) const
    {
        return command("LLEN", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::lrange(const std::string& key,
                                                     int64_t start,
                                                     int64_t stop) const
    {
        return command("LRANGE", {key, std::to_string(start), std::to_string(stop)});
    }

    RedisEncodedCommand RedisCommandBuilder::sadd(const std::string& key, const std::string& member) const
    {
        return command("SADD", std::array<std::string_view, 2>{key, member});
    }

    RedisEncodedCommand RedisCommandBuilder::srem(const std::string& key, const std::string& member) const
    {
        return command("SREM", std::array<std::string_view, 2>{key, member});
    }

    RedisEncodedCommand RedisCommandBuilder::smembers(const std::string& key) const
    {
        return command("SMEMBERS", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::scard(const std::string& key) const
    {
        return command("SCARD", std::array<std::string_view, 1>{key});
    }

    RedisEncodedCommand RedisCommandBuilder::zadd(const std::string& key,
                                                   double score,
                                                   const std::string& member) const
    {
        return command("ZADD", {key, std::to_string(score), member});
    }

    RedisEncodedCommand RedisCommandBuilder::zrem(const std::string& key, const std::string& member) const
    {
        return command("ZREM", std::array<std::string_view, 2>{key, member});
    }

    RedisEncodedCommand RedisCommandBuilder::zrange(const std::string& key,
                                                     int64_t start,
                                                     int64_t stop) const
    {
        return command("ZRANGE", {key, std::to_string(start), std::to_string(stop)});
    }

    RedisEncodedCommand RedisCommandBuilder::zscore(const std::string& key,
                                                     const std::string& member) const
    {
        return command("ZSCORE", std::array<std::string_view, 2>{key, member});
    }
} // namespace galay::redis
