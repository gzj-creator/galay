/**
 * @file ws_upgrade.h
 * @brief WebSocket 握手升级处理
 */

#ifndef GALAY_WS_UPGRADE_H
#define GALAY_WS_UPGRADE_H

#include "../../galay-http/protoc/http_request.h"
#include "../../galay-http/protoc/http_response.h"
#include "../../galay-http/builder/http_builder.h"
#include <string>
#include <optional>

namespace galay::websocket
{

using namespace galay::http;

/**
 * @brief WebSocket 握手升级结果
 */
struct WsUpgradeResult
{
    bool success = false;
    std::string error_message;
    HttpResponse response;
};

/**
 * @brief WebSocket 握手升级处理器
 * @details 处理 HTTP 到 WebSocket 的协议升级
 */
class WsUpgrade
{
public:
    /**
     * @brief 验证并处理 WebSocket 升级请求
     * @param request HTTP 请求
     * @return WsUpgradeResult 升级结果
     *
     * @details 验证以下内容：
     * - Connection: Upgrade
     * - Upgrade: websocket
     * - Sec-WebSocket-Version: 13
     * - Sec-WebSocket-Key 存在
     */
    static WsUpgradeResult handleUpgrade(HttpRequest& request);

    /**
     * @brief 生成 Sec-WebSocket-Accept 值
     * @param key Sec-WebSocket-Key 值
     * @return 计算后的 Accept 值
     *
     * @details 算法：
     * 1. 将 key 与魔术字符串 "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" 拼接
     * 2. 计算 SHA-1 哈希
     * 3. Base64 编码
     */
    static std::string generateAcceptKey(const std::string& key);

    /**
     * @brief 验证请求头是否包含必需的 WebSocket 升级字段
     * @param request HTTP 请求
     * @return 验证结果和错误信息
     */
    static std::pair<bool, std::string> validateUpgradeRequest(HttpRequest& request);

    /**
     * @brief 创建 101 Switching Protocols 响应
     * @param accept_key Sec-WebSocket-Accept 值
     * @param subprotocol 可选的子协议
     * @return HTTP 响应
     */
    static HttpResponse createUpgradeResponse(
        const std::string& accept_key,
        const std::string& subprotocol = "");

private:
    static constexpr const char* WEBSOCKET_MAGIC_STRING =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
};

} // namespace galay::websocket

#endif // GALAY_WS_UPGRADE_H
