#include <galay/cpp/galay-etcd/async/client.h>

#include <galay/cpp/galay-etcd/base/etcd_internal.h>
#include <galay/cpp/galay-etcd/base/etcd_log.h>

#include <galay/cpp/galay-http/protoc/http_error.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <concurrentqueue/moodycamel/concurrentqueue.h>

namespace galay::etcd
{

using namespace internal;

namespace
{

constexpr size_t kMaxWatchHeaderBytes = 64 * 1024;

size_t configuredEndpointCount(const EtcdConfig& config)
{
    if (!config.production.endpoints.empty()) {
        return config.production.endpoints.size();
    }
    return config.endpoint.empty() ? 0 : 1;
}

size_t queueCapacityForConfig(const EtcdConfig& config)
{
    const size_t endpoint_count = configuredEndpointCount(config);
    const size_t per_endpoint = config.production.connections_per_endpoint;
    if (endpoint_count == 0 || per_endpoint == 0 ||
        endpoint_count > std::numeric_limits<size_t>::max() / per_endpoint) {
        return 1;
    }
    return endpoint_count * per_endpoint;
}

EtcdError mapHttpError(const galay::http::HttpError& error)
{
    using galay::http::kConnectionClose;
    using galay::http::kRecvTimeOut;
    using galay::http::kRecvError;
    using galay::http::kRequestTimeOut;
    using galay::http::kSendError;
    using galay::http::kSendTimeOut;
    using galay::http::kTcpConnectError;
    using galay::http::kTcpRecvError;
    using galay::http::kTcpSendError;

    switch (error.code()) {
    case kRequestTimeOut:
    case kSendTimeOut:
    case kRecvTimeOut:
        return EtcdError(EtcdErrorType::Timeout, error.message());
    case kTcpConnectError:
        return EtcdError(EtcdErrorType::Connection, error.message());
    case kTcpSendError:
    case kSendError:
        return EtcdError(EtcdErrorType::Send, error.message());
    case kTcpRecvError:
    case kRecvError:
        return EtcdError(EtcdErrorType::Recv, error.message());
    case kConnectionClose:
        return EtcdError(EtcdErrorType::Connection, error.message());
    default:
        return EtcdError(EtcdErrorType::Http, error.message());
    }
}

EtcdError mapKernelIoError(const galay::kernel::IOError& error,
                           EtcdErrorType fallback = EtcdErrorType::Connection)
{
    using galay::kernel::IOError;
    using galay::kernel::kConnectFailed;
    using galay::kernel::kDisconnectError;
    using galay::kernel::kNotRunningOnIOScheduler;
    using galay::kernel::kRecvFailed;
    using galay::kernel::kSendFailed;
    using galay::kernel::kTimeout;

    if (IOError::contains(error.code(), kTimeout)) {
        return EtcdError(EtcdErrorType::Timeout, error.message());
    }
    if (IOError::contains(error.code(), kSendFailed)) {
        return EtcdError(EtcdErrorType::Send, error.message());
    }
    if (IOError::contains(error.code(), kRecvFailed)) {
        return EtcdError(EtcdErrorType::Recv, error.message());
    }
    if (IOError::contains(error.code(), kConnectFailed) ||
        IOError::contains(error.code(), kDisconnectError) ||
        IOError::contains(error.code(), kNotRunningOnIOScheduler)) {
        return EtcdError(EtcdErrorType::Connection, error.message());
    }
    return EtcdError(fallback, error.message());
}

galay::kernel::IOController& invalidController()
{
    static galay::kernel::IOController controller(GHandle::invalid());
    return controller;
}

std::string_view trimLeadingSlash(std::string_view path)
{
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    return path;
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

    const int timeout_ms = timeout.count() > static_cast<long long>(INT_MAX)
        ? INT_MAX
        : static_cast<int>(std::max<long long>(0, timeout.count()));

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

EtcdVoidResult setSocketTimeouts(int fd, std::chrono::milliseconds timeout)
{
    timeval tv{};
    const auto total_ms = timeout.count();
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(total_ms / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((total_ms % 1000) * 1000);

    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "setsockopt SO_SNDTIMEO failed", errno));
    }
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return std::unexpected(makeErrnoError(EtcdErrorType::Connection, "setsockopt SO_RCVTIMEO failed", errno));
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
    const std::string_view status_code_view =
        trimAscii(status_line.substr(first_space + 1, second_space - first_space - 1));
    auto [status_ptr, status_ec] = std::from_chars(
        status_code_view.data(),
        status_code_view.data() + status_code_view.size(),
        status_code);
    if (status_ec != std::errc() || status_ptr != status_code_view.data() + status_code_view.size()) {
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
                auto [len_ptr, len_ec] = std::from_chars(
                    value.data(),
                    value.data() + value.size(),
                    parsed);
                if (len_ec != std::errc() || len_ptr != value.data() + value.size()) {
                    return std::unexpected(EtcdError(EtcdErrorType::Parse, "invalid content-length value"));
                }
                if (parsed > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                    return std::unexpected(EtcdError(EtcdErrorType::Parse, "content-length value too large"));
                }
                headers.content_length = static_cast<size_t>(parsed);
            } else if (equalsAsciiIgnoreCase(key, "transfer-encoding")) {
                if (containsAsciiTokenIgnoreCase(value, "chunked")) {
                    headers.chunked = true;
                }
            } else if (equalsAsciiIgnoreCase(key, "connection")) {
                headers.connection_close = containsAsciiTokenIgnoreCase(value, "close");
            }
        }

