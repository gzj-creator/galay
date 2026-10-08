#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-api/api_server.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <unistd.h>
#if defined(__linux__)
#include <dirent.h>
#endif

namespace {

using namespace galay::api;

void require(bool condition, std::string_view message)
{
    if (condition) return;
    const int written = std::fprintf(stderr, "api.startup: %.*s errno=%d\n",
        static_cast<int>(message.size()), message.data(), errno);
    std::exit(written < 0 ? 2 : 1);
}

class PortReservation {
public:
    explicit PortReservation(std::uint16_t port = 0)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd_ >= 0, "create port reservation");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        require(::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
                "bind private loopback port");
        socklen_t size = sizeof(address);
        require(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size) == 0,
                "read private port");
        this->port = ntohs(address.sin_port);
        require(::listen(fd_, 8) == 0, "reserve listening port without SO_REUSEPORT");
    }

    ~PortReservation() { release(); }
    PortReservation(const PortReservation&) = delete;
    PortReservation& operator=(const PortReservation&) = delete;

    void release()
    {
        if (fd_ >= 0) require(::close(std::exchange(fd_, -1)) == 0, "close port reservation");
    }

    std::uint16_t port{};

private:
    int fd_ = -1;
};

#if defined(__linux__)
std::size_t entry_count(const char* path)
{
    auto* directory = ::opendir(path);
    require(directory != nullptr, "open process resources");
    std::size_t count = 0;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (!entry) { require(errno == 0, "read process resources"); break; }
        if (std::string_view(entry->d_name) != "." && std::string_view(entry->d_name) != "..") ++count;
    }
    require(::closedir(directory) == 0, "close process resources");
    return count;
}

struct ProcessResources {
    std::size_t descriptors;
    std::size_t threads;
    bool operator==(const ProcessResources&) const = default;
};

ProcessResources idle_resources()
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        const ProcessResources resources{entry_count("/proc/self/fd"), entry_count("/proc/self/task")};
        if (resources.threads == 1) return resources;
        require(std::chrono::steady_clock::now() < deadline, "runtime threads must exit during cleanup");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
#endif

galay::kernel::Task<ApiResult<std::string>> get_value(ApiContext&, NoInput)
{
    co_return "value";
}

PreparedApi prepare_api(bool conflict = false)
{
    ApiBuilder builder({.title = "Startup acceptance", .version = "1"});
    const auto added = builder.add<galay::http::HttpMethod::GET, NoInput, std::string>(
        conflict ? "/docs" : "/value", get_value, Operation{.id = "getValue"});
    require(added.has_value(), "add startup fixture");
    auto api = builder.build();
    require(api.has_value(), "build startup fixture");
    return std::move(*api);
}

struct PolicyProbe {
    int calls = 0;
    bool reject_first = false;
};

struct ProbeSwagger {
    std::shared_ptr<PolicyProbe> probe;
    ApiResult<void> install(PreparedApi& api)
    {
        ++probe->calls;
        if (probe->reject_first && probe->calls == 1) {
            return std::unexpected(ApiError{ApiErrorCode::kResourceError, "retryable policy failure", 503});
        }
        return install_docs(api);
    }
};

template<class Config>
void test_policy_retry(Config config)
{
    PortReservation occupied;
    config.port = occupied.port;
#if defined(__linux__)
    const auto baseline = idle_resources();
#endif
    auto probe = std::make_shared<PolicyProbe>(0, true);
    ApiServer<ProbeSwagger> server(config, ProbeSwagger{probe});
    server.stop();
    auto api = prepare_api();
    const auto document = api.document;
    const auto* route = api.router.find_handler(galay::http::HttpMethod::GET, "/value").request_handler;
    const auto rejected = server.start(std::move(api));
    require(!rejected && rejected.error().code == ApiErrorCode::kResourceError && rejected.error().status == 503,
            "policy failure precedes native construction even on an occupied port");
    require(!server.is_running() && !server.document() && probe->calls == 1 &&
            api.router.find_handler(galay::http::HttpMethod::GET, "/value").request_handler == route,
            "policy failure retains API and remains retryable");
#if defined(__linux__)
    require(idle_resources() == baseline, "policy failure starts no runtime and leaks no descriptor");
#endif
    occupied.release();
#if defined(__linux__)
    const auto released = idle_resources();
#endif
    const auto started = server.start(std::move(api));
    require(started && server.is_running() && server.document() == document && probe->calls == 2,
            "same API and server retry installation and start the selected native transport");
    auto unused = prepare_api();
    const auto repeated = server.start(std::move(unused));
    require(!repeated && repeated.error().code == ApiErrorCode::kServerError && probe->calls == 2 &&
            unused.router.find_handler(galay::http::HttpMethod::GET, "/value").request_handler,
            "repeat start neither reinstalls docs nor consumes another API");
    server.stop();
    server.stop();
    require(!server.is_running() && server.document() == document, "stop is idempotent and retains offline document");
    const auto restarted = server.start(std::move(unused));
    require(!restarted && restarted.error().code == ApiErrorCode::kServerError, "stopped API server is single use");
#if defined(__linux__)
    require(idle_resources() == released, "stop releases native runtime and listener before API server destruction");
#endif
    const PortReservation available(config.port);
}

