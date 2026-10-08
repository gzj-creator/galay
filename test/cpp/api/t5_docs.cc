#include <galay/cpp/galay-http/server/api_contract.h>
#include <galay/cpp/galay-http/server/http_server.h>
#include <serde/json/json.hpp>

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace galay::api;
using galay::api::router_detail::install_docs;
using galay::api::router_detail::install_docs_from_directory;
using namespace galay::http;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr std::array<std::string_view, 9> asset_names{
    "swagger-ui.css", "swagger-ui-bundle.js", "swagger-ui-standalone-preset.js",
    "favicon-16x16.png", "favicon-32x32.png", "LICENSE", "NOTICE", "README.md",
    "SHA256SUMS"};

constexpr std::array<std::string_view, asset_names.size()> content_types{
    "text/css; charset=utf-8", "application/javascript; charset=utf-8",
    "application/javascript; charset=utf-8", "image/png", "image/png",
    "text/plain; charset=utf-8", "text/plain; charset=utf-8",
    "text/plain; charset=utf-8", "text/plain; charset=utf-8"};

constexpr std::string_view document =
    R"({"openapi":"3.1.0","info":{"title":"Docs test","version":"1"},"paths":{}})";

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "api.docs: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message)
{
    if (!condition) fail(message);
}

void close_fd(int fd)
{
    require(::close(fd) == 0, "close failed: " + std::to_string(errno));
}

std::string read_file(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "cannot open " + path.string());
    std::string result;
    std::array<char, 8192> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
        // append returns the destination reference, not a recoverable result.
        (void)result.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    require(input.eof() && !input.bad(), "cannot read " + path.string());
    input.clear();
    input.close();
    require(!input.fail(), "cannot close " + path.string());
    return result;
}

class Assets {
public:
    Assets()
    {
        std::array<char, 32> name{};
        constexpr std::string_view pattern = "/tmp/galay-api-docs-XXXXXX";
        const auto copied = pattern.copy(name.data(), pattern.size());
        require(copied == pattern.size(), "cannot copy temporary directory template");
        const char* created = ::mkdtemp(name.data());
        require(created != nullptr, "mkdtemp failed");
        path = created;
        for (const auto asset : asset_names) {
            const auto content = read_file(fs::path(GALAY_API_SWAGGER_UI_DIR) / asset);
            write(asset, content);
        }
    }

    ~Assets() { erase(); }

    void erase()
    {
        if (path.empty()) return;
        std::error_code error;
        const auto count = fs::remove_all(path, error);
        require(!error, "remove_all failed: " + error.message());
        require(count != static_cast<std::uintmax_t>(-1), "invalid remove_all result");
        path.clear();
    }

    void remove(std::string_view name)
    {
        std::error_code error;
        const bool removed = fs::remove(path / name, error);
        require(removed && !error, "cannot remove test asset");
    }

    void write(std::string_view name, std::string_view bytes)
    {
        std::ofstream output(path / name, std::ios::binary | std::ios::trunc);
        require(output.is_open(), "cannot create test asset");
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require(output.good(), "cannot write test asset");
        output.close();
        require(!output.fail(), "cannot close test asset");
    }

    fs::path path;
};

router_detail::PreparedRoutes prepared()
{
    router_detail::PreparedRoutes api;
    api.document = std::make_shared<const std::string>(document);
    return api;
}

Task<void> sentinel(HttpConn&, HttpRequest)
{
    co_return;
}

std::string child(std::string_view base, std::string_view name)
{
    return (base == "/" ? "" : std::string(base)) + "/" + std::string(name);
}

std::vector<std::string> docs_paths(const DocsConfig& config)
{
    std::vector<std::string> paths{config.spec_path, config.ui_path};
    if (config.ui_path != "/") paths.push_back(config.ui_path + "/");
    paths.push_back(child(config.ui_path, "swagger-initializer.js"));
    for (const auto name : asset_names) paths.push_back(child(config.ui_path, name));
    return paths;
}