        line_pos = line_end + 2;
    }

    return headers;
}

class ChunkStreamDecoder
{
public:
    bool append(std::string_view encoded, std::string& decoded, std::string& error)
    {
        m_buffer.append(encoded.data(), encoded.size());

        while (true) {
            if (m_state == State::ReadSize) {
                const size_t line_end = m_buffer.find("\r\n");
                if (line_end == std::string::npos) {
                    return true;
                }

                std::string_view size_line = trimAscii(std::string_view(m_buffer.data(), line_end));
                const size_t ext_sep = size_line.find(';');
                if (ext_sep != std::string_view::npos) {
                    size_line = trimAscii(size_line.substr(0, ext_sep));
                }
                if (size_line.empty()) {
                    error = "invalid chunk size line";
                    return false;
                }

                uint64_t chunk_size = 0;
                auto [size_ptr, size_ec] = std::from_chars(
                    size_line.data(),
                    size_line.data() + size_line.size(),
                    chunk_size,
                    16);
                if (size_ec != std::errc() || size_ptr != size_line.data() + size_line.size()) {
                    error = "invalid chunk size value";
                    return false;
                }
                if (chunk_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max() - 2)) {
                    error = "chunk size overflows buffer bounds";
                    return false;
                }

                m_buffer.erase(0, line_end + 2);
                m_expected_size = static_cast<size_t>(chunk_size);
                m_state = m_expected_size == 0 ? State::ReadTrailer : State::ReadData;
                continue;
            }

            if (m_state == State::ReadData) {
                if (m_buffer.size() < m_expected_size + 2) {
                    return true;
                }
                decoded.append(m_buffer.data(), m_expected_size);
                if (m_buffer.compare(m_expected_size, 2, "\r\n") != 0) {
                    error = "missing CRLF after chunk data";
                    return false;
                }
                m_buffer.erase(0, m_expected_size + 2);
                m_expected_size = 0;
                m_state = State::ReadSize;
                continue;
            }

            const size_t trailer_end = m_buffer.find("\r\n\r\n");
            if (trailer_end == std::string::npos) {
                return true;
            }
            m_complete = true;
            m_buffer.erase(0, trailer_end + 4);
            return true;
        }
    }

    bool complete() const noexcept
    {
        return m_complete;
    }

private:
    enum class State
    {
        ReadSize,
        ReadData,
        ReadTrailer,
    };

    std::string m_buffer;
    size_t m_expected_size = 0;
    State m_state = State::ReadSize;
    bool m_complete = false;
};

