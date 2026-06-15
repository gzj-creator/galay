#include "galay-etcd/sync/etcd_client.h"

#include "galay-etcd/base/etcd_internal.h"
#include "galay-etcd/base/etcd_log.h"

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

std::string_view trimAscii(std::string_view value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

char toLowerAscii(char ch)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

bool equalsAsciiIgnoreCase(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (toLowerAscii(lhs[i]) != toLowerAscii(rhs[i])) {
            return false;
        }
    }
    return true;
}

bool containsAsciiIgnoreCase(std::string_view value, std::string_view needle)
{
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > value.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= value.size(); ++i) {
        if (equalsAsciiIgnoreCase(value.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

void appendUnsignedDecimal(std::string& out, size_t value)
{
    char digits[32];
    auto [ptr, ec] = std::to_chars(digits, digits + sizeof(digits), value);
    if (ec == std::errc()) {
        out.append(digits, static_cast<size_t>(ptr - digits));
        return;
    }
    out += std::to_string(value);
}

void buildJsonPostRequest(
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
    appendUnsignedDecimal(request, body.size());
    request += "\r\n\r\n";
    request += body;
}

bool isTimeoutErrno(int error_number)
{
    return error_number == EAGAIN || error_number == EWOULDBLOCK || error_number == ETIMEDOUT;
}

EtcdError makeErrnoError(EtcdErrorType type, const std::string& action, int error_number)
{
    return EtcdError(
        type,
        action + ": " + std::string(std::strerror(error_number)));
}

bool setSocketBlocking(int fd, bool blocking)
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

EtcdVoidResult connectWithTimeout(
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
        if (isTimeoutErrno(error_number)) {
            return std::unexpected(makeErrnoError(EtcdErrorType::Timeout, "connect timeout", error_number));
        }
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "connect failed", error_number));
    }

    if (!setSocketBlocking(fd, false)) {
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "set nonblocking for connect failed", errno));
    }

    if (::connect(fd, address, address_len) == 0) {
        if (!setSocketBlocking(fd, true)) {
            return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "restore blocking mode failed", errno));
        }
        return {};
    }

    if (errno != EINPROGRESS) {
        const int error_number = errno;
        (void)setSocketBlocking(fd, true);
        if (isTimeoutErrno(error_number)) {
            return std::unexpected(makeErrnoError(EtcdErrorType::Timeout, "connect timeout", error_number));
        }
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "connect failed", error_number));
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
        (void)setSocketBlocking(fd, true);
        return std::unexpected(EtcdError(EtcdErrorType::Timeout, "connect timeout"));
    }
    if (poll_result < 0) {
        const int error_number = errno;
        (void)setSocketBlocking(fd, true);
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "poll connect failed", error_number));
    }

    int socket_error = 0;
    socklen_t socket_error_len = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) != 0) {
        const int error_number = errno;
        (void)setSocketBlocking(fd, true);
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "getsockopt connect failed", error_number));
    }

    if (!setSocketBlocking(fd, true)) {
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "restore blocking mode failed", errno));
    }

    if (socket_error != 0) {
        if (isTimeoutErrno(socket_error)) {
            return std::unexpected(makeErrnoError(EtcdErrorType::Timeout, "connect timeout", socket_error));
        }
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "connect failed", socket_error));
    }

    return {};
}

EtcdVoidResult sendAll(int fd, std::string_view payload)
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
        if (isTimeoutErrno(errno)) {
            return std::unexpected(makeErrnoError(EtcdErrorType::Timeout, "send timeout", errno));
        }
        return std::unexpected(makeErrnoError(EtcdErrorType::Send, "send failed", errno));
    }
    return {};
}

struct ParsedHttpHeaders
{
    int status_code = 0;
    std::optional<size_t> content_length = std::nullopt;
    bool chunked = false;
    bool connection_close = false;
};

