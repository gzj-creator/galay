#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-api/api_server.h>
#include <serde/json/json.hpp>
#include <serde/reflect/reflect_macros.hpp>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#if defined(__linux__)
#include <dirent.h>
#endif

namespace fixture {

struct EchoInput { std::string value; };
#define ECHO_FIELDS(X) X(value)
REFLECT_FIELDS(EchoInput, ECHO_FIELDS)
#undef ECHO_FIELDS

struct HandlerLifetime {
    std::atomic<int> completed{0};
    std::string label = "builder-owned";
};

} // namespace fixture

namespace {

using namespace galay::api;
using namespace galay::http;
using galay::kernel::Task;

[[noreturn]] void fail(std::string_view message) {
    std::fprintf(stderr, "api.policy: %.*s (errno=%d)\n",
                 static_cast<int>(message.size()), message.data(), errno);
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

void close_fd(int fd) {
    require(::close(fd) == 0, "close socket");
}

sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

class PortReservation {
public:
    explicit PortReservation(std::uint16_t requested = 0, bool listen = false) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd_ >= 0, "port probe socket");
        const int reuse = 1;
        require(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0,
                "port probe SO_REUSEADDR");
        auto address = loopback(requested);
        require(::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
                "port must remain available before installation or after server destruction");
        socklen_t size = sizeof(address);
        require(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size) == 0,
                "port probe getsockname");
        port = ntohs(address.sin_port);
        if (listen) require(::listen(fd_, 1) == 0, "occupy listening port");
    }
    ~PortReservation() { release(); }
    PortReservation(const PortReservation&) = delete;
    PortReservation& operator=(const PortReservation&) = delete;

    void release() {
        if (fd_ >= 0) close_fd(std::exchange(fd_, -1));
    }

    std::uint16_t port{};

private:
    int fd_ = -1;
};

std::uint16_t free_port() {
    const PortReservation probe;
    return probe.port;
}

HttpServerConfig config(std::uint16_t port) {
    return HttpServerBuilder().host("127.0.0.1").port(port)
        .io_scheduler_count(1).parallel_scheduler_count(1).build_config();
}

#if defined(__linux__)
std::size_t entry_count(const char* path) {
    DIR* directory = ::opendir(path);
    require(directory != nullptr, "open process resource directory");
    std::size_t count = 0;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (entry == nullptr) {
            require(errno == 0, "read process resource directory");
            break;
        }
        if (std::string_view(entry->d_name) != "." && std::string_view(entry->d_name) != "..") ++count;
    }
    require(::closedir(directory) == 0, "close process resource directory");
    return count;
}

struct ProcessResources {
    std::size_t descriptors;
    std::size_t threads;
    bool operator==(const ProcessResources&) const = default;
};

ProcessResources process_resources() {
    return {entry_count("/proc/self/fd"), entry_count("/proc/self/task")};
}

ProcessResources idle_process_resources() {
    // Prior Runtime threads can remain visible in procfs briefly after join.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        const auto resources = process_resources();
        if (resources.threads == 1) return resources;
        require(std::chrono::steady_clock::now() < deadline,
                "prior Runtime threads must exit before the resource baseline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
#endif

int connect_client(std::uint16_t port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "client socket");
        const timeval timeout{5, 0};
        require(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
                "client receive timeout");
        require(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
                "client send timeout");
        const auto address = loopback(port);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) return fd;
        const int error = errno;
        close_fd(fd);
        require(error == ECONNREFUSED || error == ETIMEDOUT, "unexpected connect failure");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    fail("server did not listen");
}

struct Response {
    int status{};
    std::string headers;
    std::string body;
};

