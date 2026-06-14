#include "test_common.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>

template<typename Filter>
concept HasPreciseContains = requires(Filter filter) {
    filter.contains(1);
};

uint64_t stableBloomTestHash(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

void testTrieTree() {
    std::cout << "=== Testing TrieTree ===" << std::endl;

    TrieTree trie;

    trie.add("hello");
    trie.add("help");
    trie.add("world");
    trie.add("hello"); // Duplicate

    assert(trie.size() == 3);
    assert(trie.contains("hello"));
    assert(trie.contains("help"));
    assert(!trie.contains("hel"));

    assert(trie.startsWith("hel"));
    assert(trie.startsWith("wor"));
    assert(!trie.startsWith("xyz"));

    assert(trie.query("hello") == 2); // Added twice
    assert(trie.query("help") == 1);

    auto words = trie.getWordsWithPrefix("hel");
    assert(words.size() == 2);

    assert(trie.remove("hello"));
    assert(!trie.contains("hello"));
    assert(trie.size() == 2);

    std::cout << "TrieTree tests passed!" << std::endl;
}

// ==================== Huffman Tests ====================

void testHuffman() {
    std::cout << "=== Testing Huffman ===" << std::endl;

    // Build table from data
    std::vector<char> data = {'a', 'a', 'a', 'b', 'b', 'c'};
    auto table = HuffmanBuilder<char>::buildFromData(data);

    assert(table.size() == 3);
    assert(table.hasSymbol('a'));
    assert(table.hasSymbol('b'));
    assert(table.hasSymbol('c'));

    // Encode
    HuffmanEncoder<char> encoder(table);
    encoder.encode(data);
    auto encoded = encoder.finish();

    // Decode
    HuffmanDecoder<char> decoder(table, 1, 8);
    auto decoded = decoder.decode(encoded, data.size());

    assert(decoded.size() == data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        assert(decoded[i] == data[i]);
    }

    std::cout << "Huffman tests passed!" << std::endl;
}

// ==================== MVCC Tests ====================

void testMvcc() {
    std::cout << "=== Testing MVCC ===" << std::endl;

    Mvcc<std::string> mvcc;

    // Put values
    Version v1 = mvcc.putValue("value1");
    assert(v1 == 1);

    Version v2 = mvcc.putValue("value2");
    assert(v2 == 2);

    // Read current
    const std::string* current = mvcc.getCurrentValue();
    assert(current != nullptr);
    assert(*current == "value2");

    // Read by version
    const std::string* val1 = mvcc.getValue(v1);
    assert(val1 != nullptr);
    assert(*val1 == "value1");

    // Snapshot
    Snapshot snapshot(v1);
    const std::string* snapshotVal = snapshot.read(mvcc);
    assert(snapshotVal != nullptr);
    assert(*snapshotVal == "value1");

    // Transaction
    Transaction<std::string> txn(mvcc);
    const std::string* readVal = txn.read();
    assert(readVal != nullptr);
    assert(*readVal == "value2");

    txn.write(std::make_unique<std::string>("value3"));
    assert(txn.commit());
    assert(*mvcc.getCurrentValue() == "value3");

    // GC
    assert(mvcc.versionCount() == 3);
    mvcc.gc(2);
    assert(mvcc.versionCount() == 2);

    std::cout << "MVCC tests passed!" << std::endl;
}

// ==================== Bloom Filter Tests ====================

void testBloomFilter() {
    std::cout << "=== Testing BloomFilter ===" << std::endl;

    static_assert(!HasPreciseContains<BloomFilter<int>>);

    BloomFilter<int> minFilter(1);
    assert(minFilter.bitCount() == BloomFilter<int>::kBitsPerBlock);
    assert(minFilter.blockCount() == 1);
    assert(minFilter.hashCount() == BloomFilter<int>::kHashCount);

    BloomFilter<int> roundedFilter(BloomFilter<int>::kBitsPerBlock + 1);
    assert(roundedFilter.bitCount() == BloomFilter<int>::kBitsPerBlock * 2);
    assert(roundedFilter.blockCount() == 2);

    auto filter = BloomFilter<std::string>::fromExpectedItems(128, 0.01);
    assert(filter.bitCount() >= 256);
    assert(filter.bitCount() % BloomFilter<std::string>::kBitsPerBlock == 0);
    assert(filter.blockCount() > 0);
    assert(filter.hashCount() == 8);
    assert(filter.empty());
    assert(filter.insertionCount() == 0);

    assert(!filter.possiblyContains("alpha"));
    filter.add("alpha");
    filter.add("beta");
    filter.add("alpha");

    assert(!filter.empty());
    assert(filter.insertionCount() == 3);
    assert(filter.possiblyContains("alpha"));
    assert(filter.possiblyContains("beta"));

    filter.clear();
    assert(filter.empty());
    assert(filter.insertionCount() == 0);
    assert(!filter.possiblyContains("alpha"));
    assert(!filter.possiblyContains("beta"));

    BloomFilter<uint64_t> hashFilter(256);
    hashFilter.addHash(0x123456789abcdef0ULL);
    assert(hashFilter.possiblyContainsHash(0x123456789abcdef0ULL));
    assert(!hashFilter.possiblyContainsHash(0xfedcba9876543210ULL));

    bool invalidExpectedItems = false;
    try {
        (void)BloomFilter<int>::fromExpectedItems(0, 0.01);
    } catch (const std::invalid_argument&) {
        invalidExpectedItems = true;
    }
    assert(invalidExpectedItems);

    bool invalidFalsePositiveRate = false;
    try {
        (void)BloomFilter<int>::fromExpectedItems(10, 1.0);
    } catch (const std::invalid_argument&) {
        invalidFalsePositiveRate = true;
    }
    assert(invalidFalsePositiveRate);

    bool invalidZeroBitCount = false;
    try {
        BloomFilter<int> invalid(0);
    } catch (const std::invalid_argument&) {
        invalidZeroBitCount = true;
    }
    assert(invalidZeroBitCount);

    bool invalidZeroFalsePositiveRate = false;
    try {
        (void)BloomFilter<int>::bitCountForExpectedItems(10, 0.0);
    } catch (const std::invalid_argument&) {
        invalidZeroFalsePositiveRate = true;
    }
    assert(invalidZeroFalsePositiveRate);

    bool invalidNaNFalsePositiveRate = false;
    try {
        (void)BloomFilter<int>::bitCountForExpectedItems(
            10, std::numeric_limits<double>::quiet_NaN());
    } catch (const std::invalid_argument&) {
        invalidNaNFalsePositiveRate = true;
    }
    assert(invalidNaNFalsePositiveRate);

    constexpr size_t stressItems = 50000;
    auto stressFilter = BloomFilter<uint64_t>::fromExpectedItems(stressItems, 0.01);
    std::vector<uint64_t> inserted;
    inserted.reserve(stressItems);

    for (uint64_t i = 0; i < stressItems; ++i) {
        const uint64_t hash = stableBloomTestHash(i);
        inserted.push_back(hash);
        stressFilter.addHash(hash);
    }
    assert(stressFilter.insertionCount() == stressItems);

    for (uint64_t hash : inserted) {
        assert(stressFilter.possiblyContainsHash(hash));
    }

    size_t falsePositives = 0;
    for (uint64_t i = 0; i < stressItems; ++i) {
        const uint64_t hash = stableBloomTestHash(i + 1000000ULL);
        if (stressFilter.possiblyContainsHash(hash)) {
            ++falsePositives;
        }
    }
    assert(falsePositives < stressItems / 20);

    std::cout << "BloomFilter tests passed!" << std::endl;
}

// ==================== Parser Tests ====================

int main() {
    std::cout << "\n=== data_test ===" << std::endl;
    try {
        testTrieTree();
        testHuffman();
        testMvcc();
        testBloomFilter();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
