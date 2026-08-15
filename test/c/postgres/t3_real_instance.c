#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>
#include <galay/c/galay-postgres-c/postgres.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PostgresIntegrationState {
    const char* host;
    const char* user;
    const char* password;
    const char* database;
    uint16_t port;
    C_IOResult result;
    int values_ok;
    int transaction_ok;
    int prepared_ok;
    int pipeline_ok;
    int error_drained;
    int pool_ok;
} PostgresIntegrationState;

static int value_equals(const galay_postgres_result_set_t* result,
                        size_t row,
                        size_t column,
                        const char* expected)
{
    galay_postgres_value_view_t value = {0};
    return galay_postgres_result_set_value(result, row, column, &value) == GALAY_OK &&
        value.is_null == GALAY_FALSE && value.data_len == strlen(expected) &&
        memcmp(value.data, expected, value.data_len) == 0;
}

static void integration_client_entry(void* arg)
{
    PostgresIntegrationState* state = (PostgresIntegrationState*)arg;
    galay_postgres_config_t* config = NULL;
    galay_postgres_client_t* client = NULL;
    galay_postgres_result_set_t* result = NULL;
    galay_postgres_stmt_t* stmt = NULL;
    galay_postgres_pipeline_t* pipeline = NULL;
    galay_postgres_pipeline_result_t* pipeline_result = NULL;
    galay_postgres_pool_t* pool = NULL;
    galay_postgres_pool_lease_t* lease = NULL;
    galay_postgres_client_t* pooled_client = NULL;
    galay_postgres_result_set_t* pool_result = NULL;
    galay_postgres_value_view_t null_value = {0};
    galay_postgres_stmt_bind_t binds[2] = {
        {(const unsigned char*)"41", 2, GALAY_FALSE},
        {(const unsigned char*)"hello", 5, GALAY_FALSE},
    };
    const galay_postgres_result_set_t* pipeline_item = NULL;
    size_t fields = 0;
    size_t rows = 0;
    size_t count = 0;
    char transaction_status = 0;

    if (galay_postgres_config_create(&config) != GALAY_OK ||
        galay_postgres_config_set_host(config, state->host) != GALAY_OK ||
        galay_postgres_config_set_port(config, state->port) != GALAY_OK ||
        galay_postgres_config_set_username(config, state->user) != GALAY_OK ||
        galay_postgres_config_set_password(config, state->password) != GALAY_OK ||
        galay_postgres_config_set_database(config, state->database) != GALAY_OK ||
        galay_postgres_config_set_application_name(config, "galay-c-integration") != GALAY_OK ||
        galay_postgres_client_create(&client) != GALAY_OK) {
        state->result = (C_IOResult){C_IOResultError, 0, 0, GALAY_INTERNAL_ERROR, NULL};
        goto cleanup;
    }
    state->result = galay_postgres_client_connect_async(client, config, 5000);
    if (state->result.code != C_IOResultOk) goto cleanup;

    state->result = galay_postgres_client_query_async(
        client, "SELECT 42::int4 AS answer, NULL::text AS missing", 5000, &result);
    if (state->result.code != C_IOResultOk ||
        galay_postgres_result_set_field_count(result, &fields) != GALAY_OK || fields != 2 ||
        galay_postgres_result_set_row_count(result, &rows) != GALAY_OK || rows != 1 ||
        !value_equals(result, 0, 0, "42") ||
        galay_postgres_result_set_value(result, 0, 1, &null_value) != GALAY_OK ||
        null_value.is_null != GALAY_TRUE || null_value.data != NULL || null_value.data_len != 0) {
        state->result = (C_IOResult){C_IOResultError, 0, 0, GALAY_PROTOCOL_ERROR, NULL};
        goto cleanup;
    }
    state->values_ok = 1;
    galay_postgres_result_set_destroy(result);
    result = NULL;

    state->result = galay_postgres_client_begin_transaction_async(client, 5000, &result);
    if (state->result.code != C_IOResultOk ||
        galay_postgres_result_set_transaction_status(result, &transaction_status) != GALAY_OK ||
        transaction_status != 'T') goto cleanup;
    galay_postgres_result_set_destroy(result);
    result = NULL;
    state->result = galay_postgres_client_rollback_async(client, 5000, &result);
    if (state->result.code != C_IOResultOk ||
        galay_postgres_result_set_transaction_status(result, &transaction_status) != GALAY_OK ||
        transaction_status != 'I') goto cleanup;
    state->transaction_ok = 1;
    galay_postgres_result_set_destroy(result);
    result = NULL;

    state->result = galay_postgres_client_stmt_prepare_async(
        client, "galay_c_stmt", "SELECT $1::int4 + 1 AS answer, $2::text AS echoed",
        5000, &stmt);
    if (state->result.code != C_IOResultOk ||
        galay_postgres_stmt_param_count(stmt, &count) != GALAY_OK || count != 2 ||
        galay_postgres_stmt_column_count(stmt, &count) != GALAY_OK || count != 2) goto cleanup;
    state->result = galay_postgres_client_stmt_execute_async(
        client, stmt, binds, 2, 5000, &result);
    if (state->result.code != C_IOResultOk || !value_equals(result, 0, 0, "42") ||
        !value_equals(result, 0, 1, "hello")) goto cleanup;
    state->prepared_ok = 1;
    galay_postgres_result_set_destroy(result);
    result = NULL;

    if (galay_postgres_pipeline_create(&pipeline) != GALAY_OK ||
        galay_postgres_pipeline_append_query(pipeline, "SELECT 8") != GALAY_OK ||
        galay_postgres_pipeline_append_query(pipeline, "SELECT 9") != GALAY_OK) {
        state->result = (C_IOResult){C_IOResultError, 0, 0, GALAY_INTERNAL_ERROR, NULL};
        goto cleanup;
    }
    state->result = galay_postgres_client_pipeline_async(
        client, pipeline, 5000, &pipeline_result);
    if (state->result.code != C_IOResultOk ||
        galay_postgres_pipeline_result_count(pipeline_result, &count) != GALAY_OK || count != 2 ||
        galay_postgres_pipeline_result_at(pipeline_result, 0, &pipeline_item) != GALAY_OK ||
        !value_equals(pipeline_item, 0, 0, "8") ||
        galay_postgres_pipeline_result_at(pipeline_result, 1, &pipeline_item) != GALAY_OK ||
        !value_equals(pipeline_item, 0, 0, "9")) goto cleanup;
    state->pipeline_ok = 1;

    state->result = galay_postgres_client_query_async(
        client, "SELECT * FROM galay_c_missing_table", 5000, &result);
    if (state->result.code != C_IOResultError || result != NULL) goto cleanup;
    state->error_drained = 1;

    state->result = galay_postgres_client_query_async(client, "SELECT 7", 5000, &result);
    if (state->result.code != C_IOResultOk || !value_equals(result, 0, 0, "7")) goto cleanup;
    galay_postgres_result_set_destroy(result);
    result = NULL;
    state->result = galay_postgres_client_close_async(client, 5000);
    if (state->result.code != C_IOResultOk ||
        galay_postgres_pool_create(config, 1, &pool) != GALAY_OK) goto cleanup;
    state->result = galay_postgres_pool_acquire_async(pool, 5000, &lease);
    if (state->result.code != C_IOResultOk ||
        galay_postgres_pool_lease_client(lease, &pooled_client) != GALAY_OK ||
        pooled_client == NULL) goto cleanup;
    state->result = galay_postgres_client_query_async(
        pooled_client, "SELECT 11", 5000, &pool_result);
    if (state->result.code != C_IOResultOk || !value_equals(pool_result, 0, 0, "11") ||
        galay_postgres_pool_lease_release(lease) != GALAY_OK) goto cleanup;
    lease = NULL;
    pooled_client = NULL;
    galay_postgres_result_set_destroy(pool_result);
    pool_result = NULL;
    galay_postgres_pool_destroy(pool);
    pool = NULL;
    state->pool_ok = 1;

cleanup:
    galay_postgres_pipeline_result_destroy(pipeline_result);
    galay_postgres_pipeline_destroy(pipeline);
    galay_postgres_stmt_destroy(stmt);
    galay_postgres_result_set_destroy(pool_result);
    if (lease != NULL) {
        const galay_status_t released = galay_postgres_pool_lease_release(lease);
        if (released != GALAY_OK && state->result.code == C_IOResultOk) {
            state->result = (C_IOResult){C_IOResultError, 0, 0, released, NULL};
        }
    }
    galay_postgres_pool_destroy(pool);
    galay_postgres_result_set_destroy(result);
    galay_postgres_client_destroy(client);
    galay_postgres_config_destroy(config);
}