Response call(std::uint16_t port, std::string_view method, std::string_view target,
              std::string_view body = {}) {
    const int fd = connect_client(port);
    std::string wire = std::string(method) + " " + std::string(target) +
        " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!body.empty()) {
        wire += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    wire += "\r\n";
    wire += body;
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const auto count = ::send(fd, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "send HTTP request");
        offset += static_cast<std::size_t>(count);
    }
    std::string received;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "receive complete HTTP response and server-owned close");
        if (count == 0) break;
        // append returns the destination reference, not a recoverable result.
        (void)received.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close_fd(fd);
    const auto split = received.find("\r\n\r\n");
    const auto status = received.find(' ');
    require(split != std::string::npos && status != std::string::npos && status + 4 <= split,
            "complete response status and headers");
    Response result;
    const auto parsed = std::from_chars(received.data() + status + 1, received.data() + status + 4, result.status);
    require(parsed.ec == std::errc{} && parsed.ptr == received.data() + status + 4, "HTTP status code");
    result.headers = received.substr(0, split);
    result.body = received.substr(split + 4);
    return result;
}

void expect_string(const Response& response, int status, std::string_view expected) {
    const auto value = json::deserialize<std::string>(response.body);
    require(response.status == status && value && *value == expected, "HTTP JSON string response");
}

void expect_not_found(std::uint16_t port, std::string_view path) {
    const auto response = call(port, "GET", path);
    if (response.status != 404) {
        std::fprintf(stderr, "api.policy: unregistered GET %.*s returned status=%d\nheaders:\n%s\nbody:\n%s\n",
                     static_cast<int>(path.size()), path.data(), response.status,
                     response.headers.c_str(), response.body.c_str());
        fail("unregistered docs path must return HTTP 404; wrong status alone does not prove docs registration");
    }
}

Task<void> suspend_twice() {
    co_yield true;
    co_yield true;
    co_return;
}

Task<ApiResult<std::string>> get(ApiContext&, NoInput) {
    co_return "get-ok";
}

struct BorrowingHandler {
    std::shared_ptr<fixture::HandlerLifetime> lifetime;

    Task<ApiResult<std::string_view>> operator()(ApiContext& context, fixture::EchoInput input) {
        const auto* request = &context.request;
        const std::string before = context.request.body_str();
        const auto suspended = co_await suspend_twice();
        require(suspended.has_value(), "typed handler scheduling result");
        require(&context.request == request && context.request.body_str() == before,
                "ApiContext/request survive nested co_await");
        require(input.value == "live" && lifetime->label == "builder-owned",
                "typed input and handler survive builder/PreparedApi destruction");
        const auto previous = lifetime->completed.fetch_add(1);
        require(previous >= 0, "typed handler completion counter");
        co_return std::string_view(context.request.body_str());
    }
};

PreparedApi prepared(std::shared_ptr<fixture::HandlerLifetime> lifetime = {}) {
    if (!lifetime) lifetime = std::make_shared<fixture::HandlerLifetime>();
    ApiBuilder builder(ApiInfo{.title = "Policy loopback", .version = "1"});
    require(builder.add<HttpMethod::GET, NoInput, std::string>(
        "/value", get, Operation{.id = "getValue"}).has_value(), "register policy GET fixture");
    require(builder.add<HttpMethod::POST, fixture::EchoInput, std::string_view>(
        "/echo", BorrowingHandler{std::move(lifetime)},
        Operation{.id = "echoValue", .success_status = 201}).has_value(), "register policy POST fixture");
    auto api = builder.build();
    require(api.has_value(), "build policy fixture");
    return std::move(*api);
}

struct PolicyTrace {
    std::uint16_t port{};
    std::atomic<int> installs{0};
    std::atomic<int> completed{0};
    std::atomic<int> destroyed{0};
    std::atomic<bool> fail_install{false};
    std::atomic<const void*> installed_address{nullptr};
#if defined(__linux__)
    std::optional<ProcessResources> before_install;
#endif
};

struct PolicyResource {
    std::shared_ptr<PolicyTrace> trace;
    std::string label = "server-owned";

    ~PolicyResource() {
        const PortReservation stopped(trace->port);
        const auto previous = trace->destroyed.fetch_add(1);
        require(previous == 0, "policy resource destroyed exactly once after HTTP shutdown");
    }
};

