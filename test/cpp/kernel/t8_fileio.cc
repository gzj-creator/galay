/**
 * @file t8_fileio.cc
 * @brief 用途：验证异步文件读写路径的基础正确性与数据回环。
 * 关键覆盖点：文件创建与打开、异步读写接口、写入内容回读一致性。
 * 通过条件：文件 IO 结果与预期一致，测试流程正常结束并返回 0。
 */

#include <cstring>
#include <fstream>
#include <chrono>
#include <atomic>
#include <string_view>
#include <thread>
#include <galay/cpp/galay-kernel/common/defn.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/stdout_log.h"
#include "result_writer.h"

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#include <galay/cpp/galay-kernel/async/async_file.h>
#endif

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#include <galay/cpp/galay-kernel/async/aio_file.h>
#endif

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#include <galay/cpp/galay-kernel/async/async_file.h>
#endif

using namespace galay::kernel;

const char* TEST_FILE = "/tmp/galay_test_file.txt";
const char* TEST_CONTENT = "Hello, Galay Kernel File IO Test!\nThis is line 2.\nThis is line 3.\n";

std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_total{0};

template <typename Rep, typename Period>
bool waitForDone(const std::atomic<bool>& done,
                 std::chrono::duration<Rep, Period> timeout,
                 std::chrono::milliseconds poll_interval = std::chrono::milliseconds(1)) {
    if (done.load(std::memory_order_acquire)) {
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(poll_interval);
        if (done.load(std::memory_order_acquire)) {
            return true;
        }
    }

    return done.load(std::memory_order_acquire);
}

// 创建测试文件
void createTestFile() {
    std::ofstream ofs(TEST_FILE);
    ofs << TEST_CONTENT;
    ofs.close();
    LogInfo("Created test file: {}", TEST_FILE);
}

// 清理测试文件
void cleanupTestFile() {
    std::remove(TEST_FILE);
    LogInfo("Cleaned up test file");
}

#ifdef USE_KQUEUE
// Kqueue 平台测试 - 所有测试在一个协程中完成
Task<void> test_kqueue_file_io(std::atomic<bool>* done) {
    LogInfo("=== Kqueue (macOS) File IO Test ===");

    // 测试1: 读取已有文件
    {
        g_total++;
        LogInfo("[Kqueue] Test 1: File read...");
        galay::async::AsyncFile file;
        auto openResult = file.open(TEST_FILE, galay::async::FileOpenMode::Read);
        if (!openResult) {
            LogError("[Kqueue] Failed to open file for read: {}", openResult.error().message());
            g_failed++;
            *done = true;
            co_return;
        }

        char buffer[1024] = {0};
        auto result = co_await file.read(buffer, sizeof(buffer) - 1, 0);

        if (!result) {
            LogError("[Kqueue] Read failed: {}", result.error().message());
            g_failed++;
        } else {
            size_t bytes = result.value();
            std::string_view payload(buffer, bytes);
            LogInfo("[Kqueue] Read {} bytes: {}", bytes, payload);
            if (payload == TEST_CONTENT) {
                LogInfo("[Kqueue] Test 1 PASSED: Content matches");
                g_passed++;
            } else {
                LogError("[Kqueue] Test 1 FAILED: Content mismatch");
                g_failed++;
            }
        }
    }

    // 测试2: 写入文件然后读回验证
    {
        g_total++;
        LogInfo("[Kqueue] Test 2: File write and read back...");
        const char* writeTestFile = "/tmp/galay_kqueue_write_test.txt";

        galay::async::AsyncFile file;
        auto openResult = file.open(writeTestFile, galay::async::FileOpenMode::ReadWrite);
        if (!openResult) {
            LogError("[Kqueue] Failed to open file for write: {}", openResult.error().message());
            g_failed++;
            *done = true;
            co_return;
        }

        const char* writeContent = "Written by Kqueue AsyncFile test!\n";
        auto writeResult = co_await file.write(writeContent, strlen(writeContent), 0);

        if (!writeResult) {
            LogError("[Kqueue] Write failed: {}", writeResult.error().message());
            g_failed++;
        } else {
            LogInfo("[Kqueue] Written {} bytes", writeResult.value());

            // 同步到磁盘
            (void)file.sync();

            // 读回验证
            char buffer[1024] = {0};
            auto readResult = co_await file.read(buffer, sizeof(buffer) - 1, 0);
            if (readResult) {
                std::string_view written(writeContent);
                std::string_view readBack(buffer, readResult.value());
                if (readBack.substr(0, written.size()) == written) {
                    LogInfo("[Kqueue] Test 2 PASSED: Write and read back successful");
                    g_passed++;
                } else {
                    LogError("[Kqueue] Test 2 FAILED: Content mismatch");
                    LogError("[Kqueue] Expected: {}", written);
                    LogError("[Kqueue] Got: {}", readBack);
                    g_failed++;
                }
            } else {
                LogError("[Kqueue] Test 2 FAILED: Read back error: {}", readResult.error().message());
                g_failed++;
            }
        }

        std::remove(writeTestFile);
    }

    LogInfo("=== Kqueue Test Finished ===\n");
    *done = true;
    co_return;
}

