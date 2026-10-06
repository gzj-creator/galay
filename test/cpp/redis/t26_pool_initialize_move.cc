#include <galay/cpp/galay-redis/async/conn_pool.h>

#include <cassert>
#include <chrono>
#include <concepts>
#include <type_traits>
#include <utility>

using namespace galay::redis;
using namespace std::chrono_literals;

static_assert(std::movable<PoolInitializeAwaitable>);
static_assert(!std::is_copy_constructible_v<PoolInitializeAwaitable>);

#ifdef GALAY_SSL_FEATURE_ENABLED
static_assert(std::movable<RedissPoolInitializeAwaitable>);
static_assert(!std::is_copy_constructible_v<RedissPoolInitializeAwaitable>);
#endif

int main()
{
    auto config = ConnectionPoolConfig::create("127.0.0.1", 1, 0, 1);
    RedisConnectionPool pool(nullptr, config);
    auto first = pool.initialize();
    auto moved = std::move(first).timeout(1ms);
    assert(moved.await_ready());
    assert(moved.await_resume().has_value());
    assert(pool.initialize().await_resume().has_value());
    pool.shutdown();
    assert(!pool.initialize().await_resume().has_value());

#ifdef GALAY_SSL_FEATURE_ENABLED
    auto tls_config = RedissConnectionPoolConfig::create("127.0.0.1", 1, 0, 1);
    RedissConnectionPool tls_pool(nullptr, tls_config);
    auto tls_first = tls_pool.initialize();
    auto tls_moved = std::move(tls_first).timeout(1ms);
    assert(tls_moved.await_ready());
    assert(tls_moved.await_resume().has_value());
    assert(tls_pool.initialize().await_resume().has_value());
    tls_pool.shutdown();
    assert(!tls_pool.initialize().await_resume().has_value());
#endif
}
