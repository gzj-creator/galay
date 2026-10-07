#include "etcd_client.h"

#include "../base/etcd_internal.h"
#include "../base/etcd_log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace galay::etcd
{

using namespace internal;

namespace
{

bool contains_ascii_ignore_case(std::string_view value, std::string_view needle)
{
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > value.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= value.size(); ++i) {
        if (equals_ascii_ignore_case(value.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

void append_unsigned_decimal(std::string& out, size_t value)
{
    char digits[32];
    auto [ptr, ec] = std::to_chars(digits, digits + sizeof(digits), value);
    if (ec == std::errc()) {
        out.append(digits, static_cast<size_t>(ptr - digits));
        return;
    }
    out += std::to_string(value);
}

void build_json_post_request(
    std::string& request,
    std::string_view uri_prefix,
    std::string_view api_path,
    std::string_view body,
    std::string_view host_header,
    bool keepalive)
{
    request.clear();
    request.reserve(uri_prefix.size() + api_path.size() + body.size() + 256);
    request += "POST ";
    request += uri_prefix;
    request += api_path;
    request += " HTTP/1.1\r\n";
    request += "Host: ";
    request += host_header;
    request += "\r\n";
    request += "Accept: application/json\r\n";
    request += "Connection: ";
    request += keepalive ? "keep-alive\r\n" : "close\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: ";
    append_unsigned_decimal(request, body.size());
    request += "\r\n\r\n";
    request += body;
}

bool is_timeout_errno(int error_number)
{
    return error_number == EAGAIN || error_number == EWOULDBLOCK || error_number == ETIMEDOUT;
}

EtcdError make_errno_error(EtcdErrorType type, const std::string& action, int error_number)
{
    return EtcdError(
        type,
        action + ": " + std::string(std::strerror(error_number)));
}

bool set_socket_blocking(int fd, bool blocking)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }

    if (blocking) {
        flags &= ~O_NONBLOCK;
    } else {
        flags |= O_NONBLOCK;
    }
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

EtcdVoidResult connect_with_timeout(
    int fd,
    const sockaddr* address,
    socklen_t address_len,
    std::chrono::milliseconds timeout)
{
    if (timeout.count() < 0) {
        if (::connect(fd, address, address_len) == 0) {
            return {};
        }
        const int error_number = errno;
        if (is_timeout_errno(error_number)) {
            return std::unexpected(make_errno_error(EtcdErrorType::Timeout, "connect timeout", error_number));
        }
        return std::unexpected(make_errno_error(EtcdErrorType::Connection, "connect failed", error_number));
    }

    if (!set_socket_blocking(fd, false)) {
        return std::unexpected(make_errno_error(EtcdErrorType::Connection, "set nonblocking for connect failed", errno));
    }

    if (::connect(fd, address, address_len) == 0) {
        if (!set_socket_blocking(fd, true)) {
            return std::unexpected(make_errno_error(EtcdErrorType::Connection, "restore blocking mode failed", errno));
        }
        return {};
    }

    if (errno != EINPROGRESS) {
        const int error_number = errno;
        (void)set_socket_blocking(fd, true);
        if (is_timeout_errno(error_number)) {
            return std::unexpected(make_errno_error(EtcdErrorType::Timeout, "connect timeout", error_number));
        }
        return std::unexpected(make_errno_error(EtcdErrorType::Connection, "connect failed", error_number));
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    const long long timeout_count = timeout.count();
    const int timeout_ms = timeout_count > static_cast<long long>(INT_MAX)
        ? INT_MAX
        : static_cast<int>(std::max<long long>(0, timeout_count));

    int poll_result = 0;
    do {
        poll_result = ::poll(&pfd, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);

    if (poll_result == 0) {
        (void)set_socket_blocking(fd, true);
        return std::unexpected(EtcdError(EtcdErrorType::Timeout, "connect timeout"));
    }
    if (poll_result < 0) {
        const int error_number = errno;
        (void)set_socket_blocking(fd, true);
        return std::unexpected(make_errno_error(EtcdErrorType::Connection, "poll connect failed", error_number));
    }

    int socket_error = 0;
    socklen_t socket_error_len = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) != 0) {
        const int error_number = errno;
        (void)set_socket_blocking(fd, true);
        return std::unexpected(make_errno_error(EtcdErrorType::Connection, "getsockopt connect failed", error_number));
    }

    if (!set_socket_blocking(fd, true)) {
        return std::unexpected(make_errno_error(EtcdErrorType::Connection, "restore blocking mode failed", errno));
    }

    if (socket_error != 0) {
        if (is_timeout_errno(socket_error)) {
            return std::unexpected(make_errno_error(EtcdErrorType::Timeout, "connect timeout", socket_error));
        }
        return std::unexpected(make_errno_error(EtcdErrorType::Connection, "connect failed", socket_error));
    }

    return {};
}

EtcdVoidResult send_all(int fd, std::string_view payload)
{
    size_t sent = 0;
    while (sent < payload.size()) {
        const char* begin = payload.data() + sent;
        const size_t remaining = payload.size() - sent;
        const ssize_t sent_now = ::send(fd, begin, remaining, 0);
        if (sent_now > 0) {
            sent += static_cast<size_t>(sent_now);
            continue;
        }
        if (sent_now == 0) {
            return std::unexpected(EtcdError(EtcdErrorType::Send, "send returned zero"));
        }
        if (errno == EINTR) {
            continue;
        }
        if (is_timeout_errno(errno)) {
            return std::unexpected(make_errno_error(EtcdErrorType::Timeout, "send timeout", errno));
        }
        return std::unexpected(make_errno_error(EtcdErrorType::Send, "send failed", errno));
    }
    return {};
}

struct ParsedHttpHeaders
{
    std::optional<size_t> content_length = std::nullopt;
    int status_code = 0;
    bool chunked = false;
    bool connection_close = false;
};

std::expected<ParsedHttpHeaders, EtcdError> parse_http_headers(std::string_view header_block)
{
    ParsedHttpHeaders headers;

    const size_t status_line_end = header_block.find("\r\n");
    const std::string_view status_line = status_line_end == std::string_view::npos
        ? header_block
        : header_block.substr(0, status_line_end);
    if (status_line.empty()) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, "invalid http response status line"));
    }
    const size_t first_space = status_line.find(' ');
    if (first_space == std::string_view::npos) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, "invalid http status line format"));
    }
    size_t second_space = status_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        second_space = status_line.size();
    }

    int status_code = 0;
    const std::string_view status_code_view = trim_ascii(status_line.substr(first_space + 1, second_space - first_space - 1));
    const char* code_begin = status_code_view.data();
    const char* code_end = code_begin + status_code_view.size();
    auto [status_ptr, status_ec] = std::from_chars(code_begin, code_end, status_code);
    if (status_ec != std::errc() || status_ptr != code_end) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, "invalid http status code"));
    }
    headers.status_code = status_code;

    size_t line_pos = status_line_end == std::string_view::npos
        ? header_block.size()
        : status_line_end + 2;
    while (line_pos < header_block.size()) {
        size_t line_end = header_block.find("\r\n", line_pos);
        if (line_end == std::string_view::npos) {
            line_end = header_block.size();
        }
        if (line_end == line_pos) {
            line_pos = line_end + 2;
            continue;
        }

        const std::string_view line = header_block.substr(line_pos, line_end - line_pos);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view key = trim_ascii(line.substr(0, colon));
            const std::string_view value = trim_ascii(line.substr(colon + 1));

            if (equals_ascii_ignore_case(key, "content-length")) {
                uint64_t parsed = 0;
                const char* len_begin = value.data();
                const char* len_end = len_begin + value.size();
                auto [len_ptr, len_ec] = std::from_chars(len_begin, len_end, parsed);
                if (len_ec != std::errc() || len_ptr != len_end) {
                    return std::unexpected(EtcdError(EtcdErrorType::Parse, "invalid content-length value"));
                }
                if (parsed > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                    return std::unexpected(EtcdError(EtcdErrorType::Parse, "content-length value too large"));
                }
                headers.content_length = static_cast<size_t>(parsed);
            } else if (equals_ascii_ignore_case(key, "transfer-encoding")) {
                if (contains_ascii_token_ignore_case(value, "chunked")) {
                    headers.chunked = true;
                }
            } else if (equals_ascii_ignore_case(key, "connection")) {
                if (contains_ascii_token_ignore_case(value, "close")) {
                    headers.connection_close = true;
                }
            }
        }

        if (line_end == header_block.size()) {
            break;
        }
        line_pos = line_end + 2;
    }

    return headers;
}