class MoveOnlyPolicy {
public:
    explicit MoveOnlyPolicy(std::shared_ptr<PolicyTrace> trace)
        : resource_(std::make_unique<PolicyResource>(std::move(trace))) {}
    MoveOnlyPolicy(MoveOnlyPolicy&&) noexcept = default;
    MoveOnlyPolicy& operator=(MoveOnlyPolicy&&) noexcept = default;
    MoveOnlyPolicy(const MoveOnlyPolicy&) = delete;
    MoveOnlyPolicy& operator=(const MoveOnlyPolicy&) = delete;

    ApiResult<void> install(PreparedApi& api) {
#if defined(__linux__)
        if (resource_->trace->before_install) {
            require(process_resources() == *resource_->trace->before_install,
                    "policy must run before Runtime opens descriptors or starts threads");
        }
#endif
        const PortReservation before_listen(resource_->trace->port);
        const auto previous = resource_->trace->installs.fetch_add(1);
        require(previous >= 0 && api.router.find_handler(HttpMethod::GET, "/value").handler != nullptr,
                "policy runs before consuming PreparedApi router");
        if (resource_->trace->fail_install.load()) {
            return std::unexpected(ApiError{ApiErrorCode::kResourceError, "retryable policy failure", 503});
        }
        resource_->trace->installed_address.store(this);
        api.router.add_handler<HttpMethod::GET, HttpMethod::POST>("/policy",
            [this](HttpConn& connection, HttpRequest request) {
                return handle(connection, std::move(request));
            });
        return {};
    }

private:
    Task<ApiResult<std::string>> work(ApiContext& context) {
        const auto* request = &context.request;
        const std::string before = context.request.body_str();
        const auto suspended = co_await suspend_twice();
        require(suspended.has_value(), "policy handler scheduling result");
        require(resource_->trace->installed_address.load() == this && resource_->trace->destroyed.load() == 0,
                "server-owned policy has a stable live address across co_await");
        require(&context.request == request && context.request.body_str() == before,
                "policy ApiContext survives nested co_await");
        co_return resource_->label + ":" + context.request.body_str();
    }

    Task<void> handle(HttpConn& connection, HttpRequest request) {
        ApiContext context{request};
        const auto result = co_await work(context);
        require(result && *result, "policy handler Task and business results");
        const auto encoded = json::serialize(**result);
        require(encoded.has_value(), "encode custom policy response");
        auto response = Http1_1ResponseBuilder().status(200).header("Connection", "close")
            .header("Content-Type", "application/json").body(*encoded).build_move();
        auto writer = connection.get_writer();
        const auto sent = co_await writer.send_response(std::move(response));
        require(sent && *sent, "send custom policy response");
        const auto previous = resource_->trace->completed.fetch_add(1);
        require(previous >= 0, "policy handler completion counter");
        co_return;
    }

    std::unique_ptr<PolicyResource> resource_;
};

struct MissingInstall {};
struct WrongReturn { bool install(PreparedApi&) { return true; } };
struct WrongExpected { ApiResult<int> install(PreparedApi&) { return 0; } };
struct ImmovablePolicy {
    ImmovablePolicy() = default;
    ImmovablePolicy(const ImmovablePolicy&) = delete;
    ImmovablePolicy(ImmovablePolicy&&) = delete;
    ApiResult<void> install(PreparedApi&) { return {}; }
};

template<class Policy>
concept CanUseServerPolicy = requires { typename ApiServer<Policy>; };

template<class Server>
concept HasRunningState = requires(const Server& server) {
    { server.is_running() } noexcept -> std::same_as<bool>;
};

template<class Server>
concept HasLegacyRunningState = requires(const Server& server) {
    server.isRunning();
};

