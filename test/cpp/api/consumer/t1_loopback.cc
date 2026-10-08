#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-http/server/http_server.h>

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {

void require(bool condition, std::string_view message)
{
    if (condition) return;
    const int written = std::fprintf(stderr, "installed.loopback: %.*s errno=%d\n",
        static_cast<int>(message.size()), message.data(), errno);
    std::exit(written < 0 ? 2 : 1);
}

void close_fd(int fd)
{
    require(::close(fd) == 0, "close descriptor");
}

sockaddr_in loopback(std::uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

std::uint16_t free_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "create port probe");
    auto address = loopback(0);
    require(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "probe bind");
    socklen_t size = sizeof(address);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "probe getsockname");
    const auto port = ntohs(address.sin_port);
    close_fd(fd);
    return port;
}

galay::http::HttpResponse get(std::uint16_t port, std::string_view path,
                             galay::http::HttpStatusCode status = galay::http::HttpStatusCode::OK_200)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "create client socket");
    const timeval timeout{5, 0};
    require(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "receive timeout");
    require(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0, "send timeout");
    const auto address = loopback(port);
    require(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "connect loopback");
    const std::string wire = "GET " + std::string(path) +
        " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const auto count = ::send(fd, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "send request");
        offset += static_cast<std::size_t>(count);
    }
    std::string bytes;
    std::array<char, 16384> buffer{};
    for (;;) {
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "receive response");
        if (count == 0) break;
        (void)bytes.append(buffer.data(), static_cast<std::size_t>(count)); // append returns *this.
    }
    close_fd(fd);
    galay::http::HttpResponse response;
    std::vector<iovec> views{{.iov_base = bytes.data(), .iov_len = bytes.size()}};
    const auto [error, consumed] = response.from_io_vec(views);
    require(error == galay::http::kNoError && consumed > 0 && response.is_complete(), "complete HTTP response");
    require(response.header().code() == status, "HTTP status");
    return response;
}

galay::kernel::Task<galay::api::ApiResult<std::string>> value(galay::api::ApiContext&, galay::api::NoInput)
{
    co_return "installed";
}

template<bool EnableSwagger>
void run_loopback()
{
    using namespace galay::api;
    galay::http::HttpServerConfig config;
    config.host = "127.0.0.1";
    config.port = free_port();
    config.io_scheduler_count = 1;
    config.parallel_scheduler_count = 1;
    galay::http::HttpServerBuilder<EnableSwagger> builder(config);
    if constexpr (EnableSwagger) builder.api_info({.title = "Relocated consumer", .version = "1"});
    require(builder.template add_api<galay::http::HttpMethod::GET, NoInput, std::string>(
        "/value", value, Operation{.id = "getValue"}).has_value(), "register typed operation");
    std::string document;
    if constexpr (EnableSwagger) {
        auto exported = builder.export_openapi();
        require(exported.has_value(), "offline document");
        document = std::move(*exported);
    }
    auto built = builder.build();
    require(built.has_value(), "build native HTTP server");
    auto& server = **built;
    require(server.start().has_value(), "start native typed server without deployed files");
    require(get(config.port, "/value").body_str() == "\"installed\"", "typed GET result");
    if constexpr (!EnableSwagger) {
        for (const auto* path : {"/openapi.json", "/docs", "/docs/swagger-ui.css"}) {
            const auto missing = get(config.port, path, galay::http::HttpStatusCode::NotFound_404);
            require(missing.body_str() == "404 Not Found", "disabled mode installs no document resources");
        }
        server.stop();
        require(std::puts("Relocated typed consumer runs without Swagger") >= 0, "write result");
        return;
    }
    require(get(config.port, "/openapi.json").body_str() == document, "immutable REST document");
    require(get(config.port, "/docs").body_str().find("swagger-ui") != std::string::npos, "UI HTML");
    constexpr std::array<std::string_view, 9> names{
        "swagger-ui.css", "swagger-ui-bundle.js", "swagger-ui-standalone-preset.js",
        "favicon-16x16.png", "favicon-32x32.png", "LICENSE", "NOTICE", "README.md", "SHA256SUMS"};
    for (const auto name : names) {
        auto response = get(config.port, "/docs/" + std::string(name));
        require(!response.body_str().empty(), "embedded asset bytes");
        require(response.header().header_pairs().get_value("X-Content-Type-Options") == "nosniff", "asset nosniff");
        if (name.ends_with(".png")) {
            require(response.body_str().starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)) &&
                    response.body_str().find('\0') != std::string::npos, "PNG signature and embedded NUL survive");
        }
        if (name == "README.md") require(response.body_str().find("5.17.14") != std::string::npos, "pinned version");
        if (name == "LICENSE") require(response.body_str().find("Apache License") != std::string::npos, "license retained");
    }
    server.stop();
    require(std::puts("Relocated installed API loopback and nine embedded resources passed without share") >= 0,
            "write result");
}

} // namespace

int main()
{
#ifdef GALAY_CONSUMER_NO_DOCS
    run_loopback<false>();
#else
    run_loopback<true>();
#endif
}
