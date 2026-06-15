#include "galay-utils/cache/lru_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <list>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Operation {
    bool put;
    int key;
    int value;
};

struct Result {
    std::string name;
    std::uint64_t checksum;
    double nsPerOp;
    double mopsPerSec;
};

volatile std::uint64_t g_sink = 0;

class LeetCodeListLru {
public:
    explicit LeetCodeListLru(int capacity)
        : m_capacity(capacity) {}

    int get(int key) {
        auto it = m_index.find(key);
        if (it == m_index.end()) {
            return -1;
        }

        m_items.splice(m_items.begin(), m_items, it->second);
        return it->second->second;
    }

    void put(int key, int value) {
        auto it = m_index.find(key);
        if (it != m_index.end()) {
            it->second->second = value;
            m_items.splice(m_items.begin(), m_items, it->second);
            return;
        }

        if (static_cast<int>(m_items.size()) == m_capacity) {
            m_index.erase(m_items.back().first);
            m_items.pop_back();
        }

        m_items.push_front({key, value});
        m_index[key] = m_items.begin();
    }

private:
    int m_capacity;
    std::list<std::pair<int, int>> m_items;
    std::unordered_map<int, std::list<std::pair<int, int>>::iterator> m_index;
};

class LeetCodeArrayLru {
public:
    explicit LeetCodeArrayLru(int capacity, int maxKey)
        : m_capacity(capacity)
        , m_nodes(static_cast<std::size_t>(maxKey) + 1) {
        m_head.next = &m_tail;
        m_tail.prev = &m_head;
    }

    int get(int key) {
        auto& node = m_nodes[static_cast<std::size_t>(key)];
        if (!node.used) {
            return -1;
        }

        moveToFront(&node);
        return node.value;
    }

    void put(int key, int value) {
        auto& node = m_nodes[static_cast<std::size_t>(key)];
        if (node.used) {
            node.value = value;
            moveToFront(&node);
            return;
        }

        if (m_size == m_capacity) {
            evictLru();
        } else {
            ++m_size;
        }

        node.used = true;
        node.key = key;
        node.value = value;
        addToFront(&node);
    }

private:
    struct Node {
        int key = -1;
        int value = 0;
        bool used = false;
        Node* prev = nullptr;
        Node* next = nullptr;
    };

    void remove(Node* node) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
        node->prev = nullptr;
        node->next = nullptr;
    }

    void addToFront(Node* node) {
        node->prev = &m_head;
        node->next = m_head.next;
        m_head.next->prev = node;
        m_head.next = node;
    }

    void moveToFront(Node* node) {
        if (node->prev == &m_head) {
            return;
        }
        remove(node);
        addToFront(node);
    }

    void evictLru() {
        auto* node = m_tail.prev;
        remove(node);
        node->used = false;
        node->key = -1;
    }

    int m_capacity;
    int m_size = 0;
    std::vector<Node> m_nodes;
    Node m_head;
    Node m_tail;
};

std::vector<Operation> makeOperations(std::size_t count,
                                      int keySpace,
                                      int putPercent,
                                      std::uint32_t seed) {
    std::vector<Operation> ops;
    ops.reserve(count);

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> keyDist(0, keySpace - 1);
    std::uniform_int_distribution<int> opDist(0, 99);

    for (std::size_t i = 0; i < count; ++i) {
        const int key = keyDist(rng);
        ops.push_back(Operation{
            opDist(rng) < putPercent,
            key,
            static_cast<int>((i * 2654435761u) ^ static_cast<std::uint32_t>(key))
        });
    }

    return ops;
}

template<typename Fn>
Result measure(std::string name, const std::vector<Operation>& ops, Fn&& fn) {
    std::uint64_t checksum = 0;

    const auto start = std::chrono::steady_clock::now();
    for (const auto& op : ops) {
        checksum += static_cast<std::uint64_t>(fn(op));
    }
    const auto end = std::chrono::steady_clock::now();

    g_sink = checksum;
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double nsPerOp = static_cast<double>(elapsedNs) / static_cast<double>(ops.size());
    const double mopsPerSec = 1000.0 / nsPerOp;

    return Result{std::move(name), checksum, nsPerOp, mopsPerSec};
}

void printResult(const Result& baseline, const Result& result) {
    const double relative = result.nsPerOp / baseline.nsPerOp;
    std::cout << std::left << std::setw(28) << result.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << result.nsPerOp
              << std::setw(12) << std::fixed << std::setprecision(2) << result.mopsPerSec
              << std::setw(12) << std::fixed << std::setprecision(2) << relative << "x"
              << "  checksum=" << result.checksum << '\n';
}

template<typename BuildFn>
Result bestOf(std::string name,
              int repeats,
              const std::vector<Operation>& ops,
              BuildFn&& build) {
    Result best{name, 0, std::numeric_limits<double>::max(), 0.0};

    for (int i = 0; i < repeats; ++i) {
        auto result = build(name, ops);
        if (result.nsPerOp < best.nsPerOp) {
            best = std::move(result);
        }
    }

    return best;
}

