#include <galay/c/galay-redis-c/redis.h>

#include <stdio.h>
#include <string.h>

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "require failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1; \
        } \
    } while (0)

#define REQUIRE_STATUS(expr, expected) \
    do { \
        galay_status_t got_status = (expr); \
        if (got_status != (expected)) { \
            fprintf(stderr, "status failed: %s:%d: got %d expected %d\n", \
                    __FILE__, __LINE__, (int)got_status, (int)(expected)); \
            return 1; \
        } \
    } while (0)

static int test_hash_tag_slots_route_to_configured_node(void)
{
    galay_redis_cluster_t* cluster = NULL;
    galay_redis_cluster_route_t route = {0};
    uint16_t slot_a = 0;
    uint16_t slot_b = 0;

    REQUIRE_STATUS(galay_redis_cluster_key_slot("user:{42}:name", 14, &slot_a), GALAY_OK);
    REQUIRE_STATUS(galay_redis_cluster_key_slot("cart:{42}:items", 15, &slot_b), GALAY_OK);
    REQUIRE_TRUE(slot_a == slot_b);

    galay_redis_cluster_node_config_t node = {
        .host = "127.0.0.1",
        .port = 7001,
        .slot_start = slot_a,
        .slot_end = slot_a,
    };
    REQUIRE_STATUS(galay_redis_cluster_create(&cluster), GALAY_OK);
    REQUIRE_TRUE(cluster != NULL);
    REQUIRE_STATUS(galay_redis_cluster_add_node(cluster, &node), GALAY_OK);
    REQUIRE_STATUS(galay_redis_cluster_route_key(cluster, "cart:{42}:items", 15, &route),
                   GALAY_OK);
    REQUIRE_TRUE(route.slot == slot_a);
    REQUIRE_TRUE(route.node_index == 0);
    REQUIRE_TRUE(route.host != NULL);
    REQUIRE_TRUE(strcmp(route.host, "127.0.0.1") == 0);
    REQUIRE_TRUE(route.port == 7001);
    REQUIRE_TRUE(route.redirect_type == GALAY_REDIS_REDIRECT_NONE);

    galay_redis_cluster_destroy(cluster);
    return 0;
}

static int test_moved_redirect_updates_slot_route(void)
{
    const char raw[] = "-MOVED 1234 127.0.0.1:7002\r\n";
    galay_redis_cluster_t* cluster = NULL;
    galay_redis_reply_t* reply = NULL;
    galay_redis_cluster_route_t route = {0};
    size_t consumed = 0;

    REQUIRE_STATUS(galay_redis_cluster_create(&cluster), GALAY_OK);
    REQUIRE_STATUS(galay_redis_parse_reply(raw, sizeof(raw) - 1, &reply, &consumed), GALAY_OK);
    REQUIRE_TRUE(consumed == sizeof(raw) - 1);

    REQUIRE_STATUS(galay_redis_cluster_apply_redirect(cluster, reply, &route), GALAY_OK);
    REQUIRE_TRUE(route.redirect_type == GALAY_REDIS_REDIRECT_MOVED);
    REQUIRE_TRUE(route.slot == 1234);
    REQUIRE_TRUE(route.port == 7002);
    REQUIRE_TRUE(strcmp(route.host, "127.0.0.1") == 0);

    memset(&route, 0, sizeof(route));
    REQUIRE_STATUS(galay_redis_cluster_route_slot(cluster, 1234, &route), GALAY_OK);
    REQUIRE_TRUE(route.slot == 1234);
    REQUIRE_TRUE(route.port == 7002);

    galay_redis_reply_free(reply);
    galay_redis_cluster_destroy(cluster);
    return 0;
}

static int test_ask_redirect_is_returned_without_mutating_route_cache(void)
{
    const char raw[] = "-ASK 777 10.0.0.2:7003\r\n";
    galay_redis_cluster_t* cluster = NULL;
    galay_redis_reply_t* reply = NULL;
    galay_redis_cluster_route_t route = {0};
    size_t consumed = 0;

    REQUIRE_STATUS(galay_redis_cluster_create(&cluster), GALAY_OK);
    REQUIRE_STATUS(galay_redis_parse_reply(raw, sizeof(raw) - 1, &reply, &consumed), GALAY_OK);
    REQUIRE_TRUE(consumed == sizeof(raw) - 1);

    REQUIRE_STATUS(galay_redis_cluster_apply_redirect(cluster, reply, &route), GALAY_OK);
    REQUIRE_TRUE(route.redirect_type == GALAY_REDIS_REDIRECT_ASK);
    REQUIRE_TRUE(route.slot == 777);
    REQUIRE_TRUE(route.port == 7003);
    REQUIRE_TRUE(strcmp(route.host, "10.0.0.2") == 0);

    memset(&route, 0, sizeof(route));
    REQUIRE_STATUS(galay_redis_cluster_route_slot(cluster, 777, &route), GALAY_NOT_FOUND);

    galay_redis_reply_free(reply);
    galay_redis_cluster_destroy(cluster);
    return 0;
}

int main(void)
{
    if (test_hash_tag_slots_route_to_configured_node() != 0) {
        return 1;
    }
    if (test_moved_redirect_updates_slot_route() != 0) {
        return 1;
    }
    if (test_ask_redirect_is_returned_without_mutating_route_cache() != 0) {
        return 1;
    }
    return 0;
}
