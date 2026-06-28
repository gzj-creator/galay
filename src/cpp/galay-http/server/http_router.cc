#include <galay/cpp/galay-http/server/http_router.h>
#include <galay/cpp/galay-http/client/http_client.h>
#include <galay/cpp/galay-http/common/http_log.h>
#include <galay/cpp/galay-kernel/common/file_descriptor.h>
#include "http_etag.h"
#include "http_range.h"
#include <galay/cpp/galay-http/protoc/http_response.h>
#include <galay/cpp/galay-http/builder/http_builder.h>
#include <algorithm>
#include <array>
#include <sstream>
#include <set>
#include <cctype>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <limits>
#include <system_error>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace galay::http
{

using galay::kernel::FileDescriptor;

namespace {

constexpr size_t kProxyMaxIdleConnectionsPerUpstream = 32;
constexpr size_t kProxyRawRelayBufferSize = 16 * 1024;
thread_local std::unordered_map<std::string, std::vector<std::unique_ptr<HttpClient>>> g_proxyClientPools;

Task<void> closeHttpClientWithLog(HttpClient& client, std::string_view context)
{
    auto close_result = co_await client.close();
    if (!close_result) {
        HTTP_LOG_WARN("[proxy] [client-close-fail]",
                      "context={} error={}",
                      context,
                      close_result.error().message());
    }
    co_return;
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string toUpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string toCanonicalHeaderKey(std::string value) {
    bool word_start = true;
    for (char& ch : value) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (word_start) {
            ch = static_cast<char>(std::toupper(c));
        } else {
            ch = static_cast<char>(std::tolower(c));
        }
        word_start = (ch == '-');
    }
    return value;
}

void removeHeaderPairLoose(HeaderPair& headers, const std::string& key) {
    if (key.empty()) {
        return;
    }

    headers.removeHeaderPair(key);

    const std::string lower = toLowerAscii(key);
    const std::string upper = toUpperAscii(key);
    const std::string canonical = toCanonicalHeaderKey(key);

    if (lower != key) {
        headers.removeHeaderPair(lower);
    }
    if (upper != key && upper != lower) {
        headers.removeHeaderPair(upper);
    }
    if (canonical != key && canonical != lower && canonical != upper) {
        headers.removeHeaderPair(canonical);
    }
}

std::string getHeaderValueLoose(const HeaderPair& headers, const std::string& key) {
    return headers.getValue(key);
}

std::vector<std::string> splitConnectionTokens(const std::string& value) {
    std::vector<std::string> tokens;
    std::set<std::string> seen;
    std::string current;

    auto flush_token = [&]() {
        size_t begin = 0;
        while (begin < current.size() && std::isspace(static_cast<unsigned char>(current[begin]))) {
            ++begin;
        }

        size_t end = current.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(current[end - 1]))) {
            --end;
        }

        if (end > begin) {
            std::string token = toLowerAscii(current.substr(begin, end - begin));
            if (seen.insert(token).second) {
                tokens.push_back(std::move(token));
            }
        }
        current.clear();
    };

    for (char ch : value) {
        if (ch == ',') {
            flush_token();
        } else {
            current.push_back(ch);
        }
    }
    flush_token();

    return tokens;
}

std::string normalizeRoutePrefix(std::string routePrefix) {
    if (routePrefix.empty()) {
        return "/";
    }

    if (routePrefix.front() != '/') {
        routePrefix.insert(routePrefix.begin(), '/');
    }

    if (routePrefix.size() > 1 && routePrefix.back() == '/') {
        routePrefix.pop_back();
    }

    return routePrefix;
}

std::string buildUpstreamKey(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

std::string getClientIpFromConn(HttpConn& conn) {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);

    if (::getpeername(conn.getSocket().handle().fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return "";
    }

    char ipstr[INET6_ADDRSTRLEN] = {0};
    if (addr.ss_family == AF_INET) {
        auto* in = reinterpret_cast<sockaddr_in*>(&addr);
        if (::inet_ntop(AF_INET, &in->sin_addr, ipstr, sizeof(ipstr)) != nullptr) {
            return ipstr;
        }
    } else if (addr.ss_family == AF_INET6) {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
        if (::inet_ntop(AF_INET6, &in6->sin6_addr, ipstr, sizeof(ipstr)) != nullptr) {
            return ipstr;
        }
    }

    return "";
}

void applyForwardHeaders(HttpConn& conn, HeaderPair& headers, const std::string& original_host) {
    const std::string client_ip = getClientIpFromConn(conn);
    std::string xff = getHeaderValueLoose(headers, "X-Forwarded-For");

    if (!client_ip.empty()) {
        if (xff.empty()) {
            xff = client_ip;
        } else {
            xff += ", " + client_ip;
        }
        headers.addHeaderPair("X-Forwarded-For", xff);
        headers.addHeaderPair("X-Real-IP", client_ip);
    }

    headers.addHeaderPair("X-Forwarded-Proto", "http");
    if (!original_host.empty()) {
        headers.addHeaderPair("X-Forwarded-Host", original_host);
    }
}

std::string rewriteProxyUri(const std::string& routePrefix, const std::string& requestUri) {
    if (requestUri.empty()) {
        return "/";
    }

    if (routePrefix == "/") {
        return requestUri;
    }

    if (requestUri == routePrefix) {
        return "/";
    }

    if (requestUri.rfind(routePrefix, 0) == 0) {
        std::string suffix = requestUri.substr(routePrefix.size());
        if (suffix.empty()) {
            return "/";
        }
        if (suffix.front() == '?') {
            return "/" + suffix;
        }
        if (suffix.front() != '/') {
            return "/" + suffix;
        }
        return suffix;
    }

    return requestUri;
}

bool isLikelyStreamingRequest(const std::string& uri, const HeaderPair& headers)
{
    const std::string accept = toLowerAscii(getHeaderValueLoose(headers, "Accept"));
    if (accept.find("text/event-stream") != std::string::npos) {
        return true;
    }

    const std::string content_type = toLowerAscii(getHeaderValueLoose(headers, "Content-Type"));
    if (content_type.find("text/event-stream") != std::string::npos) {
        return true;
    }

    const std::string proxy_stream = toLowerAscii(getHeaderValueLoose(headers, "X-Proxy-Stream"));
    if (proxy_stream == "1" || proxy_stream == "true" || proxy_stream == "yes") {
        return true;
    }

    const std::string lower_uri = toLowerAscii(uri);
    if (lower_uri.find("/stream") != std::string::npos ||
        lower_uri.find("stream=true") != std::string::npos ||
        lower_uri.find("stream=1") != std::string::npos) {
        return true;
    }

    return false;
}

Task<void> sendProxyError(HttpConn& conn, HttpStatusCode code, const std::string& message) {
    auto response = Http1_1ResponseBuilder()
        .status(code)
        .header("Server", "Galay-Proxy/1.0")
        .text(message)
        .buildMove();

    auto writer = conn.getWriter();
    while (true) {
        auto result = co_await writer.sendResponse(response);
        if (!result || result.value()) {
            break;
        }
    }
    co_return;
}

Task<void> connectProxyUpstream(HttpClient& client,
                                const std::string& url,
                                bool& ok,
                                std::string& err_msg)
{
    ok = false;
    err_msg.clear();

    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        err_msg = connect_result.error().message();
        co_return;
    }
    ok = true;

    co_return;
}

