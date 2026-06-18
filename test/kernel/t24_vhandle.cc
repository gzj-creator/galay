/**
 * @file t24_vhandle.cc
 * @brief 用途：验证虚拟句柄完成回调路径能够正确驱动协程恢复。
 * 关键覆盖点：完成通知、句柄状态切换、恢复时序与结果透传。
 * 通过条件：虚拟句柄完成路径按预期触发，测试返回 0。
 */

#include "galay-kernel/core/task.h"
#include "galay-kernel/core/io_scheduler.hpp"
#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/common/host.hpp"
#include "test/stdout_log.h"
#include "result_writer.h"
#include <string_view>

#ifdef USE_IOURING
#include "galay-kernel/core/uring_scheduler.h"
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include "galay-kernel/core/epoll_scheduler.h"
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include "galay-kernel/core/kqueue_scheduler.h"
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <atomic>
#include <thread>

using namespace galay::kernel;

std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_total{0};

/**
 * @brief 继承 RecvAwaitable，重写 handleComplete
 *
 * 前 m_reject_count 次调用返回 false（模拟"还没准备好"），
 * 之后调用基类 handleComplete 正常处理。
 *
 * RecvAwaitable 继承自 RecvIOContext，handleComplete 定义在 RecvIOContext 上。
 */
struct CountingRecvAwaitable : public RecvAwaitable {
    int m_reject_count;          ///< 需要拒绝的次数
    std::atomic<int> m_call_count{0}; ///< 实际被调用次数

    CountingRecvAwaitable(IOController* controller, char* buffer, size_t length, int reject_count)
        : RecvAwaitable(controller, buffer, length)
        , m_reject_count(reject_count)
    {}

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
        int n = m_call_count.fetch_add(1, std::memory_order_relaxed);
        if (n < m_reject_count) {
            LogInfo("[CountingRecv] handleComplete called #{}, returning false (reject)", n + 1);
            return false;  // 拒绝，让调度器重新注册
        }
        LogInfo("[CountingRecv] handleComplete called #{}, accepting", n + 1);
        return RecvIOContext::handleComplete(cqe, handle);
    }
#else
    bool handleComplete(GHandle handle) override {
        int n = m_call_count.fetch_add(1, std::memory_order_relaxed);
        if (n < m_reject_count) {
            LogInfo("[CountingRecv] handleComplete called #{}, returning false (reject)", n + 1);
            return false;  // 拒绝，让调度器重新注册
        }
        LogInfo("[CountingRecv] handleComplete called #{}, accepting", n + 1);
        return RecvIOContext::handleComplete(handle);
    }
#endif
};

// 服务端：用 CountingRecvAwaitable 接收数据
Task<void> test_server([[maybe_unused]] IOScheduler* scheduler, int listen_fd, int reject_count)
{
    g_total++;

    // Accept
    IOController listen_ctrl(GHandle{.fd = listen_fd});
    Host clientHost;
    AcceptAwaitable acceptAw(&listen_ctrl, &clientHost);
    auto acceptResult = co_await acceptAw;
    if (!acceptResult) {
        LogError("[Server] Accept failed: {}", acceptResult.error().message());
        g_failed++;
        co_return;
    }

    int client_fd = acceptResult.value().fd;
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    LogInfo("[Server] Client connected, fd={}", client_fd);

    // 用自定义的 CountingRecvAwaitable 接收
    IOController ctrl(GHandle{.fd = client_fd});
    char buffer[256]{};
    CountingRecvAwaitable recvAw(&ctrl, buffer, sizeof(buffer) - 1, reject_count);

    auto result = co_await recvAw;

    int total_calls = recvAw.m_call_count.load();
    LogInfo("[Server] handleComplete was called {} times (expected >= {})",
            total_calls, reject_count + 1);

    if (total_calls >= reject_count + 1) {
        LogInfo("[Server] PASS: handleComplete was re-invoked after returning false");
        if (result) {
            LogInfo("[Server] Received data: {}", std::string_view(buffer, result.value()));
        }
        g_passed++;
    } else {
        LogError("[Server] FAIL: expected at least {} calls, got {}",
                 reject_count + 1, total_calls);
        g_failed++;
    }

    close(client_fd);
    co_return;
}

// 客户端：多次发送数据，触发服务端多次事件
Task<void> test_client([[maybe_unused]] IOScheduler* scheduler, const char* ip, int port, int send_count)
{
    g_total++;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LogError("[Client] Socket creation failed");
        g_failed++;
        co_return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    IOController ctrl(GHandle{.fd = fd});

    // Connect
    ConnectAwaitable connectAw(&ctrl, Host(IPType::IPV4, ip, port));
    auto connResult = co_await connectAw;
    if (!connResult) {
        LogError("[Client] Connect failed: {}", connResult.error().message());
        close(fd);
        g_failed++;
        co_return;
    }
    LogInfo("[Client] Connected");

    // 多次发送小数据，每次间隔一点时间，确保服务端多次收到事件
    for (int i = 0; i < send_count; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::string msg = "ping-" + std::to_string(i);
        ssize_t n = send(fd, msg.c_str(), msg.size(), 0);
        if (n > 0) {
            LogInfo("[Client] Sent: {}", msg);
        } else {
            LogError("[Client] Send failed: {}", strerror(errno));
        }
    }

    g_passed++;
    close(fd);
    co_return;
}

int main()
{
    LogInfo("========================================");
    LogInfo("Virtual handleComplete Re-registration Test");
    LogInfo("========================================");

#ifdef USE_IOURING
    LogInfo("Backend: io_uring");
#elif defined(USE_EPOLL)
    LogInfo("Backend: epoll");
#elif defined(USE_KQUEUE)
    LogInfo("Backend: kqueue");
#endif

    const int PORT = 19999;
    const int REJECT_COUNT = 3;  // handleComplete 前3次返回 false

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LogError("Failed to create listen socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LogError("Bind failed: {}", strerror(errno));
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 128) < 0) {
        LogError("Listen failed: {}", strerror(errno));
        close(listen_fd);
        return 1;
    }

    LogInfo("Server listening on port {}", PORT);

    TestScheduler scheduler;
    scheduler.start();

    // 启动服务端（reject_count=3，前3次 handleComplete 返回 false）
    scheduleTask(scheduler, test_server(&scheduler, listen_fd, REJECT_COUNT));

    // 启动客户端（发送 reject_count+1 次数据，确保触发足够多的事件）
    scheduleTask(scheduler, test_client(&scheduler, "127.0.0.1", PORT, REJECT_COUNT + 1));

    std::this_thread::sleep_for(std::chrono::seconds(3));

    scheduler.stop();
    close(listen_fd);

    // 写入测试结果
    galay::test::TestResultWriter writer("test_virtual_handle_complete");
    for (int i = 0; i < g_total.load(); ++i) writer.addTest();
    for (int i = 0; i < g_passed.load(); ++i) writer.addPassed();
    for (int i = 0; i < g_failed.load(); ++i) writer.addFailed();
    writer.writeResult();

    LogInfo("========================================");
    LogInfo("Test Results: Total={}, Passed={}, Failed={}",
            g_total.load(), g_passed.load(), g_failed.load());
    LogInfo("========================================");

    return g_failed > 0 ? 1 : 0;
}
