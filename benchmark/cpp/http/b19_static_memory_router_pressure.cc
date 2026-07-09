/**
 * @file b19_static_memory_router_pressure.cc
 * @brief HTTP/1 静态文件 MEMORY 模式真实路由压测。
 * @details 启动 HttpRouter::mount(..., MEMORY) 服务端，用多客户端通过 loopback
 *          发起 GET 请求，覆盖 blocking executor 异步读文件路径和响应发送路径。
 *
 * 使用方法:
 *   ./benchmark_http_static_memory_router_pressure [requests] [concurrency] [file_kib]
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <galay/cpp/galay-http/server/http_router.h>
#include <galay/cpp/galay-http/server/http_server.h>

using namespace galay::http;

namespace {

struct ThreadResult
{
    std::vector<int64_t> latencies_us;
    size_t success = 0;
    size_t failure = 0;
};

bool closeFd(int fd, std::string_view context)
{
    const int close_result = ::close(fd);
    if (close_result != 0) {
        std::cerr << context << " close failed errno=" << errno << "\n";
        return false;
    }
    return true;
}

bool writeAll(int fd, const char* data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

bool createPayloadFile(const std::string& dir, size_t file_size)
{
    const int mkdir_result = ::mkdir(dir.c_str(), 0755);
    if (mkdir_result != 0 && errno != EEXIST) {
        std::cerr << "mkdir failed errno=" << errno << "\n";
        return false;
    }

    const std::string path = dir + "/payload.bin";
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        std::cerr << "open payload failed errno=" << errno << "\n";
        return false;
    }

    std::vector<char> block(64 * 1024, 'M');
    size_t remaining = file_size;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, block.size());
        if (!writeAll(fd, block.data(), chunk)) {
            const bool closed = closeFd(fd, "payload write failure");
            if (!closed) {
                std::cerr << "payload close also failed after write failure\n";
            }
            return false;
        }
        remaining -= chunk;
    }

    if (!closeFd(fd, "payload")) {
        return false;
    }
    return true;
}

uint16_t reserveFreePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    const int bind_result = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (bind_result != 0) {
        const bool closed = closeFd(fd, "reserve bind failure");
        if (!closed) {
            std::cerr << "reserve close also failed after bind failure\n";
        }
        return 0;
    }

    socklen_t len = sizeof(addr);
    const int name_result = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    if (name_result != 0) {
        const bool closed = closeFd(fd, "reserve getsockname failure");
        if (!closed) {
            std::cerr << "reserve close also failed after getsockname failure\n";
        }
        return 0;
    }

    const uint16_t port = ntohs(addr.sin_port);
    if (!closeFd(fd, "reserve")) {
        return 0;
    }
    return port;
}

int connectWithRetry(uint16_t port)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        timeval timeout{};
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        const int rcv_result = ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        const int snd_result = ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (rcv_result != 0 || snd_result != 0) {
            const bool closed = closeFd(fd, "connect setsockopt failure");
            if (!closed) {
                std::cerr << "connect close also failed after setsockopt failure\n";
            }
            return -1;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        const int connect_result = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (connect_result == 0) {
            return fd;
        }

        const bool closed = closeFd(fd, "connect retry");
        if (!closed) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
}

bool sendAll(int fd, std::string_view data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool appendBytes(std::string& output, const char* data, size_t size)
{
    const size_t old_size = output.size();
    output.resize(old_size + size);
    void* copied = std::memcpy(output.data() + old_size, data, size);
    return copied == output.data() + old_size;
}

bool receiveResponse(int fd, std::string& response)
{
    char buffer[32 * 1024];
    while (true) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            if (!appendBytes(response, buffer, static_cast<size_t>(n))) {
                return false;
            }
            continue;
        }
        if (n == 0) {
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        return false;
    }
}

bool responseLooksComplete(const std::string& response, size_t file_size)
{
    const int status_compare = response.compare(0, 12, "HTTP/1.1 200");
    if (status_compare != 0) {
        return false;
    }

    const size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    const size_t body_begin = header_end + 4;
    if (response.size() - body_begin != file_size) {
        return false;
    }

    for (size_t i = body_begin; i < response.size(); ++i) {
        if (response[i] != 'M') {
            return false;
        }
    }
    return true;
}

bool runSingleRequest(uint16_t port, size_t file_size, int64_t& latency_us, bool print_failure)
{
    const int fd = connectWithRetry(port);
    if (fd < 0) {
        return false;
    }

    const std::string request =
        "GET /static/payload.bin HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n"
        "\r\n";

    const auto start = std::chrono::steady_clock::now();
    if (!sendAll(fd, request)) {
        const bool closed = closeFd(fd, "request send failure");
        if (!closed) {
            std::cerr << "request close also failed after send failure\n";
        }
        return false;
    }

    std::string response;
    response.reserve(file_size + 256);
    if (!receiveResponse(fd, response)) {
        const bool closed = closeFd(fd, "response recv failure");
        if (!closed) {
            std::cerr << "response close also failed after recv failure\n";
        }
        return false;
    }
    const auto stop = std::chrono::steady_clock::now();

    if (!closeFd(fd, "request")) {
        return false;
    }

    latency_us = std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count();
    const bool complete = responseLooksComplete(response, file_size);
    if (!complete && print_failure) {
        const size_t sample_size = std::min<size_t>(response.size(), 256);
        const std::string sample = response.substr(0, sample_size);
        std::cerr << "first failed response size=" << response.size()
                  << " sample=[" << sample << "]\n";
    }
    return complete;
}

ThreadResult runWorker(uint16_t port, size_t file_size, size_t requests, bool print_first_failure)
{
    ThreadResult result;
    result.latencies_us.reserve(requests);
    for (size_t i = 0; i < requests; ++i) {
        int64_t latency_us = 0;
        const bool print_failure = print_first_failure && i == 0;
        if (runSingleRequest(port, file_size, latency_us, print_failure)) {
            result.latencies_us.push_back(latency_us);
            ++result.success;
        } else {
            ++result.failure;
        }
    }
    return result;
}

int64_t percentile(std::vector<int64_t>& values, double pct)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const auto max_index = static_cast<double>(values.size() - 1);
    const size_t index = static_cast<size_t>(max_index * pct);
    return values[index];
}

bool cleanupDirectory(const std::string& dir)
{
    const std::string path = dir + "/payload.bin";
    bool ok = true;
    const int unlink_result = ::unlink(path.c_str());
    if (unlink_result != 0 && errno != ENOENT) {
        std::cerr << "unlink failed errno=" << errno << "\n";
        ok = false;
    }
    const int rmdir_result = ::rmdir(dir.c_str());
    if (rmdir_result != 0 && errno != ENOENT) {
        std::cerr << "rmdir failed errno=" << errno << "\n";
        ok = false;
    }
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    size_t total_requests = 128;
    size_t concurrency = 8;
    size_t file_kib = 256;
    if (argc > 1) {
        total_requests = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
    }
    if (argc > 2) {
        concurrency = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
    }
    if (argc > 3) {
        file_kib = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    }
    if (total_requests == 0 || concurrency == 0 || file_kib == 0) {
        std::cerr << "requests, concurrency and file_kib must be positive\n";
        return 1;
    }
    if (concurrency > total_requests) {
        concurrency = total_requests;
    }
    if (file_kib > std::numeric_limits<size_t>::max() / 1024) {
        std::cerr << "file_kib is too large\n";
        return 1;
    }

    const size_t file_size = file_kib * 1024;
    const std::string dir = "/tmp/galay-http-static-memory-" +
                            std::to_string(static_cast<long long>(::getpid()));
    if (!createPayloadFile(dir, file_size)) {
        const bool cleaned = cleanupDirectory(dir);
        if (!cleaned) {
            std::cerr << "cleanup failed after payload creation failure\n";
        }
        return 1;
    }

    StaticFileSetting setting;
    setting.setTransferMode(FileTransferMode::MEMORY);
    setting.setEnableETag(false);

    HttpRouter router;
    router.mount("/static", dir, setting);

    const uint16_t port = reserveFreePort();
    if (port == 0) {
        const bool cleaned = cleanupDirectory(dir);
        if (!cleaned) {
            std::cerr << "cleanup failed after port reservation failure\n";
        }
        return 1;
    }

    auto server = HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(2)
        .computeSchedulerCount(1)
        .build();
    server.start(std::move(router));

    std::vector<ThreadResult> results(concurrency);
    std::vector<std::thread> workers;
    workers.reserve(concurrency);

    const size_t base_requests = total_requests / concurrency;
    const size_t extra_requests = total_requests % concurrency;
    const auto start = std::chrono::steady_clock::now();
    bool worker_start_failed = false;
    for (size_t i = 0; i < concurrency; ++i) {
        const size_t worker_requests = base_requests + (i < extra_requests ? 1 : 0);
        const bool print_first_failure = i == 0;
        auto& worker = workers.emplace_back([port, file_size, worker_requests, print_first_failure, &results, i]() {
            results[i] = runWorker(port, file_size, worker_requests, print_first_failure);
        });
        if (!worker.joinable()) {
            std::cerr << "worker thread is not joinable\n";
            worker_start_failed = true;
            break;
        }
    }

    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    const auto stop = std::chrono::steady_clock::now();

    server.stop();
    if (worker_start_failed) {
        const bool cleaned = cleanupDirectory(dir);
        if (!cleaned) {
            std::cerr << "cleanup failed after worker creation failure\n";
        }
        return 1;
    }

    const bool cleaned = cleanupDirectory(dir);
    if (!cleaned) {
        std::cerr << "cleanup failed after benchmark\n";
        return 1;
    }

    size_t success = 0;
    size_t failure = 0;
    std::vector<int64_t> latencies;
    latencies.reserve(total_requests);
    for (ThreadResult& result : results) {
        success += result.success;
        failure += result.failure;
        for (int64_t latency : result.latencies_us) {
            latencies.push_back(latency);
        }
    }

    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count();
    const double elapsed_sec = static_cast<double>(elapsed_us) / 1'000'000.0;
    const double rps = elapsed_sec > 0.0 ? static_cast<double>(success) / elapsed_sec : 0.0;
    const double mib = static_cast<double>(success * file_size) / (1024.0 * 1024.0);
    const double mib_per_sec = elapsed_sec > 0.0 ? mib / elapsed_sec : 0.0;

    const int64_t p50 = percentile(latencies, 0.50);
    const int64_t p99 = percentile(latencies, 0.99);

    std::cout << "http static memory router pressure\n"
              << "  requests: " << total_requests << "\n"
              << "  concurrency: " << concurrency << "\n"
              << "  file_kib: " << file_kib << "\n"
              << "  success: " << success << "\n"
              << "  failure: " << failure << "\n"
              << "  elapsed_sec: " << elapsed_sec << "\n"
              << "  requests_per_sec: " << rps << "\n"
              << "  throughput_mib_per_sec: " << mib_per_sec << "\n"
              << "  p50_us: " << p50 << "\n"
              << "  p99_us: " << p99 << "\n";

    return failure == 0 ? 0 : 1;
}
