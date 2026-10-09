#include "http_router.h"
#include "../client/http_client.h"
#include "../common/http_log.h"
#include "../../galay-kernel/common/file_descriptor.h"
#include "http_etag.h"
#include "http_range.h"
#include "static_file_reader.h"
#include "../protoc/http_response.h"
#include "../builder/http_builder.h"
#include <algorithm>
#include <array>
#include <expected>
#include <set>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace galay::http
{

using galay::kernel::FileDescriptor;

namespace {

constexpr size_t kProxyMaxIdleConnectionsPerUpstream = 32;
constexpr size_t kProxyRawRelayBufferSize = 16 * 1024;
thread_local std::unordered_map<std::string, std::vector<std::unique_ptr<HttpClient>>> g_proxyClientPools;

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string to_upper_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string to_canonical_header_key(std::string value) {
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

void remove_header_pair_loose(HeaderPair& headers, const std::string& key) {
    if (key.empty()) {
        return;
    }

    headers.remove_header_pair(key);

    const std::string lower = to_lower_ascii(key);
    const std::string upper = to_upper_ascii(key);
    const std::string canonical = to_canonical_header_key(key);

    if (lower != key) {
        headers.remove_header_pair(lower);
    }
    if (upper != key && upper != lower) {
        headers.remove_header_pair(upper);
    }
    if (canonical != key && canonical != lower && canonical != upper) {
        headers.remove_header_pair(canonical);
    }
}

std::string get_header_value_loose(const HeaderPair& headers, const std::string& key) {
    return headers.get_value(key);
}

std::vector<std::string> split_connection_tokens(const std::string& value) {
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
            std::string token = to_lower_ascii(current.substr(begin, end - begin));
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

std::string normalize_route_prefix(std::string routePrefix) {
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

std::string build_upstream_key(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

std::string get_client_ip_from_conn(HttpConn& conn) {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);

    if (::getpeername(conn.get_socket().handle().fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
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

void apply_forward_headers(HttpConn& conn, HeaderPair& headers, const std::string& original_host) {
    const std::string client_ip = get_client_ip_from_conn(conn);
    std::string xff = get_header_value_loose(headers, "X-Forwarded-For");

    if (!client_ip.empty()) {
        if (xff.empty()) {
            xff = client_ip;
        } else {
            xff += ", " + client_ip;
        }
        headers.add_header_pair("X-Forwarded-For", xff);
        headers.add_header_pair("X-Real-IP", client_ip);
    }

    headers.add_header_pair("X-Forwarded-Proto", "http");
    if (!original_host.empty()) {
        headers.add_header_pair("X-Forwarded-Host", original_host);
    }
}

std::string rewrite_proxy_uri(const std::string& routePrefix, const std::string& requestUri) {
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

bool is_likely_streaming_request(const std::string& uri, const HeaderPair& headers)
{
    const std::string accept = to_lower_ascii(get_header_value_loose(headers, "Accept"));
    if (accept.find("text/event-stream") != std::string::npos) {
        return true;
    }

    const std::string content_type = to_lower_ascii(get_header_value_loose(headers, "Content-Type"));
    if (content_type.find("text/event-stream") != std::string::npos) {
        return true;
    }

    const std::string proxy_stream = to_lower_ascii(get_header_value_loose(headers, "X-Proxy-Stream"));
    if (proxy_stream == "1" || proxy_stream == "true" || proxy_stream == "yes") {
        return true;
    }

    const std::string lower_uri = to_lower_ascii(uri);
    if (lower_uri.find("/stream") != std::string::npos ||
        lower_uri.find("stream=true") != std::string::npos ||
        lower_uri.find("stream=1") != std::string::npos) {
        return true;
    }

    return false;
}

Task<void> send_proxy_error(HttpConn& conn, HttpStatusCode code, const std::string& message) {
    auto response = Http1_1ResponseBuilder()
        .status(code)
        .header("Server", "Galay-Proxy/1.0")
        .text(message)
        .build_move();

    auto writer = conn.get_writer();
    while (true) {
        auto result = co_await writer.send_response(response);
        if (!result || result.value()) {
            break;
        }
    }
    co_return;
}

Task<void> connect_proxy_upstream(HttpClient& client,
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

Task<void> relay_raw_upstream_to_downstream(AsyncTcpSocket& upstream,
                                        AsyncTcpSocket& downstream,
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

std::chrono::milliseconds response_write_timeout_from_conn(const HttpConn& conn)
{
    return std::chrono::milliseconds(conn.default_writer_setting().get_send_timeout());
}

} // namespace

HttpRouter::HttpRouter()
    : m_fallbackProxyHandlerState(std::make_shared<std::optional<HttpRouteHandler>>())
    , m_routeCount(0)
{
}

void HttpRouter::add_handler_internal(HttpMethod method, const std::string& path, HttpRouteHandler handler)
{
    add_route(method, path, HttpRouteEntry{std::move(handler), {}});
}

void HttpRouter::add_route(HttpMethod method, const std::string& path, HttpRouteEntry handlers)
{
    // 验证路径格式
    std::string error;
    if (!validate_path(path, error)) {
        // 路径格式错误，记录日志并返回
        HTTP_LOG_ERROR("[route] [invalid]", "path={} error={}", path, error);
        return;
    }

    if (is_fuzzy_pattern(path)) {
        // 模糊匹配路由 - 使用Trie树
        auto& root = m_fuzzyRoutes[method];
        if (!root) {
            root = std::make_unique<RouteTrieNode>();
        }

        auto segments = split_path(path);
        insert_route(root.get(), segments, std::move(handlers));
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

        methodRoutes[path] = std::move(handlers);

        // 只有新路由才增加计数
        if (isNewRoute) {
            m_routeCount++;
        }
    }
}

RouteMatch HttpRouter::find_handler(HttpMethod method, const std::string& path)
{
    RouteMatch result;

    // 1. 先尝试精确匹配（O(1)）
    auto methodIt = m_exactRoutes.find(method);
    if (methodIt != m_exactRoutes.end()) {
        auto pathIt = methodIt->second.find(path);
        if (pathIt != methodIt->second.end()) {
            auto& entry = pathIt->second;
            result.handler = entry.handler ? &entry.handler : nullptr;
            result.request_handler = entry.request_handler ? &entry.request_handler : nullptr;
            return result;
        }
    }

    // 2. 尝试模糊匹配 - 使用Trie树（O(k)，k为路径段数）
    auto fuzzyIt = m_fuzzyRoutes.find(method);
    if (fuzzyIt != m_fuzzyRoutes.end() && fuzzyIt->second) {
        if (auto* entry = search_route_path(fuzzyIt->second.get(), path, result.params)) {
            result.handler = entry->handler ? &entry->handler : nullptr;
            result.request_handler = entry->request_handler ? &entry->request_handler : nullptr;
        }
    }

    return result;  // 未找到，handler为nullptr
}

bool HttpRouter::has_connection_handlers() const
{
    if (m_fallbackProxyHandlerState && m_fallbackProxyHandlerState->has_value()) return true;
    for (const auto& [method, routes] : m_exactRoutes) {
        for (const auto& [path, entry] : routes) {
            if (entry.handler) return true;
        }
    }
    const auto contains = [](auto&& self, const RouteTrieNode* node) -> bool {
        if (!node) return false;
        if (node->handlers.handler) return true;
        for (const auto& [segment, child] : node->children) {
            if (self(self, child.get())) return true;
        }
        return false;
    };
    for (const auto& [method, root] : m_fuzzyRoutes) {
        if (contains(contains, root.get())) return true;
    }
    return false;
}

bool HttpRouter::del_handler(HttpMethod method, const std::string& path)
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

bool HttpRouter::is_fuzzy_pattern(const std::string& path) const
{
    // 检查是否包含路径参数（:param）或通配符（*）
    return path.find(':') != std::string::npos ||
           path.find('*') != std::string::npos;
}

std::vector<std::string> HttpRouter::split_path(const std::string& path)
{
    std::vector<std::string> segments;
    size_t offset = 0;
    while (offset < path.size()) {
        while (offset < path.size() && path[offset] == '/') {
            ++offset;
        }
        if (offset >= path.size()) {
            break;
        }
        const size_t segment_begin = offset;
        while (offset < path.size() && path[offset] != '/') {
            ++offset;
        }
        std::string& inserted = segments.emplace_back(
            path.substr(segment_begin, offset - segment_begin));
        if (inserted.empty()) {
            segments.pop_back();
        }
    }

    return segments;
}

void HttpRouter::insert_route(RouteTrieNode* root, const std::vector<std::string>& segments,
                             HttpRouteEntry handlers)
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
    node->handlers = std::move(handlers);
    node->paramNames = std::move(paramNames);
}

HttpRouteEntry* HttpRouter::search_route(RouteTrieNode* root, const std::vector<std::string>& segments,
                                          RouteParams& params)
{
    params.clear();
    std::vector<std::string> paramValues;

    // 使用递归进行深度优先搜索，收集参数值到 paramValues
    auto dfs = [&](auto&& self, RouteTrieNode* node, size_t depth) -> HttpRouteEntry* {

        // 到达路径末尾
        if (depth == segments.size()) {
            if (node->isEnd) {
                // 用终端节点的 paramNames 和收集到的 paramValues 构建 params
                for (size_t i = 0; i < node->paramNames.size() && i < paramValues.size(); ++i) {
                    const bool inserted = params.emplace(node->paramNames[i], paramValues[i]);
                    if (!inserted) {
                        return nullptr;
                    }
                }
                return &node->handlers;
            }
            return nullptr;
        }

        const std::string& segment = segments[depth];

        // 1. 优先尝试精确匹配
        auto exactIt = node->children.find(segment);
        if (exactIt != node->children.end()) {
            auto result = self(self, exactIt->second.get(), depth + 1);
            if (result) return result;
        }

        // 2. 尝试参数匹配（:param）
        auto paramIt = node->children.find(":param");
        if (paramIt != node->children.end()) {
            paramValues.push_back(segment);
            auto result = self(self, paramIt->second.get(), depth + 1);
            if (result) return result;
            paramValues.pop_back();
        }

        // 3. 尝试单段通配符（*）
        auto wildcardIt = node->children.find("*");
        if (wildcardIt != node->children.end()) {
            auto result = self(self, wildcardIt->second.get(), depth + 1);
            if (result) return result;
        }

        // 4. 尝试贪婪通配符（**）- 匹配剩余所有段
        auto greedyIt = node->children.find("**");
        if (greedyIt != node->children.end()) {
            auto* greedyNode = greedyIt->second.get();
            if (greedyNode->isEnd) {
                return &greedyNode->handlers;
            }
        }

        return nullptr;
    };

    return dfs(dfs, root, 0);
}

namespace {

bool next_route_segment(std::string_view path,
                      size_t offset,
                      std::string_view& segment,
                      size_t& next_offset)
{
    while (offset < path.size() && path[offset] == '/') {
        ++offset;
    }
    if (offset >= path.size()) {
        segment = {};
        next_offset = offset;
        return false;
    }

    const size_t segment_begin = offset;
    while (offset < path.size() && path[offset] != '/') {
        ++offset;
    }
    segment = path.substr(segment_begin, offset - segment_begin);
    next_offset = offset;
    return true;
}

RouteTrieNode* find_child_by_segment(RouteTrieNode* node, std::string_view segment)
{
    if (node == nullptr) {
        return nullptr;
    }
    for (auto& [key, child] : node->children) {
        if (key.size() == segment.size() &&
            std::string_view(key.data(), key.size()) == segment) {
            return child.get();
        }
    }
    return nullptr;
}

} // namespace

HttpRouteEntry* HttpRouter::search_route_path(RouteTrieNode* root,
                                              std::string_view path,
                                              RouteParams& params)
{
    params.clear();
    std::vector<std::string_view> paramValues;
    paramValues.reserve(8);
    return search_route_path_recursive(root, path, 0, paramValues, params);
}

HttpRouteEntry* HttpRouter::search_route_path_recursive(
    RouteTrieNode* node,
    std::string_view path,
    size_t offset,
    std::vector<std::string_view>& paramValues,
    RouteParams& params)
{
    if (node == nullptr) {
        return nullptr;
    }

    std::string_view segment;
    size_t next_offset = offset;
    const bool has_segment = next_route_segment(path, offset, segment, next_offset);
    if (!has_segment) {
        if (!node->isEnd) {
            return nullptr;
        }
        for (size_t i = 0; i < node->paramNames.size() && i < paramValues.size(); ++i) {
            const bool inserted = params.emplace(node->paramNames[i], paramValues[i]);
            if (!inserted) {
                return nullptr;
            }
        }
        return &node->handlers;
    }

    if (auto* exact = find_child_by_segment(node, segment)) {
        auto* result = search_route_path_recursive(exact, path, next_offset, paramValues, params);
        if (result != nullptr) {
            return result;
        }
    }

    if (auto paramIt = node->children.find(":param"); paramIt != node->children.end()) {
        paramValues.push_back(segment);
        auto* result = search_route_path_recursive(paramIt->second.get(),
                                                path,
                                                next_offset,
                                                paramValues,
                                                params);
        if (result != nullptr) {
            return result;
        }
        paramValues.pop_back();
    }

    if (auto wildcardIt = node->children.find("*"); wildcardIt != node->children.end()) {
        auto* result = search_route_path_recursive(wildcardIt->second.get(),
                                                path,
                                                next_offset,
                                                paramValues,
                                                params);
        if (result != nullptr) {
            return result;
        }
    }

    if (auto greedyIt = node->children.find("**"); greedyIt != node->children.end()) {
        auto* greedyNode = greedyIt->second.get();
        if (greedyNode != nullptr && greedyNode->isEnd) {
            return &greedyNode->handlers;
        }
    }

    return nullptr;
}

bool HttpRouter::validate_path(const std::string& path, std::string& error)
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
    auto segments = split_path(path);

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
            if (!std::isalpha(static_cast<unsigned char>(paramName[0])) && paramName[0] != '_') {
                error = "Parameter name '" + paramName + "' must start with a letter or underscore";
                return false;
            }

            // 检查参数名是否合法（只能包含字母、数字、下划线）
            for (char c : paramName) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
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
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.' && c != '~') {
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
            .build_move();
        auto writer = conn.get_writer();
        while (true) {
            auto send_result = co_await writer.send_response(response);
            if (!send_result || send_result.value()) break;
        }
        co_return;
    };

    auto handler = create_static_file_handler(routePrefix, dirPath, config, std::move(fallback));

    // 注册通配符路由：routePrefix/**
    std::string wildcardPath = routePrefix;
    if (wildcardPath.back() != '/') {
        wildcardPath += '/';
    }
    wildcardPath += "**";

    // 为 GET 和 HEAD 方法注册路由
    add_handler<HttpMethod::GET, HttpMethod::HEAD>(wildcardPath, handler);

    HTTP_LOG_INFO("[mount]", "dir={} route={}", dirPath, routePrefix);
}

void HttpRouter::mount_hardly(const std::string& routePrefix, const std::string& dirPath,
                             const StaticFileSetting& config)
{
    namespace fs = std::filesystem;

    // 验证目录是否存在
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        HTTP_LOG_ERROR("[mount-hard] [fail]", "dir={}", dirPath);
        return;
    }

    // 递归遍历目录并注册所有文件
    register_files_recursively(routePrefix, dirPath, config, "");

    HTTP_LOG_INFO("[mount-hard]", "dir={} route={}", dirPath, routePrefix);
}

void HttpRouter::try_files(const std::string& routePrefix,
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

    std::string normalizedPrefix = normalize_route_prefix(routePrefix);
    auto fallbackProxy = create_proxy_handler("/", upstreamHost, upstreamPort, mode);
    auto handler = create_static_file_handler(normalizedPrefix, dirPath, config, std::move(fallbackProxy));

    std::string wildcardPath = normalizedPrefix;
    if (wildcardPath.back() != '/') {
        wildcardPath += '/';
    }
    wildcardPath += "**";

    add_handler<HttpMethod::GET, HttpMethod::HEAD>(wildcardPath, handler);
    if (normalizedPrefix != "/") {
        add_handler<HttpMethod::GET, HttpMethod::HEAD>(normalizedPrefix, handler);
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

    std::string normalizedPrefix = normalize_route_prefix(routePrefix);
    auto handler = create_proxy_handler(normalizedPrefix, upstreamHost, upstreamPort, mode);

    std::string wildcardPath = normalizedPrefix == "/" ? "/**" : normalizedPrefix + "/**";
    add_handler<HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT,
               HttpMethod::PATCH, HttpMethod::DELETE, HttpMethod::HEAD,
               HttpMethod::OPTIONS>(wildcardPath, handler);

    // 让 /api 本身也命中代理，等价转发为上游 /
    if (normalizedPrefix != "/") {
        add_handler<HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT,
                   HttpMethod::PATCH, HttpMethod::DELETE, HttpMethod::HEAD,
                   HttpMethod::OPTIONS>(normalizedPrefix, handler);
    } else {
        // 统一语义：proxy("/") 同时作为本地路由未命中时的 fallback proxy
        if (!m_fallbackProxyHandlerState) {
            m_fallbackProxyHandlerState = std::make_shared<std::optional<HttpRouteHandler>>();
        }
        *m_fallbackProxyHandlerState = create_proxy_handler("/", upstreamHost, upstreamPort, mode);
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

bool HttpRouter::has_fallback_proxy() const
{
    return m_fallbackProxyHandlerState && m_fallbackProxyHandlerState->has_value();
}

HttpRouteHandler* HttpRouter::fallback_proxy_handler()
{
    if (!m_fallbackProxyHandlerState || !m_fallbackProxyHandlerState->has_value()) {
        return nullptr;
    }
    return &m_fallbackProxyHandlerState->value();
}

HttpRouteHandler HttpRouter::create_static_file_handler(const std::string& routePrefix,
                                                     const std::string& dirPath,
                                                     const StaticFileSetting& config,
                                                     HttpRouteHandler fallback_handler)
{
    namespace fs = std::filesystem;

    std::error_code canonical_dir_error;
    fs::path canonicalDir = fs::canonical(dirPath, canonical_dir_error);
    if (canonical_dir_error) {
        canonicalDir = fs::path(dirPath);
    }

    // 捕获 routePrefix、dirPath 和 config，返回一个协程处理器
    return [routePrefix, dirPath, canonicalDir, config, fallback_handler](HttpConn& conn, HttpRequest req) -> Task<void> {
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

        auto inspected = co_await StaticFileReader::inspect(fullPath.string());
        if (!inspected.has_value() || !inspected.value().has_value()) {
            if (fallback_handler) {
                co_await fallback_handler(conn, std::move(req));
                co_return;
            }
            // 文件不存在
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::NotFound_404)
                .body("404 Not Found")
                .build_move();
            auto writer = conn.get_writer();
            while (true) {
                auto send_result = co_await writer.send_response(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }

        StaticFileMetadata metadata = std::move(inspected.value().value());
        fs::path canonicalFile(metadata.canonical_path);

        // 检查文件是否在允许的目录内
        auto [dirIt, fileIt] = std::mismatch(canonicalDir.begin(), canonicalDir.end(),
                                              canonicalFile.begin());
        if (dirIt != canonicalDir.end()) {
            // 路径遍历攻击
            HTTP_LOG_WARN("[path] [traversal]", "request={}", requestPath);
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::Forbidden_403)
                .body("403 Forbidden")
                .build_move();
            auto writer = conn.get_writer();
            while (true) {
                auto send_result = co_await writer.send_response(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }

        const size_t fileSize = metadata.file_size;

        // 设置 Content-Type
        std::string extension = canonicalFile.extension().string();
        std::string ext = extension.empty() ? "" : extension.substr(1);
        std::string mimeType = MimeType::convert_to_mime_type(ext);
        HTTP_LOG_DEBUG("[static]",
                       "request={} file={} size={} mime={}",
                       requestPath,
                       canonicalFile.string(),
                       fileSize,
                       mimeType);
        co_await send_file_content(conn,
                                 req,
                                 canonicalFile.string(),
                                 fileSize,
                                 mimeType,
                                 config,
                                 metadata.last_modified);
        co_return;
    };
}

void HttpRouter::register_files_recursively(const std::string& routePrefix,
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
            register_files_recursively(routePrefix, dirPath, config, relativePath);
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
            auto handler = create_single_file_handler(filePath, config);

            // 注册路由
            add_handler<HttpMethod::GET, HttpMethod::HEAD>(routePath, handler);
        } else if (entry_error) {
            HTTP_LOG_ERROR("[file] [entry-fail]",
                           "path={} error={}",
                           entry.path().string(),
                           entry_error.message());
        }
    }
}

HttpRouteHandler HttpRouter::create_single_file_handler(const std::string& filePath,
                                                     const StaticFileSetting& config)
{
    // 捕获文件路径和配置
    return [filePath, config](HttpConn& conn, HttpRequest req) -> Task<void> {
        auto inspected = co_await StaticFileReader::inspect(filePath);
        if (!inspected.has_value() || !inspected.value().has_value()) {
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::NotFound_404)
                .body("404 Not Found")
                .build_move();
            auto writer = conn.get_writer();
            while (true) {
                auto send_result = co_await writer.send_response(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }

        StaticFileMetadata metadata = std::move(inspected.value().value());
        const size_t fileSize = metadata.file_size;

        // 设置 Content-Type
        std::filesystem::path path(metadata.canonical_path);
        std::string extension = path.extension().string();
        std::string ext = extension.empty() ? "" : extension.substr(1);
        std::string mimeType = MimeType::convert_to_mime_type(ext);

        // 使用配置的传输方式发送文件
        co_await send_file_content(conn,
                                 req,
                                 metadata.canonical_path,
                                 fileSize,
                                 mimeType,
                                 config,
                                 metadata.last_modified);
        co_return;
    };
}

HttpRouteHandler HttpRouter::create_proxy_handler(const std::string& routePrefix,
                                                const std::string& upstreamHost,
                                                uint16_t upstreamPort,
                                                ProxyMode mode)
{
    return [routePrefix, upstreamHost, mode, upstreamPort](HttpConn& conn, HttpRequest req) -> Task<void> {
        const std::string request_uri = req.header().uri();
        const std::string upstream_uri = rewrite_proxy_uri(routePrefix, request_uri);
        const std::string pool_key = build_upstream_key(upstreamHost, upstreamPort);
        const std::string upstream_connect_url = "http://" + upstreamHost + ":" +
                                                 std::to_string(upstreamPort) + "/";

        auto& headers = req.header().header_pairs();
        const std::string original_host = get_header_value_loose(headers, "Host");
        const std::string connection = get_header_value_loose(headers, "Connection");
        std::vector<std::string> hop_by_hop_tokens = split_connection_tokens(connection);

        ProxyMode effective_mode = mode;
        if (mode == ProxyMode::Http && is_likely_streaming_request(upstream_uri, headers)) {
            effective_mode = ProxyMode::Raw;
            HTTP_LOG_INFO("[proxy] [stream-upgrade]",
                          "uri={} upstream={}:{}",
                          upstream_uri,
                          upstreamHost,
                          upstreamPort);
        }

        remove_header_pair_loose(headers, "Connection");
        remove_header_pair_loose(headers, "Proxy-Connection");
        remove_header_pair_loose(headers, "Keep-Alive");
        remove_header_pair_loose(headers, "TE");
        remove_header_pair_loose(headers, "Trailer");
        remove_header_pair_loose(headers, "Transfer-Encoding");
        remove_header_pair_loose(headers, "Upgrade");
        for (const auto& token : hop_by_hop_tokens) {
            remove_header_pair_loose(headers, token);
        }

        apply_forward_headers(conn, headers, original_host);
        remove_header_pair_loose(headers, "Host");
        headers.add_header_pair("Host", upstreamHost + ":" + std::to_string(upstreamPort));
        headers.add_header_pair("Connection", effective_mode == ProxyMode::Raw ? "close" : "keep-alive");

        req.header().uri() = upstream_uri;

        if (effective_mode == ProxyMode::Raw) {
            auto client = std::make_unique<HttpClient>();
            bool connect_ok = false;
            std::string connect_err;
            co_await connect_proxy_upstream(*client, upstream_connect_url, connect_ok, connect_err);
            if (!connect_ok) {
                HTTP_LOG_ERROR("[proxy-raw] [connect-fail]", "error={}", connect_err);
                co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: connect upstream failed");
                co_return;
            }

            auto session_result = client->get_session();
            if (!session_result) {
                HTTP_LOG_ERROR("[proxy-raw] [session-fail]", "error={}", session_result.error().message());
                auto close_result = co_await client->close();
                if (!close_result) {
                    HTTP_LOG_WARN("[proxy] [client-close-fail]",
                                  "context=raw-session-fail error={}",
                                  close_result.error().message());
                }
                co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: upstream session failed");
                co_return;
            }
            auto& upstream_writer = session_result.value()->get_writer();
            bool send_ok = false;
            while (true) {
                auto send_result = co_await upstream_writer.send_request(req);
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
                auto close_result = co_await client->close();
                if (!close_result) {
                    HTTP_LOG_WARN("[proxy] [client-close-fail]",
                                  "context=raw-send-fail error={}",
                                  close_result.error().message());
                }
                co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: send upstream failed");
                co_return;
            }

            bool relay_ok = false;
            std::string relay_err;
            auto upstream_socket = client->socket();
            if (!upstream_socket) {
                HTTP_LOG_ERROR("[proxy-raw] [socket-fail]", "error={}", upstream_socket.error().message());
                auto close_result = co_await client->close();
                if (!close_result) {
                    HTTP_LOG_WARN("[proxy] [client-close-fail]",
                                  "context=raw-socket-fail error={}",
                                  close_result.error().message());
                }
                co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: upstream socket failed");
                co_return;
            }
            co_await relay_raw_upstream_to_downstream(upstream_socket.value().get(),
                                                  conn.get_socket(),
                                                  response_write_timeout_from_conn(conn),
                                                  relay_ok,
                                                  relay_err);
            if (!relay_ok && !relay_err.empty()) {
                HTTP_LOG_WARN("[proxy-raw] [relay-fail]", "error={}", relay_err);
            }

            auto close_result = co_await client->close();
            if (!close_result) {
                HTTP_LOG_WARN("[proxy] [client-close-fail]",
                              "context=raw-complete error={}",
                              close_result.error().message());
            }
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
            co_await connect_proxy_upstream(*client, upstream_connect_url, connect_ok, connect_err);
            if (!connect_ok) {
                HTTP_LOG_ERROR("[proxy] [connect-fail]", "error={}", connect_err);
                co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: connect upstream failed");
                co_return;
            }
        }

        HttpResponse upstream_response;
        bool request_ok = false;
        bool retried = false;

        while (!request_ok) {
            auto session_result = client->get_session();
            if (!session_result) {
                HTTP_LOG_ERROR("[proxy] [session-fail]", "error={}", session_result.error().message());
                auto close_result = co_await client->close();
                if (!close_result) {
                    HTTP_LOG_WARN("[proxy] [client-close-fail]",
                                  "context=session-fail error={}",
                                  close_result.error().message());
                }
                co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: upstream session failed");
                co_return;
            }
            auto& upstream_writer = session_result.value()->get_writer();
            bool send_ok = false;
            while (true) {
                auto send_result = co_await upstream_writer.send_request(req);
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
                auto close_result = co_await client->close();
                if (!close_result) {
                    HTTP_LOG_WARN("[proxy] [client-close-fail]",
                                  "context=send-fail error={}",
                                  close_result.error().message());
                }
                if (borrowed_from_pool && !retried) {
                    retried = true;
                    borrowed_from_pool = false;
                    client = std::make_unique<HttpClient>();
                    bool reconnect_ok = false;
                    std::string reconnect_err;
                    co_await connect_proxy_upstream(*client, upstream_connect_url, reconnect_ok, reconnect_err);
                    if (!reconnect_ok) {
                        HTTP_LOG_ERROR("[proxy] [reconnect-fail]", "error={}", reconnect_err);
                        co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                                "Bad Gateway: send upstream failed");
                        co_return;
                    }
                    continue;
                }

                co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: send upstream failed");
                co_return;
            }

            auto& upstream_reader = session_result.value()->get_reader();
            upstream_response.reset();
            const bool is_head_request = req.header().method() == HttpMethod::HEAD;
            bool recv_ok = false;
            while (true) {
                // HEAD 响应没有 body，只等响应头即可；get_response 会按 Content-Length
                // 等待不存在的 body 而挂起。
                std::expected<bool, HttpError> recv_result =
                    is_head_request
                        ? co_await upstream_reader.get_response_header(upstream_response.header())
                        : co_await upstream_reader.get_response(upstream_response);
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
                auto close_result = co_await client->close();
                if (!close_result) {
                    HTTP_LOG_WARN("[proxy] [client-close-fail]",
                                  "context=recv-fail error={}",
                                  close_result.error().message());
                }
                if (borrowed_from_pool && !retried) {
                    retried = true;
                    borrowed_from_pool = false;
                    client = std::make_unique<HttpClient>();
                    bool reconnect_ok = false;
                    std::string reconnect_err;
                    co_await connect_proxy_upstream(*client, upstream_connect_url, reconnect_ok, reconnect_err);
                    if (!reconnect_ok) {
                        HTTP_LOG_ERROR("[proxy] [reconnect-fail]", "error={}", reconnect_err);
                        co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                                "Bad Gateway: recv upstream failed");
                        co_return;
                    }
                    continue;
                }

                co_await send_proxy_error(conn, HttpStatusCode::BadGateway_502,
                                        "Bad Gateway: recv upstream failed");
                co_return;
            }

            request_ok = true;
        }

        auto downstream_writer = conn.get_writer();
        bool downstream_ok = false;
        while (true) {
            auto forward_result = co_await downstream_writer.send_response(upstream_response);
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
                             upstream_response.header().is_keep_alive() &&
                             !upstream_response.header().is_connection_close();

        if (keep_upstream) {
            auto& idle = g_proxyClientPools[pool_key];
            if (idle.size() < kProxyMaxIdleConnectionsPerUpstream) {
                idle.push_back(std::move(client));
            } else if (client) {
                auto close_result = co_await client->close();
                if (!close_result) {
                    HTTP_LOG_WARN("[proxy] [client-close-fail]",
                                  "context=pool-full error={}",
                                  close_result.error().message());
                }
            }
        } else if (client) {
            auto close_result = co_await client->close();
            if (!close_result) {
                HTTP_LOG_WARN("[proxy] [client-close-fail]",
                              "context=not-keepalive error={}",
                              close_result.error().message());
            }
        }

        co_return;
    };
}

// ==================== 文件传输实现 ====================

Task<void> HttpRouter::send_file_content(HttpConn& conn,
                                       HttpRequest& req,
                                       const std::string& filePath,
                                       size_t fileSize,
                                       const std::string& mimeType,
                                       const StaticFileSetting& config,
                                       std::time_t lastModified)
{
    // 生成稳定 ETag（mtime + size + inode/路径哈希）
    if (lastModified == 0) {
        // fallback: 使用当前时间，避免空值
        lastModified = std::time(nullptr);
    }

    const bool enableEtag = config.is_enable_e_tag();
    std::string etag;
    if (enableEtag) {
        etag = ETagGenerator::generate_strong(filePath, fileSize, lastModified);
    }
    std::string lastModifiedStr = ETagGenerator::format_http_date(lastModified);

    auto writer = conn.get_writer();
    const bool isHeadRequest = req.header().method() == HttpMethod::HEAD;

    // 1. 处理 If-Match (前置条件)
    std::string ifMatch = req.header().header_pairs().get_value("If-Match");
    if (enableEtag && !ifMatch.empty() && !ETagGenerator::match_if_match(etag, ifMatch)) {
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::PreconditionFailed_412)
            .header("ETag", etag)
            .header("Last-Modified", lastModifiedStr)
            .build_move();
        while (true) {
            auto send_result = co_await writer.send_response(response);
            if (!send_result || send_result.value()) break;
        }
        co_return;
    }

    // 2. 处理 If-None-Match (ETag 条件请求)
    std::string ifNoneMatch = req.header().header_pairs().get_value("If-None-Match");
    if (enableEtag && ETagGenerator::match_if_none_match(etag, ifNoneMatch)) {
        // ETag 匹配，返回 304 Not Modified
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::NotModified_304)
            .header("ETag", etag)
            .header("Last-Modified", lastModifiedStr)
            .build_move();
        while (true) {
            auto send_result = co_await writer.send_response(response);
            if (!send_result || send_result.value()) break;
        }
        co_return;
    }

    // 3. 处理 Range 请求
    std::string rangeHeader = req.header().header_pairs().get_value("Range");
    bool hasRange = !rangeHeader.empty();
    RangeParseResult rangeResult;

    if (hasRange) {
        // 解析 Range 请求
        rangeResult = HttpRangeParser::parse(rangeHeader, fileSize);

        // 3. 处理 If-Range 条件请求
        std::string ifRangeHeader = req.header().header_pairs().get_value("If-Range");
        if (!ifRangeHeader.empty()) {
            // 检查 If-Range 条件
            if (!HttpRangeParser::check_if_range(ifRangeHeader, etag, lastModified)) {
                // If-Range 条件不满足，忽略 Range 请求，返回完整文件
                hasRange = false;
                rangeResult = RangeParseResult();
            }
        }

        // 验证 Range 是否有效
        if (hasRange && !rangeResult.is_valid()) {
            // Range 无效，返回 416 Range Not Satisfiable
            const std::string body = "416 Range Not Satisfiable";
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::RangeNotSatisfiable_416)
                .header("Content-Range", "bytes */" + std::to_string(fileSize))
                .header("Content-Length", std::to_string(body.size()))
                .body(body)
                .build_move();
            if (isHeadRequest) {
                HttpResponseHeader header = response.header().clone();
                while (true) {
                    auto send_result = co_await writer.send_header(std::move(header));
                    if (!send_result || send_result.value()) break;
                }
                co_return;
            }
            while (true) {
                auto send_result = co_await writer.send_response(response);
                if (!send_result || send_result.value()) break;
            }
            co_return;
        }
    }

    // 4. 根据是否有 Range 请求决定响应方式
    if (hasRange && rangeResult.is_valid()) {
        // 处理 Range 请求
        if (rangeResult.type == RangeType::SINGLE_RANGE) {
            // 单范围请求
            co_await send_single_range(conn, req, filePath, fileSize, mimeType, etag, lastModifiedStr, rangeResult.ranges[0], config);
        } else if (rangeResult.type == RangeType::MULTIPLE_RANGES) {
            // 多范围请求 (multipart/byteranges)
            co_await send_multiple_ranges(conn, req, filePath, fileSize, mimeType, etag, lastModifiedStr, rangeResult, config);
        }
        co_return;
    }

    // 5. 发送完整文件（无 Range 请求或 Range 无效）
    // 根据配置决定传输模式
    FileTransferMode mode = config.decide_transfer_mode(fileSize);
    // 构建响应头
    Http1_1ResponseBuilder responseBuilder;
    responseBuilder
        .status(HttpStatusCode::OK_200)
        .header("Content-Type", mimeType)
        .header("Last-Modified", lastModifiedStr)
        .header("Accept-Ranges", "bytes");
    if (enableEtag) {
        responseBuilder.header("ETag", etag);
    }
    auto response = responseBuilder.build_move();
    HTTP_LOG_DEBUG("[send]",
                   "file={} size={} mode={}",
                   filePath,
                   fileSize,
                   static_cast<int>(mode));

    if (isHeadRequest) {
        response.header().header_pairs().add_header_pair("Content-Length", std::to_string(fileSize));
        HttpResponseHeader header = response.header().clone();
        while (true) {
            auto result = co_await writer.send_header(std::move(header));
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
            auto awaited_read = co_await StaticFileReader::read_all(filePath, fileSize);
            if (!awaited_read.has_value()) {
                HTTP_LOG_ERROR("[file] [async-read-await-fail]",
                               "path={} error={}",
                               filePath,
                               awaited_read.error().message());
                auto error_response = Http1_1ResponseBuilder()
                    .status(HttpStatusCode::InternalServerError_500)
                    .body("500 Internal Server Error")
                    .build_move();
                auto send_result = co_await writer.send(error_response.to_string());
                if (!send_result) {
                    HTTP_LOG_ERROR("[send] [read-await-error-fail]",
                                   "error={}",
                                   send_result.error().message());
                }
                co_return;
            }

            StaticFileReadResult file_read = std::move(awaited_read.value());
            if (!file_read.has_value()) {
                const StaticFileReadError& read_error = file_read.error();
                HTTP_LOG_ERROR("[file] [async-read-fail]",
                               "path={} code={} errno={} close_errno={} expected={} actual={}",
                               filePath,
                               static_file_read_error_name(read_error.code),
                               read_error.error_number,
                               read_error.close_error_number,
                               read_error.expected_bytes,
                               read_error.actual_bytes);
                auto error_response = Http1_1ResponseBuilder()
                    .status(HttpStatusCode::InternalServerError_500)
                    .body("500 Internal Server Error")
                    .build_move();
                auto send_result = co_await writer.send(error_response.to_string());
                if (!send_result) {
                    HTTP_LOG_ERROR("[send] [read-error-fail]",
                                   "error={}",
                                   send_result.error().message());
                }
                co_return;
            }

            std::string content = std::move(file_read.value());
            response.set_body_str(std::move(content));

            while (true) {
                auto result = co_await writer.send_response(response);
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
            response.header().header_pairs().add_header_pair("Transfer-Encoding", "chunked");

            // 发送响应头（只发送头部，不包含 body）
            HttpResponseHeader header = response.header().clone();
            auto headerResult = co_await writer.send_header(std::move(header));
            if (!headerResult) {
                HTTP_LOG_ERROR("[send] [header-fail]",
                               "error={}",
                               headerResult.error().message());
                co_return;
            }

            // 分块读取并发送
            const size_t chunkSize = config.get_chunk_size();
            size_t offset = 0;
            bool hasError = false;

            auto opened = co_await StaticFileReader::open(filePath);
            if (!opened.has_value()) {
                HTTP_LOG_ERROR("[file] [open-await-fail] [chunk]",
                               "path={} error={}",
                               filePath,
                               opened.error().message());
                co_await writer.send_chunk("", true);
                co_return;
            }
            StaticFileSessionResult session_result = std::move(opened.value());
            if (!session_result.has_value()) {
                const auto& open_error = session_result.error();
                HTTP_LOG_ERROR("[file] [open-fail] [chunk]",
                               "path={} code={} errno={}",
                               filePath,
                               static_file_read_error_name(open_error.code),
                               open_error.error_number);
                co_await writer.send_chunk("", true);
                co_return;
            }
            StaticFileSession session = std::move(session_result.value());

            while (offset < fileSize) {
                const size_t toRead = std::min(fileSize - offset, chunkSize);
                auto read_result = co_await session.read_at(offset, toRead);
                if (!read_result.has_value()) {
                    HTTP_LOG_ERROR("[file] [read-await-fail] [chunk]",
                                   "path={} error={}",
                                   filePath,
                                   read_result.error().message());
                    hasError = true;
                    break;
                }
                StaticFileReadResult chunk_result = std::move(read_result.value());
                if (!chunk_result.has_value()) {
                    const auto& read_error = chunk_result.error();
                    HTTP_LOG_ERROR("[file] [read-fail] [chunk]",
                                   "path={} code={} errno={} expected={} actual={}",
                                   filePath,
                                   static_file_read_error_name(read_error.code),
                                   read_error.error_number,
                                   read_error.expected_bytes,
                                   read_error.actual_bytes);
                    hasError = true;
                    break;
                }

                std::string chunk = std::move(chunk_result.value());
                if (chunk.empty()) {
                    HTTP_LOG_ERROR("[file] [short-read] [chunk]", "path={}", filePath);
                    hasError = true;
                    break;
                }

                auto result = co_await writer.send_chunk(std::move(chunk), false);
                if (!result) {
                    HTTP_LOG_ERROR("[send] [chunk-fail]",
                                   "error={}",
                                   result.error().message());
                    hasError = true;
                    break;
                }
                offset += toRead;
            }

            // 发送最后一个空 chunk
            if (!hasError) {
                co_await writer.send_chunk("", true);
            }

            break;
        }

        case FileTransferMode::SENDFILE: {
            // SendFile 模式：使用零拷贝 sendfile 系统调用
            response.header().header_pairs().add_header_pair("Content-Length", std::to_string(fileSize));

            // 发送响应头（只发送头部，不包含 body）
            HttpResponseHeader header = response.header().clone();
            auto headerResult = co_await writer.send_header(std::move(header));
            if (!headerResult) {
                HTTP_LOG_ERROR("[send] [header-fail]",
                               "error={}",
                               headerResult.error().message());
                co_return;
            }

            auto opened = co_await StaticFileReader::open_for_sendfile(filePath);
            if (!opened.has_value()) {
                HTTP_LOG_ERROR("[file] [open-await-fail] [sendfile]",
                               "path={} error={}",
                               filePath,
                               opened.error().message());
                co_return;
            }
            StaticFileDescriptorResult descriptor_result = std::move(opened.value());
            if (!descriptor_result.has_value()) {
                HTTP_LOG_ERROR("[file] [open-fail] [sendfile]",
                               "path={} code={} errno={}",
                               filePath,
                               static_file_read_error_name(descriptor_result.error().code),
                               descriptor_result.error().error_number);
                co_return;
            }
            FileDescriptor fd = std::move(descriptor_result.value());

            // 使用 sendfile 零拷贝发送文件内容
            off_t offset = 0;
            size_t remaining = fileSize;
            size_t sendfileChunkSize = config.get_send_file_chunk_size();
            const auto response_write_timeout = response_write_timeout_from_conn(conn);

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
            // AUTO 模式应该在 decide_transfer_mode 中已经被转换为具体模式
            HTTP_LOG_ERROR("[mode] [auto] [invalid]", "file={}", filePath);
            break;
    }

    co_return;
}

// ==================== Range 请求处理实现 ====================

Task<void> HttpRouter::send_single_range(HttpConn& conn,
                                       HttpRequest& req,
                                       const std::string& filePath,
                                       size_t fileSize,
                                       const std::string& mimeType,
                                       const std::string& etag,
                                       const std::string& lastModified,
                                       const HttpRange& range,
                                       const StaticFileSetting& config)
{
    auto writer = conn.get_writer();

    // 构建 206 Partial Content 响应
    Http1_1ResponseBuilder responseBuilder;
    responseBuilder
        .status(HttpStatusCode::PartialContent_206)
        .header("Content-Type", mimeType)
        .header("Content-Range", HttpRangeParser::make_content_range(range, fileSize))
        .header("Content-Length", std::to_string(range.length))
        .header("Last-Modified", lastModified)
        .header("Accept-Ranges", "bytes");
    if (!etag.empty()) {
        responseBuilder.header("ETag", etag);
    }
    auto response = responseBuilder.build_move();

    // 发送响应头
    HttpResponseHeader header = response.header().clone();
    auto headerResult = co_await writer.send_header(std::move(header));
    if (!headerResult) {
        HTTP_LOG_ERROR("[send] [header-fail]",
                       "error={}",
                       headerResult.error().message());
        co_return;
    }

    if (req.header().method() == HttpMethod::HEAD) {
        co_return;
    }

    // 根据配置决定传输模式
    FileTransferMode mode = config.decide_transfer_mode(range.length);

    if (mode == FileTransferMode::SENDFILE) {
        auto opened = co_await StaticFileReader::open_for_sendfile(filePath);
        if (!opened.has_value()) {
            HTTP_LOG_ERROR("[file] [open-await-fail] [range]",
                           "path={} error={}",
                           filePath,
                           opened.error().message());
            co_return;
        }
        StaticFileDescriptorResult descriptor_result = std::move(opened.value());
        if (!descriptor_result.has_value()) {
            HTTP_LOG_ERROR("[file] [open-fail] [range]",
                           "path={} code={} errno={}",
                           filePath,
                           static_file_read_error_name(descriptor_result.error().code),
                           descriptor_result.error().error_number);
            co_return;
        }
        FileDescriptor fd = std::move(descriptor_result.value());

        // 使用 sendfile 零拷贝发送范围内容
        off_t offset = range.start;
        size_t remaining = range.length;
        size_t sendfileChunkSize = config.get_send_file_chunk_size();
        const auto response_write_timeout = response_write_timeout_from_conn(conn);

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
        // 使用统一 reader 读取范围内容，避免在 IO scheduler 上执行同步文件操作。
        const size_t chunkSize = config.get_chunk_size();
        size_t offset = range.start;
        size_t remaining = range.length;

        auto opened = co_await StaticFileReader::open(filePath);
        if (!opened.has_value()) {
            HTTP_LOG_ERROR("[file] [open-await-fail] [range]",
                           "path={} error={}",
                           filePath,
                           opened.error().message());
            co_return;
        }
        StaticFileSessionResult session_result = std::move(opened.value());
        if (!session_result.has_value()) {
            const auto& open_error = session_result.error();
            HTTP_LOG_ERROR("[file] [open-fail] [range]",
                           "path={} code={} errno={}",
                           filePath,
                           static_file_read_error_name(open_error.code),
                           open_error.error_number);
            co_return;
        }
        StaticFileSession session = std::move(session_result.value());

        while (remaining > 0) {
            const size_t toRead = std::min(remaining, chunkSize);
            auto read_result = co_await session.read_at(offset, toRead);
            if (!read_result.has_value()) {
                HTTP_LOG_ERROR("[file] [read-await-fail] [range]",
                               "path={} error={}",
                               filePath,
                               read_result.error().message());
                break;
            }
            StaticFileReadResult chunk_result = std::move(read_result.value());
            if (!chunk_result.has_value()) {
                const auto& read_error = chunk_result.error();
                HTTP_LOG_ERROR("[file] [read-fail] [range]",
                               "path={} code={} errno={} expected={} actual={}",
                               filePath,
                               static_file_read_error_name(read_error.code),
                               read_error.error_number,
                               read_error.expected_bytes,
                               read_error.actual_bytes);
                break;
            }

            std::string chunk = std::move(chunk_result.value());
            if (chunk.empty()) {
                HTTP_LOG_ERROR("[file] [short-read] [range]", "path={}", filePath);
                break;
            }
            auto result = co_await writer.send(std::move(chunk));
            if (!result) {
                HTTP_LOG_ERROR("[send] [chunk-fail]",
                               "error={}",
                               result.error().message());
                break;
            }

            offset += toRead;
            remaining -= toRead;
        }
    }

    co_return;
}

Task<void> HttpRouter::send_multiple_ranges(HttpConn& conn,
                                          HttpRequest& req,
                                          const std::string& filePath,
                                          size_t fileSize,
                                          const std::string& mimeType,
                                          const std::string& etag,
                                          const std::string& lastModified,
                                          const RangeParseResult& rangeResult,
                                          const StaticFileSetting& config)
{
    auto writer = conn.get_writer();

    // 构建 206 Partial Content 响应（multipart/byteranges）
    std::string boundary = rangeResult.boundary;
    Http1_1ResponseBuilder responseBuilder;
    responseBuilder
        .status(HttpStatusCode::PartialContent_206)
        .header("Content-Type", "multipart/byteranges; boundary=" + boundary)
        .header("Last-Modified", lastModified)
        .header("Accept-Ranges", "bytes");
    if (!etag.empty()) {
        responseBuilder.header("ETag", etag);
    }
    auto response = responseBuilder.build_move();

    // 计算总长度（包括所有边界和头部）
    size_t totalLength = 0;
    for (const auto& range : rangeResult.ranges) {
        // 边界行
        totalLength += 2 + boundary.length() + 2;  // "--boundary\r\n"
        // Content-Type 头
        totalLength += 14 + mimeType.length() + 2;  // "Content-Type: \r\n"
        // Content-Range 头
        std::string contentRange = HttpRangeParser::make_content_range(range, fileSize);
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

    response.header().header_pairs().add_header_pair("Content-Length", std::to_string(totalLength));

    // 发送响应头
    HttpResponseHeader header = response.header().clone();
    auto headerResult = co_await writer.send_header(std::move(header));
    if (!headerResult) {
        HTTP_LOG_ERROR("[send] [header-fail]",
                       "error={}",
                       headerResult.error().message());
        co_return;
    }

    if (req.header().method() == HttpMethod::HEAD) {
        co_return;
    }

    auto opened = co_await StaticFileReader::open(filePath);
    if (!opened.has_value()) {
        HTTP_LOG_ERROR("[file] [open-await-fail] [range-multi]",
                       "path={} error={}",
                       filePath,
                       opened.error().message());
        co_return;
    }
    StaticFileSessionResult session_result = std::move(opened.value());
    if (!session_result.has_value()) {
        const auto& open_error = session_result.error();
        HTTP_LOG_ERROR("[file] [open-fail] [range-multi]",
                       "path={} code={} errno={}",
                       filePath,
                       static_file_read_error_name(open_error.code),
                       open_error.error_number);
        co_return;
    }
    StaticFileSession session = std::move(session_result.value());

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
            HttpRangeParser::make_content_range(range, fileSize) + "\r\n";
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

        // 读取并发送范围内容
        const size_t chunkSize = config.get_chunk_size();
        size_t offset = range.start;
        size_t remaining = range.length;

        while (remaining > 0) {
            const size_t toRead = std::min(remaining, chunkSize);
            auto read_result = co_await session.read_at(offset, toRead);
            if (!read_result.has_value()) {
                HTTP_LOG_ERROR("[file] [read-await-fail] [range-multi]",
                               "path={} error={}",
                               filePath,
                               read_result.error().message());
                co_return;
            }
            StaticFileReadResult chunk_result = std::move(read_result.value());
            if (!chunk_result.has_value()) {
                const auto& read_error = chunk_result.error();
                HTTP_LOG_ERROR("[file] [read-fail] [range-multi]",
                               "path={} code={} errno={} expected={} actual={}",
                               filePath,
                               static_file_read_error_name(read_error.code),
                               read_error.error_number,
                               read_error.expected_bytes,
                               read_error.actual_bytes);
                break;
            }

            std::string chunk = std::move(chunk_result.value());
            if (chunk.empty()) {
                HTTP_LOG_ERROR("[file] [short-read] [range-multi]", "path={}", filePath);
                co_return;
            }
            auto result = co_await writer.send(std::move(chunk));
            if (!result) {
                HTTP_LOG_ERROR("[send] [chunk-fail]",
                               "error={}",
                               result.error().message());
                co_return;
            }

            offset += toRead;
            remaining -= toRead;
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