int main(void)
{
    const char* enabled = getenv("GALAY_IT_ENABLE");
    const char* host = getenv("GALAY_POSTGRES_TEST_HOST");
    const char* port_text = getenv("GALAY_POSTGRES_TEST_PORT");
    const char* user = getenv("GALAY_POSTGRES_TEST_USER");
    const char* password = getenv("GALAY_POSTGRES_TEST_PASSWORD");
    const char* database = getenv("GALAY_POSTGRES_TEST_DATABASE");
    char* end = NULL;
    unsigned long port = 0;
    if (enabled == NULL || strcmp(enabled, "1") != 0 || host == NULL || port_text == NULL ||
        user == NULL || password == NULL || database == NULL) return 125;
    port = strtoul(port_text, &end, 10);
    if (end == port_text || *end != '\0' || port == 0 || port > 65535) return 125;

    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_coro_task_t task = {0};
    PostgresIntegrationState state = {
        .host = host,
        .user = user,
        .password = password,
        .database = database,
        .port = (uint16_t)port,
    };
    int result = 0;
    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess) return 1;
    if (galay_c_coro_spawn(&runtime, integration_client_entry, &state, NULL, &task).code !=
        C_IOResultOk || galay_c_coro_join(&task, 15000).code != C_IOResultOk) {
        result = 2;
    } else if (state.result.code != C_IOResultOk || !state.values_ok ||
               !state.transaction_ok || !state.prepared_ok || !state.pipeline_ok ||
               !state.error_drained || !state.pool_ok) {
        fprintf(stderr, "integration failed: code=%d value=%lld errno=%d\n",
                (int)state.result.code, (long long)state.result.value, state.result.sys_errno);
        result = 3;
    }
    if (task.task != NULL && galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) result = 4;
    if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) result = 5;
    if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) result = 6;
    return result;
}
