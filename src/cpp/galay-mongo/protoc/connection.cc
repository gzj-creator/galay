#include "connection.h"
#include <galay/cpp/galay-mongo/base/mongo_config.h>

#include <galay/cpp/galay-kernel/common/handle_option.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

namespace galay::mongo::protocol
{

namespace
{

int32_t readInt32LE(const char* p)
{
    return static_cast<int32_t>(
        (static_cast<uint32_t>(static_cast<uint8_t>(p[0]))      ) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) <<  8) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24));
}

} // namespace

Connection::ConnectOptions::ConnectOptions()
    : host(::galay::mongo::MongoConfig::kDefaultHost)
    , port(::galay::mongo::MongoConfig::kDefaultPort)
    , timeout_ms(::galay::mongo::MongoConfig::kDefaultConnectTimeoutMs)
    , tcp_nodelay(::galay::mongo::MongoConfig::kDefaultTcpNoDelay)
    , recv_buffer_size(::galay::mongo::MongoConfig::kDefaultRecvBufferSize)
{
}

Connection::ConnectOptions
Connection::ConnectOptions::fromMongoConfig(const ::galay::mongo::MongoConfig& config)
{
    ConnectOptions options;
    options.host = config.host;
    options.port = config.port;
    options.timeout_ms = config.connect_timeout_ms;
    options.tcp_nodelay = config.tcp_nodelay;
    options.recv_buffer_size = config.recv_buffer_size;
    return options;
}

Connection::Connection()
    : m_socket_fd(-1)
    , m_connected(false)
    , m_recv_ring(kDefaultBufferSize)
{
}

Connection::~Connection()
{
    disconnect();
}

Connection::Connection(Connection&& other) noexcept
    : m_socket_fd(other.m_socket_fd)
    , m_connected(other.m_connected)
    , m_recv_ring(std::move(other.m_recv_ring))
{
    other.m_socket_fd = -1;
    other.m_connected = false;
}

Connection& Connection::operator=(Connection&& other) noexcept
{
    if (this != &other) {
        disconnect();
        m_socket_fd = other.m_socket_fd;
        m_connected = other.m_connected;
        m_recv_ring = std::move(other.m_recv_ring);

        other.m_socket_fd = -1;
        other.m_connected = false;
    }
    return *this;
}

std::expected<void, MongoError>
Connection::connect(const ConnectOptions& options)
{
    if (m_connected) {
        disconnect();
    }

    m_socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket_fd < 0) {
        return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                          "Failed to create socket: " + std::string(std::strerror(errno))));
    }

    int flags = ::fcntl(m_socket_fd, F_GETFL, 0);
    if (flags < 0) {
        ::close(m_socket_fd);
        m_socket_fd = -1;
        return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                          "Failed to get socket flags: " + std::string(std::strerror(errno))));
    }

    if (::fcntl(m_socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(m_socket_fd);
        m_socket_fd = -1;
        return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                          "Failed to set non-blocking socket: " +
                                          std::string(std::strerror(errno))));
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options.port);

    if (::inet_pton(AF_INET, options.host.c_str(), &addr.sin_addr) <= 0) {
        struct addrinfo hints{};
        struct addrinfo* result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int ret = ::getaddrinfo(options.host.c_str(), nullptr, &hints, &result);
        if (ret != 0 || result == nullptr) {
            ::close(m_socket_fd);
            m_socket_fd = -1;
            return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                              "Failed to resolve host: " + options.host));
        }
        addr.sin_addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr)->sin_addr;
        ::freeaddrinfo(result);
    }

    int ret = ::connect(m_socket_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        ::close(m_socket_fd);
        m_socket_fd = -1;
        return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                          "Connect failed: " + std::string(std::strerror(errno))));
    }

    if (ret < 0) {
        struct pollfd pfd{};
        pfd.fd = m_socket_fd;
        pfd.events = POLLOUT;

        const int poll_ret = ::poll(&pfd, 1, static_cast<int>(options.timeout_ms));
        if (poll_ret <= 0) {
            ::close(m_socket_fd);
            m_socket_fd = -1;
            return std::unexpected(MongoError(MONGO_ERROR_TIMEOUT, "Connection timeout"));
        }

        int error = 0;
        socklen_t error_len = sizeof(error);
        ::getsockopt(m_socket_fd, SOL_SOCKET, SO_ERROR, &error, &error_len);
        if (error != 0) {
            ::close(m_socket_fd);
            m_socket_fd = -1;
            return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                              "Connect failed: " + std::string(std::strerror(error))));
        }
    }

    if (::fcntl(m_socket_fd, F_SETFL, flags) < 0) {
        ::close(m_socket_fd);
        m_socket_fd = -1;
        return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                          "Failed to restore blocking mode: " +
                                          std::string(std::strerror(errno))));
    }

    if (options.tcp_nodelay) {
        auto nodelay_result =
            ::galay::kernel::HandleOption(::GHandle{m_socket_fd}).handleTcpNoDelay();
        if (!nodelay_result.has_value()) {
            ::close(m_socket_fd);
            m_socket_fd = -1;
            return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                              "Failed to set TCP_NODELAY: " +
                                              nodelay_result.error().message()));
        }
    }

    m_connected = true;
    if (options.recv_buffer_size > 0 && m_recv_ring.capacity() != options.recv_buffer_size) {
        m_recv_ring = galay::utils::RingBuffer(options.recv_buffer_size);
    } else {
        m_recv_ring.clear();
    }

    return {};
}

