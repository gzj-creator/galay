#include "redis_client.h"

#include <galay/cpp/galay-redis/base/redis_log.h>

#include <galay/cpp/galay-utils/process/system.hpp>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
#include <regex>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::redis
{
    namespace detail
    {
        RedisError mapIoErrorToRedisErrorLocal(const IOError& io_error, RedisErrorType fallback)
        {
            if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR, io_error.message());
            }
            if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, io_error.message());
            }
            return RedisError(fallback, io_error.message());
        }

#ifdef GALAY_SSL_FEATURE_ENABLED
        RedisError mapSslErrorToRedisError(const galay::ssl::SslError& ssl_error,
                                           RedisErrorType fallback)
        {
            using galay::ssl::SslErrorCode;

            switch (ssl_error.code()) {
            case SslErrorCode::kTimeout:
            case SslErrorCode::kHandshakeTimeout:
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR, ssl_error.message());
            case SslErrorCode::kPeerClosed:
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, ssl_error.message());
            case SslErrorCode::kReadFailed:
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR, ssl_error.message());
            case SslErrorCode::kWriteFailed:
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR, ssl_error.message());
            case SslErrorCode::kVerificationFailed:
            case SslErrorCode::kHandshakeFailed:
            case SslErrorCode::kSNISetFailed:
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR, ssl_error.message());
            default:
                return RedisError(fallback, ssl_error.message());
            }
        }

        static size_t decimalDigits(size_t value)
        {
            size_t digits = 1;
            while (value >= 10) {
                value /= 10;
                ++digits;
            }
            return digits;
        }

        static size_t estimateRespCommandBytes(std::string_view cmd, std::span<const std::string_view> args)
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

        static ParseChunkResult parseRepliesFromChunk(protocol::RespParser& parser,
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

        static bool parseRepliesFromRingBuffer(galay::utils::RingBuffer& ring_buffer,
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
                parse_buffer.resize(first_tail_len + second_len);
                std::memcpy(parse_buffer.data(), first_data + first_tail_offset, first_tail_len);
                std::memcpy(parse_buffer.data() + first_tail_len, second_data, second_len);

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

        std::string encodePipelineBuffer(std::span<const RedisCommandView> commands)
        {
            protocol::RespEncoder encoder;
            std::string encoded;
            size_t encoded_bytes = 0;
            for (const auto& cmd_view : commands) {
                encoded_bytes += !cmd_view.encoded.empty()
                                     ? cmd_view.encoded.size()
                                     : estimateRespCommandBytes(cmd_view.command, cmd_view.args);
            }
            encoded.reserve(encoded_bytes);

            for (const auto& cmd_view : commands) {
                if (!cmd_view.encoded.empty()) {
                    encoded.append(cmd_view.encoded.data(), cmd_view.encoded.size());
                } else {
                    encoder.appendCommandFast(encoded, cmd_view.command, cmd_view.args);
                }
            }
            return encoded;
        }

        struct ParsedRedisEndpoint
        {
            std::string original_host;
            std::string resolved_host;
            int32_t port = 0;
            int version = 2;
            RedisConnectOptions options;
        };

        bool resolveRedisHost(std::string_view host, std::string* resolved_host, int* version)
        {
            if (!resolved_host || !version) {
                return false;
            }

            using galay::utils::System;
            *resolved_host = System::resolveHostIPv4(std::string(host));
            *version = 2;
            if (!resolved_host->empty()) {
                return true;
            }

            *resolved_host = System::resolveHostIPv6(std::string(host));
            *version = resolved_host->empty() ? 2 : 6;
            return !resolved_host->empty();
        }

        std::expected<ParsedRedisEndpoint, RedisError> parseRedisUrl(const std::string& url,
                                                                     std::string_view expected_scheme,
                                                                     int32_t default_port)
        {
            static const std::regex kUrlPattern(
                R"(^(redis|rediss)://(?:([^:@/]*)(?::([^@/]*))?@)?(\[[^\]]+\]|[^:/?#]+)(?::(\d+))?(?:/(\d+))?$)",
                std::regex::icase);

            std::smatch matches;
            if (!std::regex_match(url, matches, kUrlPattern)) {
                return std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_URL_INVALID_ERROR,
                    "Invalid redis url"));
            }

            std::string scheme = matches[1].str();
            std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (scheme != expected_scheme) {
                return std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_URL_INVALID_ERROR,
                    "Unexpected redis url scheme"));
            }

            ParsedRedisEndpoint parsed;
            if (matches[2].matched) {
                parsed.options.username = matches[2].str();
            }
            if (matches[3].matched) {
                parsed.options.password = matches[3].str();
            }

            parsed.original_host = matches[4].str();
            if (parsed.original_host.size() >= 2 &&
                parsed.original_host.front() == '[' &&
                parsed.original_host.back() == ']') {
                parsed.original_host = parsed.original_host.substr(1, parsed.original_host.size() - 2);
            }

            parsed.port = default_port;
            if (matches[5].matched) {
                try {
                    parsed.port = static_cast<int32_t>(std::stoi(matches[5].str()));
                } catch (const std::exception&) {
                    return std::unexpected(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_PORT_INVALID_ERROR,
                        "Invalid redis port"));
                }
            }

            if (matches[6].matched) {
                try {
                    parsed.options.db_index = static_cast<int32_t>(std::stoi(matches[6].str()));
                } catch (const std::exception&) {
                    return std::unexpected(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_DB_INDEX_INVALID_ERROR,
                        "Invalid redis db index"));
                }
            }

            using galay::utils::System;
            switch (System::checkAddressType(parsed.original_host)) {
            case System::AddressType::IPv4:
                parsed.resolved_host = parsed.original_host;
                parsed.version = 2;
                break;
            case System::AddressType::IPv6:
                parsed.resolved_host = parsed.original_host;
                parsed.version = 6;
                break;
            case System::AddressType::Domain:
            case System::AddressType::Invalid:
                if (!resolveRedisHost(parsed.original_host, &parsed.resolved_host, &parsed.version)) {
                    return std::unexpected(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_HOST_INVALID_ERROR,
                        "Failed to resolve redis host"));
                }
                break;
            }

            parsed.options.version = parsed.version;
            return parsed;
        }

        bool prepareReadWindow(galay::utils::RingBuffer& ring_buffer,
                               std::array<struct iovec, 2>& read_iovecs,
                               size_t& read_iov_count,
                               char*& read_buffer,
                               size_t& read_length)
        {
            read_iov_count = ring_buffer.getWriteIovecs(read_iovecs.data(), read_iovecs.size());
            if (read_iov_count == 0) {
                return false;
            }

            for (size_t i = 0; i < read_iov_count; ++i) {
                if (read_iovecs[i].iov_base != nullptr && read_iovecs[i].iov_len > 0) {
                    read_buffer = static_cast<char*>(read_iovecs[i].iov_base);
                    read_length = read_iovecs[i].iov_len;
                    return true;
                }
            }
            return false;
        }

        struct RedissClientImpl
        {
            RedissClientImpl(IOScheduler* scheduler_in,
                             AsyncRedisConfig config_in,
                             RedissClientConfig tls_config_in)
                : scheduler(scheduler_in)
                , config(std::move(config_in))
                , tls_config(std::move(tls_config_in))
                , ring_buffer(std::make_shared<galay::utils::RingBuffer>(config.buffer_size))
                , ready_socket(IPType::IPV4)
                , ssl_context(galay::ssl::SslMethod::TLS_Client)
            {
                if (!ssl_context.isValid()) {
                    boot_error = RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                        ssl_context.error().message());
                    return;
                }

                if (!tls_config.ca_path.empty()) {
                    const auto load_ca = ssl_context.loadCACertificate(tls_config.ca_path);
                    if (!load_ca) {
                        boot_error = mapSslErrorToRedisError(
                            load_ca.error(),
                            RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR);
                        return;
                    }
                } else if (tls_config.verify_peer) {
                    const auto load_default_ca = ssl_context.useDefaultCA();
                    if (!load_default_ca) {
                        boot_error = mapSslErrorToRedisError(
                            load_default_ca.error(),
                            RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR);
                        return;
                    }
                }

                if (tls_config.verify_peer) {
                    ssl_context.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
                    ssl_context.setVerifyDepth(tls_config.verify_depth);
                } else {
                    ssl_context.setVerifyMode(galay::ssl::SslVerifyMode::None);
                }
            }

            void resetProtocolState()
            {
                parser = protocol::RespParser();
                ring_buffer->clear();
            }

            std::expected<void, RedisError> resetSocket(int version, std::string_view server_name)
            {
                const auto ip_type = version == 6 ? IPType::IPV6 : IPType::IPV4;
                try {
                    socket.reset();
                    socket.emplace(&ssl_context, ip_type);
                } catch (const std::exception& e) {
                    return std::unexpected(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                        e.what()));
                }

                const auto non_block = socket->option().handleNonBlock();
                if (!non_block) {
                    return std::unexpected(mapIoErrorToRedisErrorLocal(
                        non_block.error(),
                        RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR));
                }
                if (config.tcp_no_delay) {
                    const auto nodelay = socket->option().handleTcpNoDelay();
                    if (!nodelay) {
                        return std::unexpected(mapIoErrorToRedisErrorLocal(
                            nodelay.error(),
                            RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR));
                    }
                }

                if (!server_name.empty()) {
                    const auto sni_result = socket->setHostname(std::string(server_name));
                    if (!sni_result) {
                        REDIS_LOG_WARN("[client]", "Failed to set TLS SNI to {}: {}",
                                     server_name,
                                     sni_result.error().message());
                    }
                }

                resetProtocolState();
                is_closed = true;
                return {};
            }

            galay::ssl::SslSocket& socketRef()
            {
                return *socket;
            }

            galay::kernel::IOController* readyController()
            {
                if (socket.has_value()) {
                    return socketRef().controller();
                }
                return ready_socket.controller();
            }

            IOScheduler* scheduler = nullptr;
            AsyncRedisConfig config;
            RedissClientConfig tls_config;
            std::shared_ptr<galay::utils::RingBuffer> ring_buffer;
            TcpSocket ready_socket;
            protocol::RespParser parser;
            bool is_closed = true;
            std::optional<RedisError> boot_error;
            galay::ssl::SslContext ssl_context;
            std::optional<galay::ssl::SslSocket> socket;
        };
