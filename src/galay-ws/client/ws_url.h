/**
 * @file ws_url.h
 * @brief WebSocket URL 解析与密钥生成
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 WsUrl 结构体用于解析 ws:// 和 wss:// 格式的 URL，
 *          以及 generateWebSocketKey 函数用于生成 Sec-WebSocket-Key。
 */

#ifndef GALAY_WS_URL_H
#define GALAY_WS_URL_H

#include <galay-utils/encoding/base64.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <random>
#include <string>
#include <system_error>

namespace galay::websocket
{

/**
 * @brief WebSocket URL 解析结果
 * @details 存储 ws:// 或 wss:// URL 的各个组成部分
 */
struct WsUrl {
    std::string scheme;         ///< 协议方案（ws 或 wss）
    std::string host;           ///< 主机名
    int port = 0;               ///< 端口号
    std::string path;           ///< 路径
    bool is_secure = false;     ///< 是否为安全连接（wss）

    /**
     * @brief 解析 WebSocket URL
     * @param url 形如 `ws://host[:port][/path]` 或 `wss://host[:port][/path]` 的 URL 字符串
     * @return 解析成功返回 WsUrl，格式不合法返回 std::nullopt
     */
    static std::optional<WsUrl> parse(const std::string& url) {
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos || scheme_end == 0) {
            return std::nullopt;
        }

        WsUrl result;
        result.scheme = url.substr(0, scheme_end);
        std::transform(result.scheme.begin(), result.scheme.end(), result.scheme.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (result.scheme != "ws" && result.scheme != "wss") {
            return std::nullopt;
        }
        result.is_secure = (result.scheme == "wss");

        const size_t authority_begin = scheme_end + 3;
        const size_t path_begin = url.find('/', authority_begin);
        const std::string authority = path_begin == std::string::npos
            ? url.substr(authority_begin)
            : url.substr(authority_begin, path_begin - authority_begin);
        if (authority.empty()) {
            return std::nullopt;
        }

        const size_t colon = authority.find(':');
        if (colon == std::string::npos) {
            result.host = authority;
            result.port = result.is_secure ? 443 : 80;
        } else {
            result.host = authority.substr(0, colon);
            const std::string port_text = authority.substr(colon + 1);
            if (result.host.empty() || port_text.empty()) {
                return std::nullopt;
            }
            int port = 0;
            const auto* begin = port_text.data();
            const auto* end = begin + port_text.size();
            auto [ptr, ec] = std::from_chars(begin, end, port);
            if (ec != std::errc{} || ptr != end || port <= 0 || port > 65535) {
                return std::nullopt;
            }
            result.port = port;
        }

        if (result.host.empty()) {
            return std::nullopt;
        }

        if (path_begin == std::string::npos) {
            result.path = "/";
        } else {
            result.path = url.substr(path_begin);
        }

        return result;
    }
};

/**
 * @brief 生成 WebSocket 握手所需的 Sec-WebSocket-Key
 * @return Base64 编码的 16 字节随机密钥
 */
inline std::string generateWebSocketKey() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    unsigned char random_bytes[16];
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = static_cast<unsigned char>(dis(gen));
    }

    return galay::utils::Base64Util::Base64Encode(random_bytes, 16);
}

} // namespace galay::websocket

#endif // GALAY_WS_URL_H
