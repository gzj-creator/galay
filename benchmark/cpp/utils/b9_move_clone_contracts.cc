#include <galay/cpp/galay-utils/algorithm/bloom_filter.hpp>
#include <galay/cpp/galay-utils/algorithm/huffman.hpp>
#include <galay/cpp/galay-utils/cache/bytes.hpp>
#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

volatile std::size_t g_sink = 0;

struct Result {
    std::string name;
    double nsPerOp;
    std::size_t checksum;
};

std::size_t parseArg(int argc, char** argv, int index, std::size_t fallback) {
    if (argc <= index) {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long long value = std::strtoull(argv[index], &end, 10);
    if (end == argv[index] || *end != '\0' || value == 0) {
        return fallback;
    }
    return static_cast<std::size_t>(value);
}

uint64_t stableHash(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

template<typename Fn>
Result measure(std::string name, std::size_t iterations, Fn&& fn) {
    std::size_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        checksum += fn(i);
    }
    const auto end = std::chrono::steady_clock::now();

    g_sink = checksum;
    const auto elapsedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double nsPerOp = static_cast<double>(elapsedNs) / static_cast<double>(iterations);
    return Result{std::move(name), nsPerOp, checksum};
}

void printResult(const Result& result) {
    std::cout << std::left << std::setw(32) << result.name
              << std::right << std::setw(14) << std::fixed << std::setprecision(2)
              << result.nsPerOp
              << "  checksum=" << result.checksum << '\n';
}

galay::utils::BloomFilter<uint64_t> makeBloom(std::size_t items) {
    auto filter = galay::utils::BloomFilter<uint64_t>::fromExpectedItems(items, 0.01);
    for (std::size_t i = 0; i < items; ++i) {
        filter.addHash(stableHash(i));
    }
    return filter;
}

std::vector<char> makeHuffmanData(std::size_t items) {
    std::vector<char> data;
    data.reserve(items);
    for (std::size_t i = 0; i < items; ++i) {
        data.push_back(static_cast<char>('a' + (i % 8)));
    }
    return data;
}

bool prepareRing(galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Vector>& buffer,
                 std::string_view prefix,
                 std::string_view suffix) {
    const std::size_t prefix_written = buffer.write(prefix.data(), prefix.size());
    if (prefix_written != prefix.size()) {
        return false;
    }
    buffer.consume(prefix.size() / 2);
    const std::size_t suffix_written = buffer.write(suffix.data(), suffix.size());
    return suffix_written == suffix.size();
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t iterations = parseArg(argc, argv, 1, 1000);
    const std::size_t items = parseArg(argc, argv, 2, 4096);
    const std::size_t ringCapacity = std::max<std::size_t>(parseArg(argc, argv, 3, 8192), 128);

    std::cout << "Move/clone contract benchmark\n";
    std::cout << "Build with -O3 -DNDEBUG. iterations=" << iterations
              << ", items=" << items
              << ", ring_capacity=" << ringCapacity << '\n';
    std::cout << std::left << std::setw(32) << "Scenario"
              << std::right << std::setw(14) << "ns/op" << '\n';

    const auto bloom = makeBloom(items);
    printResult(measure("Bloom clone", iterations, [&](std::size_t i) {
        auto copy = bloom.clone();
        return copy.insertionCount() + (copy.possiblyContainsHash(stableHash(i % items)) ? 1u : 0u);
    }));

    {
        std::vector<galay::utils::BloomFilter<uint64_t>> copies;
        copies.reserve(iterations);
        for (std::size_t i = 0; i < iterations; ++i) {
            copies.emplace_back(bloom.clone());
        }
        printResult(measure("Bloom move", iterations, [&](std::size_t i) {
            galay::utils::BloomFilter<uint64_t> moved(std::move(copies[i]));
            return moved.bitCount() + moved.insertionCount();
        }));
    }

    printResult(measure("Bloom construct", iterations, [&](std::size_t i) {
        auto filter = galay::utils::BloomFilter<uint64_t>::fromExpectedItems(items, 0.01);
        filter.addHash(stableHash(i));
        return filter.bitCount() + filter.insertionCount();
    }));

    const auto huffmanData = makeHuffmanData(items);
    const auto huffman = galay::utils::HuffmanBuilder<char>::buildFromData(huffmanData);
    printResult(measure("Huffman clone", iterations, [&](std::size_t i) {
        auto copy = huffman.clone();
        return copy.size() + (copy.hasSymbol(static_cast<char>('a' + (i % 8))) ? 1u : 0u);
    }));

    {
        std::vector<galay::utils::HuffmanTable<char>> copies;
        copies.reserve(iterations);
        for (std::size_t i = 0; i < iterations; ++i) {
            copies.emplace_back(huffman.clone());
        }
        printResult(measure("Huffman move", iterations, [&](std::size_t i) {
            galay::utils::HuffmanTable<char> moved(std::move(copies[i]));
            return moved.size();
        }));
    }

    printResult(measure("Huffman build", iterations, [&](std::size_t) {
        auto table = galay::utils::HuffmanBuilder<char>::buildFromData(huffmanData);
        return table.size();
    }));

    const std::string payload(items, 'x');
    const galay::utils::Bytes bytes(payload.data(), payload.size());
    printResult(measure("Bytes clone", iterations, [&](std::size_t) {
        auto copy = bytes.clone();
        return copy.size() + copy.capacity();
    }));

    {
        std::vector<galay::utils::Bytes> copies;
        copies.reserve(iterations);
        for (std::size_t i = 0; i < iterations; ++i) {
            copies.emplace_back(bytes.clone());
        }
        printResult(measure("Bytes move", iterations, [&](std::size_t i) {
            galay::utils::Bytes moved(std::move(copies[i]));
            return moved.size();
        }));
    }

    printResult(measure("Bytes construct", iterations, [&](std::size_t) {
        galay::utils::Bytes constructed(payload.data(), payload.size());
        return constructed.size() + constructed.capacity();
    }));

    const std::string prefix(ringCapacity / 2, 'a');
    const std::string suffix(ringCapacity / 4, 'b');
    galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Vector> ring(ringCapacity);
    if (!prepareRing(ring, prefix, suffix)) {
        std::cerr << "failed to prepare ring buffer workload\n";
        return 1;
    }

    printResult(measure("RingBuffer clone", iterations, [&](std::size_t) {
        auto copy = ring.clone();
        return copy.readable() + copy.capacity();
    }));

    {
        std::vector<galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Vector>> copies;
        copies.reserve(iterations);
        for (std::size_t i = 0; i < iterations; ++i) {
            copies.emplace_back(ring.clone());
        }
        printResult(measure("RingBuffer move", iterations, [&](std::size_t i) {
            galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Vector> moved(
                std::move(copies[i]));
            return moved.readable() + moved.capacity();
        }));
    }

    printResult(measure("RingBuffer construct+write", iterations, [&](std::size_t) {
        galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Vector> constructed(ringCapacity);
        const std::size_t written = constructed.write(prefix.data(), prefix.size());
        return written + constructed.capacity();
    }));

    return static_cast<int>(g_sink == static_cast<std::size_t>(-1));
}