enum class ChunkDecodeState
{
    Complete,
    Incomplete,
    Error,
};

struct ChunkDecodeResult
{
    std::string body;
    std::string error;
    size_t consumed = 0;
    ChunkDecodeState state = ChunkDecodeState::Incomplete;
};

ChunkDecodeResult decode_chunked_body(std::string_view raw)
{
    ChunkDecodeResult result;
    size_t pos = 0;
    std::string decoded;
    decoded.reserve(raw.size());

    auto make_error = [](const std::string& message) {
        ChunkDecodeResult res;
        res.state = ChunkDecodeState::Error;
        res.error = message;
        return res;
    };

    while (true) {
        const size_t line_end = raw.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            return result;
        }

        std::string_view size_line = trim_ascii(raw.substr(pos, line_end - pos));
        const size_t ext_sep = size_line.find(';');
        if (ext_sep != std::string_view::npos) {
            size_line = trim_ascii(size_line.substr(0, ext_sep));
        }
        if (size_line.empty()) {
            return make_error("invalid chunk size line");
        }

        uint64_t chunk_size = 0;
        auto [size_ptr, size_ec] = std::from_chars(
            size_line.data(),
            size_line.data() + size_line.size(),
            chunk_size,
            16);
        if (size_ec != std::errc() || size_ptr != size_line.data() + size_line.size()) {
            return make_error("invalid chunk size value");
        }

        pos = line_end + 2;
        if (chunk_size == 0) {
            const size_t trailer_end = raw.find("\r\n\r\n", pos);
            if (trailer_end == std::string_view::npos) {
                return result;
            }
            result.state = ChunkDecodeState::Complete;
            result.body = std::move(decoded);
            result.consumed = trailer_end + 4;
            return result;
        }

        if (chunk_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max() - 2)) {
            return make_error("chunk size overflows buffer bounds");
        }
        const size_t body_size = static_cast<size_t>(chunk_size);
        if (pos > std::numeric_limits<size_t>::max() - body_size - 2) {
            return make_error("chunk size overflows buffer bounds");
        }
        if (raw.size() < pos + body_size + 2) {
            return result;
        }

        decoded.append(raw.data() + pos, body_size);
        pos += body_size;
        if (raw.compare(pos, 2, "\r\n") != 0) {
            return make_error("missing CRLF after chunk data");
        }
        pos += 2;
    }
}

