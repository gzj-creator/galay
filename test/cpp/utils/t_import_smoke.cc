import galay.utils;

#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

int main() {
    using namespace galay::utils;

    const auto pageSize = Memory::page_size();
    if (!pageSize || *pageSize == 0) {
        return 1;
    }
    static_assert(Numa::kMaxNodes > 0);
    static_assert(Memory::kMaxNodes > 0);

    const auto invalidEnv = Env::get("");
    if (invalidEnv || invalidEnv.error() != std::errc::invalid_argument) {
        std::cerr << "Env should be exported and reject invalid names\n";
        return 1;
    }

    auto parts = StringUtils::split("a,b,c", ',');
    assert(parts.size() == 3);
    assert(parts[1] == "b");
    assert(StringUtils::join(parts, "-") == "a-b-c");
    RandomGenerator random(7);
    assert(random.random_string(4, "a") == "aaaa");
    assert(Time::format_time(0, "%Y", true) == "1970");
    if (CPU::count() != std::thread::hardware_concurrency()) {
        return 1;
    }

    LruCache<int, int> cache(1);
    assert(cache.put(1, 10));
    assert(cache.get(1) != nullptr);

    ByteQueueView queue;
    queue.append("ab", 2);
    assert(queue.view(0, 2) == "ab");

    Bytes bytes("mod", 3);
    assert(bytes.to_string_view() == "mod");

    RingBuffer ring(4);
    assert(ring.try_write_batch("xy", 2) == 2);
    assert(ring.readable() == 2);

    TypeRingBuffer<int> spscRing(2);
    int spscValue = 9;
    assert(spscRing.error() == TypeRingBufferError::kNone);
    assert(spscRing.try_write(std::move(spscValue)));
    auto spscReceived = spscRing.try_read();
    assert(spscReceived.has_value() && *spscReceived == 9);

    ObjectPool<std::string> pool(1);
    auto pooled = pool.acquire();
    assert(pooled != nullptr);

    CountingSemaphore semaphore(1);
    assert(semaphore.try_acquire());
    semaphore.release();

    CircuitBreaker breaker;
    assert(breaker.allow_request());

    RoundRobinLoadBalancer<int> balancer({1, 2});
    assert(balancer.select().has_value());

    ConsistentHash hash;
    hash.add_node(NodeConfig{"node-a", "127.0.0.1:1", 1});
    assert(hash.node_count() == 1);

    BloomFilter<std::string> bloom(256);
    assert(!bloom.possibly_contains("abc"));
    bloom.add("abc");
    assert(bloom.possibly_contains("abc"));

    TrieTree trie;
    trie.add("abc");
    assert(trie.contains("abc"));

    Mvcc<int> mvcc;
    mvcc.put_value(42);
    assert(mvcc.get_current_value() != nullptr);

    HuffmanTable<char> huffman;
    huffman.add_code('a', 0, 1);
    assert(huffman.has_symbol('a'));

    App cliApp("smoke");
    auto& cliCount = cliApp.opt<int>("count", 'c', "count").def(1);
    const char* cliArgv[] = {"smoke", "-c", "7"};
    assert(cliApp.run(3, cliArgv) == 0);
    assert(cliCount.value() == 7);

    auto parser = ParserManager::instance().create_parser("config.ini");
    assert(parser != nullptr);

    assert(Base64Util::base64_encode("x") == "eA==");
    assert(MD5Util::md5("x").size() == 32);
    assert(HMAC::hmac_sha256_hex("k", "v").size() == 64);

    std::cout << "Module import smoke test passed." << std::endl;
    return 0;
}
