/**
 * @file test_reader_writer_server.cc
 * @brief HTTP Reader and Writer 测试 - 服务器端
 */

#include <iostream>
#include <atomic>
#include "http/kernel/http_reader.h"
#include "http/kernel/http_writer.h"
#include "http/protoc/http_request.h"
#include "http/protoc/http_response.h"
#include "http/builder/http_builder.h"
#include "kernel/async/tcp_socket.h"
#include <utils/cache/ring_buffer.hpp>
#include "kernel/kernel/runtime.h"

#ifdef USE_KQUEUE
#include "kernel/kernel/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "kernel/kernel/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "kernel/kernel/io_uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::kernel;
using ::galay::utils::RingBuffer;
using namespace galay::async;

std::atomic<int> g_request_count{0};

// Echo服务器
Task<void> echoServer() {

    TcpSocket listener;

    // 设置选项
    auto optResult = listener.option().handleReuseAddr();
    if (!optResult) {
        co_return;
    }

    optResult = listener.option().handleNonBlock();
    if (!optResult) {
        co_return;
    }

    // 绑定地址
    Host bindHost(IPType::IPV4, "127.0.0.1", 9999);
    auto bindResult = listener.bind(bindHost);
    if (!bindResult) {
        co_return;
    }

    // 监听
    auto listenResult = listener.listen(128);
    if (!listenResult) {
        co_return;
    }


    while (true) {
        // 接受连接
        Host clientHost;
        auto acceptResult = co_await listener.accept(&clientHost);
        if (!acceptResult) {
            continue;
        }


        // 创建客户端socket
        TcpSocket client(acceptResult.value());
        client.option().handleNonBlock();

        // 创建RingBuffer和HttpReader
        RingBuffer ringBuffer(8192);
        HttpReaderSetting readerSetting;
        HttpWriterSetting writerSetting;
        HttpReader reader(ringBuffer, readerSetting, client);
        HttpWriter writer(writerSetting, client);

        // 读取HTTP请求
        HttpRequest request;
        bool requestComplete = false;

        while (!requestComplete) {
            // 异步读取数据（getRequest 内部会自动调用 readv）
            auto result = co_await reader.getRequest(request);

            if (!result) {
                auto& error = result.error();
                if (error.code() == kConnectionClose) {
                } else {
                }
                break;
            }

            requestComplete = result.value();
        }

        if (requestComplete) {
            g_request_count++;

            // 测试不同的发送方式
            int testCase = g_request_count.load() % 3;

            if (testCase == 0) {
                // 方式1: 使用 sendResponse 发送完整响应
                std::string body = "Echo: " + request.header().uri() + "\n";
                body += "Request #" + std::to_string(g_request_count.load());
                auto response = Http1_1ResponseBuilder()
                    .status(HttpStatusCode::OK_200)
                    .header("Content-Type", "text/plain")
                    .header("Server", "galay-http-test/1.0")
                    .body(std::move(body))
                    .buildMove();

                auto sendResult = co_await writer.sendResponse(response);
                if (!sendResult) {
                } else {
                }
            } else if (testCase == 1) {
                // 方式2: 使用 sendHeader + send(string) 分离发送
                HttpResponseHeader respHeader;
                respHeader.version() = HttpVersion::HttpVersion_1_1;
                respHeader.code() = HttpStatusCode::OK_200;
                respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
                respHeader.headerPairs().addHeaderPair("Server", "galay-http-test/1.0");

                std::string body = "Echo: " + request.header().uri() + "\n";
                body += "Request #" + std::to_string(g_request_count.load());
                respHeader.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

                // 发送头部
                auto headerResult = co_await writer.sendHeader(std::move(respHeader));
                if (!headerResult) {
                } else {
                    // 发送body
                    auto bodyResult = co_await writer.send(std::move(body));
                    if (bodyResult) {
                    } else {
                    }
                }
            } else {
                // 方式3: 使用 send(buffer, length) 发送原始数据
                HttpResponseHeader respHeader;
                respHeader.version() = HttpVersion::HttpVersion_1_1;
                respHeader.code() = HttpStatusCode::OK_200;
                respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
                respHeader.headerPairs().addHeaderPair("Server", "galay-http-test/1.0");

                std::string body = "Echo: " + request.header().uri() + "\n";
                body += "Request #" + std::to_string(g_request_count.load());
                respHeader.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

                std::string headerStr = respHeader.toString();

                // 发送头部（原始数据）
                auto headerResult = co_await writer.send(headerStr.data(), headerStr.size());
                if (!headerResult) {
                } else {
                    // 发送body（原始数据）
                    auto bodyResult = co_await writer.send(body.data(), body.size());
                    if (bodyResult) {
                    } else {
                    }
                }
            }
        }

        co_await client.close();
    }

    co_await listener.close();
    co_return;
}

int main() {

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    Runtime rt = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    rt.start();

    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        rt.stop();
        return 1;
    }

    // 启动服务器
    scheduleTask(scheduler, echoServer());


    // 保持运行
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    rt.stop();
#else
    return 1;
#endif

    return 0;
}
