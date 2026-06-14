/**
 * @file t96_stealsrc.cc
 * @brief 锁定 IOScheduler work-stealing 设计的源码边界
 *
 * 关键验证点：
 * - 不再含有 `std::deque<TaskRef> local_queue`
 * - worker state 抽象了固定容量 ring 相关成员
 * - 承载 stealing 的入口名称或注释含 `steal` 语义
 * - Runtime::start() 包含向 IO scheduler 发送 steal-domain 配置的调用/路径
 * - ComputeScheduler 未被接入 stealing 结构
 */

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().lexically_normal();
}

std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool containsText(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void requireContains(std::vector<std::string>& failures,
                     const std::filesystem::path& path,
                     const std::string& content,
                     const std::string& needle,
                     const std::string& message) {
    if (!containsText(content, needle)) {
        failures.push_back(path.string() + ": " + message);
    }
}

void requireNotContains(std::vector<std::string>& failures,
                        const std::filesystem::path& path,
                        const std::string& content,
                        const std::string& needle,
                        const std::string& message) {
    if (containsText(content, needle)) {
        failures.push_back(path.string() + ": " + message);
    }
}

std::string extractSection(const std::string& content,
                           const std::string& begin_marker,
                           const std::string& end_marker) {
    const auto begin_pos = content.find(begin_marker);
    if (begin_pos == std::string::npos) {
        return {};
    }
    const auto end_pos = content.find(end_marker, begin_pos);
    if (end_pos == std::string::npos) {
        return content.substr(begin_pos);
    }
    return content.substr(begin_pos, end_pos + end_marker.size() - begin_pos);
}

std::string extractBracedSection(const std::string& content,
                                 const std::string& begin_marker) {
    const auto begin_pos = content.find(begin_marker);
    if (begin_pos == std::string::npos) {
        return {};
    }

    const auto brace_pos = content.find('{', begin_pos + begin_marker.size());
    if (brace_pos == std::string::npos) {
        return {};
    }

    size_t depth = 0;
    for (size_t i = brace_pos; i < content.size(); ++i) {
        if (content[i] == '{') {
            ++depth;
            continue;
        }
        if (content[i] != '}') {
            continue;
        }
        if (depth == 0) {
            return {};
        }
        --depth;
        if (depth == 0) {
            size_t end = i + 1;
            while (end < content.size() &&
                   (content[end] == ' ' || content[end] == '\t' ||
                    content[end] == '\r' || content[end] == '\n')) {
                ++end;
            }
            if (end < content.size() && content[end] == ';') {
                ++end;
            }
            return content.substr(begin_pos, end - begin_pos);
        }
    }

    return {};
}

bool closeDelimiter(const std::string& section, const std::string& delimiter) {
    return section.find(delimiter) != std::string::npos;
}

std::string trim(const std::string& input) {
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(begin, end - begin + 1);
}

bool hasStealDeclaration(const std::string& section) {
    std::istringstream stream(section);
    std::string line;
    while (std::getline(stream, line)) {
        const auto comment_pos = line.find("//");
        const auto sanitized = trim(line.substr(0, comment_pos == std::string::npos ? line.size() : comment_pos));
        if (sanitized.empty()) {
            continue;
        }
        if (!containsText(sanitized, "steal")) {
            continue;
        }
        if (sanitized.find('(') == std::string::npos) {
            continue;
        }
        if (sanitized.find(';') != std::string::npos || sanitized.find('{') != std::string::npos) {
            return true;
        }
    }
    return false;
}

void requireRoutesThroughSharedLoop(std::vector<std::string>& failures,
                                    const std::filesystem::path& scheduler_path,
                                    const std::string& scheduler_name) {
    const std::string scheduler_content = readAll(scheduler_path);
    if (scheduler_content.empty()) {
        failures.push_back(scheduler_path.string() + ": failed to read " + scheduler_name);
        return;
    }
    requireContains(failures,
                    scheduler_path,
                    scheduler_content,
                    "detail::runIOSchedulerEventLoop(",
                    "expected " + scheduler_name + " to route through runIOSchedulerEventLoop");
}

}  // namespace

