#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-http/server/http_server.h>
#ifdef GALAY_HTTP2_FEATURE_ENABLED
#include <galay/cpp/galay-http2/server/http2_server.h>
#endif

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

template<class Builder>
ApiResult<void> register_value(Builder& builder, bool conflict = false)
{
    return builder.template add_api<galay::http::HttpMethod::GET, NoInput, std::string>(
        conflict ? "/docs" : "/value", get_value, Operation{.id = "getValue"});
}

template<template<bool> class Builder, class Config>
void test_lifecycle(Config config)
{
    PortReservation reservation;
    config.port = reservation.port;
    reservation.release();
#if defined(__linux__)
    const auto baseline = idle_resources();
#endif
    Builder<true> retry(config);
    require(register_value(retry).has_value(), "register startup route");
    retry.docs({}, "/definitely-missing-startup-assets");
    const auto failed_build = retry.build();
    require(!failed_build && failed_build.error().code == ApiErrorCode::kResourceError,
        "resource failure is returned before native construction");
#if defined(__linux__)
    require(idle_resources() == baseline, "failed build allocates no runtime or descriptors");
#endif
    retry.docs();
    auto built = retry.build();
    require(built.has_value(), "same builder retries after fixing resources");
    auto& server = **built;
    require(server.start().has_value() && server.is_running(), "native managed server starts");
    const auto repeated = server.start();
    require(!repeated && repeated.error().code == ApiErrorCode::kServerError, "repeat start is explicit");
    server.stop();
    server.stop();
    require(!server.start(), "stopped managed server cannot restart");
    built->reset();
#if defined(__linux__)
    require(idle_resources() == baseline, "destruction releases native runtime and listeners");
#endif
    Builder<true> conflicting(config);
    require(register_value(conflicting, true).has_value(), "register docs conflict");
    const auto conflict = conflicting.build();
    require(!conflict && conflict.error().code == ApiErrorCode::kRouteConflict, "docs conflict is explicit");
#if defined(__linux__)
    require(idle_resources() == baseline, "docs conflict acquires no native resources");
#endif
    conflicting.docs({.ui_path = "/reference"});
    require(conflicting.build().has_value(), "corrected docs path retries without consuming routes");

    Builder<false> native_builder(config);
    auto native = native_builder.build();
    require(native.has_value(), "build low-level native server");
#ifdef GALAY_HTTP2_FEATURE_ENABLED
    if constexpr (requires { config.stream_handler; }) {
        (*native)->start([](galay::http2::Http2Stream::ptr) -> galay::kernel::Task<void> { co_return; });
    } else
#endif
    {
        galay::http::HttpRouter router;
        router.add_request_handler<galay::http::HttpMethod::GET>("/native",
            [](galay::http::HttpRequest) -> galay::kernel::Task<galay::http::HttpResponseResult> {
                co_return galay::http::HttpResponse{};
            });
        (*native)->start(std::move(router));
    }
    require((*native)->is_running(), "low-level native startup succeeds");
    const auto already_running = (*native)->start();
    require(!already_running && already_running.error().code == ApiErrorCode::kServerError &&
        (*native)->is_running(), "managed startup cannot disturb an already-running native server");
    (*native)->stop();
}

template<template<bool> class Builder, class Config>
void test_transport_failure(const Config& config, std::string_view reason)
{
#if defined(__linux__)
    const auto baseline = idle_resources();
#endif
    Builder<true> builder(config);
    require(register_value(builder).has_value(), "register failing transport route");
    auto built = builder.build();
    require(built.has_value(), "transport initialization is deferred until start");
    const auto failed = (*built)->start();
    require(!failed && failed.error().code == ApiErrorCode::kTransportError &&
        failed.error().message.find(reason) != std::string::npos, "start retains actual transport failure");
    require(!(*built)->is_running() && !(*built)->start(), "failed managed startup is single-use");
    (*built)->stop();
    built->reset();
#if defined(__linux__)
    require(idle_resources() == baseline, "failed transport destruction leaks no resources");
#endif
}

#ifdef GALAY_SSL_FEATURE_ENABLED
template<template<bool> class Builder, class Config>
void test_tls_failures(Config config, const std::string& directory)
{
    PortReservation reservation;
    config.port = reservation.port;
    reservation.release();
    const auto check = [&](Config invalid, std::string_view reason) {
        test_transport_failure<Builder>(invalid, reason);
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

} // namespace

int main(int argc, char** argv)
{
    using namespace galay::http;
    const PortReservation occupied;
    auto http = HttpServerBuilder<>().host("127.0.0.1").port(occupied.port)
        .io_scheduler_count(2).parallel_scheduler_count(1).build_config();
    test_lifecycle<HttpServerBuilder>(http);
    test_transport_failure<HttpServerBuilder>(http, "listen");
#ifdef GALAY_SSL_FEATURE_ENABLED
    require(argc == 2, "certificate directory argument");
    const std::string directory = argv[1];
    auto https = HttpsServerBuilder<>().host("127.0.0.1").port(occupied.port)
        .cert_path(directory + "/localhost.crt").key_path(directory + "/localhost.key")
        .io_scheduler_count(2).parallel_scheduler_count(1).build_config();
    test_lifecycle<HttpsServerBuilder>(https);
    test_transport_failure<HttpsServerBuilder>(https, "listen");
    test_tls_failures<HttpsServerBuilder>(https, directory);
#else
    require(argc >= 1 && argv != nullptr, "startup arguments");
#endif
#ifdef GALAY_HTTP2_FEATURE_ENABLED
    using namespace galay::http2;
    auto h2c = H2cServerBuilder<>().host("127.0.0.1").port(occupied.port)
        .io_scheduler_count(2).parallel_scheduler_count(1).build_config();
    test_lifecycle<H2cServerBuilder>(h2c);
    test_transport_failure<H2cServerBuilder>(h2c, "listen");
#ifdef GALAY_SSL_FEATURE_ENABLED
    auto h2 = H2ServerBuilder<>().host("127.0.0.1").port(occupied.port)
        .cert_path(directory + "/localhost.crt").key_path(directory + "/localhost.key")
        .io_scheduler_count(2).parallel_scheduler_count(1).build_config();
    test_lifecycle<H2ServerBuilder>(h2);
    test_transport_failure<H2ServerBuilder>(h2, "listen");
    test_tls_failures<H2ServerBuilder>(h2, directory);
#endif
#endif
    require(std::puts("Native transport startup regression passed") >= 0, "write result");
}
