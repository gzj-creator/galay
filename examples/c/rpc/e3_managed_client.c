#include <galay/c/galay-rpc-c/rpc.h>

#include <stdio.h>

int main(void)
{
    galay_rpc_pool_config_t config = galay_rpc_pool_config_default();
    galay_rpc_pool_t* pool = NULL;
    galay_rpc_pool_lease_t* lease = NULL;
    uint64_t lease_id = 0;
    size_t available = 0;
    int exit_code = 0;
    config.min_connections_per_endpoint = 1;
    config.max_connections_per_endpoint = 2;

    if (galay_rpc_pool_create(&config, &pool) != GALAY_OK ||
        galay_rpc_pool_ensure_endpoint(pool, "127.0.0.1", 9000) != GALAY_OK ||
        galay_rpc_pool_available_count(pool, "127.0.0.1", 9000, &available) != GALAY_OK ||
        available != 1 ||
        galay_rpc_pool_acquire(pool, "127.0.0.1", 9000, &lease) != GALAY_OK ||
        galay_rpc_pool_lease_id(lease, &lease_id) != GALAY_OK ||
        lease_id == 0 ||
        galay_rpc_pool_release(pool, lease, GALAY_FALSE) != GALAY_OK ||
        galay_rpc_pool_shutdown(pool) != GALAY_OK) {
        exit_code = 1;
    }

    if (exit_code == 0) {
        printf("rpc managed client pool lease id=%llu\n", (unsigned long long)lease_id);
    }
    galay_rpc_pool_destroy(pool);
    return exit_code;
}
