#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>
#include <galay/c/galay-postgres-c/postgres.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PostgresAsyncQueryExample {
    const char* host;
    const char* user;
    const char* password;
    const char* database;
    uint16_t port;
    C_IOResult result;
    size_t field_count;
    size_t row_count;
    char transaction_status;
    char value[32];
    galay_bool_t optional_is_null;
} PostgresAsyncQueryExample;

static C_IOResult example_error(galay_status_t status)
{
    return (C_IOResult){C_IOResultError, (int)status, 0, 0, NULL};
}

static const char* environment_or(const char* name, const char* fallback)
{
    const char* value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static uint16_t postgres_port(void)
{
    const char* text = environment_or("GALAY_POSTGRES_PORT", "5432");
    char* end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > UINT16_MAX) {
        return 5432;
    }
    return (uint16_t)value;
}

static void query_entry(void* arg)
{
    PostgresAsyncQueryExample* example = (PostgresAsyncQueryExample*)arg;
    galay_postgres_config_t* config = NULL;
    galay_postgres_client_t* client = NULL;
    galay_postgres_result_set_t* result_set = NULL;
    galay_postgres_value_view_t value = {0};
    galay_postgres_value_view_t optional_value = {0};

    galay_status_t status = galay_postgres_config_create(&config);
    if (status == GALAY_OK) status = galay_postgres_config_set_host(config, example->host);
    if (status == GALAY_OK) status = galay_postgres_config_set_port(config, example->port);
    if (status == GALAY_OK) status = galay_postgres_config_set_username(config, example->user);
    if (status == GALAY_OK) status = galay_postgres_config_set_password(config, example->password);
    if (status == GALAY_OK) status = galay_postgres_config_set_database(config, example->database);
    if (status == GALAY_OK) {
        status = galay_postgres_config_set_application_name(config, "galay-c-example");
    }
    if (status == GALAY_OK) status = galay_postgres_client_create(&client);
    if (status != GALAY_OK) {
        example->result = example_error(status);
        goto cleanup;
    }

    example->result = galay_postgres_client_connect_async(client, config, 5000);
    if (example->result.code != C_IOResultOk) {
        goto cleanup;
    }
    example->result = galay_postgres_client_query_async(
        client,
        "SELECT 1::int4 AS value, NULL::text AS optional_value",
        5000,
        &result_set);
    if (example->result.code != C_IOResultOk) {
        goto close_client;
    }

    status = galay_postgres_result_set_field_count(result_set, &example->field_count);
    if (status == GALAY_OK) {
        status = galay_postgres_result_set_row_count(result_set, &example->row_count);
    }
    if (status == GALAY_OK) {
        status = galay_postgres_result_set_transaction_status(
            result_set, &example->transaction_status);
    }
    if (status == GALAY_OK) {
        status = galay_postgres_result_set_value(result_set, 0, 0, &value);
    }
    if (status == GALAY_OK) {
        status = galay_postgres_result_set_value(result_set, 0, 1, &optional_value);
    }
    if (status != GALAY_OK || value.is_null != GALAY_FALSE ||
        value.data_len >= sizeof(example->value)) {
        example->result = example_error(
            status == GALAY_OK ? GALAY_PROTOCOL_ERROR : status);
        goto close_client;
    }
    if (value.data_len != 0) {
        memcpy(example->value, value.data, value.data_len);
    }
    example->value[value.data_len] = '\0';
    example->optional_is_null = optional_value.is_null;

close_client:
    {
        C_IOResult closed = galay_postgres_client_close_async(client, 5000);
        if (example->result.code == C_IOResultOk && closed.code != C_IOResultOk) {
            example->result = closed;
        }
    }

cleanup:
    galay_postgres_result_set_destroy(result_set);
    galay_postgres_client_destroy(client);
    galay_postgres_config_destroy(config);
}

int main(void)
{
    PostgresAsyncQueryExample example = {
        .host = environment_or("GALAY_POSTGRES_HOST", "127.0.0.1"),
        .user = environment_or("GALAY_POSTGRES_USER", "postgres"),
        .password = environment_or("GALAY_POSTGRES_PASSWORD", "postgres"),
        .database = environment_or("GALAY_POSTGRES_DB", "postgres"),
        .port = postgres_port(),
    };
    C_RuntimeConfig runtime_config = galay_c_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_coro_task_t task = {0};
    int exit_code = 0;

    C_RuntimeResultCode runtime_status = galay_c_runtime_create(&runtime_config, &runtime);
    if (runtime_status == C_RuntimeSuccess) {
        runtime_status = galay_c_runtime_start(&runtime);
    }
    if (runtime_status != C_RuntimeSuccess) {
        fprintf(stderr, "runtime setup failed: %s\n",
                galay_c_runtime_get_error(runtime_status));
        exit_code = 1;
        goto cleanup;
    }

    C_IOResult spawned = galay_c_coro_spawn(&runtime, query_entry, &example, NULL, &task);
    if (spawned.code != C_IOResultOk) {
        fprintf(stderr, "query task spawn failed: %s\n",
                galay_c_coro_ioresult_get_error(spawned.code));
        exit_code = 2;
        goto cleanup;
    }
    C_IOResult joined = galay_c_coro_join(&task, 15000);
    if (joined.code != C_IOResultOk) {
        fprintf(stderr, "query task join failed: %s\n",
                galay_c_coro_ioresult_get_error(joined.code));
        exit_code = 3;
        goto cleanup;
    }
    if (example.result.code != C_IOResultOk || example.field_count != 2 ||
        example.row_count != 1 || example.transaction_status != 'I' ||
        strcmp(example.value, "1") != 0 || example.optional_is_null != GALAY_TRUE) {
        fprintf(stderr,
                "query failed: io=%s status=%d fields=%zu rows=%zu tx=%c\n",
                galay_c_coro_ioresult_get_error(example.result.code),
                example.result.sys_errno,
                example.field_count,
                example.row_count,
                example.transaction_status == '\0' ? '?' : example.transaction_status);
        exit_code = 4;
        goto cleanup;
    }
    if (printf("postgres value=%s optional=NULL transaction_status=%c\n",
               example.value,
               example.transaction_status) < 0) {
        exit_code = 5;
    }

cleanup:
    if (task.task != NULL) {
        C_IOResult destroyed = galay_c_coro_destroy(&task);
        if (destroyed.code != C_IOResultOk && exit_code == 0) {
            exit_code = 6;
        }
    }
    if (runtime.runtime != NULL) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 7;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 8;
        }
    }
    return exit_code;
}
