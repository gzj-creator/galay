#include <galay/cpp/galay-mcp/v2/server/http_server.h>

#include <iostream>

namespace galay::mcp::v2 {

struct McpHttpServerTestAccess {
    static bool run()
    {
        McpHttpServer server("127.0.0.1", 0, 1, 0);
        server.m_admission.store(0);
        SubscriptionFilter filter;
        filter.toolsListChanged = true;
        filter.resourceSubscriptions.emplace_back("mem://owned");
        auto state = std::make_unique<McpHttpServer::Subscription>(42, std::move(filter));
        auto& subscription = *state;
        auto registered = server.submit(McpHttpServer::Command(
            McpHttpServer::CommandKind::Register, {}, std::move(state)));
        if (!registered || !server.processCommands() || !subscription.registered.isReady()) return false;

        // These paths must work even when no new coroutine can be allocated.
        kernel::detail::setFrameAllocationFailureForTesting(true);
        auto notified = server.notifyToolsListChanged();
        auto uri = std::string("mem://owned");
        auto resource = server.notifyResourceUpdated(uri);
        uri.assign("mem://changed");
        const bool processed = server.processCommands();
        kernel::detail::setFrameAllocationFailureForTesting(false);
        if (!notified || !resource || !processed) return false;
        auto first = subscription.events.tryRecv();
        auto second = subscription.events.tryRecv();
        if (!first || !second || first->find("notifications/tools/list_changed") == std::string::npos ||
            second->find("mem://owned") == std::string::npos ||
            second->find("mem://changed") != std::string::npos) return false;

        // Subscriber overflow remains bounded; accepted commands are not receipts.
        for (int i = 0; i != 257; ++i) {
            if (!server.notifyToolsListChanged()) return false;
        }
        while (server.processCommands()) {}
        std::size_t received = 0;
        while (subscription.events.tryRecv()) ++received;
        if (received != 256) return false;

        if (!server.m_commands.close()) return false;
        auto rejected = server.notifyToolsListChanged();
        if (rejected || rejected.error().code() != McpErrorCode::Overload) return false;
        // Release cannot require another command, allocation, or coroutine.
        kernel::detail::setFrameAllocationFailureForTesting(true);
        subscription.finished.store(true, std::memory_order_release);
        server.reapSubscriptions();
        kernel::detail::setFrameAllocationFailureForTesting(false);
        if (server.m_subscriptions != nullptr) return false;
        server.m_admission.store(McpHttpServer::kAdmissionClosed);
        rejected = server.notifyToolsListChanged();
        return !rejected && rejected.error().code() == McpErrorCode::ConnectionClosed;
    }
};

} // namespace galay::mcp::v2

int main()
{
    if (!galay::mcp::v2::McpHttpServerTestAccess::run()) {
        std::cerr << "MCP owner command boundary failed" << std::endl;
        return 1;
    }
    std::cout << "MCP owner commands PASS" << std::endl;
}
