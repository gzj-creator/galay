/**
 * @file b17_static_server_throughput.cc
 * @brief HTTP/1.1 静态文件 echo 压测程序（公平口径）
 * @details 与 b1 相同的服务器骨架，但 handler 每请求做真实 stat+open+read+close
 *          读取磁盘文件并回显，镜像 Apache httpd 静态文件服务的每请求工作量，
 *          用于 galay-static vs httpd-static 的公平对比（而非 b1 的内存固定响应）。
 *
 * 使用方法:
 *   ./benchmark_http_static_server_throughput [port] [io_threads] [file_path]
 */

#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-http/kernel/http_conn.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <iostream>
#include <csignal>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

using namespace galay::http;
using namespace galay::kernel;

static volatile bool g_running = true;
static std::string g_file_path;

void signalHandler(int) { g_running = false; }

// Real per-request static-file work: stat + open + read (page-cached) + close.
static bool read_file(std::string& out) {
    struct stat st{};
    if (::stat(g_file_path.c_str(), &st) != 0) return false;
    const auto size = static_cast<size_t>(st.st_size);
    out.resize(size);
    if (size == 0) {
        int fd = ::open(g_file_path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        ::close(fd);
        return true;
    }
    int fd = ::open(g_file_path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < size) {
        ssize_t n = ::read(fd, out.data() + got, size - got);
        if (n <= 0) { ::close(fd); return false; }
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    return true;
}

Task<void> handleStaticRequest(HttpConn conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();
    // Stable buffers that outlive each co_await sendView (sendView holds a raw pointer).
    std::string body;
    std::string response;
    static const std::string not_found =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";

    while (true) {
        HttpRequest request;
        while (true) {
            auto read_result = co_await reader.getRequest(request);
            if (!read_result) co_return;
            if (read_result.value()) break;
        }

        if (!read_file(body)) {
            auto r = co_await writer.sendView(not_found);
            if (!r) co_return;
            continue;
        }

        response.clear();
        response.reserve(160 + body.size());
        response.append("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                        "Content-Length: ");
        response.append(std::to_string(body.size()));
        response.append("\r\nConnection: keep-alive\r\n\r\n");
        response.append(body);

        auto result = co_await writer.sendView(response);
        if (!result) co_return;
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 18081;
    int io_threads = 4;
    g_file_path = "/tmp/galay-http-static-www/ok.txt";
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) io_threads = std::atoi(argv[2]);
    if (argc > 3) g_file_path = argv[3];

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpServer server(HttpServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .ioSchedulerCount(static_cast<size_t>(io_threads))
            .computeSchedulerCount(0)
            .build());
        server.start(handleStaticRequest);

        std::cout << "========================================\n"
                  << "HTTP Static-File Server Benchmark\n"
                  << "Port: " << port << "\nIO Threads: " << io_threads
                  << "\nFile: " << g_file_path << "\n========================================\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "\nShutting down...\n";
        server.stop();
        std::cout << "Server stopped.\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