bool dispatchWatchLines(
    std::string& line_buffer,
    const std::function<void(EtcdWatchResponse)>& dispatch,
    EtcdError* error)
{
    while (true) {
        const size_t line_end = line_buffer.find('\n');
        if (line_end == std::string::npos) {
            return true;
        }

        std::string line = line_buffer.substr(0, line_end);
        line_buffer.erase(0, line_end + 1);
        const std::string_view trimmed = trimAscii(line);
        if (trimmed.empty()) {
            continue;
        }

        auto parsed = parseWatchResponse(std::string(trimmed));
        if (!parsed.has_value()) {
            if (error != nullptr) {
                *error = parsed.error();
            }
            return false;
        }
        dispatch(std::move(parsed.value()));
    }
}

} // namespace

namespace details
{

struct AsyncEtcdClientPoolState
{
    AsyncEtcdClientPoolState(
        galay::kernel::IOScheduler* owner_scheduler,
        EtcdConfig config)
        : idle_clients(queueCapacityForConfig(config))
        , scheduler(owner_scheduler)
    {
        std::vector<std::string> endpoints = config.production.endpoints;
        if (endpoints.empty() && !config.endpoint.empty()) {
            endpoints.push_back(config.endpoint);
        }
        if (endpoints.empty()) {
            init_error.emplace(
                EtcdErrorType::InvalidEndpoint,
                "cluster endpoints are empty");
            return;
        }

        const size_t per_endpoint = config.production.connections_per_endpoint;
        if (per_endpoint == 0) {
            init_error.emplace(
                EtcdErrorType::InvalidParam,
                "connections_per_endpoint must be greater than zero");
            return;
        }
        if (endpoints.size() > std::numeric_limits<size_t>::max() / per_endpoint) {
            init_error.emplace(
                EtcdErrorType::InvalidParam,
                "configured async client pool size overflows size_t");
            return;
        }

        const size_t total = endpoints.size() * per_endpoint;
        clients.reserve(total);
        for (const auto& endpoint : endpoints) {
            for (size_t index = 0; index < per_endpoint; ++index) {
                EtcdConfig client_config = config;
                client_config.endpoint = endpoint;
                auto client = std::make_unique<AsyncEtcdClient>(
                    scheduler,
                    std::move(client_config));
                AsyncEtcdClient* const client_ptr = client.get();
                clients.push_back(std::move(client));
                const bool enqueued = idle_clients.enqueue(client_ptr);
                if (!enqueued) {
                    queue_failed.store(true, std::memory_order_release);
                    init_error.emplace(
                        EtcdErrorType::Internal,
                        "failed to initialize async client pool queue");
                    return;
                }
            }
        }
    }

