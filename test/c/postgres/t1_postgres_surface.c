#include <galay/c/galay-postgres-c/postgres.h>

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

static int test_error_mapping(void)
{
    static const galay_status_t statuses[] = {
        GALAY_OK,
        GALAY_INVALID_ARGUMENT,
        GALAY_NOT_FOUND,
        GALAY_OUT_OF_MEMORY,
        GALAY_PROTOCOL_ERROR,
        GALAY_UNSUPPORTED,
        GALAY_IO_ERROR,
        GALAY_INTERNAL_ERROR,
        GALAY_EOF,
        GALAY_TIMEOUT,
        GALAY_CANCELLED,
    };
    size_t index;
    for (index = 0; index < sizeof(statuses) / sizeof(statuses[0]); ++index) {
        const char* message = galay_postgres_get_error(statuses[index]);
        REQUIRE_TRUE(message != NULL);
        REQUIRE_TRUE(strcmp(message, "unknown") != 0);
    }
    REQUIRE_TRUE(strcmp(galay_postgres_get_error((galay_status_t)999), "unknown") == 0);
    return 0;
}

static int test_config(void)
{
    galay_postgres_config_t* config = NULL;
    const char* value = NULL;
    uint16_t port = 0;
    uint32_t timeout_ms = 0;
    galay_bool_t enabled = GALAY_FALSE;

    REQUIRE_STATUS(galay_postgres_config_create(NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_create(&config), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_host(config, &value), GALAY_OK);
    REQUIRE_TRUE(strcmp(value, "127.0.0.1") == 0);
    REQUIRE_STATUS(galay_postgres_config_port(config, &port), GALAY_OK);
    REQUIRE_TRUE(port == 5432);
    REQUIRE_STATUS(galay_postgres_config_connect_timeout_ms(config, &timeout_ms), GALAY_OK);
    REQUIRE_TRUE(timeout_ms == 5000);
    REQUIRE_STATUS(galay_postgres_config_tcp_no_delay(config, &enabled), GALAY_OK);
    REQUIRE_TRUE(enabled == GALAY_TRUE);
    REQUIRE_STATUS(galay_postgres_config_validate(config), GALAY_INVALID_ARGUMENT);

    REQUIRE_STATUS(galay_postgres_config_set_host(config, "db.local"), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_set_port(config, 5433), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_set_username(config, "galay"), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_set_password(config, "secret"), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_set_database(config, "app"), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_set_application_name(config, "c-surface"), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_set_connect_timeout_ms(config, 2000), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_set_tcp_no_delay(config, GALAY_FALSE), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_validate(config), GALAY_OK);

    REQUIRE_STATUS(galay_postgres_config_set_host(config, ""), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_port(config, 0), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_username(config, ""), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_connect_timeout_ms(config, 0), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_tcp_no_delay(config, (galay_bool_t)9),
                   GALAY_INVALID_ARGUMENT);
    galay_postgres_config_destroy(config);
    galay_postgres_config_destroy(NULL);
    return 0;
}

static int test_wire_helpers(void)
{
    static const unsigned char query_message[] = {
        'Q', 0x00, 0x00, 0x00, 0x0d,
        'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0x00,
    };
    galay_postgres_message_header_t header;
    galay_postgres_message_view_t view;
    galay_postgres_buffer_t* buffer = NULL;
    const unsigned char* encoded = NULL;
    size_t encoded_len = 0;

    REQUIRE_STATUS(galay_postgres_parse_message_header(query_message, sizeof(query_message),
                                                       &header), GALAY_OK);
    REQUIRE_TRUE(header.type == 'Q');
    REQUIRE_TRUE(header.length == 13);
    REQUIRE_STATUS(galay_postgres_extract_message(query_message, sizeof(query_message), &view),
                   GALAY_OK);
    REQUIRE_TRUE(view.type == 'Q');
    REQUIRE_TRUE(view.payload_len == 9);
    REQUIRE_TRUE(view.consumed == sizeof(query_message));
    REQUIRE_TRUE(memcmp(view.payload, "SELECT 1\0", 9) == 0);

    REQUIRE_STATUS(galay_postgres_parse_message_header(query_message, 4, &header),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_STATUS(galay_postgres_extract_message(query_message, sizeof(query_message) - 1, &view),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_STATUS(galay_postgres_encode_query("SELECT 1", &buffer), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_buffer_data(buffer, &encoded, &encoded_len), GALAY_OK);
    REQUIRE_TRUE(encoded_len == sizeof(query_message));
    REQUIRE_TRUE(memcmp(encoded, query_message, encoded_len) == 0);
    galay_postgres_buffer_destroy(buffer);
    return 0;
}

static int test_result_decode(void)
{
    static const unsigned char response[] = {
        'T', 0x00, 0x00, 0x00, 0x1e,
        0x00, 0x01,
        'v', 'a', 'l', 'u', 'e', 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00, 0x00, 0x17,
        0x00, 0x04,
        0xff, 0xff, 0xff, 0xff,
        0x00, 0x00,
        'D', 0x00, 0x00, 0x00, 0x0d,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x03,
        '4', '2', '1',
        'C', 0x00, 0x00, 0x00, 0x0d,
        'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0x00,
        'Z', 0x00, 0x00, 0x00, 0x05,
        'I',
    };
    galay_postgres_result_set_t* result = NULL;
    galay_postgres_field_view_t field;
    galay_postgres_value_view_t value;
    const char* tag = NULL;
    size_t count = 0;
    size_t index = 0;
    uint64_t affected_rows = 0;
    char transaction_status = 0;

    REQUIRE_STATUS(galay_postgres_result_set_decode(response, sizeof(response), &result), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_result_set_field_count(result, &count), GALAY_OK);
    REQUIRE_TRUE(count == 1);
    REQUIRE_STATUS(galay_postgres_result_set_row_count(result, &count), GALAY_OK);
    REQUIRE_TRUE(count == 1);
    REQUIRE_STATUS(galay_postgres_result_set_field(result, 0, &field), GALAY_OK);
    REQUIRE_TRUE(strcmp(field.name, "value") == 0);
    REQUIRE_TRUE(field.type_oid == 23);
    REQUIRE_STATUS(galay_postgres_result_set_find_field(result, "value", &index), GALAY_OK);
    REQUIRE_TRUE(index == 0);
    REQUIRE_STATUS(galay_postgres_result_set_value(result, 0, 0, &value), GALAY_OK);
    REQUIRE_TRUE(value.is_null == GALAY_FALSE);
    REQUIRE_TRUE(value.data_len == 3);
    REQUIRE_TRUE(memcmp(value.data, "421", 3) == 0);
    REQUIRE_STATUS(galay_postgres_result_set_command_tag(result, &tag), GALAY_OK);
    REQUIRE_TRUE(strcmp(tag, "SELECT 1") == 0);
    REQUIRE_STATUS(galay_postgres_result_set_affected_rows(result, &affected_rows), GALAY_OK);
    REQUIRE_TRUE(affected_rows == 1);
    REQUIRE_STATUS(galay_postgres_result_set_transaction_status(result, &transaction_status),
                   GALAY_OK);
    REQUIRE_TRUE(transaction_status == 'I');
    REQUIRE_STATUS(galay_postgres_result_set_field(result, 1, &field), GALAY_NOT_FOUND);
    galay_postgres_result_set_destroy(result);
    result = NULL;
    REQUIRE_STATUS(galay_postgres_result_set_decode(response, sizeof(response) - 1, &result),
                   GALAY_PROTOCOL_ERROR);
    galay_postgres_result_set_destroy(result);
    return 0;
}

static int test_result_decode_rejects_data_row_without_description(void)
{
    static const unsigned char response[] = {
        'D', 0x00, 0x00, 0x00, 0x0b,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        '1',
        'C', 0x00, 0x00, 0x00, 0x0d,
        'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0x00,
        'Z', 0x00, 0x00, 0x00, 0x05,
        'I',
    };
    galay_postgres_result_set_t* result = NULL;

    REQUIRE_STATUS(galay_postgres_result_set_decode(response, sizeof(response), &result),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(result == NULL);

    static const unsigned char invalid_ready[] = {
        'C', 0x00, 0x00, 0x00, 0x0d,
        'S', 'E', 'L', 'E', 'C', 'T', ' ', '0', 0x00,
        'Z', 0x00, 0x00, 0x00, 0x05,
        'X',
    };
    REQUIRE_STATUS(galay_postgres_result_set_decode(invalid_ready, sizeof(invalid_ready), &result),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(result == NULL);
    return 0;
}

static int test_extended_and_lifecycle(void)
{
    galay_postgres_stmt_t* stmt = NULL;
    galay_postgres_pipeline_t* pipeline = NULL;
    galay_postgres_buffer_t* buffer = NULL;
    galay_postgres_client_t* client = NULL;
    galay_postgres_config_t* config = NULL;
    const unsigned char* data = NULL;
    size_t data_len = 0;
    size_t expected_ready = 0;
    size_t count = 99;
    const char* name = NULL;
    galay_bool_t connected = GALAY_TRUE;
    galay_postgres_result_set_t* result = NULL;
    galay_postgres_result_set_t* reusable_result = NULL;

    REQUIRE_STATUS(galay_postgres_result_set_create(NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_create(&reusable_result), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_result_set_reset(reusable_result), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_result_set_reset(NULL), GALAY_INVALID_ARGUMENT);

    REQUIRE_STATUS(galay_postgres_stmt_create("s1", &stmt), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_stmt_name(stmt, &name), GALAY_OK);
    REQUIRE_TRUE(strcmp(name, "s1") == 0);
    REQUIRE_STATUS(galay_postgres_stmt_param_count(stmt, &count), GALAY_OK);
    REQUIRE_TRUE(count == 0);
    galay_postgres_stmt_destroy(stmt);

    REQUIRE_STATUS(galay_postgres_pipeline_create(&pipeline), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_pipeline_append_query(pipeline, "SELECT 1"), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_pipeline_append_sync(pipeline), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_pipeline_build(pipeline, &buffer, &expected_ready), GALAY_OK);
    REQUIRE_TRUE(expected_ready == 2);
    REQUIRE_STATUS(galay_postgres_buffer_data(buffer, &data, &data_len), GALAY_OK);
    REQUIRE_TRUE(data_len > 10);
    galay_postgres_buffer_destroy(buffer);
    galay_postgres_pipeline_destroy(pipeline);

    REQUIRE_STATUS(galay_postgres_client_create(&client), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_client_is_connected(client, &connected), GALAY_OK);
    REQUIRE_TRUE(connected == GALAY_FALSE);
    REQUIRE_STATUS(galay_postgres_config_create(&config), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_set_username(config, "galay"), GALAY_OK);
    REQUIRE_TRUE(galay_postgres_client_connect_async(NULL, config, 1).code == C_IOResultInvalid);
    REQUIRE_TRUE(galay_postgres_client_query_async(client, "SELECT 1", 1, &result).code ==
                 C_IOResultInvalid);
    REQUIRE_TRUE(galay_postgres_client_query_into_async(
                     client, "SELECT 1", 1, reusable_result).code == C_IOResultInvalid);
    REQUIRE_TRUE(galay_postgres_client_close_async(NULL, 1).code == C_IOResultInvalid);
    galay_postgres_result_set_destroy(reusable_result);
    galay_postgres_config_destroy(config);
    galay_postgres_client_close(client);
    galay_postgres_client_destroy(client);
    return 0;
}

int main(void)
{
    if (test_error_mapping() != 0) return 1;
    if (test_config() != 0) return 1;
    if (test_wire_helpers() != 0) return 1;
    if (test_result_decode() != 0) return 1;
    if (test_result_decode_rejects_data_row_without_description() != 0) return 1;
    if (test_extended_and_lifecycle() != 0) return 1;
    return 0;
}
