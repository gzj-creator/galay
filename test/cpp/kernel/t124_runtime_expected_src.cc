/**
 * @file t124_runtime_expected_src.cc
 * @brief 用途：锁定 kernel/C wrapper 公开边界通过 std::expected 返回错误，不使用 throw/try/catch。
 * 关键覆盖点：runtime、scheduler、socket、reactor 和 C wrapper 边界不包含显式异常源码。
 * 通过条件：kernel 启动与 socket 创建路径只通过返回值传播错误，测试返回 0。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cctype>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectRoot()
{
    return std::filesystem::path(GALAY_PROJECT_ROOT);
}

std::filesystem::path cppRoot()
{
    return std::filesystem::path(GALAY_SOURCE_ROOT);
}

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

bool containsWord(const std::string& text, const std::string& word)
{
    size_t pos = text.find(word);
    while (pos != std::string::npos) {
        const bool left_ok = pos == 0 ||
            (!std::isalnum(static_cast<unsigned char>(text[pos - 1])) && text[pos - 1] != '_');
        const size_t after = pos + word.size();
        const bool right_ok = after >= text.size() ||
            (!std::isalnum(static_cast<unsigned char>(text[after])) && text[after] != '_');
        if (left_ok && right_ok) {
            return true;
        }
        pos = text.find(word, pos + word.size());
    }
    return false;
}

void checkRuntimeSource(const std::filesystem::path& path,
                        const std::string& text,
                        std::vector<std::string>& failures)
{
    if (text.empty()) {
        failures.push_back(path.string() + ": failed to read source");
        return;
    }
    if (containsWord(text, "throw")) {
        failures.push_back(path.string() + ": runtime API boundary must not throw");
    }
    if (contains(text, "@throws")) {
        failures.push_back(path.string() + ": runtime API docs must not advertise throwing");
    }
    if (contains(text, "std::runtime_error")) {
        failures.push_back(path.string() + ": runtime API boundary must not depend on runtime_error");
    }
    if (containsWord(text, "try")) {
        failures.push_back(path.string() + ": runtime API boundary must not use try/catch");
    }
    if (containsWord(text, "catch")) {
        failures.push_back(path.string() + ": runtime API boundary must not catch exceptions");
    }
}

void requireContains(const std::filesystem::path& path,
                     const std::string& text,
                     const std::string& needle,
                     const std::string& message,
                     std::vector<std::string>& failures)
{
    if (!contains(text, needle)) {
        failures.push_back(path.string() + ": " + message);
    }
}

}  // namespace

int main()
{
    const auto project_root = projectRoot();
    const auto root = cppRoot();
    const std::vector<std::filesystem::path> source_paths = {
        project_root / "src" / "c" / "galay-kernel" / "galay_kernel.cc",
        root / "galay-kernel" / "async" / "tcp_socket.h",
        root / "galay-kernel" / "async" / "tcp_socket.cc",
        root / "galay-kernel" / "async" / "udp_socket.h",
        root / "galay-kernel" / "async" / "udp_socket.cc",
        root / "galay-kernel" / "core" / "scheduler.hpp",
        root / "galay-kernel" / "core" / "runtime.h",
        root / "galay-kernel" / "core" / "runtime.cc",
        root / "galay-kernel" / "core" / "compute_scheduler.h",
        root / "galay-kernel" / "core" / "compute_scheduler.cc",
        root / "galay-kernel" / "core" / "epoll_scheduler.h",
        root / "galay-kernel" / "core" / "epoll_scheduler.cc",
        root / "galay-kernel" / "core" / "kqueue_scheduler.h",
        root / "galay-kernel" / "core" / "kqueue_scheduler.cc",
        root / "galay-kernel" / "core" / "uring_scheduler.h",
        root / "galay-kernel" / "core" / "uring_scheduler.cc",
        root / "galay-kernel" / "core" / "epoll_reactor.h",
        root / "galay-kernel" / "core" / "epoll_reactor.cc",
        root / "galay-kernel" / "core" / "kqueue_reactor.h",
        root / "galay-kernel" / "core" / "kqueue_reactor.cc",
        root / "galay-kernel" / "core" / "uring_reactor.h",
        root / "galay-kernel" / "core" / "uring_reactor.cc",
        root / "galay-kernel" / "core" / "task.h",
        root / "galay-kernel" / "core" / "task.cc",
        root / "galay-kernel" / "core" / "blocking_executor.h",
        root / "galay-kernel" / "core" / "blocking_executor.cc",
    };

    std::vector<std::string> failures;
    for (const auto& source_path : source_paths) {
        checkRuntimeSource(source_path, readAll(source_path), failures);
    }

    const auto scheduler_h = readAll(root / "galay-kernel" / "core" / "scheduler.hpp");
    requireContains(root / "galay-kernel" / "core" / "scheduler.hpp",
                    scheduler_h,
                    "virtual std::expected<void, IOError> start() = 0;",
                    "Scheduler::start() must return std::expected<void, IOError>",
                    failures);

    const auto runtime_h = readAll(root / "galay-kernel" / "core" / "runtime.h");
    requireContains(root / "galay-kernel" / "core" / "runtime.h",
                    runtime_h,
                    "std::expected<void, RuntimeError> start();",
                    "Runtime::start() must return std::expected<void, RuntimeError>",
                    failures);

    const auto tcp_h = readAll(root / "galay-kernel" / "async" / "tcp_socket.h");
    requireContains(root / "galay-kernel" / "async" / "tcp_socket.h",
                    tcp_h,
                    "static std::expected<TcpSocket, galay::kernel::IOError> create(",
                    "TcpSocket must expose an expected-based create factory",
                    failures);

    const auto udp_h = readAll(root / "galay-kernel" / "async" / "udp_socket.h");
    requireContains(root / "galay-kernel" / "async" / "udp_socket.h",
                    udp_h,
                    "static std::expected<UdpSocket, galay::kernel::IOError> create(",
                    "UdpSocket must expose an expected-based create factory",
                    failures);

    const std::vector<std::pair<std::filesystem::path, std::string>> reactor_headers = {
        {root / "galay-kernel" / "core" / "epoll_reactor.h", "EpollReactor"},
        {root / "galay-kernel" / "core" / "kqueue_reactor.h", "KqueueReactor"},
        {root / "galay-kernel" / "core" / "uring_reactor.h", "IOUringReactor"},
    };
    for (const auto& [path, reactor_name] : reactor_headers) {
        const auto text = readAll(path);
        requireContains(path,
                        text,
                        "std::expected<void, IOError> start();",
                        reactor_name + " must expose an expected-based explicit start()",
                        failures);
        if (contains(text, "std::expected<void, IOError> initialize();")) {
            failures.push_back(path.string() + ": " + reactor_name + " must not expose initialize(); use start()");
        }
    }

    const std::vector<std::pair<std::filesystem::path, std::string>> reactor_sources = {
        {root / "galay-kernel" / "core" / "epoll_reactor.cc", "EpollReactor"},
        {root / "galay-kernel" / "core" / "kqueue_reactor.cc", "KqueueReactor"},
        {root / "galay-kernel" / "core" / "uring_reactor.cc", "IOUringReactor"},
    };
    for (const auto& [path, reactor_name] : reactor_sources) {
        const auto text = readAll(path);
        if (contains(text, "(void)initialize();")) {
            failures.push_back(path.string() + ": " + reactor_name + " constructor must not discard initialization errors");
        }
        requireContains(path,
                        text,
                        reactor_name + "::start()",
                        reactor_name + " must implement explicit start()",
                        failures);
        if (contains(text, reactor_name + "::initialize()")) {
            failures.push_back(path.string() + ": " + reactor_name + " must rename initialize() to start()");
        }
    }

    if (!failures.empty()) {
        for (const auto& failure : failures) {
            std::cerr << "[T124] " << failure << '\n';
        }
        return 1;
    }

    std::cout << "T124-RuntimeExpectedSourceBoundary PASS\n";
    return 0;
}