    void release(AsyncEtcdClient* client) noexcept
    {
        if (client == nullptr) {
            return;
        }
        const bool enqueued = idle_clients.enqueue(client);
        if (!enqueued) {
            queue_failed.store(true, std::memory_order_release);
            return;
        }
        borrowed_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    alignas(64) moodycamel::ConcurrentQueue<AsyncEtcdClient*> idle_clients;
    alignas(64) std::atomic<size_t> borrowed_count{0};
    alignas(64) std::atomic<bool> queue_failed{false};
    std::optional<EtcdError> init_error;
    std::vector<std::unique_ptr<AsyncEtcdClient>> clients;
    galay::kernel::IOScheduler* scheduler = nullptr;
};

} // namespace details

AsyncEtcdClientLease::AsyncEtcdClientLease(
    std::shared_ptr<details::AsyncEtcdClientPoolState> state,
    AsyncEtcdClient* client) noexcept
    : m_state(std::move(state))
    , m_client(client)
{
}

AsyncEtcdClientLease::AsyncEtcdClientLease(AsyncEtcdClientLease&& other) noexcept
    : m_state(std::move(other.m_state))
    , m_client(std::exchange(other.m_client, nullptr))
{
}

AsyncEtcdClientLease& AsyncEtcdClientLease::operator=(AsyncEtcdClientLease&& other) noexcept
{
    if (this != &other) {
        release();
        m_state = std::move(other.m_state);
        m_client = std::exchange(other.m_client, nullptr);
    }
    return *this;
}

AsyncEtcdClientLease::~AsyncEtcdClientLease()
{
    release();
}

AsyncEtcdClient* AsyncEtcdClientLease::get() const noexcept
{
    return m_client;
}

AsyncEtcdClient& AsyncEtcdClientLease::operator*() const noexcept
{
    return *m_client;
}

AsyncEtcdClient* AsyncEtcdClientLease::operator->() const noexcept
{
    return m_client;
}

AsyncEtcdClientLease::operator bool() const noexcept
{
    return m_client != nullptr;
}

void AsyncEtcdClientLease::release() noexcept
{
    AsyncEtcdClient* const client = std::exchange(m_client, nullptr);
    auto state = std::move(m_state);
    if (state != nullptr && client != nullptr) {
        state->release(client);
    }
}

AsyncEtcdClusterClient::AsyncEtcdClusterClient(
    galay::kernel::IOScheduler* scheduler,
    EtcdConfig config)
    : m_state(std::make_shared<details::AsyncEtcdClientPoolState>(
          scheduler,
          std::move(config)))
{
}

AsyncEtcdClientAcquireResult AsyncEtcdClusterClient::tryAcquire()
{
    if (m_state == nullptr) {
        return std::unexpected(
            EtcdError(EtcdErrorType::Internal, "async client pool is moved from"));
    }
    if (m_state->init_error.has_value()) {
        return std::unexpected(*m_state->init_error);
    }
    if (m_state->queue_failed.load(std::memory_order_acquire)) {
        return std::unexpected(
            EtcdError(EtcdErrorType::Internal, "async client pool queue failed"));
    }

    AsyncEtcdClient* client = nullptr;
    const bool dequeued = m_state->idle_clients.try_dequeue(client);
    if (!dequeued || client == nullptr) {
        return std::unexpected(
            EtcdError(EtcdErrorType::PoolExhausted, "no idle AsyncEtcdClient"));
    }
    m_state->borrowed_count.fetch_add(1, std::memory_order_acq_rel);
    return AsyncEtcdClientLease(m_state, client);
}

galay::kernel::Task<AsyncEtcdClientAcquireResult>
AsyncEtcdClusterClient::acquireConnected()
{
    auto lease_result = tryAcquire();
    if (!lease_result.has_value()) {
        co_return std::unexpected(lease_result.error());
    }

    auto lease = std::move(*lease_result);
    if (!lease->connected()) {
        auto connected = co_await lease->connect();
        if (!connected.has_value()) {
            co_return std::unexpected(connected.error());
        }
    }
    co_return AsyncEtcdClientAcquireResult(std::move(lease));
}

size_t AsyncEtcdClusterClient::size() const noexcept
{
    return m_state == nullptr ? 0 : m_state->clients.size();
}

size_t AsyncEtcdClusterClient::idleCount() const noexcept
{
    if (m_state == nullptr) {
        return 0;
    }
    const size_t borrowed = m_state->borrowed_count.load(std::memory_order_acquire);
    return borrowed >= m_state->clients.size()
        ? 0
        : m_state->clients.size() - borrowed;
}

galay::kernel::IOScheduler* AsyncEtcdClusterClient::scheduler() const noexcept
{
    return m_state == nullptr ? nullptr : m_state->scheduler;
}

struct AsyncEtcdClient::WatchWorkerState
{
    std::thread thread;
    std::atomic<bool> stop{false};
};

AsyncEtcdClient::AsyncEtcdClient(galay::kernel::IOScheduler* scheduler,
                                 EtcdConfig config)
    : m_scheduler(scheduler)
    , m_config(std::move(config))
    , m_network_config(m_config)
    , m_api_prefix(normalizeApiPrefix(m_config.api_prefix))
{
    auto endpoint_result = parseEndpoint(m_config.endpoint);
    if (!endpoint_result.has_value()) {
        m_endpoint_error = endpoint_result.error();
        ETCD_LOG_WARN("[async] [init]", "invalid endpoint endpoint={} error={}",
                      m_config.endpoint,
                      m_endpoint_error);
        return;
    }

    if (endpoint_result->secure) {
        m_endpoint_error = "https endpoint is not supported in AsyncEtcdClient: " + m_config.endpoint;
        ETCD_LOG_WARN("[async] [init]", "unsupported https endpoint={}", m_config.endpoint);
        return;
    }

    m_ip_type = endpoint_result->ipv6 ? galay::kernel::IPType::IPV6 : galay::kernel::IPType::IPV4;
    m_server_host.emplace(m_ip_type, endpoint_result->host, endpoint_result->port);
    m_host_header = buildHostHeader(endpoint_result->host, endpoint_result->port, endpoint_result->ipv6);
    m_serialized_request_prefix = "POST " + m_api_prefix + "/";
    m_serialized_request_headers =
        " HTTP/1.1\r\n"
        "Host: " + m_host_header + "\r\n"
        "Accept: application/json\r\n"
        "Connection: " + std::string(m_network_config.keepalive ? "keep-alive" : "close") + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: ";
    m_endpoint_valid = true;
}

AsyncEtcdClient::~AsyncEtcdClient()
{
    stopWatchWorkers();
}

#include "../details/awaitable.inl"

AsyncEtcdClient::PostJsonAwaitable AsyncEtcdClient::postJsonInternal(
    const std::string& api_path,
    const std::string& body,
    std::optional<std::chrono::milliseconds> force_timeout)
{
    return PostJsonAwaitable(*this, api_path, body, force_timeout);
}

std::string AsyncEtcdClient::buildSerializedPostRequest(std::string_view api_path,
                                                        std::string_view body) const
{
    const std::string_view normalized_path = trimLeadingSlash(api_path);
    const std::string content_length = std::to_string(body.size());
    std::string request;
    request.reserve(
        m_serialized_request_prefix.size() +
        m_serialized_request_headers.size() +
        normalized_path.size() +
        content_length.size() +
        4 +
        body.size());
    request.append(m_serialized_request_prefix);
    request.append(normalized_path.data(), normalized_path.size());
    request.append(m_serialized_request_headers);
    request.append(content_length);
    request.append("\r\n\r\n");
    request.append(body.data(), body.size());
    return request;
}

AsyncEtcdClient::ConnectAwaitable AsyncEtcdClient::connect()
{
    return ConnectAwaitable(*this);
}

AsyncEtcdClient::CloseAwaitable AsyncEtcdClient::close()
{
    return CloseAwaitable(*this);
}

AsyncEtcdClient::PutAwaitable AsyncEtcdClient::put(const std::string& key,
                                         const std::string& value,
                                         std::optional<int64_t> lease_id)
{
    return PutAwaitable(*this, key, value, lease_id);
}

AsyncEtcdClient::GetAwaitable AsyncEtcdClient::get(const std::string& key,
                                         bool prefix,
                                         std::optional<int64_t> limit)
{
    return GetAwaitable(*this, key, prefix, limit);
}

AsyncEtcdClient::DeleteAwaitable AsyncEtcdClient::del(const std::string& key, bool prefix)
{
    return DeleteAwaitable(*this, key, prefix);
}

AsyncEtcdClient::GrantLeaseAwaitable AsyncEtcdClient::grantLease(int64_t ttl_seconds)
{
    return GrantLeaseAwaitable(*this, ttl_seconds);
}

AsyncEtcdClient::KeepAliveAwaitable AsyncEtcdClient::keepAliveOnce(int64_t lease_id)
{
    return KeepAliveAwaitable(*this, lease_id);
}

AsyncEtcdClient::PipelineAwaitable AsyncEtcdClient::pipeline(std::span<const PipelineOp> operations)
{
    return PipelineAwaitable(*this, operations);
}

AsyncEtcdClient::PipelineAwaitable AsyncEtcdClient::pipeline(std::vector<PipelineOp> operations)
{
    return PipelineAwaitable(*this, std::span<const PipelineOp>(operations.data(), operations.size()));
}

EtcdBoolResult AsyncEtcdClient::watch(const std::string& key, WatchTaskHandler handler)
{
    if (m_scheduler == nullptr) {
        EtcdError error(EtcdErrorType::Internal, "IOScheduler is null");
        setError(error);
        ETCD_LOG_ERROR("[async] [watch]", "scheduler is null key={}", key);
        return std::unexpected(error);
    }

    return startWatchWorker(
        key,
        [scheduler = m_scheduler, handler = std::move(handler)](EtcdWatchResponse response) mutable {
            if (scheduler == nullptr || !handler) {
                return;
            }
            (void)galay::kernel::scheduleTask(scheduler, handler(std::move(response)));
        });
}

EtcdBoolResult AsyncEtcdClient::watch(const std::string& key, WatchFunctionHandler handler)
{
    return startWatchWorker(
        key,
        [handler = std::move(handler)](EtcdWatchResponse response) mutable {
            if (!handler) {
                return;
            }
            handler(std::move(response));
        });
}

bool AsyncEtcdClient::connected() const
{
    return m_connected;
}

EtcdBoolResult AsyncEtcdClient::startWatchWorker(
    const std::string& key,
    std::function<void(EtcdWatchResponse)> dispatch)
{
    resetLastOperation();

    if (!dispatch) {
        EtcdError error(EtcdErrorType::InvalidParam, "watch handler must not be empty");
        setError(error);
        ETCD_LOG_WARN("[async] [watch]", "watch handler is empty key={}", key);
        return std::unexpected(error);
    }

    auto endpoint_result = parseEndpoint(m_config.endpoint);
    if (!endpoint_result.has_value()) {
        EtcdError error(EtcdErrorType::InvalidEndpoint, endpoint_result.error());
        setError(error);
        ETCD_LOG_ERROR("[async] [watch]", "invalid endpoint endpoint={} key={} error={}",
                       m_config.endpoint,
                       key,
                       error.message());
        return std::unexpected(error);
    }
    if (endpoint_result->secure) {
        EtcdError error(
            EtcdErrorType::InvalidEndpoint,
            "https endpoint is not supported in AsyncEtcdClient watch: " + m_config.endpoint);
        setError(error);
        ETCD_LOG_WARN("[async] [watch]", "unsupported https endpoint={} key={}",
                      m_config.endpoint,
                      key);
        return std::unexpected(error);
    }

    auto request_body = buildWatchRequestBody(key);
    if (!request_body.has_value()) {
        setError(request_body.error());
        ETCD_LOG_WARN("[async] [watch]", "build watch request failed key={} error={}",
                      key,
                      request_body.error().message());
        return std::unexpected(request_body.error());
    }

    auto worker = std::make_shared<WatchWorkerState>();
    const std::string host = endpoint_result->host;
    const uint16_t port = endpoint_result->port;
    const std::string request = buildSerializedPostRequest("/watch", request_body.value());
    const auto network_config = m_network_config;
    const std::string watch_key = key;

    worker->thread = std::thread(
        [worker, host, request, network_config, watch_key, dispatch = std::move(dispatch), port]() mutable {
            ETCD_LOG_INFO("[async] [watch]", "worker started key={} host={} port={}",
                          watch_key,
                          host,
                          port);
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;

            addrinfo* results = nullptr;
            const std::string port_string = std::to_string(port);
            const int gai_rc = ::getaddrinfo(host.c_str(), port_string.c_str(), &hints, &results);
            if (gai_rc != 0) {
                ETCD_LOG_ERROR("[async] [watch]", "getaddrinfo failed key={} host={} port={} error={}",
                               watch_key,
                               host,
                               port,
                               gai_strerror(gai_rc));
                return;
            }

            int fd = -1;
            for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
                fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
                if (fd < 0) {
                    continue;
                }

#ifdef SO_NOSIGPIPE
                {
                    int nosigpipe = 1;
                    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
                }
#endif

                if (network_config.tcp_no_delay) {
                    int nodelay = 1;
                    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
                        ETCD_LOG_WARN("[async] [watch]",
                                      "setsockopt TCP_NODELAY failed key={} host={} port={} error={}",
                                      watch_key,
                                      host,
                                      port,
                                      std::strerror(errno));
                        if (::close(fd) != 0) {
                            ETCD_LOG_WARN("[async] [watch]",
                                          "close after TCP_NODELAY failure failed key={} error={}",
                                          watch_key,
                                          std::strerror(errno));
                        }
                        fd = -1;
                        continue;
                    }
                }

                if (network_config.keepalive) {
                    int enable_keepalive = 1;
                    (void)::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable_keepalive, sizeof(enable_keepalive));
                }

                auto connect_result = connectWithTimeout(
                    fd,
                    it->ai_addr,
                    static_cast<socklen_t>(it->ai_addrlen),
                    network_config.isRequestTimeoutEnabled()
                        ? network_config.request_timeout
                        : std::chrono::seconds(5));
                if (connect_result.has_value()) {
                    ETCD_LOG_INFO("[async] [watch]", "worker connected key={} host={} port={}",
                                  watch_key,
                                  host,
                                  port);
                    break;
                }

                ETCD_LOG_WARN("[async] [watch]", "worker connect attempt failed key={} host={} port={} error={}",
                              watch_key,
                              host,
                              port,
                              connect_result.error().message());

                (void)::close(fd);
                fd = -1;
            }

