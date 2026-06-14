import galay.utils;

#include <utils/tool/rate_limiter.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>

int main() {
    using namespace galay::utils;

    auto parts = StringUtils::split("a,b,c", ',');
    assert(parts.size() == 3);
    assert(parts[1] == "b");
    assert(StringUtils::join(parts, "-") == "a-b-c");
    RandomGenerator random(7);
    assert(random.randomString(4, "a") == "aaaa");
    assert(Time::formatTime(0, "%Y", true) == "1970");
    assert(System::cpuCount() > 0);

    LruCache<int, int> cache(1);
    assert(cache.put(1, 10));
    assert(cache.get(1) != nullptr);

    ByteQueueView queue;
    queue.append("ab", 2);
    assert(queue.view(0, 2) == "ab");

    Bytes bytes("mod", 3);
    assert(bytes.toStringView() == "mod");

    RingBuffer ring(4);
    assert(ring.write("xy", 2) == 2);
    assert(ring.readable() == 2);

    ObjectPool<std::string> pool(1);
    auto pooled = pool.acquire();
    assert(pooled != nullptr);

    CountingSemaphore semaphore(1);
    assert(semaphore.tryAcquire());
    semaphore.release();

    CircuitBreaker breaker;
    assert(breaker.allowRequest());

    RoundRobinLoadBalancer<int> balancer({1, 2});
    assert(balancer.select().has_value());

    ConsistentHash hash;
    hash.addNode(NodeConfig{"node-a", "127.0.0.1:1", 1});
    assert(hash.nodeCount() == 1);

    BloomFilter<std::string> bloom(256);
    assert(!bloom.possiblyContains("abc"));
    bloom.add("abc");
    assert(bloom.possiblyContains("abc"));

    TrieTree trie;
    trie.add("abc");
    assert(trie.contains("abc"));

    Mvcc<int> mvcc;
    mvcc.putValue(42);
    assert(mvcc.getCurrentValue() != nullptr);

    HuffmanTable<char> huffman;
    huffman.addCode('a', 0, 1);
    assert(huffman.hasSymbol('a'));

    ArgValue value(std::string("7"));
    assert(value.as<int>() == 7);

    auto parser = ParserManager::instance().createParser("config.ini");
    assert(parser != nullptr);

    assert(Base64Util::Base64Encode("x") == "eA==");
    assert(MD5Util::MD5("x").size() == 32);
    assert(HMAC::hmacSha256Hex("k", "v").size() == 64);

    std::cout << "Module import smoke test passed." << std::endl;
    return 0;
}
