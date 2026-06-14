/**
 * @file t115_taskstoresrc.cc
 * @brief 用途：锁定 Task 完成态内嵌与 TaskState 池化分配的源码边界。
 * 关键覆盖点：协程 Task/JoinHandle 不再持有独立 shared_ptr 完成态、TaskPromise 不再 make_shared、
 * TaskState 提供 class-specific operator new/delete 入口。
 * 通过条件：源码包含目标 token，且移除了旧的 shared_ptr 热路径。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::filesystem::path projectRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().lexically_normal();
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

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto task_h = root / "galay-kernel" / "kernel" / "task.h";
    const auto task_cc = root / "galay-kernel" / "kernel" / "task.cc";

    const std::string task_h_text = readAll(task_h);
    const std::string task_cc_text = readAll(task_cc);
    if (task_h_text.empty() || task_cc_text.empty()) {
        std::cerr << "[T115] failed to read Task sources\n";
        return 1;
    }

    if (containsText(task_h_text, "Task(TaskRef task, std::shared_ptr<TaskCompletionState<T>> completion)")) {
        std::cerr << "[T115] expected Task<T> coroutine path to stop storing shared_ptr completion state\n";
        return 1;
    }
    if (containsText(task_h_text, "Task(TaskRef task, std::shared_ptr<TaskCompletionState<void>> completion)")) {
        std::cerr << "[T115] expected Task<void> coroutine path to stop storing shared_ptr completion state\n";
        return 1;
    }
    if (containsText(task_h_text, "std::shared_ptr<TaskCompletionState<T>> m_completion")) {
        std::cerr << "[T115] expected Task<T> storage to stop embedding shared_ptr completion state\n";
        return 1;
    }
    if (containsText(task_h_text, "std::shared_ptr<TaskCompletionState<void>> m_completion")) {
        std::cerr << "[T115] expected Task<void> storage to stop embedding shared_ptr completion state\n";
        return 1;
    }
    if (containsText(task_h_text, "std::make_shared<TaskCompletionState<T>>()")) {
        std::cerr << "[T115] expected TaskPromise<T> to stop allocating shared_ptr completion state\n";
        return 1;
    }
    if (containsText(task_h_text, "std::make_shared<TaskCompletionState<void>>()")) {
        std::cerr << "[T115] expected TaskPromise<void> to stop allocating shared_ptr completion state\n";
        return 1;
    }
    if (!containsText(task_h_text, "static void* operator new(std::size_t size);")) {
        std::cerr << "[T115] expected TaskState to declare class-specific operator new\n";
        return 1;
    }
    if (!containsText(task_h_text, "static void operator delete(void* ptr, std::size_t size) noexcept;")) {
        std::cerr << "[T115] expected TaskState to declare sized operator delete\n";
        return 1;
    }
    if (!containsText(task_h_text, "std::atomic<TaskWaiter*> m_waiter{nullptr};")) {
        std::cerr << "[T115] expected TaskState to lazily own a waiter pointer\n";
        return 1;
    }
    if (!containsText(task_cc_text, "thread_local TaskStateFreeNode* g_taskStateFreeList = nullptr;")) {
        std::cerr << "[T115] expected task.cc to keep a thread-local TaskState freelist\n";
        return 1;
    }

    std::cout << "T115-RuntimeTaskStorageSourceCase PASS\n";
    return 0;
}
