/**
 * @file t67_aioevfd.cc
 * @brief 用途：验证 `epoll + AioFile` 在同一文件对象上连续两次 `commit()` 时会重新注册 AIO eventfd。
 * 关键覆盖点：第一次 AIO 完成后的 `EPOLL_CTL_DEL` 收尾、第二次 `commit()` 的重新注册、顺序 write->read 链路。
 * 通过条件：测试在超时前完成，第二次 read 成功返回并读回第一次 write 的内容。
 */

#include "galay-kernel/core/task.h"

#include <iostream>

#ifdef USE_EPOLL

#include "galay-kernel/async/aio_file.h"
#include "galay-kernel/core/epoll_scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace galay::kernel;

namespace {

Task<void> aioWriteThenRead(std::atomic<bool>* done, std::atomic<bool>* passed)
{
    const char* path = "/tmp/galay_epoll_aio_eventfd_rearm_test.dat";
    galay::async::AioFile file;
    auto open_result = file.open(path, galay::async::AioOpenMode::ReadWrite);
    if (!open_result) {
        done->store(true, std::memory_order_release);
        co_return;
    }

    char* write_buffer = galay::async::AioFile::allocAlignedBuffer(4096);
    char* read_buffer = galay::async::AioFile::allocAlignedBuffer(4096);
    if (!write_buffer || !read_buffer) {
        galay::async::AioFile::freeAlignedBuffer(write_buffer);
        galay::async::AioFile::freeAlignedBuffer(read_buffer);
        std::remove(path);
        done->store(true, std::memory_order_release);
        co_return;
    }

    std::memset(write_buffer, 0, 4096);
    std::memset(read_buffer, 0, 4096);
    std::memcpy(write_buffer, "epoll-aio-rearm-ok", sizeof("epoll-aio-rearm-ok"));

    file.clear();
    file.preWrite(write_buffer, 4096, 0);
    auto write_result = co_await file.commit();
    const bool write_ok = write_result.has_value() &&
                          !write_result->empty() &&
                          write_result->front() == 4096;

    if (write_ok) {
        file.clear();
        file.preRead(read_buffer, 4096, 0);
        auto read_result = co_await file.commit();
        const bool read_ok = read_result.has_value() &&
                             !read_result->empty() &&
                             read_result->front() == 4096 &&
                             std::memcmp(write_buffer, read_buffer, 4096) == 0;
        passed->store(read_ok, std::memory_order_release);
    }

    galay::async::AioFile::freeAlignedBuffer(write_buffer);
    galay::async::AioFile::freeAlignedBuffer(read_buffer);
    std::remove(path);
    done->store(true, std::memory_order_release);
    co_return;
}

}  // namespace

int main()
{
    EpollScheduler scheduler;
    scheduler.start();

    std::atomic<bool> done{false};
    std::atomic<bool> passed{false};
    if (!scheduleTask(scheduler, aioWriteThenRead(&done, &passed))) {
        scheduler.stop();
        std::cerr << "T67 scheduleTask failed\n";
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    scheduler.stop();

    if (!done.load(std::memory_order_acquire)) {
        std::cerr << "T67 timeout waiting for second AIO completion\n";
        return 1;
    }
    if (!passed.load(std::memory_order_acquire)) {
        std::cerr << "T67 read-after-write verification failed\n";
        return 1;
    }

    std::cout << "T67-EpollAioEventfdRearm PASS\n";
    return 0;
}

#else

int main()
{
    std::cout << "T67-EpollAioEventfdRearm SKIP (non-epoll backend)\n";
    return 0;
}

#endif
