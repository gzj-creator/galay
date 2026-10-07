/**
 * @file t104_iouoffsrc.cc
 * @brief 锁定 IO 后端禁用 work-stealing 的源码边界。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

}  // namespace

int main() {
    const auto root = project_root();
    const auto ioscheduler = root / "galay-kernel" / "core" / "io_ready_queue.cc";
    const auto scheduler_base = root / "galay-kernel" / "core" / "io_scheduler_base.hpp";

    std::vector<std::string> failures;

    const auto ioscheduler_src = read_all(ioscheduler);
    if (ioscheduler_src.empty()) {
        failures.push_back(ioscheduler.string() + ": failed to read io_ready_queue.cc");
    } else if (!contains(ioscheduler_src, "!stealing_enabled")) {
        failures.push_back(ioscheduler.string() +
                           ": try_steal() must guard on worker stealing_enabled");
    }

    const auto scheduler_src = read_all(scheduler_base);
    if (!contains(scheduler_src, "m_worker.set_stealing_enabled(false);")) {
        failures.push_back(scheduler_base.string() +
                           ": shared IO scheduler must disable sibling work-stealing");
    }

    if (!failures.empty()) {
        for (const auto& failure : failures) {
            std::cerr << "[T104] " << failure << '\n';
        }
        return 1;
    }

    std::cout << "T104-IOUringStealDisabledSourceCase PASS\n";
    return 0;
}
