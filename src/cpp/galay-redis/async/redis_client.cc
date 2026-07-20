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

    #include "../details/awaitable.inl"

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

    template struct detail::RedisExchangeSharedState<RingBufferBackendStrategy::Mmap>;
    template struct detail::RedisExchangeMachine<RingBufferBackendStrategy::Mmap>;
    template struct detail::RedisConnectSharedState<RingBufferBackendStrategy::Mmap>;
    template struct detail::RedisConnectMachine<RingBufferBackendStrategy::Mmap>;
    template class RedisClient<RingBufferBackendStrategy::Mmap>;

    template struct detail::RedisExchangeSharedState<RingBufferBackendStrategy::Vector>;
    template struct detail::RedisExchangeMachine<RingBufferBackendStrategy::Vector>;
    template struct detail::RedisConnectSharedState<RingBufferBackendStrategy::Vector>;
    template struct detail::RedisConnectMachine<RingBufferBackendStrategy::Vector>;
    template class RedisClient<RingBufferBackendStrategy::Vector>;

    template struct detail::RedisExchangeSharedState<RingBufferBackendStrategy::Auto>;
    template struct detail::RedisExchangeMachine<RingBufferBackendStrategy::Auto>;
    template struct detail::RedisConnectSharedState<RingBufferBackendStrategy::Auto>;
    template struct detail::RedisConnectMachine<RingBufferBackendStrategy::Auto>;
    template class RedisClient<RingBufferBackendStrategy::Auto>;
} // namespace galay::redis
