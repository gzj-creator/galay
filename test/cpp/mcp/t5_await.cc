/**
 * @brief 锁定 McpClient 的 HTTP awaitable surface 与底层 HttpClient 保持一致。
 */

#include <galay/cpp/galay-mcp/client/client.h>
#include <galay/cpp/galay-http/client/http_client.h>

#include <concepts>
#include <string>
#include <utility>

using galay::http::HttpClient;
using galay::mcp::McpClient;

static_assert(requires(McpClient& client, const std::string& url) {
    {
        client.connect(url)
    } -> std::same_as<decltype(std::declval<HttpClient&>().connect(std::declval<const std::string&>()))>;
    {
        client.disconnectAsync()
    } -> std::same_as<decltype(std::declval<HttpClient&>().close())>;
});

int main()
{
    return 0;
}
