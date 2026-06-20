#include <galay/c/galay-redis/redis.h>

#include <stdio.h>

int main(void)
{
    galay_redis_command_builder_t* builder = NULL;
    const char* args[] = {"key", "value"};
    const char* data = NULL;
    size_t data_len = 0;

    if (galay_redis_command_builder_create(&builder) != GALAY_OK) {
        return 1;
    }
    if (galay_redis_command_builder_build(builder,
                                          "SET",
                                          args,
                                          NULL,
                                          2,
                                          &data,
                                          &data_len) != GALAY_OK) {
        galay_redis_command_builder_destroy(builder);
        return 1;
    }

    fwrite(data, 1, data_len, stdout);
    galay_redis_command_builder_destroy(builder);
    return 0;
}
