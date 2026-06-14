#include "redis/base/redis_value.h"

using galay::redis::RedisValue;

static_assert(sizeof(RedisValue) <= 96, "RedisValue hot-path footprint regressed");

int main()
{
    return 0;
}
