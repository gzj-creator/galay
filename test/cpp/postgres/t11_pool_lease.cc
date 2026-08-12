#include <galay/cpp/galay-postgres/async/conn_pool.h>

#include <type_traits>
#include <utility>

using namespace galay::postgres;

static_assert(!std::is_copy_constructible_v<PostgresPoolLease>);
static_assert(!std::is_copy_assignable_v<PostgresPoolLease>);
static_assert(std::is_nothrow_move_constructible_v<PostgresPoolLease>);
static_assert(std::is_nothrow_move_assignable_v<PostgresPoolLease>);

int main()
{
    PostgresPoolLease lease;
    if (lease || lease.get() != nullptr) {
        return 1;
    }
    lease.release();
    if (lease.dismiss() != nullptr) {
        return 2;
    }

    PostgresConnectionPoolConfig zero_config;
    zero_config.min_connections = 7;
    zero_config.max_connections = 0;
    PostgresConnectionPool zero_pool(nullptr, std::move(zero_config));
    if (zero_pool.size() != 0 || zero_pool.idleCount() != 0) {
        return 3;
    }
    zero_pool.release(nullptr);

    PostgresConnectionPoolConfig clamped_config;
    clamped_config.min_connections = 3;
    clamped_config.max_connections = 1;
    PostgresConnectionPool clamped_pool(nullptr, std::move(clamped_config));
    return clamped_pool.size() == 1 && clamped_pool.idleCount() == 0 ? 0 : 4;
}