struct HttpResponseData
{
    std::string body;
    int status_code = 0;
    bool connection_close = false;
};

std::expected<HttpResponseData, EtcdError> recv_http_response(
    int fd,
    size_t buffer_size,
    std::string& raw,
    std::vector<char>& buffer)
{
    const size_t read_size = std::max<size_t>(buffer_size, 1024);
    raw.clear();
    if (raw.capacity() < read_size * 2) {
        raw.reserve(read_size * 2);
    }
    if (buffer.size() < read_size) {
        buffer.resize(read_size);
    }

    std::optional<ParsedHttpHeaders> headers = std::nullopt;
    size_t header_end = std::string::npos;
    bool peer_closed = false;

    while (true) {
        if (headers.has_value()) {
            const size_t body_offset = header_end + 4;

            if (headers->chunked) {
                auto chunked = decode_chunked_body(std::string_view(raw.data() + body_offset, raw.size() - body_offset));
                if (chunked.state == ChunkDecodeState::Complete) {
                    return HttpResponseData{
                        .body = std::move(chunked.body),
                        .status_code = headers->status_code,
                        .connection_close = headers->connection_close || peer_closed,
                    };
                }
                if (chunked.state == ChunkDecodeState::Error) {
                    return std::unexpected(EtcdError(EtcdErrorType::Parse, chunked.error));
                }
            } else if (headers->content_length.has_value()) {
                const size_t total_needed = body_offset + headers->content_length.value();
                if (raw.size() >= total_needed) {
                    return HttpResponseData{
                        .body = raw.substr(body_offset, headers->content_length.value()),
                        .status_code = headers->status_code,
                        .connection_close = headers->connection_close || peer_closed,
                    };
                }
            } else if (peer_closed) {
                return HttpResponseData{
                    .body = raw.substr(body_offset),
                    .status_code = headers->status_code,
                    .connection_close = true,
                };
            }
        }

        const ssize_t recv_bytes = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (recv_bytes > 0) {
            raw.append(buffer.data(), static_cast<size_t>(recv_bytes));
            if (!headers.has_value()) {
                header_end = raw.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    auto parsed = parse_http_headers(std::string_view(raw.data(), header_end));
                    if (!parsed.has_value()) {
                        return std::unexpected(parsed.error());
                    }
                    headers = parsed.value();

                    if (!headers->chunked &&
                        !headers->content_length.has_value() &&
                        !headers->connection_close) {
                        return std::unexpected(EtcdError(
                            EtcdErrorType::Parse,
                            "response missing content-length or chunked encoding"));
                    }
                }
            }
            continue;
        }

        if (recv_bytes == 0) {
            peer_closed = true;
            if (!headers.has_value()) {
                return std::unexpected(EtcdError(EtcdErrorType::Connection, "connection closed before response header"));
            }

            const size_t body_offset = header_end + 4;
            if (headers->chunked) {
                auto chunked = decode_chunked_body(std::string_view(raw.data() + body_offset, raw.size() - body_offset));
                if (chunked.state == ChunkDecodeState::Complete) {
                    return HttpResponseData{
                        .body = std::move(chunked.body),
                        .status_code = headers->status_code,
                        .connection_close = true,
                    };
                }
                return std::unexpected(EtcdError(EtcdErrorType::Recv, "connection closed before complete chunked body"));
            }

            if (headers->content_length.has_value()) {
                const size_t expected_size = body_offset + headers->content_length.value();
                if (raw.size() < expected_size) {
                    return std::unexpected(EtcdError(EtcdErrorType::Recv, "connection closed before complete response body"));
                }
                return HttpResponseData{
                    .body = raw.substr(body_offset, headers->content_length.value()),
                    .status_code = headers->status_code,
                    .connection_close = true,
                };
            }

            return HttpResponseData{
                .body = raw.substr(body_offset),
                .status_code = headers->status_code,
                .connection_close = true,
            };
        }

        if (errno == EINTR) {
            continue;
        }
        if (is_timeout_errno(errno)) {
            return std::unexpected(make_errno_error(EtcdErrorType::Timeout, "recv timeout", errno));
        }
        return std::unexpected(make_errno_error(EtcdErrorType::Recv, "recv failed", errno));
    }
}

} // namespace

