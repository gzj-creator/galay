/**
 * @file t34_hotpath.cc
 * @brief 用途：验证 Awaitable 热路径辅助工具的状态传递与快速分支行为。
 * 关键覆盖点：热路径辅助函数、结果封装、分支裁剪以及状态读取语义。
 * 通过条件：辅助工具返回结果与状态均符合预期，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <cerrno>
#include <cstdint>
#include <iostream>

using namespace galay::kernel;

namespace {

Task<void> noop_task() {
    co_return;
}

bool verify_promise_direct_accessors() {
    Task<void> task_wrapper = noop_task();
    auto task = detail::TaskAccess::task_ref(task_wrapper);
    auto erased_handle = task.state()->m_handle;
    if (!erased_handle) {
        std::cerr << "[T34] task handle is invalid\n";
        return false;
    }
    auto handle = std::coroutine_handle<TaskPromise<void>>::from_address(
        erased_handle.address());

    const TaskRef& promise_task = handle.promise().task_ref_view();
    if (!promise_task.is_valid() || promise_task.state() != task.state()) {
        std::cerr << "[T34] promise taskRefView does not match task state\n";
        return false;
    }

    erased_handle.resume();
    return true;
}

template <typename ResultT>
uint32_t system_code(const std::expected<ResultT, IOError>& result) {
    return static_cast<uint32_t>(result.error().code() >> 32);
}

bool verify_awaitable_add_result_helper() {
    {
        std::expected<size_t, IOError> result = static_cast<size_t>(7);
        if (detail::finalize_awaitable_add_result(1, kSendFailed, result)) {
            std::cerr << "[T34] OK path should not suspend\n";
            return false;
        }
        if (!result || *result != 7) {
            std::cerr << "[T34] OK path should preserve successful result\n";
            return false;
        }
    }

    {
        std::expected<size_t, IOError> result = static_cast<size_t>(11);
        if (!detail::finalize_awaitable_add_result(0, kSendFailed, result)) {
            std::cerr << "[T34] pending path should suspend\n";
            return false;
        }
        if (!result || *result != 11) {
            std::cerr << "[T34] pending path should preserve result payload\n";
            return false;
        }
    }

    {
        std::expected<size_t, IOError> result = static_cast<size_t>(0);
        if (detail::finalize_awaitable_add_result(-ECONNRESET, kRecvFailed, result)) {
            std::cerr << "[T34] negative errno path should not suspend\n";
            return false;
        }
        if (result || !IOError::contains(result.error().code(), kRecvFailed) ||
            system_code(result) != static_cast<uint32_t>(ECONNRESET)) {
            std::cerr << "[T34] negative errno path should map ret to system code\n";
            return false;
        }
    }

    {
        errno = EPIPE;
        std::expected<void, IOError> result{};
        if (detail::finalize_awaitable_add_result(-1, kSendFailed, result)) {
            std::cerr << "[T34] errno fallback path should not suspend\n";
            return false;
        }
        if (result || !IOError::contains(result.error().code(), kSendFailed) ||
            system_code(result) != static_cast<uint32_t>(EPIPE)) {
            std::cerr << "[T34] errno fallback path should use errno\n";
            return false;
        }
    }

    return true;
}

}  // namespace

int main() {
    if (!verify_promise_direct_accessors()) {
        return 1;
    }

    if (!verify_awaitable_add_result_helper()) {
        return 1;
    }

    std::cout << "T34-AwaitableHotPathHelpers PASS\n";
    return 0;
}
