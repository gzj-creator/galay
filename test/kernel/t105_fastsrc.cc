/**
 * @file t105_fastsrc.cc
 * @brief 用途：锁定第一批运行时 fast path 优化的源码边界。
 * 关键覆盖点：`accept4` + socket flags、kqueue `EVFILT_USER` 唤醒、io_uring 稳定 token 池。
 * 通过条件：源码包含目标 token，且不再保留旧的堆分配/pipe 唤醒路径。
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
    const auto iohandlers = root / "galay-kernel" / "kernel" / "io_handlers.hpp";
    const auto iocontroller = root / "galay-kernel" / "kernel" / "io_controller.hpp";
    const auto iouring = root / "galay-kernel" / "kernel" / "uring_reactor.cc";
    const auto kqueue = root / "galay-kernel" / "kernel" / "kqueue_reactor.cc";

    const std::string iohandlers_text = readAll(iohandlers);
    const std::string iocontroller_text = readAll(iocontroller);
    const std::string iouring_text = readAll(iouring);
    const std::string kqueue_text = readAll(kqueue);

    if (iohandlers_text.empty() || iocontroller_text.empty() ||
        iouring_text.empty() || kqueue_text.empty()) {
        std::cerr << "[T105] failed to read source files\n";
        return 1;
    }

    if (!containsText(iohandlers_text, "accept4(")) {
        std::cerr << "[T105] expected IOHandlers accept path to use accept4\n";
        return 1;
    }
    if (!containsText(iohandlers_text, "SOCK_NONBLOCK | SOCK_CLOEXEC")) {
        std::cerr << "[T105] expected IOHandlers accept path to request nonblock + cloexec\n";
        return 1;
    }
    if ((!containsText(iouring_text, "io_uring_prep_accept(") &&
         !containsText(iouring_text, "io_uring_prep_multishot_accept(")) ||
        !containsText(iouring_text, "SOCK_NONBLOCK | SOCK_CLOEXEC")) {
        std::cerr << "[T105] expected io_uring accept path to request nonblock + cloexec\n";
        return 1;
    }

    if (!containsText(kqueue_text, "EVFILT_USER")) {
        std::cerr << "[T105] expected kqueue wakeup path to use EVFILT_USER\n";
        return 1;
    }
    if (containsText(kqueue_text, "pipe(m_notify_pipe)")) {
        std::cerr << "[T105] expected kqueue wakeup path to stop creating a notify pipe\n";
        return 1;
    }
    if (!containsText(kqueue_text, "NOTE_TRIGGER")) {
        std::cerr << "[T105] expected kqueue notify path to trigger NOTE_TRIGGER\n";
        return 1;
    }

    if (containsText(iocontroller_text, "new (std::nothrow) SqeRequestToken")) {
        std::cerr << "[T105] expected IOController to stop heap allocating SqeRequestToken\n";
        return 1;
    }
    if (!containsText(iocontroller_text, "std::shared_ptr<SqeTokenArena> m_sqe_token_pool[SIZE]")) {
        std::cerr << "[T105] expected IOController to keep per-slot stable SqeTokenArena pools\n";
        return 1;
    }
    if (containsText(iouring_text, "std::unique_ptr<SqeRequestToken> token")) {
        std::cerr << "[T105] expected IOUringReactor completion path to stop owning token via unique_ptr\n";
        return 1;
    }

    std::cout << "T105-RuntimeFastpathSourceCase PASS\n";
    return 0;
}