#else
        struct RedissClientImpl
        {
            RedissClientImpl(IOScheduler* scheduler_in,
                             AsyncRedisConfig config_in,
                             RedissClientConfig tls_config_in)
                : scheduler(scheduler_in)
                , config(std::move(config_in))
                , tls_config(std::move(tls_config_in))
                , ring_buffer(std::make_shared<galay::utils::RingBuffer>(config.buffer_size))
                , ready_socket(IPType::IPV4)
            {
                boot_error = RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                    "galay-redis was built without SSL support");
            }

            void resetProtocolState()
            {
                ring_buffer->clear();
            }

            IOScheduler* scheduler = nullptr;
            AsyncRedisConfig config;
            RedissClientConfig tls_config;
            std::shared_ptr<galay::utils::RingBuffer> ring_buffer;
            TcpSocket ready_socket;
            bool is_closed = true;
            std::optional<RedisError> boot_error;
        };
#endif
    } // namespace detail

#ifdef GALAY_SSL_FEATURE_ENABLED
    namespace detail
    {
        RedissExchangeSharedState::RedissExchangeSharedState(RedissClientImpl* impl_in,
                                                             std::string encoded_command_in,
                                                             size_t expected_replies_in,
                                                             bool recv_only_in)
            : impl(impl_in)
            , encoded_cmd(std::move(encoded_command_in))
            , expected_replies(expected_replies_in)
            , recv_only(recv_only_in)
        {
            if (recv_only) {
                encoded_cmd.clear();
            }
            values.reserve(expected_replies);
            if (impl == nullptr) {
                result = std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss client impl is null"));
                phase = Phase::Invalid;
                return;
            }
            if (expected_replies == 0) {
                result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
                phase = Phase::Done;
            }
        }

        RedissExchangeMachine::RedissExchangeMachine(std::shared_ptr<RedissExchangeSharedState> state)
            : m_state(std::move(state))
        {
        }

        void RedissExchangeMachine::setError(RedisError error) noexcept
        {
            m_state->result = std::unexpected(std::move(error));
            m_state->phase = RedissExchangeSharedState::Phase::Invalid;
        }

        void RedissExchangeMachine::setSendError(const galay::ssl::SslError& ssl_error) noexcept
        {
            setError(mapSslErrorToRedisError(
                ssl_error,
                RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR));
        }

        void RedissExchangeMachine::setRecvError(const galay::ssl::SslError& ssl_error) noexcept
        {
            setError(mapSslErrorToRedisError(
                ssl_error,
                RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR));
        }

        bool RedissExchangeMachine::prepareReadWindow()
        {
            if (!::galay::redis::detail::prepareReadWindow(*m_state->impl->ring_buffer,
                                                           m_state->read_iovecs,
                                                           m_state->read_iov_count,
                                                           m_state->read_buffer,
                                                           m_state->read_length)) {
                setError(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                    "Ring buffer exhausted before parsing complete response"));
                return false;
            }
            return true;
        }

        std::expected<bool, RedisError> RedissExchangeMachine::tryParseReplies()
        {
            bool parse_error = false;
            const bool done = parseRepliesFromRingBuffer(*m_state->impl->ring_buffer,
                                                         m_state->impl->parser,
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

        galay::ssl::SslMachineAction<RedissExchangeMachine::result_type>
        RedissExchangeMachine::advance()
        {
            if (!m_state) {
                return galay::ssl::SslMachineAction<result_type>::complete(std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss exchange state is null")));
            }

            if (m_state->result.has_value()) {
                return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_state->result));
            }

            switch (m_state->phase) {
            case RedissExchangeSharedState::Phase::Invalid:
                setError(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss exchange machine in invalid state"));
                return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_state->result));
            case RedissExchangeSharedState::Phase::Start:
                if (m_state->expected_replies == 0) {
                    m_state->result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
                    m_state->phase = RedissExchangeSharedState::Phase::Done;
                    return galay::ssl::SslMachineAction<result_type>::continue_();
                }
                m_state->phase = (m_state->recv_only || m_state->encoded_cmd.empty())
                    ? RedissExchangeSharedState::Phase::Parse
                    : RedissExchangeSharedState::Phase::Send;
                return galay::ssl::SslMachineAction<result_type>::continue_();
            case RedissExchangeSharedState::Phase::Send:
                if (m_state->sent >= m_state->encoded_cmd.size()) {
                    m_state->phase = RedissExchangeSharedState::Phase::Parse;
                    return galay::ssl::SslMachineAction<result_type>::continue_();
                }
                return galay::ssl::SslMachineAction<result_type>::send(
                    m_state->encoded_cmd.data() + m_state->sent,
                    m_state->encoded_cmd.size() - m_state->sent);
            case RedissExchangeSharedState::Phase::Parse: {
                auto parsed = tryParseReplies();
                if (!parsed.has_value()) {
                    setError(std::move(parsed.error()));
                    return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_state->result));
                }
                if (parsed.value()) {
                    m_state->result = std::optional<std::vector<RedisValue>>(std::move(m_state->values));
                    m_state->phase = RedissExchangeSharedState::Phase::Done;
                    return galay::ssl::SslMachineAction<result_type>::continue_();
                }
                if (!prepareReadWindow()) {
                    return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_state->result));
                }
                return galay::ssl::SslMachineAction<result_type>::recv(
                    m_state->read_buffer,
                    m_state->read_length);
            }
            case RedissExchangeSharedState::Phase::Done:
                return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_state->result));
            }

            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Unknown rediss exchange state"));
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_state->result));
        }

        void RedissExchangeMachine::onHandshake(std::expected<void, galay::ssl::SslError>)
        {
        }

        void RedissExchangeMachine::onRecv(std::expected<galay::utils::Bytes, galay::ssl::SslError> result)
        {
            if (!m_state || m_state->result.has_value()) {
                return;
            }
            if (!result) {
                setRecvError(result.error());
                return;
            }
            if (result->empty()) {
                setError(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                    "TLS redis connection closed"));
                return;
            }

            m_state->impl->ring_buffer->produce(result->size());
            m_state->phase = RedissExchangeSharedState::Phase::Parse;
        }

        void RedissExchangeMachine::onSend(std::expected<size_t, galay::ssl::SslError> result)
        {
            if (!m_state || m_state->result.has_value()) {
                return;
            }
            if (!result) {
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
                m_state->phase = RedissExchangeSharedState::Phase::Parse;
            }
        }

        void RedissExchangeMachine::onShutdown(std::expected<void, galay::ssl::SslError>)
        {
        }

        RedissConnectSharedState::RedissConnectSharedState(RedissClientImpl* impl_in,
                                                           std::string ip_in,
                                                           int32_t port_in,
                                                           RedisConnectOptions options_in)
            : impl(impl_in)
            , ip(std::move(ip_in))
            , port(port_in)
            , options(std::move(options_in))
            , host(options.version == 6 ? IPType::IPV6 : IPType::IPV4, ip, port)
        {
            values.reserve(1);
            if (impl == nullptr) {
                result = std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss client impl is null"));
                phase = Phase::Invalid;
                return;
            }

            impl->resetProtocolState();
            impl->is_closed = true;
        }

        RedissConnectMachine::RedissConnectMachine(std::shared_ptr<RedissConnectSharedState> state)
            : m_state(std::move(state))
            , m_driver((m_state && m_state->impl && m_state->impl->socket.has_value())
                           ? &m_state->impl->socketRef()
                           : nullptr)
        {
        }

        void RedissConnectMachine::setError(RedisError error) noexcept
        {
            m_state->result = std::unexpected(std::move(error));
            m_state->phase = RedissConnectSharedState::Phase::Invalid;
            m_ssl_active = false;
        }

        void RedissConnectMachine::setConnectError(const IOError& io_error) noexcept
        {
            setError(mapIoErrorToRedisErrorLocal(
                io_error,
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR));
        }

        void RedissConnectMachine::setSendError(const galay::ssl::SslError& ssl_error) noexcept
        {
            setError(mapSslErrorToRedisError(
                ssl_error,
                RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR));
        }

        void RedissConnectMachine::setRecvError(const galay::ssl::SslError& ssl_error) noexcept
        {
            setError(mapSslErrorToRedisError(
                ssl_error,
                RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR));
        }

        bool RedissConnectMachine::prepareReadWindow()
        {
            if (!::galay::redis::detail::prepareReadWindow(*m_state->impl->ring_buffer,
                                                           m_state->read_iovecs,
                                                           m_state->read_iov_count,
                                                           m_state->read_buffer,
                                                           m_state->read_length)) {
                setError(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                    "No writable TLS read window for response"));
                return false;
            }
            return true;
        }

        bool RedissConnectMachine::prepareNextCommand()
        {
            RedisCommandBuilder builder;
            if (!m_state->auth_sent &&
                (!m_state->options.username.empty() || !m_state->options.password.empty())) {
                m_state->pending_command = RedissConnectSharedState::PendingCommand::Auth;
                m_state->auth_sent = true;
                m_state->encoded_cmd = m_state->options.username.empty()
                    ? builder.auth(m_state->options.password).encoded
                    : builder.auth(m_state->options.username, m_state->options.password).encoded;
                m_state->sent = 0;
                return true;
            }

            if (!m_state->select_sent && m_state->options.db_index != 0) {
                m_state->pending_command = RedissConnectSharedState::PendingCommand::Select;
                m_state->select_sent = true;
                m_state->encoded_cmd = builder.select(m_state->options.db_index).encoded;
                m_state->sent = 0;
                return true;
            }

            m_state->pending_command = RedissConnectSharedState::PendingCommand::None;
            m_state->encoded_cmd.clear();
            return false;
        }

        std::expected<bool, RedisError> RedissConnectMachine::tryParseReply()
        {
            bool parse_error = false;
            const bool done = parseRepliesFromRingBuffer(*m_state->impl->ring_buffer,
                                                         m_state->impl->parser,
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
                const auto error_type =
                    m_state->pending_command == RedissConnectSharedState::PendingCommand::Auth
                        ? RedisErrorType::REDIS_ERROR_TYPE_AUTH_ERROR
                        : RedisErrorType::REDIS_ERROR_TYPE_INVALID_ERROR;
                return std::unexpected(RedisError(error_type, reply.toError()));
            }

            if (prepareNextCommand()) {
                m_state->phase = RedissConnectSharedState::Phase::Send;
            } else {
                m_state->phase = RedissConnectSharedState::Phase::Done;
                m_state->impl->is_closed = false;
                m_state->result = RedisVoidResult{};
            }
            return true;
        }

        galay::kernel::MachineAction<RedissConnectMachine::result_type>
        RedissConnectMachine::advanceSsl()
        {
            auto wait = m_driver.poll();
            if (m_driver.completed()) {
                if (m_state->phase == RedissConnectSharedState::Phase::Handshake) {
                    handleHandshakeResult(m_driver.takeHandshakeResult());
                } else if (m_state->phase == RedissConnectSharedState::Phase::Send) {
                    handleSendResult(m_driver.takeSendResult());
                } else if (m_state->phase == RedissConnectSharedState::Phase::Parse) {
                    handleRecvResult(m_driver.takeRecvResult());
                } else {
                    setError(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                        "Unexpected TLS connect driver phase"));
                }
                return advance();
            }

            if (wait.kind == galay::ssl::SslOperationDriver::WaitKind::kRead) {
                return galay::kernel::MachineAction<result_type>::waitRead(
                    m_driver.recvContext().m_buffer,
                    m_driver.recvContext().m_length);
            }
            if (wait.kind == galay::ssl::SslOperationDriver::WaitKind::kWrite) {
                return galay::kernel::MachineAction<result_type>::waitWrite(
                    m_driver.sendContext().m_buffer,
                    m_driver.sendContext().m_length);
            }

            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "TLS connect driver returned no wait action"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        void RedissConnectMachine::handleHandshakeResult(std::expected<void, galay::ssl::SslError> result)
        {
            m_ssl_active = false;
            if (!result) {
                setError(mapSslErrorToRedisError(
                    result.error(),
                    RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR));
                return;
            }

            if (prepareNextCommand()) {
                m_state->phase = RedissConnectSharedState::Phase::Send;
            } else {
                m_state->phase = RedissConnectSharedState::Phase::Done;
                m_state->impl->is_closed = false;
                m_state->result = RedisVoidResult{};
            }
        }

        void RedissConnectMachine::handleSendResult(std::expected<size_t, galay::ssl::SslError> result)
        {
            m_ssl_active = false;
            if (!result) {
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
                m_state->phase = RedissConnectSharedState::Phase::Parse;
            }
        }

        void RedissConnectMachine::handleRecvResult(std::expected<galay::utils::Bytes, galay::ssl::SslError> result)
        {
            m_ssl_active = false;
            if (!result) {
                setRecvError(result.error());
                return;
            }
            if (result->empty()) {
                setError(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                    "TLS redis connection closed"));
                return;
            }

            m_state->impl->ring_buffer->produce(result->size());
            m_state->phase = RedissConnectSharedState::Phase::Parse;
        }

        galay::kernel::MachineAction<RedissConnectMachine::result_type>
        RedissConnectMachine::advance()
        {
            if (!m_state) {
                return galay::kernel::MachineAction<result_type>::complete(std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss connect state is null")));
            }

            if (m_state->result.has_value()) {
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }

            switch (m_state->phase) {
            case RedissConnectSharedState::Phase::Invalid:
                setError(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss connect machine in invalid state"));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            case RedissConnectSharedState::Phase::Connect:
                return galay::kernel::MachineAction<result_type>::waitConnect(m_state->host);
            case RedissConnectSharedState::Phase::Handshake:
                if (!m_ssl_active) {
                    m_driver.startHandshake();
                    m_ssl_active = true;
                }
                return advanceSsl();
            case RedissConnectSharedState::Phase::Send:
                if (m_state->sent >= m_state->encoded_cmd.size()) {
                    m_state->phase = RedissConnectSharedState::Phase::Parse;
                    m_ssl_active = false;
                    return galay::kernel::MachineAction<result_type>::continue_();
                }
                if (!m_ssl_active) {
                    m_driver.startSend(m_state->encoded_cmd.data() + m_state->sent,
                                       m_state->encoded_cmd.size() - m_state->sent);
                    m_ssl_active = true;
                }
                return advanceSsl();
            case RedissConnectSharedState::Phase::Parse: {
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
                if (!m_ssl_active) {
                    m_driver.startRecv(m_state->read_buffer, m_state->read_length);
                    m_ssl_active = true;
                }
                return advanceSsl();
            }
            case RedissConnectSharedState::Phase::Done:
                if (!m_state->result.has_value()) {
                    m_state->result = RedisVoidResult{};
                }
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }

            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Unknown rediss connect state"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        void RedissConnectMachine::onConnect(std::expected<void, IOError> result)
        {
            if (m_state->result.has_value()) {
                return;
            }
            if (!result.has_value()) {
                setConnectError(result.error());
                return;
            }

            m_state->phase = RedissConnectSharedState::Phase::Handshake;
        }

        void RedissConnectMachine::onRead(std::expected<size_t, IOError> result)
        {
            if (m_state->result.has_value()) {
                return;
            }
            m_driver.onRead(std::move(result));
        }

        void RedissConnectMachine::onWrite(std::expected<size_t, IOError> result)
        {
            if (m_state->result.has_value()) {
                return;
            }
            m_driver.onWrite(std::move(result));
        }

        RedissExchangeOperation makeReadyExchangeOperation(galay::kernel::IOController* controller,
                                                           galay::ssl::SslSocket* socket,
                                                           RedissCommandResult result)
        {
            auto state = std::make_shared<RedissExchangeSharedState>(nullptr, std::string(), 0, false);
            state->result = std::move(result);
            state->phase = RedissExchangeSharedState::Phase::Done;
            return galay::ssl::SslAwaitableBuilder<RedissCommandResult>::fromStateMachine(
                       controller,
                       socket,
                       RedissExchangeMachine(std::move(state)))
                .build();
        }

        RedissConnectOperation makeReadyConnectOperation(galay::kernel::IOController* controller,
                                                         RedisVoidResult result)
        {
            auto state = std::make_shared<RedissConnectSharedState>(nullptr, std::string(), 0, RedisConnectOptions{});
            state->result = std::move(result);
            state->phase = RedissConnectSharedState::Phase::Done;
            return galay::kernel::AwaitableBuilder<RedisVoidResult>::fromStateMachine(
                       controller,
                       RedissConnectMachine(std::move(state)))
                .build();
        }
    } // namespace detail
#endif

    RedissClient::RedissClient(IOScheduler* scheduler,
                               AsyncRedisConfig config,
                               RedissClientConfig tls_config)
        : m_impl(std::make_unique<detail::RedissClientImpl>(
              scheduler,
              std::move(config),
              std::move(tls_config)))
    {
    }

    RedissClient::RedissClient(RedissClient&& other) noexcept = default;
    RedissClient& RedissClient::operator=(RedissClient&& other) noexcept = default;
    RedissClient::~RedissClient() = default;

    detail::RedissConnectOperation RedissClient::connect(const std::string& url)
    {
#ifdef GALAY_SSL_FEATURE_ENABLED
        if (!m_impl) {
            return detail::makeReadyConnectOperation(
                nullptr,
                std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss client impl is null")));
        }
        if (m_impl->boot_error.has_value()) {
            return detail::makeReadyConnectOperation(
                m_impl->readyController(),
                std::unexpected(*m_impl->boot_error));
        }

        auto parsed = detail::parseRedisUrl(url, "rediss", 6380);
        if (!parsed.has_value()) {
            return detail::makeReadyConnectOperation(
                m_impl->readyController(),
                std::unexpected(parsed.error()));
        }

        const std::string server_name = !m_impl->tls_config.server_name.empty()
            ? m_impl->tls_config.server_name
            : parsed->original_host;
        const auto reset_socket = m_impl->resetSocket(parsed->version, server_name);
        if (!reset_socket) {
            return detail::makeReadyConnectOperation(
                m_impl->readyController(),
                std::unexpected(reset_socket.error()));
        }

        auto state = std::make_shared<detail::RedissConnectSharedState>(
            m_impl.get(),
            parsed->resolved_host,
            parsed->port,
            parsed->options);
        return galay::kernel::AwaitableBuilder<RedisVoidResult>::fromStateMachine(
                   m_impl->socketRef().controller(),
                   detail::RedissConnectMachine(std::move(state)))
            .build();
#else
        (void)url;
        return galay::kernel::AwaitableBuilder<RedisVoidResult>::ready(
            std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "galay-redis was built without SSL support")));
#endif
    }

    detail::RedissConnectOperation RedissClient::connect(const std::string& ip,
                                                         int32_t port,
                                                         RedisConnectOptions options)
    {
#ifdef GALAY_SSL_FEATURE_ENABLED
        if (!m_impl) {
            return detail::makeReadyConnectOperation(
                nullptr,
                std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss client impl is null")));
        }
        if (m_impl->boot_error.has_value()) {
            return detail::makeReadyConnectOperation(
                m_impl->readyController(),
                std::unexpected(*m_impl->boot_error));
        }

        const auto reset_socket = m_impl->resetSocket(options.version, m_impl->tls_config.server_name);
        if (!reset_socket) {
            return detail::makeReadyConnectOperation(
                m_impl->readyController(),
                std::unexpected(reset_socket.error()));
        }

        auto state = std::make_shared<detail::RedissConnectSharedState>(
            m_impl.get(),
            ip,
            port,
            std::move(options));
        return galay::kernel::AwaitableBuilder<RedisVoidResult>::fromStateMachine(
                   m_impl->socketRef().controller(),
                   detail::RedissConnectMachine(std::move(state)))
            .build();
#else
        (void)ip;
        (void)port;
        (void)options;
        return galay::kernel::AwaitableBuilder<RedisVoidResult>::ready(
            std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "galay-redis was built without SSL support")));
#endif
    }

    detail::RedissExchangeOperation RedissClient::command(RedisEncodedCommand command_packet)
    {
#ifdef GALAY_SSL_FEATURE_ENABLED
        if (!m_impl) {
            return detail::makeReadyExchangeOperation(
                nullptr,
                nullptr,
                std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss client impl is null")));
        }
        if (m_impl->boot_error.has_value()) {
            return detail::makeReadyExchangeOperation(
                m_impl->readyController(),
                m_impl->socket.has_value() ? &m_impl->socketRef() : nullptr,
                std::unexpected(*m_impl->boot_error));
        }
        if (m_impl->is_closed) {
            return detail::makeReadyExchangeOperation(
                m_impl->readyController(),
                m_impl->socket.has_value() ? &m_impl->socketRef() : nullptr,
                std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                    "Rediss client is not connected")));
        }

        auto state = std::make_shared<detail::RedissExchangeSharedState>(
            m_impl.get(),
            std::move(command_packet.encoded),
            command_packet.expected_replies,
            false);
        return galay::ssl::SslAwaitableBuilder<detail::RedissCommandResult>::fromStateMachine(
                   m_impl->socketRef().controller(),
                   &m_impl->socketRef(),
                   detail::RedissExchangeMachine(std::move(state)))
            .build();