EtcdClient::EtcdClient(EtcdConfig config)
    : m_config(std::move(config))
    , m_network_config(m_config)
    , m_api_prefix(normalize_api_prefix(m_config.api_prefix))
{
    m_request_buffer.reserve(512);
    m_response_raw_buffer.reserve(std::max<size_t>(m_network_config.buffer_size, 1024) * 2);
    m_recv_buffer.resize(std::max<size_t>(m_network_config.buffer_size, 1024));

    auto endpoint_result = parse_endpoint(m_config.endpoint);
    if (!endpoint_result.has_value()) {
        m_endpoint_error = endpoint_result.error();
        ETCD_LOG_WARN("[sync] [init]", "invalid endpoint endpoint={} error={}",
                      m_config.endpoint,
                      m_endpoint_error);
        return;
    }

    if (endpoint_result->secure) {
        m_endpoint_error = "https endpoint is not supported in EtcdClient: " + m_config.endpoint;
        ETCD_LOG_WARN("[sync] [init]", "unsupported https endpoint={}", m_config.endpoint);
        return;
    }

    m_endpoint_host = endpoint_result->host;
    m_endpoint_port = endpoint_result->port;
    m_endpoint_secure = endpoint_result->secure;
    m_endpoint_ipv6 = endpoint_result->ipv6;
    m_host_header = build_host_header(endpoint_result->host, endpoint_result->port, endpoint_result->ipv6);
    m_endpoint_valid = true;
}