Task<void> relayRawUpstreamToDownstream(TcpSocket& upstream,
                                        TcpSocket& downstream,
                                        std::chrono::milliseconds downstream_write_timeout,
                                        bool& ok,
                                        std::string& err_msg)
{
    ok = false;
    err_msg.clear();

    std::array<char, kProxyRawRelayBufferSize> buffer{};
    while (true) {
        auto recv_result = co_await upstream.recv(buffer.data(), buffer.size());
        if (!recv_result) {
            err_msg = recv_result.error().message();
            co_return;
        }

        const size_t bytes = recv_result.value();
        if (bytes == 0) {
            ok = true;
            co_return;
        }

        size_t offset = 0;
        while (offset < bytes) {
            const char* data = buffer.data() + offset;
            auto send_result = co_await downstream.send(data, bytes - offset)
                .timeout(downstream_write_timeout);
            if (!send_result) {
                err_msg = send_result.error().message();
                co_return;
            }

            const size_t sent = send_result.value();
            if (sent == 0) {
                err_msg = "downstream send returned 0";
                co_return;
            }

            offset += sent;
        }
    }
}

std::chrono::milliseconds responseWriteTimeoutFromConn(const HttpConn& conn)
{
    return std::chrono::milliseconds(conn.defaultWriterSetting().getSendTimeout());
}

} // namespace

HttpRouter::HttpRouter()
    : m_fallbackProxyHandlerState(std::make_shared<std::optional<HttpRouteHandler>>())
    , m_routeCount(0)
{
}

void HttpRouter::addHandlerInternal(HttpMethod method, const std::string& path, HttpRouteHandler handler)
{
    // 验证路径格式
    std::string error;
    if (!validatePath(path, error)) {
        // 路径格式错误，记录日志并返回
        HTTP_LOG_ERROR("[route] [invalid]", "path={} error={}", path, error);
        return;
    }

    if (isFuzzyPattern(path)) {
        // 模糊匹配路由 - 使用Trie树
        auto& root = m_fuzzyRoutes[method];
        if (!root) {
            root = std::make_unique<RouteTrieNode>();
        }

        auto segments = splitPath(path);
        insertRoute(root.get(), segments, handler);
        m_routeCount++;
    } else {
        // 精确匹配路由 - 使用unordered_map
        // 检查是否已存在（冲突检测）
        auto& methodRoutes = m_exactRoutes[method];
        bool isNewRoute = !methodRoutes.count(path);

        if (!isNewRoute) {
            HTTP_LOG_WARN("[route] [overwrite]",
                          "method={} path={}",
                          static_cast<int>(method),
                          path);
        }

        methodRoutes[path] = handler;

        // 只有新路由才增加计数
        if (isNewRoute) {
            m_routeCount++;
        }
    }
}

RouteMatch HttpRouter::findHandler(HttpMethod method, const std::string& path)
{
    RouteMatch result;

    // 1. 先尝试精确匹配（O(1)）
    auto methodIt = m_exactRoutes.find(method);
    if (methodIt != m_exactRoutes.end()) {
        auto pathIt = methodIt->second.find(path);
        if (pathIt != methodIt->second.end()) {
            result.handler = &pathIt->second;
            return result;
        }
    }

    // 2. 尝试模糊匹配 - 使用Trie树（O(k)，k为路径段数）
    auto fuzzyIt = m_fuzzyRoutes.find(method);
    if (fuzzyIt != m_fuzzyRoutes.end() && fuzzyIt->second) {
        auto segments = splitPath(path);
        result.handler = searchRoute(fuzzyIt->second.get(), segments, result.params);
    }

    return result;  // 未找到，handler为nullptr
}

bool HttpRouter::delHandler(HttpMethod method, const std::string& path)
{
    // 尝试从精确匹配中移除
    auto methodIt = m_exactRoutes.find(method);
    if (methodIt != m_exactRoutes.end()) {
        auto removed = methodIt->second.erase(path);
        if (removed > 0) {
            m_routeCount--;
            return true;
        }
    }

    // TODO: 从Trie树中移除路由（较复杂，暂不实现）
    // Trie树的删除需要递归处理，避免留下空节点

    return false;
}

void HttpRouter::clear()
{
    m_exactRoutes.clear();
    m_fuzzyRoutes.clear();
    if (m_fallbackProxyHandlerState) {
        m_fallbackProxyHandlerState->reset();
    }
    m_routeCount = 0;
}

size_t HttpRouter::size() const
{
    return m_routeCount;
}

bool HttpRouter::isFuzzyPattern(const std::string& path) const
{
    // 检查是否包含路径参数（:param）或通配符（*）
    return path.find(':') != std::string::npos ||
           path.find('*') != std::string::npos;
}

std::vector<std::string> HttpRouter::splitPath(const std::string& path) const
{
    std::vector<std::string> segments;
    std::stringstream ss(path);
    std::string segment;

    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) {
            segments.push_back(segment);
        }
    }

    return segments;
}

void HttpRouter::insertRoute(RouteTrieNode* root, const std::vector<std::string>& segments,
                             HttpRouteHandler handler)
{
    RouteTrieNode* node = root;
    std::vector<std::string> paramNames;

    for (const auto& segment : segments) {
        // 判断段类型
        if (segment == "*" || segment == "**") {
            // 通配符节点
            auto& child = node->children[segment];
            if (!child) {
                child = std::make_unique<RouteTrieNode>();
                child->isWildcard = true;
            }
            node = child.get();
        } else if (!segment.empty() && segment[0] == ':') {
            // 参数节点（:id）
            // 所有参数节点共享同一个键 ":param"
            std::string paramName = segment.substr(1);  // 去掉冒号
            paramNames.push_back(paramName);

            auto& child = node->children[":param"];
            if (!child) {
                child = std::make_unique<RouteTrieNode>();
                child->isParam = true;
            }
            node = child.get();
        } else {
            // 普通节点
            auto& child = node->children[segment];
            if (!child) {
                child = std::make_unique<RouteTrieNode>();
            }
            node = child.get();
        }
    }

    // 标记为路径终点并设置处理函数
    node->isEnd = true;
    node->handler = handler;
    node->paramNames = std::move(paramNames);
}

HttpRouteHandler* HttpRouter::searchRoute(RouteTrieNode* root, const std::vector<std::string>& segments,
                                          std::map<std::string, std::string>& params)
{
    params.clear();
    std::vector<std::string> paramValues;

    // 使用递归进行深度优先搜索，收集参数值到 paramValues
    std::function<HttpRouteHandler*(RouteTrieNode*, size_t)> dfs =
        [&](RouteTrieNode* node, size_t depth) -> HttpRouteHandler* {

        // 到达路径末尾
        if (depth == segments.size()) {
            if (node->isEnd) {
                // 用终端节点的 paramNames 和收集到的 paramValues 构建 params
                for (size_t i = 0; i < node->paramNames.size() && i < paramValues.size(); ++i) {
                    params[node->paramNames[i]] = paramValues[i];
                }
                return &node->handler;
            }
            return nullptr;
        }

        const std::string& segment = segments[depth];

        // 1. 优先尝试精确匹配
        auto exactIt = node->children.find(segment);
        if (exactIt != node->children.end()) {
            auto result = dfs(exactIt->second.get(), depth + 1);
            if (result) return result;
        }

        // 2. 尝试参数匹配（:param）
        auto paramIt = node->children.find(":param");
        if (paramIt != node->children.end()) {
            paramValues.push_back(segment);
            auto result = dfs(paramIt->second.get(), depth + 1);
            if (result) return result;
            paramValues.pop_back();
        }

        // 3. 尝试单段通配符（*）
        auto wildcardIt = node->children.find("*");
        if (wildcardIt != node->children.end()) {
            auto result = dfs(wildcardIt->second.get(), depth + 1);
            if (result) return result;
        }

        // 4. 尝试贪婪通配符（**）- 匹配剩余所有段
        auto greedyIt = node->children.find("**");
        if (greedyIt != node->children.end()) {
            auto* greedyNode = greedyIt->second.get();
            if (greedyNode->isEnd) {
                return &greedyNode->handler;
            }
        }

        return nullptr;
    };

    return dfs(root, 0);
}