std::expected<ParsedHttpHeaders, EtcdError> parseHttpHeaders(std::string_view header_block)
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
    const std::string_view status_code_view = trimAscii(status_line.substr(first_space + 1, second_space - first_space - 1));
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
            const std::string_view key = trimAscii(line.substr(0, colon));
            const std::string_view value = trimAscii(line.substr(colon + 1));

            if (equalsAsciiIgnoreCase(key, "content-length")) {
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
            } else if (equalsAsciiIgnoreCase(key, "transfer-encoding")) {
                if (containsAsciiIgnoreCase(value, "chunked")) {
                    headers.chunked = true;
                }
            } else if (equalsAsciiIgnoreCase(key, "connection")) {
                if (containsAsciiIgnoreCase(value, "close")) {
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
    ChunkDecodeState state = ChunkDecodeState::Incomplete;
    std::string body;
    size_t consumed = 0;
    std::string error;
};

ChunkDecodeResult decodeChunkedBody(std::string_view raw)
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

        std::string_view size_line = trimAscii(raw.substr(pos, line_end - pos));
        const size_t ext_sep = size_line.find(';');
        if (ext_sep != std::string_view::npos) {
            size_line = trimAscii(size_line.substr(0, ext_sep));
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

        if (chunk_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            return make_error("chunk size too large");
        }
        const size_t body_size = static_cast<size_t>(chunk_size);
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
    int status_code = 0;
    std::string body;
    bool connection_close = false;
};

std::expected<HttpResponseData, EtcdError> recvHttpResponse(
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
                auto chunked = decodeChunkedBody(std::string_view(raw.data() + body_offset, raw.size() - body_offset));
                if (chunked.state == ChunkDecodeState::Complete) {
                    return HttpResponseData{
                        headers->status_code,
                        std::move(chunked.body),
                        headers->connection_close || peer_closed,
                    };
                }
                if (chunked.state == ChunkDecodeState::Error) {
                    return std::unexpected(EtcdError(EtcdErrorType::Parse, chunked.error));
                }
            } else if (headers->content_length.has_value()) {
                const size_t total_needed = body_offset + headers->content_length.value();
                if (raw.size() >= total_needed) {
                    return HttpResponseData{
                        headers->status_code,
                        raw.substr(body_offset, headers->content_length.value()),
                        headers->connection_close || peer_closed,
                    };
                }
            } else if (peer_closed) {
                return HttpResponseData{
                    headers->status_code,
                    raw.substr(body_offset),
                    true,
                };
            }
        }

        const ssize_t recv_bytes = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (recv_bytes > 0) {
            raw.append(buffer.data(), static_cast<size_t>(recv_bytes));
            if (!headers.has_value()) {
                header_end = raw.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    auto parsed = parseHttpHeaders(std::string_view(raw.data(), header_end));
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
                auto chunked = decodeChunkedBody(std::string_view(raw.data() + body_offset, raw.size() - body_offset));
                if (chunked.state == ChunkDecodeState::Complete) {
                    return HttpResponseData{
                        headers->status_code,
                        std::move(chunked.body),
                        true,
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
                    headers->status_code,
                    raw.substr(body_offset, headers->content_length.value()),
                    true,
                };
            }

            return HttpResponseData{
                headers->status_code,
                raw.substr(body_offset),
                true,
            };
        }

        if (errno == EINTR) {
            continue;
        }
        if (isTimeoutErrno(errno)) {
            return std::unexpected(makeErrnoError(EtcdErrorType::Timeout, "recv timeout", errno));
        }
        return std::unexpected(makeErrnoError(EtcdErrorType::Recv, "recv failed", errno));
    }
}

} // namespace

