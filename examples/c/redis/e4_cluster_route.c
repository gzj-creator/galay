#include <galay/c/galay-redis-c/redis.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    galay_redis_cluster_t* cluster = NULL;
    uint16_t slot = 0;
    galay_redis_cluster_route_t route = {0};
    galay_redis_reply_t* redirect = NULL;
    size_t consumed = 0;
    const char moved[] = "-MOVED 1234 127.0.0.1:7002\r\n";

    if (galay_redis_cluster_create(&cluster) != GALAY_OK) {
        return 1;
    }
    if (galay_redis_cluster_key_slot("user:{42}:name", strlen("user:{42}:name"), &slot) !=
        GALAY_OK) {
        galay_redis_cluster_destroy(cluster);
        return 2;
    }

    galay_redis_cluster_node_config_t node = {
        .host = "127.0.0.1",
        .port = 7001,
        .slot_start = slot,
        .slot_end = slot,
    };
    if (galay_redis_cluster_add_node(cluster, &node) != GALAY_OK ||
        galay_redis_cluster_route_key(
            cluster, "cart:{42}:items", strlen("cart:{42}:items"), &route) != GALAY_OK) {
        galay_redis_cluster_destroy(cluster);
        return 3;
    }
    if (printf("key slot=%u route=%s:%u\n", route.slot, route.host, route.port) < 0) {
        galay_redis_cluster_destroy(cluster);
        return 4;
    }

    if (galay_redis_parse_reply(moved, sizeof(moved) - 1, &redirect, &consumed) != GALAY_OK ||
        consumed != sizeof(moved) - 1 ||
        galay_redis_cluster_apply_redirect(cluster, redirect, &route) != GALAY_OK) {
        if (redirect != NULL) {
            galay_redis_reply_free(redirect);
        }
        galay_redis_cluster_destroy(cluster);
        return 5;
    }
    if (printf("redirect slot=%u route=%s:%u type=%d\n",
               route.slot,
               route.host,
               route.port,
               (int)route.redirect_type) < 0) {
        galay_redis_reply_free(redirect);
        galay_redis_cluster_destroy(cluster);
        return 6;
    }

    galay_redis_reply_free(redirect);
    galay_redis_cluster_destroy(cluster);
    return 0;
}
