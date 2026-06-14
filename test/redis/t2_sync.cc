#include "redis/async/redis_client.h"
#include "redis/async/conn_pool.h"
#include <kernel/kernel/runtime.h>

using namespace galay::kernel;
using namespace galay::redis;

int main()
{
    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        runtime.stop();
        return 1;
    }

    AsyncRedisConfig client_config = AsyncRedisConfig::noTimeout();
    auto client = RedisClientBuilder().scheduler(scheduler).config(client_config).build();

    auto pool_config = ConnectionPoolConfig::create("127.0.0.1", 6379, 1, 2);
    RedisConnectionPool pool(scheduler, pool_config);

    (void)client;
    (void)pool;

    runtime.stop();
    return 0;
}
