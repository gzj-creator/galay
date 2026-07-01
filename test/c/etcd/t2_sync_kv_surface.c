#include <galay/c/galay-etcd-c/etcd_c.h>

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

static int test_builder_rejects_invalid_endpoint_on_connect(void)
{
    galay_etcd_config_builder_t* builder = NULL;
    galay_etcd_client_t* client = NULL;
    galay_etcd_error_code_t code = GALAY_ETCD_ERROR_SUCCESS;
    const char* message = NULL;

    REQUIRE_STATUS(galay_etcd_config_builder_create(&builder), GALAY_OK);
    REQUIRE_STATUS(galay_etcd_config_builder_set_endpoint(builder, "ftp://127.0.0.1:2379"), GALAY_OK);
    REQUIRE_STATUS(galay_etcd_client_create(builder, &client), GALAY_OK);
    REQUIRE_STATUS(galay_etcd_client_connect(client, &code), GALAY_INVALID_ARGUMENT);
    REQUIRE_TRUE(code == GALAY_ETCD_ERROR_INVALID_ENDPOINT);
    message = galay_etcd_error_string(code);
    REQUIRE_TRUE(message != NULL);
    REQUIRE_TRUE(strcmp(message, "invalid endpoint") == 0);
    REQUIRE_TRUE(galay_etcd_error_status(code) == GALAY_INVALID_ARGUMENT);

    galay_etcd_client_destroy(client);
    galay_etcd_config_builder_destroy(builder);
    return 0;
}

static int test_empty_key_is_rejected_without_network(void)
{
    galay_etcd_config_builder_t* builder = NULL;
    galay_etcd_client_t* client = NULL;
    int64_t deleted_count = 99;
    galay_etcd_get_result_t* result = (galay_etcd_get_result_t*)0x1;

    REQUIRE_STATUS(galay_etcd_config_builder_create(&builder), GALAY_OK);
    REQUIRE_STATUS(galay_etcd_client_create(builder, &client), GALAY_OK);
    REQUIRE_STATUS(galay_etcd_client_put(client, "", "value", strlen("value"), NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_etcd_client_get(client, "", GALAY_FALSE, 0, &result, NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_TRUE(result == NULL);
    REQUIRE_STATUS(galay_etcd_client_delete(client, "", GALAY_FALSE, &deleted_count, NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_TRUE(deleted_count == 0);

    galay_etcd_client_destroy(client);
    galay_etcd_config_builder_destroy(builder);
    return 0;
}

static int test_null_value_buffer_is_rejected(void)
{
    galay_etcd_config_builder_t* builder = NULL;
    galay_etcd_client_t* client = NULL;

    REQUIRE_STATUS(galay_etcd_config_builder_create(&builder), GALAY_OK);
    REQUIRE_STATUS(galay_etcd_client_create(builder, &client), GALAY_OK);
    REQUIRE_STATUS(galay_etcd_client_put(client, "key", NULL, 1, NULL), GALAY_INVALID_ARGUMENT);

    galay_etcd_client_destroy(client);
    galay_etcd_config_builder_destroy(builder);
    return 0;
}

static int test_not_found_result_accessor(void)
{
    galay_etcd_get_result_t* result = NULL;
    size_t count = 99;
    const char* key = NULL;
    size_t key_len = 99;
    const char* value = NULL;
    size_t value_len = 99;

    REQUIRE_STATUS(galay_etcd_get_result_create_empty(&result), GALAY_OK);
    REQUIRE_STATUS(galay_etcd_get_result_count(result, &count), GALAY_OK);
    REQUIRE_TRUE(count == 0);
    REQUIRE_STATUS(galay_etcd_get_result_item(result, 0, &key, &key_len, &value, &value_len),
                   GALAY_NOT_FOUND);
    REQUIRE_TRUE(key == NULL);
    REQUIRE_TRUE(key_len == 0);
    REQUIRE_TRUE(value == NULL);
    REQUIRE_TRUE(value_len == 0);

    galay_etcd_get_result_destroy(result);
    return 0;
}

int main(void)
{
    if (test_builder_rejects_invalid_endpoint_on_connect() != 0) {
        return 1;
    }
    if (test_empty_key_is_rejected_without_network() != 0) {
        return 1;
    }
    if (test_null_value_buffer_is_rejected() != 0) {
        return 1;
    }
    if (test_not_found_result_accessor() != 0) {
        return 1;
    }
    return 0;
}
