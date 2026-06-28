/**
 * @file test_reader_writer_client.cc
 * @brief HTTP Reader/Writer 测试 - 客户端
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <cctype>
#include <galay/cpp/galay-kernel/async/tcp_socket.h>
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

using namespace galay::async;
using namespace galay::kernel;

std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};
std::atomic<bool> g_test_done{false};

std::string normalizeExpectedEchoPath(std::string path) {
    auto query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path.resize(query_pos);
    }

    std::string decoded;
    decoded.reserve(path.size());

    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return -1;
    };

    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            int hi = hex_value(path[i + 1]);
            int lo = hex_value(path[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(path[i]);
    }

    return decoded;
}

bool parseExpectedResponseSize(const std::string& response, size_t& expected_size) {
    auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    expected_size = header_end + 4;

    std::string header_part = response.substr(0, header_end);
    for (auto& c : header_part) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    auto content_length_pos = header_part.find("content-length:");
    if (content_length_pos == std::string::npos) {
        return true;
    }

    content_length_pos += sizeof("Content-Length:") - 1;
    while (content_length_pos < header_part.size() &&
           std::isspace(static_cast<unsigned char>(header_part[content_length_pos]))) {
        ++content_length_pos;
    }

    size_t num_end = content_length_pos;
    while (num_end < header_part.size() &&
           std::isdigit(static_cast<unsigned char>(header_part[num_end]))) {
        ++num_end;
    }

    if (num_end > content_length_pos) {
        expected_size += static_cast<size_t>(
            std::stoull(header_part.substr(content_length_pos, num_end - content_length_pos)));
    }

    return true;
}

// 客户端测试
Task<void> test_client(int test_id, std::string path) {

    TcpSocket client;
    client.option().handleNonBlock();

    // 连接到服务器
    Host serverHost(IPType::IPV4, "127.0.0.1", 9999);
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        g_failed++;
        co_return;
    }


    // 构造HTTP请求
    std::string requestStr =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: localhost:9999\r\n"
        "User-Agent: galay-http-test/1.0\r\n"
        "Connection: close\r\n"
        "\r\n";

    // 发送请求
    auto sendResult = co_await client.send(requestStr.c_str(), requestStr.size());
    if (!sendResult) {
        g_failed++;
        co_await client.close();
        co_return;
    }


    // 接收完整响应（处理分片场景）
    std::string response;
    size_t expected_size = 0;
    bool header_ready = false;

    while (true) {
        char buffer[4096];
        auto recvResult = co_await client.recv(buffer, sizeof(buffer));
        if (!recvResult) {
            if (response.empty()) {
                g_failed++;
                co_await client.close();
                co_return;
            }
            break;
        }

        const size_t bytes = recvResult.value();
        if (bytes == 0) {
            break;
        }

        response.append(buffer, bytes);

        if (!header_ready) {
            header_ready = parseExpectedResponseSize(response, expected_size);
        }

        if (header_ready && response.size() >= expected_size) {
            break;
        }
    }

    if (response.empty()) {
        g_failed++;
        co_await client.close();
        co_return;
    }


    // 验证响应
    std::string expected_path = normalizeExpectedEchoPath(path);
    if (response.find("HTTP/1.1 200 OK") != std::string::npos &&
        response.find("Echo: " + expected_path) != std::string::npos) {
        g_passed++;
    } else {
        g_failed++;
    }

    co_await client.close();
    co_return;
}

// 运行所有测试
Task<void> runAllTests(IOScheduler* scheduler) {
    // 等待一下确保服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动所有测试（并发执行）
    scheduleTask(scheduler, test_client(1, "/test"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    scheduleTask(scheduler, test_client(2, "/api/users?id=123"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    scheduleTask(scheduler, test_client(3, "/very/long/path/to/resource"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    scheduleTask(scheduler, test_client(4, "/"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    scheduleTask(scheduler, test_client(5, "/test%20path"));

    // 等待所有测试完成
    std::this_thread::sleep_for(std::chrono::seconds(2));

    g_test_done = true;
    co_return;
}

int main() {


    std::this_thread::sleep_for(std::chrono::seconds(1));

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    Runtime rt = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    rt.start();

    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        rt.stop();
        return 1;
    }

    // 运行测试
    scheduleTask(scheduler, runAllTests(scheduler));

    // 等待测试完成
    while (!g_test_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    rt.stop();
#else
    return 1;
#endif

    return g_failed > 0 ? 1 : 0;
}
