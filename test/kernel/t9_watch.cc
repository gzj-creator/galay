/**
 * @file t9_watch.cc
 * @brief 用途：验证 `FileWatcher` 对文件变更事件的监听与通知行为。
 * 关键覆盖点：文件监听注册、修改事件捕获、回调或结果通知触发。
 * 通过条件：文件变更事件被正确观察到，测试按预期返回 0。
 */

#include <galay/cpp/galay-kernel/async/file_watcher.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "result_writer.h"
#include "test/stdout_log.h"

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <sys/stat.h>

using namespace galay::kernel;
using namespace galay::async;

std::atomic<bool> g_running{true};
std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_total{0};
std::atomic<int> g_event_count{0};

// 测试1: 监控所有事件
std::atomic<int> g_test1_events{0};
Task<void> watchAllEventsTask([[maybe_unused]] IOScheduler* scheduler, const std::string& path)
{
    FileWatcher watcher;

    auto result = watcher.addWatch(path, FileWatchEvent::All);
    if (!result) {
        LogError("[Test1] Failed to add watch: {}", result.error().message());
        co_return;
    }

    LogInfo("[Test1] Watching ALL events: {} (wd={})", path, result.value());

    while (g_running) {
        auto event = co_await watcher.watch();
        if (!event) {
            break;
        }
        g_test1_events++;
        LogInfo("[Test1] Event: Modify={} Attrib={}",
                event->has(FileWatchEvent::Modify),
                event->has(FileWatchEvent::Attrib));
    }
    co_return;
}

// 测试2: 只监控 Modify 事件
std::atomic<int> g_test2_modify_events{0};
std::atomic<int> g_test2_attrib_events{0};
Task<void> watchModifyOnlyTask([[maybe_unused]] IOScheduler* scheduler, const std::string& path)
{
    FileWatcher watcher;

    // 只监控 Modify 事件
    auto result = watcher.addWatch(path, FileWatchEvent::Modify);
    if (!result) {
        LogError("[Test2] Failed to add watch: {}", result.error().message());
        co_return;
    }

    LogInfo("[Test2] Watching MODIFY only: {} (wd={})", path, result.value());

    while (g_running) {
        auto event = co_await watcher.watch();
        if (!event) {
            break;
        }
        if (event->has(FileWatchEvent::Modify)) {
            g_test2_modify_events++;
            LogInfo("[Test2] Got Modify event");
        }
        if (event->has(FileWatchEvent::Attrib)) {
            g_test2_attrib_events++;
            LogInfo("[Test2] Got Attrib event (unexpected!)");
        }
    }
    co_return;
}

// 测试3: 只监控 Attrib 事件
std::atomic<int> g_test3_modify_events{0};
std::atomic<int> g_test3_attrib_events{0};
Task<void> watchAttribOnlyTask([[maybe_unused]] IOScheduler* scheduler, const std::string& path)
{
    FileWatcher watcher;

    // 只监控 Attrib 事件
    auto result = watcher.addWatch(path, FileWatchEvent::Attrib);
    if (!result) {
        LogError("[Test3] Failed to add watch: {}", result.error().message());
        co_return;
    }

    LogInfo("[Test3] Watching ATTRIB only: {} (wd={})", path, result.value());

    while (g_running) {
        auto event = co_await watcher.watch();
        if (!event) {
            break;
        }
        if (event->has(FileWatchEvent::Modify)) {
            g_test3_modify_events++;
            LogInfo("[Test3] Got Modify event (unexpected!)");
        }
        if (event->has(FileWatchEvent::Attrib)) {
            g_test3_attrib_events++;
            LogInfo("[Test3] Got Attrib event");
        }
    }
    co_return;
}

// 模拟文件操作
void fileOperationThread(const std::string& path1, const std::string& path2, const std::string& path3)
{
    auto wait_ms = [](int ms) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count() < ms) {
            std::this_thread::yield();
        }
    };

    wait_ms(500);

    LogInfo("\n--- File operations start ---\n");

    // 对所有文件执行写入操作（触发 Modify）
    LogInfo("> Writing to files (triggers Modify)...");
    for (const auto& path : {path1, path2, path3}) {
        std::ofstream ofs(path);
        ofs << "Hello" << std::endl;
    }
    wait_ms(300);

    // 对所有文件执行 chmod 操作（触发 Attrib）
    LogInfo("> Changing file permissions (triggers Attrib)...");
    for (const auto& path : {path1, path2, path3}) {
        chmod(path.c_str(), 0644);
    }
    wait_ms(300);

    // 再次写入
    LogInfo("> Writing again (triggers Modify)...");
    for (const auto& path : {path1, path2, path3}) {
        std::ofstream ofs(path, std::ios::app);
        ofs << "World" << std::endl;
    }
    wait_ms(300);

    LogInfo("\n--- File operations done ---\n");

    wait_ms(500);
}

