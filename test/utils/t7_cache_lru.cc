#include "test_common.hpp"

#include <filesystem>

void testCacheHeadersMovedToCache() {
    const auto sourceRoot = std::filesystem::path(GALAY_UTILS_SOURCE_DIR);
    assert(!std::filesystem::exists(sourceRoot / "utils/tool/lru_cache.hpp"));
    assert(std::filesystem::exists(sourceRoot / "utils/cache/lru_cache.hpp"));
}

void testLruCache() {
    std::cout << "=== Testing LruCache ===" << std::endl;

    static_assert(!std::is_copy_constructible_v<LruCache<int, int>>);
    static_assert(!std::is_copy_assignable_v<LruCache<int, int>>);
    static_assert(!std::is_move_constructible_v<LruCache<int, int>>);
    static_assert(!std::is_move_assignable_v<LruCache<int, int>>);

    {
        using namespace std::chrono_literals;

        LruCache<int, int> cache(2, 1h);

        cache.put(1, 10);
        assert(cache.get(1) != nullptr && *cache.get(1) == 10);
    }

    {
        LruCache<int, std::string> cache(2);

        cache.put(1, "one");
        cache.put(2, "two");

        auto* one = cache.get(1);
        assert(one != nullptr && *one == "one");

        cache.put(3, "three");

        assert(cache.get(2) == nullptr);
        assert(cache.get(1) != nullptr && *cache.get(1) == "one");
        assert(cache.get(3) != nullptr && *cache.get(3) == "three");
        assert(cache.size() == 2);
    }

    {
        LruCache<int, std::string> cache(2);

        cache.put(1, "one");
        cache.put(2, "two");
        cache.put(1, "uno");
        cache.put(3, "three");

        assert(cache.get(2) == nullptr);
        assert(cache.get(1) != nullptr && *cache.get(1) == "uno");
        assert(cache.get(3) != nullptr && *cache.get(3) == "three");
    }

    {
        LruCache<int, std::string> cache(2);

        cache.put(1, "one");
        cache.put(2, "two");
        assert(cache.peek(1) != nullptr && *cache.peek(1) == "one");

        cache.put(3, "three");

        assert(cache.get(1) == nullptr);
        assert(cache.get(2) != nullptr && *cache.get(2) == "two");
        assert(cache.get(3) != nullptr && *cache.get(3) == "three");
    }

    {
        LruCache<int, std::string> cache(2);

        cache.put(1, "one");
        cache.put(2, "two");
        assert(cache.contains(1));

        cache.put(3, "three");

        assert(cache.get(1) == nullptr);
        assert(cache.get(2) != nullptr && *cache.get(2) == "two");
        assert(cache.get(3) != nullptr && *cache.get(3) == "three");
    }

    {
        LruCache<int, std::string> cache(2);

        cache.emplace(1, 3, 'a');
        cache.emplace(2, 3, 'b');
        cache.emplace(1, 4, 'c');
        cache.put(3, "ddd");

        assert(cache.get(2) == nullptr);
        assert(cache.get(1) != nullptr && *cache.get(1) == "cccc");
        assert(cache.get(3) != nullptr && *cache.get(3) == "ddd");
    }

    {
        using Cache = LruCache<std::string, int, std::hash<std::string>,
                               std::equal_to<std::string>, ManualClock>;

        ManualClock::reset();
        Cache cache(10, ManualClock::duration{50});

        cache.put("alpha", 1);
        ManualClock::advance(ManualClock::duration{30});
        assert(cache.get("alpha") != nullptr && *cache.get("alpha") == 1);

        ManualClock::advance(ManualClock::duration{21});
        assert(cache.get("alpha") == nullptr);
        assert(cache.empty());
    }

    {
        using Cache = LruCache<std::string, int, std::hash<std::string>,
                               std::equal_to<std::string>, ManualClock>;

        ManualClock::reset();
        Cache cache(10);

        cache.putFor("alpha", 1, ManualClock::duration{50});
        ManualClock::advance(ManualClock::duration{25});
        cache.putFor("alpha", 2, ManualClock::duration{100});

        ManualClock::advance(ManualClock::duration{30});
        assert(cache.get("alpha") != nullptr && *cache.get("alpha") == 2);

        ManualClock::advance(ManualClock::duration{80});
        assert(cache.get("alpha") == nullptr);
    }

    {
        using Cache = LruCache<std::string, int, std::hash<std::string>,
                               std::equal_to<std::string>, ManualClock>;

        ManualClock::reset();
        Cache cache(10);

        assert(!cache.putFor("zero", 1, ManualClock::duration{0}));
        assert(!cache.putFor("negative", 2, ManualClock::duration{-1}));
        assert(cache.empty());

        assert(cache.putUntil("future", 3, ManualClock::now() + ManualClock::duration{10}));
        assert(!cache.putUntil("past", 4, ManualClock::now()));
        assert(cache.get("future") != nullptr && *cache.get("future") == 3);
        assert(cache.get("past") == nullptr);
    }

    {
        using Cache = LruCache<std::string, int, std::hash<std::string>,
                               std::equal_to<std::string>, ManualClock>;

        ManualClock::reset();
        Cache cache(1);

        cache.putFor("alpha", 1, ManualClock::duration{10});
        cache.put("beta", 2);
        ManualClock::advance(ManualClock::duration{11});
        cache.put("alpha", 3);

        assert(cache.get("alpha") != nullptr && *cache.get("alpha") == 3);
        assert(cache.get("beta") == nullptr);
    }

    {
        using Cache = LruCache<std::string, int, std::hash<std::string>,
                               std::equal_to<std::string>, ManualClock>;

        ManualClock::reset();
        Cache cache(10, ManualClock::duration{10});

        cache.put("old", 1);
        cache.setDefaultTtl(ManualClock::duration{100});
        cache.put("new", 2);

        ManualClock::advance(ManualClock::duration{11});
        assert(cache.get("old") == nullptr);
        assert(cache.get("new") != nullptr && *cache.get("new") == 2);
        assert(cache.defaultTtl().has_value());

        cache.setDefaultTtl(std::nullopt);
        cache.put("forever", 3);
        ManualClock::advance(ManualClock::duration{200});
        assert(cache.get("forever") != nullptr && *cache.get("forever") == 3);
    }

    {
        using Cache = LruCache<int, std::string>;

        std::vector<Cache::EvictReason> reasons;
        Cache cache(1, std::nullopt, [&](const int&, const std::string&, Cache::EvictReason reason) {
            reasons.push_back(reason);
        });

        cache.put(1, "one");
        cache.put(2, "two");

        assert(reasons.size() == 1);
        assert(reasons[0] == Cache::EvictReason::Capacity);
    }

    {
        using Cache = LruCache<int, std::string>;

        std::vector<Cache::EvictReason> reasons;
        std::vector<std::string> values;
        Cache cache(3, std::nullopt, [&](const int&, const std::string& value, Cache::EvictReason reason) {
            values.push_back(value);
            reasons.push_back(reason);
        });

        cache.put(1, "one");
        cache.put(2, "two");
        assert(cache.remove(1));
        assert(!cache.remove(1));

        cache.clear();
        cache.clear();

        assert(reasons.size() == 2);
        assert(values[0] == "one" && reasons[0] == Cache::EvictReason::Removed);
        assert(values[1] == "two" && reasons[1] == Cache::EvictReason::Cleared);
        assert(cache.empty());
    }

    {
        using Cache = LruCache<std::string, int, std::hash<std::string>,
                               std::equal_to<std::string>, ManualClock>;

        std::vector<Cache::EvictReason> reasons;
        ManualClock::reset();
        Cache cache(2, std::nullopt, [&](const std::string&, const int&, Cache::EvictReason reason) {
            reasons.push_back(reason);
        });

        cache.putFor("alpha", 1, ManualClock::duration{10});
        ManualClock::advance(ManualClock::duration{11});

        assert(cache.size() == 0);
        assert(reasons.size() == 1 && reasons[0] == Cache::EvictReason::Expired);
    }

    {
        LruCache<int, std::string> cache(0);

        cache.put(1, "one");

        assert(cache.empty());
        assert(cache.get(1) == nullptr);
    }

    {
        LruCache<int, std::string> cache(3);

        cache.put(1, "one");
        cache.put(2, "two");
        cache.put(3, "three");

        cache.setCapacity(2);
        assert(cache.get(1) == nullptr);
        assert(cache.get(2) != nullptr);
        assert(cache.get(3) != nullptr);

        cache.setCapacity(4);
        cache.put(4, "four");
        cache.put(5, "five");
        assert(cache.size() == 4);
    }

    {
        using Cache = LruCache<int, std::string, std::hash<int>,
                               std::equal_to<int>, CountingClock>;

        CountingClock::reset();
        Cache cache(2);

        cache.put(1, "one");
        cache.put(2, "two");
        assert(cache.get(1) != nullptr);
        assert(cache.contains(2));
        assert(cache.peek(1) != nullptr);
        assert(cache.size() == 2);

        cache.put(3, "three");
        cache.setCapacity(1);
        assert(cache.get(3) != nullptr);
        assert(CountingClock::nowCalls == 0);
    }

    {
        LruCache<int, std::unique_ptr<int>> cache(2);

        cache.put(1, std::make_unique<int>(10));
        cache.emplace(2, std::make_unique<int>(20));

        auto* value = cache.get(1);
        assert(value != nullptr && *value != nullptr && **value == 10);
        cache.put(1, std::make_unique<int>(30));
        value = cache.get(1);
        assert(value != nullptr && *value != nullptr && **value == 30);
    }

    {
        LruCache<std::string, int, CaseInsensitiveHash, CaseInsensitiveEqual> cache(2);

        cache.put("Alpha", 1);
        cache.put("beta", 2);

        assert(cache.get("ALPHA") != nullptr && *cache.get("ALPHA") == 1);
        cache.put("Gamma", 3);

        assert(cache.get("beta") == nullptr);
        assert(cache.get("alpha") != nullptr && *cache.get("alpha") == 1);
        assert(cache.get("gamma") != nullptr && *cache.get("gamma") == 3);
    }

    {
        LruCache<int, std::string> cache(2);
        const auto& constCache = cache;

        cache.put(1, "one");
        cache.put(2, "two");

        assert(constCache.get(1) != nullptr && *constCache.get(1) == "one");
        cache.put(3, "three");

        assert(cache.get(2) == nullptr);
        assert(cache.get(1) != nullptr);
        assert(cache.get(3) != nullptr);
    }

    {
        using Cache = LruCache<int, std::string>;

        Cache cache(2);
        cache.put(1, "one");
        assert(cache.get(1) != nullptr);
        assert(cache.get(9) == nullptr);
        auto disabledStats = cache.stats();
        assert(disabledStats.hits == 0);
        assert(disabledStats.misses == 0);
        assert(!Cache::statsEnabled());
    }

    {
        using Cache = LruCache<int, std::string, std::hash<int>,
                               std::equal_to<int>, std::chrono::steady_clock, true>;

        static_assert(Cache::statsEnabled());
        Cache cache(2);
        cache.reserve(16);
        cache.maxLoadFactor(0.7f);
        assert(cache.maxLoadFactor() <= 0.71f);

        cache.put(1, "one");
        cache.put(2, "two");
        assert(cache.get(1) != nullptr);
        assert(cache.get(9) == nullptr);
        cache.put(1, "uno");
        cache.put(3, "three");
        assert(cache.remove(1));
        cache.clear();

        auto stats = cache.stats();
        assert(stats.hits == 1);
        assert(stats.misses == 1);
        assert(stats.inserts == 3);
        assert(stats.updates == 1);
        assert(stats.capacityEvictions == 1);
        assert(stats.expiredEvictions == 0);
        assert(stats.removes == 1);
        assert(stats.clears == 1);

        cache.resetStats();
        stats = cache.stats();
        assert(stats.hits == 0);
        assert(stats.misses == 0);
        assert(stats.inserts == 0);
        assert(stats.updates == 0);
        assert(stats.capacityEvictions == 0);
        assert(stats.expiredEvictions == 0);
        assert(stats.removes == 0);
        assert(stats.clears == 0);
    }

    {
        using Cache = LruCache<std::string, int, std::hash<std::string>,
                               std::equal_to<std::string>, ManualClock, true>;

        ManualClock::reset();
        Cache cache(3);

        cache.putFor("alpha", 1, ManualClock::duration{10});
        cache.putFor("beta", 2, ManualClock::duration{20});
        ManualClock::advance(ManualClock::duration{11});

        assert(cache.purgeExpired() == 1);
        assert(cache.size() == 1);

        auto stats = cache.stats();
        assert(stats.expiredEvictions == 1);
        assert(stats.misses == 0);
    }

    {
        using Cache = LruCache<std::string, int, std::hash<std::string>,
                               std::equal_to<std::string>, ManualClock>;

        ManualClock::reset();
        Cache cache(2, ManualClock::duration{10}, nullptr,
                    Cache::ExpirationPolicy::ExpireAfterAccess);

        cache.put("alpha", 1);
        ManualClock::advance(ManualClock::duration{7});
        assert(cache.get("alpha") != nullptr && *cache.get("alpha") == 1);

        ManualClock::advance(ManualClock::duration{7});
        assert(cache.get("alpha") != nullptr && *cache.get("alpha") == 1);

        ManualClock::advance(ManualClock::duration{11});
        assert(cache.get("alpha") == nullptr);
    }

    std::cout << "LruCache tests passed!" << std::endl;
}

// ==================== Stress Tests ====================

int main() {
    std::cout << "\n=== cache_test ===" << std::endl;
    try {
        testCacheHeadersMovedToCache();
        testLruCache();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
