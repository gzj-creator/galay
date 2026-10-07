#include "test_common.hpp"

#include <concepts>
#include <string_view>
#include <utility>

namespace {

template<typename T>
concept HasValueClone = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

template<typename T>
concept HasAnyClone = requires(const T& value) {
    value.clone();
};

template<typename T>
void assert_move_only_value_clone_contract() {
    static_assert(!std::is_copy_constructible_v<T>);
    static_assert(!std::is_copy_assignable_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_move_assignable_v<T>);
    static_assert(HasValueClone<T>);
}

template<typename T>
void assert_move_only_parser_clone_contract() {
    static_assert(!std::is_copy_constructible_v<T>);
    static_assert(!std::is_copy_assignable_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_move_assignable_v<T>);
    static_assert(HasValueClone<T>);
}

void assert_static_contracts() {
    assert_move_only_value_clone_contract<ByteQueueView>();
    assert_move_only_value_clone_contract<BloomFilter<std::string>>();
    assert_move_only_value_clone_contract<HuffmanTable<char>>();
    assert_move_only_value_clone_contract<TrieTree>();
    assert_move_only_value_clone_contract<Bytes>();
    assert_move_only_value_clone_contract<RingBuffer<RingBufferBackendStrategy::Vector, std::dynamic_extent>>();
    assert_move_only_value_clone_contract<RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent>>();

    assert_move_only_parser_clone_contract<ConfigParser>();
    assert_move_only_parser_clone_contract<IniParser>();
    assert_move_only_parser_clone_contract<EnvParser>();
    assert_move_only_parser_clone_contract<TomlParser>();
    static_assert(!HasAnyClone<ParserBase>);

    assert_move_only_value_clone_contract<RoundRobinLoadBalancer<std::string>>();
    assert_move_only_value_clone_contract<WeightRoundRobinLoadBalancer<std::string>>();
    assert_move_only_value_clone_contract<RandomLoadBalancer<std::string>>();
    assert_move_only_value_clone_contract<WeightedRandomLoadBalancer<std::string>>();

    static_assert(!std::is_copy_constructible_v<LruCache<int, int>>);
    static_assert(!std::is_copy_assignable_v<LruCache<int, int>>);
    static_assert(!std::is_move_constructible_v<LruCache<int, int>>);
    static_assert(!std::is_move_assignable_v<LruCache<int, int>>);

    static_assert(!std::is_copy_constructible_v<Mvcc<std::string>>);
    static_assert(!std::is_copy_assignable_v<Mvcc<std::string>>);
    static_assert(!std::is_move_constructible_v<Mvcc<std::string>>);
    static_assert(!std::is_move_assignable_v<Mvcc<std::string>>);
    static_assert(!std::is_copy_constructible_v<Transaction<std::string>>);
    static_assert(!std::is_copy_assignable_v<Transaction<std::string>>);
    static_assert(!std::is_move_constructible_v<Transaction<std::string>>);
    static_assert(!std::is_move_assignable_v<Transaction<std::string>>);
    static_assert(!std::is_copy_constructible_v<VersionedValue<std::string>>);
    static_assert(!std::is_copy_assignable_v<VersionedValue<std::string>>);
    static_assert(std::is_nothrow_move_constructible_v<VersionedValue<std::string>>);
    static_assert(std::is_nothrow_move_assignable_v<VersionedValue<std::string>>);

    static_assert(std::is_copy_constructible_v<ByteMetaData>);
    static_assert(std::is_copy_assignable_v<ByteMetaData>);
    static_assert(std::is_copy_constructible_v<HuffmanCode>);
    static_assert(std::is_copy_assignable_v<HuffmanCode>);
    static_assert(std::is_copy_constructible_v<RingBufferError>);
    static_assert(std::is_copy_constructible_v<CircuitBreakerConfig>);
    static_assert(std::is_copy_constructible_v<NodeConfig>);
}

void test_byte_queue_view_clone_and_move() {
    ByteQueueView queue;
    queue.append("frame:payload", 13);
    queue.consume(6);

    ByteQueueView copy = queue.clone();
    ByteQueueView moved = std::move(queue);

    assert(moved.view(0, moved.size()) == "payload");
    assert(copy.view(0, copy.size()) == "payload");

    moved.append("!", 1);
    assert(moved.view(0, moved.size()) == "payload!");
    assert(copy.view(0, copy.size()) == "payload");
}

void test_bloom_filter_clone_is_independent() {
    auto filter = BloomFilter<std::string>::from_expected_items(64, 0.01);
    filter.add("alpha");
    filter.add("beta");

    auto copy = filter.clone();
    filter.clear();

    assert(copy.insertion_count() == 2);
    assert(copy.possibly_contains("alpha"));
    assert(copy.possibly_contains("beta"));
    assert(!filter.possibly_contains("alpha"));
}

void test_huffman_table_clone_is_independent() {
    HuffmanTable<char> table;
    table.add_code('a', 0b0, 1);
    table.add_code('b', 0b10, 2);

    auto copy = table.clone();
    table.clear();

    assert(copy.size() == 2);
    assert(copy.has_symbol('a'));
    assert(copy.get_symbol(0b10, 2) == 'b');
}

void test_config_parser_clone_is_independent() {
    ConfigParser config;
    assert(config.parse_string("[server]\nport = 8080\n"));

    auto copy = config.clone();
    assert(config.parse_string("[server]\nport = 9090\n"));

    assert(copy.get_value("server.port").value() == "8080");
    assert(config.get_value("server.port").value() == "9090");

    TomlParser toml;
    assert(toml.parse_string("title = \"galay\"\nports = [8000, 8001]\n"));
    auto tomlCopy = toml.clone();
    assert(toml.parse_string("title = \"other\"\nports = [9000]\n"));
    assert(tomlCopy.get_value("title").value() == "galay");
    assert(toml.get_value("title").value() == "other");
}

void test_load_balancer_clone_is_independent() {
    RoundRobinLoadBalancer<std::string> rr({"a", "b"});
    auto first = rr.select();
    assert(first.has_value());
    assert(*first == "a");
    auto rrCopy = rr.clone();
    rr.append("c");
    assert(rr.size() == 3);
    assert(rrCopy.size() == 2);
    auto nextFromCopy = rrCopy.select();
    assert(nextFromCopy.has_value());
    assert(*nextFromCopy == "b");

    WeightRoundRobinLoadBalancer<std::string> weighted({"a", "b"}, {1, 3});
    auto weightedCopy = weighted.clone();
    weighted.append("c", 5);
    assert(weighted.size() == 3);
    assert(weightedCopy.size() == 2);

    RandomLoadBalancer<std::string> random({"a", "b"});
    auto randomCopy = random.clone();
    random.append("c");
    assert(random.size() == 3);
    assert(randomCopy.size() == 2);

    WeightedRandomLoadBalancer<std::string> weightedRandom({"a", "b"}, {1, 3});
    auto weightedRandomCopy = weightedRandom.clone();
    weightedRandom.append("c", 5);
    assert(weightedRandom.size() == 3);
    assert(weightedRandomCopy.size() == 2);
}

void test_trie_bytes_and_ring_buffer_clone() {
    TrieTree trie;
    trie.add("hello");
    trie.add("help");
    auto trieCopy = trie.clone();
    trie.remove("hello");
    assert(!trie.contains("hello"));
    assert(trieCopy.contains("hello"));
    assert(trieCopy.contains("help"));

    std::string source = "view";
    Bytes view = Bytes::from_string(source);
    Bytes bytesCopy = view.clone();
    source[0] = 'V';
    assert(view.to_string_view() == "View");
    assert(bytesCopy.to_string_view() == "view");

    RingBuffer<RingBufferBackendStrategy::Vector, std::dynamic_extent> buffer(5);
    assert(buffer.try_write_batch("abcde", 5) == 5);
    char skipped[3]{};
    assert(buffer.try_read_batch(skipped, sizeof(skipped)) == 3);
    assert(std::string_view(skipped, sizeof(skipped)) == "abc");
    assert(buffer.try_write_batch("fg", 2) == 2);

    auto bufferCopy = buffer.clone();
    buffer.consume(2);
    assert(buffer.try_write_batch("hi", 2) == 2);

    char copied[4]{};
    assert(bufferCopy.try_read_batch(copied, sizeof(copied)) == 4);
    assert(std::string_view(copied, sizeof(copied)) == "defg");

    RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> defaultBuffer(8);
    assert(defaultBuffer.try_write_batch("abcd", 4) == 4);
    auto defaultCopy = defaultBuffer.clone();
    defaultBuffer.clear();
    char defaultCopied[4]{};
    assert(defaultCopy.try_read_batch(defaultCopied, sizeof(defaultCopied)) == 4);
    assert(std::string_view(defaultCopied, sizeof(defaultCopied)) == "abcd");
}

} // namespace

int main() {
    try {
        assert_static_contracts();
        test_byte_queue_view_clone_and_move();
        test_bloom_filter_clone_is_independent();
        test_huffman_table_clone_is_independent();
        test_config_parser_clone_is_independent();
        test_load_balancer_clone_is_independent();
        test_trie_bytes_and_ring_buffer_clone();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