void Connection::disconnect()
{
    if (m_socket_fd >= 0) {
        ::close(m_socket_fd);
        m_socket_fd = -1;
    }

    m_connected = false;
    m_recv_ring.clear();
}

std::expected<void, MongoError> Connection::send(const std::string& data)
{
    if (!m_connected || m_socket_fd < 0) {
        return std::unexpected(MongoError(MONGO_ERROR_CONNECTION_CLOSED, "Not connected"));
    }

    size_t sent_total = 0;
    while (sent_total < data.size()) {
        const ssize_t n = ::send(m_socket_fd,
                                 data.data() + sent_total,
                                 data.size() - sent_total,
                                 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            m_connected = false;
            return std::unexpected(MongoError(MONGO_ERROR_SEND,
                                              "Send failed: " + std::string(std::strerror(errno))));
        }

        if (n == 0) {
            m_connected = false;
            return std::unexpected(MongoError(MONGO_ERROR_CONNECTION_CLOSED,
                                              "Connection closed during send"));
        }

        sent_total += static_cast<size_t>(n);
    }

    return {};
}

std::expected<void, MongoError> Connection::ensureData(size_t n)
{
    if (n > m_recv_ring.capacity()) {
        return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM,
                                          "Requested bytes exceed receive ring buffer capacity"));
    }

    while (m_recv_ring.readable() < n) {
        if (m_recv_ring.writable() == 0) {
            return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                              "Receive ring buffer is full before enough data"));
        }

        std::array<struct iovec, 2> write_iovecs{};
        const size_t write_iovecs_count = m_recv_ring.getWriteIovecs(write_iovecs);
        if (write_iovecs_count == 0) {
            return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                              "Receive ring buffer has no writable regions"));
        }

        const ssize_t received = ::readv(m_socket_fd,
                                         write_iovecs.data(),
                                         static_cast<int>(write_iovecs_count));
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            m_connected = false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::unexpected(MongoError(MONGO_ERROR_TIMEOUT, "Recv timeout"));
            }
            return std::unexpected(MongoError(MONGO_ERROR_RECV,
                                              "Recv failed: " + std::string(std::strerror(errno))));
        }

        if (received == 0) {
            m_connected = false;
            return std::unexpected(MongoError(MONGO_ERROR_CONNECTION_CLOSED,
                                              "Connection closed during recv"));
        }

        m_recv_ring.produce(static_cast<size_t>(received));
    }

    return {};
}

std::expected<void, MongoError> Connection::recvExact(char* buffer, size_t n)
{
    size_t received_total = 0;
    while (received_total < n) {
        const ssize_t received = ::recv(m_socket_fd,
                                        buffer + received_total,
                                        n - received_total,
                                        0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            m_connected = false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::unexpected(MongoError(MONGO_ERROR_TIMEOUT, "Recv timeout"));
            }
            return std::unexpected(MongoError(MONGO_ERROR_RECV,
                                              "Recv failed: " + std::string(std::strerror(errno))));
        }
        if (received == 0) {
            m_connected = false;
            return std::unexpected(MongoError(MONGO_ERROR_CONNECTION_CLOSED,
                                              "Connection closed during recv"));
        }
        received_total += static_cast<size_t>(received);
    }
    return {};
}