bool HttpRouter::validatePath(const std::string& path, std::string& error) const
{
    // 1. 检查路径是否为空
    if (path.empty()) {
        error = "Path cannot be empty";
        return false;
    }

    // 2. 检查是否以 / 开头
    if (path[0] != '/') {
        error = "Path must start with '/'";
        return false;
    }

    // 3. 检查路径长度
    if (path.length() > 2048) {
        error = "Path is too long (max 2048 characters)";
        return false;
    }

    // 4. 分割路径并检查每个段
    auto segments = splitPath(path);

    if (segments.empty() && path != "/") {
        error = "Invalid path format";
        return false;
    }

    // 5. 检查参数名是否重复
    std::set<std::string> paramNames;
    bool hasWildcard = false;

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];

        // 检查空段
        if (segment.empty()) {
            error = "Path contains empty segment";
            return false;
        }

        // 检查参数节点
        if (segment[0] == ':') {
            if (segment.length() == 1) {
                error = "Parameter name cannot be empty (found ':' without name)";
                return false;
            }

            std::string paramName = segment.substr(1);

            // 检查参数名第一个字符（必须是字母或下划线）
            if (!std::isalpha(paramName[0]) && paramName[0] != '_') {
                error = "Parameter name '" + paramName + "' must start with a letter or underscore";
                return false;
            }

            // 检查参数名是否合法（只能包含字母、数字、下划线）
            for (char c : paramName) {
                if (!std::isalnum(c) && c != '_') {
                    error = "Parameter name '" + paramName + "' contains invalid character '" + std::string(1, c) + "'";
                    return false;
                }
            }

            // 检查参数名是否重复
            if (paramNames.count(paramName)) {
                error = "Duplicate parameter name: '" + paramName + "'";
                return false;
            }
            paramNames.insert(paramName);
        }
        // 检查通配符节点
        else if (segment == "*" || segment == "**") {
            if (hasWildcard) {
                error = "Path can only contain one wildcard";
                return false;
            }

            // 通配符必须是最后一个段
            if (i != segments.size() - 1) {
                error = "Wildcard '" + segment + "' must be the last segment";
                return false;
            }

            hasWildcard = true;
        }
        // 普通段
        else {
            // 检查是否包含非法字符
            for (char c : segment) {
                if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '~') {
                    error = "Segment '" + segment + "' contains invalid character '" + std::string(1, c) + "'";
                    return false;
                }
            }
        }
    }

    return true;
}

// ==================== 静态文件服务实现 ====================

void HttpRouter::mount(const std::string& routePrefix, const std::string& dirPath,
                       const StaticFileSetting& config)
{
    namespace fs = std::filesystem;

    // 验证目录是否存在
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        HTTP_LOG_ERROR("[mount] [fail]", "dir={}", dirPath);
        return;
    }

    // 保存挂载信息
    m_mountedDirs[routePrefix] = dirPath;

    // 创建动态文件处理器。文件未命中时，优先走 fallback proxy（如已配置）
    auto fallback_state = m_fallbackProxyHandlerState;
    HttpRouteHandler fallback = [fallback_state](HttpConn& conn, HttpRequest req) -> Task<void> {
        if (fallback_state && fallback_state->has_value()) {
            co_await fallback_state->value()(conn, std::move(req));
            co_return;
        }

        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::NotFound_404)
            .body("404 Not Found")
            .buildMove();
        auto writer = conn.getWriter();
        while (true) {
            auto send_result = co_await writer.sendResponse(response);
            if (!send_result || send_result.value()) break;
        }
        co_return;
    };

    auto handler = createStaticFileHandler(routePrefix, dirPath, config, std::move(fallback));

    // 注册通配符路由：routePrefix/**
    std::string wildcardPath = routePrefix;
    if (wildcardPath.back() != '/') {
        wildcardPath += '/';
    }
    wildcardPath += "**";

    // 为 GET 和 HEAD 方法注册路由
    addHandler<HttpMethod::GET, HttpMethod::HEAD>(wildcardPath, handler);

    HTTP_LOG_INFO("[mount]", "dir={} route={}", dirPath, routePrefix);
}

void HttpRouter::mountHardly(const std::string& routePrefix, const std::string& dirPath,
                             const StaticFileSetting& config)
{
    namespace fs = std::filesystem;

    // 验证目录是否存在
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        HTTP_LOG_ERROR("[mount-hard] [fail]", "dir={}", dirPath);
        return;
    }

    // 递归遍历目录并注册所有文件
    registerFilesRecursively(routePrefix, dirPath, config, "");

    HTTP_LOG_INFO("[mount-hard]", "dir={} route={}", dirPath, routePrefix);
}

void HttpRouter::tryFiles(const std::string& routePrefix,
                          const std::string& dirPath,
                          const std::string& upstreamHost,
                          uint16_t upstreamPort,
                          const StaticFileSetting& config,
                          ProxyMode mode)
{
    namespace fs = std::filesystem;

    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        HTTP_LOG_ERROR("[try-files] [mount-fail]", "dir={}", dirPath);
        return;
    }

    if (upstreamHost.empty() || upstreamPort == 0) {
        HTTP_LOG_ERROR("[try-files] [invalid-upstream]",
                       "host={} port={}",
                       upstreamHost,
                       upstreamPort);
        return;
    }

    std::string normalizedPrefix = normalizeRoutePrefix(routePrefix);
    auto fallbackProxy = createProxyHandler("/", upstreamHost, upstreamPort, mode);
    auto handler = createStaticFileHandler(normalizedPrefix, dirPath, config, std::move(fallbackProxy));

    std::string wildcardPath = normalizedPrefix;
    if (wildcardPath.back() != '/') {
        wildcardPath += '/';
    }
    wildcardPath += "**";

    addHandler<HttpMethod::GET, HttpMethod::HEAD>(wildcardPath, handler);
    if (normalizedPrefix != "/") {
        addHandler<HttpMethod::GET, HttpMethod::HEAD>(normalizedPrefix, handler);
    }

    HTTP_LOG_INFO("[try-files]",
                  "dir={} route={} upstream={}:{}",
                  dirPath,
                  normalizedPrefix,
                  upstreamHost,
                  upstreamPort);
}

void HttpRouter::proxy(const std::string& routePrefix,
                       const std::string& upstreamHost,
                       uint16_t upstreamPort,
                       ProxyMode mode)
{
    if (upstreamHost.empty() || upstreamPort == 0) {
        HTTP_LOG_ERROR("[proxy] [invalid-upstream]",
                       "host={} port={}",
                       upstreamHost,
                       upstreamPort);
        return;
    }

    std::string normalizedPrefix = normalizeRoutePrefix(routePrefix);
    auto handler = createProxyHandler(normalizedPrefix, upstreamHost, upstreamPort, mode);

    std::string wildcardPath = normalizedPrefix == "/" ? "/**" : normalizedPrefix + "/**";
    addHandler<HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT,
               HttpMethod::PATCH, HttpMethod::DELETE, HttpMethod::HEAD,
               HttpMethod::OPTIONS>(wildcardPath, handler);

    // 让 /api 本身也命中代理，等价转发为上游 /
    if (normalizedPrefix != "/") {
        addHandler<HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT,
                   HttpMethod::PATCH, HttpMethod::DELETE, HttpMethod::HEAD,
                   HttpMethod::OPTIONS>(normalizedPrefix, handler);
    } else {
        // 统一语义：proxy("/") 同时作为本地路由未命中时的 fallback proxy
        if (!m_fallbackProxyHandlerState) {
            m_fallbackProxyHandlerState = std::make_shared<std::optional<HttpRouteHandler>>();
        }
        *m_fallbackProxyHandlerState = createProxyHandler("/", upstreamHost, upstreamPort, mode);
        HTTP_LOG_INFO("[proxy-fallback] [enable]",
                      "upstream={}:{} mode={}",
                      upstreamHost,
                      upstreamPort,
                      mode == ProxyMode::Raw ? "raw" : "http");
    }

    HTTP_LOG_INFO("[proxy] [mount]",
                  "upstream={}:{} route={}",
                  upstreamHost,
                  upstreamPort,
                  normalizedPrefix);
}

