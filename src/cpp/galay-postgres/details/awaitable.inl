#ifndef GALAY_POSTGRES_DETAILS_AWAITABLE_INL
#define GALAY_POSTGRES_DETAILS_AWAITABLE_INL

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace galay::postgres::details
{

namespace detail
{

template<RingBufferBackendStrategy Strategy>
std::string_view linearizeRingBuffer(const RingBuffer<Strategy, std::dynamic_extent>& ring,
                                     std::string& scratch)
{
    std::array<struct iovec, 2> iovecs{};
    const size_t count = ring.getReadIovecs(iovecs);
    if (count == 0) {
        return {};
    }
    if (count == 1) {
        return {static_cast<const char*>(iovecs[0].iov_base), iovecs[0].iov_len};
    }

    scratch.clear();
    scratch.reserve(iovecs[0].iov_len + iovecs[1].iov_len);
    for (size_t index = 0; index < count; ++index) {
        scratch.append(static_cast<const char*>(iovecs[index].iov_base),
                       iovecs[index].iov_len);
    }
    return scratch;
}

inline PostgresError protocolError(std::string_view message)
{
    return PostgresError(POSTGRES_ERROR_PROTOCOL, std::string(message));
}

inline PostgresError serverError(protocol::ErrorFields fields,
                                 PostgresErrorType type = POSTGRES_ERROR_SERVER)
{
    std::string message = std::move(fields.message);
    if (!fields.detail.empty()) {
        message += "; detail: " + fields.detail;
    }
    if (!fields.hint.empty()) {
        message += "; hint: " + fields.hint;
    }
    return PostgresError(type,
                         std::move(fields.sql_state),
                         std::move(fields.severity),
                         std::move(message));
}

inline PostgresError mapIoError(const galay::kernel::IOError& error,
                                PostgresErrorType fallback)
{
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kTimeout)) {
        return PostgresError(POSTGRES_ERROR_TIMEOUT, error.message());
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kDisconnectError) ||
        galay::kernel::IOError::contains(error.code(), galay::kernel::kClosed)) {
        return PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED, error.message());
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kNotReady) ||
        galay::kernel::IOError::contains(error.code(),
                                         galay::kernel::kNotRunningOnIOScheduler)) {
        return PostgresError(POSTGRES_ERROR_INTERNAL, error.message());
    }
    return PostgresError(fallback, error.message());
}

inline bool invalidatesConnection(const galay::kernel::IOError& error)
{
    return !galay::kernel::IOError::contains(error.code(), galay::kernel::kNotReady) &&
           !galay::kernel::IOError::contains(error.code(),
                                             galay::kernel::kNotRunningOnIOScheduler);
}

inline bool invalidatesConnection(const PostgresError& error) noexcept
{
    switch (error.type()) {
    case POSTGRES_ERROR_CONNECTION:
    case POSTGRES_ERROR_PROTOCOL:
    case POSTGRES_ERROR_TIMEOUT:
    case POSTGRES_ERROR_SEND:
    case POSTGRES_ERROR_RECV:
    case POSTGRES_ERROR_CONNECTION_CLOSED:
    case POSTGRES_ERROR_BUFFER_OVERFLOW:
        return true;
    default:
        return false;
    }
}

template<RingBufferBackendStrategy Strategy>
bool prepareReadWindow(AsyncPostgresClient<Strategy>& client,
                       std::array<struct iovec, 2>& iovecs,
                       size_t& count,
                       PostgresError& error)
{
    count = client.ringBuffer().getWriteIovecs(iovecs);
    if (count != 0) {
        return true;
    }
    error = PostgresError(POSTGRES_ERROR_BUFFER_OVERFLOW,
                          "PostgreSQL response exceeds receive buffer capacity");
    return false;
}

inline void appendFields(PostgresResultSet& result,
                         std::vector<protocol::RowDescriptionField> fields)
{
    result.reserveFields(fields.size());
    for (auto& field : fields) {
        result.addField(PostgresField(std::move(field.name),
                                      field.table_oid,
                                      field.column_index,
                                      field.type_oid,
                                      field.type_size,
                                      field.type_modifier,
                                      field.format));
    }
}

inline void appendFields(std::vector<PostgresField>& output,
                         std::vector<protocol::RowDescriptionField> fields)
{
    output.reserve(fields.size());
    for (auto& field : fields) {
        output.emplace_back(std::move(field.name),
                            field.table_oid,
                            field.column_index,
                            field.type_oid,
                            field.type_size,
                            field.type_modifier,
                            field.format);
    }
}

template<RingBufferBackendStrategy Strategy>
std::expected<std::optional<protocol::MessageView>, PostgresError>
peekMessage(AsyncPostgresClient<Strategy>& client, std::string& scratch)
{
    const std::string_view bytes = linearizeRingBuffer(client.ringBuffer(), scratch);
    if (bytes.empty()) {
        return std::optional<protocol::MessageView>{};
    }
    auto message = client.parser().extractMessage(bytes.data(), bytes.size());
    if (!message) {
        if (message.error() == protocol::ParseError::Incomplete) {
            return std::optional<protocol::MessageView>{};
        }
        return std::unexpected(protocolError("Malformed PostgreSQL message frame"));
    }
    return std::optional<protocol::MessageView>(*message);
}

template<RingBufferBackendStrategy Strategy>
std::expected<void, PostgresError>
consumeCommonMessage(AsyncPostgresClient<Strategy>& client,
                     const protocol::MessageView& message)
{
    if (message.type == protocol::kMsgParameterStatus) {
        auto status = client.parser().parseParameterStatus(message.payload, message.payload_len);
        if (!status) {
            return std::unexpected(protocolError("Malformed ParameterStatus"));
        }
        client.setServerParameter(std::move(status->name), std::move(status->value));
        return {};
    }
    if (message.type == protocol::kMsgBackendKeyData) {
        auto key = client.parser().parseBackendKeyData(message.payload, message.payload_len);
        if (!key) {
            return std::unexpected(protocolError("Malformed BackendKeyData"));
        }
        client.setBackendKeyData(*key);
        return {};
    }
    return std::unexpected(protocolError("Unexpected PostgreSQL asynchronous message"));
}

} // namespace detail

template<RingBufferBackendStrategy Strategy>
PostgresConnectAwaitable<Strategy>::PostgresConnectAwaitable(
    AsyncPostgresClient<Strategy>& client,
    PostgresConfig config)
    : m_state(std::make_shared<SharedState>(client, std::move(config)))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool PostgresConnectAwaitable<Strategy>::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