static_assert(HasRunningState<ApiServer<>> && HasRunningState<ApiServer<HttpSwagger>>);
static_assert(!HasLegacyRunningState<ApiServer<>> && !HasLegacyRunningState<ApiServer<HttpSwagger>>);
static_assert(ApiDocsPolicy<NoSwagger> && ApiDocsPolicy<HttpSwagger> && ApiDocsPolicy<MoveOnlyPolicy>);
static_assert(!ApiDocsPolicy<MissingInstall> && !ApiDocsPolicy<WrongReturn> && !ApiDocsPolicy<WrongExpected>);
static_assert(!ApiDocsPolicy<ImmovablePolicy> && !ApiDocsPolicy<NoSwagger&> && !ApiDocsPolicy<void>);
static_assert(CanUseServerPolicy<MoveOnlyPolicy> && !CanUseServerPolicy<MissingInstall> &&
              !CanUseServerPolicy<WrongReturn> && !CanUseServerPolicy<WrongExpected>);
static_assert(!std::copy_constructible<MoveOnlyPolicy> && !std::default_initializable<MoveOnlyPolicy>);
static_assert(std::constructible_from<ApiServer<MoveOnlyPolicy>, HttpServerConfig, MoveOnlyPolicy>);
static_assert(std::same_as<ApiServer<>, ApiServer<NoSwagger>>);
static_assert(!std::is_polymorphic_v<ApiServer<>> && !std::is_polymorphic_v<ApiServer<MoveOnlyPolicy>>);
static_assert(!std::is_copy_constructible_v<ApiServer<>> && !std::is_copy_assignable_v<ApiServer<>>);
static_assert(!std::is_move_constructible_v<ApiServer<>> && !std::is_move_assignable_v<ApiServer<>>);
static_assert(!std::is_polymorphic_v<NoSwagger> && !std::is_polymorphic_v<HttpSwagger>);
static_assert(!std::is_base_of_v<NoSwagger, HttpSwagger> && !std::is_base_of_v<HttpSwagger, NoSwagger>);
static_assert(!std::is_base_of_v<HttpServer, ApiServer<>>);
static_assert(noexcept(std::declval<const NoSwagger&>().install(std::declval<PreparedApi&>())));
static_assert(std::default_initializable<HttpSwagger>);
static_assert(std::constructible_from<ApiServer<HttpSwagger>, HttpServerConfig>);

class CountingSwagger {
public:
    CountingSwagger(DocsConfig docs, std::shared_ptr<PolicyTrace> trace)
        : swagger_(std::move(docs)), trace_(std::move(trace)) {}

    ApiResult<void> install(PreparedApi& api) {
#if defined(__linux__)
        if (trace_->before_install) {
            require(process_resources() == *trace_->before_install,
                    "HttpSwagger must run before Runtime resource acquisition");
        }
#endif
        const PortReservation before_listen(trace_->port);
        const auto previous = trace_->installs.fetch_add(1);
        require(previous >= 0 && api.router.find_handler(HttpMethod::GET, "/value").handler != nullptr,
                "HttpSwagger runs before PreparedApi router consumption");
        return swagger_.install(api);
    }

private:
    HttpSwagger swagger_;
    std::shared_ptr<PolicyTrace> trace_;
};

class InstallCounter {
public:
    explicit InstallCounter(std::shared_ptr<PolicyTrace> trace) : trace_(std::move(trace)) {}

    ApiResult<void> install(PreparedApi&) {
        const auto previous = trace_->installs.fetch_add(1);
        require(previous >= 0, "installation counter");
        return {};
    }

private:
    std::shared_ptr<PolicyTrace> trace_;
};

class DirectoryDocs {
public:
    DirectoryDocs(DocsConfig docs, std::string directory, std::shared_ptr<PolicyTrace> trace)
        : docs_(std::move(docs)), directory_(std::move(directory)), trace_(std::move(trace)) {}

    ApiResult<void> install(PreparedApi& api) {
#if defined(__linux__)
        if (trace_->before_install) {
            require(process_resources() == *trace_->before_install,
                    "explicit file strategy must run before Runtime resource acquisition");
        }
#endif
        const PortReservation before_listen(trace_->port);
        const auto previous = trace_->installs.fetch_add(1);
        require(previous >= 0 && api.router.find_handler(HttpMethod::GET, "/value").handler != nullptr,
                "explicit file strategy runs before PreparedApi router consumption");
        return install_docs_from_directory(api, docs_, directory_);
    }

private:
    DocsConfig docs_;
    std::string directory_;
    std::shared_ptr<PolicyTrace> trace_;
};