void runKqueueTest() {
    KqueueScheduler scheduler;
    scheduler.start();

    std::atomic<bool> done{false};
    scheduleTask(scheduler, test_kqueue_file_io(&done));

    if (!waitForDone(done, std::chrono::seconds(5))) {
        LogError("[Kqueue] Test timed out waiting for file IO task completion");
        g_failed++;
    }

    scheduler.stop();
}
#endif

#ifdef USE_EPOLL
// Epoll 平台测试 - 所有测试在一个协程中完成
Task<void> test_epoll_file_io(std::atomic<bool>* done) {
    LogInfo("=== Epoll (Linux libaio) File IO Test ===");

    // 创建测试文件用于 O_DIRECT
    const char* aioTestFile = "/tmp/galay_aio_test.dat";
    {
        std::ofstream ofs(aioTestFile, std::ios::binary);
        char block[4096];
        memset(block, 'A', sizeof(block));
        strcpy(block, TEST_CONTENT);
        for (int i = 0; i < 10; ++i) {
            ofs.write(block, sizeof(block));
        }
        ofs.close();
    }

    // 测试1: 单次读取
    {
        g_total++;
        LogInfo("[Epoll/AIO] Test 1: Single read...");
        galay::async::AioFile file;
        auto openResult = file.open(aioTestFile, galay::async::AioOpenMode::Read);
        if (!openResult) {
            LogError("[Epoll/AIO] Failed to open file: {}", openResult.error().message());
            g_failed++;
            *done = true;
            co_return;
        }

        char* buffer = galay::async::AioFile::allocAlignedBuffer(4096);
        if (!buffer) {
            LogError("[Epoll/AIO] Failed to allocate aligned buffer");
            g_failed++;
            *done = true;
            co_return;
        }

        file.preRead(buffer, 4096, 0);
        auto result = co_await file.commit();

        if (!result) {
            LogError("[Epoll/AIO] Read failed: {}", result.error().message());
            g_failed++;
        } else {
            auto& results = result.value();
            if (!results.empty() && results[0] > 0) {
                buffer[results[0]] = '\0';
                LogInfo("[Epoll/AIO] Read {} bytes: {}", results[0], buffer);
                LogInfo("[Epoll/AIO] Test 1 PASSED");
                g_passed++;
            } else {
                LogError("[Epoll/AIO] Test 1 FAILED: No data read");
                g_failed++;
            }
        }

        galay::async::AioFile::freeAlignedBuffer(buffer);
    }

    // 测试2: 批量读取
    {
        g_total++;
        LogInfo("[Epoll/AIO] Test 2: Batch read (3 requests)...");
        galay::async::AioFile file;
        auto openResult = file.open(aioTestFile, galay::async::AioOpenMode::Read);
        if (!openResult) {
            LogError("[Epoll/AIO] Failed to open file: {}", openResult.error().message());
            g_failed++;
            *done = true;
            co_return;
        }

        char* buffer1 = galay::async::AioFile::allocAlignedBuffer(4096);
        char* buffer2 = galay::async::AioFile::allocAlignedBuffer(4096);
        char* buffer3 = galay::async::AioFile::allocAlignedBuffer(4096);

        if (!buffer1 || !buffer2 || !buffer3) {
            LogError("[Epoll/AIO] Failed to allocate aligned buffers");
            g_failed++;
            *done = true;
            co_return;
        }

        file.preRead(buffer1, 4096, 0);
        file.preRead(buffer2, 4096, 4096);
        file.preRead(buffer3, 4096, 8192);

        auto result = co_await file.commit();

        if (!result) {
            LogError("[Epoll/AIO] Batch read failed: {}", result.error().message());
            g_failed++;
        } else {
            auto& results = result.value();
            LogInfo("[Epoll/AIO] Batch read completed with {} results", results.size());
            for (size_t i = 0; i < results.size(); ++i) {
                LogInfo("[Epoll/AIO] Result[{}]: {} bytes", i, results[i]);
            }
            if (results.size() == 3 &&
                results[0] == 4096 &&
                results[1] == 4096 &&
                results[2] == 4096) {
                LogInfo("[Epoll/AIO] Test 2 PASSED");
                g_passed++;
            } else {
                LogError("[Epoll/AIO] Test 2 FAILED: expected 3 full results, got {}", results.size());
                g_failed++;
            }
        }

        galay::async::AioFile::freeAlignedBuffer(buffer1);
        galay::async::AioFile::freeAlignedBuffer(buffer2);
        galay::async::AioFile::freeAlignedBuffer(buffer3);
    }

    // 测试3: 写入
    {
        g_total++;
        LogInfo("[Epoll/AIO] Test 3: Write...");
        const char* writeTestFile = "/tmp/galay_aio_write_test.dat";
        galay::async::AioFile file;
        auto openResult = file.open(writeTestFile, galay::async::AioOpenMode::Write);
        if (!openResult) {
            LogError("[Epoll/AIO] Failed to open file for write: {}", openResult.error().message());
            g_failed++;
            *done = true;
            co_return;
        }

        char* buffer = galay::async::AioFile::allocAlignedBuffer(4096);
        if (!buffer) {
            LogError("[Epoll/AIO] Failed to allocate aligned buffer");
            g_failed++;
            *done = true;
            co_return;
        }

        const char* writeContent = "Written by Epoll AioFile test!\n";
        memset(buffer, 0, 4096);
        strcpy(buffer, writeContent);

        file.preWrite(buffer, 512, 0);  // O_DIRECT 需要 512 对齐
        auto result = co_await file.commit();

        if (!result) {
            LogError("[Epoll/AIO] Write failed: {}", result.error().message());
            g_failed++;
        } else {
            auto& results = result.value();
            if (!results.empty() && results[0] >= 0) {
                LogInfo("[Epoll/AIO] Written {} bytes", results[0]);
                LogInfo("[Epoll/AIO] Test 3 PASSED");
                g_passed++;
            } else {
                LogError("[Epoll/AIO] Test 3 FAILED");
                g_failed++;
            }
        }

        galay::async::AioFile::freeAlignedBuffer(buffer);
        std::remove(writeTestFile);
    }

    std::remove(aioTestFile);
    LogInfo("=== Epoll Test Finished ===\n");
    *done = true;
    co_return;
}

