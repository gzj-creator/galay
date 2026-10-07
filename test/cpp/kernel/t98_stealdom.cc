/**
 * @file t98_stealdom.cc
 * @brief Surface-level assertions guarding Runtime-managed IO steal domain wiring
 *
 * Validates:
 * - `Runtime::start()` declares `configure_io_scheduler_steal_domains()` and calls it before launching IO scheduler threads
 * - The helper walks `m_io_schedulers`, configures steal domains, and leaves parallel schedulers untouched
 * - `IOReadyQueue` declares the steal-domain fields and helper
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path project_root() {
    return std::filesystem::path(GALAY_SOURCE_ROOT);
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string extract_section(const std::string& content,
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

std::string extract_braced_section(const std::string& content,
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
    const auto root = project_root();
    const auto runtime = root / "galay-kernel" / "core" / "runtime.cc";
    const auto ioscheduler = root / "galay-kernel" / "core" / "io_ready_queue.hpp";

    std::vector<std::string> failures;

    const auto runtime_src = read_all(runtime);
    if (runtime_src.empty()) {
        failures.push_back(runtime.string() + ": failed to read runtime.cc");
    } else {
        const auto start_body = extract_section(
            runtime_src,
            "std::expected<void, RuntimeError> Runtime::start()",
            "void Runtime::stop()");
        if (start_body.empty()) {
            failures.push_back(runtime.string() + ": failed to isolate Runtime::start()");
        } else {
            if (!contains(start_body, "configure_io_scheduler_steal_domains()")) {
                failures.push_back(runtime.string() + ": start() missing configure_io_scheduler_steal_domains() call");
            }
            if (!contains(start_body, "for (auto& scheduler : m_io_schedulers)")) {
                failures.push_back(runtime.string() + ": start() missing IO scheduler start loop");
            }
            const auto helper_call = start_body.find("configure_io_scheduler_steal_domains()");
            const auto io_loop = start_body.find("for (auto& scheduler : m_io_schedulers)");
            if (helper_call != std::string::npos &&
                io_loop != std::string::npos &&
                helper_call > io_loop) {
                failures.push_back(runtime.string() +
                                   ": configure_io_scheduler_steal_domains() must run before IO scheduler start loop");
            }
        }

        const auto helper_section =
            extract_section(
                runtime_src,
                "void Runtime::configure_io_scheduler_steal_domains()",
                "std::expected<void, RuntimeError> Runtime::start()");
        if (helper_section.empty()) {
            failures.push_back(runtime.string() + ": missing configure_io_scheduler_steal_domains() definition");
        } else {
            if (!contains(helper_section, "m_io_schedulers")) {
                failures.push_back(runtime.string() + ": helper must iterate m_io_schedulers");
            }
            if (!contains(helper_section, "configure_steal_domain")) {
                failures.push_back(runtime.string() + ": helper must call configure_steal_domain()");
            }
            if (contains(helper_section, "m_parallel_schedulers")) {
                failures.push_back(runtime.string() + ": helper must avoid parallel schedulers");
            }
        }
    }

    const auto ioscheduler_src = read_all(ioscheduler);
    if (ioscheduler_src.empty()) {
        failures.push_back(ioscheduler.string() + ": failed to read io_scheduler.hpp");
    } else {
        const auto worker_section =
            extract_braced_section(ioscheduler_src, "struct IOReadyQueue");
        if (worker_section.empty()) {
            failures.push_back(ioscheduler.string() + ": failed to isolate IOReadyQueue");
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
            if (!contains(worker_section, "configure_steal_domain(")) {
                failures.push_back(ioscheduler.string() + ": worker state missing configure_steal_domain helper");
            }
        }
    }

    const std::vector<std::filesystem::path> scheduler_headers = {
        root / "galay-kernel" / "core" / "io_scheduler_base.hpp",
    };

    for (const auto& scheduler_path : scheduler_headers) {
        const auto scheduler_src = read_all(scheduler_path);
        if (scheduler_src.empty()) {
            failures.push_back(scheduler_path.string() + ": failed to read scheduler header");
            continue;
        }
        if (!contains(scheduler_src, "m_worker.configure_steal_domain")) {
            failures.push_back(scheduler_path.string() +
                               ": scheduler must propagate configure_steal_domain() to m_worker");
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