static_assert(ApiDocsPolicy<CountingSwagger> && ApiDocsPolicy<InstallCounter> && ApiDocsPolicy<DirectoryDocs>);

class TemporaryAssets {
public:
    TemporaryAssets() {
        std::array<char, 40> name{};
        constexpr std::string_view pattern = "/tmp/galay-api-policy-XXXXXX";
        require(pattern.copy(name.data(), pattern.size()) == pattern.size(), "temporary asset path");
        const auto* created = ::mkdtemp(name.data());
        require(created != nullptr, "create temporary policy assets");
        path = created;
    }
    ~TemporaryAssets() { erase(); }
    TemporaryAssets(const TemporaryAssets&) = delete;
    TemporaryAssets& operator=(const TemporaryAssets&) = delete;

    void populate() {
        for (const auto name : {"swagger-ui.css", "swagger-ui-bundle.js", "swagger-ui-standalone-preset.js",
                                "favicon-16x16.png", "favicon-32x32.png", "LICENSE", "NOTICE",
                                "README.md", "SHA256SUMS"}) {
            std::error_code error;
            const bool copied = std::filesystem::copy_file(
                std::filesystem::path(GALAY_API_SWAGGER_UI_DIR) / name, path / name, error);
            require(copied && !error, "populate actual Swagger UI assets after failed install");
        }
    }

    void erase() {
        if (path.empty()) return;
        std::error_code error;
        const auto removed = std::filesystem::remove_all(path, error);
        require(!error && removed != static_cast<std::uintmax_t>(-1), "remove temporary policy assets");
        path.clear();
    }

    std::filesystem::path path;
};

void expect_docs(std::uint16_t port, const std::shared_ptr<const std::string>& document,
                 std::string_view spec_path = "/openapi.json", std::string_view ui_path = "/docs") {
    const auto spec = call(port, "GET", spec_path);
    require(spec.status == 200 && spec.body == *document, "served OpenAPI is original immutable document");
    const auto ui = call(port, "GET", ui_path);
    require(ui.status == 200 && ui.body.find("swagger-ui") != std::string::npos,
            "HttpSwagger exposes actual Swagger UI HTML");
    const auto initializer = call(port, "GET", std::string(ui_path) + "/swagger-initializer.js");
    require(initializer.status == 200 && initializer.body.find(spec_path) != std::string::npos,
            "Swagger initializer uses configured spec path");
    const auto css = call(port, "GET", std::string(ui_path) + "/swagger-ui.css");
    require(css.status == 200 && !css.body.empty(), "local Swagger UI CSS route");
    for (const auto name : {"LICENSE", "NOTICE", "README.md", "SHA256SUMS"}) {
        const auto metadata = call(port, "GET", std::string(ui_path) + "/" + name);
        require(metadata.status == 200 && !metadata.body.empty(), "UI license, source and verification metadata routes");
    }
}

void invalid_prepared_has_no_side_effects() {
    const auto port = free_port();
    auto trace = std::make_shared<PolicyTrace>();
#if defined(__linux__)
    const auto before = idle_process_resources();
#endif
    ApiServer<InstallCounter> server(config(port), InstallCounter(trace));
    auto api = prepared();
    const auto document = api.document;
    const auto* route = api.router.find_handler(HttpMethod::GET, "/value").handler;
    for (const auto invalid : {std::shared_ptr<const std::string>{}, std::make_shared<const std::string>()}) {
        api.document = invalid;
        const auto result = server.start(std::move(api));
        require(!result && !result.error().message.empty(), "invalid PreparedApi is explicitly rejected");
        require(!server.is_running() && !server.document() && trace->installs.load() == 0,
                "invalid PreparedApi rejected before policy invocation or HTTP startup");
        require(api.document == invalid && api.router.find_handler(HttpMethod::GET, "/value").handler == route,
                "invalid PreparedApi rejection preserves caller state");
#if defined(__linux__)
        require(process_resources() == before, "invalid PreparedApi has no fd/thread side effects");
#endif
        const PortReservation available(port);
    }
    api.document = document;
    require(server.start(std::move(api)).has_value(), "invalid input does not freeze server startup");
    expect_string(call(port, "GET", "/value"), 200, "get-ok");
    server.stop();
}