            (void)::freeaddrinfo(results);
            if (fd < 0) {
                ETCD_LOG_ERROR("[async] [watch]", "worker connect failed key={} host={} port={}",
                               watch_key,
                               host,
                               port);
                return;
            }

            const auto io_timeout = network_config.isRequestTimeoutEnabled()
                ? std::min(network_config.request_timeout, std::chrono::milliseconds(1000))
                : std::chrono::milliseconds(1000);
            if (!setSocketTimeouts(fd, io_timeout).has_value()) {
                ETCD_LOG_ERROR("[async] [watch]", "set socket timeouts failed key={}", watch_key);
                (void)::close(fd);
                return;
            }
            if (!sendAll(fd, request).has_value()) {
                ETCD_LOG_ERROR("[async] [watch]", "send watch request failed key={}", watch_key);
                (void)::close(fd);
                return;
            }

            std::string raw_header;
            raw_header.reserve(std::max<size_t>(network_config.buffer_size, 1024) * 2);
            std::vector<char> buffer(std::max<size_t>(network_config.buffer_size, 1024));
            std::optional<ParsedHttpHeaders> headers = std::nullopt;
            std::string line_buffer;
            ChunkStreamDecoder chunked_decoder;
            size_t remaining_content_length = 0;

            auto process_body = [&](std::string_view chunk) -> bool {
                if (!headers.has_value()) {
                    return false;
                }

                if (headers->chunked) {
                    std::string decoded;
                    std::string chunk_error;
                    if (!chunked_decoder.append(chunk, decoded, chunk_error)) {
                        return false;
                    }
                    if (!decoded.empty()) {
                        line_buffer.append(decoded);
                        EtcdError dispatch_error(EtcdErrorType::Success);
                        if (!dispatchWatchLines(line_buffer, dispatch, &dispatch_error)) {
                            return false;
                        }
                    }
                    return !chunked_decoder.complete();
                }

                std::string_view to_append = chunk;
                if (headers->content_length.has_value()) {
                    const size_t take = std::min(remaining_content_length, chunk.size());
                    to_append = chunk.substr(0, take);
                    remaining_content_length -= take;
                }
                if (!to_append.empty()) {
                    line_buffer.append(to_append.data(), to_append.size());
                    EtcdError dispatch_error(EtcdErrorType::Success);
                    if (!dispatchWatchLines(line_buffer, dispatch, &dispatch_error)) {
                        return false;
                    }
                }
                return !headers->content_length.has_value() || remaining_content_length > 0;
            };