EtcdClient::EtcdClient(EtcdConfig config)
    : m_config(std::move(config))
    , m_network_config(m_config)
    , m_api_prefix(normalizeApiPrefix(m_config.api_prefix))
{
    m_request_buffer.reserve(512);
    m_response_raw_buffer.reserve(std::max<size_t>(m_network_config.buffer_size, 1024) * 2);
    m_recv_buffer.resize(std::max<size_t>(m_network_config.buffer_size, 1024));

    auto endpoint_result = parseEndpoint(m_config.endpoint);
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
    m_host_header = buildHostHeader(endpoint_result->host, endpoint_result->port, endpoint_result->ipv6);
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

EtcdVoidResult EtcdClient::applySocketTimeout(std::optional<std::chrono::milliseconds> timeout)
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
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "setsockopt SO_SNDTIMEO failed", errno));
    }
    if (::setsockopt(m_socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "setsockopt SO_RCVTIMEO failed", errno));
    }
    m_applied_socket_timeout = timeout;
    m_socket_timeout_cached = true;
    return {};
}

EtcdBoolResult EtcdClient::connect()
{
    resetLastOperation();

    if (m_connected && m_socket_fd >= 0) {
        return true;
    }

    if (!m_endpoint_valid) {
        const std::string message = m_endpoint_error.empty()
            ? "invalid endpoint"
            : m_endpoint_error;
        setError(EtcdErrorType::InvalidEndpoint, message);
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
        setError(EtcdErrorType::Connection, std::string("getaddrinfo failed: ") + gai_strerror(gai_rc));
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
            last_connect_error = makeErrnoError(EtcdErrorType::Connection, "socket create failed", errno);
            continue;
        }

#ifdef SO_NOSIGPIPE
        {
            int nosigpipe = 1;
            (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
        }
#endif

        if (m_network_config.keepalive) {
            int enable_keepalive = 1;
            if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable_keepalive, sizeof(enable_keepalive)) != 0) {
                last_connect_error = makeErrnoError(EtcdErrorType::Connection, "setsockopt SO_KEEPALIVE failed", errno);
                (void)::close(fd);
                continue;
            }
        }

        EtcdVoidResult connect_result{};
        if (m_network_config.isRequestTimeoutEnabled()) {
            connect_result = connectWithTimeout(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen), m_network_config.request_timeout);
        } else {
            connect_result = connectWithTimeout(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen), std::chrono::milliseconds(-1));
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

        auto timeout_result = applySocketTimeout(
            m_network_config.isRequestTimeoutEnabled()
                ? std::optional<std::chrono::milliseconds>(m_network_config.request_timeout)
                : std::nullopt);
        if (!timeout_result.has_value()) {
            setError(timeout_result.error());
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
        setError(last_connect_error.value());
    } else {
        setError(EtcdErrorType::Connection, "connect failed");
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
    resetLastOperation();

    if (m_socket_fd < 0) {
        m_connected = false;
        m_socket_timeout_cached = false;
        m_applied_socket_timeout.reset();
        return true;
    }

    if (::close(m_socket_fd) != 0) {
        const EtcdError error = makeErrnoError(EtcdErrorType::Connection, "close failed", errno);
        setError(error);
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

std::expected<std::string, EtcdError> EtcdClient::postJsonInternal(
    const std::string& api_path,
    std::string body,
    std::optional<std::chrono::milliseconds> force_timeout)
{
    if (!m_connected || m_socket_fd < 0) {
        EtcdError error(EtcdErrorType::NotConnected, "etcd client is not connected");
        setError(error);
        ETCD_LOG_WARN("[sync] [request]", "request rejected path={} error={}",
                      api_path,
                      error.message());
        return std::unexpected(error);
    }

    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
    if (force_timeout.has_value()) {
        timeout = force_timeout.value();
    } else if (m_network_config.isRequestTimeoutEnabled()) {
        timeout = m_network_config.request_timeout;
    }

    auto timeout_result = applySocketTimeout(timeout);
    if (!timeout_result.has_value()) {
        setError(timeout_result.error());
        ETCD_LOG_ERROR("[sync] [request]", "apply socket timeout failed path={} error={}",
                       api_path,
                       timeout_result.error().message());
        return std::unexpected(timeout_result.error());
    }

    buildJsonPostRequest(
        m_request_buffer,
        m_api_prefix,
        api_path,
        body,
        m_host_header,
        m_network_config.keepalive);

    auto send_result = sendAll(m_socket_fd, m_request_buffer);
    if (!send_result.has_value()) {
        setError(send_result.error());
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

    auto response_result = recvHttpResponse(
        m_socket_fd,
        m_network_config.buffer_size,
        m_response_raw_buffer,
        m_recv_buffer);
    if (!response_result.has_value()) {
        setError(response_result.error());
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
        setError(error);
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
    resetLastOperation();
    auto body = buildPutRequestBody(key, value, lease_id);
    if (!body.has_value()) {
        setError(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = postJsonInternal("/kv/put", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto put_result = parsePutResponse(response_body.value());
    if (!put_result.has_value()) {
        setError(put_result.error());
        return std::unexpected(put_result.error());
    }

    return true;
}

EtcdGetResult EtcdClient::get(const std::string& key,
                              bool prefix,
                              std::optional<int64_t> limit)
{
    resetLastOperation();
    auto body = buildGetRequestBody(key, prefix, limit);
    if (!body.has_value()) {
        setError(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = postJsonInternal("/kv/range", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto kvs_result = parseGetResponseKvs(response_body.value());
    if (!kvs_result.has_value()) {
        setError(kvs_result.error());
        return std::unexpected(kvs_result.error());
    }

    return kvs_result.value();
}

EtcdDeleteResult EtcdClient::del(const std::string& key, bool prefix)
{
    resetLastOperation();
    auto body = buildDeleteRequestBody(key, prefix);
    if (!body.has_value()) {
        setError(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = postJsonInternal("/kv/deleterange", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto deleted_result = parseDeleteResponseDeletedCount(response_body.value());
    if (!deleted_result.has_value()) {
        setError(deleted_result.error());
        return std::unexpected(deleted_result.error());
    }
    return deleted_result.value();
}

EtcdLeaseGrantResult EtcdClient::grantLease(int64_t ttl_seconds)
{
    resetLastOperation();
    auto body = buildLeaseGrantRequestBody(ttl_seconds);
    if (!body.has_value()) {
        setError(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = postJsonInternal("/lease/grant", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto lease_result = parseLeaseGrantResponseId(response_body.value());
    if (!lease_result.has_value()) {
        setError(lease_result.error());
        return std::unexpected(lease_result.error());
    }
    return lease_result.value();
}

EtcdLeaseGrantResult EtcdClient::keepAliveOnce(int64_t lease_id)
{
    resetLastOperation();
    auto body = buildLeaseKeepAliveRequestBody(lease_id);
    if (!body.has_value()) {
        setError(body.error());
        return std::unexpected(body.error());
    }

    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
    if (!m_network_config.isRequestTimeoutEnabled()) {
        timeout = std::chrono::seconds(5);
    }

    auto response_body = postJsonInternal("/lease/keepalive", std::move(body.value()), timeout);
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto keepalive_result = parseLeaseKeepAliveResponseId(response_body.value(), lease_id);
    if (!keepalive_result.has_value()) {
        setError(keepalive_result.error());
        return std::unexpected(keepalive_result.error());
    }

    return keepalive_result.value();
}

EtcdPipelineResult EtcdClient::pipeline(std::span<const PipelineOp> operations)
{
    resetLastOperation();
    auto body = buildTxnBody(operations);
    if (!body.has_value()) {
        setError(body.error());
        return std::unexpected(body.error());
    }

    auto response_body = postJsonInternal("/kv/txn", std::move(body.value()));
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto pipeline_results = parsePipelineTxnResponse(response_body.value(), operations);
    if (!pipeline_results.has_value()) {
        setError(pipeline_results.error());
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

void EtcdClient::resetLastOperation()
{
    m_last_error = EtcdError(EtcdErrorType::Success);
}

void EtcdClient::setError(EtcdErrorType type, const std::string& message)
{
    m_last_error = EtcdError(type, message);
}

void EtcdClient::setError(EtcdError error)
{
    m_last_error = std::move(error);
}

} // namespace galay::etcd