PostgresConnectAwaitable<Strategy>::SharedState::SharedState(
    AsyncPostgresClient<Strategy>& client_in,
    PostgresConfig config_in)
    : client(&client_in)
    , config(std::move(config_in))
    , host(galay::kernel::IPType::IPV4, config.host, config.port)
{
    if (config.host.empty() || config.username.empty() || config.port == 0 || !host.valid()) {
        result = std::unexpected(PostgresError(
            POSTGRES_ERROR_INVALID_PARAM,
            "PostgreSQL async connect requires an IPv4 address, username, and port"));
        phase = Phase::Invalid;
        return;
    }

    outgoing = client->encoder().encodeStartupMessage(config);
    if (outgoing.empty()) {
        result = std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                               "Invalid PostgreSQL startup parameters"));
        phase = Phase::Invalid;
        return;
    }
}

template<RingBufferBackendStrategy Strategy>
PostgresConnectAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void PostgresConnectAwaitable<Strategy>::Machine::setError(PostgresError error) noexcept
{
    m_state->client->setClosed(true);
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void PostgresConnectAwaitable<Strategy>::Machine::setIoError(
    const galay::kernel::IOError& error,
    PostgresErrorType fallback) noexcept
{
    PostgresError mapped = detail::mapIoError(error, fallback);
    if (detail::invalidatesConnection(error)) {
        setError(std::move(mapped));
        return;
    }
    m_state->result = std::unexpected(std::move(mapped));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void PostgresConnectAwaitable<Strategy>::Machine::completeSuccess() noexcept
{
    m_state->client->setClosed(false);
    m_state->phase = Phase::Done;
    m_state->result = std::optional<bool>(true);
}

template<RingBufferBackendStrategy Strategy>
bool PostgresConnectAwaitable<Strategy>::Machine::prepareReadWindow()
{
    PostgresError error(POSTGRES_ERROR_INTERNAL);
    if (!detail::prepareReadWindow(*m_state->client,
                                   m_state->read_iovecs,
                                   m_state->read_iov_count,
                                   error)) {
        setError(std::move(error));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, PostgresError>
PostgresConnectAwaitable<Strategy>::Machine::parseFromRingBuffer()
{
    while (true) {
        auto message_result = detail::peekMessage(*m_state->client, m_state->parse_scratch);
        if (!message_result) {
            return std::unexpected(message_result.error());
        }
        if (!message_result->has_value()) {
            return false;
        }

        const protocol::MessageView message = **message_result;
        if (message.type == protocol::kMsgAuthentication) {
            auto authentication = m_state->client->parser().parseAuthenticationRequest(
                message.payload, message.payload_len);
            if (!authentication) {
                return std::unexpected(detail::protocolError(
                    "Malformed PostgreSQL AuthenticationRequest"));
            }
            m_state->client->ringBuffer().consume(message.consumed);

            switch (authentication->kind) {
            case protocol::AuthRequestKind::Ok:
                m_state->authentication_ok = true;
                m_state->auth_stage = AuthStage::AwaitReadyForQuery;
                continue;
            case protocol::AuthRequestKind::Sasl: {
                if (m_state->auth_stage != AuthStage::AwaitAuthRequest ||
                    std::find(authentication->mechanisms.begin(),
                              authentication->mechanisms.end(),
                              "SCRAM-SHA-256") == authentication->mechanisms.end()) {
                    return std::unexpected(PostgresError(
                        POSTGRES_ERROR_AUTH,
                        "Server did not offer SCRAM-SHA-256 at the expected stage"));
                }
                auto nonce = protocol::ScramSha256::generateNonce();
                if (!nonce) {
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH, nonce.error()));
                }
                m_state->client_nonce = std::move(*nonce);
                auto first = m_state->scram.clientFirstMessage({}, m_state->client_nonce);
                if (!first) {
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH, first.error()));
                }
                m_state->outgoing = m_state->client->encoder().encodeSASLInitialResponse(
                    "SCRAM-SHA-256", *first);
                if (m_state->outgoing.empty()) {
                    return std::unexpected(PostgresError(
                        POSTGRES_ERROR_AUTH,
                        "Failed to encode PostgreSQL SCRAM initial response"));
                }
                m_state->sent = 0;
                m_state->auth_stage = AuthStage::SendSASLInitial;
                m_state->phase = Phase::AuthWrite;
                return true;
            }
            case protocol::AuthRequestKind::SaslContinue: {
                if (m_state->auth_stage != AuthStage::AwaitSASLContinue) {
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                         "Unexpected SCRAM continuation"));
                }
                auto parsed = m_state->scram.parseServerFirst(authentication->data);
                if (!parsed) {
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH, parsed.error()));
                }
                auto final_message = m_state->scram.clientFinalMessage(m_state->config.password);
                if (!final_message) {
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                         final_message.error()));
                }
                m_state->outgoing =
                    m_state->client->encoder().encodeSASLResponse(*final_message);
                if (m_state->outgoing.empty()) {
                    return std::unexpected(PostgresError(
                        POSTGRES_ERROR_AUTH,
                        "Failed to encode PostgreSQL SCRAM final response"));
                }
                m_state->sent = 0;
                m_state->auth_stage = AuthStage::SendSASLFinal;
                m_state->phase = Phase::AuthWrite;
                return true;
            }
            case protocol::AuthRequestKind::SaslFinal: {
                if (m_state->auth_stage != AuthStage::AwaitSASLFinal) {
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                         "Unexpected SCRAM final response"));
                }
                auto verified = m_state->scram.verifyServerFinal(authentication->data);
                if (!verified) {
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                         verified.error()));
                }
                m_state->server_signature_verified = true;
                m_state->auth_stage = AuthStage::AwaitAuthRequest;
                continue;
            }
            case protocol::AuthRequestKind::Md5Password: {
                if (authentication->data.size() != 4) {
                    return std::unexpected(detail::protocolError(
                        "Malformed PostgreSQL MD5 authentication salt"));
                }
                std::array<uint8_t, 4> salt{};
                std::memcpy(salt.data(), authentication->data.data(), salt.size());
                m_state->outgoing = m_state->client->encoder().encodePasswordMessage(
                    protocol::md5Password(m_state->config.username,
                                          m_state->config.password,
                                          salt));
                m_state->sent = 0;
                m_state->phase = Phase::AuthWrite;
                m_state->auth_stage = AuthStage::AwaitAuthRequest;
                return true;
            }
            case protocol::AuthRequestKind::CleartextPassword:
                m_state->outgoing = m_state->client->encoder().encodePasswordMessage(
                    m_state->config.password);
                m_state->sent = 0;
                m_state->phase = Phase::AuthWrite;
                m_state->auth_stage = AuthStage::AwaitAuthRequest;
                return true;
            default:
                return std::unexpected(PostgresError(
                    POSTGRES_ERROR_AUTH,
                    "Unsupported PostgreSQL authentication method"));
            }
        }

        if (message.type == protocol::kMsgParameterStatus ||
            message.type == protocol::kMsgBackendKeyData) {
            auto common = detail::consumeCommonMessage(*m_state->client, message);
            if (!common) {
                return std::unexpected(common.error());
            }
            m_state->client->ringBuffer().consume(message.consumed);
            continue;
        }
        if (message.type == protocol::kMsgNoticeResponse) {
            m_state->client->ringBuffer().consume(message.consumed);
            continue;
        }
        if (message.type == protocol::kMsgErrorResponse) {
            auto fields = m_state->client->parser().parseErrorResponse(message.payload,
                                                                      message.payload_len);
            m_state->client->ringBuffer().consume(message.consumed);
            if (!fields) {
                return std::unexpected(detail::protocolError("Malformed ErrorResponse"));
            }
            return std::unexpected(detail::serverError(std::move(*fields),
                                                       POSTGRES_ERROR_AUTH));
        }
        if (message.type == protocol::kMsgReadyForQuery) {
            auto ready = m_state->client->parser().parseReadyForQuery(message.payload,
                                                                     message.payload_len);
            m_state->client->ringBuffer().consume(message.consumed);
            if (!ready) {
                return std::unexpected(detail::protocolError("Malformed ReadyForQuery"));
            }
            if (!m_state->authentication_ok ||
                (!m_state->client_nonce.empty() && !m_state->server_signature_verified)) {
                return std::unexpected(PostgresError(
                    POSTGRES_ERROR_AUTH,
                    "Authentication did not complete before ReadyForQuery"));
            }
            m_state->client->setTransactionStatus(ready->transaction_status);
            m_state->phase = Phase::StartupComplete;
            return true;
        }
        return std::unexpected(detail::protocolError(
            "Unexpected PostgreSQL message during startup"));
    }
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename PostgresConnectAwaitable<Strategy>::Result>
PostgresConnectAwaitable<Strategy>::Machine::advance()
{
    using Action = galay::kernel::MachineAction<result_type>;
    if (m_state->result.has_value()) {
        return Action::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                               "PostgreSQL connect machine entered invalid state"));
        return Action::complete(std::move(*m_state->result));
    case Phase::Connect: {
        m_state->client->ringBuffer().clear();
        m_state->client->m_server_parameters.clear();
        m_state->client->m_backend_key_data.reset();
        m_state->client->setTransactionStatus('I');
        m_state->client->setClosed(true);

        auto nonblocking = m_state->client->socket().option().handleNonBlock();
        if (!nonblocking) {
            setError(PostgresError(POSTGRES_ERROR_CONNECTION,
                                   nonblocking.error().message()));
            return Action::complete(std::move(*m_state->result));
        }
        if (m_state->config.tcp_no_delay) {
            auto no_delay = m_state->client->socket().option().handleTcpNoDelay();
            if (!no_delay) {
                setError(PostgresError(POSTGRES_ERROR_CONNECTION,
                                       no_delay.error().message()));
                return Action::complete(std::move(*m_state->result));
            }
        }
        return Action::waitConnect(m_state->host);
    }
    case Phase::StartupWrite:
    case Phase::AuthWrite:
        if (m_state->sent >= m_state->outgoing.size()) {
            m_state->sent = 0;
            if (m_state->phase == Phase::StartupWrite) {
                m_state->auth_stage = AuthStage::AwaitAuthRequest;
            } else if (m_state->auth_stage == AuthStage::SendSASLInitial) {
                m_state->auth_stage = AuthStage::AwaitSASLContinue;
            } else if (m_state->auth_stage == AuthStage::SendSASLFinal) {
                m_state->auth_stage = AuthStage::AwaitSASLFinal;
            }
            m_state->phase = Phase::AuthRead;
            return Action::continue_();
        }
        return Action::waitWrite(m_state->outgoing.data() + m_state->sent,
                                 m_state->outgoing.size() - m_state->sent);
    case Phase::AuthRead: {
        auto parsed = parseFromRingBuffer();
        if (!parsed) {
            setError(std::move(parsed.error()));
            return Action::complete(std::move(*m_state->result));
        }
        if (*parsed) {
            return Action::continue_();
        }
        if (!prepareReadWindow()) {
            return Action::complete(std::move(*m_state->result));
        }
        return Action::waitReadv(m_state->read_iovecs.data(), m_state->read_iov_count);
    }
    case Phase::StartupComplete:
        completeSuccess();
        return Action::continue_();
    case Phase::Done:
        if (!m_state->result.has_value()) {
            completeSuccess();
        }
        return Action::complete(std::move(*m_state->result));
    }

    setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                           "Unknown PostgreSQL connect machine state"));
    return Action::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void PostgresConnectAwaitable<Strategy>::Machine::onConnect(
    std::expected<void, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result) {
        setIoError(result.error(), POSTGRES_ERROR_CONNECTION);
        return;
    }
    m_state->phase = Phase::StartupWrite;
}