void runScenario(const std::string& scenario,
                 const std::vector<Operation>& ops,
                 int capacity,
                 int maxKey) {
    constexpr int repeats = 5;

    std::cout << "\nScenario: " << scenario << '\n';
    std::cout << "Operations: " << ops.size() << ", capacity: " << capacity
              << ", maxKey: " << maxKey << '\n';
    std::cout << std::left << std::setw(28) << "Implementation"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(12) << "Mops/s"
              << std::setw(13) << "vs fastest" << '\n';

    auto fastest = bestOf("LeetCode array-index LRU", repeats, ops,
        [capacity, maxKey](std::string name, const std::vector<Operation>& scenarioOps) {
            LeetCodeArrayLru cache(capacity, maxKey);
            for (int key = 0; key < capacity; ++key) {
                cache.put(key, key);
            }

            return measure(std::move(name), scenarioOps, [&cache](const Operation& op) {
                if (op.put) {
                    cache.put(op.key, op.value);
                    return op.value;
                }

                return cache.get(op.key);
            });
        });

    auto stl = bestOf("LeetCode list+hash LRU", repeats, ops,
        [capacity](std::string name, const std::vector<Operation>& scenarioOps) {
            LeetCodeListLru cache(capacity);
            for (int key = 0; key < capacity; ++key) {
                cache.put(key, key);
            }

            return measure(std::move(name), scenarioOps, [&cache](const Operation& op) {
                if (op.put) {
                    cache.put(op.key, op.value);
                    return op.value;
                }

                return cache.get(op.key);
            });
        });

    auto galayCapacity = bestOf("galay LruCache capacity", repeats, ops,
        [capacity](std::string name, const std::vector<Operation>& scenarioOps) {
            galay::utils::LruCache<int, int> cache(static_cast<std::size_t>(capacity));
            for (int key = 0; key < capacity; ++key) {
                cache.put(key, key);
            }

            return measure(std::move(name), scenarioOps, [&cache](const Operation& op) {
                if (op.put) {
                    cache.put(op.key, op.value);
                    return op.value;
                }

                auto* value = cache.get(op.key);
                return value ? *value : -1;
            });
        });

    auto galayCapacityStats = bestOf("galay LruCache stats on", repeats, ops,
        [capacity](std::string name, const std::vector<Operation>& scenarioOps) {
            using Cache = galay::utils::LruCache<int,
                                                 int,
                                                 std::hash<int>,
                                                 std::equal_to<int>,
                                                 std::chrono::steady_clock,
                                                 true>;
            Cache cache(static_cast<std::size_t>(capacity));
            for (int key = 0; key < capacity; ++key) {
                cache.put(key, key);
            }

            auto result = measure(std::move(name), scenarioOps, [&cache](const Operation& op) {
                if (op.put) {
                    cache.put(op.key, op.value);
                    return op.value;
                }

                auto* value = cache.get(op.key);
                return value ? *value : -1;
            });

            const auto stats = cache.stats();
            result.checksum += stats.hits + stats.misses + stats.inserts +
                               stats.updates + stats.capacityEvictions;
            g_sink = result.checksum;
            return result;
        });

    auto galayTtl = bestOf("galay LruCache TTL", repeats, ops,
        [capacity](std::string name, const std::vector<Operation>& scenarioOps) {
            using namespace std::chrono_literals;
            using Cache = galay::utils::LruCache<int, int>;
            Cache cache(static_cast<std::size_t>(capacity),
                        std::chrono::duration_cast<Cache::duration>(24h));
            for (int key = 0; key < capacity; ++key) {
                cache.put(key, key);
            }

            return measure(std::move(name), scenarioOps, [&cache](const Operation& op) {
                if (op.put) {
                    cache.put(op.key, op.value);
                    return op.value;
                }

                auto* value = cache.get(op.key);
                return value ? *value : -1;
            });
        });

    printResult(fastest, fastest);
    printResult(fastest, stl);
    printResult(fastest, galayCapacity);
    printResult(fastest, galayCapacityStats);
    printResult(fastest, galayTtl);
}

} // namespace

int main() {
    constexpr std::size_t opCount = 5'000'000;
    constexpr int capacity = 8192;

    const auto readHeavy = makeOperations(opCount, capacity, 10, 0xC0FFEE);
    const auto mixedEviction = makeOperations(opCount, capacity * 8, 50, 0xBADC0DE);
    const auto writeHeavy = makeOperations(opCount, capacity * 8, 90, 0xDEADBEEF);

    std::cout << "LRU cache benchmark\n";
    std::cout << "Build with -O3 -DNDEBUG. Best result of 5 runs per implementation.\n";
    std::cout << "LeetCode array-index LRU is a specialized int-key baseline using fixed key range.\n";

    runScenario("read-heavy hot set (10% put / 90% get)", readHeavy, capacity, capacity - 1);
    runScenario("mixed eviction (50% put / 50% get)", mixedEviction, capacity, capacity * 8 - 1);
    runScenario("write-heavy eviction (90% put / 10% get)", writeHeavy, capacity, capacity * 8 - 1);

    return static_cast<int>(g_sink == 0xFFFFFFFFFFFFFFFFull);
}
