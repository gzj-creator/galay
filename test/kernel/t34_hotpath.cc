/**
 * @file t34_hotpath.cc
 * @brief 用途：验证 Awaitable 热路径辅助工具的状态传递与快速分支行为。
 * 关键覆盖点：热路径辅助函数、结果封装、分支裁剪以及状态读取语义。
 * 通过条件：辅助工具返回结果与状态均符合预期，测试返回 0。
 */

#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/core/task.h"
#include <cerrno>
#include <cstdint>
#include <iostream>

using namespace galay::kernel;

namespace {

Task<void> noopTask() {
    co_return;
}

bool verifyPromiseDirectAccessors() {
    Task<void> task_wrapper = noopTask();
    auto task = detail::TaskAccess::taskRef(task_wrapper);
    auto erased_handle = task.state()->m_handle;
    if (!erased_handle) {
        std::cerr << "[T34] task handle is invalid\n";
        return false;
    }
    auto handle = std::coroutine_handle<TaskPromise<void>>::from_address(
        erased_handle.address());

    const TaskRef& promise_task = handle.promise().taskRefView();
    if (!promise_task.isValid() || promise_task.state() != task.state()) {
        std::cerr << "[T34] promise taskRefView does not match task state\n";
        return false;
    }

    erased_handle.resume();
    return true;
}

template <typename ResultT>
uint32_t systemCode(const std::expected<ResultT, IOError>& result) {
    return static_cast<uint32_t>(result.error().code() >> 32);
}

bool verifyAwaitableAddResultHelper() {
    {
        std::expected<size_t, IOError> result = static_cast<size_t>(7);
        if (detail::finalizeAwaitableAddResult(1, kSendFailed, result)) {
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
        if (!detail::finalizeAwaitableAddResult(0, kSendFailed, result)) {
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
        if (detail::finalizeAwaitableAddResult(-ECONNRESET, kRecvFailed, result)) {
            std::cerr << "[T34] negative errno path should not suspend\n";
            return false;
        }
        if (result || !IOError::contains(result.error().code(), kRecvFailed) ||
            systemCode(result) != static_cast<uint32_t>(ECONNRESET)) {
            std::cerr << "[T34] negative errno path should map ret to system code\n";
            return false;
        }
    }

    {
        errno = EPIPE;
        std::expected<void, IOError> result{};
        if (detail::finalizeAwaitableAddResult(-1, kSendFailed, result)) {
            std::cerr << "[T34] errno fallback path should not suspend\n";
            return false;
        }
        if (result || !IOError::contains(result.error().code(), kSendFailed) ||
            systemCode(result) != static_cast<uint32_t>(EPIPE)) {
            std::cerr << "[T34] errno fallback path should use errno\n";
            return false;
        }
    }

    return true;
}

}  // namespace

int main() {
    if (!verifyPromiseDirectAccessors()) {
        return 1;
    }

    if (!verifyAwaitableAddResultHelper()) {
        return 1;
    }

    std::cout << "T34-AwaitableHotPathHelpers PASS\n";
    return 0;
}
