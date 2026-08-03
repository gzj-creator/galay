/**
 * @file b10_ringbuffer.cc
 * @brief 用途：压测 `RingBuffer` 的基础写入、读写混合与环绕场景性能。
 * 关键覆盖点：连续写入吞吐、写后即读、频繁环绕、借用 `iovec` 接口开销。
 * 通过条件：各类压测样本均能完成并输出结果，进程返回 0。
 */

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <galay/cpp/galay-kernel/common/buffer.h>
#include "test/cpp/common/stdout_log.h"

using namespace galay::kernel;
using namespace std::chrono;

namespace {

std::atomic<size_t> g_benchmark_sink{0};
std::atomic<bool> g_benchmark_valid{true};

inline void benchmarkBarrier(const void* address) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r"(address) : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const size_t current = g_benchmark_sink.load(std::memory_order_relaxed);
    g_benchmark_sink.store(current ^ reinterpret_cast<std::uintptr_t>(address),
                           std::memory_order_relaxed);
#endif
}

void observeChecksum(size_t checksum) noexcept {
    const size_t current = g_benchmark_sink.load(std::memory_order_relaxed);
    g_benchmark_sink.store(current ^ checksum, std::memory_order_relaxed);
}

}  // namespace

// ============== 压测参数 ==============
constexpr size_t BUFFER_SIZE = 64 * 1024;        // 64KB 缓冲区
constexpr size_t SMALL_BUFFER_SIZE = 1024;       // 1KB 小缓冲区（测试环绕）
constexpr size_t TOTAL_DATA_SIZE = 1024 * 1024 * 1024;  // 1GB 总数据量
constexpr size_t CHUNK_SIZES[] = {64, 256, 1024, 4096, 16384};
constexpr auto MIN_WRITE_SAMPLE_DURATION = std::chrono::milliseconds(100);
constexpr auto MIN_WRAP_SAMPLE_DURATION = std::chrono::milliseconds(100);
constexpr size_t MICRO_BENCH_SAMPLES = 5;

