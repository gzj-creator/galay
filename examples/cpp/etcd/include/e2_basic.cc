#include <galay/cpp/galay-etcd/async/client.h>

#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

using galay::kernel::IOScheduler;
using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::kernel::Task;

namespace
{

struct EndpointParts {
    std::string host;
    std::string port;
    bool valid = false;
};

EndpointParts parseHttpEndpoint(const std::string& endpoint)
{
    constexpr std::string_view prefix = "http://";
    if (endpoint.rfind(prefix, 0) != 0) {
        return {};
    }

    std::string_view rest(endpoint.data() + prefix.size(), endpoint.size() - prefix.size());
    const size_t slash = rest.find('/');
    if (slash != std::string_view::npos) {
        rest = rest.substr(0, slash);
    }

    EndpointParts parts;
    const size_t colon = rest.rfind(':');
    if (colon == std::string_view::npos) {
        parts.host = std::string(rest);
        parts.port = "2379";
    } else {
        parts.host = std::string(rest.substr(0, colon));
        parts.port = std::string(rest.substr(colon + 1));
    }
    parts.valid = !parts.host.empty() && !parts.port.empty();
    return parts;
}

bool setNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool endpointReachable(const EndpointParts& endpoint)
{
    if (!endpoint.valid) {
        return true;
    }

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo* raw = nullptr;
    const int gai = ::getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &raw);
    if (gai != 0 || raw == nullptr) {
        return false;
    }

    bool reachable = false;
    for (addrinfo* item = raw; item != nullptr && !reachable; item = item->ai_next) {
        int fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (!setNonBlocking(fd)) {
            (void)::close(fd);
            continue;
        }

        if (::connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            reachable = true;
        } else if (errno == EINPROGRESS) {
            pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
            const int polled = ::poll(&pfd, 1, 200);
            if (polled > 0) {
                int socket_error = 0;
                socklen_t len = sizeof(socket_error);
                reachable = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) == 0 &&
                    socket_error == 0;
            }
        }

        if (::close(fd) != 0 && reachable) {
            reachable = false;
        }
    }

    ::freeaddrinfo(raw);
    return reachable;
}

Task<void> runExample(IOScheduler* scheduler,
                      std::string endpoint,
                      std::atomic<bool>* done,
                      int* exit_code)
{
    auto finish = [&](int code) {
        *exit_code = code;
        done->store(true, std::memory_order_release);
    };

    const std::string key = "/galay-etcd/examples/async/" + std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count());
    const std::string value = "hello-async";

    galay::etcd::EtcdConfig cfg;
    cfg.endpoint = endpoint;

    auto client = galay::etcd::AsyncEtcdClientBuilder().scheduler(scheduler).config(cfg).build();
    auto conn = co_await client.connect();
    if (!conn.has_value()) {
        std::cerr << "connect failed: " << conn.error().message() << '\n';
        finish(1);
        co_return;
    }

    auto put = co_await client.put(key, value);
    if (!put.has_value()) {
        std::cerr << "put failed: " << put.error().message() << '\n';
        finish(2);
        co_return;
    }

    auto get = co_await client.get(key);
    if (!get.has_value()) {
        std::cerr << "get failed: " << get.error().message() << '\n';
        finish(3);
        co_return;
    }
    if (get.value().empty()) {
        std::cerr << "get returned empty kvs\n";
        finish(4);
        co_return;
    }

    std::cout << "async example ok: " << get.value().front().key
              << " => " << get.value().front().value << '\n';

    (void)co_await client.del(key);
    (void)co_await client.close();

    finish(0);
}

} // namespace

int main(int argc, char** argv)
{
    const std::string endpoint = argc > 1 ? argv[1] : "http://127.0.0.1:2379";
    const EndpointParts endpoint_parts = parseHttpEndpoint(endpoint);
    if (!endpointReachable(endpoint_parts)) {
        std::cerr << "[EXTERNAL_DEP] etcd endpoint is required: " << endpoint << '\n';
        return 1;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "failed to get io scheduler\n";
        return 1;
    }

    std::atomic<bool> done{false};
    int exit_code = 1;
    if (!galay::kernel::scheduleTask(scheduler, runExample(scheduler, endpoint, &done, &exit_code))) {
        runtime.stop();
        std::cerr << "failed to schedule async example task\n";
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done.load(std::memory_order_acquire)) {
        exit_code = 2;
        std::cerr << "async example timeout\n";
    }

    runtime.stop();
    return exit_code;
}