EtcdClient::~EtcdClient()
{
    if (m_socket_fd >= 0) {
        (void)::close(m_socket_fd);
        m_socket_fd = -1;
    }
    m_socket_timeout_cached = false;
    m_applied_socket_timeout.reset();
}

EtcdVoidResult EtcdClient::apply_socket_timeout(std::optional<std::chrono::milliseconds> timeout)
{
    if (m_socket_fd < 0) {
        return std::unexpected(EtcdError(EtcdErrorType::NotConnected, "socket not connected"));
    }

    if (m_socket_timeout_cached && m_applied_socket_timeout == timeout) {
        return {};
    }

    timeval tv{};
    if (timeout.has_value() && timeout.value().count() >= 0) {
        const auto total_ms = timeout.value().count();
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(total_ms / 1000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((total_ms % 1000) * 1000);
    }

    if (::setsockopt(m_socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return std::unexpected(make_errno_error(EtcdErrorType::Connection, "setsockopt SO_SNDTIMEO failed", errno));
    }
    if (::setsockopt(m_socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return std::unexpected(make_errno_error(EtcdErrorType::Connection, "setsockopt SO_RCVTIMEO failed", errno));
    }
    m_applied_socket_timeout = timeout;
    m_socket_timeout_cached = true;
    return {};
}

EtcdBoolResult EtcdClient::connect()
{
    reset_last_operation();

    if (m_connected && m_socket_fd >= 0) {
        return true;
    }

    if (!m_endpoint_valid) {
        const std::string message = m_endpoint_error.empty()
            ? "invalid endpoint"
            : m_endpoint_error;
        set_error(EtcdErrorType::InvalidEndpoint, message);
        ETCD_LOG_ERROR("[sync] [connect]", "invalid endpoint endpoint={} error={}",
                       m_config.endpoint,
                       m_last_error.message());
        return std::unexpected(m_last_error);
    }

    ETCD_LOG_INFO("[sync] [connect]", "connecting endpoint={} host={} port={}",
                  m_config.endpoint,
                  m_endpoint_host,
                  m_endpoint_port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const std::string port_string = std::to_string(m_endpoint_port);
    const int gai_rc = ::getaddrinfo(m_endpoint_host.c_str(), port_string.c_str(), &hints, &results);
    if (gai_rc != 0) {
        set_error(EtcdErrorType::Connection, std::string("getaddrinfo failed: ") + gai_strerror(gai_rc));
        ETCD_LOG_ERROR("[sync] [connect]", "getaddrinfo failed host={} port={} error={}",
                       m_endpoint_host,
                       m_endpoint_port,
                       m_last_error.message());
        return std::unexpected(m_last_error);
    }

    std::optional<EtcdError> last_connect_error = std::nullopt;
    for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
        const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            last_connect_error = make_errno_error(EtcdErrorType::Connection, "socket create failed", errno);
            continue;
        }

#ifdef SO_NOSIGPIPE
        {
            int nosigpipe = 1;
            (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
        }
#endif

        if (m_network_config.tcp_no_delay) {
            int nodelay = 1;
            if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
                last_connect_error = make_errno_error(
                    EtcdErrorType::Connection,
                    "setsockopt TCP_NODELAY failed",
                    errno);
                if (::close(fd) != 0) {
                    ETCD_LOG_WARN("[sync] [connect]",
                                  "close after TCP_NODELAY failure failed endpoint={} error={}",
                                  m_config.endpoint,
                                  std::strerror(errno));
                }
                continue;
            }
        }

        if (m_network_config.keepalive) {
            int enable_keepalive = 1;
            if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable_keepalive, sizeof(enable_keepalive)) != 0) {
                last_connect_error = make_errno_error(EtcdErrorType::Connection, "setsockopt SO_KEEPALIVE failed", errno);
                (void)::close(fd);
                continue;
            }
        }

        EtcdVoidResult connect_result{};
        if (m_network_config.is_request_timeout_enabled()) {
            connect_result = connect_with_timeout(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen), m_network_config.request_timeout);
        } else {
            connect_result = connect_with_timeout(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen), std::chrono::milliseconds(-1));
        }

        if (!connect_result.has_value()) {
            last_connect_error = connect_result.error();
            (void)::close(fd);
            continue;
        }

        m_socket_fd = fd;
        m_connected = true;
        m_socket_timeout_cached = false;
        m_applied_socket_timeout.reset();

        auto timeout_result = apply_socket_timeout(
            m_network_config.is_request_timeout_enabled()
                ? std::optional<std::chrono::milliseconds>(m_network_config.request_timeout)
                : std::nullopt);
        if (!timeout_result.has_value()) {
            set_error(timeout_result.error());
            ETCD_LOG_ERROR("[sync] [connect]", "apply socket timeout failed endpoint={} error={}",
                           m_config.endpoint,
                           m_last_error.message());
            (void)::close(m_socket_fd);
            m_socket_fd = -1;
            m_connected = false;
            m_socket_timeout_cached = false;
            m_applied_socket_timeout.reset();
            (void)::freeaddrinfo(results);
            return std::unexpected(m_last_error);
        }

        (void)::freeaddrinfo(results);
        ETCD_LOG_INFO("[sync] [connect]", "connected endpoint={} host={} port={}",
                      m_config.endpoint,
                      m_endpoint_host,
                      m_endpoint_port);
        return true;
    }

    (void)::freeaddrinfo(results);
    if (last_connect_error.has_value()) {
        set_error(last_connect_error.value());
    } else {
        set_error(EtcdErrorType::Connection, "connect failed");
    }
    m_connected = false;
    m_socket_fd = -1;
    m_socket_timeout_cached = false;
    m_applied_socket_timeout.reset();
    ETCD_LOG_ERROR("[sync] [connect]", "connect failed endpoint={} error={}",
                   m_config.endpoint,
                   m_last_error.message());
    return std::unexpected(m_last_error);
}