            while (!worker->stop.load(std::memory_order_acquire)) {
                const ssize_t recv_bytes = ::recv(fd, buffer.data(), buffer.size(), 0);
                if (recv_bytes > 0) {
                    std::string_view incoming(buffer.data(), static_cast<size_t>(recv_bytes));
                    if (!headers.has_value()) {
                        raw_header.append(incoming.data(), incoming.size());
                        if (raw_header.size() > kMaxWatchHeaderBytes) {
                            ETCD_LOG_WARN("[async] [watch]", "watch response header too large key={} bytes={}",
                                          watch_key,
                                          raw_header.size());
                            break;
                        }
                        const size_t header_end = raw_header.find("\r\n\r\n");
                        if (header_end == std::string::npos) {
                            continue;
                        }

                        auto parsed_headers = parseHttpHeaders(std::string_view(raw_header.data(), header_end));
                        if (!parsed_headers.has_value()) {
                            ETCD_LOG_WARN("[async] [watch]", "parse response header failed key={} error={}",
                                          watch_key,
                                          parsed_headers.error().message());
                            break;
                        }
                        headers = parsed_headers.value();
                        if (headers->status_code < 200 || headers->status_code >= 300) {
                            ETCD_LOG_WARN("[async] [watch]", "unexpected http status key={} status={}",
                                          watch_key,
                                          headers->status_code);
                            break;
                        }
                        if (headers->content_length.has_value()) {
                            remaining_content_length = headers->content_length.value();
                        }

                        const size_t body_offset = header_end + 4;
                        if (raw_header.size() > body_offset) {
                            const std::string_view initial_body(raw_header.data() + body_offset, raw_header.size() - body_offset);
                            if (!process_body(initial_body)) {
                                break;
                            }
                        }
                        raw_header.clear();
                        continue;
                    }

                    if (!process_body(incoming)) {
                        ETCD_LOG_WARN("[async] [watch]", "process watch body stopped key={}", watch_key);
                        break;
                    }
                    continue;
                }

                if (recv_bytes == 0) {
                    ETCD_LOG_INFO("[async] [watch]", "worker peer closed key={}", watch_key);
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (isTimeoutErrno(errno)) {
                    continue;
                }
                ETCD_LOG_ERROR("[async] [watch]", "recv failed key={} error={}",
                               watch_key,
                               std::strerror(errno));
                break;
            }

            if (!line_buffer.empty()) {
                line_buffer.push_back('\n');
                EtcdError dispatch_error(EtcdErrorType::Success);
                (void)dispatchWatchLines(line_buffer, dispatch, &dispatch_error);
            }

            (void)::close(fd);
            ETCD_LOG_INFO("[async] [watch]", "worker stopped key={}", watch_key);
        });

    {
        std::lock_guard<std::mutex> lock(m_watch_mutex);
        m_watch_workers.push_back(worker);
    }

    return true;
}