template<RingBufferBackendStrategy Strategy>
void PostgresConnectAwaitable<Strategy>::Machine::onRead(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result) {
        setIoError(result.error(), POSTGRES_ERROR_RECV);
        return;
    }
    if (*result == 0) {
        setError(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                               "Connection closed during PostgreSQL startup"));
        return;
    }
    m_state->client->ringBuffer().produce(*result);
    auto parsed = parseFromRingBuffer();
    if (!parsed) {
        setError(std::move(parsed.error()));
    }
}

template<RingBufferBackendStrategy Strategy>
void PostgresConnectAwaitable<Strategy>::Machine::onWrite(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result) {
        setIoError(result.error(), POSTGRES_ERROR_SEND);
        return;
    }
    if (*result == 0) {
        setError(PostgresError(POSTGRES_ERROR_SEND,
                               "PostgreSQL startup send returned zero bytes"));
        return;
    }
    m_state->sent += *result;
}

template<RingBufferBackendStrategy Strategy>
PostgresQueryAwaitable<Strategy>::PostgresQueryAwaitable(
    AsyncPostgresClient<Strategy>& client,
    std::string_view sql)
    : m_state(std::make_shared<SharedState>(client, sql))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool PostgresQueryAwaitable<Strategy>::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
PostgresQueryAwaitable<Strategy>::SharedState::SharedState(
    AsyncPostgresClient<Strategy>& client_in,
    std::string_view sql)
    : client(&client_in)
    , encoded_cmd(client_in.encoder().encodeQuery(sql))
{
    if (client_in.isClosed()) {
        result = std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                                               "PostgreSQL connection is closed"));
        phase = Phase::Invalid;
    } else if (encoded_cmd.empty()) {
        result = std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                               "Invalid PostgreSQL query"));
        phase = Phase::Invalid;
    } else if (client_in.asyncConfig().result_row_reserve_hint != 0) {
        result_set.reserveRows(client_in.asyncConfig().result_row_reserve_hint);
    }
}