void require_uninstalled(router_detail::PreparedRoutes& api, const DocsConfig& config,
                         std::string_view existing = {})
{
    require(!api.docs_installed, "failed install must not set docs_installed");
    require(*api.document == document, "failed install must not change document");
    for (const auto& path : docs_paths(config)) {
        if (path == existing) continue;
        require(!api.router.find_handler(HttpMethod::GET, path),
                "failed install partially registered " + path);
    }
}

void missing_resources()
{
    auto api = prepared();
    const DocsConfig config;
    auto missing = install_docs_from_directory(api, config,
                                               "/definitely-missing/galay-swagger-ui");
    require(!missing && missing.error().code == ApiErrorCode::kResourceError,
            "missing explicit directory must fail, not fall back to embedded resources");
    require_uninstalled(api, config);

    for (const auto name : asset_names) {
        Assets assets;
        auto candidate = prepared();
        assets.remove(name);
        auto result = install_docs_from_directory(candidate, config, assets.path.string());
        require(!result && result.error().code == ApiErrorCode::kResourceError,
                "every required asset must be loaded before registration");
        require(result.error().message.find(name) != std::string::npos,
                "missing asset error must identify the file");
        require_uninstalled(candidate, config);
        assets.write(name, "restored asset");
        auto retry = install_docs_from_directory(candidate, config, assets.path.string());
        require(retry.has_value(), "failed install must remain retryable: " +
                    (retry ? std::string{} : retry.error().message));
    }

    for (const auto name : asset_names) {
        Assets assets;
        assets.write(name, "");
        auto empty_api = prepared();
        auto empty = install_docs_from_directory(empty_api, config, assets.path.string());
        require(!empty && empty.error().code == ApiErrorCode::kResourceError,
                "every empty explicit asset must fail without fallback");
        require_uninstalled(empty_api, config);
        assets.remove(name);
        std::error_code error;
        const bool created = fs::create_directory(assets.path / name, error);
        require(created && !error, "cannot create directory in place of an asset");
        auto directory_api = prepared();
        auto directory = install_docs_from_directory(directory_api, config, assets.path.string());
        require(!directory && directory.error().code == ApiErrorCode::kResourceError,
                "every non-regular explicit asset must fail without fallback");
        require_uninstalled(directory_api, config);
    }

    for (const auto directory : {std::string{}, std::string("/assets\0hidden", 14)}) {
        auto candidate = prepared();
        auto result = install_docs_from_directory(candidate, config, directory);
        require(!result && result.error().code == ApiErrorCode::kResourceError,
                "empty or NUL-containing asset directory must fail explicitly");
        require_uninstalled(candidate, config);
    }
}

void invalid_paths_and_document()
{
    constexpr std::array<std::string_view, 21> bad_paths{
        "", "relative", "//remote.example/docs", "/docs//nested", "/docs/..",
        "/docs/.", "/docs/../spec", "/docs?url=https://evil.example/spec",
        "/docs#fragment", "/docs%2fother", "/docs%252fother", "/docs/:id", "/docs/*",
        "/docs\\other", "/docs\" onload=\"x", "/docs'", "/docs&other", "/docs/<script>",
        "/docs\nheader", "/docs\rheader", "/docs "};
    for (const auto path : bad_paths) {
        for (const bool spec : {true, false}) {
            auto api = prepared();
            DocsConfig config;
            (spec ? config.spec_path : config.ui_path) = path;
            auto result = install_docs(api, config);
            require(!result && result.error().code == ApiErrorCode::kInvalidPath,
                    "unsafe or unrouteable docs path must fail explicitly");
            require(!api.docs_installed, "invalid path must not install docs");
            require(!api.router.find_handler(HttpMethod::GET, "/openapi.json"),
                    "invalid path must not partially register docs");
        }
    }
    for (const auto path : {std::string("/docs\0hidden", 12), std::string("/bad/\xff", 6),
                            "/" + std::string(2048, 'a'), std::string("/docs/")}) {
        auto api = prepared();
        DocsConfig config{.ui_path = path};
        auto result = install_docs(api, config);
        require(!result && result.error().code == ApiErrorCode::kInvalidPath,
                "NUL, invalid UTF-8, oversized and noncanonical paths must fail");
    }
    for (const bool null_document : {true, false}) {
        auto api = prepared();
        api.document = null_document ? nullptr : std::make_shared<const std::string>();
        DocsConfig config;
        auto result = install_docs(api, config);
        require(!result, "absent document must fail");
        require(!api.docs_installed, "absent document must not install routes");
        require(!api.router.find_handler(HttpMethod::GET, config.ui_path),
                "absent document must not install UI");
    }
    auto long_api = prepared();
    const DocsConfig long_config{.ui_path = "/" + std::string(2030, 'a')};
    auto long_path = install_docs(long_api, long_config);
    require(!long_path && long_path.error().code == ApiErrorCode::kInvalidPath,
            "generated asset path length must be checked, not just the UI base");
    require_uninstalled(long_api, long_config);
}