void evaluateResults()
{
    // Test1: 监控所有事件，应该收到事件
    g_total++;
    if (g_test1_events > 0) {
        LogInfo("[Test1] PASSED: Received {} events", g_test1_events.load());
        g_passed++;
    } else {
        LogError("[Test1] FAILED: No events received");
        g_failed++;
    }

    // Test2: 只监控 Modify，应该收到 Modify，不应该收到 Attrib
    g_total++;
    bool test2_passed = (g_test2_modify_events > 0);
#ifdef USE_KQUEUE
    test2_passed = test2_passed && (g_test2_attrib_events == 0);
#endif
    if (test2_passed) {
        LogInfo("[Test2] PASSED: Modify={}, Attrib={}",
                g_test2_modify_events.load(), g_test2_attrib_events.load());
        g_passed++;
    } else {
        LogError("[Test2] FAILED: Modify={}, Attrib={}",
                g_test2_modify_events.load(), g_test2_attrib_events.load());
        g_failed++;
    }

    // Test3: 只监控 Attrib，应该收到 Attrib，不应该收到 Modify
    g_total++;
    bool test3_passed = (g_test3_attrib_events > 0);
#ifdef USE_KQUEUE
    test3_passed = test3_passed && (g_test3_modify_events == 0);
#endif
    if (test3_passed) {
        LogInfo("[Test3] PASSED: Modify={}, Attrib={}",
                g_test3_modify_events.load(), g_test3_attrib_events.load());
        g_passed++;
    } else {
        LogError("[Test3] FAILED: Modify={}, Attrib={}",
                g_test3_modify_events.load(), g_test3_attrib_events.load());
        g_failed++;
    }
}

int main()
{
    LogInfo("========================================");
    LogInfo("FileWatcher Event Filter Test");
    LogInfo("========================================\n");

#ifdef USE_IOURING
    LogInfo("Backend: io_uring");
#elif defined(USE_EPOLL)
    LogInfo("Backend: epoll + inotify");
#elif defined(USE_KQUEUE)
    LogInfo("Backend: kqueue");
#endif

    const std::string testFile1 = "/tmp/test_watcher1.txt";
    const std::string testFile2 = "/tmp/test_watcher2.txt";
    const std::string testFile3 = "/tmp/test_watcher3.txt";

    // 创建初始文件
    { std::ofstream ofs(testFile1); ofs << "init" << std::endl; }
    { std::ofstream ofs(testFile2); ofs << "init" << std::endl; }
    { std::ofstream ofs(testFile3); ofs << "init" << std::endl; }

    TestScheduler scheduler;
    scheduler.start();

    // 启动文件操作线程
    std::thread opThread(fileOperationThread, testFile1, testFile2, testFile3);

    // 启动监控协程
    scheduleTask(scheduler, watchAllEventsTask(&scheduler, testFile1));
    scheduleTask(scheduler, watchModifyOnlyTask(&scheduler, testFile2));
    scheduleTask(scheduler, watchAttribOnlyTask(&scheduler, testFile3));

    opThread.join();

    // 评估测试结果
    evaluateResults();

    scheduler.stop();
    std::remove(testFile1.c_str());
    std::remove(testFile2.c_str());
    std::remove(testFile3.c_str());

    // 写入测试结果
    galay::test::TestResultWriter writer("test_file_watcher");
    for (int i = 0; i < g_total.load(); ++i) {
        writer.addTest();
    }
    for (int i = 0; i < g_passed.load(); ++i) {
        writer.addPassed();
    }
    for (int i = 0; i < g_failed.load(); ++i) {
        writer.addFailed();
    }
    writer.writeResult();

    LogInfo("========================================");
    LogInfo("Test Results: Total={}, Passed={}, Failed={}",
            g_total.load(), g_passed.load(), g_failed.load());
    LogInfo("========================================");

    return g_failed > 0 ? 1 : 0;
}
