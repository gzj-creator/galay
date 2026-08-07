/**
 * @file t166_spsc_paired_final_drain_source.cc
 * @brief 锁定 SPSC paired benchmark 的最终 drain、退避和批量对照协议。
 */

#include "result_writer.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string compactWhitespace(std::string_view content)
{
    std::string compact;
    compact.reserve(content.size());
    bool pendingSpace = false;
    for (const char ch : content) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            pendingSpace = !compact.empty();
            continue;
        }
        if (pendingSpace) {
            compact.push_back(' ');
            pendingSpace = false;
        }
        compact.push_back(ch);
    }
    return compact;
}

size_t countOccurrences(std::string_view content, std::string_view needle) noexcept
{
    if (needle.empty()) {
        return 0;
    }

    size_t count = 0;
    size_t position = 0;
    while ((position = content.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

void requireCount(std::vector<std::string>& failures,
                  std::string_view content,
                  std::string_view needle,
                  size_t expected,
                  std::string_view label)
{
    const size_t actual = countOccurrences(content, needle);
    if (actual != expected) {
        failures.push_back(std::string(label) + ": expected " +
                           std::to_string(expected) + ", found " +
                           std::to_string(actual));
    }
}

void requireAtLeast(std::vector<std::string>& failures,
                    std::string_view content,
                    std::string_view needle,
                    size_t minimum,
                    std::string_view label)
{
    const size_t actual = countOccurrences(content, needle);
    if (actual < minimum) {
        failures.push_back(std::string(label) + ": expected at least " +
                           std::to_string(minimum) + ", found " +
                           std::to_string(actual));
    }
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t166_spsc_paired_final_drain_source");
    writer.addTest();

    const std::filesystem::path root(GALAY_PROJECT_ROOT);
    const auto cppPath = root / "benchmark" / "cpp" / "kernel" / "compare" /
        "spsc-paired" / "spsc_paired.cc";
    const auto rustPath = root / "benchmark" / "cpp" / "kernel" / "compare" /
        "rust-channel" / "src" / "bin" / "spsc_paired.rs";
    const auto runnerPath = root / "benchmark" / "cpp" / "kernel" / "compare" /
        "run_spsc_paired.py";

    const std::string cpp = readAll(cppPath);
    const std::string rust = readAll(rustPath);
    const std::string runner = readAll(runnerPath);
    std::vector<std::string> failures;
    if (cpp.empty()) {
        failures.push_back("failed to read " + cppPath.string());
    }
    if (rust.empty()) {
        failures.push_back("failed to read " + rustPath.string());
    }
    if (runner.empty()) {
        failures.push_back("failed to read " + runnerPath.string());
    }

    if (!cpp.empty()) {
        const std::string compact = compactWhitespace(cpp);
        requireAtLeast(
            failures,
            compact,
            "while (consumerResult.received < config.messages)",
            2,
            "C++ consumer must drain until the expected message count");
        requireCount(
            failures,
            compact,
            "producerDoneObserved",
            0,
            "C++ consumer must not use a single empty read as completion");
        requireCount(
            failures,
            compact,
            "galay::spsc::UnboundedChannel<uint64_t> channel",
            2,
            "C++ scalar and batch unbounded cases must use the waiter-capable SPSC channel");
        requireCount(
            failures,
            compact,
            "galay::spsc::UnboundedQueue<uint64_t> channel",
            0,
            "C++ unbounded case must not compare a polling-only queue with a channel");
        requireCount(
            failures,
            compact,
            "galay::spsc::BoundedChannel<uint64_t> channel",
            1,
            "C++ paired benchmark must cover the waiter-capable bounded channel");
        requireCount(
            failures,
            compact,
            "auto endpoints = channel.split()",
            2,
            "C++ scalar and batch bounded cases must move split SPSC endpoints to the workers");
        requireCount(
            failures,
            compact,
            "galay.spsc.paired.v4",
            1,
            "C++ result must use the audited paired schema");
        requireCount(
            failures,
            compact,
            "nearest_available_measured_path",
            1,
            "C++ unbounded result must expose its comparison limitation");
        requireCount(
            failures,
            compact,
            "tryWriteBatch(",
            1,
            "C++ bounded batch case must use the ring batch write API");
        requireCount(
            failures,
            compact,
            "channel.sendBatch(",
            1,
            "C++ unbounded batch case must use the channel batch send API");
        requireCount(
            failures,
            compact,
            "tryReadBatch(",
            1,
            "C++ bounded batch case must use the ring batch read API");
        requireCount(
            failures,
            compact,
            "tryRecvBatch(",
            1,
            "C++ unbounded batch case must use the channel batch receive API");
        requireAtLeast(
            failures,
            compact,
            "CaseKind::kBatchUnbounded",
            5,
            "C++ benchmark must parse, execute, and report batch_unbounded");
        requireAtLeast(
            failures,
            compact,
            "value == consumerResult.received",
            1,
            "C++ scalar consumers must validate FIFO order");
        requireCount(
            failures,
            compact,
            "orderMismatch |= values[offset] ^ (firstExpected + static_cast<uint64_t>(offset))",
            1,
            "C++ batch consumers must validate FIFO order from a fixed batch base");
        requireCount(
            failures,
            compact,
            "measurement.received == config->messages",
            1,
            "C++ result validity must validate the exact message count");
        requireCount(
            failures,
            compact,
            "measurement.checksum == measurement.expectedChecksum",
            1,
            "C++ result validity must validate checksum");
        requireAtLeast(
            failures,
            compact,
            "BackoffKind::kSpin",
            2,
            "C++ benchmark must parse and execute the spin backoff");
        requireAtLeast(
            failures,
            compact,
            "BackoffKind::kHybrid",
            2,
            "C++ benchmark must parse and execute the hybrid backoff");
    }

    if (!rust.empty()) {
        const std::string compact = compactWhitespace(rust);
        requireAtLeast(
            failures,
            compact,
            "while result.received < messages",
            3,
            "Rust consumers must drain until the expected message count");
        requireCount(
            failures,
            compact,
            "producer_done_observed",
            0,
            "Rust consumers must not use a single empty pop as completion");
        requireAtLeast(
            failures,
            compact,
            "producer_finished",
            4,
            "Rust unbounded final drain must observe producer completion");
        requireAtLeast(
            failures,
            compact,
            "consumer_done",
            4,
            "Rust unbounded sender endpoint must outlive the final drain");
        requireAtLeast(
            failures,
            compact,
            "stalled_since",
            4,
            "Rust unbounded final drain must terminate bounded stalls as invalid");
        requireAtLeast(
            failures,
            compact,
            "RingBuffer::<u64>::new(config.capacity)",
            2,
            "Rust bounded case must use the dedicated rtrb SPSC ring");
        requireCount(
            failures,
            compact,
            "unbounded_spsc::channel::<u64>()",
            2,
            "Rust scalar and batch unbounded cases must use the dedicated unbounded SPSC channel");
        requireCount(
            failures,
            compact,
            "crossbeam_queue",
            0,
            "Rust SPSC comparison must not use an MPMC queue");
        requireCount(
            failures,
            compact,
            "galay.spsc.paired.v4",
            1,
            "Rust result must use the audited paired schema");
        requireCount(
            failures,
            compact,
            "nearest_available_measured_path",
            1,
            "Rust unbounded result must expose its comparison limitation");
        requireCount(
            failures,
            compact,
            "push_partial_slice(",
            1,
            "Rust batch case must copy from the caller-owned batch buffer");
        requireCount(
            failures,
            compact,
            "pop_partial_slice(",
            1,
            "Rust batch case must copy into the caller-owned batch buffer");
        requireAtLeast(
            failures,
            compact,
            "CaseKind::BatchUnbounded",
            2,
            "Rust benchmark must parse, execute, and report batch_unbounded");
        requireAtLeast(
            failures,
            compact,
            "value == result.received",
            2,
            "Rust scalar consumers must validate FIFO order");
        requireCount(
            failures,
            compact,
            "order_mismatch |= value ^ (first_expected + offset as u64)",
            2,
            "Rust batch consumers must validate FIFO order from a fixed batch base");
        requireCount(
            failures,
            compact,
            "measurement.received == config.messages",
            1,
            "Rust result validity must validate the exact message count");
        requireCount(
            failures,
            compact,
            "measurement.checksum == measurement.expected_checksum",
            1,
            "Rust result validity must validate checksum");
        requireCount(
            failures,
            compact,
            "BackoffKind::Spin",
            2,
            "Rust benchmark must parse and execute the spin backoff");
        requireCount(
            failures,
            compact,
            "BackoffKind::Hybrid",
            2,
            "Rust benchmark must parse and execute the hybrid backoff");
    }

    if (!runner.empty()) {
        const std::string compact = compactWhitespace(runner);
        requireCount(
            failures,
            compact,
            "galay.spsc.paired.v4",
            1,
            "runner must reject pre-v4 benchmark output");
        requireCount(
            failures,
            compact,
            "galay.spsc.paired.report.v4",
            1,
            "runner report must use the v4 schema");
        requireAtLeast(
            failures,
            compact,
            "batch_bounded",
            5,
            "runner must register and validate the batch case");
        requireAtLeast(
            failures,
            compact,
            "batch_unbounded",
            5,
            "runner must register and validate the unbounded batch case");
        requireAtLeast(
            failures,
            compact,
            "comparison_eligible",
            2,
            "runner must keep reference-only cases out of formal wins");
        requireAtLeast(
            failures,
            compact,
            "p99",
            1,
            "runner must report QPS p99");
        requireAtLeast(
            failures,
            compact,
            "retry_ratio",
            2,
            "runner must report and gate retry ratios");
        requireAtLeast(
            failures,
            compact,
            "placement_passed",
            2,
            "runner acceptance must require real pinned placement");
        requireAtLeast(
            failures,
            compact,
            "--backoff",
            2,
            "runner must accept and forward the selected backoff");
        requireCount(
            failures,
            compact,
            "default=\"raw_bounded,batch_bounded\"",
            1,
            "runner default scope must contain only bounded SPSC cases");
        requireCount(
            failures,
            compact,
            "--max-cv-percent\", type=float, default=25.0",
            1,
            "runner default CV gate must match the audited 25 percent threshold");
    }

    if (!failures.empty()) {
        for (const std::string& failure : failures) {
            std::cerr << failure << '\n';
        }
        writer.addFailed();
        writer.writeResult();
        return 1;
    }

    writer.addPassed();
    writer.writeResult();
    std::cout << "t166_spsc_paired_final_drain_source PASS\n";
    return 0;
}