bool HttpRouter::hasFallbackProxy() const
{
    return m_fallbackProxyHandlerState && m_fallbackProxyHandlerState->has_value();
}

HttpRouteHandler* HttpRouter::fallbackProxyHandler()
{
    if (!m_fallbackProxyHandlerState || !m_fallbackProxyHandlerState->has_value()) {
        return nullptr;
    }
    return &m_fallbackProxyHandlerState->value();
}

HttpRouteHandler HttpRouter::createStaticFileHandler(const std::string& routePrefix,
                                                     const std::string& dirPath,
                                                     const StaticFileSetting& config,
                                                     HttpRouteHandler fallbackHandler)
{
    namespace fs = std::filesystem;

    std::error_code canonical_dir_error;
    fs::path canonicalDir = fs::canonical(dirPath, canonical_dir_error);
    if (canonical_dir_error) {
        canonicalDir = fs::path(dirPath);
    }

    // 捕获 routePrefix、dirPath 和 config，返回一个协程处理器
    return [routePrefix, dirPath, canonicalDir, config, fallbackHandler](HttpConn& conn, HttpRequest req) -> Task<void> {
        namespace fs = std::filesystem;

        // 获取请求的路径参数（通配符匹配的部分）
        std::string requestPath = req.header().uri();

        // 从 URI 中提取相对路径
        // 例如：/static/css/style.css -> css/style.css
        std::string relativePath;
        if (requestPath.size() > routePrefix.size()) {
            // 跳过 routePrefix 和后面的 /
            size_t start = routePrefix.size();
            if (requestPath[start] == '/') {
                start++;
            }
            relativePath = requestPath.substr(start);
        }

        // 构建完整文件路径
        fs::path fullPath = fs::path(dirPath) / relativePath;

        // 安全检查：防止路径遍历攻击
        std::error_code canonical_file_error;
        fs::path canonicalFile = fs::canonical(fullPath, canonical_file_error);
        const bool fileNotFound = static_cast<bool>(canonical_file_error);

        if (fileNotFound) {
            if (fallbackHandler) {
                co_await fallbackHandler(conn, std::move(req));
                co_return;
            }
            // 文件不存在
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::NotFound_404)
                .body("404 Not Found")
                .buildMove();
            auto writer = conn.getWriter();
            while (true) {
                auto send_result = co_await writer.sendResponse(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }

        // 检查文件是否在允许的目录内
        auto [dirIt, fileIt] = std::mismatch(canonicalDir.begin(), canonicalDir.end(),
                                              canonicalFile.begin());
        if (dirIt != canonicalDir.end()) {
            // 路径遍历攻击
            HTTP_LOG_WARN("[path] [traversal]", "request={}", requestPath);
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::Forbidden_403)
                .body("403 Forbidden")
                .buildMove();
            auto writer = conn.getWriter();
            while (true) {
                auto send_result = co_await writer.sendResponse(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }

        // 检查文件是否存在且是普通文件
        std::error_code exists_error;
        const bool file_exists = fs::exists(canonicalFile, exists_error);
        std::error_code regular_file_error;
        const bool is_regular = fs::is_regular_file(canonicalFile, regular_file_error);
        if (exists_error || regular_file_error || !file_exists || !is_regular) {
            if (fallbackHandler) {
                co_await fallbackHandler(conn, std::move(req));
                co_return;
            }
            HTTP_LOG_WARN("[file] [missing]", "path={}", canonicalFile.string());
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::NotFound_404)
                .body("404 Not Found")
                .buildMove();
            auto writer = conn.getWriter();
            while (true) {
                auto send_result = co_await writer.sendResponse(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }

        // 获取文件大小
        std::error_code file_size_error;
        uintmax_t rawFileSize = fs::file_size(canonicalFile, file_size_error);
        if (file_size_error || rawFileSize > std::numeric_limits<size_t>::max()) {
            HTTP_LOG_ERROR("[file] [stat-fail]",
                           "path={} error={}",
                           canonicalFile.string(),
                           file_size_error.message());
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::InternalServerError_500)
                .body("500 Internal Server Error")
                .buildMove();
            auto writer = conn.getWriter();
            while (true) {
                auto send_result = co_await writer.sendResponse(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }
        size_t fileSize = static_cast<size_t>(rawFileSize);

        // 设置 Content-Type
        std::string extension = canonicalFile.extension().string();
        std::string ext = extension.empty() ? "" : extension.substr(1);
        std::string mimeType = MimeType::convertToMimeType(ext);
        HTTP_LOG_DEBUG("[static]",
                       "request={} file={} size={} mime={}",
                       requestPath,
                       canonicalFile.string(),
                       fileSize,
                       mimeType);
        co_await sendFileContent(conn, req, canonicalFile.string(), fileSize, mimeType, config);
        co_return;
    };
}

void HttpRouter::registerFilesRecursively(const std::string& routePrefix,
                                          const std::string& dirPath,
                                          const StaticFileSetting& config,
                                          const std::string& currentPath)
{
    namespace fs = std::filesystem;

    fs::path fullPath = fs::path(dirPath) / currentPath;

    std::error_code iterator_error;
    fs::directory_iterator dir_it(fullPath, iterator_error);
    fs::directory_iterator end_it;
    if (iterator_error) {
        HTTP_LOG_ERROR("[dir] [read-fail]",
                       "dir={} error={}",
                       fullPath.string(),
                       iterator_error.message());
        return;
    }

    for (; dir_it != end_it; dir_it.increment(iterator_error)) {
        if (iterator_error) {
            HTTP_LOG_ERROR("[dir] [read-fail]",
                           "dir={} error={}",
                           fullPath.string(),
                           iterator_error.message());
            return;
        }

        const auto& entry = *dir_it;
        std::string entryName = entry.path().filename().string();
        std::string relativePath = currentPath.empty() ? entryName : currentPath + "/" + entryName;

        std::error_code entry_error;
        if (entry.is_directory(entry_error)) {
            if (entry_error) {
                HTTP_LOG_ERROR("[dir] [entry-fail]",
                               "path={} error={}",
                               entry.path().string(),
                               entry_error.message());
                continue;
            }
            // 递归处理子目录
            registerFilesRecursively(routePrefix, dirPath, config, relativePath);
            continue;
        }

        if (entry.is_regular_file(entry_error)) {
            if (entry_error) {
                HTTP_LOG_ERROR("[file] [entry-fail]",
                               "path={} error={}",
                               entry.path().string(),
                               entry_error.message());
                continue;
            }
            // 为文件创建路由
            std::string routePath = routePrefix;
            if (routePath.back() != '/') {
                routePath += '/';
            }
            routePath += relativePath;

            // 创建文件处理器
            std::string filePath = entry.path().string();
            auto handler = createSingleFileHandler(filePath, config);

            // 注册路由
            addHandler<HttpMethod::GET, HttpMethod::HEAD>(routePath, handler);
        } else if (entry_error) {
            HTTP_LOG_ERROR("[file] [entry-fail]",
                           "path={} error={}",
                           entry.path().string(),
                           entry_error.message());
        }
    }
}

HttpRouteHandler HttpRouter::createSingleFileHandler(const std::string& filePath,
                                                     const StaticFileSetting& config)
{
    // 捕获文件路径和配置
    return [filePath, config](HttpConn& conn, HttpRequest req) -> Task<void> {
        namespace fs = std::filesystem;

        // 检查文件是否存在
        if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::NotFound_404)
                .body("404 Not Found")
                .buildMove();
            auto writer = conn.getWriter();
            while (true) {
                auto send_result = co_await writer.sendResponse(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }

        // 获取文件大小
        size_t fileSize = fs::file_size(filePath);

        // 设置 Content-Type
        fs::path path(filePath);
        std::string extension = path.extension().string();
        std::string ext = extension.empty() ? "" : extension.substr(1);
        std::string mimeType = MimeType::convertToMimeType(ext);

        // 使用配置的传输方式发送文件
        co_await sendFileContent(conn, req, filePath, fileSize, mimeType, config);
        co_return;
    };
}

HttpRouteHandler HttpRouter::createProxyHandler(const std::string& routePrefix,
                                                const std::string& upstreamHost,
                                                uint16_t upstreamPort,
                                                ProxyMode mode)
{
    return [routePrefix, upstreamHost, upstreamPort, mode](HttpConn& conn, HttpRequest req) -> Task<void> {
        const std::string request_uri = req.header().uri();
        const std::string upstream_uri = rewriteProxyUri(routePrefix, request_uri);
        const std::string pool_key = buildUpstreamKey(upstreamHost, upstreamPort);
        const std::string upstream_connect_url = "http://" + upstreamHost + ":" +
                                                 std::to_string(upstreamPort) + "/";

        auto& headers = req.header().headerPairs();
        const std::string original_host = getHeaderValueLoose(headers, "Host");
        const std::string connection = getHeaderValueLoose(headers, "Connection");
        std::vector<std::string> hop_by_hop_tokens = splitConnectionTokens(connection);

        ProxyMode effective_mode = mode;
        if (mode == ProxyMode::Http && isLikelyStreamingRequest(upstream_uri, headers)) {
            effective_mode = ProxyMode::Raw;
            HTTP_LOG_INFO("[proxy] [stream-upgrade]",
                          "uri={} upstream={}:{}",
                          upstream_uri,
                          upstreamHost,
                          upstreamPort);
        }

        removeHeaderPairLoose(headers, "Connection");
        removeHeaderPairLoose(headers, "Proxy-Connection");
        removeHeaderPairLoose(headers, "Keep-Alive");
        removeHeaderPairLoose(headers, "TE");
        removeHeaderPairLoose(headers, "Trailer");
        removeHeaderPairLoose(headers, "Transfer-Encoding");
        removeHeaderPairLoose(headers, "Upgrade");
        for (const auto& token : hop_by_hop_tokens) {
            removeHeaderPairLoose(headers, token);
        }

        applyForwardHeaders(conn, headers, original_host);
        removeHeaderPairLoose(headers, "Host");
        headers.addHeaderPair("Host", upstreamHost + ":" + std::to_string(upstreamPort));
        headers.addHeaderPair("Connection", effective_mode == ProxyMode::Raw ? "close" : "keep-alive");

        req.header().uri() = upstream_uri;

        if (effective_mode == ProxyMode::Raw) {
            auto client = std::make_unique<HttpClient>();
            bool connect_ok = false;
            std::string connect_err;
            co_await connectProxyUpstream(*client, upstream_connect_url, connect_ok, connect_err);
            if (!connect_ok) {
                HTTP_LOG_ERROR("[proxy-raw] [connect-fail]", "error={}", connect_err);
                co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: connect upstream failed");
                co_return;
            }

            auto session_result = client->getSession();
            if (!session_result) {
                HTTP_LOG_ERROR("[proxy-raw] [session-fail]", "error={}", session_result.error().message());
                co_await closeHttpClientWithLog(*client, "raw-session-fail");
                co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: upstream session failed");
                co_return;
            }
            auto& upstream_writer = session_result.value()->getWriter();
            bool send_ok = false;
            while (true) {
                auto send_result = co_await upstream_writer.sendRequest(req);
                if (!send_result) {
                    HTTP_LOG_WARN("[proxy-raw] [send-fail]",
                                  "error={}",
                                  send_result.error().message());
                    break;
                }
                if (send_result.value()) {
                    send_ok = true;
                    break;
                }
            }

            if (!send_ok) {
                co_await closeHttpClientWithLog(*client, "raw-send-fail");
                co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: send upstream failed");
                co_return;
            }

            bool relay_ok = false;
            std::string relay_err;
            auto upstream_socket = client->socket();
            if (!upstream_socket) {
                HTTP_LOG_ERROR("[proxy-raw] [socket-fail]", "error={}", upstream_socket.error().message());
                co_await closeHttpClientWithLog(*client, "raw-socket-fail");
                co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: upstream socket failed");
                co_return;
            }
            co_await relayRawUpstreamToDownstream(upstream_socket.value().get(),
                                                  conn.getSocket(),
                                                  responseWriteTimeoutFromConn(conn),
                                                  relay_ok,
                                                  relay_err);
            if (!relay_ok && !relay_err.empty()) {
                HTTP_LOG_WARN("[proxy-raw] [relay-fail]", "error={}", relay_err);
            }

            co_await closeHttpClientWithLog(*client, "raw-complete");
            co_return;
        }
        
        auto& pool = g_proxyClientPools[pool_key];
        std::unique_ptr<HttpClient> client;
        bool borrowed_from_pool = false;
        if (!pool.empty()) {
            client = std::move(pool.back());
            pool.pop_back();
            borrowed_from_pool = true;
        }

        if (!client) {
            client = std::make_unique<HttpClient>();
            bool connect_ok = false;
            std::string connect_err;
            co_await connectProxyUpstream(*client, upstream_connect_url, connect_ok, connect_err);
            if (!connect_ok) {
                HTTP_LOG_ERROR("[proxy] [connect-fail]", "error={}", connect_err);
                co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: connect upstream failed");
                co_return;
            }
        }

        HttpResponse upstream_response;
        bool request_ok = false;
        bool retried = false;

        while (!request_ok) {
            auto session_result = client->getSession();
            if (!session_result) {
                HTTP_LOG_ERROR("[proxy] [session-fail]", "error={}", session_result.error().message());
                co_await closeHttpClientWithLog(*client, "session-fail");
                co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: upstream session failed");
                co_return;
            }
            auto& upstream_writer = session_result.value()->getWriter();
            bool send_ok = false;
            while (true) {
                auto send_result = co_await upstream_writer.sendRequest(req);
                if (!send_result) {
                    HTTP_LOG_WARN("[proxy] [send-fail]",
                                  "error={}",
                                  send_result.error().message());
                    break;
                }
                if (send_result.value()) {
                    send_ok = true;
                    break;
                }
            }

            if (!send_ok) {
                co_await closeHttpClientWithLog(*client, "send-fail");
                if (borrowed_from_pool && !retried) {
                    retried = true;
                    borrowed_from_pool = false;
                    client = std::make_unique<HttpClient>();
                    bool reconnect_ok = false;
                    std::string reconnect_err;
                    co_await connectProxyUpstream(*client, upstream_connect_url, reconnect_ok, reconnect_err);
                    if (!reconnect_ok) {
                        HTTP_LOG_ERROR("[proxy] [reconnect-fail]", "error={}", reconnect_err);
                        co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                                "Bad Gateway: send upstream failed");
                        co_return;
                    }
                    continue;
                }

                co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: send upstream failed");
                co_return;
            }

            auto& upstream_reader = session_result.value()->getReader();
            upstream_response.reset();
            bool recv_ok = false;
            while (true) {
                auto recv_result = co_await upstream_reader.getResponse(upstream_response);
                if (!recv_result) {
                    HTTP_LOG_WARN("[proxy] [recv-fail]",
                                  "error={}",
                                  recv_result.error().message());
                    break;
                }
                if (recv_result.value()) {
                    recv_ok = true;
                    break;
                }
            }

            if (!recv_ok) {
                co_await closeHttpClientWithLog(*client, "recv-fail");
                if (borrowed_from_pool && !retried) {
                    retried = true;
                    borrowed_from_pool = false;
                    client = std::make_unique<HttpClient>();
                    bool reconnect_ok = false;
                    std::string reconnect_err;
                    co_await connectProxyUpstream(*client, upstream_connect_url, reconnect_ok, reconnect_err);
                    if (!reconnect_ok) {
                        HTTP_LOG_ERROR("[proxy] [reconnect-fail]", "error={}", reconnect_err);
                        co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                                "Bad Gateway: recv upstream failed");
                        co_return;
                    }
                    continue;
                }

                co_await sendProxyError(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: recv upstream failed");
                co_return;
            }

            request_ok = true;
        }

        auto downstream_writer = conn.getWriter();
        bool downstream_ok = false;
        while (true) {
            auto forward_result = co_await downstream_writer.sendResponse(upstream_response);
            if (!forward_result) {
                HTTP_LOG_ERROR("[proxy] [forward-fail]",
                               "error={}",
                               forward_result.error().message());
                break;
            }
            if (forward_result.value()) {
                downstream_ok = true;
                break;
            }
        }

        bool keep_upstream = downstream_ok &&
                             upstream_response.header().isKeepAlive() &&
                             !upstream_response.header().isConnectionClose();

        if (keep_upstream) {
            auto& idle = g_proxyClientPools[pool_key];
            if (idle.size() < kProxyMaxIdleConnectionsPerUpstream) {
                idle.push_back(std::move(client));
            } else if (client) {
                co_await closeHttpClientWithLog(*client, "pool-full");
            }
        } else if (client) {
            co_await closeHttpClientWithLog(*client, "not-keepalive");
        }

        co_return;
    };
}

// ==================== 文件传输实现 ====================

Task<void> HttpRouter::sendFileContent(HttpConn& conn,
                                       HttpRequest& req,
                                       const std::string& filePath,
                                       size_t fileSize,
                                       const std::string& mimeType,
                                       const StaticFileSetting& config)
{
    // 生成稳定 ETag（mtime + size + inode/路径哈希）
    namespace fs = std::filesystem;
    std::time_t lastModified = 0;
#ifdef _WIN32
    {
        std::error_code ec;
        auto ftime = fs::last_write_time(filePath, ec);
        if (!ec) {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            lastModified = std::chrono::system_clock::to_time_t(sctp);
        }
    }
#else
    struct stat st;
    if (stat(filePath.c_str(), &st) == 0) {
        lastModified = st.st_mtime;
    } else {
        std::error_code ec;
        auto ftime = fs::last_write_time(filePath, ec);
        if (!ec) {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            lastModified = std::chrono::system_clock::to_time_t(sctp);
        }
    }
#endif
    if (lastModified == 0) {
        // fallback: 使用当前时间，避免空值
        lastModified = std::time(nullptr);
    }

    const bool enableEtag = config.isEnableETag();
    std::string etag;
    if (enableEtag) {
        etag = ETagGenerator::generateStrong(filePath, fileSize, lastModified);
    }
    std::string lastModifiedStr = ETagGenerator::formatHttpDate(lastModified);

    auto writer = conn.getWriter();
    const bool isHeadRequest = req.header().method() == HttpMethod::HEAD;

    // 1. 处理 If-Match (前置条件)
    std::string ifMatch = req.header().headerPairs().getValue("If-Match");
    if (enableEtag && !ifMatch.empty() && !ETagGenerator::matchIfMatch(etag, ifMatch)) {
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::PreconditionFailed_412)
            .header("ETag", etag)
            .header("Last-Modified", lastModifiedStr)
            .buildMove();
        while (true) {
            auto send_result = co_await writer.sendResponse(response);
            if (!send_result || send_result.value()) break;
        }
        co_return;
    }

    // 2. 处理 If-None-Match (ETag 条件请求)
    std::string ifNoneMatch = req.header().headerPairs().getValue("If-None-Match");
    if (enableEtag && ETagGenerator::matchIfNoneMatch(etag, ifNoneMatch)) {
        // ETag 匹配，返回 304 Not Modified
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::NotModified_304)
            .header("ETag", etag)
            .header("Last-Modified", lastModifiedStr)
            .buildMove();
        while (true) {
            auto send_result = co_await writer.sendResponse(response);
            if (!send_result || send_result.value()) break;
        }
        co_return;
    }

    // 3. 处理 Range 请求
    std::string rangeHeader = req.header().headerPairs().getValue("Range");
    bool hasRange = !rangeHeader.empty();
    RangeParseResult rangeResult;

    if (hasRange) {
        // 解析 Range 请求
        rangeResult = HttpRangeParser::parse(rangeHeader, fileSize);

        // 3. 处理 If-Range 条件请求
        std::string ifRangeHeader = req.header().headerPairs().getValue("If-Range");
        if (!ifRangeHeader.empty()) {
            // 检查 If-Range 条件
            if (!HttpRangeParser::checkIfRange(ifRangeHeader, etag, lastModified)) {
                // If-Range 条件不满足，忽略 Range 请求，返回完整文件
                hasRange = false;
                rangeResult = RangeParseResult();
            }
        }

        // 验证 Range 是否有效
        if (hasRange && !rangeResult.isValid()) {
            // Range 无效，返回 416 Range Not Satisfiable
            const std::string body = "416 Range Not Satisfiable";
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::RangeNotSatisfiable_416)
                .header("Content-Range", "bytes */" + std::to_string(fileSize))
                .header("Content-Length", std::to_string(body.size()))
                .body(body)
                .buildMove();
            if (isHeadRequest) {
                HttpResponseHeader header = response.header();
                while (true) {
                    auto send_result = co_await writer.sendHeader(std::move(header));
                    if (!send_result || send_result.value()) break;
                }
                co_return;
            }
            while (true) {
                auto send_result = co_await writer.sendResponse(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }
    }

    // 4. 根据是否有 Range 请求决定响应方式
    if (hasRange && rangeResult.isValid()) {
        // 处理 Range 请求
        if (rangeResult.type == RangeType::SINGLE_RANGE) {
            // 单范围请求
            co_await sendSingleRange(conn, req, filePath, fileSize, mimeType, etag, lastModifiedStr, rangeResult.ranges[0], config);
        } else if (rangeResult.type == RangeType::MULTIPLE_RANGES) {
            // 多范围请求 (multipart/byteranges)
            co_await sendMultipleRanges(conn, req, filePath, fileSize, mimeType, etag, lastModifiedStr, rangeResult, config);
        }
        co_return;
    }

    // 5. 发送完整文件（无 Range 请求或 Range 无效）
    // 根据配置决定传输模式
    FileTransferMode mode = config.decideTransferMode(fileSize);
    // 构建响应头
    auto responseBuilder = Http1_1ResponseBuilder()
        .status(HttpStatusCode::OK_200)
        .header("Content-Type", mimeType)
        .header("Last-Modified", lastModifiedStr)
        .header("Accept-Ranges", "bytes");
    if (enableEtag) {
        responseBuilder.header("ETag", etag);
    }
    auto response = responseBuilder.buildMove();
    HTTP_LOG_DEBUG("[send]",
                   "file={} size={} mode={}",
                   filePath,
                   fileSize,
                   static_cast<int>(mode));

    if (isHeadRequest) {
        response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(fileSize));
        HttpResponseHeader header = response.header();
        while (true) {
            auto result = co_await writer.sendHeader(std::move(header));
            if (!result) {
                HTTP_LOG_ERROR("[send] [head-fail]",
                               "error={}",
                               result.error().message());
                break;
            }
            if (result.value()) {
                break;
            }
        }
        co_return;
    }

    switch (mode) {
        case FileTransferMode::MEMORY: {
            // 内存模式：将文件完整读入内存后发送
            std::ifstream file(filePath, std::ios::binary);
            if (!file) {
                HTTP_LOG_ERROR("[file] [open-fail]", "path={}", filePath);
                auto error_response = Http1_1ResponseBuilder()
                    .status(HttpStatusCode::InternalServerError_500)
                    .body("500 Internal Server Error")
                    .buildMove();
                co_await writer.send(error_response.toString());
                co_return;
            }

            std::string content(fileSize, '\0');
            file.read(&content[0], fileSize);
            response.setBodyStr(std::move(content));

            while (true) {
                auto result = co_await writer.sendResponse(response);
                if (!result) {
                    HTTP_LOG_ERROR("[send] [fail]",
                                   "error={}",
                                   result.error().message());
                    break;
                }
                if (result.value()) {
                    break;
                }
            }
            break;
        }

        case FileTransferMode::CHUNK: {
            // Chunk 模式：使用 HTTP chunked 编码分块传输
            response.header().headerPairs().addHeaderPair("Transfer-Encoding", "chunked");

            // 发送响应头（只发送头部，不包含 body）
            HttpResponseHeader header = response.header();
            auto headerResult = co_await writer.sendHeader(std::move(header));
            if (!headerResult) {
                HTTP_LOG_ERROR("[send] [header-fail]",
                               "error={}",
                               headerResult.error().message());
                co_return;
            }

            // 使用 RAII 管理文件描述符
            FileDescriptor fd;
            bool openSuccess = false;
            auto open_result = fd.open(filePath.c_str(), O_RDONLY);
            if (open_result) {
                openSuccess = true;
            } else {
                HTTP_LOG_ERROR("[file] [open-fail] [chunk]",
                               "path={} error={}",
                               filePath,
                               open_result.error().message());
            }

            if (!openSuccess) {
                // 发送空 chunk 结束
                co_await writer.sendChunk("", true);
                co_return;
            }

            // 分块读取并发送
            size_t chunkSize = config.getChunkSize();
            std::vector<char> buffer(chunkSize);
            ssize_t bytesRead;
            bool hasError = false;

            while ((bytesRead = read(fd.get(), buffer.data(), chunkSize)) > 0) {
                std::string chunk(buffer.data(), bytesRead);
                auto result = co_await writer.sendChunk(chunk, false);
                if (!result) {
                    HTTP_LOG_ERROR("[send] [chunk-fail]",
                                   "error={}",
                                   result.error().message());
                    hasError = true;
                    break;
                }
            }

            // 检查读取错误
            if (bytesRead < 0) {
                HTTP_LOG_ERROR("[file] [read-fail]", "error={}", strerror(errno));
                hasError = true;
            }

            // 发送最后一个空 chunk
            if (!hasError) {
                co_await writer.sendChunk("", true);
            }

            // fd 会在作用域结束时自动关闭
            break;
        }

        case FileTransferMode::SENDFILE: {
            // SendFile 模式：使用零拷贝 sendfile 系统调用
            response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(fileSize));

            // 发送响应头（只发送头部，不包含 body）
            HttpResponseHeader header = response.header();
            auto headerResult = co_await writer.sendHeader(std::move(header));
            if (!headerResult) {
                HTTP_LOG_ERROR("[send] [header-fail]",
                               "error={}",
                               headerResult.error().message());
                co_return;
            }

            // 使用 RAII 管理文件描述符
            FileDescriptor fd;
            auto open_result = fd.open(filePath.c_str(), O_RDONLY);
            if (!open_result) {
                HTTP_LOG_ERROR("[file] [open-fail] [sendfile]",
                               "path={} error={}",
                               filePath,
                               open_result.error().message());
                co_return;
            }

            // 使用 sendfile 零拷贝发送文件内容
            off_t offset = 0;
            size_t remaining = fileSize;
            size_t sendfileChunkSize = config.getSendFileChunkSize();
            const auto response_write_timeout = responseWriteTimeoutFromConn(conn);

            while (remaining > 0) {
                size_t toSend = std::min(remaining, sendfileChunkSize);
                auto result = co_await conn.socket().sendfile(fd.get(), offset, toSend)
                    .timeout(response_write_timeout);

                if (!result) {
                    HTTP_LOG_ERROR("[sendfile] [fail]",
                                   "error={}",
                                   result.error().message());
                    break;
                }

                size_t sent = result.value();
                if (sent == 0) {
                    HTTP_LOG_WARN("[sendfile] [zero]", "file={}", filePath);
                    break;
                }

                offset += sent;
                remaining -= sent;
            }

            // fd 会在作用域结束时自动关闭
            break;
        }

        case FileTransferMode::AUTO:
            // AUTO 模式应该在 decideTransferMode 中已经被转换为具体模式
            HTTP_LOG_ERROR("[mode] [auto] [invalid]", "file={}", filePath);
            break;
    }

    co_return;
}

