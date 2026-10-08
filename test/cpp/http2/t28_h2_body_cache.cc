#include <galay/cpp/galay-http2/server/h2_static_file.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using galay::http2::H2StaticFileBodyCacheSlot;

void test_single_publication()
{
    auto slot = std::make_shared<H2StaticFileBodyCacheSlot>();
    assert(!slot->load());
    assert(!slot->store_if_empty(nullptr));
    auto body = std::make_shared<const std::string>("cached body");
    const std::weak_ptr<const std::string> lifetime = body;
    assert(slot->store_if_empty(body));
    assert(!slot->store_if_empty(std::make_shared<const std::string>("replacement")));
    auto snapshot = slot->load();
    assert(snapshot == body);
    body.reset();
    slot.reset();
    assert(!lifetime.expired() && *snapshot == "cached body");
    snapshot.reset();
    assert(lifetime.expired());

    H2StaticFileBodyCacheSlot empty;
    const auto empty_body = std::make_shared<const std::string>();
    assert(empty.store_if_empty(empty_body));
    assert(empty.load() == empty_body);
}

void test_concurrent_publication()
{
    constexpr int writer_count = 16;
    H2StaticFileBodyCacheSlot slot;
    std::vector<std::shared_ptr<const std::string>> bodies;
    for (int index = 0; index < writer_count; ++index) {
        bodies.push_back(std::make_shared<const std::string>(32768, static_cast<char>('a' + index)));
    }
    std::atomic<bool> ready{false};
    std::atomic<int> completed{0};
    std::atomic<int> publications{0};
    std::atomic<int> winner{-1};
    std::vector<std::thread> threads;
    for (int index = 0; index < writer_count; ++index) {
        threads.emplace_back([&, index] {
            while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
            if (slot.store_if_empty(bodies[index])) {
                const int previous = publications.fetch_add(1);
                assert(previous == 0);
                winner.store(index);
            }
            const int previous = completed.fetch_add(1, std::memory_order_release);
            assert(previous < writer_count);
        });
    }
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&] {
            while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
            do {
                const auto body = slot.load();
                if (!body) continue;
                assert(body->size() == 32768);
                const int owner = body->front() - 'a';
                assert(owner >= 0 && owner < writer_count && body == bodies[owner]);
                assert(*body == std::string(32768, static_cast<char>('a' + owner)));
            } while (completed.load(std::memory_order_acquire) != writer_count);
        });
    }
    ready.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    assert(publications.load() == 1 && winner.load() >= 0);
    assert(slot.load() == bodies[winner.load()]);
}

int main()
{
    test_single_publication();
    for (int iteration = 0; iteration < 20; ++iteration) test_concurrent_publication();
}