template<RingBufferBackendStrategy Strategy>
PostgresQueryAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void PostgresQueryAwaitable<Strategy>::Machine::setError(PostgresError error) noexcept
{
    if (detail::invalidatesConnection(error)) {
        m_state->client->setClosed(true);
    }
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void PostgresQueryAwaitable<Strategy>::Machine::setIoError(
    const galay::kernel::IOError& error,
    PostgresErrorType fallback) noexcept
{
    if (detail::invalidatesConnection(error)) {
        m_state->client->setClosed(true);
    }
    setError(detail::mapIoError(error, fallback));
}

template<RingBufferBackendStrategy Strategy>
bool PostgresQueryAwaitable<Strategy>::Machine::prepareReadWindow()
{
    PostgresError error(POSTGRES_ERROR_INTERNAL);
    if (!detail::prepareReadWindow(*m_state->client,
                                   m_state->read_iovecs,
                                   m_state->read_iov_count,
                                   error)) {
        setError(std::move(error));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, PostgresError>
PostgresQueryAwaitable<Strategy>::Machine::parseFromRingBuffer()
{
    while (true) {
        auto message_result = detail::peekMessage(*m_state->client, m_state->parse_scratch);
        if (!message_result) {
            return std::unexpected(message_result.error());
        }
        if (!message_result->has_value()) {
            return false;
        }
        const protocol::MessageView message = **message_result;

        switch (message.type) {
        case protocol::kMsgRowDescription: {
            auto fields = m_state->client->parser().parseRowDescription(message.payload,
                                                                       message.payload_len);
            if (!fields) {
                return std::unexpected(detail::protocolError("Malformed RowDescription"));
            }
            detail::appendFields(m_state->result_set, std::move(*fields));
            break;
        }
        case protocol::kMsgDataRow: {
            auto row = m_state->client->parser().parseDataRow(message.payload,
                                                             message.payload_len);
            if (!row || (m_state->result_set.fieldCount() != 0 &&
                         row->size() != m_state->result_set.fieldCount())) {
                return std::unexpected(detail::protocolError("Malformed DataRow"));
            }
            m_state->result_set.addRow(std::move(*row));
            break;
        }
        case protocol::kMsgCommandComplete: {
            auto complete = m_state->client->parser().parseCommandComplete(message.payload,
                                                                          message.payload_len);
            if (!complete) {
                return std::unexpected(detail::protocolError("Malformed CommandComplete"));
            }
            m_state->result_set.setCommandTag(std::move(complete->tag));
            m_state->result_set.setAffectedRows(complete->affected_rows);
            break;
        }
        case protocol::kMsgEmptyQueryResponse:
        case protocol::kMsgNoticeResponse:
            break;
        case protocol::kMsgParameterStatus: {
            auto common = detail::consumeCommonMessage(*m_state->client, message);
            if (!common) {
                return std::unexpected(common.error());
            }
            break;
        }
        case protocol::kMsgErrorResponse: {
            auto fields = m_state->client->parser().parseErrorResponse(message.payload,
                                                                      message.payload_len);
            if (!fields) {
                return std::unexpected(detail::protocolError("Malformed ErrorResponse"));
            }
            if (!m_state->pending_error.has_value()) {
                m_state->pending_error = detail::serverError(std::move(*fields));
            }
            break;
        }
        case protocol::kMsgReadyForQuery: {
            auto ready = m_state->client->parser().parseReadyForQuery(message.payload,
                                                                     message.payload_len);
            if (!ready) {
                return std::unexpected(detail::protocolError("Malformed ReadyForQuery"));
            }
            m_state->client->ringBuffer().consume(message.consumed);
            m_state->client->setTransactionStatus(ready->transaction_status);
            m_state->phase = Phase::Done;
            if (m_state->pending_error.has_value()) {
                m_state->result = std::unexpected(std::move(*m_state->pending_error));
            } else {
                m_state->result = std::optional<PostgresResultSet>(
                    std::move(m_state->result_set));
            }
            return true;
        }
        default:
            return std::unexpected(detail::protocolError(
                "Unexpected PostgreSQL simple-query response message"));
        }
        m_state->client->ringBuffer().consume(message.consumed);
    }
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename PostgresQueryAwaitable<Strategy>::Result>
PostgresQueryAwaitable<Strategy>::Machine::advance()
{
    using Action = galay::kernel::MachineAction<result_type>;
    if (m_state->result.has_value()) {
        return Action::complete(std::move(*m_state->result));
    }
    switch (m_state->phase) {
    case Phase::Invalid:
        setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                               "PostgreSQL query machine entered invalid state"));
        return Action::complete(std::move(*m_state->result));
    case Phase::SendCommand:
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->phase = Phase::Receiving;
            return Action::continue_();
        }
        return Action::waitWrite(m_state->encoded_cmd.data() + m_state->sent,
                                 m_state->encoded_cmd.size() - m_state->sent);
    case Phase::Receiving: {
        auto parsed = parseFromRingBuffer();
        if (!parsed) {
            setError(std::move(parsed.error()));
            return Action::complete(std::move(*m_state->result));
        }
        if (*parsed) {
            return Action::continue_();
        }
        if (!prepareReadWindow()) {
            return Action::complete(std::move(*m_state->result));
        }
        return Action::waitReadv(m_state->read_iovecs.data(), m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            m_state->result = std::optional<PostgresResultSet>(std::move(m_state->result_set));
        }
        return Action::complete(std::move(*m_state->result));
    }
    setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                           "Unknown PostgreSQL query machine state"));
    return Action::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void PostgresQueryAwaitable<Strategy>::Machine::onRead(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result) {
        setIoError(result.error(), POSTGRES_ERROR_RECV);
        return;
    }
    if (*result == 0) {
        m_state->client->setClosed(true);
        setError(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                               "Connection closed during PostgreSQL query"));
        return;
    }
    m_state->client->ringBuffer().produce(*result);
    auto parsed = parseFromRingBuffer();
    if (!parsed) {
        setError(std::move(parsed.error()));
    }
}

