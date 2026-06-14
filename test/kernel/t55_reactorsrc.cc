/**
 * @file t55_reactorsrc.cc
 * @brief 用途：验证 scheduler 与各 reactor 后端的源码边界拆分是否到位。
 * 关键覆盖点：Reactor 文件存在性、scheduler 头源文件引用、后端边界禁用 token。
 * 通过条件：源码结构满足边界约束且测试返回 0。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectRoot() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path();
    return path.lexically_normal();
}

bool containsText(const std::filesystem::path& path, const std::string& needle) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    return content.find(needle) != std::string::npos;
}

bool containsAnyText(const std::filesystem::path& path,
                     const std::vector<std::string>& needles,
                     std::string* matched = nullptr) {
    for (const auto& needle : needles) {
        if (containsText(path, needle)) {
            if (matched != nullptr) {
                *matched = needle;
            }
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto kernel_dir = root / "galay-kernel" / "kernel";

    const std::vector<std::filesystem::path> required_files = {
        kernel_dir / "backend_reactor.h",
        kernel_dir / "kqueue_reactor.h",
        kernel_dir / "epoll_reactor.h",
        kernel_dir / "uring_reactor.h",
    };

    for (const auto& path : required_files) {
        if (!std::filesystem::exists(path)) {
            std::cerr << "[T55] missing reactor file: " << path << '\n';
            return 1;
        }
    }

    struct Expectation {
        std::filesystem::path file;
        std::string symbol;
    };

    const std::vector<Expectation> expectations = {
        {kernel_dir / "kqueue_scheduler.h", "KqueueReactor"},
        {kernel_dir / "epoll_scheduler.h", "EpollReactor"},
        {kernel_dir / "uring_scheduler.h", "IOUringReactor"},
    };

    for (const auto& expectation : expectations) {
        if (!containsText(expectation.file, expectation.symbol)) {
            std::cerr << "[T55] expected " << expectation.file
                      << " to reference " << expectation.symbol << '\n';
            return 1;
        }
    }

    struct BackendBoundaryExpectation {
        std::filesystem::path scheduler_header;
        std::filesystem::path scheduler_source;
        std::vector<std::string> forbidden_header_tokens;
        std::vector<std::string> forbidden_source_tokens;
    };

    const std::vector<BackendBoundaryExpectation> boundary_expectations = {
        {
            kernel_dir / "epoll_scheduler.h",
            kernel_dir / "epoll_scheduler.cc",
            {
                "m_epoll_fd",
                "m_event_fd",
                "m_events",
                "processEvent(",
                "buildEpollEvents(",
                "applyEpollEvents(",
            },
            {
                "epoll_wait(",
                "epoll_ctl(",
                "eventfd(",
                "processEvent(",
                "buildEpollEvents(",
                "applyEpollEvents(",
            },
        },
        {
            kernel_dir / "uring_scheduler.h",
            kernel_dir / "uring_scheduler.cc",
            {
                "m_ring",
                "m_event_fd",
                "m_eventfd_buf",
                "processCompletion(",
                "submitSequenceSqe(",
            },
            {
                "io_uring_queue_init_params(",
                "io_uring_get_sqe(",
                "io_uring_wait_cqe_timeout(",
                "io_uring_submit_and_wait_timeout(",
                "processCompletion(",
                "submitSequenceSqe(",
            },
        },
    };

    for (const auto& expectation : boundary_expectations) {
        std::string matched;
        if (containsAnyText(expectation.scheduler_header,
                            expectation.forbidden_header_tokens,
                            &matched)) {
            std::cerr << "[T55] expected " << expectation.scheduler_header
                      << " to delegate backend state to reactor, found token "
                      << matched << '\n';
            return 1;
        }

        matched.clear();
        if (containsAnyText(expectation.scheduler_source,
                            expectation.forbidden_source_tokens,
                            &matched)) {
            std::cerr << "[T55] expected " << expectation.scheduler_source
                      << " to delegate backend operations to reactor, found token "
                      << matched << '\n';
            return 1;
        }
    }

    const auto iouring_reactor = kernel_dir / "uring_reactor.cc";
    if (containsText(iouring_reactor, "auto* sequence = controller->getAwaitable<SequenceAwaitableBase>();")) {
        std::cerr << "[T55] expected IOUringReactor sequence path to avoid single-owner lookup\n";
        return 1;
    }
    if (containsText(iouring_reactor, "int IOUringReactor::submitSequenceSqe(IOEventType type,")) {
        std::cerr << "[T55] expected IOUringReactor submitSequenceSqe to be slot-aware\n";
        return 1;
    }
    if (!containsText(iouring_reactor, "int IOUringReactor::submitSequenceSqe(IOController::Index slot,")) {
        std::cerr << "[T55] expected IOUringReactor submitSequenceSqe to accept slot parameter\n";
        return 1;
    }
    if (!containsText(iouring_reactor, "controller->m_awaitable[slot] == owner")) {
        std::cerr << "[T55] expected IOUringReactor sequence path to skip already-armed slots\n";
        return 1;
    }
    if (!containsText(iouring_reactor, "controller->m_awaitable[slot] = owner;")) {
        std::cerr << "[T55] expected IOUringReactor sequence SQE submission to bind slot owner\n";
        return 1;
    }

    std::cout << "T55-SchedulerReactorSourceCase PASS\n";
    return 0;
}
