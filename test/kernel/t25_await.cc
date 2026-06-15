/**
 * @file t25_await.cc
 * @brief 用途：验证显式 SequenceAwaitable 步骤组合在当前内核中的接入与执行语义。
 * 关键覆盖点：显式 `SequenceStep` 编排、状态传递、调度器集成、SEND/RECV 双阶段组合。
 * 通过条件：显式 sequence 流程断言全部成立，测试返回 0。
 */

#include "galay-kernel/core/task.h"
#include "galay-kernel/core/io_scheduler.hpp"
#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/common/host.hpp"
#include "test/stdout_log.h"
#include "result_writer.h"

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
#include <string>

using namespace galay::kernel;

std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_total{0};

using SequenceResult = std::expected<size_t, IOError>;
using SequenceT = SequenceAwaitable<SequenceResult, 4>;

struct SendThenRecvFlow {
    void onSend(SequenceOps<SequenceResult, 4>& ops, SendIOContext&);
    void onRecv(SequenceOps<SequenceResult, 4>& ops, RecvIOContext& recv_ctx);

    using SendStep = SequenceStep<SequenceResult, 4, SendThenRecvFlow, SendIOContext, &SendThenRecvFlow::onSend>;
    using RecvStep = SequenceStep<SequenceResult, 4, SendThenRecvFlow, RecvIOContext, &SendThenRecvFlow::onRecv>;

    SendThenRecvFlow(const char* send_data, size_t send_len, char* recv_buf, size_t recv_buf_len)
        : send(this, send_data, send_len)
        , recv(this, recv_buf, recv_buf_len) {}

    auto make(IOController* controller) -> SequenceT {
        SequenceT awaitable(controller);
        awaitable.queue(send);
        return awaitable;
    }

    SendStep send;
    RecvStep recv;
};

inline void SendThenRecvFlow::onSend(SequenceOps<SequenceResult, 4>& ops, SendIOContext&) {
    ops.queue(recv);
}

inline void SendThenRecvFlow::onRecv(SequenceOps<SequenceResult, 4>& ops, RecvIOContext& recv_ctx) {
    ops.complete(std::move(recv_ctx.m_result));
}

Task<void> serverCoroutine([[maybe_unused]] IOScheduler* scheduler, int listen_fd)
{
    g_total++;

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

    IOController ctrl(GHandle{.fd = client_fd});
    const std::string greeting = "hello";
    char recvBuf[256]{};

    SendThenRecvFlow flow(greeting.c_str(), greeting.size(), recvBuf, sizeof(recvBuf) - 1);
    auto sequence = flow.make(&ctrl);

    LogInfo("[Server] co_await explicit SequenceAwaitable...");
    auto recvResult = co_await sequence;
    LogInfo("[Server] SequenceAwaitable completed");

    bool sendOk = false;
    if (flow.send.m_result.has_value()) {
        LogInfo("[Server] SEND: {} bytes", flow.send.m_result.value());
        sendOk = (flow.send.m_result.value() == greeting.size());
    } else {
        LogError("[Server] SEND failed: {}", flow.send.m_result.error().message());
    }

    bool recvOk = false;
    if (recvResult.has_value()) {
        std::string received(recvBuf, recvResult.value());
        LogInfo("[Server] RECV: \"{}\"", received);
        recvOk = (received == "world");
    } else {
        LogError("[Server] RECV failed: {}", recvResult.error().message());
    }

    if (sendOk && recvOk) {
        LogInfo("[Server] PASS");
        g_passed++;
    } else {
        LogError("[Server] FAIL (sendOk={}, recvOk={})", sendOk, recvOk);
        g_failed++;
    }

    close(client_fd);
    co_return;
}

Task<void> clientCoroutine([[maybe_unused]] IOScheduler* scheduler, const char* ip, int port)
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

    IOController ctrl(GHandle{.fd = fd});

    ConnectAwaitable connectAw(&ctrl, Host(IPType::IPV4, ip, port));
    auto connResult = co_await connectAw;
    if (!connResult) {
        LogError("[Client] Connect failed: {}", connResult.error().message());
        close(fd);
        g_failed++;
        co_return;
    }

    char buf[256]{};
    RecvAwaitable recvAw(&ctrl, buf, sizeof(buf) - 1);
    auto recvResult = co_await recvAw;
    if (!recvResult) {
        LogError("[Client] Recv failed: {}", recvResult.error().message());
        close(fd);
        g_failed++;
        co_return;
    }

    const std::string reply = "world";
    SendAwaitable sendAw(&ctrl, reply.c_str(), reply.size());
    auto sendResult = co_await sendAw;
    if (!sendResult) {
        LogError("[Client] Send failed: {}", sendResult.error().message());
        close(fd);
        g_failed++;
        co_return;
    }

    if (std::string(buf, recvResult.value()) == "hello") {
        g_passed++;
    } else {
        g_failed++;
    }

    close(fd);
    co_return;
}

int main()
{
    LogInfo("========================================");
    LogInfo("SequenceAwaitable Explicit Step Test");
    LogInfo("  SendThenRecvFlow: SEND + RECV");
    LogInfo("========================================");

    const int PORT = 20030;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
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
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 128) < 0) {
        close(listen_fd);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();
    scheduleTask(scheduler, serverCoroutine(&scheduler, listen_fd));
    scheduleTask(scheduler, clientCoroutine(&scheduler, "127.0.0.1", PORT));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    scheduler.stop();
    close(listen_fd);

    const bool ok = g_failed.load() == 0 && g_passed.load() == g_total.load();
    galay::test::TestResultWriter writer("T25");
    writer.addTest();
    if (ok) {
        writer.addPassed();
    } else {
        writer.addFailed();
    }
    writer.writeResult();
    return ok ? 0 : 1;
}
