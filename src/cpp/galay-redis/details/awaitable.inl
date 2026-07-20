#ifndef GALAY_REDIS_DETAILS_AWAITABLE_INL
#define GALAY_REDIS_DETAILS_AWAITABLE_INL

/**
 * @file awaitable.inl
 * @brief Redis 客户端状态机等待体模板实现
 * @details 仅由 async/redis_client.cc 在辅助解析函数定义之后包含。
 */

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

#endif // GALAY_REDIS_DETAILS_AWAITABLE_INL
