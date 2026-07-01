#include <galay/c/galay-redis-c/redis_c.h>

int main(void)
{
    return GALAY_REDIS_RESP_ARRAY == 4 ? 0 : 1;
}
