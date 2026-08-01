/**
 * @file t126_await_suspend_race_source.cc
 * @brief 锁定跨 scheduler wake 与 await_suspend 竞态的源码边界。
 *
 * 关键覆盖点：
 * - galay::mpsc::UnboundedChannel 在 waiter 进入可唤醒状态前完成最后一次就绪重检，
 *   发布 waiter 后不再访问 awaiter frame。
 * - WithTimeout 在 inner 发布 waiter 前复制 timer/scheduler，发布后只访问栈上副本。
 * - AsyncWaiter 使用 empty/waiting/ready 状态机，避免 ready 与 notify 竞态下双恢复。
 * - AsyncMutex 发布 waiter 后只使用栈上本地副本，避免被跨线程恢复后继续触碰 awaiter frame。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

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

bool containsText(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
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

void checkMpscAwaitSuspend(std::vector<std::string>& failures,
                           const std::filesystem::path& path,
                           const std::string& content,
                           const std::string& marker,
                           const std::string& label,
                           const std::string& retry_call) {
    const std::string section = extractFunction(content, marker);
    if (section.empty()) {
        failures.push_back(path.string() + ": failed to locate " + label);
        return;
    }

    requireContains(failures,
                    path,
                    section,
                    "beginWaiterRegistration()",
                    label + " should enter the non-wakeable arming phase before the final retry");
    const std::string publish_call =
        "return channel->publishWaiter(waiterState, std::move(timeoutTimer));";
    requireContains(failures,
                    path,
                    section,
                    publish_call,
                    label + " should make waiter publication the final frame-touching operation");

    const auto begin_pos = section.find("beginWaiterRegistration()");
    const auto retry_pos = section.find(retry_call, begin_pos);
    const auto publish_pos = section.find(publish_call);
    if (begin_pos == std::string::npos || retry_pos == std::string::npos ||
        publish_pos == std::string::npos || begin_pos >= retry_pos || retry_pos >= publish_pos) {
        failures.push_back(path.string() + ": " + label +
                           " should arm without enabling wake, retry, then publish the waiter");
        return;
    }
    const std::string after_publish = section.substr(
        publish_pos + publish_call.size());
    requireNotContains(failures,
                       path,
                       after_publish,
                       "tryReceiveNow()",
                       label + " must not call tryReceiveNow() after publishing the waiter");
}

void checkWithTimeoutAwaitSuspend(std::vector<std::string>& failures,
                                  const std::filesystem::path& path,
                                  const std::string& content) {
    const std::string section = extractFunction(
        content, "bool await_suspend(std::coroutine_handle<Promise> handle)");
    if (section.empty()) {
        failures.push_back(path.string() + ": failed to locate WithTimeout::await_suspend");
        return;
    }

    requireContains(failures,
                    path,
                    section,
                    "auto timer = m_timer;",
                    "WithTimeout must retain the timer before inner waiter publication");
    requireContains(failures,
                    path,
                    section,
                    "Scheduler* scheduler = waker.getScheduler();",
                    "WithTimeout must copy the scheduler before inner waiter publication");

    const auto publish_pos = section.find("m_inner.await_suspend(handle)");
    if (publish_pos == std::string::npos) {
        failures.push_back(path.string() + ": failed to locate inner await_suspend publication");
        return;
    }
    const std::string publish_call = "m_inner.await_suspend(handle)";
    const std::string after_publish = section.substr(
        publish_pos + publish_call.size());
    requireNotContains(failures,
                       path,
                       after_publish,
                       "m_inner",
                       "WithTimeout must not access inner storage after waiter publication");
    requireNotContains(failures,
                       path,
                       after_publish,
                       "m_timer",
                       "WithTimeout must not access timer member after waiter publication");
    requireNotContains(failures,
                       path,
                       after_publish,
                       "m_scheduler",
                       "WithTimeout must not access scheduler member after waiter publication");
    requireContains(failures,
                    path,
                    after_publish,
                    "scheduler->addTimer(timer)",
                    "WithTimeout must register the retained local timer after suspension");
}

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto mpsc_path =
        root / "galay-kernel" / "concurrency" / "mpsc" / "unbounded_channel.h";
    const auto waiter_path = root / "galay-kernel" / "async" / "async_waiter.h";
    const auto mutex_path = root / "galay-kernel" / "async" / "async_mutex.h";
    const auto timeout_path = root / "galay-kernel" / "core" / "timeout.hpp";
    const auto c_bridge_path = std::filesystem::path(GALAY_PROJECT_ROOT) /
        "src" / "c" / "galay-bridge-c" / "coro-c" / "c_coro_async_waiter_bridge.cc";

    std::vector<std::string> failures;

    const std::string mpsc = readAll(mpsc_path);
    if (mpsc.empty()) {
        failures.push_back(mpsc_path.string() + ": failed to read mpsc/unbounded_channel.h");
    } else {
        checkMpscAwaitSuspend(
            failures,
            mpsc_path,
            mpsc,
            "inline bool UnboundedRecvAwaitable<T>::await_suspend",
            "mpsc::UnboundedRecvAwaitable::await_suspend",
            "tryReceiveNow()");
        checkMpscAwaitSuspend(
            failures,
            mpsc_path,
            mpsc,
            "inline bool UnboundedRecvBatchAwaitable<T>::await_suspend",
            "mpsc::UnboundedRecvBatchAwaitable::await_suspend",
            "hasPublishedValueForWaiter()");
    }

    const std::string timeout = readAll(timeout_path);
    if (timeout.empty()) {
        failures.push_back(timeout_path.string() + ": failed to read timeout.hpp");
    } else {
        checkWithTimeoutAwaitSuspend(failures, timeout_path, timeout);
    }

    const std::string waiter = readAll(waiter_path);
    if (waiter.empty()) {
        failures.push_back(waiter_path.string() + ": failed to read async_waiter.h");
    } else {
        const std::string typed_wait =
            extractFunction(waiter, "bool AsyncWaiterAwaitable<T>::await_suspend");
        const std::string void_wait =
            extractFunction(waiter, "inline bool AsyncWaiterAwaitable<void>::await_suspend");
        if (typed_wait.empty()) {
            failures.push_back(waiter_path.string() + ": failed to locate typed AsyncWaiter await_suspend");
        } else {
            requireContains(failures,
                            waiter_path,
                            typed_wait,
                            "AsyncWaiterState::kWaiting",
                            "typed AsyncWaiter must publish a waiting state as the final suspend decision");
            const auto publish_pos = typed_wait.find("AsyncWaiterState::kWaiting");
            if (publish_pos != std::string::npos) {
                requireNotContains(failures,
                                   waiter_path,
                                   typed_wait.substr(publish_pos),
                                   "m_ready.load",
                                   "typed AsyncWaiter must not re-read ready after publishing waiting state");
            }
        }
        if (void_wait.empty()) {
            failures.push_back(waiter_path.string() + ": failed to locate void AsyncWaiter await_suspend");
        } else {
            requireContains(failures,
                            waiter_path,
                            void_wait,
                            "AsyncWaiterState::kWaiting",
                            "void AsyncWaiter must publish a waiting state as the final suspend decision");
            const auto publish_pos = void_wait.find("AsyncWaiterState::kWaiting");
            if (publish_pos != std::string::npos) {
                requireNotContains(failures,
                                   waiter_path,
                                   void_wait.substr(publish_pos),
                                   "m_ready.load",
                                   "void AsyncWaiter must not re-read ready after publishing waiting state");
            }
        }
    }

    const std::string mutex = readAll(mutex_path);
    if (mutex.empty()) {
        failures.push_back(mutex_path.string() + ": failed to read async_mutex.h");
    } else {
        const std::string mutex_wait =
            extractFunction(mutex, "inline bool AsyncMutexAwaitable::await_suspend");
        if (mutex_wait.empty()) {
            failures.push_back(mutex_path.string() + ": failed to locate AsyncMutex await_suspend");
        } else {
            requireContains(failures,
                            mutex_path,
                            mutex_wait,
                            "auto* mutex = m_mutex;",
                            "AsyncMutex await_suspend should copy m_mutex before publishing waiter");
            requireContains(failures,
                            mutex_path,
                            mutex_wait,
                            "auto waiter = m_waiter;",
                            "AsyncMutex await_suspend should use a local waiter copy after enqueue");
        }
    }

    const std::string c_bridge = readAll(c_bridge_path);
    if (c_bridge.empty()) {
        failures.push_back(c_bridge_path.string() + ": failed to read c_coro_async_waiter_bridge.cc");
    } else {
        const std::string bridge_wait =
            extractFunction(c_bridge, "GalayCoreCoroIOResult galay_core_coro_async_waiter_wait");
        if (bridge_wait.empty()) {
            failures.push_back(c_bridge_path.string() + ": failed to locate galay_core_coro_async_waiter_wait");
        } else {
            const auto suspend_pos = bridge_wait.find("operation.awaitable.await_suspend");
            const auto wait_pos = bridge_wait.find("operation.wait(timeout_ms)");
            if (suspend_pos == std::string::npos || wait_pos == std::string::npos ||
                suspend_pos >= wait_pos) {
                failures.push_back(c_bridge_path.string() + ": failed to locate async waiter suspend/wait boundary");
            } else {
                requireContains(failures,
                                c_bridge_path,
                                bridge_wait.substr(suspend_pos, wait_pos - suspend_pos),
                                "await_resume()",
                                "C async waiter bridge must consume await_resume when await_suspend returns false");
            }
        }

        const std::string bridge_complete =
            extractFunction(c_bridge, "C_IOResult completeAndReleaseUserData");
        if (bridge_complete.empty()) {
            failures.push_back(c_bridge_path.string() + ": failed to locate completeAndReleaseUserData");
        } else {
            requireContains(failures,
                            c_bridge_path,
                            bridge_complete,
                            "auto release_user_data = m_wait_ops.release_user_data;",
                            "C async waiter bridge must copy release_user_data before completing user_data");
            const auto complete_pos = bridge_complete.find("complete_user_data(user_data, result)");
            if (complete_pos != std::string::npos) {
                requireNotContains(failures,
                                   c_bridge_path,
                                   bridge_complete.substr(complete_pos),
                                   "m_wait_ops.release_user_data",
                                   "C async waiter bridge must not read release_user_data after completion can resume");
            }
        }
    }

    if (!failures.empty()) {
        for (const auto& failure : failures) {
            std::cerr << "[T126] " << failure << '\n';
        }
        return 1;
    }

    std::cout << "T126-AwaitSuspendRaceSourceCase PASS\n";
    return 0;
}
