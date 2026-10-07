/**
 * @file test_chunked_client.cc
 * @brief HTTP Chunked编码完整测试 - 客户端
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <galay/cpp/galay-http/kernel/http_reader.h>
#include <galay/cpp/galay-http/kernel/http_writer.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/protoc/http_response.h>
#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-utils/buffer/ring_buffer.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::kernel;
using ::galay::utils::RingBuffer;
using namespace galay::async;

std::atomic<bool> g_test_done{false};
std::atomic<bool> g_test_passed{false};

// 发送chunked请求并接收响应
Task<void> send_chunked_request() {
    auto fail = [] {
        g_test_passed = false;
        g_test_done = true;
    };


    AsyncTcpSocket client;

    // 设置非阻塞
    auto optResult = client.option().handle_non_block();
    if (!optResult) {
        fail();
        co_return;
    }

    // 连接到服务器
    Host serverHost(IPType::IPV4, "127.0.0.1", 9999);
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        fail();
        co_return;
    }


    // 创建RingBuffer和HttpReader/Writer
    RingBuffer ringBuffer(8192);
    HttpReaderSetting readerSetting;
    HttpWriterSetting writerSetting;
    HttpReader reader(ringBuffer, readerSetting, client);
    HttpWriter writer(writerSetting, client);

    // 构造chunked请求头
    HttpRequestHeader reqHeader;
    reqHeader.method() = HttpMethod::POST;
    reqHeader.uri() = "/test";
    reqHeader.version() = HttpVersion::HttpVersion_1_1;
    reqHeader.header_pairs().add_header_pair("Host", "127.0.0.1:9999");
    reqHeader.header_pairs().add_header_pair("Transfer-Encoding", "chunked");
    reqHeader.header_pairs().add_header_pair("User-Agent", "galay-http-chunked-client/1.0");

    // 发送请求头
    auto headerResult = co_await writer.send_header(std::move(reqHeader));
    if (!headerResult) {
        co_await client.close();
        fail();
        co_return;
    }

    // 发送多个chunk
    std::string chunk1 = "Hello ";
    auto chunk1Result = co_await writer.send_chunk(chunk1, false);
    if (!chunk1Result) {
        co_await client.close();
        fail();
        co_return;
    }

    std::string chunk2 = "from ";
    auto chunk2Result = co_await writer.send_chunk(chunk2, false);
    if (!chunk2Result) {
        co_await client.close();
        fail();
        co_return;
    }

    std::string chunk3 = "chunked ";
    auto chunk3Result = co_await writer.send_chunk(chunk3, false);
    if (!chunk3Result) {
        co_await client.close();
        fail();
        co_return;
    }

    std::string chunk4 = "client!";
    auto chunk4Result = co_await writer.send_chunk(chunk4, false);
    if (!chunk4Result) {
        co_await client.close();
        fail();
        co_return;
    }

    // 发送最后一个chunk
    std::string emptyChunk;
    auto lastChunkResult = co_await writer.send_chunk(emptyChunk, true);
    if (!lastChunkResult) {
        co_await client.close();
        fail();
        co_return;
    }


    // 读取响应头
    HttpResponse response;
    bool responseHeaderComplete = false;

    while (!responseHeaderComplete) {
        auto result = co_await reader.get_response(response);

        if (!result) {
            auto& error = result.error();
            if (error.code() == kConnectionClose) {
            } else {
            }
            co_await client.close();
            fail();
            co_return;
        }

        responseHeaderComplete = result.value();
    }


    // `get_response()` 会把 chunked 响应聚合成完整 body 再返回。
    const std::string responseBody = response.get_body_str();

    const std::string expected = "Decoded body bytes: 26\nEcho: Hello from chunked client!";
    if (responseBody != expected) {
        co_await client.close();
        fail();
        co_return;
    }

    co_await client.close();
    g_test_passed = true;
    g_test_done = true;
}

int main() {

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    Runtime rt = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    rt.start();

    auto* scheduler = rt.get_next_io_scheduler();
    if (!scheduler) {
        rt.stop();
        return 1;
    }

    // 启动客户端
    schedule_task(scheduler, send_chunked_request());

    constexpr auto kTimeout = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (!g_test_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!g_test_done.load()) {
        g_test_passed = false;
        g_test_done = true;
    }

    rt.stop();
#else
    return 1;
#endif

    return g_test_passed.load() ? 0 : 1;
}
