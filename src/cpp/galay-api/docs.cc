#include "docs.h"
#include "ui_assets.h"
#include <galay/cpp/galay-http/protoc/http_response.h>
#include <serde/json/json.hpp>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <unistd.h>

namespace galay::api {
namespace {

struct UiConfiguration {
    std::string url;
    std::string dom_id = "#swagger-ui";
    bool deepLinking = true;
    std::nullptr_t validatorUrl = nullptr;
    bool persistAuthorization = false;
    bool queryConfigEnabled = false;
    std::string layout = "StandaloneLayout";
};

constexpr auto reflect_fields(std::type_identity<UiConfiguration>)
{
    return std::make_tuple(
        reflect::make_field("url", &UiConfiguration::url),
        reflect::make_field("dom_id", &UiConfiguration::dom_id),
        reflect::make_field("deepLinking", &UiConfiguration::deepLinking),
        reflect::make_field("validatorUrl", &UiConfiguration::validatorUrl),
        reflect::make_field("persistAuthorization", &UiConfiguration::persistAuthorization),
        reflect::make_field("queryConfigEnabled", &UiConfiguration::queryConfigEnabled),
        reflect::make_field("layout", &UiConfiguration::layout));
}

constexpr auto reflect_fields(const UiConfiguration&)
{
    return reflect_fields(std::type_identity<UiConfiguration>{});
}

struct Resource {
    std::shared_ptr<const std::string> bytes;
    std::string content_type;
};

struct DocumentRoute {
    std::string path;
    std::shared_ptr<const Resource> resource;
};

ApiError system_error(std::string_view action, const std::string& path, int error)
{
    return {ApiErrorCode::kResourceError,
            std::string(action) + " " + path + ": " +
                std::error_code(error, std::generic_category()).message(), 500};
}

ApiResult<std::shared_ptr<const std::string>> close_after_failure(int fd, ApiError error)
{
    if (::close(fd) != 0) {
        const int close_error = errno;
        error.message += "; close also failed: " +
            std::error_code(close_error, std::generic_category()).message();
    }
    return std::unexpected(std::move(error));
}

ApiResult<std::shared_ptr<const std::string>> load_asset(const std::string& directory,
                                                       std::string_view name)
{
    const std::string path = directory + "/" + std::string(name);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return std::unexpected(system_error("open", path, errno));
    struct stat information{};
    if (::fstat(fd, &information) != 0) {
        return close_after_failure(fd, system_error("fstat", path, errno));
    }
    constexpr std::size_t maximum_asset_size = 16 * 1024 * 1024;
    if (!S_ISREG(information.st_mode) || information.st_size <= 0 ||
        static_cast<std::uintmax_t>(information.st_size) > maximum_asset_size) {
        return close_after_failure(fd, {ApiErrorCode::kResourceError,
            "asset must be a nonempty regular file of at most 16 MiB: " + path, 500});
    }
    std::string bytes(static_cast<std::size_t>(information.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return close_after_failure(fd, system_error("read", path, errno));
        if (count == 0) {
            return close_after_failure(fd, {ApiErrorCode::kResourceError,
                "asset changed or was truncated while loading: " + path, 500});
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(fd) != 0) return std::unexpected(system_error("close", path, errno));
    return std::make_shared<const std::string>(std::move(bytes));
}

ApiResult<void> validate_document_path(std::string_view path)
{
    if (path.empty() || path.front() != '/' || path.size() > 2048 ||
        (path.size() > 1 && path.back() == '/')) {
        return std::unexpected(ApiError{ApiErrorCode::kInvalidPath,
            "docs path must be absolute, canonical and at most 2048 bytes", 400});
    }
    // The HTTP router accepts only ASCII unreserved literal segments. Reject
    // rather than silently encode a URL that it cannot route to the same bytes.
    for (const unsigned char character : path) {
        if (character != '/' && character != '-' && character != '_' &&
            character != '.' && character != '~' &&
            !(character >= 'a' && character <= 'z') &&
            !(character >= 'A' && character <= 'Z') &&
            !(character >= '0' && character <= '9')) {
            return std::unexpected(ApiError{ApiErrorCode::kInvalidPath,
                "docs path contains a nonliteral or unsafe character", 400});
        }
    }
    if (path == "/") return {};
    std::size_t offset = 1;
    while (offset < path.size()) {
        const auto end = path.find('/', offset);
        const auto segment = path.substr(offset, end == path.npos ? end : end - offset);
        if (segment.empty() || segment == "." || segment == "..") {
            return std::unexpected(ApiError{ApiErrorCode::kInvalidPath,
                "docs path contains an empty or dot segment", 400});
        }
        if (end == path.npos) break;
        offset = end + 1;
    }
    return {};
}

std::string resource_path(std::string_view base, std::string_view name)
{
    return (base == "/" ? "" : std::string(base)) + "/" + std::string(name);
}

ApiResult<std::string> make_initializer(std::string_view spec_path)
{
    const UiConfiguration configuration{.url = std::string(spec_path)};
    auto serialized = json::serialize(configuration);
    if (!serialized) {
        return std::unexpected(ApiError{ApiErrorCode::kEncodingError,
            "Swagger UI initialization configuration: " + serialized.error(), 500});
    }
    return "window.onload = function() {\n  const config = " + *serialized +
        ";\n  config.presets = [SwaggerUIBundle.presets.apis, SwaggerUIStandalonePreset];\n"
        "  config.plugins = [SwaggerUIBundle.plugins.DownloadUrl];\n"
        "  window.ui = SwaggerUIBundle(config);\n};\n";
}

std::string make_html(std::string_view ui_path)
{
    // Only validated ASCII literal paths enter HTML attributes; the router
    // cannot serve quotes, entities or percent-encoded alternatives safely.
    return "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>Swagger UI</title>\n<link rel=\"stylesheet\" href=\"" +
        resource_path(ui_path, "swagger-ui.css") + "\">\n"
        "<link rel=\"icon\" type=\"image/png\" sizes=\"32x32\" href=\"" +
        resource_path(ui_path, "favicon-32x32.png") + "\">\n"
        "<link rel=\"icon\" type=\"image/png\" sizes=\"16x16\" href=\"" +
        resource_path(ui_path, "favicon-16x16.png") + "\">\n"
        "<style>html{box-sizing:border-box;overflow-y:scroll}"
        "*,*:before,*:after{box-sizing:inherit}body{margin:0;background:#fafafa}</style>\n"
        "</head>\n<body>\n<div id=\"swagger-ui\"></div>\n<script src=\"" +
        resource_path(ui_path, "swagger-ui-bundle.js") + "\"></script>\n<script src=\"" +
        resource_path(ui_path, "swagger-ui-standalone-preset.js") + "\"></script>\n<script src=\"" +
        resource_path(ui_path, "swagger-initializer.js") + "\"></script>\n</body>\n</html>\n";
}

bool endpoint_matches(std::string_view pattern, std::string_view path)
{
    if (pattern == path) return true;
    while (!pattern.empty() && !path.empty()) {
        if (pattern.front() != '/' || path.front() != '/') return false;
        pattern.remove_prefix(1);
        path.remove_prefix(1);
        const auto pattern_end = pattern.find('/');
        const auto path_end = path.find('/');
        const auto segment = pattern.substr(0, pattern_end);
        const auto literal = path.substr(0, path_end);
        if (segment != literal && (segment.empty() || segment.front() != ':' || literal.empty())) {
            return false;
        }
        pattern = pattern_end == pattern.npos ? std::string_view{} : pattern.substr(pattern_end);
        path = path_end == path.npos ? std::string_view{} : path.substr(path_end);
    }
    return pattern.empty() && path.empty();
}

kernel::Task<void> send_resource(http::HttpConn& connection, http::HttpRequest request,
                         std::shared_ptr<const Resource> resource)
{
    http::HttpResponse response;
    response.header().version() = http::HttpVersion::HttpVersion_1_1;
    response.header().code() = http::HttpStatusCode::OK_200;
    const bool keep_alive = request.header().is_keep_alive() && !request.header().is_connection_close();
    auto& headers = response.header().header_pairs();
    const std::array<std::pair<std::string, std::string>, 4> fields{{
        {"Content-Type", resource->content_type},
        {"X-Content-Type-Options", "nosniff"},
        {"Cache-Control", "no-store"},
        {"Connection", keep_alive ? "keep-alive" : "close"}}};
    for (const auto& [name, value] : fields) {
        const auto added = headers.add_header_pair(name, value);
        if (added != http::kNoError) {
            HTTP_LOG_ERROR("[api-docs] [header-fail]", "path={} header={} code={}",
                           request.header().uri(), name, static_cast<int>(added));
            const auto closed = co_await connection.close();
            if (!closed) {
                HTTP_LOG_ERROR("[api-docs] [close-fail]", "path={} error={}",
                               request.header().uri(), closed.error().message());
            }
            co_return;
        }
    }
    response.set_body_str(std::string(*resource->bytes));
    auto writer = connection.get_writer();
    const auto sent = co_await writer.send_response(std::move(response));
    if (!sent || !*sent) {
        HTTP_LOG_ERROR("[api-docs] [send-fail]", "path={} error={}",
                       request.header().uri(), sent ? "HTTP writer returned false" : sent.error().message());
        const auto closed = co_await connection.close();
        if (!closed) {
            HTTP_LOG_ERROR("[api-docs] [close-fail]", "path={} error={}",
                           request.header().uri(), closed.error().message());
        }
    }
    // Only a failed response terminates early; normal closes remain server-owned.
    co_return;
}

ApiResult<void> install_resources(PreparedApi& api, const DocsConfig& config,
                                  const std::string* directory)
{
    if (api.docs_installed) {
        return std::unexpected(ApiError{ApiErrorCode::kRouteConflict,
            "docs are already installed on this PreparedApi", 409});
    }
    if (!api.document || api.document->empty()) {
        return std::unexpected(ApiError{ApiErrorCode::kResourceError,
            "PreparedApi must own a nonempty OpenAPI document", 500});
    }
    if (directory && (directory->empty() || directory->find('\0') != std::string::npos)) {
        return std::unexpected(ApiError{ApiErrorCode::kResourceError,
            "Swagger UI directory must be a nonempty filesystem path without NUL", 500});
    }
    const auto assets = docs_detail::embedded_assets();
    std::vector<std::shared_ptr<const std::string>> loaded;
    loaded.reserve(assets.size());
    for (const auto& asset : assets) {
        if (directory) {
            auto bytes = load_asset(*directory, asset.name);
            if (!bytes) return std::unexpected(std::move(bytes.error()));
            loaded.push_back(std::move(*bytes));
        } else {
            loaded.push_back(std::make_shared<const std::string>(asset.bytes));
        }
    }
    auto initializer = make_initializer(config.spec_path);
    if (!initializer) return std::unexpected(std::move(initializer.error()));
    for (const auto& path : {config.spec_path, config.ui_path}) {
        auto checked = validate_document_path(path);
        if (!checked) return checked;
    }
    std::vector<DocumentRoute> routes;
    routes.push_back({config.spec_path,
        std::make_shared<const Resource>(api.document, "application/json; charset=utf-8")});
    const auto html = std::make_shared<const Resource>(
        std::make_shared<const std::string>(make_html(config.ui_path)), "text/html; charset=utf-8");
    routes.push_back({config.ui_path, html});
    if (config.ui_path != "/") routes.push_back({config.ui_path + "/", html});
    routes.push_back({resource_path(config.ui_path, "swagger-initializer.js"),
        std::make_shared<const Resource>(std::make_shared<const std::string>(std::move(*initializer)),
                                        "application/javascript; charset=utf-8")});
    for (std::size_t index = 0; index < assets.size(); ++index) {
        routes.push_back({resource_path(config.ui_path, assets[index].name),
            std::make_shared<const Resource>(std::move(loaded[index]), std::string(assets[index].content_type))});
    }
    for (std::size_t index = 0; index < routes.size(); ++index) {
        const auto& path = routes[index].path;
        if (path.size() > 2048) {
            return std::unexpected(ApiError{ApiErrorCode::kInvalidPath,
                "generated docs resource path exceeds 2048 bytes: " + path, 400});
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (routes[earlier].path == path) {
                return std::unexpected(ApiError{ApiErrorCode::kRouteConflict,
                    "docs paths overlap: " + path, 409});
            }
        }
        for (const auto& endpoint : api.endpoints) {
            if (endpoint_matches(endpoint.path, path)) {
                return std::unexpected(ApiError{ApiErrorCode::kRouteConflict,
                    "docs path conflicts with an endpoint: " + path, 409});
            }
        }
        constexpr std::array methods{
            http::HttpMethod::GET, http::HttpMethod::POST, http::HttpMethod::HEAD,
            http::HttpMethod::PUT, http::HttpMethod::DELETE, http::HttpMethod::TRACE,
            http::HttpMethod::OPTIONS, http::HttpMethod::CONNECT, http::HttpMethod::PATCH,
            http::HttpMethod::PRI, http::HttpMethod::UNKNOWN};
        for (const auto method : methods) {
            if (api.router.find_handler(method, path).handler != nullptr) {
                return std::unexpected(ApiError{ApiErrorCode::kRouteConflict,
                    "docs path conflicts with an existing router handler: " + path, 409});
            }
        }
    }
    for (const auto& route : routes) {
        api.router.add_handler<http::HttpMethod::GET>(route.path,
            [resource = route.resource](http::HttpConn& connection, http::HttpRequest request) {
                return send_resource(connection, std::move(request), resource);
            });
    }
    api.docs_installed = true;
    return {};
}

} // namespace

ApiResult<void> install_docs(PreparedApi& api, const DocsConfig& config)
{
    return install_resources(api, config, nullptr);
}

ApiResult<void> install_docs_from_directory(PreparedApi& api, const DocsConfig& config,
                                          const std::string& directory)
{
    return install_resources(api, config, &directory);
}

} // namespace galay::api
