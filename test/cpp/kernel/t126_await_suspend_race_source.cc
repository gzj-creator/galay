/**
 * @file t126_await_suspend_race_source.cc
 * @brief 锁定跨 scheduler wake 与 await_suspend 竞态的源码边界。
 *
 * 关键覆盖点：
 * - MpscChannel 注册 waiter 后只消费提前唤醒，不再继续消费并写入 awaiter frame。
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
                           const std::string& label) {
    const std::string section = extractFunction(content, marker);
    if (section.empty()) {
        failures.push_back(path.string() + ": failed to locate " + label);
        return;
    }

    requireContains(failures,
                    path,
                    section,
                    "publishWaiter(waiter_state)",
                    label + " should compensate a lost wake through publishWaiter(waiter_state)");

    const auto publish_pos = section.find("publishWaiter");
    if (publish_pos == std::string::npos) {
        failures.push_back(path.string() + ": " + label + " should publish a waiter");
        return;
    }
    const std::string after_publish = section.substr(publish_pos);
    requireNotContains(failures,
                       path,
                       after_publish,
                       "tryReceiveNow()",
                       label + " must not call tryReceiveNow() after publishing the waiter");
}

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto mpsc_path = root / "galay-kernel" / "concurrency" / "mpsc_channel.h";
    const auto waiter_path = root / "galay-kernel" / "concurrency" / "async_waiter.h";
    const auto mutex_path = root / "galay-kernel" / "concurrency" / "async_mutex.h";

    std::vector<std::string> failures;

    const std::string mpsc = readAll(mpsc_path);
    if (mpsc.empty()) {
        failures.push_back(mpsc_path.string() + ": failed to read mpsc_channel.h");
    } else {
        checkMpscAwaitSuspend(
            failures,
            mpsc_path,
            mpsc,
            "inline bool MpscRecvAwaitable<T>::await_suspend",
            "MpscRecvAwaitable::await_suspend");
        checkMpscAwaitSuspend(
            failures,
            mpsc_path,
            mpsc,
            "inline bool MpscRecvBatchAwaitable<T>::await_suspend",
            "MpscRecvBatchAwaitable::await_suspend");
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

    if (!failures.empty()) {
        for (const auto& failure : failures) {
            std::cerr << "[T126] " << failure << '\n';
        }
        return 1;
    }

    std::cout << "T126-AwaitSuspendRaceSourceCase PASS\n";
    return 0;
}