void preflight_conflicts(const DocsConfig& config)
{
    for (const auto& path : docs_paths(config)) {
        auto api = prepared();
        api.router.add_handler<HttpMethod::GET>(path, sentinel);
        const auto* original = api.router.find_handler(HttpMethod::GET, path).handler;
        auto result = install_docs(api, config);
        require(!result && result.error().code == ApiErrorCode::kRouteConflict,
                "every existing router path must be preflighted");
        require(api.router.find_handler(HttpMethod::GET, path).handler == original,
                "conflicting route must not be replaced");
        require_uninstalled(api, config, path);
    }
    for (const auto& pattern : {child(config.ui_path, ":asset"), child(config.ui_path, "*"),
                                child(config.ui_path, "**")}) {
        auto api = prepared();
        api.router.add_handler<HttpMethod::GET>(pattern, sentinel);
        const auto old_size = api.router.size();
        const auto* spec_handler = api.router.find_handler(HttpMethod::GET, config.spec_path).handler;
        auto result = install_docs(api, config);
        require(!result && result.error().code == ApiErrorCode::kRouteConflict,
                "parameter and wildcard router conflicts must fail");
        require(!api.docs_installed, "fuzzy conflict must not install docs");
        require(api.router.size() == old_size &&
                    api.router.find_handler(HttpMethod::GET, config.spec_path).handler == spec_handler,
                "late fuzzy conflict must preserve existing matches without registering a spec");
    }
    for (const auto& path : docs_paths(config)) {
        auto api = prepared();
        api.endpoints.push_back(EndpointSpec{.method = HttpMethod::POST, .path = path});
        auto result = install_docs(api, config);
        require(!result && result.error().code == ApiErrorCode::kRouteConflict,
                "endpoint metadata must reserve docs paths even without a router handler");
        require_uninstalled(api, config);
    }
    auto dynamic = prepared();
    dynamic.endpoints.push_back(EndpointSpec{.path = child(config.ui_path, ":asset")});
    auto dynamic_result = install_docs(dynamic, config);
    require(!dynamic_result && dynamic_result.error().code == ApiErrorCode::kRouteConflict,
            "parameterized endpoint metadata must be checked");
    require_uninstalled(dynamic, config);

    auto post_router = prepared();
    post_router.router.add_handler<HttpMethod::POST>(child(config.ui_path, "SHA256SUMS"), sentinel);
    auto post_conflict = install_docs(post_router, config);
    require(!post_conflict && post_conflict.error().code == ApiErrorCode::kRouteConflict,
            "docs paths must be checked against existing router methods, not only GET");
    require_uninstalled(post_router, config);

    for (const auto& path : docs_paths(config)) {
        if (path == config.spec_path) continue;
        auto api = prepared();
        auto conflicting = config;
        conflicting.spec_path = path;
        auto result = install_docs(api, conflicting);
        require(!result, "spec must not collide with UI or an asset path");
        require(!api.docs_installed, "self conflict must not install docs");
        require_uninstalled(api, conflicting);
    }

    Assets assets;
    auto api = prepared();
    const auto reserved = child(config.ui_path, "SHA256SUMS");
    api.router.add_handler<HttpMethod::GET>(reserved, sentinel);
    assets.remove(asset_names.front());
    auto failure = install_docs_from_directory(api, config, assets.path.string());
    require(!failure && failure.error().code == ApiErrorCode::kResourceError,
            "load all resources before checking late conflicts");
    require_uninstalled(api, config, reserved);
}