#else
        (void)command_packet;
        return galay::kernel::AwaitableBuilder<detail::RedissCommandResult>::ready(
            std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "galay-redis was built without SSL support")));
#endif
    }

    detail::RedissExchangeOperation RedissClient::receive(size_t expected_replies)
    {
#ifdef GALAY_SSL_FEATURE_ENABLED
        if (!m_impl) {
            return detail::makeReadyExchangeOperation(
                nullptr,
                nullptr,
                std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss client impl is null")));
        }
        if (m_impl->boot_error.has_value()) {
            return detail::makeReadyExchangeOperation(
                m_impl->readyController(),
                m_impl->socket.has_value() ? &m_impl->socketRef() : nullptr,
                std::unexpected(*m_impl->boot_error));
        }
        if (m_impl->is_closed) {
            return detail::makeReadyExchangeOperation(
                m_impl->readyController(),
                m_impl->socket.has_value() ? &m_impl->socketRef() : nullptr,
                std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                    "Rediss client is not connected")));
        }

        auto state = std::make_shared<detail::RedissExchangeSharedState>(
            m_impl.get(),
            std::string(),
            expected_replies,
            true);
        return galay::ssl::SslAwaitableBuilder<detail::RedissCommandResult>::fromStateMachine(
                   m_impl->socketRef().controller(),
                   &m_impl->socketRef(),
                   detail::RedissExchangeMachine(std::move(state)))
            .build();
