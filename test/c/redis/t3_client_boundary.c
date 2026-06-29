#include <galay/c/galay-redis-c/redis.h>

#include <stdio.h>

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

static int test_client_create_accepts_auth_config(void)
{
    galay_redis_client_config_t config = {
        .host = "127.0.0.1",
        .port = 6379,
        .username = "default",
        .password = "secret",
        .db_index = 2,
        .resp_version = 2,
        .connect_timeout_ms = 1,
    };
    galay_redis_client_t* client = NULL;

    REQUIRE_STATUS(galay_redis_client_create(&config, &client), GALAY_OK);
    REQUIRE_TRUE(client != NULL);
    REQUIRE_STATUS(galay_redis_client_disconnect(client), GALAY_OK);

    galay_redis_client_destroy(client);
    return 0;
}

static int test_client_rejects_invalid_auth_config(void)
{
    galay_redis_client_config_t config = {
        .host = "127.0.0.1",
        .port = 6379,
        .username = "default",
        .password = NULL,
        .db_index = 0,
        .resp_version = 2,
        .connect_timeout_ms = 1,
    };
    galay_redis_client_t* client = NULL;

    REQUIRE_STATUS(galay_redis_client_create(&config, &client), GALAY_INVALID_ARGUMENT);
    REQUIRE_TRUE(client == NULL);
    return 0;
}

static int test_client_rejects_null_command(void)
{
    galay_redis_client_t* client = NULL;
    galay_redis_reply_t* reply = NULL;

    REQUIRE_STATUS(galay_redis_client_create(NULL, &client), GALAY_OK);
    REQUIRE_STATUS(galay_redis_client_command(client, NULL, NULL, NULL, 0, &reply),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_TRUE(reply == NULL);

    galay_redis_client_destroy(client);
    return 0;
}

int main(void)
{
    if (test_client_create_accepts_auth_config() != 0) {
        return 1;
    }
    if (test_client_rejects_invalid_auth_config() != 0) {
        return 1;
    }
    if (test_client_rejects_null_command() != 0) {
        return 1;
    }
    return 0;
}