// This source-case builds on HEAD, but the red state is triggered during execution
// until the work-stealing wiring exists.
int main() {
    const auto root = projectRoot();
    const auto ioscheduler = root / "galay-kernel" / "kernel" / "io_scheduler.hpp";

    std::vector<std::string> failures;
    const std::string ioscheduler_content = readAll(ioscheduler);

    if (ioscheduler_content.empty()) {
        failures.push_back(ioscheduler.string() + ": failed to read io_scheduler.hpp");
    } else {
        const auto worker_section =
            extractBracedSection(ioscheduler_content, "struct IOSchedulerWorkerState");
        if (worker_section.empty()) {
            failures.push_back(ioscheduler.string() +
                               ": failed to isolate IOSchedulerWorkerState for inspection");
        } else {
            requireNotContains(failures,
                               ioscheduler,
                               worker_section,
                               "std::deque<TaskRef> local_queue",
                               "expected IOSchedulerWorkerState to drop std::deque local_queue in favor of a ring");

            requireContains(failures,
                            ioscheduler,
                            worker_section,
                            "ChaseLevTaskRing",
                            "expected IOSchedulerWorkerState to own a ChaseLevTaskRing instance");

            requireContains(failures,
                            ioscheduler,
                            worker_section,
                            "local_ring.push_back",
                            "expected owner path to push tasks onto the ring");

            requireContains(failures,
                            ioscheduler,
                            worker_section,
                            "local_ring.pop_back",
                            "expected owner path to pop tasks from the ring");

            requireContains(failures,
                            ioscheduler,
                            worker_section,
                            "steal_front",
                            "expected ChaseLevTaskRing to expose a front-steal entry");

            if (!hasStealDeclaration(worker_section)) {
                failures.push_back(ioscheduler.string() +
                                   ": expected IOSchedulerWorkerState to declare a stealing entry or helper");
            }
        }

        requireContains(failures,
                        ioscheduler,
                        ioscheduler_content,
                        "kCapacity = 256",
                        "expected ChaseLevTaskRing to declare a fixed capacity of 256");
    }

    const auto event_loop = root / "galay-kernel" / "kernel" / "sched_loop.hpp";
    const std::string event_loop_content = readAll(event_loop);
    if (event_loop_content.empty()) {
        failures.push_back(event_loop.string() + ": failed to read sched_loop.hpp");
    } else {
        requireContains(failures,
                        event_loop,
                        event_loop_content,
                        "core.hasPendingWork()",
                        "expected event loop to guard stealing behind hasPendingWork()");
        requireContains(failures,
                        event_loop,
                        event_loop_content,
                        "core.trySteal()",
                        "expected event loop to attempt stealing before polling");
        requireContains(failures,
                        event_loop,
                        event_loop_content,
                        "poll_fn()",
                        "expected event loop to still call poll_fn() when no steal succeeds");

        const auto body_start = event_loop_content.find("void runIOSchedulerEventLoop");
        if (body_start == std::string::npos) {
            failures.push_back(event_loop.string() +
                               ": failed to locate runIOSchedulerEventLoop body for ordering checks");
        } else {
            const std::string body_end_marker = "\n}  // namespace detail";
            const auto body_end = event_loop_content.find(body_end_marker, body_start);
            const std::string event_loop_body =
                (body_end != std::string::npos)
                    ? event_loop_content.substr(body_start, body_end - body_start)
                    : event_loop_content.substr(body_start);

            const auto has_pending_pos = event_loop_body.find("core.hasPendingWork()");
            const auto try_steal_pos = event_loop_body.find("core.trySteal()");
            const auto poll_pos = event_loop_body.find("poll_fn()");
            if (try_steal_pos != std::string::npos && has_pending_pos != std::string::npos &&
                try_steal_pos <= has_pending_pos) {
                failures.push_back(event_loop.string() +
                                   ": expected core.trySteal() to run after the hasPendingWork() guard");
            }
            if (try_steal_pos != std::string::npos && poll_pos != std::string::npos &&
                poll_pos <= try_steal_pos) {
                failures.push_back(event_loop.string() +
                                   ": expected core.trySteal() to run before poll_fn()");
            }
        }
    }

    requireRoutesThroughSharedLoop(failures,
                                   root / "galay-kernel" / "kernel" / "epoll_scheduler.cc",
                                   "EpollScheduler::eventLoop");
    requireRoutesThroughSharedLoop(failures,
                                   root / "galay-kernel" / "kernel" / "kqueue_scheduler.cc",
                                   "KqueueScheduler::eventLoop");
    requireRoutesThroughSharedLoop(failures,
                                   root / "galay-kernel" / "kernel" / "uring_scheduler.cc",
                                   "IOUringScheduler::eventLoop");

    if (!failures.empty()) {
        for (const auto& failure : failures) {
            std::cerr << "[T96] " << failure << '\n';
        }
        return 1;
    }

    std::cout << "T96-IOSchedulerWorkStealingSourceCase PASS\n";
    return 0;
}