void directory_policy_failure_retry() {
    const auto port = free_port();
    TemporaryAssets assets;
    const DocsConfig docs{.spec_path = "/reference.json", .ui_path = "/reference"};
    auto trace = std::make_shared<PolicyTrace>();
    trace->port = port;
#if defined(__linux__)
    trace->before_install = idle_process_resources();
#endif
    ApiServer<DirectoryDocs> server(config(port), DirectoryDocs(docs, assets.path.string(), trace));
    std::shared_ptr<const std::string> document;
    {
        auto api = prepared();
        document = api.document;
        const auto* route = api.router.find_handler(HttpMethod::GET, "/value").handler;
        const auto failed = server.start(std::move(api));
        require(!failed && failed.error().code == ApiErrorCode::kResourceError &&
                failed.error().message.find("swagger-ui.css") != std::string::npos,
                "explicit file strategy forwards asset failure rather than falling back to embedded UI");
        require(!server.is_running() && !server.document() && trace->installs.load() == 1,
                "failed explicit file install does not initialize server");
        require(!api.docs_installed && api.document == document &&
                api.router.find_handler(HttpMethod::GET, "/value").handler == route,
                "failed explicit file install preserves PreparedApi router/document");
        for (const auto path : {"/reference.json", "/reference", "/reference/", "/reference/swagger-initializer.js",
                                "/reference/swagger-ui.css", "/reference/swagger-ui-bundle.js",
                                "/reference/swagger-ui-standalone-preset.js", "/reference/favicon-16x16.png",
                                "/reference/favicon-32x32.png", "/reference/LICENSE", "/reference/NOTICE",
                                "/reference/README.md", "/reference/SHA256SUMS"}) {
            require(api.router.find_handler(HttpMethod::GET, path).handler == nullptr,
                    "failed explicit file install leaves no partial docs routes");
        }
#if defined(__linux__)
        require(process_resources() == *trace->before_install, "failed explicit file install opens no lasting runtime resources");
#endif
        assets.populate();
        require(server.start(std::move(api)).has_value(), "explicit resource repair retries same server and PreparedApi");
    }
    assets.erase();
    expect_docs(port, document, docs.spec_path, docs.ui_path);
    expect_string(call(port, "GET", "/value"), 200, "get-ok");
    expect_string(call(port, "POST", "/echo", R"({"value":"live"})"), 201, R"({"value":"live"})");
    expect_not_found(port, "/docs");
    expect_not_found(port, "/openapi.json");
    require(trace->installs.load() == 2, "explicit file strategy installs only once per start attempt, never per docs request");
    auto unused = prepared();
    const auto repeated = server.start(std::move(unused));
    require(!repeated && repeated.error().code == ApiErrorCode::kServerError && trace->installs.load() == 2,
            "repeated explicit file strategy start fails before reinstall");
    server.stop();
    require(server.document() == document, "explicit file strategy document persists after stop");
}

