/**
 * @file b7_fileio.cc
 * @brief 用途：压测并发异步文件读写路径的吞吐、字节量与错误率。
 * 关键覆盖点：不同 worker 数与块大小、读写操作计数、异常与错误统计。
 * 通过条件：所有测量轮次正常完成并输出统计结果，进程返回 0。
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <csignal>
#include <vector>
#include <cstring>
#include "galay-kernel/core/task.h"
#include "test/stdout_log.h"

#ifdef USE_KQUEUE
#include "galay-kernel/core/kqueue_scheduler.h"
#include "galay-kernel/async/async_file.h"
#elif defined(USE_IOURING)
#include "galay-kernel/core/uring_scheduler.h"
#include "galay-kernel/async/async_file.h"
#elif defined(USE_EPOLL)
#include "galay-kernel/core/epoll_scheduler.h"
#include "galay-kernel/async/aio_file.h"
#endif

using namespace galay::kernel;

// 统计数据
std::atomic<uint64_t> g_total_reads{0};
std::atomic<uint64_t> g_total_writes{0};
std::atomic<uint64_t> g_total_bytes_read{0};
std::atomic<uint64_t> g_total_bytes_written{0};
std::atomic<uint64_t> g_total_errors{0};
std::atomic<bool> g_running{true};

void signalHandler([[maybe_unused]] int signum) {
    g_running.store(false, std::memory_order_release);
}

// 压测配置
struct BenchConfig {
    int num_workers = 4;           // 并发工作协程数
    int operations_per_worker = 1000;  // 每个工作协程的操作次数
    size_t block_size = 4096;      // 块大小
    int batch_size = 1;            // 批量操作大小（仅 AIO）
    bool use_direct_io = true;     // 是否使用 O_DIRECT（仅 AIO）
    std::string test_dir = "/tmp"; // 测试目录
};

#if defined(USE_EPOLL)
// Epoll/AIO 压测 - 支持批量操作
Task<void> benchmarkWorkerAIO(galay::async::AioFile* file, int worker_id, const BenchConfig& config) {
    char* read_buffer = galay::async::AioFile::allocAlignedBuffer(config.block_size * config.batch_size);
    char* write_buffer = galay::async::AioFile::allocAlignedBuffer(config.block_size * config.batch_size);

    if (!read_buffer || !write_buffer) {
        LogError("[Worker {}] Failed to allocate aligned buffers", worker_id);
        g_total_errors.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    // 初始化写入数据
    for (size_t i = 0; i < config.block_size * config.batch_size; ++i) {
        write_buffer[i] = 'A' + (i % 26);
    }

    int ops = 0;
    while (g_running.load(std::memory_order_relaxed) && ops < config.operations_per_worker) {
        // 写入测试
        file->clear();
        for (int i = 0; i < config.batch_size; ++i) {
            off_t offset = (worker_id * config.operations_per_worker + ops) * config.block_size + i * config.block_size;
            file->preWrite(write_buffer + i * config.block_size, config.block_size, offset);
        }

        auto write_result = co_await file->commit();
        if (!write_result) {
            g_total_errors.fetch_add(1, std::memory_order_relaxed);
        } else {
            for (auto bytes : write_result.value()) {
                if (bytes > 0) {
                    g_total_writes.fetch_add(1, std::memory_order_relaxed);
                    g_total_bytes_written.fetch_add(bytes, std::memory_order_relaxed);
                }
            }
        }

        // 读取测试
        file->clear();
        for (int i = 0; i < config.batch_size; ++i) {
            off_t offset = (worker_id * config.operations_per_worker + ops) * config.block_size + i * config.block_size;
            file->preRead(read_buffer + i * config.block_size, config.block_size, offset);
        }

        auto read_result = co_await file->commit();
        if (!read_result) {
            g_total_errors.fetch_add(1, std::memory_order_relaxed);
        } else {
            for (auto bytes : read_result.value()) {
                if (bytes > 0) {
                    g_total_reads.fetch_add(1, std::memory_order_relaxed);
                    g_total_bytes_read.fetch_add(bytes, std::memory_order_relaxed);
                }
            }
        }

        ops++;
    }

    galay::async::AioFile::freeAlignedBuffer(read_buffer);
    galay::async::AioFile::freeAlignedBuffer(write_buffer);
    co_return;
}

void runEpollBenchmark(const BenchConfig& config) {
    LogInfo("=== Epoll/AIO File IO Benchmark ===");
    LogInfo("Workers: {}, Operations per worker: {}, Block size: {}, Batch size: {}",
            config.num_workers, config.operations_per_worker, config.block_size, config.batch_size);

    EpollScheduler scheduler;
    scheduler.start();

    std::vector<galay::async::AioFile*> files;
    std::vector<std::atomic<bool>*> done_flags;

    // 为每个 worker 创建独立的文件
    for (int i = 0; i < config.num_workers; ++i) {
        auto* file = new galay::async::AioFile();
        std::string filename = config.test_dir + "/galay_bench_" + std::to_string(i) + ".dat";

        auto open_result = file->open(filename, galay::async::AioOpenMode::ReadWrite);
        if (!open_result) {
            LogError("Failed to open file {}: {}", filename, open_result.error().message());
            delete file;
            continue;
        }

        files.push_back(file);
        auto* done = new std::atomic<bool>(false);
        done_flags.push_back(done);
    }

    auto start = std::chrono::high_resolution_clock::now();

    // 启动所有 worker
    for (size_t i = 0; i < files.size(); ++i) {
        scheduleTask(scheduler, benchmarkWorkerAIO(files[i], i, config));
    }

    // 等待所有 worker 完成
    while (g_running.load(std::memory_order_relaxed)) {
        bool all_done = true;
        uint64_t current_reads = g_total_reads.load(std::memory_order_relaxed);
        uint64_t current_writes = g_total_writes.load(std::memory_order_relaxed);
        uint64_t expected_total = config.num_workers * config.operations_per_worker * 2;

        if (current_reads + current_writes < expected_total) {
            all_done = false;
        }

        if (all_done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    scheduler.stop();

    // 清理
    for (auto* file : files) {
        delete file;
    }
    for (auto* done : done_flags) {
        delete done;
    }

    // 删除测试文件
    for (int i = 0; i < config.num_workers; ++i) {
        std::string filename = config.test_dir + "/galay_bench_" + std::to_string(i) + ".dat";
        std::remove(filename.c_str());
    }

    // 输出统计
    uint64_t total_reads = g_total_reads.load();
    uint64_t total_writes = g_total_writes.load();
    uint64_t total_bytes_read = g_total_bytes_read.load();
    uint64_t total_bytes_written = g_total_bytes_written.load();
    uint64_t total_errors = g_total_errors.load();

    double seconds = duration.count() / 1000.0;
    double read_iops = total_reads / seconds;
    double write_iops = total_writes / seconds;
    double read_throughput_mb = (total_bytes_read / 1024.0 / 1024.0) / seconds;
    double write_throughput_mb = (total_bytes_written / 1024.0 / 1024.0) / seconds;

    LogInfo("=== Benchmark Results ===");
    LogInfo("Duration: {:.2f} seconds", seconds);
    LogInfo("Total Reads: {}, Total Writes: {}", total_reads, total_writes);
    LogInfo("Total Bytes Read: {} MB, Total Bytes Written: {} MB",
            total_bytes_read / 1024 / 1024, total_bytes_written / 1024 / 1024);
    LogInfo("Read IOPS: {:.2f}, Write IOPS: {:.2f}", read_iops, write_iops);
    LogInfo("Read Throughput: {:.2f} MB/s, Write Throughput: {:.2f} MB/s",
            read_throughput_mb, write_throughput_mb);
    LogInfo("Total Errors: {}", total_errors);
}
#endif

#if defined(USE_KQUEUE) || defined(USE_IOURING)
// Kqueue/io_uring 压测 - 单次操作
Task<void> benchmarkWorkerAsync(galay::async::AsyncFile* file, int worker_id, const BenchConfig& config) {
    char read_buffer[8192];
    char write_buffer[8192];

    // 初始化写入数据
    for (size_t i = 0; i < sizeof(write_buffer); ++i) {
        write_buffer[i] = 'A' + (i % 26);
    }

    int ops = 0;
    while (g_running.load(std::memory_order_relaxed) && ops < config.operations_per_worker) {
        off_t offset = (worker_id * config.operations_per_worker + ops) * config.block_size;

        // 写入测试
        auto write_result = co_await file->write(write_buffer, config.block_size, offset);
        if (!write_result) {
            g_total_errors.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_total_writes.fetch_add(1, std::memory_order_relaxed);
            g_total_bytes_written.fetch_add(write_result.value(), std::memory_order_relaxed);
        }

        // 读取测试
        auto read_result = co_await file->read(read_buffer, config.block_size, offset);
        if (!read_result) {
            g_total_errors.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_total_reads.fetch_add(1, std::memory_order_relaxed);
            g_total_bytes_read.fetch_add(read_result.value(), std::memory_order_relaxed);
        }

        ops++;
    }

    co_return;
}

#ifdef USE_KQUEUE
void runKqueueBenchmark(const BenchConfig& config) {
    LogInfo("=== Kqueue File IO Benchmark ===");
    LogInfo("Workers: {}, Operations per worker: {}, Block size: {}",
            config.num_workers, config.operations_per_worker, config.block_size);

    KqueueScheduler scheduler;
    scheduler.start();

    std::vector<galay::async::AsyncFile*> files;

    // 为每个 worker 创建独立的文件
    for (int i = 0; i < config.num_workers; ++i) {
        auto* file = new galay::async::AsyncFile();
        std::string filename = config.test_dir + "/galay_bench_" + std::to_string(i) + ".dat";

        auto open_result = file->open(filename, galay::async::FileOpenMode::ReadWrite);
        if (!open_result) {
            LogError("Failed to open file {}: {}", filename, open_result.error().message());
            delete file;
            continue;
        }

        files.push_back(file);
    }

    auto start = std::chrono::high_resolution_clock::now();

    // 启动所有 worker
    for (size_t i = 0; i < files.size(); ++i) {
        scheduleTask(scheduler, benchmarkWorkerAsync(files[i], i, config));
    }

    // 等待所有 worker 完成
    while (g_running.load(std::memory_order_relaxed)) {
        uint64_t current_reads = g_total_reads.load(std::memory_order_relaxed);
        uint64_t current_writes = g_total_writes.load(std::memory_order_relaxed);
        uint64_t expected_total = config.num_workers * config.operations_per_worker * 2;

        if (current_reads + current_writes >= expected_total) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    scheduler.stop();

    // 清理
    for (auto* file : files) {
        delete file;
    }

    // 删除测试文件
    for (int i = 0; i < config.num_workers; ++i) {
        std::string filename = config.test_dir + "/galay_bench_" + std::to_string(i) + ".dat";
        std::remove(filename.c_str());
    }

    // 输出统计
    uint64_t total_reads = g_total_reads.load();
    uint64_t total_writes = g_total_writes.load();
    uint64_t total_bytes_read = g_total_bytes_read.load();
    uint64_t total_bytes_written = g_total_bytes_written.load();
    uint64_t total_errors = g_total_errors.load();

    double seconds = duration.count() / 1000.0;
    double read_iops = total_reads / seconds;
    double write_iops = total_writes / seconds;
    double read_throughput_mb = (total_bytes_read / 1024.0 / 1024.0) / seconds;
    double write_throughput_mb = (total_bytes_written / 1024.0 / 1024.0) / seconds;

    LogInfo("=== Benchmark Results ===");
    LogInfo("Duration: {:.2f} seconds", seconds);
    LogInfo("Total Reads: {}, Total Writes: {}", total_reads, total_writes);
    LogInfo("Total Bytes Read: {} MB, Total Bytes Written: {} MB",
            total_bytes_read / 1024 / 1024, total_bytes_written / 1024 / 1024);
    LogInfo("Read IOPS: {:.2f}, Write IOPS: {:.2f}", read_iops, write_iops);
    LogInfo("Read Throughput: {:.2f} MB/s, Write Throughput: {:.2f} MB/s",
            read_throughput_mb, write_throughput_mb);
    LogInfo("Total Errors: {}", total_errors);
}
#endif

#ifdef USE_IOURING
void runIOUringBenchmark(const BenchConfig& config) {
    LogInfo("=== io_uring File IO Benchmark ===");
    LogInfo("Workers: {}, Operations per worker: {}, Block size: {}",
            config.num_workers, config.operations_per_worker, config.block_size);

    IOUringScheduler scheduler;
    scheduler.start();

    std::vector<galay::async::AsyncFile*> files;

    // 为每个 worker 创建独立的文件
    for (int i = 0; i < config.num_workers; ++i) {
        auto* file = new galay::async::AsyncFile();
        std::string filename = config.test_dir + "/galay_bench_" + std::to_string(i) + ".dat";

        auto open_result = file->open(filename, galay::async::FileOpenMode::ReadWrite);
        if (!open_result) {
            LogError("Failed to open file {}: {}", filename, open_result.error().message());
            delete file;
            continue;
        }

        files.push_back(file);
    }

    auto start = std::chrono::high_resolution_clock::now();

    // 启动所有 worker
    for (size_t i = 0; i < files.size(); ++i) {
        scheduleTask(scheduler, benchmarkWorkerAsync(files[i], i, config));
    }

    // 等待所有 worker 完成
    while (g_running.load(std::memory_order_relaxed)) {
        uint64_t current_reads = g_total_reads.load(std::memory_order_relaxed);
        uint64_t current_writes = g_total_writes.load(std::memory_order_relaxed);
        uint64_t expected_total = config.num_workers * config.operations_per_worker * 2;

        if (current_reads + current_writes >= expected_total) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    scheduler.stop();

    // 清理
    for (auto* file : files) {
        delete file;
    }

    // 删除测试文件
    for (int i = 0; i < config.num_workers; ++i) {
        std::string filename = config.test_dir + "/galay_bench_" + std::to_string(i) + ".dat";
        std::remove(filename.c_str());
    }

    // 输出统计
    uint64_t total_reads = g_total_reads.load();
    uint64_t total_writes = g_total_writes.load();
    uint64_t total_bytes_read = g_total_bytes_read.load();
    uint64_t total_bytes_written = g_total_bytes_written.load();
    uint64_t total_errors = g_total_errors.load();

    double seconds = duration.count() / 1000.0;
    double read_iops = total_reads / seconds;
    double write_iops = total_writes / seconds;
    double read_throughput_mb = (total_bytes_read / 1024.0 / 1024.0) / seconds;
    double write_throughput_mb = (total_bytes_written / 1024.0 / 1024.0) / seconds;

    LogInfo("=== Benchmark Results ===");
    LogInfo("Duration: {:.2f} seconds", seconds);
    LogInfo("Total Reads: {}, Total Writes: {}", total_reads, total_writes);
    LogInfo("Total Bytes Read: {} MB, Total Bytes Written: {} MB",
            total_bytes_read / 1024 / 1024, total_bytes_written / 1024 / 1024);
    LogInfo("Read IOPS: {:.2f}, Write IOPS: {:.2f}", read_iops, write_iops);
    LogInfo("Read Throughput: {:.2f} MB/s, Write Throughput: {:.2f} MB/s",
            read_throughput_mb, write_throughput_mb);
    LogInfo("Total Errors: {}", total_errors);
}
#endif
#endif

int main(int argc, char* argv[]) {
    // 注册信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    BenchConfig config;

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-w" && i + 1 < argc) {
            config.num_workers = std::stoi(argv[++i]);
        } else if (arg == "-n" && i + 1 < argc) {
            config.operations_per_worker = std::stoi(argv[++i]);
        } else if (arg == "-b" && i + 1 < argc) {
            config.block_size = std::stoul(argv[++i]);
        } else if (arg == "-batch" && i + 1 < argc) {
            config.batch_size = std::stoi(argv[++i]);
        } else if (arg == "-d" && i + 1 < argc) {
            config.test_dir = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -w <num>      Number of worker coroutines (default: 4)\n"
                      << "  -n <num>      Operations per worker (default: 1000)\n"
                      << "  -b <size>     Block size in bytes (default: 4096)\n"
                      << "  -batch <num>  Batch size for AIO (default: 1)\n"
                      << "  -d <dir>      Test directory (default: /tmp)\n"
                      << "  -h, --help    Show this help message\n";
            return 0;
        }
    }

    LogInfo("========================================");
    LogInfo("Galay Kernel File IO Benchmark");
    LogInfo("========================================");

#ifdef USE_KQUEUE
    runKqueueBenchmark(config);
#elif defined(USE_EPOLL)
    runEpollBenchmark(config);
#elif defined(USE_IOURING)
    runIOUringBenchmark(config);
#else
    LogError("No supported platform detected (kqueue/epoll/io_uring)");
    return 1;
#endif

    return 0;
}
