#include <galay/cpp/galay-postgres/async/conn_pool.h>

#include <type_traits>

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
    return lease.dismiss() == nullptr ? 0 : 1;
}
