/**
 * @file ws_upgrade.cc
 * @brief WebSocket 握手升级处理实现
 */

#include <galay/cpp/galay-ws/server/ws_upgrade.h>
#include <galay/cpp/galay-utils/encoding/base64.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace galay::websocket
{

using namespace galay::utils;

// 简单的 SHA-1 实现（用于 WebSocket 握手）
static void sha1(const unsigned char* data, size_t length, unsigned char* hash)
{
    // 初始化哈希值
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    // 预处理消息
    size_t ml = length * 8;
    size_t padded_length = ((length + 8) / 64 + 1) * 64;
    unsigned char* padded = new unsigned char[padded_length]();
    std::memcpy(padded, data, length);
    padded[length] = 0x80;

    // 添加长度（大端序）
    for (int i = 0; i < 8; i++) {
        padded[padded_length - 1 - i] = (ml >> (i * 8)) & 0xFF;
    }

    // 处理每个 512 位块
    for (size_t chunk = 0; chunk < padded_length; chunk += 64) {
        uint32_t w[80];

        // 将块分成 16 个 32 位大端字
        for (int i = 0; i < 16; i++) {
            w[i] = (padded[chunk + i * 4] << 24) |
                   (padded[chunk + i * 4 + 1] << 16) |
                   (padded[chunk + i * 4 + 2] << 8) |
                   (padded[chunk + i * 4 + 3]);
        }

        // 扩展到 80 个字
        for (int i = 16; i < 80; i++) {
            uint32_t temp = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (temp << 1) | (temp >> 31);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        // 主循环
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    delete[] padded;

    // 输出哈希值（大端序）
    for (int i = 0; i < 4; i++) {
        hash[i] = (h0 >> (24 - i * 8)) & 0xFF;
        hash[i + 4] = (h1 >> (24 - i * 8)) & 0xFF;
        hash[i + 8] = (h2 >> (24 - i * 8)) & 0xFF;
        hash[i + 12] = (h3 >> (24 - i * 8)) & 0xFF;
        hash[i + 16] = (h4 >> (24 - i * 8)) & 0xFF;
    }
}

std::string WsUpgrade::generateAcceptKey(const std::string& key)
{
    // 1. 拼接 key 和魔术字符串
    std::string combined = key + WEBSOCKET_MAGIC_STRING;

    // 2. 计算 SHA-1 哈希
    unsigned char hash[20];
    sha1(reinterpret_cast<const unsigned char*>(combined.c_str()),
         combined.length(),
         hash);

    // 3. 使用 galay-utils 的 Base64 编码
    return Base64Util::Base64Encode(hash, 20);
}

std::pair<bool, std::string> WsUpgrade::validateUpgradeRequest(HttpRequest& request)
{
    // 检查 HTTP 方法必须是 GET
    if (request.header().method() != HttpMethod::GET) {
        return {false, "WebSocket upgrade requires GET method"};
    }

    // 检查 Connection 头
    if (!request.header().headerPairs().hasKey("Connection")) {
        return {false, "Missing Connection header"};
    }

    std::string connection = request.header().headerPairs().getValue("Connection");
    std::transform(connection.begin(), connection.end(), connection.begin(), ::tolower);
    if (connection.find("upgrade") == std::string::npos) {
        return {false, "Connection header must contain 'Upgrade'"};
    }

    // 检查 Upgrade 头
    if (!request.header().headerPairs().hasKey("Upgrade")) {
        return {false, "Missing Upgrade header"};
    }

    std::string upgrade = request.header().headerPairs().getValue("Upgrade");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
    if (upgrade != "websocket") {
        return {false, "Upgrade header must be 'websocket'"};
    }

    // 检查 Sec-WebSocket-Version
    if (!request.header().headerPairs().hasKey("Sec-WebSocket-Version")) {
        return {false, "Missing Sec-WebSocket-Version header"};
    }

    std::string version = request.header().headerPairs().getValue("Sec-WebSocket-Version");
    if (version != "13") {
        return {false, "Only WebSocket version 13 is supported"};
    }

    // 检查 Sec-WebSocket-Key
    if (!request.header().headerPairs().hasKey("Sec-WebSocket-Key")) {
        return {false, "Missing Sec-WebSocket-Key header"};
    }

    std::string key = request.header().headerPairs().getValue("Sec-WebSocket-Key");
    if (key.empty()) {
        return {false, "Sec-WebSocket-Key cannot be empty"};
    }

    return {true, ""};
}

HttpResponse WsUpgrade::createUpgradeResponse(
    const std::string& accept_key,
    const std::string& subprotocol)
{
    auto response = Http1_1ResponseBuilder()
        .status(HttpStatusCode::SwitchingProtocol_101)
        .header("Upgrade", "websocket")
        .header("Connection", "Upgrade")
        .header("Sec-WebSocket-Accept", accept_key)
        .buildMove();

    if (!subprotocol.empty()) {
        response.header().headerPairs().addHeaderPair("Sec-WebSocket-Protocol", subprotocol);
    }

    return response;
}

WsUpgradeResult WsUpgrade::handleUpgrade(HttpRequest& request)
{
    WsUpgradeResult result;

    // 验证请求
    auto [valid, error_msg] = validateUpgradeRequest(request);
    if (!valid) {
        result.success = false;
        result.error_message = error_msg;

        // 返回 400 Bad Request
        result.response = Http1_1ResponseBuilder::badRequest()
            .text("WebSocket upgrade failed: " + error_msg)
            .build();

        return result;
    }

    // 获取 Sec-WebSocket-Key
    std::string key = request.header().headerPairs().getValue("Sec-WebSocket-Key");

    // 生成 Sec-WebSocket-Accept
    std::string accept_key = generateAcceptKey(key);

    // 检查是否有子协议请求
    std::string subprotocol;
    if (request.header().headerPairs().hasKey("Sec-WebSocket-Protocol")) {
        subprotocol = request.header().headerPairs().getValue("Sec-WebSocket-Protocol");
        // TODO: 验证子协议是否支持
    }

    // 创建 101 响应
    result.success = true;
    result.response = createUpgradeResponse(accept_key, subprotocol);

    return result;
}

} // namespace galay::websocket