void AsyncEtcdClient::stopWatchWorkers()
{
    std::vector<std::shared_ptr<WatchWorkerState>> workers;
    {
        std::lock_guard<std::mutex> lock(m_watch_mutex);
        workers = m_watch_workers;
    }

    for (const auto& worker : workers) {
        if (worker != nullptr) {
            worker->stop.store(true, std::memory_order_release);
        }
    }

    joinWatchWorkers();
}

void AsyncEtcdClient::joinWatchWorkers()
{
    std::vector<std::shared_ptr<WatchWorkerState>> workers;
    {
        std::lock_guard<std::mutex> lock(m_watch_mutex);
        workers.swap(m_watch_workers);
    }

    for (auto& worker : workers) {
        if (worker != nullptr && worker->thread.joinable()) {
            worker->thread.join();
        }
    }
}

EtcdBoolResult AsyncEtcdClient::currentBoolResult() const
{
    if (m_last_error.isOk()) {
        return true;
    }
    return std::unexpected(m_last_error);
}

std::expected<std::string, EtcdError> AsyncEtcdClient::resumePostOrCurrent(
    PostJsonAwaitable* post_awaitable)
{
    if (post_awaitable == nullptr) {
        if (m_last_error.isOk()) {
            return std::unexpected(EtcdError(EtcdErrorType::Internal, "post awaitable not started"));
        }
        return std::unexpected(m_last_error);
    }

    auto post_result = post_awaitable->await_resume();
    if (!post_result.has_value()) {
        setError(post_result.error());
        return std::unexpected(post_result.error());
    }

    return post_result.value();
}

void AsyncEtcdClient::resetLastOperation()
{
    m_last_error = EtcdError(EtcdErrorType::Success);
}

void AsyncEtcdClient::setError(EtcdErrorType type, const std::string& message)
{
    m_last_error = EtcdError(type, message);
}

void AsyncEtcdClient::setError(EtcdError error)
{
    m_last_error = std::move(error);
}

} // namespace galay::etcd