template<RingBufferBackendStrategy Strategy>
void PostgresQueryAwaitable<Strategy>::Machine::onWrite(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result) {
        setIoError(result.error(), POSTGRES_ERROR_SEND);
        return;
    }
    if (*result == 0) {
        setError(PostgresError(POSTGRES_ERROR_SEND,
                               "PostgreSQL query send returned zero bytes"));
        return;
    }
    m_state->sent += *result;
}

template<RingBufferBackendStrategy Strategy>
typename PostgresPrepareAwaitable<Strategy>::PrepareResult
PostgresPrepareAwaitable<Strategy>::PrepareResult::clone() const
{
    PrepareResult copy;
    copy.statement_name = statement_name;
    copy.parameter_types = parameter_types;
    copy.fields.reserve(fields.size());
    for (const auto& field : fields) {
        copy.fields.push_back(field.clone());
    }
    return copy;
}

template<RingBufferBackendStrategy Strategy>
PostgresPrepareAwaitable<Strategy>::PostgresPrepareAwaitable(
    AsyncPostgresClient<Strategy>& client,
    std::string_view name,
    std::string_view sql,
    std::span<const uint32_t> parameter_types)
    : m_state(std::make_shared<SharedState>(client, name, sql, parameter_types))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool PostgresPrepareAwaitable<Strategy>::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
PostgresPrepareAwaitable<Strategy>::SharedState::SharedState(
    AsyncPostgresClient<Strategy>& client_in,
    std::string_view name,
    std::string_view sql,
    std::span<const uint32_t> parameter_types)
    : client(&client_in)
{
    prepare_result.statement_name.assign(name);
    if (client->isClosed()) {
        result = std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                                               "PostgreSQL connection is closed"));
        phase = Phase::Invalid;
        return;
    }

    std::string parse = client->encoder().encodeParse(name, sql, parameter_types);
    std::string describe = client->encoder().encodeDescribeStatement(name);
    std::string sync = client->encoder().encodeSync();
    if (parse.empty() || describe.empty() || sync.empty()) {
        result = std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                               "Invalid PostgreSQL prepared statement"));
        phase = Phase::Invalid;
        return;
    }
    parse += describe;
    parse += sync;
    encoded_cmd = std::move(parse);
}

template<RingBufferBackendStrategy Strategy>
PostgresPrepareAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void PostgresPrepareAwaitable<Strategy>::Machine::setError(PostgresError error) noexcept
{
    if (detail::invalidatesConnection(error)) {
        m_state->client->setClosed(true);
    }
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void PostgresPrepareAwaitable<Strategy>::Machine::setIoError(
    const galay::kernel::IOError& error,
    PostgresErrorType fallback) noexcept
{
    if (detail::invalidatesConnection(error)) {
        m_state->client->setClosed(true);
    }
    setError(detail::mapIoError(error, fallback));
}

template<RingBufferBackendStrategy Strategy>
bool PostgresPrepareAwaitable<Strategy>::Machine::prepareReadWindow()
{
    PostgresError error(POSTGRES_ERROR_INTERNAL);
    if (!detail::prepareReadWindow(*m_state->client,
                                   m_state->read_iovecs,
                                   m_state->read_iov_count,
                                   error)) {
        setError(std::move(error));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, PostgresError>
PostgresPrepareAwaitable<Strategy>::Machine::parseFromRingBuffer()
{
    while (true) {
        auto message_result = detail::peekMessage(*m_state->client, m_state->parse_scratch);
        if (!message_result) {
            return std::unexpected(message_result.error());
        }
        if (!message_result->has_value()) {
            return false;
        }
        const protocol::MessageView message = **message_result;
        switch (message.type) {
        case protocol::kMsgParseComplete:
            if (!m_state->client->parser().parseParseComplete(message.payload,
                                                              message.payload_len)) {
                return std::unexpected(detail::protocolError("Malformed ParseComplete"));
            }
            break;
        case protocol::kMsgParameterDescription: {
            auto parameters = m_state->client->parser().parseParameterDescription(
                message.payload, message.payload_len);
            if (!parameters) {
                return std::unexpected(detail::protocolError(
                    "Malformed ParameterDescription"));
            }
            m_state->prepare_result.parameter_types = std::move(*parameters);
            break;
        }
        case protocol::kMsgRowDescription: {
            auto fields = m_state->client->parser().parseRowDescription(message.payload,
                                                                       message.payload_len);
            if (!fields) {
                return std::unexpected(detail::protocolError(
                    "Malformed prepared RowDescription"));
            }
            detail::appendFields(m_state->prepare_result.fields, std::move(*fields));
            break;
        }
        case protocol::kMsgNoData:
        case protocol::kMsgNoticeResponse:
            break;
        case protocol::kMsgParameterStatus: {
            auto common = detail::consumeCommonMessage(*m_state->client, message);
            if (!common) {
                return std::unexpected(common.error());
            }
            break;
        }
        case protocol::kMsgErrorResponse: {
            auto fields = m_state->client->parser().parseErrorResponse(message.payload,
                                                                      message.payload_len);
            if (!fields) {
                return std::unexpected(detail::protocolError("Malformed ErrorResponse"));
            }
            if (!m_state->pending_error.has_value()) {
                m_state->pending_error = detail::serverError(
                    std::move(*fields), POSTGRES_ERROR_PREPARED_STMT);
            }
            break;
        }
        case protocol::kMsgReadyForQuery: {
            auto ready = m_state->client->parser().parseReadyForQuery(message.payload,
                                                                     message.payload_len);
            if (!ready) {
                return std::unexpected(detail::protocolError("Malformed ReadyForQuery"));
            }
            m_state->client->ringBuffer().consume(message.consumed);
            m_state->client->setTransactionStatus(ready->transaction_status);
            m_state->phase = Phase::Done;
            if (m_state->pending_error.has_value()) {
                m_state->result = std::unexpected(std::move(*m_state->pending_error));
            } else {
                m_state->result = std::optional<PrepareResult>(
                    std::move(m_state->prepare_result));
            }
            return true;
        }
        default:
            return std::unexpected(detail::protocolError(
                "Unexpected PostgreSQL prepare response message"));
        }
        m_state->client->ringBuffer().consume(message.consumed);
    }
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename PostgresPrepareAwaitable<Strategy>::Result>
PostgresPrepareAwaitable<Strategy>::Machine::advance()
{
    using Action = galay::kernel::MachineAction<result_type>;
    if (m_state->result.has_value()) {
        return Action::complete(std::move(*m_state->result));
    }
    switch (m_state->phase) {
    case Phase::Invalid:
        setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                               "PostgreSQL prepare machine entered invalid state"));
        return Action::complete(std::move(*m_state->result));
    case Phase::SendCommand:
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->phase = Phase::Receiving;
            return Action::continue_();
        }
        return Action::waitWrite(m_state->encoded_cmd.data() + m_state->sent,
                                 m_state->encoded_cmd.size() - m_state->sent);
    case Phase::Receiving: {
        auto parsed = parseFromRingBuffer();
        if (!parsed) {
            setError(std::move(parsed.error()));
            return Action::complete(std::move(*m_state->result));
        }
        if (*parsed) {
            return Action::continue_();
        }
        if (!prepareReadWindow()) {
            return Action::complete(std::move(*m_state->result));
        }
        return Action::waitReadv(m_state->read_iovecs.data(), m_state->read_iov_count);
    }
    case Phase::Done:
        return Action::complete(std::move(*m_state->result));
    }
    setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                           "Unknown PostgreSQL prepare machine state"));
    return Action::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void PostgresPrepareAwaitable<Strategy>::Machine::onRead(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) return;
    if (!result) {
        setIoError(result.error(), POSTGRES_ERROR_RECV);
    } else if (*result == 0) {
        m_state->client->setClosed(true);
        setError(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                               "Connection closed during PostgreSQL prepare"));
    } else {
        m_state->client->ringBuffer().produce(*result);
        auto parsed = parseFromRingBuffer();
        if (!parsed) setError(std::move(parsed.error()));
    }
}