void swagger_delegation() {
    const auto direct_port = free_port();
    ApiServer<HttpSwagger> direct(config(direct_port));
    std::shared_ptr<const std::string> document;
    {
        auto api = prepared();
        document = api.document;
        require(direct.start(std::move(api)).has_value(), "default-constructed HttpSwagger policy starts without a resource directory");
    }
    expect_docs(direct_port, document);
    expect_string(call(direct_port, "GET", "/value"), 200, "get-ok");
    expect_string(call(direct_port, "POST", "/echo", R"({"value":"live"})"), 201, R"({"value":"live"})");
    direct.stop();

    const auto port = free_port();
    const DocsConfig docs{.spec_path = "/reference.json", .ui_path = "/reference"};
    auto trace = std::make_shared<PolicyTrace>();
    trace->port = port;
#if defined(__linux__)
    trace->before_install = idle_process_resources();
#endif
    ApiServer<CountingSwagger> server(config(port), CountingSwagger(docs, trace));
#if defined(__linux__)
    require(process_resources() == *trace->before_install,
            "embedded policy server construction has no fd/thread side effects");
#endif
    {
        auto api = prepared();
        document = api.document;
        require(server.start(std::move(api)).has_value(), "counting embedded Swagger installs with custom paths");
    }
    expect_docs(port, document, docs.spec_path, docs.ui_path);
    expect_docs(port, document, docs.spec_path, docs.ui_path);
    expect_not_found(port, "/docs");
    expect_not_found(port, "/openapi.json");
    require(trace->installs.load() == 1, "embedded policy installs once, never during repeated docs requests");
    auto unused = prepared();
    const auto repeated = server.start(std::move(unused));
    require(!repeated && repeated.error().code == ApiErrorCode::kServerError && trace->installs.load() == 1,
            "repeated embedded Swagger start fails before reinstall");
    server.stop();
    require(server.document() == document, "embedded Swagger document persists after stop");
}

void listener_failure_is_single_use() {
    PortReservation occupied(0, true);
    auto trace = std::make_shared<PolicyTrace>();
    ApiServer<InstallCounter> server(config(occupied.port), InstallCounter(trace));
    auto api = prepared();
    const auto document = api.document;
    const auto failed = server.start(std::move(api));
    require(!failed && failed.error().code == ApiErrorCode::kTransportError && !failed.error().message.empty(),
            "occupied loopback port returns explicit transport_error");
    require(!server.is_running() && trace->installs.load() == 1 && server.document() == document,
            "listen failure occurs after one successful install and retains document");
    require(api.router.find_handler(HttpMethod::GET, "/value").handler == nullptr,
            "HTTP initialization consumes router even when listen fails");
    occupied.release();
    auto unused = prepared();
    const auto* route = unused.router.find_handler(HttpMethod::GET, "/value").handler;
    const auto repeated = server.start(std::move(unused));
    require(!repeated && repeated.error().code == ApiErrorCode::kServerError && trace->installs.load() == 1,
            "listen failure freezes server even after port becomes available");
    server.stop();
    const auto restarted = server.start(std::move(unused));
    require(!restarted && restarted.error().code == ApiErrorCode::kServerError &&
            unused.router.find_handler(HttpMethod::GET, "/value").handler == route,
            "stop after failed listen cannot restart or consume another router");
    ApiServer<> replacement(config(occupied.port));
    require(replacement.start(std::move(unused)).has_value(), "fresh server and API can use released port");
    expect_string(call(occupied.port, "GET", "/value"), 200, "get-ok");
    replacement.stop();
}

void default_no_docs() {
    const auto port = free_port();
    ApiServer<> server(config(port));
    require(!server.is_running() && !server.document(), "default server construction has no running/document state");
    std::shared_ptr<const std::string> document;
    {
        auto api = prepared();
        document = api.document;
        require(server.start(std::move(api)).has_value(), "start default NoSwagger server");
    }
    require(server.is_running() && server.document() == document, "default server retains original document");
    expect_string(call(port, "GET", "/value"), 200, "get-ok");
    expect_string(call(port, "POST", "/echo", R"({"value":"live"})"), 201, R"({"value":"live"})");
    for (const auto path : {"/docs", "/openapi.json", "/docs/swagger-ui.css"}) {
        expect_not_found(port, path);
    }
    server.stop();
    server.stop();
    require(!server.is_running() && server.document() == document, "stop is idempotent and keeps offline document");
}

