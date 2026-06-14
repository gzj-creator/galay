/**
 * @file t98_stealdom.cc
 * @brief Surface-level assertions guarding Runtime-managed IO steal domain wiring
 *
 * Validates:
 * - `Runtime::start()` declares `configureIOSchedulerStealDomains()` and calls it before launching IO scheduler threads
 * - The helper walks `m_io_schedulers`, configures steal domains, and leaves compute schedulers untouched
 * - `IOSchedulerWorkerState` declares the steal-domain fields and helper
 */

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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string extractSection(const std::string& content,
                           const std::string& begin,
                           const std::string& end) {
    const auto begin_pos = content.find(begin);
    if (begin_pos == std::string::npos) {
        return {};
    }
    const auto end_pos = content.find(end, begin_pos + begin.size());
    if (end_pos == std::string::npos) {
        return content.substr(begin_pos);
    }
    return content.substr(begin_pos, end_pos - begin_pos);
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

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto runtime = root / "galay-kernel" / "kernel" / "runtime.cc";
    const auto ioscheduler = root / "galay-kernel" / "kernel" / "io_scheduler.hpp";

    std::vector<std::string> failures;

    const auto runtime_src = readAll(runtime);
    if (runtime_src.empty()) {
        failures.push_back(runtime.string() + ": failed to read runtime.cc");
    } else {
        const auto start_body = extractSection(runtime_src, "void Runtime::start()", "void Runtime::stop()");
        if (start_body.empty()) {
            failures.push_back(runtime.string() + ": failed to isolate Runtime::start()");
        } else {
            if (!contains(start_body, "configureIOSchedulerStealDomains()")) {
                failures.push_back(runtime.string() + ": start() missing configureIOSchedulerStealDomains() call");
            }
            if (!contains(start_body, "for (auto& scheduler : m_io_schedulers)")) {
                failures.push_back(runtime.string() + ": start() missing IO scheduler start loop");
            }
            const auto helper_call = start_body.find("configureIOSchedulerStealDomains()");
            const auto io_loop = start_body.find("for (auto& scheduler : m_io_schedulers)");
            if (helper_call != std::string::npos &&
                io_loop != std::string::npos &&
                helper_call > io_loop) {
                failures.push_back(runtime.string() +
                                   ": configureIOSchedulerStealDomains() must run before IO scheduler start loop");
            }
        }

        const auto helper_section =
            extractSection(runtime_src, "void Runtime::configureIOSchedulerStealDomains()", "void Runtime::start()");
        if (helper_section.empty()) {
            failures.push_back(runtime.string() + ": missing configureIOSchedulerStealDomains() definition");
        } else {
            if (!contains(helper_section, "m_io_schedulers")) {
                failures.push_back(runtime.string() + ": helper must iterate m_io_schedulers");
            }
            if (!contains(helper_section, "configureStealDomain")) {
                failures.push_back(runtime.string() + ": helper must call configureStealDomain()");
            }
            if (contains(helper_section, "m_compute_schedulers")) {
                failures.push_back(runtime.string() + ": helper must avoid compute schedulers");
            }
        }
    }

    const auto ioscheduler_src = readAll(ioscheduler);
    if (ioscheduler_src.empty()) {
        failures.push_back(ioscheduler.string() + ": failed to read io_scheduler.hpp");
    } else {
        const auto worker_section =
            extractBracedSection(ioscheduler_src, "struct IOSchedulerWorkerState");
        if (worker_section.empty()) {
            failures.push_back(ioscheduler.string() + ": failed to isolate IOSchedulerWorkerState");
        } else {
            if (!contains(worker_section, "self_index")) {
                failures.push_back(ioscheduler.string() + ": worker state missing self_index");
            }
            if (!contains(worker_section, "std::span<IOScheduler* const>")) {
                failures.push_back(ioscheduler.string() + ": worker state missing sibling span");
            }
            if (!contains(worker_section, "random_seed")) {
                failures.push_back(ioscheduler.string() + ": worker state missing random_seed");
            }
            if (!contains(worker_section, "configureStealDomain(")) {
                failures.push_back(ioscheduler.string() + ": worker state missing configureStealDomain helper");
            }
        }
    }

    const std::vector<std::filesystem::path> scheduler_headers = {
        root / "galay-kernel" / "kernel" / "epoll_scheduler.h",
        root / "galay-kernel" / "kernel" / "kqueue_scheduler.h",
        root / "galay-kernel" / "kernel" / "uring_scheduler.h",
    };

    for (const auto& scheduler_path : scheduler_headers) {
        const auto scheduler_src = readAll(scheduler_path);
        if (scheduler_src.empty()) {
            failures.push_back(scheduler_path.string() + ": failed to read scheduler header");
            continue;
        }
        if (!contains(scheduler_src, "m_worker.configureStealDomain")) {
            failures.push_back(scheduler_path.string() +
                               ": scheduler must propagate configureStealDomain() to m_worker");
        }
    }

    if (!failures.empty()) {
        for (const auto& failure : failures) {
            std::cerr << "[T98] " << failure << '\n';
        }
        return 1;
    }

    std::cout << "T98-RuntimeIOStealDomainSurface PASS\\n";
    return 0;
}
