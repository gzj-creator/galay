#include "connection.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <cstring>
#include <cerrno>

namespace galay::redis::protocol
{
    Connection::Connection()
        : m_socket_fd(-1)
        , m_connected(false)
    {
        m_recv_buffer.resize(BUFFER_SIZE);
    }

    Connection::~Connection()
    {
        disconnect();
    }

    std::expected<void, RedisError> Connection::connect(const std::string& host,
                                                        int port,
                                                        uint32_t timeout_ms,
                                                        bool tcp_no_delay)
    {
        // 创建socket
        m_socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_socket_fd < 0) {
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Failed to create socket: " + std::string(strerror(errno))));
        }

        // 设置为非阻塞模式以支持连接超时
        int flags = fcntl(m_socket_fd, F_GETFL, 0);
        if (flags == -1 || fcntl(m_socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            ::close(m_socket_fd);
            m_socket_fd = -1;
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Failed to set non-blocking mode"));
        }
        if (tcp_no_delay) {
            int opt = 1;
            if (::setsockopt(m_socket_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) != 0) {
                ::close(m_socket_fd);
                m_socket_fd = -1;
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                    "Failed to set TCP_NODELAY: " + std::string(strerror(errno))));
            }
        }

        // 解析主机地址
        struct sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);

        // 尝试将host作为IP地址解析
        if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) != 1) {
            // DNS解析
            struct addrinfo hints, *result = nullptr;
            std::memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;

            if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
                ::close(m_socket_fd);
                m_socket_fd = -1;
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                    "Failed to resolve hostname: " + host));
            }

            if (result && result->ai_family == AF_INET) {
                server_addr.sin_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
            }
            freeaddrinfo(result);
        }

        // 尝试连接
        int connect_result = ::connect(m_socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));

        if (connect_result < 0 && errno != EINPROGRESS) {
            ::close(m_socket_fd);
            m_socket_fd = -1;
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Connection failed: " + std::string(strerror(errno))));
        }

        if (errno == EINPROGRESS) {
            // 等待连接完成（带超时）
            struct pollfd pfd;
            pfd.fd = m_socket_fd;
            pfd.events = POLLOUT;

            int poll_result = poll(&pfd, 1, timeout_ms);
            if (poll_result < 0) {
                ::close(m_socket_fd);
                m_socket_fd = -1;
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                    "Poll failed: " + std::string(strerror(errno))));
            } else if (poll_result == 0) {
                ::close(m_socket_fd);
                m_socket_fd = -1;
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                    "Connection timeout"));
            }

            // 检查连接是否成功
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(m_socket_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                ::close(m_socket_fd);
                m_socket_fd = -1;
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                    "Connection failed: " + std::string(strerror(error))));
            }
        }

        // 恢复阻塞模式
        if (fcntl(m_socket_fd, F_SETFL, flags) == -1) {
            ::close(m_socket_fd);
            m_socket_fd = -1;
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Failed to restore blocking mode"));
        }

        m_connected = true;
        return {};
    }

    void Connection::disconnect()
    {
        if (m_socket_fd >= 0) {
            ::close(m_socket_fd);
            m_socket_fd = -1;
        }
        m_connected = false;
    }

    std::expected<void, RedisError> Connection::send(const std::string& data)
    {
        if (!m_connected || m_socket_fd < 0) {
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Not connected"));
        }

        size_t total_sent = 0;
        while (total_sent < data.length()) {
            ssize_t sent = ::send(m_socket_fd, data.data() + total_sent, data.length() - total_sent, 0);
            if (sent < 0) {
                if (errno == EINTR) continue;
                m_connected = false;
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                    "Send failed: " + std::string(strerror(errno))));
            }
            total_sent += sent;
        }

        return {};
    }

    std::expected<RedisReply, RedisError> Connection::receiveReply()
    {
        if (!m_connected || m_socket_fd < 0) {
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Not connected"));
        }

        std::vector<char> buffer;
        buffer.reserve(BUFFER_SIZE);

        while (true) {
            // 尝试解析已接收的数据
            if (!buffer.empty()) {
                auto parse_result = m_parser.parse(buffer.data(), buffer.size());
                if (parse_result) {
                    return std::move(parse_result->second);
                } else if (parse_result.error() != ParseError::Incomplete) {
                    return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                        "Failed to parse response"));
                }
            }

            // 接收更多数据
            ssize_t received = ::recv(m_socket_fd, m_recv_buffer.data(), m_recv_buffer.size(), 0);
            if (received < 0) {
                if (errno == EINTR) continue;
                m_connected = false;
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR,
                    "Receive failed: " + std::string(strerror(errno))));
            } else if (received == 0) {
                m_connected = false;
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                    "Connection closed by peer"));
            }

            buffer.insert(buffer.end(), m_recv_buffer.begin(), m_recv_buffer.begin() + received);

            // 防止缓冲区无限增长
            if (buffer.size() > 1024 * 1024) {  // 1MB限制
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                    "Response too large"));
            }
        }
    }

    std::expected<RedisReply, RedisError> Connection::execute(const std::string& encoded_command)
    {
        auto send_result = send(encoded_command);
        if (!send_result) {
            return std::unexpected(send_result.error());
        }

        return receiveReply();
    }
}
