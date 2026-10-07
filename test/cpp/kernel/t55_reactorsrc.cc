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

std::filesystem::path project_root() {
    return std::filesystem::path(GALAY_SOURCE_ROOT);
}

bool contains_text(const std::filesystem::path& path, const std::string& needle) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    return content.find(needle) != std::string::npos;
}

bool contains_any_text(const std::filesystem::path& path,
                     const std::vector<std::string>& needles,
                     std::string* matched = nullptr) {
    for (const auto& needle : needles) {
        if (contains_text(path, needle)) {
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
    const auto root = project_root();
    const auto kernel_dir = root / "galay-kernel" / "core";

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
        if (!contains_text(expectation.file, expectation.symbol)) {
            std::cerr << "[T55] expected " << expectation.file
                      << " to reference " << expectation.symbol << '\n';
            return 1;
        }
    }

    const auto backend_reactor = kernel_dir / "backend_reactor.h";
    if (!contains_text(backend_reactor, "concept ReactorType")) {
        std::cerr << "[T55] expected ReactorType to be a concept contract\n";
        return 1;
    }
    if (contains_text(backend_reactor, "BackendReactor") ||
        contains_text(backend_reactor, "class BackendReactor") ||
        contains_text(backend_reactor, "virtual ~BackendReactor") ||
        contains_text(backend_reactor, "virtual void notify()") ||
        contains_text(backend_reactor, "virtual GHandle get_handle()")) {
        std::cerr << "[T55] expected ReactorType to replace the old BackendReactor name\n";
        return 1;
    }

    const std::vector<std::filesystem::path> reactor_headers = {
        kernel_dir / "kqueue_reactor.h",
        kernel_dir / "epoll_reactor.h",
        kernel_dir / "uring_reactor.h",
    };
    for (const auto& path : reactor_headers) {
        if (contains_text(path, "BackendReactor") ||
            contains_text(path, "public BackendReactor") ||
            contains_text(path, "void notify() override") ||
            contains_text(path, "GHandle get_handle() const override")) {
            std::cerr << "[T55] expected " << path
                      << " to satisfy ReactorType concept without virtual inheritance\n";
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
                "process_event(",
                "buildEpollEvents(",
                "applyEpollEvents(",
            },
            {
                "epoll_wait(",
                "epoll_ctl(",
                "eventfd(",
                "process_event(",
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
                "process_completion(",
                "submit_sequence_sqe(",
            },
            {
                "io_uring_queue_init_params(",
                "io_uring_get_sqe(",
                "io_uring_wait_cqe_timeout(",
                "io_uring_submit_and_wait_timeout(",
                "process_completion(",
                "submit_sequence_sqe(",
            },
        },
    };

    for (const auto& expectation : boundary_expectations) {
        std::string matched;
        if (contains_any_text(expectation.scheduler_header,
                            expectation.forbidden_header_tokens,
                            &matched)) {
            std::cerr << "[T55] expected " << expectation.scheduler_header
                      << " to delegate backend state to reactor, found token "
                      << matched << '\n';
            return 1;
        }

        matched.clear();
        if (contains_any_text(expectation.scheduler_source,
                            expectation.forbidden_source_tokens,
                            &matched)) {
            std::cerr << "[T55] expected " << expectation.scheduler_source
                      << " to delegate backend operations to reactor, found token "
                      << matched << '\n';
            return 1;
        }
    }

    const auto iouring_reactor = kernel_dir / "uring_reactor.cc";
    if (contains_text(iouring_reactor, "auto* sequence = controller->get_awaitable<SequenceAwaitableBase>();")) {
        std::cerr << "[T55] expected IOUringReactor sequence path to avoid single-owner lookup\n";
        return 1;
    }
    if (contains_text(iouring_reactor, "int IOUringReactor::submit_sequence_sqe(IOEventType type,")) {
        std::cerr << "[T55] expected IOUringReactor submit_sequence_sqe to be slot-aware\n";
        return 1;
    }
    if (!contains_text(iouring_reactor, "int IOUringReactor::submit_sequence_sqe(IOController::Index slot,")) {
        std::cerr << "[T55] expected IOUringReactor submit_sequence_sqe to accept slot parameter\n";
        return 1;
    }
    if (!contains_text(iouring_reactor, "controller->m_awaitable[slot] == owner")) {
        std::cerr << "[T55] expected IOUringReactor sequence path to skip already-armed slots\n";
        return 1;
    }
    if (!contains_text(iouring_reactor, "controller->m_awaitable[slot] = owner;")) {
        std::cerr << "[T55] expected IOUringReactor sequence SQE submission to bind slot owner\n";
        return 1;
    }

    std::cout << "T55-SchedulerReactorSourceCase PASS\n";
    return 0;
}
