/**
 * @file t166_spsc_paired_final_drain_source.cc
 * @brief 锁定 C++/Rust SPSC paired benchmark 按预期消息数完成最终 drain。
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

    const std::string cpp = readAll(cppPath);
    const std::string rust = readAll(rustPath);
    std::vector<std::string> failures;
    if (cpp.empty()) {
        failures.push_back("failed to read " + cppPath.string());
    }
    if (rust.empty()) {
        failures.push_back("failed to read " + rustPath.string());
    }

    if (!cpp.empty()) {
        const std::string compact = compactWhitespace(cpp);
        requireCount(
            failures,
            compact,
            "while (consumerResult.received < config.messages)",
            1,
            "C++ consumer must drain until the expected message count");
        requireCount(
            failures,
            compact,
            "producerDoneObserved",
            0,
            "C++ consumer must not use a single empty read as completion");
    }

    if (!rust.empty()) {
        const std::string compact = compactWhitespace(rust);
        requireCount(
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