template<class Config>
void test_transport_failure(const Config& config, std::string_view reason)
{
#if defined(__linux__)
    const auto baseline = idle_resources();
#endif
    auto probe = std::make_shared<PolicyProbe>();
    ApiServer<ProbeSwagger> server(config, ProbeSwagger{probe});
    auto api = prepare_api();
    const auto document = api.document;
    const auto failed = server.start(std::move(api));
    require(!failed && failed.error().code == ApiErrorCode::kTransportError &&
            failed.error().message.find(reason) != std::string::npos,
            "native failure must be synchronous and retain its actual cause");
    require(!server.is_running() && server.document() == document && probe->calls == 1,
            "failed native initialization freezes the attempt after one docs installation");
    server.stop();
    server.stop();
    auto unused = prepare_api();
    const auto repeated = server.start(std::move(unused));
    require(!repeated && repeated.error().code == ApiErrorCode::kServerError && probe->calls == 1 &&
            unused.router.find_handler(galay::http::HttpMethod::GET, "/value").request_handler,
            "transport failure cannot be retried on the same API server");
#if defined(__linux__)
    require(idle_resources() == baseline, "failed startup releases native runtime and descriptors immediately");
#endif
}

template<class Config>
void test_docs_conflict(Config config)
{
    PortReservation probe;
    config.port = probe.port;
    probe.release();
#if defined(__linux__)
    const auto baseline = idle_resources();
#endif
    ApiServer<HttpSwagger> server(config);
    auto conflicting = prepare_api(true);
    const auto failed = server.start(std::move(conflicting));
    require(!failed && failed.error().code == ApiErrorCode::kRouteConflict && !server.document(),
            "docs conflict is explicit before native initialization on every transport");
#if defined(__linux__)
    require(idle_resources() == baseline, "docs conflict creates no listener or runtime");
#endif
    auto api = prepare_api();
    const auto started = server.start(std::move(api));
    require(started && server.is_running(), "docs conflict remains retryable with a corrected API");
    server.stop();
}

template<class Config>
void test_lifecycle(Config config)
{
    if constexpr (!std::same_as<Config, galay::http::HttpServerConfig>) {
        auto probe = std::make_shared<PolicyProbe>();
        ApiServer<ProbeSwagger> server(config, ProbeSwagger{probe});
        auto invalid = prepare_api();
        invalid.router.template add_handler<galay::http::HttpMethod::GET>("/connection",
            [](galay::http::HttpConn&, galay::http::HttpRequest) -> galay::kernel::Task<void> { co_return; });
        const auto rejected = server.start(std::move(invalid));
        require(!rejected && rejected.error().code == ApiErrorCode::kInvalidBinding &&
                probe->calls == 0 && !server.document() && !server.is_running() &&
                invalid.router.find_handler(galay::http::HttpMethod::GET, "/connection").handler,
                "connection-bound routes fail before policy, runtime or router consumption");
    }
    test_policy_retry(config);
    test_docs_conflict(config);
    const PortReservation occupied;
    config.port = occupied.port;
    test_transport_failure(config, "listen");
}