std::uint16_t free_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const auto error = errno;
        close_fd(fd);
        fail("bind failed: " + std::to_string(error));
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        const auto error = errno;
        close_fd(fd);
        fail("getsockname failed: " + std::to_string(error));
    }
    const auto port = ntohs(address.sin_port);
    close_fd(fd);
    return port;
}

HttpResponse request(std::uint16_t port, const std::string& path)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    int fd = -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "client socket failed");
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) break;
        const auto error = errno;
        close_fd(fd);
        fd = -1;
        require(error == ECONNREFUSED || error == EINTR, "connect failed");
        std::this_thread::sleep_for(10ms);
    }
    require(fd >= 0, "server did not accept requests");
    timeval timeout{.tv_sec = 5, .tv_usec = 0};
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        close_fd(fd);
        fail("cannot set socket timeouts");
    }
    const std::string wire = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                             "Connection: close\r\n\r\n";
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const auto size = ::send(fd, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) {
            close_fd(fd);
            fail("send failed");
        }
        offset += static_cast<std::size_t>(size);
    }
    std::string bytes;
    std::array<char, 16384> buffer{};
    while (true) {
        const auto size = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (size < 0 && errno == EINTR) continue;
        if (size < 0) {
            close_fd(fd);
            fail("recv failed");
        }
        if (size == 0) break;
        // append returns the destination reference, not a recoverable result.
        (void)bytes.append(buffer.data(), static_cast<std::size_t>(size));
    }
    close_fd(fd);
    HttpResponse response;
    std::vector<iovec> views{{.iov_base = bytes.data(), .iov_len = bytes.size()}};
    const auto [error, consumed] = response.from_io_vec(views);
    require(error == kNoError && consumed > 0 && response.is_complete(),
            "invalid or truncated HTTP response for " + path);
    require(response.header().code() == HttpStatusCode::OK_200, "docs response must be 200");
    return response;
}