void Connection::copyReadable(size_t offset, char* dst, size_t len) const
{
    std::array<struct iovec, 2> read_iovecs{};
    const size_t read_iovecs_count = m_recv_ring.getReadIovecs(read_iovecs);
    size_t skipped = offset;
    size_t copied = 0;

    for (size_t i = 0; i < read_iovecs_count; ++i) {
        const auto& iov = read_iovecs[i];
        if (copied >= len) {
            break;
        }

        const char* src = static_cast<const char*>(iov.iov_base);
        size_t src_len = iov.iov_len;
        if (skipped >= src_len) {
            skipped -= src_len;
            continue;
        }

        src += skipped;
        src_len -= skipped;
        skipped = 0;

        const size_t chunk = std::min(src_len, len - copied);
        std::memcpy(dst + copied, src, chunk);
        copied += chunk;
    }
}

std::string Connection::consumeToString(size_t len)
{
    std::string data(len, '\0');
    if (len > 0) {
        copyReadable(0, data.data(), len);
        m_recv_ring.consume(len);
    }
    return data;
}

std::expected<std::string, MongoError> Connection::recvBytes(size_t expected_len)
{
    if (expected_len <= m_recv_ring.capacity()) {
        auto ensured = ensureData(expected_len);
        if (!ensured) {
            return std::unexpected(ensured.error());
        }

        return consumeToString(expected_len);
    }

    std::string data(expected_len, '\0');
    size_t copied = 0;
    const size_t buffered = std::min(m_recv_ring.readable(), expected_len);
    if (buffered > 0) {
        copyReadable(0, data.data(), buffered);
        m_recv_ring.consume(buffered);
        copied = buffered;
    }

    auto direct_recv = recvExact(data.data() + copied, expected_len - copied);
    if (!direct_recv) {
        return std::unexpected(direct_recv.error());
    }

    return data;
}

std::expected<std::string, MongoError> Connection::recvMessageRaw()
{
    auto ensured = ensureData(4);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }

    char len_bytes[4];
    copyReadable(0, len_bytes, sizeof(len_bytes));
    const int32_t message_len = readInt32LE(len_bytes);
    if (message_len < 16 || static_cast<size_t>(message_len) > kMaxMessageSize) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                          "Invalid Mongo message length: " +
                                          std::to_string(message_len)));
    }

    const size_t message_size = static_cast<size_t>(message_len);
    if (message_size <= m_recv_ring.capacity()) {
        ensured = ensureData(message_size);
        if (!ensured) {
            return std::unexpected(ensured.error());
        }
        return consumeToString(message_size);
    }

    std::string raw(message_size, '\0');
    size_t copied = 0;

    const size_t buffered = std::min(m_recv_ring.readable(), message_size);
    if (buffered > 0) {
        copyReadable(0, raw.data(), buffered);
        m_recv_ring.consume(buffered);
        copied = buffered;
    }

    auto direct_recv = recvExact(raw.data() + copied, message_size - copied);
    if (!direct_recv) {
        return std::unexpected(direct_recv.error());
    }

    return raw;
}

std::expected<MongoMessage, MongoError> Connection::recvMessage()
{
    auto ensured = ensureData(4);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }

    char len_bytes[4];
    copyReadable(0, len_bytes, sizeof(len_bytes));
    const int32_t message_len = readInt32LE(len_bytes);
    if (message_len < 16 || static_cast<size_t>(message_len) > kMaxMessageSize) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                          "Invalid Mongo message length: " +
                                          std::to_string(message_len)));
    }

    const size_t message_size = static_cast<size_t>(message_len);
    if (message_size <= m_recv_ring.capacity()) {
        ensured = ensureData(message_size);
        if (!ensured) {
            return std::unexpected(ensured.error());
        }

        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = m_recv_ring.getReadIovecs(read_iovecs);
        if (read_iovecs_count == 0) {
            return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                              "Receive ring buffer has no readable regions"));
        }

        std::expected<MongoMessage, MongoError> decoded;
        if (read_iovecs[0].iov_len >= message_size) {
            decoded = MongoProtocol::decodeMessage(
                static_cast<const char*>(read_iovecs[0].iov_base),
                message_size);
        } else {
            m_decode_buffer.resize(message_size);
            copyReadable(0, m_decode_buffer.data(), message_size);
            decoded = MongoProtocol::decodeMessage(m_decode_buffer.data(), message_size);
        }

        m_recv_ring.consume(message_size);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        return decoded;
    }

    auto raw_or_err = recvMessageRaw();
    if (!raw_or_err) {
        return std::unexpected(raw_or_err.error());
    }
    return MongoProtocol::decodeMessage(raw_or_err->data(), raw_or_err->size());
}

} // namespace galay::mongo::protocol
