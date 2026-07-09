#include <fcntl.h>
#include <unistd.h>

#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class ReadError {
    kOpen,
    kRead,
    kClose,
    kPath,
};

std::expected<std::string, ReadError> repoRoot()
{
    std::string path(__FILE__);
    const std::string marker = "/test/cpp/http/";
    const size_t marker_pos = path.rfind(marker);
    if (marker_pos == std::string::npos) {
        return std::unexpected(ReadError::kPath);
    }
    path.resize(marker_pos);
    return path;
}

std::expected<std::string, ReadError> readFile(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::unexpected(ReadError::kOpen);
    }

    std::string content;
    std::vector<char> buffer(64 * 1024);
    while (true) {
        const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
        if (bytes_read < 0) {
            const int close_result = ::close(fd);
            if (close_result != 0) {
                std::cerr << "close after read failure also failed\n";
            }
            return std::unexpected(ReadError::kRead);
        }
        if (bytes_read == 0) {
            break;
        }
        std::string& appended = content.append(buffer.data(), static_cast<size_t>(bytes_read));
        if (&appended != &content) {
            const int close_result = ::close(fd);
            if (close_result != 0) {
                std::cerr << "close after append failure also failed\n";
            }
            return std::unexpected(ReadError::kRead);
        }
    }

    const int close_result = ::close(fd);
    if (close_result != 0) {
        return std::unexpected(ReadError::kClose);
    }
    return content;
}

std::expected<std::string, ReadError> repoFile(std::string_view relative_path)
{
    auto root = repoRoot();
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    std::string path = *root;
    std::string& separator_appended = path.append("/");
    if (&separator_appended != &path) {
        return std::unexpected(ReadError::kPath);
    }
    std::string& appended = path.append(relative_path.data(), relative_path.size());
    if (&appended != &path) {
        return std::unexpected(ReadError::kPath);
    }
    return readFile(path);
}

std::expected<std::string, ReadError> routerSource()
{
    return repoFile("src/cpp/galay-http/server/http_router.cc");
}

std::expected<std::string, ReadError> routerHeader()
{
    return repoFile("src/cpp/galay-http/server/http_router.h");
}

std::expected<std::string, ReadError> requestHeader()
{
    return repoFile("src/cpp/galay-http/protoc/http_request.h");
}

int requireContains(std::string_view haystack, std::string_view needle, const char* message)
{
    const size_t found = haystack.find(needle);
    if (found == std::string_view::npos) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

int requireNotContains(std::string_view haystack, std::string_view needle, const char* message)
{
    const size_t found = haystack.find(needle);
    if (found != std::string_view::npos) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

} // namespace

int main()
{
    auto source = routerSource();
    if (!source.has_value()) {
        std::cerr << "failed to read http_router.cc\n";
        return 1;
    }
    auto header = routerHeader();
    if (!header.has_value()) {
        std::cerr << "failed to read http_router.h\n";
        return 1;
    }
    auto request = requestHeader();
    if (!request.has_value()) {
        std::cerr << "failed to read http_request.h\n";
        return 1;
    }

    if (const int rc = requireContains(*source,
                                       "searchRoutePath",
                                       "HttpRouter fuzzy hot path must scan string_view path segments")) {
        return rc;
    }
    if (const int rc = requireContains(*source,
                                       "std::vector<std::string_view>",
                                       "HttpRouter fuzzy hot path must store borrowed param segments before materializing params")) {
        return rc;
    }
    if (const int rc = requireNotContains(*source,
                                          "std::stringstream ss(path)",
                                          "HttpRouter path splitting must not use stringstream")) {
        return rc;
    }
    if (const int rc = requireNotContains(*source,
                                          "std::function<HttpRouteHandler*",
                                          "HttpRouter fuzzy hot path must not allocate through std::function recursion")) {
        return rc;
    }
    if (const int rc = requireNotContains(*source,
                                          "result.handler = searchRoute(fuzzyIt->second.get(), segments, result.params)",
                                          "HttpRouter findHandler must not allocate splitPath vector on fuzzy match")) {
        return rc;
    }
    if (const int rc = requireContains(*header,
                                       "RouteParams params",
                                       "RouteMatch must use the small route parameter container instead of std::map")) {
        return rc;
    }
    if (const int rc = requireNotContains(*header,
                                          "std::map<std::string, std::string> params",
                                          "RouteMatch params must not allocate std::map nodes on the match hot path")) {
        return rc;
    }
    if (const int rc = requireContains(*request,
                                       "RouteParams m_routeParams",
                                       "HttpRequest must keep route params in the small container until map compatibility is requested")) {
        return rc;
    }

    std::cout << "T88-RouterMatchSource PASS\n";
    return 0;
}