template<RingBufferBackendStrategy Strategy>
void PostgresPrepareAwaitable<Strategy>::Machine::onWrite(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) return;
    if (!result) {
        setIoError(result.error(), POSTGRES_ERROR_SEND);
    } else if (*result == 0) {
        setError(PostgresError(POSTGRES_ERROR_SEND,
                               "PostgreSQL prepare send returned zero bytes"));
    } else {
        m_state->sent += *result;
    }
}

template<RingBufferBackendStrategy Strategy>
PostgresExecuteAwaitable<Strategy>::PostgresExecuteAwaitable(
    AsyncPostgresClient<Strategy>& client,
    std::string_view name,
    std::span<const std::optional<std::string_view>> params)
    : m_state(std::make_shared<SharedState>(client, name, params))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool PostgresExecuteAwaitable<Strategy>::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
PostgresExecuteAwaitable<Strategy>::SharedState::SharedState(
    AsyncPostgresClient<Strategy>& client_in,
    std::string_view name,
    std::span<const std::optional<std::string_view>> params)
    : client(&client_in)
{
    if (client->isClosed()) {
        result = std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                                               "PostgreSQL connection is closed"));
        phase = Phase::Invalid;
        return;
    }

    std::string bind = client->encoder().encodeBind({}, name, params);
    std::string describe = client->encoder().encodeDescribePortal({});
    std::string execute = client->encoder().encodeExecute({});
    std::string sync = client->encoder().encodeSync();
    if (bind.empty() || describe.empty() || execute.empty() || sync.empty()) {
        result = std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                               "Invalid PostgreSQL prepared execution"));
        phase = Phase::Invalid;
        return;
    }
    bind += describe;
    bind += execute;
    bind += sync;
    encoded_cmd = std::move(bind);
    if (client->asyncConfig().result_row_reserve_hint != 0) {
        result_set.reserveRows(client->asyncConfig().result_row_reserve_hint);
    }
}