EtcdBoolResult EtcdClient::close()
{
    reset_last_operation();

    if (m_socket_fd < 0) {
        m_connected = false;
        m_socket_timeout_cached = false;
        m_applied_socket_timeout.reset();
        return true;
    }

    if (::close(m_socket_fd) != 0) {
        const EtcdError error = make_errno_error(EtcdErrorType::Connection, "close failed", errno);
        set_error(error);
        ETCD_LOG_ERROR("[sync] [close]", "close failed endpoint={} error={}",
                       m_config.endpoint,
                       error.message());
        m_socket_fd = -1;
        m_connected = false;
        m_socket_timeout_cached = false;
        m_applied_socket_timeout.reset();
        return std::unexpected(error);
    }

    m_socket_fd = -1;
    m_connected = false;
    m_socket_timeout_cached = false;
    m_applied_socket_timeout.reset();
    ETCD_LOG_INFO("[sync] [close]", "closed endpoint={}", m_config.endpoint);
    return true;
}

std::expected<std::string, EtcdError> EtcdClient::post_json_internal(
    const std::string& api_path,
    std::string body,
    std::optional<std::chrono::milliseconds> force_timeout)
{
    if (!m_connected || m_socket_fd < 0) {
        EtcdError error(EtcdErrorType::NotConnected, "etcd client is not connected");
        set_error(error);
        ETCD_LOG_WARN("[sync] [request]", "request rejected path={} error={}",
                      api_path,
                      error.message());
        return std::unexpected(error);
    }

    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
    if (force_timeout.has_value()) {
        timeout = force_timeout.value();
    } else if (m_network_config.is_request_timeout_enabled()) {
        timeout = m_network_config.request_timeout;
    }

    auto timeout_result = apply_socket_timeout(timeout);
    if (!timeout_result.has_value()) {
        set_error(timeout_result.error());
        ETCD_LOG_ERROR("[sync] [request]", "apply socket timeout failed path={} error={}",
                       api_path,
                       timeout_result.error().message());
        return std::unexpected(timeout_result.error());
    }

    build_json_post_request(
        m_request_buffer,
        m_api_prefix,
        api_path,
        body,
        m_host_header,
        m_network_config.keepalive);

    auto send_result = send_all(m_socket_fd, m_request_buffer);
    if (!send_result.has_value()) {
        set_error(send_result.error());
        ETCD_LOG_ERROR("[sync] [request]", "send failed path={} error={}",
                       api_path,
                       send_result.error().message());
        if (m_socket_fd >= 0) {
            (void)::close(m_socket_fd);
            m_socket_fd = -1;
            m_connected = false;
            m_socket_timeout_cached = false;
            m_applied_socket_timeout.reset();
        }
        return std::unexpected(send_result.error());
    }

    auto response_result = recv_http_response(
        m_socket_fd,
        m_network_config.buffer_size,
        m_response_raw_buffer,
        m_recv_buffer);
    if (!response_result.has_value()) {
        set_error(response_result.error());
        ETCD_LOG_ERROR("[sync] [request]", "recv failed path={} error={}",
                       api_path,
                       response_result.error().message());
        if (m_socket_fd >= 0) {
            (void)::close(m_socket_fd);
            m_socket_fd = -1;
            m_connected = false;
            m_socket_timeout_cached = false;
            m_applied_socket_timeout.reset();
        }
        return std::unexpected(response_result.error());
    }

    if (response_result->connection_close) {
        ETCD_LOG_DEBUG("[sync] [request]", "server closed connection path={} status={}",
                       api_path,
                       response_result->status_code);
        if (m_socket_fd >= 0) {
            (void)::close(m_socket_fd);
            m_socket_fd = -1;
        }
        m_connected = false;
        m_socket_timeout_cached = false;
        m_applied_socket_timeout.reset();
    }

    if (response_result->status_code < 200 || response_result->status_code >= 300) {
        EtcdError error(
            EtcdErrorType::Server,
            "HTTP status=" + std::to_string(response_result->status_code) +
                ", body=" + response_result->body);
        set_error(error);
        ETCD_LOG_WARN("[sync] [request]", "unexpected http status path={} status={} body_size={}",
                      api_path,
                      response_result->status_code,
                      response_result->body.size());
        return std::unexpected(error);
    }

    ETCD_LOG_DEBUG("[sync] [request]", "request completed path={} status={} body_size={}",
                   api_path,
                   response_result->status_code,
                   response_result->body.size());

    return response_result->body;
}

