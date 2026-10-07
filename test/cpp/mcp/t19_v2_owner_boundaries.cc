#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-mcp/v2/client/client.h>
#include <galay/cpp/galay-mcp/v2/server/http_server.h>

#include <chrono>
#include <future>
#include <iostream>
#include <type_traits>

static_assert(std::is_same_v<decltype(std::declval<galay::mcp::v2::McpHttpServer&>()
                                        .notify_tools_list_changed()),
                             std::expected<void, galay::mcp::McpError>>);

namespace {

galay::kernel::Task<void> connection_failure(
    galay::kernel::Runtime* runtime, std::promise<bool>* done)
{
    using namespace galay::mcp;
    v2::McpHttpClient client(*runtime, "invalid-url");
    std::expected<std::vector<v2::Tool>, McpError> tools;
    const auto requested = co_await client.list_tools(tools);
    // A failed request must release the ordinary owner-only request lease.
    const auto retried = co_await client.list_tools(tools);
    std::expected<v2::SubscriptionFilter, McpError> filter;
    const auto listened = co_await client.listen({}, [](std::string) { return true; }, filter);
    const auto cause = galay::kernel::IOError(galay::kernel::kParamInvalid, 0).message();
    done->set_value(requested && retried && listened && !tools && !filter &&
        tools.error().code() == McpErrorCode::ConnectionFailed &&
        filter.error().code() == McpErrorCode::ConnectionFailed &&
        tools.error().details() == cause && filter.error().details() == cause);
}

galay::kernel::Task<void> wrong_owner(
    galay::mcp::v2::McpHttpClient* client, std::promise<bool>* done)
{
    std::expected<std::vector<galay::mcp::v2::Tool>, galay::mcp::McpError> result;
    const auto completed = co_await client->list_tools(result);
    auto connected = client->connect();
    auto closed = client->close();
    done->set_value(completed.has_value() && !result &&
                    !connected && !closed &&
                    connected.error().code() == galay::mcp::McpErrorCode::InvalidParams &&
                    closed.error().code() == galay::mcp::McpErrorCode::InvalidParams &&
                    result.error().code() == galay::mcp::McpErrorCode::InvalidParams);
}

} // namespace

int main()
{
    using namespace std::chrono_literals;
    using namespace galay::kernel;
    galay::mcp::v2::McpHttpServer stoppedServer("127.0.0.1", 0, 1, 0);
    const auto rejected = stoppedServer.notify_tools_list_changed();
    if (rejected || rejected.error().code() != galay::mcp::McpErrorCode::ConnectionClosed) return 3;
    Runtime runtime = RuntimeBuilder().io_scheduler_count(2).parallel_scheduler_count(0).build();
    if (!runtime.start()) return 1;
    galay::mcp::v2::McpHttpClient client(runtime, "http://127.0.0.1:1/mcp");
    std::promise<bool> done;
    auto ready = done.get_future();
    const bool submitted = schedule_task(runtime.get_io_scheduler(1), wrong_owner(&client, &done));
    const bool passed = submitted && ready.wait_for(5s) == std::future_status::ready && ready.get();
    std::promise<bool> failedConnection;
    auto failureReady = failedConnection.get_future();
    const bool failureSubmitted = schedule_task(client.owner(), connection_failure(&runtime, &failedConnection));
    const bool failurePassed = failureSubmitted &&
        failureReady.wait_for(5s) == std::future_status::ready && failureReady.get();
    runtime.stop();
    if (!failurePassed) {
        std::cerr << "client lost connection error cause or request lease" << std::endl;
        return 4;
    }
    if (!passed) {
        std::cerr << "client accessed transport from a non-owner scheduler" << std::endl;
        return 2;
    }
    std::cout << "MCP v2 owner boundaries PASS" << std::endl;
    return 0;
}
