/**
 * @file t141_file_watch_ready_queue.cc
 * @brief 验证 FileWatchAwaitable 能逐个消费已 drain 的文件事件队列。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>

#include <deque>
#include <iostream>

using galay::kernel::FileWatchAwaitable;
using galay::kernel::FileWatchEvent;
using galay::kernel::FileWatchResult;
using galay::kernel::IOController;

namespace {

bool expectReadyEvent(FileWatchAwaitable& awaitable,
                      const char* expected_name,
                      FileWatchEvent expected_event)
{
    if (!awaitable.await_ready()) {
        std::cerr << "queued file-watch event was not ready\n";
        return false;
    }
    auto result = awaitable.await_resume();
    if (!result) {
        std::cerr << "queued file-watch event returned error\n";
        return false;
    }
    if (result->name != expected_name) {
        std::cerr << "unexpected event name: " << result->name << "\n";
        return false;
    }
    if (!result->has(expected_event)) {
        std::cerr << "unexpected event mask\n";
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    IOController controller(GHandle::invalid());
    char buffer[1] = {};
    std::deque<FileWatchResult> ready_events;
    ready_events.push_back(FileWatchResult{"first.txt", FileWatchEvent::Create, false});
    ready_events.push_back(FileWatchResult{"second.txt", FileWatchEvent::Delete, false});

#ifdef USE_KQUEUE
    FileWatchAwaitable first(&controller,
                             buffer,
                             sizeof(buffer),
                             FileWatchEvent::All,
                             &ready_events);
#else
    FileWatchAwaitable first(&controller, buffer, sizeof(buffer), &ready_events);
#endif
    if (!expectReadyEvent(first, "first.txt", FileWatchEvent::Create)) {
        return 1;
    }
    if (ready_events.size() != 1) {
        std::cerr << "first awaitable did not consume exactly one event\n";
        return 1;
    }

#ifdef USE_KQUEUE
    FileWatchAwaitable second(&controller,
                              buffer,
                              sizeof(buffer),
                              FileWatchEvent::All,
                              &ready_events);
#else
    FileWatchAwaitable second(&controller, buffer, sizeof(buffer), &ready_events);
#endif
    if (!expectReadyEvent(second, "second.txt", FileWatchEvent::Delete)) {
        return 1;
    }
    if (!ready_events.empty()) {
        std::cerr << "ready event queue was not drained\n";
        return 1;
    }

    return 0;
}