EtcdBoolResult EtcdClient::put(const std::string& key,
                               const std::string& value,
                               std::optional<int64_t> lease_id)
{
    reset_last_operation();
    auto body = build_put_request_body(key, value, lease_id);
    if (!body.has_value()) {
        set_error(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = post_json_internal("/kv/put", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto put_result = parse_put_response(response_body.value());
    if (!put_result.has_value()) {
        set_error(put_result.error());
        return std::unexpected(put_result.error());
    }

    return true;
}

EtcdGetResult EtcdClient::get(const std::string& key,
                              bool prefix,
                              std::optional<int64_t> limit)
{
    reset_last_operation();
    auto body = build_get_request_body(key, prefix, limit);
    if (!body.has_value()) {
        set_error(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = post_json_internal("/kv/range", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto kvs_result = parse_get_response_kvs(response_body.value());
    if (!kvs_result.has_value()) {
        set_error(kvs_result.error());
        return std::unexpected(kvs_result.error());
    }

    return kvs_result.value();
}

EtcdDeleteResult EtcdClient::del(const std::string& key, bool prefix)
{
    reset_last_operation();
    auto body = build_delete_request_body(key, prefix);
    if (!body.has_value()) {
        set_error(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = post_json_internal("/kv/deleterange", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto deleted_result = parse_delete_response_deleted_count(response_body.value());
    if (!deleted_result.has_value()) {
        set_error(deleted_result.error());
        return std::unexpected(deleted_result.error());
    }
    return deleted_result.value();
}

EtcdLeaseGrantResult EtcdClient::grant_lease(int64_t ttl_seconds)
{
    reset_last_operation();
    auto body = build_lease_grant_request_body(ttl_seconds);
    if (!body.has_value()) {
        set_error(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = post_json_internal("/lease/grant", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto lease_result = parse_lease_grant_response_id(response_body.value());
    if (!lease_result.has_value()) {
        set_error(lease_result.error());
        return std::unexpected(lease_result.error());
    }
    return lease_result.value();
}

EtcdLeaseGrantResult EtcdClient::keep_alive_once(int64_t lease_id)
{
    reset_last_operation();
    auto body = build_lease_keep_alive_request_body(lease_id);
    if (!body.has_value()) {
        set_error(body.error());
        return std::unexpected(body.error());
    }

    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
    if (!m_network_config.is_request_timeout_enabled()) {
        timeout = std::chrono::seconds(5);
    }

    auto response_body = post_json_internal("/lease/keepalive", std::move(body.value()), timeout);
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto keepalive_result = parse_lease_keep_alive_response_id(response_body.value(), lease_id);
    if (!keepalive_result.has_value()) {
        set_error(keepalive_result.error());
        return std::unexpected(keepalive_result.error());
    }

    return keepalive_result.value();
}

EtcdPipelineResult EtcdClient::pipeline(std::span<const PipelineOp> operations)
{
    reset_last_operation();
    auto body = build_txn_body(operations);
    if (!body.has_value()) {
        set_error(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = post_json_internal("/kv/txn", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto pipeline_results = parse_pipeline_txn_response(response_body.value(), operations);
    if (!pipeline_results.has_value()) {
        set_error(pipeline_results.error());
        return std::unexpected(pipeline_results.error());
    }

    return pipeline_results.value();
}

EtcdPipelineResult EtcdClient::pipeline(std::vector<PipelineOp> operations)
{
    return pipeline(std::span<const PipelineOp>(operations.data(), operations.size()));
}

bool EtcdClient::connected() const
{
    return m_connected && m_socket_fd >= 0;
}

void EtcdClient::reset_last_operation()
{
    m_last_error = EtcdError(EtcdErrorType::Success);
}

void EtcdClient::set_error(EtcdErrorType type, const std::string& message)
{
    m_last_error = EtcdError(type, message);
}

void EtcdClient::set_error(EtcdError error)
{
    m_last_error = std::move(error);
}

} // namespace galay::etcd