void loopback_and_lifetime(const DocsConfig& config, bool from_directory)
{
    std::unique_ptr<Assets> assets;
    if (from_directory) assets = std::make_unique<Assets>();
    std::array<std::string, asset_names.size()> expected_assets;
    for (std::size_t i = 0; i < asset_names.size(); ++i) {
        expected_assets[i] = read_file(fs::path(GALAY_API_SWAGGER_UI_DIR) / asset_names[i]);
        if (i == 3 || i == 4) {
            require(expected_assets[i].find('\0') != std::string::npos,
                    "PNG fixtures must exercise embedded NUL bytes");
        }
        if (from_directory) {
            expected_assets[i] += "\n/* custom directory resource: " + std::string(asset_names[i]) + " */\n";
            expected_assets[i].push_back('\0');
            assets->write(asset_names[i], expected_assets[i]);
        }
    }
    HttpRouter detached;
    {
        auto api = prepared();
        auto installed = from_directory ? install_docs_from_directory(api, config, assets->path.string())
                                        : install_docs(api, config);
        require(installed.has_value(), installed ? "" : installed.error().message);
        require(api.docs_installed, "success must mark docs installed");
        for (const auto& path : docs_paths(config)) {
            const auto match = api.router.find_handler(HttpMethod::GET, path);
            require(match.request_handler != nullptr, "docs route not registered: " + path);
            detached.add_request_handler<HttpMethod::GET>(path, *match.request_handler);
        }
        auto repeated_config = config;
        repeated_config.ui_path = "/second-docs";
        auto repeated = install_docs(api, repeated_config);
        require(!repeated && repeated.error().code == ApiErrorCode::kRouteConflict,
                "install_docs must reject a second installation even at different paths");
        require(!api.router.find_handler(HttpMethod::GET, "/second-docs"),
                "second install must not mutate router");
        api.router.clear();
        api.document.reset();
    }
    assets.reset();

    const auto port = free_port();
    HttpServer server(HttpServerBuilder<>().host("127.0.0.1").port(port)
                          .io_scheduler_count(1).parallel_scheduler_count(1).build_config());
    server.start(std::move(detached));
    require(server.is_running(), "docs loopback server failed to start");
    auto spec = request(port, config.spec_path);
    require(spec.body_str() == document, "served document must match offline snapshot");
    require(spec.header().header_pairs().get_value("Content-Type") == "application/json; charset=utf-8",
            "spec content type must be JSON");
    auto html = request(port, config.ui_path);
    require(html.header().header_pairs().get_value("Content-Type") == "text/html; charset=utf-8",
            "UI content type must be HTML");
    require(html.body_str().find("https://") == std::string::npos &&
                html.body_str().find("http://") == std::string::npos,
            "HTML must reference only same-origin assets");
    require(html.body_str().find("width=device-width") != std::string::npos,
            "UI must use the mobile viewport");
    for (const auto name : {"swagger-ui.css", "swagger-ui-bundle.js",
                            "swagger-ui-standalone-preset.js", "swagger-initializer.js",
                            "favicon-16x16.png", "favicon-32x32.png"}) {
        require(html.body_str().find(child(config.ui_path, name)) != std::string::npos,
                "HTML asset paths must use the configured UI prefix");
    }
    if (config.ui_path != "/") {
        auto slash = request(port, config.ui_path + "/");
        require(slash.body_str() == html.body_str(), "UI slash alias must serve the same page");
    }
    auto initializer = request(port, child(config.ui_path, "swagger-initializer.js"));
    require(initializer.header().header_pairs().get_value("Content-Type") ==
                "application/javascript; charset=utf-8", "initializer must be JavaScript");
    constexpr std::string_view marker = "const config = ";
    const auto begin = initializer.body_str().find(marker);
    require(begin != std::string::npos, "initializer must contain serialized configuration");
    const auto end = initializer.body_str().find(';', begin);
    require(end != std::string::npos, "initializer configuration must terminate");
    auto parsed = json::parse(std::string_view(initializer.body_str()).substr(
        begin + marker.size(), end - begin - marker.size()));
    require(parsed.has_value(), "initializer config must be valid JSON");
    const auto url = parsed->at("url").as_string();
    require(url && *url == config.spec_path, "initializer URL must equal configured spec path");
    require(parsed->at("validatorUrl").is_null(), "remote validation must be disabled");
    const auto persist = parsed->at("persistAuthorization").as_bool();
    require(persist && !*persist, "authorization must not persist");
    const auto query = parsed->at("queryConfigEnabled").as_bool();
    require(query && !*query, "query strings must not override the offline UI configuration");
    for (std::size_t i = 0; i < asset_names.size(); ++i) {
        auto resource = request(port, child(config.ui_path, asset_names[i]));
        require(resource.body_str() == expected_assets[i],
                from_directory ? "explicit custom resource must survive owners and disk removal, not use embedded bytes"
                               : "embedded browser and metadata bytes must exactly match official fixtures, including NUL");
        require(resource.header().header_pairs().get_value("Content-Type") == content_types[i],
                "every browser and metadata resource must use the expected Content-Type");
        require(resource.header().header_pairs().get_value("X-Content-Type-Options") == "nosniff",
                "asset response must not permit MIME sniffing");
    }
    server.stop();
}

} // namespace

int main()
{
    missing_resources();
    invalid_paths_and_document();
    for (const auto& config : {DocsConfig{},
            DocsConfig{.spec_path = "/v1/schema~private.json", .ui_path = "/help/api-v1"},
            DocsConfig{.spec_path = "/schema.json", .ui_path = "/"}}) {
        preflight_conflicts(config);
        loopback_and_lifetime(config, false);
        loopback_and_lifetime(config, true);
    }
    std::cout << "api.docs PASS\n";
}