#else
        (void)expected_replies;
        return galay::kernel::AwaitableBuilder<detail::RedissCommandResult>::ready(
            std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "galay-redis was built without SSL support")));
#endif
    }

    detail::RedissExchangeOperation RedissClient::batch(std::span<const RedisCommandView> commands)
    {
#ifdef GALAY_SSL_FEATURE_ENABLED
        if (!m_impl) {
            return detail::makeReadyExchangeOperation(
                nullptr,
                nullptr,
                std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Rediss client impl is null")));
        }
        if (m_impl->boot_error.has_value()) {
            return detail::makeReadyExchangeOperation(
                m_impl->readyController(),
                m_impl->socket.has_value() ? &m_impl->socketRef() : nullptr,
                std::unexpected(*m_impl->boot_error));
        }
        if (m_impl->is_closed) {
            return detail::makeReadyExchangeOperation(
                m_impl->readyController(),
                m_impl->socket.has_value() ? &m_impl->socketRef() : nullptr,
                std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                    "Rediss client is not connected")));
        }

        auto state = std::make_shared<detail::RedissExchangeSharedState>(
            m_impl.get(),
            detail::encodePipelineBuffer(commands),
            commands.size(),
            false);
        return galay::ssl::SslAwaitableBuilder<detail::RedissCommandResult>::fromStateMachine(
                   m_impl->socketRef().controller(),
                   &m_impl->socketRef(),
                   detail::RedissExchangeMachine(std::move(state)))
            .build();
#else
        (void)commands;
        return galay::kernel::AwaitableBuilder<detail::RedissCommandResult>::ready(
            std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "galay-redis was built without SSL support")));
#endif
    }

    const AsyncRedisConfig& RedissClient::asyncConfig() const
    {
        return m_impl->config;
    }

    const RedissClientConfig& RedissClient::tlsConfig() const
    {
        return m_impl->tls_config;
    }

    bool RedissClient::isClosed() const
    {
        return !m_impl || m_impl->is_closed;
    }

    void RedissClient::setClosed(bool closed)
    {
        if (m_impl) {
            m_impl->is_closed = closed;
        }
    }

    galay::kernel::CloseAwaitable RedissClient::close()
    {
        m_impl->is_closed = true;
        m_impl->resetProtocolState();
#ifdef GALAY_SSL_FEATURE_ENABLED
        if (!m_impl->socket.has_value()) {
            (void)m_impl->resetSocket(2, std::string_view{});
        }
        return m_impl->socketRef().close();
#else
        return m_impl->ready_socket.close();
#endif
    }
} // namespace galay::redis