// ==================== Range 请求处理实现 ====================

Task<void> HttpRouter::sendSingleRange(HttpConn& conn,
                                       HttpRequest& req,
                                       const std::string& filePath,
                                       size_t fileSize,
                                       const std::string& mimeType,
                                       const std::string& etag,
                                       const std::string& lastModified,
                                       const HttpRange& range,
                                       const StaticFileSetting& config)
{
    auto writer = conn.getWriter();

    // 构建 206 Partial Content 响应
    auto responseBuilder = Http1_1ResponseBuilder()
        .status(HttpStatusCode::PartialContent_206)
        .header("Content-Type", mimeType)
        .header("Content-Range", HttpRangeParser::makeContentRange(range, fileSize))
        .header("Content-Length", std::to_string(range.length))
        .header("Last-Modified", lastModified)
        .header("Accept-Ranges", "bytes");
    if (!etag.empty()) {
        responseBuilder.header("ETag", etag);
    }
    auto response = responseBuilder.buildMove();

    // 发送响应头
    HttpResponseHeader header = response.header();
    auto headerResult = co_await writer.sendHeader(std::move(header));
    if (!headerResult) {
        HTTP_LOG_ERROR("[send] [header-fail]",
                       "error={}",
                       headerResult.error().message());
        co_return;
    }

    if (req.header().method() == HttpMethod::HEAD) {
        co_return;
    }

    // 打开文件
    FileDescriptor fd;
    auto open_result = fd.open(filePath.c_str(), O_RDONLY);
    if (!open_result) {
        HTTP_LOG_ERROR("[file] [open-fail] [range]",
                       "path={} error={}",
                       filePath,
                       open_result.error().message());
        co_return;
    }

    // 根据配置决定传输模式
    FileTransferMode mode = config.decideTransferMode(range.length);

    if (mode == FileTransferMode::SENDFILE) {
        // 使用 sendfile 零拷贝发送范围内容
        off_t offset = range.start;
        size_t remaining = range.length;
        size_t sendfileChunkSize = config.getSendFileChunkSize();
        const auto response_write_timeout = responseWriteTimeoutFromConn(conn);

        while (remaining > 0) {
            size_t toSend = std::min(remaining, sendfileChunkSize);
            auto result = co_await conn.socket().sendfile(fd.get(), offset, toSend)
                .timeout(response_write_timeout);

            if (!result) {
                HTTP_LOG_ERROR("[sendfile] [fail]",
                               "error={}",
                               result.error().message());
                break;
            }

            size_t sent = result.value();
            if (sent == 0) {
                HTTP_LOG_WARN("[sendfile] [zero]", "file={}", filePath);
                break;
            }

            offset += sent;
            remaining -= sent;
        }
    } else {
        // 使用普通读取方式发送范围内容
        // 定位到起始位置
        if (lseek(fd.get(), range.start, SEEK_SET) == -1) {
            HTTP_LOG_ERROR("[file] [seek-fail]", "error={}", strerror(errno));
            co_return;
        }

        // 分块读取并发送
        size_t chunkSize = config.getChunkSize();
        std::vector<char> buffer(chunkSize);
        size_t remaining = range.length;

        while (remaining > 0) {
            size_t toRead = std::min(remaining, chunkSize);
            ssize_t bytesRead = read(fd.get(), buffer.data(), toRead);

            if (bytesRead < 0) {
                HTTP_LOG_ERROR("[file] [read-fail]", "error={}", strerror(errno));
                break;
            }

            if (bytesRead == 0) {
                break;
            }

            std::string chunk(buffer.data(), bytesRead);
            auto result = co_await writer.send(std::move(chunk));
            if (!result) {
                HTTP_LOG_ERROR("[send] [chunk-fail]",
                               "error={}",
                               result.error().message());
                break;
            }

            remaining -= bytesRead;
        }
    }

    co_return;
}

