#include "redis_client.h"

#include <galay/cpp/galay-redis/base/redis_error.h>
#include <galay/cpp/galay-redis/base/redis_log.h>

#include <galay/cpp/galay-utils/process/system.hpp>

#include <array>
#include <cerrno>
#include <regex>
#include <sys/uio.h>
#include <utility>

namespace galay::redis
{
    using galay::utils::RingBufferBackendStrategy;

    namespace detail
    {
        RedisError mapIoErrorToRedisError(const IOError& io_error, RedisErrorType fallback)
        {
            if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR, io_error.message());
            }
            if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, io_error.message());
            }
            return RedisError(fallback, io_error.message());
        }

        size_t decimalDigits(size_t value)
        {
            size_t digits = 1;
            while (value >= 10) {
                value /= 10;
                ++digits;
            }
            return digits;
        }

        size_t estimateRespCommandBytes(std::string_view cmd, std::span<const std::string_view> args)
        {
            size_t total = 1 + decimalDigits(1 + args.size()) + 2;
            total += 1 + decimalDigits(cmd.size()) + 2 + cmd.size() + 2;
            for (const auto& arg : args) {
                total += 1 + decimalDigits(arg.size()) + 2 + arg.size() + 2;
            }
            return total;
        }

        enum class ParseChunkState : uint8_t
        {
            Done,
            NeedMore,
            ParseError
        };

        struct ParseChunkResult
        {
            size_t consumed = 0;
            ParseChunkState state = ParseChunkState::NeedMore;
        };

        ParseChunkResult parseRepliesFromChunk(protocol::RespParser& parser,
                                               const char* data,
                                               size_t len,
                                               size_t expected_replies,
                                               std::vector<RedisValue>& values)
        {
            ParseChunkResult result;
            while (values.size() < expected_replies && result.consumed < len) {
                protocol::RedisReply reply;
                auto parse_result =
                    parser.parseFast(data + result.consumed, len - result.consumed, &reply);
                if (parse_result) {
                    result.consumed += parse_result.value();
                    values.emplace_back(std::move(reply));
                    continue;
                }

                if (parse_result.error() == protocol::ParseError::Incomplete) {
                    result.state = ParseChunkState::NeedMore;
                } else {
                    result.state = ParseChunkState::ParseError;
                }
                return result;
            }

            result.state = values.size() >= expected_replies
                               ? ParseChunkState::Done
                               : ParseChunkState::NeedMore;
            return result;
        }

        template<RingBufferBackendStrategy Strategy>
        bool parseRepliesFromRingBuffer(galay::utils::RingBuffer<Strategy>& ring_buffer,
                                        protocol::RespParser& parser,
                                        std::string& parse_buffer,
                                        size_t expected_replies,
                                        std::vector<RedisValue>& values,
                                        bool& parse_error)
        {
            struct iovec read_iovecs[2];
            while (values.size() < expected_replies) {
                const size_t read_iovec_count = ring_buffer.getReadIovecs(read_iovecs, 2);
                if (read_iovec_count == 0) {
                    return false;
                }

                const char* first_data = static_cast<const char*>(read_iovecs[0].iov_base);
                const size_t first_len = read_iovecs[0].iov_len;
                if (first_data == nullptr || first_len == 0) {
                    return false;
                }

                const auto first_chunk =
                    parseRepliesFromChunk(parser, first_data, first_len, expected_replies, values);
                if (first_chunk.consumed > 0) {
                    ring_buffer.consume(first_chunk.consumed);
                }

                if (first_chunk.state == ParseChunkState::ParseError) {
                    parse_error = true;
                    return true;
                }
                if (values.size() >= expected_replies) {
                    return true;
                }
                if (first_chunk.consumed == first_len) {
                    if (first_chunk.consumed == 0) {
                        return false;
                    }
                    continue;
                }
                if (read_iovec_count < 2) {
                    return false;
                }

                const char* second_data = static_cast<const char*>(read_iovecs[1].iov_base);
                const size_t second_len = read_iovecs[1].iov_len;
                if (second_data == nullptr || second_len == 0) {
                    return false;
                }

                const size_t first_tail_offset = first_chunk.consumed;
                const size_t first_tail_len = first_len - first_tail_offset;
                parse_buffer.clear();
                parse_buffer.reserve(first_tail_len + second_len);
                parse_buffer.append(first_data + first_tail_offset, first_tail_len);
                parse_buffer.append(second_data, second_len);

                const auto stitched_chunk = parseRepliesFromChunk(parser,
                                                                  parse_buffer.data(),
                                                                  parse_buffer.size(),
                                                                  expected_replies,
                                                                  values);
                if (stitched_chunk.consumed > 0) {
                    ring_buffer.consume(stitched_chunk.consumed);
                }

                if (stitched_chunk.state == ParseChunkState::ParseError) {
                    parse_error = true;
                    return true;
                }
                if (values.size() >= expected_replies) {
                    return true;
                }
                if (stitched_chunk.consumed == 0) {
                    return false;
                }
            }
            return true;
        }

    } // namespace detail

    template<RingBufferBackendStrategy Strategy>
    detail::RedisExchangeSharedState<Strategy>::RedisExchangeSharedState(RedisClient<Strategy>& client,
                                                                         std::string encoded_command_in,
                                                                         size_t expected_replies_in,
                                                                         bool recv_only_in)
        : encoded_cmd(std::move(encoded_command_in))
        , client(&client)
        , expected_replies(expected_replies_in)
        , recv_only(recv_only_in)
    {
        if (recv_only) {
            encoded_cmd.clear();
        } else {
            encoded_view = encoded_cmd;
        }
        values.reserve(expected_replies);
        if (expected_replies == 0) {
            result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
            phase = RedisExchangeSharedState<Strategy>::Phase::Done;
        }
    }

    template<RingBufferBackendStrategy Strategy>
    detail::RedisExchangeSharedState<Strategy>::RedisExchangeSharedState(RedisClient<Strategy>& client,
                                                                         std::string_view encoded_command_in,
                                                                         size_t expected_replies_in,
                                                                         bool recv_only_in)
        : encoded_view(recv_only_in ? std::string_view{} : encoded_command_in)
        , client(&client)
        , expected_replies(expected_replies_in)
        , recv_only(recv_only_in)
    {
        values.reserve(expected_replies);
        if (expected_replies == 0) {
            result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
            phase = RedisExchangeSharedState<Strategy>::Phase::Done;
        }
    }

    template<RingBufferBackendStrategy Strategy>
    detail::RedisExchangeSharedState<Strategy>::RedisExchangeSharedState(
        RedisClient<Strategy>& client,
        std::span<const RedisCommandView> commands)
        : client(&client)
        , expected_replies(commands.size())
    {
        values.reserve(expected_replies);
        if (expected_replies == 0) {
            result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
            phase = RedisExchangeSharedState<Strategy>::Phase::Done;
            return;
        }

        static thread_local protocol::RespEncoder encoder;
        size_t encoded_bytes = 0;
        for (const auto& cmd_view : commands) {
            encoded_bytes += !cmd_view.encoded.empty()
                                 ? cmd_view.encoded.size()
                                 : detail::estimateRespCommandBytes(cmd_view.command, cmd_view.args);
        }

        encoded_cmd.reserve(encoded_bytes);
        for (const auto& cmd_view : commands) {
            if (!cmd_view.encoded.empty()) {
                encoded_cmd.append(cmd_view.encoded.data(), cmd_view.encoded.size());
            } else {
                encoder.appendCommandFast(encoded_cmd, cmd_view.command, cmd_view.args);
            }
        }
        encoded_view = encoded_cmd;
    }

    template<RingBufferBackendStrategy Strategy>
    detail::RedisExchangeMachine<Strategy>::RedisExchangeMachine(
        std::shared_ptr<RedisExchangeSharedState<Strategy>> state)
        : m_state(std::move(state))
    {
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisExchangeMachine<Strategy>::setError(RedisError error) noexcept
    {
        m_state->result = std::unexpected(std::move(error));
        m_state->phase = RedisExchangeSharedState<Strategy>::Phase::Invalid;
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisExchangeMachine<Strategy>::setSendError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR));
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisExchangeMachine<Strategy>::setRecvError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR));
    }

    template<RingBufferBackendStrategy Strategy>
    bool detail::RedisExchangeMachine<Strategy>::prepareReadWindow()
    {
        m_state->read_iov_count = m_state->client->ringBuffer().getWriteIovecs(
            m_state->read_iovecs.data(),
            m_state->read_iovecs.size());
        if (m_state->read_iov_count == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                "Ring buffer exhausted before parsing complete response"));
            return false;
        }
        return true;
    }

    template<RingBufferBackendStrategy Strategy>
    std::expected<bool, RedisError> detail::RedisExchangeMachine<Strategy>::tryParseReplies()
    {
        bool parse_error = false;
        const bool done = detail::parseRepliesFromRingBuffer(
            m_state->client->ringBuffer(),
            m_state->client->parser(),
            m_state->parse_buffer,
            m_state->expected_replies,
            m_state->values,
            parse_error);
        if (parse_error) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                "Parse error"));
        }
        return done;
    }

    template<RingBufferBackendStrategy Strategy>
    galay::kernel::MachineAction<typename detail::RedisExchangeMachine<Strategy>::result_type>
    detail::RedisExchangeMachine<Strategy>::advance()
    {
        if (m_state->result.has_value()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        switch (m_state->phase) {
        case RedisExchangeSharedState<Strategy>::Phase::Invalid:
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Redis exchange machine in invalid state"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        case RedisExchangeSharedState<Strategy>::Phase::Start:
            if (m_state->expected_replies == 0) {
                m_state->result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
                m_state->phase = RedisExchangeSharedState<Strategy>::Phase::Done;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            m_state->phase = (m_state->recv_only || m_state->encoded_view.empty())
                ? RedisExchangeSharedState<Strategy>::Phase::Parse
                : RedisExchangeSharedState<Strategy>::Phase::Send;
            return galay::kernel::MachineAction<result_type>::continue_();
        case RedisExchangeSharedState<Strategy>::Phase::Send:
            if (m_state->sent >= m_state->encoded_view.size()) {
                m_state->phase = RedisExchangeSharedState<Strategy>::Phase::Parse;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            return galay::kernel::MachineAction<result_type>::waitWrite(
                m_state->encoded_view.data() + m_state->sent,
                m_state->encoded_view.size() - m_state->sent);
        case RedisExchangeSharedState<Strategy>::Phase::Parse: {
            auto parsed = tryParseReplies();
            if (!parsed.has_value()) {
                setError(std::move(parsed.error()));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            if (parsed.value()) {
                auto values = std::move(m_state->values);
                m_state->result = std::optional<std::vector<RedisValue>>(std::move(values));
                m_state->phase = RedisExchangeSharedState<Strategy>::Phase::Done;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            if (!prepareReadWindow()) {
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitReadv(
                m_state->read_iovecs.data(),
                m_state->read_iov_count);
        }
        case RedisExchangeSharedState<Strategy>::Phase::Done:
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        setError(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "Unknown redis exchange machine state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisExchangeMachine<Strategy>::onRead(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setRecvError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                "Connection closed"));
            return;
        }

        m_state->client->ringBuffer().produce(result.value());
        m_state->phase = RedisExchangeSharedState<Strategy>::Phase::Parse;
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisExchangeMachine<Strategy>::onWrite(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setSendError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                "Send returned 0"));
            return;
        }

        m_state->sent += result.value();
        if (m_state->sent >= m_state->encoded_view.size()) {
            m_state->phase = RedisExchangeSharedState<Strategy>::Phase::Parse;
    }
    }

    template<RingBufferBackendStrategy Strategy>
    detail::RedisConnectSharedState<Strategy>::RedisConnectSharedState(RedisClient<Strategy>& client,
                                                                       std::string ip_in,
                                                                       int32_t port_in,
                                                                       std::string username_in,
                                                                       std::string password_in,
                                                                       int32_t db_index_in,
                                                                       int version_in)
        : host(version_in == 6 ? IPType::IPV6 : IPType::IPV4, ip_in, port_in)
        , ip(std::move(ip_in))
        , username(std::move(username_in))
        , password(std::move(password_in))
        , client(&client)
        , port(port_in)
        , db_index(db_index_in)
        , version(version_in)
    {
        client.ringBuffer().clear();
        client.parser() = protocol::RespParser();
    }

    template<RingBufferBackendStrategy Strategy>
    detail::RedisConnectMachine<Strategy>::RedisConnectMachine(
        std::shared_ptr<RedisConnectSharedState<Strategy>> state)
        : m_state(std::move(state))
    {
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisConnectMachine<Strategy>::setError(RedisError error) noexcept
    {
        m_state->result = std::unexpected(std::move(error));
        m_state->phase = RedisConnectSharedState<Strategy>::Phase::Invalid;
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisConnectMachine<Strategy>::setConnectError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR));
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisConnectMachine<Strategy>::setSendError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR));
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisConnectMachine<Strategy>::setRecvError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR));
    }

    template<RingBufferBackendStrategy Strategy>
    bool detail::RedisConnectMachine<Strategy>::prepareReadWindow()
    {
        m_state->read_iov_count = m_state->client->ringBuffer().getWriteIovecs(
            m_state->read_iovecs.data(),
            m_state->read_iovecs.size());
        if (m_state->read_iov_count == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                "No writable iovec for response"));
            return false;
        }
        return true;
    }

    template<RingBufferBackendStrategy Strategy>
    bool detail::RedisConnectMachine<Strategy>::prepareNextCommand()
    {
        RedisCommandBuilder builder;
        if (!m_state->auth_sent && (!m_state->username.empty() || !m_state->password.empty())) {
            m_state->pending_command = RedisConnectSharedState<Strategy>::PendingCommand::Auth;
            m_state->auth_sent = true;
            m_state->encoded_cmd = m_state->username.empty()
                ? builder.auth(m_state->password).encoded
                : builder.auth(m_state->username, m_state->password).encoded;
            m_state->sent = 0;
            return true;
        }

        if (!m_state->select_sent && m_state->db_index != 0) {
            m_state->pending_command = RedisConnectSharedState<Strategy>::PendingCommand::Select;
            m_state->select_sent = true;
            m_state->encoded_cmd = builder.select(m_state->db_index).encoded;
            m_state->sent = 0;
            return true;
        }

        m_state->pending_command = RedisConnectSharedState<Strategy>::PendingCommand::None;
        m_state->encoded_cmd.clear();
        return false;
    }

    template<RingBufferBackendStrategy Strategy>
    std::expected<bool, RedisError> detail::RedisConnectMachine<Strategy>::tryParseReply()
    {
        bool parse_error = false;
        const bool done = detail::parseRepliesFromRingBuffer(
            m_state->client->ringBuffer(),
            m_state->client->parser(),
            m_state->parse_buffer,
            1,
            m_state->values,
            parse_error);

        if (parse_error) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                "Parse response error"));
        }
        if (!done) {
            return false;
        }
        if (m_state->values.empty()) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                "Empty response"));
        }

        RedisValue reply = std::move(m_state->values.front());
        m_state->values.clear();
        if (reply.isError()) {
            const auto error_type = m_state->pending_command == RedisConnectSharedState<Strategy>::PendingCommand::Auth
                ? RedisErrorType::REDIS_ERROR_TYPE_AUTH_ERROR
                : RedisErrorType::REDIS_ERROR_TYPE_INVALID_ERROR;
            return std::unexpected(RedisError(error_type, reply.toError()));
        }

        if (prepareNextCommand()) {
            m_state->phase = RedisConnectSharedState<Strategy>::Phase::Send;
        } else {
            m_state->phase = RedisConnectSharedState<Strategy>::Phase::Done;
            m_state->result = RedisVoidResult{};
        }
        return true;
    }

    template<RingBufferBackendStrategy Strategy>
    galay::kernel::MachineAction<typename detail::RedisConnectMachine<Strategy>::result_type>
    detail::RedisConnectMachine<Strategy>::advance()
    {
        if (m_state->result.has_value()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        switch (m_state->phase) {
        case RedisConnectSharedState<Strategy>::Phase::Invalid:
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Redis connect machine in invalid state"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        case RedisConnectSharedState<Strategy>::Phase::Connect:
            return galay::kernel::MachineAction<result_type>::waitConnect(m_state->host);
        case RedisConnectSharedState<Strategy>::Phase::Send:
            if (m_state->sent >= m_state->encoded_cmd.size()) {
                m_state->phase = RedisConnectSharedState<Strategy>::Phase::Parse;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            return galay::kernel::MachineAction<result_type>::waitWrite(
                m_state->encoded_cmd.data() + m_state->sent,
                m_state->encoded_cmd.size() - m_state->sent);
        case RedisConnectSharedState<Strategy>::Phase::Parse: {
            auto parsed = tryParseReply();
            if (!parsed.has_value()) {
                setError(std::move(parsed.error()));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            if (parsed.value()) {
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            if (!prepareReadWindow()) {
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitReadv(
                m_state->read_iovecs.data(),
                m_state->read_iov_count);
        }
        case RedisConnectSharedState<Strategy>::Phase::Done:
            if (!m_state->result.has_value()) {
                m_state->result = RedisVoidResult{};
            }
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        setError(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "Unknown redis connect machine state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisConnectMachine<Strategy>::onConnect(std::expected<void, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setConnectError(result.error());
            return;
        }

        m_state->client->setClosed(false);
        if (prepareNextCommand()) {
            m_state->phase = RedisConnectSharedState<Strategy>::Phase::Send;
        } else {
            m_state->phase = RedisConnectSharedState<Strategy>::Phase::Done;
            m_state->result = RedisVoidResult{};
        }
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisConnectMachine<Strategy>::onRead(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setRecvError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                "Connection closed"));
            return;
        }

        m_state->client->ringBuffer().produce(result.value());
        m_state->phase = RedisConnectSharedState<Strategy>::Phase::Parse;
    }

    template<RingBufferBackendStrategy Strategy>
    void detail::RedisConnectMachine<Strategy>::onWrite(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setSendError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                "Send returned 0"));
            return;
        }

        m_state->sent += result.value();
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->phase = RedisConnectSharedState<Strategy>::Phase::Parse;
        }
    }

    template<RingBufferBackendStrategy Strategy>
    RedisClient<Strategy>::RedisClient(IOScheduler* scheduler,
                                       AsyncRedisConfig config)
        : m_config(config)
        , m_ring_buffer(std::make_shared<galay::utils::RingBuffer<Strategy>>(config.buffer_size))
        , m_scheduler(scheduler)
    {
    }

    template<RingBufferBackendStrategy Strategy>
    RedisClient<Strategy>::RedisClient(RedisClient&& other) noexcept
        : m_socket(std::move(other.m_socket))
        , m_config(other.m_config)
        , m_ring_buffer(std::move(other.m_ring_buffer))
        , m_scheduler(other.m_scheduler)
        , m_is_closed(other.m_is_closed)
        , m_parser(std::move(other.m_parser))
    {
        other.m_is_closed = true;
    }

    template<RingBufferBackendStrategy Strategy>
    RedisClient<Strategy>& RedisClient<Strategy>::operator=(RedisClient&& other) noexcept
    {
        if (this != &other) {
            m_is_closed = other.m_is_closed;
            m_socket = std::move(other.m_socket);
            m_scheduler = other.m_scheduler;
            m_parser = std::move(other.m_parser);
            m_config = other.m_config;
            m_ring_buffer = std::move(other.m_ring_buffer);
            other.m_is_closed = true;
        }
        return *this;
    }

    template<RingBufferBackendStrategy Strategy>
    RedisExchangeOperationFor<Strategy> RedisClient<Strategy>::command(RedisEncodedCommand command_packet)
    {
        auto state = std::make_shared<detail::RedisExchangeSharedState<Strategy>>(
            *this,
            std::move(command_packet.encoded),
            command_packet.expected_replies,
            false);
        return galay::kernel::AwaitableBuilder<detail::RedisExchangeResult>::fromStateMachine(
                   m_socket.controller(),
                   detail::RedisExchangeMachine<Strategy>(std::move(state)))
            .build();
    }

    template<RingBufferBackendStrategy Strategy>
    RedisExchangeOperationFor<Strategy> RedisClient<Strategy>::commandBorrowed(const RedisBorrowedCommand& packet)
    {
        auto state = std::make_shared<detail::RedisExchangeSharedState<Strategy>>(
            *this,
            packet.encoded(),
            packet.expectedReplies(),
            false);
        return galay::kernel::AwaitableBuilder<detail::RedisExchangeResult>::fromStateMachine(
                   m_socket.controller(),
                   detail::RedisExchangeMachine<Strategy>(std::move(state)))
            .build();
    }

    template<RingBufferBackendStrategy Strategy>
    RedisExchangeOperationFor<Strategy> RedisClient<Strategy>::receive(size_t expected_replies)
    {
        auto state = std::make_shared<detail::RedisExchangeSharedState<Strategy>>(
            *this,
            std::string(),
            expected_replies,
            true);
        return galay::kernel::AwaitableBuilder<detail::RedisExchangeResult>::fromStateMachine(
                   m_socket.controller(),
                   detail::RedisExchangeMachine<Strategy>(std::move(state)))
            .build();
    }

    template<RingBufferBackendStrategy Strategy>
    RedisExchangeOperationFor<Strategy> RedisClient<Strategy>::batch(std::span<const RedisCommandView> commands)
    {
        auto state = std::make_shared<detail::RedisExchangeSharedState<Strategy>>(*this, commands);
        return galay::kernel::AwaitableBuilder<detail::RedisExchangeResult>::fromStateMachine(
                   m_socket.controller(),
                   detail::RedisExchangeMachine<Strategy>(std::move(state)))
            .build();
    }

    template<RingBufferBackendStrategy Strategy>
    RedisExchangeOperationFor<Strategy> RedisClient<Strategy>::batchBorrowed(const std::string& encoded,
                                                                            size_t expected_replies)
    {
        auto state = std::make_shared<detail::RedisExchangeSharedState<Strategy>>(
            *this,
            std::string_view(encoded),
            expected_replies,
            false);
        return galay::kernel::AwaitableBuilder<detail::RedisExchangeResult>::fromStateMachine(
                   m_socket.controller(),
                   detail::RedisExchangeMachine<Strategy>(std::move(state)))
            .build();
    }

    template<RingBufferBackendStrategy Strategy>
    RedisConnectOperationFor<Strategy> RedisClient<Strategy>::connect(const std::string& url)
    {
        std::regex pattern(R"(^redis://(?:([^:@]*)(?::([^@]*))?@)?([a-zA-Z0-9\-\.]+)(?::(\d+))?(?:/(\d+))?$)");
        std::smatch matches;
        std::string username;
        std::string password;
        std::string host;
        int32_t port = 6379;
        int32_t db_index = 0;

        if (std::regex_match(url, matches, pattern)) {
            if (matches.size() > 1 && !matches[1].str().empty()) {
                username = matches[1];
            }
            if (matches.size() > 2 && !matches[2].str().empty()) {
                password = matches[2];
            }
            if (matches.size() > 3 && !matches[3].str().empty()) {
                host = matches[3];
            }
            if (matches.size() > 4 && !matches[4].str().empty()) {
                try {
                    port = std::stoi(matches[4]);
                } catch (const std::exception& e) {
                    REDIS_LOG_WARN("[client]", "Failed to parse port from URL, using default 6379: {}", e.what());
                    port = 6379;
                }
            }
            if (matches.size() > 5 && !matches[5].str().empty()) {
                try {
                    db_index = std::stoi(matches[5]);
                } catch (const std::exception& e) {
                    REDIS_LOG_WARN("[client]", "Failed to parse db_index from URL, using default 0: {}", e.what());
                    db_index = 0;
                }
            }
        }

        using namespace galay::utils;
        std::string ip;
        int version = 2;
        switch (System::checkAddressType(host)) {
        case System::AddressType::IPv4:
            ip = host;
            version = 2;
            break;
        case System::AddressType::IPv6:
            ip = host;
            version = 6;
            break;
        case System::AddressType::Domain:
        case System::AddressType::Invalid:
            ip = System::resolveHostIPv4(host);
            version = 2;
            if (ip.empty()) {
                ip = System::resolveHostIPv6(host);
                version = ip.empty() ? 2 : 6;
            }
            break;
        default:
            ip = host;
            break;
        }

        RedisConnectOptions options;
        options.username = std::move(username);
        options.password = std::move(password);
        options.db_index = db_index;
        options.version = version;
        return connect(ip, port, std::move(options));
    }

    template<RingBufferBackendStrategy Strategy>
    RedisConnectOperationFor<Strategy> RedisClient<Strategy>::connect(const std::string& ip,
                                                                     int32_t port,
                                                                     RedisConnectOptions options)
    {
        auto state = std::make_shared<detail::RedisConnectSharedState<Strategy>>(
            *this,
            ip,
            port,
            std::move(options.username),
            std::move(options.password),
            options.db_index,
            options.version);
        if (m_config.tcp_no_delay) {
            auto nodelay_result = m_socket.option().handleTcpNoDelay();
            if (!nodelay_result) {
                state->result = std::unexpected(detail::mapIoErrorToRedisError(
                    nodelay_result.error(),
                    RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR));
                state->phase = detail::RedisConnectSharedState<Strategy>::Phase::Invalid;
            }
        }
        return galay::kernel::AwaitableBuilder<RedisVoidResult>::fromStateMachine(
                   m_socket.controller(),
                   detail::RedisConnectMachine<Strategy>(std::move(state)))
            .build();
    }

    template class RedisClient<RingBufferBackendStrategy::Mmap>;
    template class RedisClient<RingBufferBackendStrategy::Vector>;
    template class RedisClient<RingBufferBackendStrategy::Auto>;
} // namespace galay::redis
