#include "test_common.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>

template<typename Filter>
concept HasPreciseContains = requires(Filter filter) {
    filter.contains(1);
};

uint64_t stable_bloom_test_hash(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

void test_trie_tree() {
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

    assert(trie.starts_with("hel"));
    assert(trie.starts_with("wor"));
    assert(!trie.starts_with("xyz"));

    assert(trie.query("hello") == 2); // Added twice
    assert(trie.query("help") == 1);

    auto words = trie.get_words_with_prefix("hel");
    assert(words.size() == 2);

    assert(trie.remove("hello"));
    assert(!trie.contains("hello"));
    assert(trie.size() == 2);

    std::cout << "TrieTree tests passed!" << std::endl;
}

// ==================== Huffman Tests ====================

void test_huffman() {
    std::cout << "=== Testing Huffman ===" << std::endl;

    // Build table from data
    std::vector<char> data = {'a', 'a', 'a', 'b', 'b', 'c'};
    auto table = HuffmanBuilder<char>::build_from_data(data);

    assert(table.size() == 3);
    assert(table.has_symbol('a'));
    assert(table.has_symbol('b'));
    assert(table.has_symbol('c'));

    // encode
    HuffmanEncoder<char> encoder(table);
    encoder.encode(data);
    auto encoded = encoder.finish();

    // decode
    HuffmanDecoder<char> decoder(table, 1, 8);
    auto decoded = decoder.decode(encoded, data.size());

    assert(decoded.size() == data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        assert(decoded[i] == data[i]);
    }

    std::cout << "Huffman tests passed!" << std::endl;
}

// ==================== MVCC Tests ====================

void test_mvcc() {
    std::cout << "=== Testing MVCC ===" << std::endl;

    Mvcc<std::string> mvcc;

    // Put values
    Version v1 = mvcc.put_value("value1");
    assert(v1 == 1);

    Version v2 = mvcc.put_value("value2");
    assert(v2 == 2);

    // Read current
    const std::string* current = mvcc.get_current_value();
    assert(current != nullptr);
    assert(*current == "value2");

    // Read by version
    const std::string* val1 = mvcc.get_value(v1);
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
    assert(*mvcc.get_current_value() == "value3");

    // GC
    assert(mvcc.version_count() == 3);
    mvcc.gc(2);
    assert(mvcc.version_count() == 2);

    std::cout << "MVCC tests passed!" << std::endl;
}

// ==================== Bloom Filter Tests ====================

void test_bloom_filter() {
    std::cout << "=== Testing BloomFilter ===" << std::endl;

    static_assert(!HasPreciseContains<BloomFilter<int>>);

    BloomFilter<int> minFilter(1);
    assert(minFilter.bit_count() == BloomFilter<int>::kBitsPerBlock);
    assert(minFilter.block_count() == 1);
    assert(minFilter.hash_count() == BloomFilter<int>::kHashCount);

    BloomFilter<int> roundedFilter(BloomFilter<int>::kBitsPerBlock + 1);
    assert(roundedFilter.bit_count() == BloomFilter<int>::kBitsPerBlock * 2);
    assert(roundedFilter.block_count() == 2);

    auto filter = BloomFilter<std::string>::from_expected_items(128, 0.01);
    assert(filter.bit_count() >= 256);
    assert(filter.bit_count() % BloomFilter<std::string>::kBitsPerBlock == 0);
    assert(filter.block_count() > 0);
    assert(filter.hash_count() == 8);
    assert(filter.empty());
    assert(filter.insertion_count() == 0);

    assert(!filter.possibly_contains("alpha"));
    filter.add("alpha");
    filter.add("beta");
    filter.add("alpha");

    assert(!filter.empty());
    assert(filter.insertion_count() == 3);
    assert(filter.possibly_contains("alpha"));
    assert(filter.possibly_contains("beta"));

    filter.clear();
    assert(filter.empty());
    assert(filter.insertion_count() == 0);
    assert(!filter.possibly_contains("alpha"));
    assert(!filter.possibly_contains("beta"));

    BloomFilter<uint64_t> hashFilter(256);
    hashFilter.add_hash(0x123456789abcdef0ULL);
    assert(hashFilter.possibly_contains_hash(0x123456789abcdef0ULL));
    assert(!hashFilter.possibly_contains_hash(0xfedcba9876543210ULL));

    bool invalidExpectedItems = false;
    try {
        (void)BloomFilter<int>::from_expected_items(0, 0.01);
    } catch (const std::invalid_argument&) {
        invalidExpectedItems = true;
    }
    assert(invalidExpectedItems);

    bool invalidFalsePositiveRate = false;
    try {
        (void)BloomFilter<int>::from_expected_items(10, 1.0);
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
        (void)BloomFilter<int>::bit_count_for_expected_items(10, 0.0);
    } catch (const std::invalid_argument&) {
        invalidZeroFalsePositiveRate = true;
    }
    assert(invalidZeroFalsePositiveRate);

    bool invalidNaNFalsePositiveRate = false;
    try {
        (void)BloomFilter<int>::bit_count_for_expected_items(
            10, std::numeric_limits<double>::quiet_NaN());
    } catch (const std::invalid_argument&) {
        invalidNaNFalsePositiveRate = true;
    }
    assert(invalidNaNFalsePositiveRate);

    constexpr size_t stressItems = 50000;
    auto stressFilter = BloomFilter<uint64_t>::from_expected_items(stressItems, 0.01);
    std::vector<uint64_t> inserted;
    inserted.reserve(stressItems);

    for (uint64_t i = 0; i < stressItems; ++i) {
        const uint64_t hash = stable_bloom_test_hash(i);
        inserted.push_back(hash);
        stressFilter.add_hash(hash);
    }
    assert(stressFilter.insertion_count() == stressItems);

    for (uint64_t hash : inserted) {
        assert(stressFilter.possibly_contains_hash(hash));
    }

    size_t falsePositives = 0;
    for (uint64_t i = 0; i < stressItems; ++i) {
        const uint64_t hash = stable_bloom_test_hash(i + 1000000ULL);
        if (stressFilter.possibly_contains_hash(hash)) {
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
        test_trie_tree();
        test_huffman();
        test_mvcc();
        test_bloom_filter();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
