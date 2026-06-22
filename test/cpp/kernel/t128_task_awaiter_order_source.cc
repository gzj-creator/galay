/**
 * @file t128_task_awaiter_order_source.cc
 * @brief 锁定 TaskAwaiter 注册 continuation 与调度子任务的顺序。
 *
 * 关键覆盖点：
 * - `TaskAwaiter::await_suspend()` 必须先把父任务写入子任务 `m_next`，
 *   再调用 `scheduleTaskImmediately(childTask)`。
 * - 防止立即完成的子任务在 continuation 尚未注册时结束，导致父任务永久挂起。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::filesystem::path projectRoot() {
    return std::filesystem::path(GALAY_SOURCE_ROOT);
}

std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string extractFunction(const std::string& content, const std::string& marker) {
    const auto begin_pos = content.find(marker);
    if (begin_pos == std::string::npos) {
        return {};
    }

    const auto brace_pos = content.find('{', begin_pos + marker.size());
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
            return content.substr(begin_pos, i + 1 - begin_pos);
        }
    }
    return {};
}

}  // namespace

int main() {
    const auto task_path = projectRoot() / "galay-kernel" / "core" / "task.h";
    const std::string task = readAll(task_path);
    if (task.empty()) {
        std::cerr << "[T128] failed to read " << task_path << '\n';
        return 1;
    }

    const std::string await_suspend =
        extractFunction(task, "bool await_suspend(std::coroutine_handle<Promise> handle)");
    if (await_suspend.empty()) {
        std::cerr << "[T128] failed to locate TaskAwaiter::await_suspend\n";
        return 1;
    }

    const auto attach_pos = await_suspend.find("childTask.state()->m_next");
    const auto schedule_pos = await_suspend.find("scheduleTaskImmediately(childTask)");
    if (attach_pos == std::string::npos) {
        std::cerr << "[T128] TaskAwaiter::await_suspend must attach childTask.m_next\n";
        return 1;
    }
    if (schedule_pos == std::string::npos) {
        std::cerr << "[T128] TaskAwaiter::await_suspend must schedule child task immediately\n";
        return 1;
    }
    if (attach_pos > schedule_pos) {
        std::cerr << "[T128] TaskAwaiter registers continuation after scheduling child task\n";
        return 1;
    }

    const auto done_after_schedule_pos = await_suspend.find("if (m_task.done())", schedule_pos);
    if (done_after_schedule_pos != std::string::npos) {
        const auto return_false_pos = await_suspend.find("return false;", done_after_schedule_pos);
        if (return_false_pos != std::string::npos) {
            std::cerr << "[T128] TaskAwaiter must not return false after scheduling a child "
                         "that may already have queued the parent continuation\n";
            return 1;
        }
    }

    std::cout << "T128-TaskAwaiterOrderSourceCase PASS\n";
    return 0;
}