template <typename SampleFn>
double medianSample(SampleFn&& sample_fn) {
    std::vector<double> samples;
    samples.reserve(MICRO_BENCH_SAMPLES);
    for (size_t i = 0; i < MICRO_BENCH_SAMPLES; ++i) {
        samples.push_back(sample_fn());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

// ============== 辅助函数 ==============

void printResult(const char* testName, size_t totalBytes, double durationMs) {
    double throughputMBps = (totalBytes / (1024.0 * 1024.0)) / (durationMs / 1000.0);
    double throughputGbps = throughputMBps * 8 / 1024.0;
    LogInfo("{}: {:.2f} MB/s ({:.2f} Gbps), duration: {:.2f} ms",
            testName, throughputMBps, throughputGbps, durationMs);
}

// ============== 压测1: 写入吞吐量 ==============

void benchWriteThroughput(size_t chunkSize) {
    RingBuffer buffer(BUFFER_SIZE);
    std::vector<char> data(chunkSize, 'A');

    size_t totalWritten = 0;
    auto start = steady_clock::now();

    while (totalWritten < TOTAL_DATA_SIZE ||
           steady_clock::now() - start < MIN_WRITE_SAMPLE_DURATION) {
        size_t written = buffer.write(data.data(), chunkSize);
        benchmarkBarrier(&buffer);
        totalWritten += written;
        if (written != chunkSize) {
            g_benchmark_valid.store(false, std::memory_order_relaxed);
        }

        // 消费数据以腾出空间
        if (buffer.writable() < chunkSize) {
            buffer.consume(buffer.readable());
            benchmarkBarrier(&buffer);
        }
    }

    auto end = steady_clock::now();
    double durationMs = duration_cast<microseconds>(end - start).count() / 1000.0;

    const std::string testName = "Write Throughput (chunk=" + std::to_string(chunkSize) + ")";
    printResult(testName.c_str(), totalWritten, durationMs);
}

// ============== 压测2: 读写吞吐量（模拟网络IO场景）==============

void benchReadWriteThroughput(size_t chunkSize) {
    RingBuffer buffer(BUFFER_SIZE);
    std::vector<char> writeData(chunkSize, 'B');
    std::vector<char> readData(chunkSize);

    size_t totalProcessed = 0;
    size_t checksum = 0;
    auto start = steady_clock::now();
    std::array<struct iovec, 2> iovecs{};

    while (totalProcessed < TOTAL_DATA_SIZE) {
        // 写入
        size_t written = buffer.write(writeData.data(), chunkSize);
        benchmarkBarrier(&buffer);

        // 通过 iovec 读取
        size_t iovecCount = buffer.getReadIovecs(iovecs);
        size_t bytesRead = 0;
        for (size_t i = 0; i < iovecCount; ++i) {
            const auto& iov = iovecs[i];
            size_t toRead = std::min(iov.iov_len, chunkSize - bytesRead);
            std::memcpy(readData.data() + bytesRead, iov.iov_base, toRead);
            bytesRead += toRead;
            if (bytesRead >= chunkSize) break;
        }
        buffer.consume(bytesRead);
        benchmarkBarrier(&buffer);

        if (written != chunkSize || bytesRead != chunkSize) {
            g_benchmark_valid.store(false, std::memory_order_relaxed);
        }
        if (bytesRead > 0) {
            checksum += static_cast<unsigned char>(readData[bytesRead - 1]);
            benchmarkBarrier(readData.data());
        }

        totalProcessed += written;
    }
    observeChecksum(checksum);

    auto end = steady_clock::now();
    double durationMs = duration_cast<microseconds>(end - start).count() / 1000.0;

    const std::string testName = "Read/Write Throughput (chunk=" +
        std::to_string(chunkSize) + ")";
    printResult(testName.c_str(), totalProcessed, durationMs);
}

// ============== 压测3: 环绕性能 ==============

void benchWrapAroundPerformance() {
    // mmap 双映射后跨逻辑边界仍保持单 iovec，不能用 iovec_count==2 判断环绕。
    RingBuffer mmap_buffer(SMALL_BUFFER_SIZE);
    std::vector<char> data(256, 'C');

    size_t totalWritten = 0;
    size_t targetData = 100 * 1024 * 1024;  // 100MB
    size_t fallback_physical_wraps = 0;

    auto start = steady_clock::now();
    std::array<struct iovec, 2> iovecs{};

    while (totalWritten < targetData ||
           steady_clock::now() - start < MIN_WRAP_SAMPLE_DURATION) {
        size_t written = mmap_buffer.write(data.data(), data.size());
        benchmarkBarrier(&mmap_buffer);
        totalWritten += written;
        if (mmap_buffer.getReadIovecs(iovecs) > 1) {
            ++fallback_physical_wraps;
        }

        const size_t toConsume = mmap_buffer.readable() / 2;
        mmap_buffer.consume(toConsume);
        benchmarkBarrier(&mmap_buffer);
    }

    auto end = steady_clock::now();
    double durationMs = duration_cast<microseconds>(end - start).count() / 1000.0;
    const size_t logical_wraps = totalWritten / mmap_buffer.capacity();

    if (fallback_physical_wraps == 0) {
        LogInfo("Mmap Logical Wrap Performance: {:.2f} MB/s, logical_wraps: {}, read_iovecs: 1, duration: {:.2f} ms",
                (totalWritten / (1024.0 * 1024.0)) / (durationMs / 1000.0),
                logical_wraps, durationMs);
    } else {
        LogInfo("Requested Mmap Wrap Performance: skipped=true, actual_backend=vector-fallback, dual_iovec_wraps: {}, duration: {:.2f} ms",
                fallback_physical_wraps, durationMs);
    }

    galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Vector, std::dynamic_extent> vector_buffer(
        SMALL_BUFFER_SIZE);
    totalWritten = 0;
    size_t physical_wraps = 0;
    start = steady_clock::now();
    while (totalWritten < targetData ||
           steady_clock::now() - start < MIN_WRAP_SAMPLE_DURATION) {
        const size_t written = vector_buffer.write(data.data(), data.size());
        benchmarkBarrier(&vector_buffer);
        totalWritten += written;
        if (vector_buffer.getReadIovecs(iovecs) == 2) {
            ++physical_wraps;
        }

        const size_t toConsume = vector_buffer.readable() / 2;
        vector_buffer.consume(toConsume);
        benchmarkBarrier(&vector_buffer);
    }
    end = steady_clock::now();
    durationMs = duration_cast<microseconds>(end - start).count() / 1000.0;
    if (physical_wraps == 0) {
        g_benchmark_valid.store(false, std::memory_order_relaxed);
    }
    LogInfo("Vector Physical Wrap Performance: {:.2f} MB/s, dual_iovec_wraps: {}, duration: {:.2f} ms",
            (totalWritten / (1024.0 * 1024.0)) / (durationMs / 1000.0),
            physical_wraps, durationMs);
}

// ============== 压测4: iovec 获取性能 ==============

void benchIovecPerformance() {
    RingBuffer buffer(BUFFER_SIZE);

    // 先写入一些数据
    std::vector<char> data(1024, 'D');
    const size_t prepared = buffer.write(data.data(), data.size());
    if (prepared != data.size()) {
        g_benchmark_valid.store(false, std::memory_order_relaxed);
        return;
    }

    constexpr size_t iterations = 10000000;
    std::array<struct iovec, 2> writeIovecs{};
    std::array<struct iovec, 2> readIovecs{};
    size_t sink = 0;

    const double duration1 = medianSample([&]() {
        auto start1 = steady_clock::now();
        for (size_t i = 0; i < iterations; i++) {
            sink += buffer.getWriteIovecs(writeIovecs);
            benchmarkBarrier(writeIovecs.data());
        }
        auto end1 = steady_clock::now();
        return duration_cast<nanoseconds>(end1 - start1).count() / (double)iterations;
    });

    const double duration2 = medianSample([&]() {
        auto start2 = steady_clock::now();
        for (size_t i = 0; i < iterations; i++) {
            sink += buffer.getReadIovecs(readIovecs);
            benchmarkBarrier(readIovecs.data());
        }
        auto end2 = steady_clock::now();
        return duration_cast<nanoseconds>(end2 - start2).count() / (double)iterations;
    });
    observeChecksum(sink);

    LogInfo("getWriteIovecs: {:.2f} amortized ns/call ({:.2f} M calls/s)",
            duration1, 1000.0 / duration1);
    LogInfo("getReadIovecs: {:.2f} amortized ns/call ({:.2f} M calls/s)",
            duration2, 1000.0 / duration2);
}

// ============== 压测5: produce/consume 性能 ==============

void benchProduceConsumePerformance() {
    RingBuffer buffer(BUFFER_SIZE);

    constexpr size_t iterations = 100000000;
    size_t checksum = 0;

    const double duration = medianSample([&]() {
        auto start = steady_clock::now();
        for (size_t i = 0; i < iterations; i++) {
            const size_t amount = 1 + (i & 63);
            buffer.produce(amount);
            benchmarkBarrier(&buffer);
            checksum += buffer.readable();
            buffer.consume(amount);
            benchmarkBarrier(&buffer);
        }
        auto end = steady_clock::now();
        return duration_cast<nanoseconds>(end - start).count() / (double)iterations;
    });
    observeChecksum(checksum);

    LogInfo("produce+consume: {:.2f} amortized ns/pair ({:.2f} M pairs/s)",
            duration, 1000.0 / duration);
}

// ============== 压测6: 模拟网络接收场景 ==============

void benchNetworkReceiveSimulation() {
    RingBuffer buffer(BUFFER_SIZE);
    std::vector<char> networkData(4096, 'E');

    size_t totalReceived = 0;
    size_t checksum = 0;
    size_t targetData = 500 * 1024 * 1024;  // 500MB

    auto start = steady_clock::now();
    std::array<struct iovec, 2> writeIovecs{};

    while (totalReceived < targetData) {
        // 模拟 readv: 获取可写 iovec
        size_t writeCount = buffer.getWriteIovecs(writeIovecs);
        if (writeCount == 0) {
            // 缓冲区满，消费数据
            buffer.consume(buffer.readable());
            continue;
        }

        // 模拟数据到达：直接拷贝到 iovec
        size_t bytesReceived = 0;
        for (size_t i = 0; i < writeCount; ++i) {
            auto& iov = writeIovecs[i];
            size_t toWrite = std::min(iov.iov_len, networkData.size() - bytesReceived);
            std::memcpy(iov.iov_base, networkData.data() + bytesReceived, toWrite);
            if (toWrite > 0) {
                checksum += static_cast<unsigned char*>(iov.iov_base)[toWrite - 1];
            }
            bytesReceived += toWrite;
            if (bytesReceived >= networkData.size()) break;
        }
        buffer.produce(bytesReceived);
        benchmarkBarrier(&buffer);
        totalReceived += bytesReceived;

        // 模拟应用层处理：消费部分数据
        if (buffer.readable() > BUFFER_SIZE / 2) {
            buffer.consume(buffer.readable() / 2);
            benchmarkBarrier(&buffer);
        }
    }
    observeChecksum(checksum);

    auto end = steady_clock::now();
    double durationMs = duration_cast<microseconds>(end - start).count() / 1000.0;

    printResult("Memory-only Receive Copy", totalReceived, durationMs);
}

// ============== 压测7: 模拟网络发送场景 ==============

void benchNetworkSendSimulation() {
    RingBuffer buffer(BUFFER_SIZE);
    std::vector<char> appData(1024, 'F');
    std::vector<char> networkBuffer(4096);

    size_t totalSent = 0;
    size_t checksum = 0;
    size_t targetData = 500 * 1024 * 1024;  // 500MB

    auto start = steady_clock::now();
    std::array<struct iovec, 2> readIovecs{};

    while (totalSent < targetData) {
        // 应用层写入数据
        while (buffer.writable() >= appData.size()) {
            const size_t written = buffer.write(appData.data(), appData.size());
            if (written != appData.size()) {
                g_benchmark_valid.store(false, std::memory_order_relaxed);
                break;
            }
            benchmarkBarrier(&buffer);
        }

        // 模拟 writev: 获取可读 iovec
        size_t readCount = buffer.getReadIovecs(readIovecs);
        if (readCount == 0) continue;

        // 模拟发送：从 iovec 拷贝到网络缓冲区
        size_t bytesSent = 0;
        for (size_t i = 0; i < readCount; ++i) {
            const auto& iov = readIovecs[i];
            size_t toSend = std::min(iov.iov_len, networkBuffer.size() - bytesSent);
            std::memcpy(networkBuffer.data() + bytesSent, iov.iov_base, toSend);
            bytesSent += toSend;
            if (bytesSent >= networkBuffer.size()) break;
        }
        if (bytesSent > 0) {
            checksum += static_cast<unsigned char>(networkBuffer[bytesSent - 1]);
            benchmarkBarrier(networkBuffer.data());
        }
        buffer.consume(bytesSent);
        benchmarkBarrier(&buffer);
        totalSent += bytesSent;
    }
    observeChecksum(checksum);

    auto end = steady_clock::now();
    double durationMs = duration_cast<microseconds>(end - start).count() / 1000.0;

    printResult("Memory-only Send Copy", totalSent, durationMs);
}

int main() {
    LogInfo("========================================");
    LogInfo("RingBuffer Performance Benchmark");
    LogInfo("========================================");
    LogInfo("Buffer size: {} KB", BUFFER_SIZE / 1024);
    LogInfo("Total data: {} MB", TOTAL_DATA_SIZE / (1024 * 1024));
    LogInfo("meta: benchmark_kind=single_thread_hot_cache memory_only=true syscalls=0 compiler_observable=true network_results_are_copy_simulations");
    LogInfo("");

    // 压测1: 写入吞吐量
    LogInfo("--- Write Throughput ---");
    for (size_t chunkSize : CHUNK_SIZES) {
        benchWriteThroughput(chunkSize);
    }
    LogInfo("");

    // 压测2: 读写吞吐量
    LogInfo("--- Read/Write Throughput ---");
    for (size_t chunkSize : CHUNK_SIZES) {
        benchReadWriteThroughput(chunkSize);
    }
    LogInfo("");

    // 压测3: 环绕性能
    LogInfo("--- Wrap Around Performance ---");
    benchWrapAroundPerformance();
    LogInfo("");

    // 压测4: iovec 获取性能
    LogInfo("--- iovec Performance ---");
    benchIovecPerformance();
    LogInfo("");

    // 压测5: produce/consume 性能
    LogInfo("--- produce/consume Performance ---");
    benchProduceConsumePerformance();
    LogInfo("");

    // 压测6: 网络接收模拟
    LogInfo("--- Memory-only Receive Copy ---");
    benchNetworkReceiveSimulation();
    LogInfo("");

    // 压测7: 网络发送模拟
    LogInfo("--- Memory-only Send Copy ---");
    benchNetworkSendSimulation();
    LogInfo("");

    LogInfo("========================================");
    LogInfo("Benchmark Complete");
    LogInfo("========================================");

    if (!g_benchmark_valid.load(std::memory_order_relaxed) ||
        g_benchmark_sink.load(std::memory_order_relaxed) == static_cast<size_t>(-1)) {
        return 1;
    }
    return 0;
}
