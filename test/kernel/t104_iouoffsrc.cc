/**
 * @file t104_iouoffsrc.cc
 * @brief 锁定 io_uring 后端禁用 work-stealing 的源码边界。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto ioscheduler = root / "galay-kernel" / "kernel" / "io_scheduler.hpp";
    const auto io_uring_scheduler = root / "galay-kernel" / "kernel" / "uring_scheduler.cc";

    std::vector<std::string> failures;

    const auto ioscheduler_src = readAll(ioscheduler);
    if (ioscheduler_src.empty()) {
        failures.push_back(ioscheduler.string() + ": failed to read io_scheduler.hpp");
    } else if (!contains(ioscheduler_src, "!stealing_enabled")) {
        failures.push_back(ioscheduler.string() +
                           ": trySteal() must guard on worker stealing_enabled");
    }

    const auto io_uring_src = readAll(io_uring_scheduler);
    if (io_uring_src.empty()) {
        failures.push_back(io_uring_scheduler.string() + ": failed to read uring_scheduler.cc");
    } else if (!contains(io_uring_src, "m_worker.setStealingEnabled(false);")) {
        failures.push_back(io_uring_scheduler.string() +
                           ": io_uring scheduler must disable sibling work-stealing");
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