void runEpollTest() {
    EpollScheduler scheduler;
    scheduler.start();

    std::atomic<bool> done{false};
    scheduleTask(scheduler, test_epoll_file_io(&done));

    if (!waitForDone(done, std::chrono::seconds(5))) {
        LogError("[Epoll/AIO] Test timed out waiting for file IO task completion");
        g_failed++;
    }

    scheduler.stop();
}
#endif

#ifdef USE_IOURING
// io_uring 平台测试 - 所有测试在一个协程中完成
Task<void> test_io_uring_file_io(std::atomic<bool>* done) {
    LogInfo("=== io_uring (Linux) File IO Test ===");

    // 测试1: 读取已有文件
    {
        g_total++;
        LogInfo("[io_uring] Test 1: File read...");
        galay::async::AsyncFile file;
        auto openResult = file.open(TEST_FILE, galay::async::FileOpenMode::Read);
        if (!openResult) {
            LogError("[io_uring] Failed to open file for read: {}", openResult.error().message());
            g_failed++;
            *done = true;
            co_return;
        }

        char buffer[1024] = {0};
        auto result = co_await file.read(buffer, sizeof(buffer) - 1, 0);

        if (!result) {
            LogError("[io_uring] Read failed: {}", result.error().message());
            g_failed++;
        } else {
            size_t bytes = result.value();
            std::string_view payload(buffer, bytes);
            LogInfo("[io_uring] Read {} bytes: {}", bytes, payload);
            if (payload == TEST_CONTENT) {
                LogInfo("[io_uring] Test 1 PASSED: Content matches");
                g_passed++;
            } else {
                LogError("[io_uring] Test 1 FAILED: Content mismatch");
                g_failed++;
            }
        }
    }

    // 测试2: 写入文件然后读回验证
    {
        g_total++;
        LogInfo("[io_uring] Test 2: File write and read back...");
        const char* writeTestFile = "/tmp/galay_iouring_write_test.txt";

        galay::async::AsyncFile file;
        auto openResult = file.open(writeTestFile, galay::async::FileOpenMode::ReadWrite);
        if (!openResult) {
            LogError("[io_uring] Failed to open file for write: {}", openResult.error().message());
            g_failed++;
            *done = true;
            co_return;
        }

        const char* writeContent = "Written by io_uring AsyncFile test!\n";
        auto writeResult = co_await file.write(writeContent, strlen(writeContent), 0);

        if (!writeResult) {
            LogError("[io_uring] Write failed: {}", writeResult.error().message());
            g_failed++;
        } else {
            LogInfo("[io_uring] Written {} bytes", writeResult.value());

            // 同步到磁盘
            (void)file.sync();

            // 读回验证
            char buffer[1024] = {0};
            auto readResult = co_await file.read(buffer, sizeof(buffer) - 1, 0);
            if (readResult) {
                std::string_view written(writeContent);
                std::string_view readBack(buffer, readResult.value());
                if (readBack.substr(0, written.size()) == written) {
                    LogInfo("[io_uring] Test 2 PASSED: Write and read back successful");
                    g_passed++;
                } else {
                    LogError("[io_uring] Test 2 FAILED: Content mismatch");
                    LogError("[io_uring] Expected: {}", written);
                    LogError("[io_uring] Got: {}", readBack);
                    g_failed++;
                }
            } else {
                LogError("[io_uring] Test 2 FAILED: Read back error: {}", readResult.error().message());
                g_failed++;
            }
        }

        std::remove(writeTestFile);
    }

    LogInfo("=== io_uring Test Finished ===\n");
    *done = true;
    co_return;
}

void runIOUringTest() {
    IOUringScheduler scheduler;
    scheduler.start();

    std::atomic<bool> done{false};
    scheduleTask(scheduler, test_io_uring_file_io(&done));

    if (!waitForDone(done, std::chrono::seconds(5))) {
        LogError("[io_uring] Test timed out waiting for file IO task completion");
        g_failed++;
    }

    scheduler.stop();
}
#endif

int main() {
    LogInfo("========================================");
    LogInfo("Galay Kernel File IO Test");
    LogInfo("========================================\n");

    // 创建测试文件
    createTestFile();

#ifdef USE_KQUEUE
    runKqueueTest();
#elif defined(USE_EPOLL)
    runEpollTest();
#elif defined(USE_IOURING)
    runIOUringTest();
#else
    LogWarn("No supported platform detected (kqueue/epoll/io_uring)");
#endif

    // 清理
    cleanupTestFile();

    galay::test::TestResultWriter writer("test_file_io");
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

    LogInfo("Test Results: Total={}, Passed={}, Failed={}", g_total.load(), g_passed.load(), g_failed.load());

    LogInfo("========================================");
    LogInfo("All File IO Tests Completed");
    LogInfo("========================================");

    return g_failed.load() == 0 ? 0 : 1;
}