Task<void> HttpRouter::sendMultipleRanges(HttpConn& conn,
                                          HttpRequest& req,
                                          const std::string& filePath,
                                          size_t fileSize,
                                          const std::string& mimeType,
                                          const std::string& etag,
                                          const std::string& lastModified,
                                          const RangeParseResult& rangeResult,
                                          const StaticFileSetting& config)
{
    auto writer = conn.getWriter();

    // 构建 206 Partial Content 响应（multipart/byteranges）
    std::string boundary = rangeResult.boundary;
    auto responseBuilder = Http1_1ResponseBuilder()
        .status(HttpStatusCode::PartialContent_206)
        .header("Content-Type", "multipart/byteranges; boundary=" + boundary)
        .header("Last-Modified", lastModified)
        .header("Accept-Ranges", "bytes");
    if (!etag.empty()) {
        responseBuilder.header("ETag", etag);
    }
    auto response = responseBuilder.buildMove();

    // 计算总长度（包括所有边界和头部）
    size_t totalLength = 0;
    for (const auto& range : rangeResult.ranges) {
        // 边界行
        totalLength += 2 + boundary.length() + 2;  // "--boundary\r\n"
        // Content-Type 头
        totalLength += 14 + mimeType.length() + 2;  // "Content-Type: \r\n"
        // Content-Range 头
        std::string contentRange = HttpRangeParser::makeContentRange(range, fileSize);
        totalLength += 16 + contentRange.length() + 2;  // "Content-Range: \r\n"
        // 空行
        totalLength += 2;  // "\r\n"
        // 内容
        totalLength += range.length;
        // 换行
        totalLength += 2;  // "\r\n"
    }
    // 最后的边界
    totalLength += 2 + boundary.length() + 4;  // "--boundary--\r\n"

    response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(totalLength));

    // 发送响应头
    HttpResponseHeader header = response.header();
    auto headerResult = co_await writer.sendHeader(std::move(header));
    if (!headerResult) {
        HTTP_LOG_ERROR("[send] [header-fail]",
                       "error={}",
                       headerResult.error().message());
        co_return;
    }

    if (req.header().method() == HttpMethod::HEAD) {
        co_return;
    }

    // 打开文件
    FileDescriptor fd;
    auto open_result = fd.open(filePath.c_str(), O_RDONLY);
    if (!open_result) {
        HTTP_LOG_ERROR("[file] [open-fail] [range-multi]",
                       "path={} error={}",
                       filePath,
                       open_result.error().message());
        co_return;
    }

    // 发送每个范围
    for (const auto& range : rangeResult.ranges) {
        // 发送边界
        std::string boundaryLine = "--" + boundary + "\r\n";
        auto boundaryResult = co_await writer.send(std::move(boundaryLine));
        if (!boundaryResult) {
            HTTP_LOG_ERROR("[send] [boundary-fail]",
                           "error={}",
                           boundaryResult.error().message());
            co_return;
        }

        // 发送 Content-Type 头
        std::string contentTypeHeader = "Content-Type: " + mimeType + "\r\n";
        auto ctResult = co_await writer.send(std::move(contentTypeHeader));
        if (!ctResult) {
            HTTP_LOG_ERROR("[send] [ctype-fail]",
                           "error={}",
                           ctResult.error().message());
            co_return;
        }

        // 发送 Content-Range 头
        std::string contentRangeHeader = "Content-Range: " +
            HttpRangeParser::makeContentRange(range, fileSize) + "\r\n";
        auto crResult = co_await writer.send(std::move(contentRangeHeader));
        if (!crResult) {
            HTTP_LOG_ERROR("[send] [crange-fail]",
                           "error={}",
                           crResult.error().message());
            co_return;
        }

        // 发送空行
        std::string emptyLine = "\r\n";
        auto emptyLineResult = co_await writer.send(std::move(emptyLine));
        if (!emptyLineResult) {
            HTTP_LOG_ERROR("[send] [emptyline-fail]",
                           "error={}",
                           emptyLineResult.error().message());
            co_return;
        }

        // 定位到起始位置
        if (lseek(fd.get(), range.start, SEEK_SET) == -1) {
            HTTP_LOG_ERROR("[file] [seek-fail]", "error={}", strerror(errno));
            co_return;
        }

        // 读取并发送范围内容
        size_t chunkSize = config.getChunkSize();
        std::vector<char> buffer(chunkSize);
        size_t remaining = range.length;

        while (remaining > 0) {
            size_t toRead = std::min(remaining, chunkSize);
            ssize_t bytesRead = read(fd.get(), buffer.data(), toRead);

            if (bytesRead < 0) {
                HTTP_LOG_ERROR("[file] [read-fail]", "error={}", strerror(errno));
                co_return;
            }

            if (bytesRead == 0) {
                break;
            }

            std::string chunk(buffer.data(), bytesRead);
            auto result = co_await writer.send(std::move(chunk));
            if (!result) {
                HTTP_LOG_ERROR("[send] [chunk-fail]",
                               "error={}",
                               result.error().message());
                co_return;
            }

            remaining -= bytesRead;
        }

        // 发送换行
        std::string newline = "\r\n";
        auto newlineResult = co_await writer.send(std::move(newline));
        if (!newlineResult) {
            HTTP_LOG_ERROR("[send] [newline-fail]",
                           "error={}",
                           newlineResult.error().message());
            co_return;
        }
    }

    // 发送最后的边界
    std::string finalBoundary = "--" + boundary + "--\r\n";
    auto finalResult = co_await writer.send(std::move(finalBoundary));
    if (!finalResult) {
        HTTP_LOG_ERROR("[send] [final-boundary-fail]",
                       "error={}",
                       finalResult.error().message());
    }

    co_return;
}

} // namespace galay::http