template<RingBufferBackendStrategy Strategy>
PostgresExecuteAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void PostgresExecuteAwaitable<Strategy>::Machine::setError(PostgresError error) noexcept
{
    if (detail::invalidatesConnection(error)) {
        m_state->client->setClosed(true);
    }
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void PostgresExecuteAwaitable<Strategy>::Machine::setIoError(
    const galay::kernel::IOError& error,
    PostgresErrorType fallback) noexcept
{
    if (detail::invalidatesConnection(error)) {
        m_state->client->setClosed(true);
    }
    setError(detail::mapIoError(error, fallback));
}

template<RingBufferBackendStrategy Strategy>
bool PostgresExecuteAwaitable<Strategy>::Machine::prepareReadWindow()
{
    PostgresError error(POSTGRES_ERROR_INTERNAL);
    if (!detail::prepareReadWindow(*m_state->client,
                                   m_state->read_iovecs,
                                   m_state->read_iov_count,
                                   error)) {
        setError(std::move(error));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, PostgresError>
PostgresExecuteAwaitable<Strategy>::Machine::parseFromRingBuffer()
{
    while (true) {
        auto message_result = detail::peekMessage(*m_state->client, m_state->parse_scratch);
        if (!message_result) return std::unexpected(message_result.error());
        if (!message_result->has_value()) return false;
        const protocol::MessageView message = **message_result;

        switch (message.type) {
        case protocol::kMsgBindComplete:
            if (!m_state->client->parser().parseBindComplete(message.payload,
                                                             message.payload_len)) {
                return std::unexpected(detail::protocolError("Malformed BindComplete"));
            }
            break;
        case protocol::kMsgRowDescription: {
            auto fields = m_state->client->parser().parseRowDescription(message.payload,
                                                                       message.payload_len);
            if (!fields) return std::unexpected(detail::protocolError("Malformed RowDescription"));
            detail::appendFields(m_state->result_set, std::move(*fields));
            break;
        }
        case protocol::kMsgDataRow: {
            auto row = m_state->client->parser().parseDataRow(message.payload,
                                                             message.payload_len);
            if (!row || (m_state->result_set.fieldCount() != 0 &&
                         row->size() != m_state->result_set.fieldCount())) {
                return std::unexpected(detail::protocolError("Malformed DataRow"));
            }
            m_state->result_set.addRow(std::move(*row));
            break;
        }
        case protocol::kMsgCommandComplete: {
            auto complete = m_state->client->parser().parseCommandComplete(message.payload,
                                                                          message.payload_len);
            if (!complete) return std::unexpected(detail::protocolError("Malformed CommandComplete"));
            m_state->result_set.setCommandTag(std::move(complete->tag));
            m_state->result_set.setAffectedRows(complete->affected_rows);
            break;
        }
        case protocol::kMsgNoData:
        case protocol::kMsgPortalSuspended:
        case protocol::kMsgNoticeResponse:
            break;
        case protocol::kMsgParameterStatus: {
            auto common = detail::consumeCommonMessage(*m_state->client, message);
            if (!common) return std::unexpected(common.error());
            break;
        }
        case protocol::kMsgErrorResponse: {
            auto fields = m_state->client->parser().parseErrorResponse(message.payload,
                                                                      message.payload_len);
            if (!fields) return std::unexpected(detail::protocolError("Malformed ErrorResponse"));
            if (!m_state->pending_error.has_value()) {
                m_state->pending_error = detail::serverError(
                    std::move(*fields), POSTGRES_ERROR_PREPARED_STMT);
            }
            break;
        }
        case protocol::kMsgReadyForQuery: {
            auto ready = m_state->client->parser().parseReadyForQuery(message.payload,
                                                                     message.payload_len);
            if (!ready) return std::unexpected(detail::protocolError("Malformed ReadyForQuery"));
            m_state->client->ringBuffer().consume(message.consumed);
            m_state->client->setTransactionStatus(ready->transaction_status);
            m_state->phase = Phase::Done;
            if (m_state->pending_error.has_value()) {
                m_state->result = std::unexpected(std::move(*m_state->pending_error));
            } else {
                m_state->result = std::optional<PostgresResultSet>(
                    std::move(m_state->result_set));
            }
            return true;
        }
        default:
            return std::unexpected(detail::protocolError(
                "Unexpected PostgreSQL execute response message"));
        }
        m_state->client->ringBuffer().consume(message.consumed);
    }
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename PostgresExecuteAwaitable<Strategy>::Result>
PostgresExecuteAwaitable<Strategy>::Machine::advance()
{
    using Action = galay::kernel::MachineAction<result_type>;
    if (m_state->result.has_value()) return Action::complete(std::move(*m_state->result));
    switch (m_state->phase) {
    case Phase::Invalid:
        setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                               "PostgreSQL execute machine entered invalid state"));
        return Action::complete(std::move(*m_state->result));
    case Phase::SendCommand:
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->phase = Phase::Receiving;
            return Action::continue_();
        }
        return Action::waitWrite(m_state->encoded_cmd.data() + m_state->sent,
                                 m_state->encoded_cmd.size() - m_state->sent);
    case Phase::Receiving: {
        auto parsed = parseFromRingBuffer();
        if (!parsed) {
            setError(std::move(parsed.error()));
            return Action::complete(std::move(*m_state->result));
        }
        if (*parsed) return Action::continue_();
        if (!prepareReadWindow()) return Action::complete(std::move(*m_state->result));
        return Action::waitReadv(m_state->read_iovecs.data(), m_state->read_iov_count);
    }
    case Phase::Done:
        return Action::complete(std::move(*m_state->result));
    }
    setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                           "Unknown PostgreSQL execute machine state"));
    return Action::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void PostgresExecuteAwaitable<Strategy>::Machine::onRead(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) return;
    if (!result) setIoError(result.error(), POSTGRES_ERROR_RECV);
    else if (*result == 0) {
        m_state->client->setClosed(true);
        setError(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                               "Connection closed during PostgreSQL execute"));
    } else {
        m_state->client->ringBuffer().produce(*result);
        auto parsed = parseFromRingBuffer();
        if (!parsed) setError(std::move(parsed.error()));
    }
}

template<RingBufferBackendStrategy Strategy>
void PostgresExecuteAwaitable<Strategy>::Machine::onWrite(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) return;
    if (!result) setIoError(result.error(), POSTGRES_ERROR_SEND);
    else if (*result == 0) {
        setError(PostgresError(POSTGRES_ERROR_SEND,
                               "PostgreSQL execute send returned zero bytes"));
    } else m_state->sent += *result;
}

template<RingBufferBackendStrategy Strategy>
PostgresPipelineAwaitable<Strategy>::PostgresPipelineAwaitable(
    AsyncPostgresClient<Strategy>& client,
    std::span<const protocol::PostgresCommandView> commands)
    : m_state(std::make_shared<SharedState>(client, commands))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool PostgresPipelineAwaitable<Strategy>::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
PostgresPipelineAwaitable<Strategy>::SharedState::SharedState(
    AsyncPostgresClient<Strategy>& client_in,
    std::span<const protocol::PostgresCommandView> commands)
    : client(&client_in)
{
    if (commands.empty()) {
        phase = Phase::Done;
        result = std::optional<std::vector<PostgresResultSet>>(
            std::vector<PostgresResultSet>{});
        return;
    }
    if (client->isClosed()) {
        phase = Phase::Invalid;
        result = std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                                               "PostgreSQL connection is closed"));
        return;
    }
    size_t bytes = 0;
    for (const auto& command : commands) {
        if (command.encoded.empty()) {
            phase = Phase::Invalid;
            result = std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                                   "Pipeline contains an empty command"));
            return;
        }
        bytes += command.encoded.size();
        if (command.kind == protocol::PostgresCommandKind::Query ||
            command.kind == protocol::PostgresCommandKind::Sync) {
            ++expected_ready;
        }
    }
    if (expected_ready == 0) {
        phase = Phase::Invalid;
        result = std::unexpected(PostgresError(
            POSTGRES_ERROR_INVALID_PARAM,
            "PostgreSQL pipeline must contain Query or Sync boundaries"));
        return;
    }
    encoded_commands.reserve(bytes);
    for (const auto& command : commands) {
        encoded_commands.append(command.encoded);
    }
    results.reserve(expected_ready);
    if (client->asyncConfig().result_row_reserve_hint != 0) {
        current_result.reserveRows(client->asyncConfig().result_row_reserve_hint);
    }
}

