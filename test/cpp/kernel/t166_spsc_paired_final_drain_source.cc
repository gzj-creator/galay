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
            1,
            "C++ unbounded case must use the waiter-capable SPSC channel");
        requireCount(
            failures,
            compact,
            "galay::spsc::UnboundedQueue<uint64_t> channel",
            0,
            "C++ unbounded case must not compare a polling-only queue with a channel");
        requireCount(
            failures,
            compact,
            "auto endpoints = channel.split()",
            1,
            "C++ bounded case must move split SPSC endpoints to the workers");
        requireCount(
            failures,
            compact,
            "galay.spsc.paired.v3",
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
            "trySendBatch(",
            1,
            "C++ batch case must use the ring batch send API");
        requireCount(
            failures,
            compact,
            "tryRecvBatch(",
            1,
            "C++ batch case must use the ring batch receive API");
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
            2,
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
            "RingBuffer::<u64>::new(config.capacity)",
            2,
            "Rust bounded case must use the dedicated rtrb SPSC ring");
        requireCount(
            failures,
            compact,
            "unbounded_spsc::channel::<u64>()",
            1,
            "Rust unbounded case must use the dedicated unbounded SPSC channel");
        requireCount(
            failures,
            compact,
            "crossbeam_queue",
            0,
            "Rust SPSC comparison must not use an MPMC queue");
        requireCount(
            failures,
            compact,
            "galay.spsc.paired.v3",
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
            "write_chunk_uninit(",
            1,
            "Rust batch case must use the rtrb batch write API");
        requireCount(
            failures,
            compact,
            "read_chunk(",
            1,
            "Rust batch case must use the rtrb batch read API");
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
            "galay.spsc.paired.v3",
            1,
            "runner must reject pre-v3 benchmark output");
        requireCount(
            failures,
            compact,
            "galay.spsc.paired.report.v3",
            1,
            "runner report must use the v3 schema");
        requireAtLeast(
            failures,
            compact,
            "batch_bounded",
            5,
            "runner must register and validate the batch case");
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
