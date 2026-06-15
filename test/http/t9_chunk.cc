/**
 * @file test_chunked_server.cc
 * @brief HTTP Chunked编码完整测试 - 服务器端
 */

#include <iostream>
#include <atomic>
#include "galay-http/kernel/http_reader.h"
#include "galay-http/kernel/http_writer.h"
#include "galay-http/protoc/http_request.h"
#include "galay-http/protoc/http_response.h"
#include "galay-http/builder/http_builder.h"
#include "galay-kernel/async/tcp_socket.h"
#include <galay-utils/cache/ring_buffer.hpp>
#include "galay-kernel/core/runtime.h"

#ifdef USE_KQUEUE
#include "galay-kernel/core/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "galay-kernel/core/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "galay-kernel/core/io_uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::kernel;
using ::galay::utils::RingBuffer;
using namespace galay::async;

std::atomic<int> g_request_count{0};

// 处理客户端连接
Task<void> handleClient(TcpSocket client, Host clientHost) {

    client.option().handleNonBlock();

    // 创建RingBuffer和HttpReader/Writer
    RingBuffer ringBuffer(8192);
    HttpReaderSetting readerSetting;
    HttpWriterSetting writerSetting;
    HttpReader reader(ringBuffer, readerSetting, client);
    HttpWriter writer(writerSetting, client);

    // 读取HTTP请求头
    HttpRequest request;
    bool requestHeaderComplete = false;

    while (!requestHeaderComplete) {
        auto result = co_await reader.getRequest(request);

        if (!result) {
            auto& error = result.error();
            if (error.code() == kConnectionClose) {
            } else {
            }
            co_await client.close();
            co_return;
        }

        requestHeaderComplete = result.value();
    }

    g_request_count++;

    const std::string& requestBody = request.bodyStr();

    if (!requestBody.empty()) {

        // `getRequest()` 当前会在 chunked 请求完成时把 body 聚合完毕，
        // 因此这里直接基于聚合后的正文回发 chunked 响应。
        HttpResponseHeader respHeader;
        respHeader.version() = HttpVersion::HttpVersion_1_1;
        respHeader.code() = HttpStatusCode::OK_200;
        respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
        respHeader.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
        respHeader.headerPairs().addHeaderPair("Server", "galay-http-chunked-test/1.0");

        auto headerResult = co_await writer.sendHeader(std::move(respHeader));
        if (!headerResult) {
            co_await client.close();
            co_return;
        }

        std::string chunk1 = "Decoded body bytes: " + std::to_string(requestBody.size()) + "\n";
        auto chunk1Result = co_await writer.sendChunk(chunk1, false);
        if (!chunk1Result) {
            co_await client.close();
            co_return;
        }

        std::string chunk2 = "Echo: ";
        auto chunk2Result = co_await writer.sendChunk(chunk2, false);
        if (!chunk2Result) {
            co_await client.close();
            co_return;
        }

        auto chunk3Result = co_await writer.sendChunk(requestBody, false);
        if (!chunk3Result) {
            co_await client.close();
            co_return;
        }

        std::string emptyChunk;
        auto lastChunkResult = co_await writer.sendChunk(emptyChunk, true);
        if (!lastChunkResult) {
            co_await client.close();
            co_return;
        }

    } else {
        // 非chunked请求

        // 发送简单响应
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::OK_200)
            .header("Content-Type", "text/plain")
            .header("Server", "galay-http-chunked-test/1.0")
            .body("Non-chunked request received\n")
            .buildMove();

        auto sendResult = co_await writer.sendResponse(response);
        if (!sendResult) {
        } else {
        }
    }

    co_await client.close();
}

// Chunk测试服务器
Task<void> chunkedTestServer() {

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

        // 创建RingBuffer和HttpReader/Writer
        RingBuffer ringBuffer(8192);
        HttpReaderSetting readerSetting;
        HttpWriterSetting writerSetting;
        HttpReader reader(ringBuffer, readerSetting, client);
        HttpWriter writer(writerSetting, client);

        // 读取HTTP请求头
        HttpRequest request;
        bool requestHeaderComplete = false;

        while (!requestHeaderComplete) {
            auto result = co_await reader.getRequest(request);

            if (!result) {
                auto& error = result.error();
                if (error.code() == kConnectionClose) {
                } else {
                }
                co_await client.close();
                break;
            }

            requestHeaderComplete = result.value();
        }

        if (!requestHeaderComplete) {
            co_await client.close();
            continue;
        }

        g_request_count++;

        const std::string& requestBody = request.bodyStr();

        if (!requestBody.empty()) {

            HttpResponseHeader respHeader;
            respHeader.version() = HttpVersion::HttpVersion_1_1;
            respHeader.code() = HttpStatusCode::OK_200;
            respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
            respHeader.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
            respHeader.headerPairs().addHeaderPair("Server", "galay-http-chunked-test/1.0");

            auto headerResult = co_await writer.sendHeader(std::move(respHeader));
            if (!headerResult) {
                co_await client.close();
                continue;
            }

            std::string chunk1 = "Decoded body bytes: " + std::to_string(requestBody.size()) + "\n";
            auto chunk1Result = co_await writer.sendChunk(chunk1, false);
            if (!chunk1Result) {
                co_await client.close();
                continue;
            }

            std::string chunk2 = "Echo: ";
            auto chunk2Result = co_await writer.sendChunk(chunk2, false);
            if (!chunk2Result) {
                co_await client.close();
                continue;
            }

            auto chunk3Result = co_await writer.sendChunk(requestBody, false);
            if (!chunk3Result) {
                co_await client.close();
                continue;
            }

            std::string emptyChunk;
            auto lastChunkResult = co_await writer.sendChunk(emptyChunk, true);
            if (!lastChunkResult) {
                co_await client.close();
                continue;
            }

        } else {
            // 非chunked请求

            // 发送简单响应
            auto response = Http1_1ResponseBuilder()
                .status(HttpStatusCode::OK_200)
                .header("Content-Type", "text/plain")
                .header("Server", "galay-http-chunked-test/1.0")
                .body("Non-chunked request received\n")
                .buildMove();

            auto sendResult = co_await writer.sendResponse(response);
            if (!sendResult) {
            } else {
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
    scheduleTask(scheduler, chunkedTestServer());


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
