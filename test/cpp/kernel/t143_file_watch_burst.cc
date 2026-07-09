/**
 * @file t143_file_watch_burst.cc
 * @brief 验证 epoll/inotify burst 文件事件不会因单次 read 只解析一个事件而丢失。
 */

#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#ifdef USE_EPOLL

#include <galay/cpp/galay-kernel/async/file_watcher.h>
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <unistd.h>

using namespace galay::kernel;

namespace {

constexpr int kEventCount = 100;

Task<void> watchCreates(const std::filesystem::path& dir,
                        std::atomic<int>* create_count,
                        std::atomic<bool>* done)
{
    galay::async::FileWatcher watcher;
    auto watch_result = watcher.addWatch(dir.string(), FileWatchEvent::Create);
    if (!watch_result) {
        done->store(true, std::memory_order_release);
        co_return;
    }

    std::unordered_set<std::string> names;
    while (static_cast<int>(names.size()) < kEventCount) {
        auto event = co_await watcher.watch();
        if (!event) {
            break;
        }
        if (event->has(FileWatchEvent::Create) && !event->name.empty()) {
            names.insert(event->name);
            create_count->store(static_cast<int>(names.size()), std::memory_order_release);
        }
    }
    done->store(true, std::memory_order_release);
    co_return;
}

void createBurst(const std::filesystem::path& dir)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (int i = 0; i < kEventCount; ++i) {
        const auto path = dir / ("file_" + std::to_string(i));
        std::ofstream out(path);
        out << i << '\n';
    }
}

}  // namespace

int main()
{
    const auto dir = std::filesystem::temp_directory_path() /
        ("galay_file_watch_burst_" + std::to_string(getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ec.clear();
    if (!std::filesystem::create_directories(dir, ec)) {
        std::cerr << "failed to create test directory: " << ec.message() << "\n";
        return 1;
    }

    EpollScheduler scheduler;
    scheduler.start();

    std::atomic<int> create_count{0};
    std::atomic<bool> done{false};
    if (!scheduleTask(scheduler, watchCreates(dir, &create_count, &done))) {
        scheduler.stop();
        std::filesystem::remove_all(dir, ec);
        std::cerr << "failed to schedule watch task\n";
        return 1;
    }

    std::thread producer(createBurst, dir);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    producer.join();
    scheduler.stop();
    std::filesystem::remove_all(dir, ec);

    const int observed = create_count.load(std::memory_order_acquire);
    if (observed != kEventCount) {
        std::cerr << "expected " << kEventCount
                  << " create events, observed " << observed << "\n";
        return 1;
    }

    return 0;
}

#else

int main()
{
    std::cout << "T143-FileWatchBurst SKIP (non-epoll backend)\n";
    return 0;
}

#endif