#ifdef GALAY_SSL_FEATURE_ENABLED
template<class Config>
void test_tls_failures(Config config, const std::string& directory)
{
    PortReservation probe;
    config.port = probe.port;
    probe.release();
    const auto check = [&](Config invalid, std::string_view reason) {
        test_transport_failure(invalid, reason);
        const PortReservation available(invalid.port);
    };
    auto invalid = config;
    invalid.cert_path.clear();
    check(invalid, "both cert_path and key_path");
    invalid = config;
    invalid.key_path.clear();
    check(invalid, "both cert_path and key_path");
    invalid = config;
    invalid.cert_path = directory + "/missing.crt";
    check(invalid, "TLS certificate");
    invalid = config;
    invalid.cert_path = directory + "/invalid.pem";
    check(invalid, "TLS certificate");
    invalid = config;
    invalid.key_path = directory + "/invalid.pem";
    check(invalid, "TLS private key");
    invalid = config;
    invalid.key_path = directory + "/other.key";
    check(invalid, "TLS private key");
    invalid = config;
    invalid.ca_path = directory + "/missing.crt";
    check(invalid, "TLS CA");
    invalid = config;
    invalid.ca_path = directory + "/invalid.pem";
    check(invalid, "TLS CA");
}
#endif

#ifdef GALAY_API_HTTP2_FEATURE_ENABLED
galay::kernel::Task<void> stream_handler(galay::http2::Http2Stream::ptr)
{
    co_return;
}

template<class Config>
void test_native_handler_conflict(Config config)
{
    config.stream_handler = stream_handler;
    auto probe = std::make_shared<PolicyProbe>();
    ApiServer<ProbeSwagger> server(config, ProbeSwagger{probe});
    auto api = prepare_api();
    const auto rejected = server.start(std::move(api));
    require(!rejected && rejected.error().code == ApiErrorCode::kInvalidBinding &&
            probe->calls == 0 && !server.document() && !server.is_running() &&
            api.router.find_handler(galay::http::HttpMethod::GET, "/value").request_handler,
            "native HTTP/2 handler overrides fail explicitly before API initialization");
}

void test_h2c_bind_failure()
{
    const PortReservation occupied;
    galay::http2::H2cServerConfig config;
    config.host = "127.0.0.1";
    config.port = occupied.port;
    config.io_scheduler_count = 2;
    config.parallel_scheduler_count = 0;
    galay::http2::H2cServer server(config);
    server.start(stream_handler);
    require(!server.is_running(), "h2c bind failure must be reported synchronously by start");
    require(!server.is_ready(), "failed h2c start must never publish readiness");
    require(server.start_error().find("listen") != std::string::npos, "native h2c start cause");
    server.stop();
    server.stop();
}
#endif

} // namespace

int main(int argc, char** argv)
{
#ifdef GALAY_SSL_FEATURE_ENABLED
    require(argc == 2, "startup certificate directory argument");
    const std::string directory = argv[1];
#else
    require(argc >= 1 && argv != nullptr, "startup arguments");
#endif
    test_lifecycle(galay::http::HttpServerBuilder().host("127.0.0.1")
        .io_scheduler_count(2).parallel_scheduler_count(1).build_config());
#ifdef GALAY_SSL_FEATURE_ENABLED
    const auto https = galay::http::HttpsServerBuilder().host("127.0.0.1")
        .cert_path(directory + "/localhost.crt").key_path(directory + "/localhost.key")
        .io_scheduler_count(2).parallel_scheduler_count(1).build_config();
    test_lifecycle(https);
    test_tls_failures(https, directory);
#endif
#ifdef GALAY_API_HTTP2_FEATURE_ENABLED
    test_h2c_bind_failure();
    test_native_handler_conflict(galay::http2::H2cServerConfig{});
    test_lifecycle(galay::http2::H2cServerBuilder().host("127.0.0.1")
        .io_scheduler_count(2).parallel_scheduler_count(1).build_config());
#ifdef GALAY_SSL_FEATURE_ENABLED
    const auto h2 = galay::http2::H2ServerBuilder().host("127.0.0.1")
        .cert_path(directory + "/localhost.crt").key_path(directory + "/localhost.key")
        .io_scheduler_count(2).parallel_scheduler_count(1).build_config();
    test_lifecycle(h2);
    test_native_handler_conflict(h2);
    test_tls_failures(h2, directory);
#endif
#endif
    require(std::puts("HTTP transport startup failure regression passed") >= 0, "write result");
}
