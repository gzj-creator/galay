/**
 * @file t94_alignsrc.cc
 * @brief 用途：锁定运行时修复、平台声明和构建入口的源码收口结果。
 * 关键覆盖点：Timer 原子 flag、Windogalay-ws/IOCP 收口、Bazel BUILD、concurrentqueue 建模、
 * AioCommitAwaitable 的 scheduler 空指针保护、宏污染清理。
 * 通过条件：关键源码 token 与顺序满足预期，测试返回 0。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectRoot() {
    return std::filesystem::path(GALAY_PROJECT_ROOT);
}

std::filesystem::path sourceRoot() {
    return std::filesystem::path(GALAY_SOURCE_ROOT);
}

std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
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

void requireOrdered(std::vector<std::string>& failures,
                    const std::filesystem::path& path,
                    const std::string& content,
                    const std::string& first,
                    const std::string& second,
                    const std::string& message) {
    const auto first_pos = content.find(first);
    const auto second_pos = content.find(second);
    if (first_pos == std::string::npos || second_pos == std::string::npos || first_pos >= second_pos) {
        failures.push_back(path.string() + ": " + message);
    }
}

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto source_root = sourceRoot();

    const auto timer_hpp = source_root / "galay-kernel" / "common" / "timer.hpp";
    const auto option_cmake = root / "cmake" / "option.cmake";
    const auto root_cmake = root / "CMakeLists.txt";
    const auto root_build = root / "BUILD.bazel";
    const auto kernel_build = source_root / "galay-kernel" / "BUILD";
    const auto kernel_cmake = source_root / "galay-kernel" / "CMakeLists.txt";
    const auto package_config = root / "cmake" / "galay-module-config.cmake.in";
    const auto aio_file_h = source_root / "galay-kernel" / "async" / "aio_file.h";
    const auto defn_hpp = source_root / "galay-kernel" / "common" / "defn.hpp";
    const auto kqueue_scheduler_h = source_root / "galay-kernel" / "core" / "kqueue_scheduler.h";

    std::vector<std::string> failures;

    const std::string timer_content = readAll(timer_hpp);
    const std::string option_content = readAll(option_cmake);
    const std::string root_cmake_content = readAll(root_cmake);
    const std::string root_build_content = readAll(root_build);
    const std::string kernel_build_content = readAll(kernel_build);
    const std::string kernel_cmake_content = readAll(kernel_cmake);
    const std::string package_config_content = readAll(package_config);
    const std::string aio_content = readAll(aio_file_h);
    const std::string defn_content = readAll(defn_hpp);
    const std::string kqueue_scheduler_content = readAll(kqueue_scheduler_h);

    if (timer_content.empty()) failures.push_back(timer_hpp.string() + ": failed to read file");
    if (option_content.empty()) failures.push_back(option_cmake.string() + ": failed to read file");
    if (root_cmake_content.empty()) failures.push_back(root_cmake.string() + ": failed to read file");
    if (root_build_content.empty()) failures.push_back(root_build.string() + ": failed to read file");
    if (kernel_cmake_content.empty()) failures.push_back(kernel_cmake.string() + ": failed to read file");
    if (package_config_content.empty()) failures.push_back(package_config.string() + ": failed to read file");
    if (aio_content.empty()) failures.push_back(aio_file_h.string() + ": failed to read file");
    if (defn_content.empty()) failures.push_back(defn_hpp.string() + ": failed to read file");
    if (kqueue_scheduler_content.empty()) failures.push_back(kqueue_scheduler_h.string() + ": failed to read file");
    if (!std::filesystem::exists(kernel_build)) failures.push_back(kernel_build.string() + ": missing Bazel BUILD file");

    requireContains(failures,
                    timer_hpp,
                    timer_content,
                    "enum class TimerFlag : int",
                    "expected TimerFlag enum class");
    requireContains(failures,
                    timer_hpp,
                    timer_content,
                    "std::atomic<int> m_flag{0};",
                    "expected atomic timer flag");
    requireContains(failures,
                    timer_hpp,
                    timer_content,
                    "load(std::memory_order_acquire)",
                    "expected acquire loads for timer flag reads");
    requireContains(failures,
                    timer_hpp,
                    timer_content,
                    "fetch_or(",
                    "expected release fetch_or writes for timer flag updates");
    requireNotContains(failures,
                       timer_hpp,
                       timer_content,
                       "#define DONE",
                       "expected DONE macro removal");
    requireNotContains(failures,
                       timer_hpp,
                       timer_content,
                       "#define CANCEL",
                       "expected CANCEL macro removal");
    requireNotContains(failures,
                       timer_hpp,
                       timer_content,
                       "#define TIMEOUT",
                       "expected TIMEOUT macro removal");

    requireNotContains(failures,
                       root_cmake,
                       root_cmake_content,
                       "GALAY_KERNEL_BACKEND STREQUAL \"iocp\"",
                       "expected top-level backend allow-list to drop iocp");

    requireContains(failures,
                    kernel_build,
                    kernel_build_content,
                    "cc_library(",
                    "expected galay-kernel/BUILD to define a cc_library");
    requireContains(failures,
                    kernel_build,
                    kernel_build_content,
                    "name = \"galay-kernel\"",
                    "expected galay-kernel Bazel target name");

    requireContains(failures,
                    kernel_cmake,
                    kernel_cmake_content,
                    "CONFIGURE_DEPENDS",
                    "expected GLOB_RECURSE to use CONFIGURE_DEPENDS");

    requireContains(failures,
                    aio_file_h,
                    aio_content,
                    "auto scheduler = m_waker.getScheduler();",
                    "expected AioCommitAwaitable to fetch scheduler before registration");
    requireContains(failures,
                    aio_file_h,
                    aio_content,
                    "scheduler == nullptr",
                    "expected AioCommitAwaitable to guard null scheduler");
    requireOrdered(failures,
                   aio_file_h,
                   aio_content,
                   "auto scheduler = m_waker.getScheduler();",
                   "m_controller->m_handle.fd = m_event_fd;",
                   "expected scheduler lookup before controller state write");
    requireOrdered(failures,
                   aio_file_h,
                   aio_content,
                   "auto scheduler = m_waker.getScheduler();",
                   "m_controller->fillAwaitable(",
                   "expected scheduler lookup before controller awaitable binding");
    requireOrdered(failures,
                   aio_file_h,
                   aio_content,
                   "scheduler == nullptr",
                   "scheduler->type() != galay::kernel::kIOScheduler",
                   "expected null guard before scheduler type dereference");

    requireNotContains(failures,
                       defn_hpp,
                       defn_content,
                       "#define close(x) closesocket(x)",
                       "expected close macro removal");
    requireNotContains(failures,
                       defn_hpp,
                       defn_content,
                       "bool operator==(GHandle&& other)",
                       "expected GHandle rvalue equality overload removal");
    requireContains(failures,
                    defn_hpp,
                    defn_content,
                    "bool operator==(const GHandle& other) const",
                    "expected const GHandle equality overload");

    requireNotContains(failures,
                       kqueue_scheduler_h,
                       kqueue_scheduler_content,
                       "#define  OK 1",
                       "expected KqueueScheduler OK macro removal");
    requireNotContains(failures,
                       kqueue_scheduler_h,
                       kqueue_scheduler_content,
                       "#define OK 1",
                       "expected KqueueScheduler OK macro removal");

    if (!failures.empty()) {
        std::cerr << "T94 runtime/build alignment violations:\n";
        for (const auto& failure : failures) {
            std::cerr << "  - " << failure << '\n';
        }
        return 1;
    }

    std::cout << "T94-RuntimeAlignmentSourceCase PASS\n";
    return 0;
}
