/**
 * @file test_websocket_client.cc
 * @brief WebSocket 客户端测试程序
 */

#include <string>
#include <galay/cpp/galay-http/client/http_client.h>
#include <galay/cpp/galay-ws/kernel/ws_conn.h>
#include <galay/cpp/galay-http/builder/http_builder.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/io_uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

/**
 * @brief WebSocket 客户端测试
 */
Task<void> test_web_socket_client(IOScheduler* scheduler) {

    // 创建 socket 并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        co_return;
    }


    // 创建 HTTP 客户端
    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

    // 构建 WebSocket 升级请求
    auto request = Http1_1RequestBuilder::get("/ws")
        .header("Host", "localhost:8080")
        .header("Connection", "Upgrade")
        .header("Upgrade", "websocket")
        .header("Sec-WebSocket-Version", "13")
        .header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==")
        .build();


    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();

    // 发送升级请求
    auto writer = session.getWriter();
    auto send_result = co_await writer.sendRequest(request);
    if (!send_result) {
        co_await client.close();
        co_return;
    }

    // 读取升级响应
    auto reader = session.getReader();
    HttpResponse response;
    bool complete = false;
    while (!complete) {
        auto read_result = co_await reader.getResponse(response);
        if (!read_result) {
            co_await client.close();
            co_return;
        }
        complete = read_result.value();
    }

    // 检查升级是否成功
    if (response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
        co_await client.close();
        co_return;
    }


    // 升级到 WebSocket 连接
    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 1024 * 1024;
    reader_setting.max_message_size = 10 * 1024 * 1024;

    WsWriterSetting writer_setting = WsWriterSetting::byClient();

    auto client_socket = client.socket();
    if (!client_socket) {
        co_await client.close();
        co_return;
    }

    WsConn ws_conn(
        std::move(client_socket.value().get()),
        false  // is_server = false (客户端)
    );

    auto ws_reader = ws_conn.getReader(reader_setting);
    auto ws_writer = ws_conn.getWriter(writer_setting);

    // 读取欢迎消息
    std::string welcome_msg;
    WsOpcode welcome_opcode;
    auto welcome_result = co_await ws_reader.getMessage(welcome_msg, welcome_opcode);
    if (welcome_result.has_value() && welcome_result.value()) {
    }

    // 发送测试消息
    for (int i = 1; i <= 5; i++) {
        std::string test_msg = "Test message " + std::to_string(i);

        WsFrame frame;
        frame.header.fin = true;
        frame.header.opcode = WsOpcode::Text;
        frame.header.mask = true;  // 客户端必须设置 mask
        frame.payload = test_msg;
        frame.header.payload_length = test_msg.size();

        auto send_result = co_await ws_writer.sendFrame(frame);
        if (!send_result) {
            break;
        }

        // 读取回显消息
        std::string echo_msg;
        WsOpcode echo_opcode;
        auto echo_result = co_await ws_reader.getMessage(echo_msg, echo_opcode);
        if (echo_result.has_value() && echo_result.value()) {
        } else {
            break;
        }

        // 等待一下
        co_await galay::kernel::sleep(std::chrono::seconds(1));
    }

    // 关闭连接
    co_await ws_conn.close();

    co_return;
}

int main() {

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    galay::kernel::Runtime rt;
    rt.start();

    // 获取 IO 调度器并启动测试协程
    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        return 1;
    }

    scheduleTask(scheduler, test_web_socket_client(scheduler));

    // 等待测试完成（这里简单地等待一段时间）
    std::this_thread::sleep_for(std::chrono::seconds(30));

    rt.stop();
    return 0;
#else
    return 1;
#endif
}