void move_only_failure_retry_and_lifetime() {
    const auto port = free_port();
    auto trace = std::make_shared<PolicyTrace>();
    trace->port = port;
    trace->fail_install = true;
    auto lifetime = std::make_shared<fixture::HandlerLifetime>();
    std::weak_ptr<fixture::HandlerLifetime> weak = lifetime;
#if defined(__linux__)
    trace->before_install = idle_process_resources();
#endif
    {
        ApiServer<MoveOnlyPolicy> server(config(port), MoveOnlyPolicy(trace));
#if defined(__linux__)
        require(process_resources() == *trace->before_install, "ApiServer construction has no runtime resource side effects");
#endif
        std::shared_ptr<const std::string> document;
        {
            auto api = prepared(lifetime);
            lifetime.reset();
            document = api.document;
            const auto* handler = api.router.find_handler(HttpMethod::GET, "/value").handler;
            const auto failed = server.start(std::move(api));
            require(!failed && failed.error().code == ApiErrorCode::kResourceError &&
                    failed.error().message == "retryable policy failure" && failed.error().status == 503,
                    "failed policy error must propagate unchanged");
            require(!server.is_running() && !server.document() && trace->installs.load() == 1,
                    "failed policy must not publish/start HTTP state");
            require(api.document == document && api.router.find_handler(HttpMethod::GET, "/value").handler == handler &&
                    api.router.find_handler(HttpMethod::GET, "/policy").handler == nullptr,
                    "failed install must not consume or mutate the caller router");
#if defined(__linux__)
            require(process_resources() == *trace->before_install, "failed custom install has no fd/thread side effects");
#endif
            trace->fail_install = false;
            require(server.start(std::move(api)).has_value(), "same server/PreparedApi can retry failed installation");
        }
        require(!weak.expired() && server.document() == document, "routes/document survive builder/PreparedApi destruction");
        expect_string(call(port, "GET", "/value"), 200, "get-ok");
        expect_string(call(port, "POST", "/echo", R"({"value":"live"})"), 201, R"({"value":"live"})");
        expect_string(call(port, "GET", "/policy"), 200, "server-owned:");
        expect_string(call(port, "POST", "/policy", R"({"value":"live"})"), 200, "server-owned:{\"value\":\"live\"}");
        require(trace->installs.load() == 2 && trace->completed.load() == 2 && trace->destroyed.load() == 0,
                "custom install runs once per attempt, never during requests");
        auto unused = prepared();
        const auto* untouched = unused.router.find_handler(HttpMethod::GET, "/value").handler;
        const auto repeated = server.start(std::move(unused));
        require(!repeated && repeated.error().code == ApiErrorCode::kServerError && server.is_running(),
                "start while running must fail with server_error");
        require(trace->installs.load() == 2 && unused.router.find_handler(HttpMethod::GET, "/value").handler == untouched,
                "repeated start must not reinstall or consume another router");
        server.stop();
        const auto restarted = server.start(std::move(unused));
        require(!restarted && restarted.error().code == ApiErrorCode::kServerError && !server.is_running(),
                "start after stop must fail with server_error");
        require(trace->installs.load() == 2 && server.document() == document && trace->destroyed.load() == 0,
                "stopped server retains document/policy without reinstalling");
    }
    require(trace->destroyed.load() == 1 && weak.expired(), "server destruction releases policy and typed handlers");

    auto single = std::make_shared<PolicyTrace>();
    single->port = port;
    {
        ApiServer<MoveOnlyPolicy> server(config(port), MoveOnlyPolicy(single));
        {
            auto api = prepared();
            require(server.start(std::move(api)).has_value(), "successful custom policy starts once");
        }
        expect_string(call(port, "GET", "/policy"), 200, "server-owned:");
        require(single->installs.load() == 1 && single->completed.load() == 1,
                "successful custom policy is not reinstalled during requests");
        // Deliberately leave HTTP running: PolicyResource checks that HTTP stops first.
    }
    require(single->destroyed.load() == 1, "implicit server destruction stops HTTP before policy destruction");
}

} // namespace

int main() {
    move_only_failure_retry_and_lifetime();
    invalid_prepared_has_no_side_effects();
    default_no_docs();
    directory_policy_failure_retry();
    swagger_delegation();
    listener_failure_is_single_use();
    std::puts("HTTP/1 server policy loopback PASS");
}
