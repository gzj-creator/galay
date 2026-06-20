#include <galay/c/galay-redis/redis.h>

int main(void)
{
    return GALAY_REDIS_RESP_ARRAY == 4 ? 0 : 1;
}