template<RingBufferBackendStrategy Strategy>
PostgresPipelineAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void PostgresPipelineAwaitable<Strategy>::Machine::setError(PostgresError error) noexcept
{
    if (detail::invalidatesConnection(error)) {
        m_state->client->setClosed(true);
    }
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void PostgresPipelineAwaitable<Strategy>::Machine::setIoError(
    const galay::kernel::IOError& error,
    PostgresErrorType fallback) noexcept
{
    if (detail::invalidatesConnection(error)) {
        m_state->client->setClosed(true);
    }
    setError(detail::mapIoError(error, fallback));
}

template<RingBufferBackendStrategy Strategy>
bool PostgresPipelineAwaitable<Strategy>::Machine::prepareReadWindow()
{
    PostgresError error(POSTGRES_ERROR_INTERNAL);
    if (!detail::prepareReadWindow(*m_state->client,
                                   m_state->read_iovecs,
                                   m_state->read_iov_count,
                                   error)) {
        setError(std::move(error));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, PostgresError>
PostgresPipelineAwaitable<Strategy>::Machine::parseFromRingBuffer()
{
    while (m_state->completed_ready < m_state->expected_ready) {
        auto message_result = detail::peekMessage(*m_state->client, m_state->parse_scratch);
        if (!message_result) return std::unexpected(message_result.error());
        if (!message_result->has_value()) return false;
        const protocol::MessageView message = **message_result;

        switch (message.type) {
        case protocol::kMsgRowDescription: {
            auto fields = m_state->client->parser().parseRowDescription(message.payload,
                                                                       message.payload_len);
            if (!fields) return std::unexpected(detail::protocolError("Malformed RowDescription"));
            detail::appendFields(m_state->current_result, std::move(*fields));
            break;
        }
        case protocol::kMsgDataRow: {
            auto row = m_state->client->parser().parseDataRow(message.payload,
                                                             message.payload_len);
            if (!row || (m_state->current_result.fieldCount() != 0 &&
                         row->size() != m_state->current_result.fieldCount())) {
                return std::unexpected(detail::protocolError("Malformed DataRow"));
            }
            m_state->current_result.addRow(std::move(*row));
            break;
        }
        case protocol::kMsgCommandComplete: {
            auto complete = m_state->client->parser().parseCommandComplete(message.payload,
                                                                          message.payload_len);
            if (!complete) return std::unexpected(detail::protocolError("Malformed CommandComplete"));
            m_state->current_result.setCommandTag(std::move(complete->tag));
            m_state->current_result.setAffectedRows(complete->affected_rows);
            break;
        }
        case protocol::kMsgParseComplete:
        case protocol::kMsgBindComplete:
        case protocol::kMsgNoData:
        case protocol::kMsgPortalSuspended:
        case protocol::kMsgCloseComplete:
        case protocol::kMsgEmptyQueryResponse:
        case protocol::kMsgNoticeResponse:
            break;
        case protocol::kMsgParameterStatus: {
            auto common = detail::consumeCommonMessage(*m_state->client, message);
            if (!common) return std::unexpected(common.error());
            break;
        }
        case protocol::kMsgErrorResponse: {
            auto fields = m_state->client->parser().parseErrorResponse(message.payload,
                                                                      message.payload_len);
            if (!fields) return std::unexpected(detail::protocolError("Malformed ErrorResponse"));
            if (!m_state->first_error.has_value()) {
                m_state->first_error = detail::serverError(std::move(*fields));
            }
            break;
        }
        case protocol::kMsgReadyForQuery: {
            auto ready = m_state->client->parser().parseReadyForQuery(message.payload,
                                                                     message.payload_len);
            if (!ready) return std::unexpected(detail::protocolError("Malformed ReadyForQuery"));
            m_state->client->setTransactionStatus(ready->transaction_status);
            m_state->client->ringBuffer().consume(message.consumed);
            m_state->results.push_back(std::move(m_state->current_result));
            ++m_state->completed_ready;
            if (m_state->completed_ready >= m_state->expected_ready) {
                m_state->phase = Phase::Done;
                if (m_state->first_error.has_value()) {
                    m_state->result = std::unexpected(std::move(*m_state->first_error));
                } else {
                    m_state->result = std::optional<std::vector<PostgresResultSet>>(
                        std::move(m_state->results));
                }
                return true;
            }
            m_state->current_result = PostgresResultSet{};
            if (m_state->client->asyncConfig().result_row_reserve_hint != 0) {
                m_state->current_result.reserveRows(
                    m_state->client->asyncConfig().result_row_reserve_hint);
            }
            continue;
        }
        default:
            return std::unexpected(detail::protocolError(
                "Unexpected PostgreSQL pipeline response message"));
        }
        m_state->client->ringBuffer().consume(message.consumed);
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename PostgresPipelineAwaitable<Strategy>::Result>
PostgresPipelineAwaitable<Strategy>::Machine::advance()
{
    using Action = galay::kernel::MachineAction<result_type>;
    if (m_state->result.has_value()) return Action::complete(std::move(*m_state->result));
    switch (m_state->phase) {
    case Phase::Invalid:
        setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                               "PostgreSQL pipeline machine entered invalid state"));
        return Action::complete(std::move(*m_state->result));
    case Phase::SendCommands:
        if (m_state->sent >= m_state->encoded_commands.size()) {
            m_state->phase = Phase::Receiving;
            return Action::continue_();
        }
        return Action::waitWrite(m_state->encoded_commands.data() + m_state->sent,
                                 m_state->encoded_commands.size() - m_state->sent);
    case Phase::Receiving: {
        auto parsed = parseFromRingBuffer();
        if (!parsed) {
            setError(std::move(parsed.error()));
            return Action::complete(std::move(*m_state->result));
        }
        if (*parsed) return Action::continue_();
        if (!prepareReadWindow()) return Action::complete(std::move(*m_state->result));
        return Action::waitReadv(m_state->read_iovecs.data(), m_state->read_iov_count);
    }
    case Phase::Done:
        return Action::complete(std::move(*m_state->result));
    }
    setError(PostgresError(POSTGRES_ERROR_INTERNAL,
                           "Unknown PostgreSQL pipeline machine state"));
    return Action::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void PostgresPipelineAwaitable<Strategy>::Machine::onRead(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) return;
    if (!result) setIoError(result.error(), POSTGRES_ERROR_RECV);
    else if (*result == 0) {
        m_state->client->setClosed(true);
        setError(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                               "Connection closed during PostgreSQL pipeline"));
    } else {
        m_state->client->ringBuffer().produce(*result);
        auto parsed = parseFromRingBuffer();
        if (!parsed) setError(std::move(parsed.error()));
    }
}

template<RingBufferBackendStrategy Strategy>
void PostgresPipelineAwaitable<Strategy>::Machine::onWrite(
    std::expected<size_t, galay::kernel::IOError> result)
{
    if (m_state->result.has_value()) return;
    if (!result) setIoError(result.error(), POSTGRES_ERROR_SEND);
    else if (*result == 0) {
        setError(PostgresError(POSTGRES_ERROR_SEND,
                               "PostgreSQL pipeline send returned zero bytes"));
    } else m_state->sent += *result;
}

} // namespace galay::postgres::details

#endif // GALAY_POSTGRES_DETAILS_AWAITABLE_INL
