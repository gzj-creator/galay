#include <galay/c/galay-postgres-c/postgres.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
        const galay_status_t got_status = (expr); \
        if (got_status != (expected)) { \
            fprintf(stderr, "status failed: %s:%d: got %d expected %d\n", \
                    __FILE__, __LINE__, (int)got_status, (int)(expected)); \
            return 1; \
        } \
    } while (0)

static int test_config_null_matrix(void)
{
    galay_postgres_config_t* config = NULL;
    const char* text = NULL;
    uint16_t port = 0;
    uint32_t timeout_ms = 0;
    galay_bool_t enabled = GALAY_FALSE;

    REQUIRE_STATUS(galay_postgres_config_create(&config), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_host(NULL, &text), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_host(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_username(NULL, &text), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_password(NULL, &text), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_database(NULL, &text), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_application_name(NULL, &text), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_port(NULL, &port), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_port(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_connect_timeout_ms(NULL, &timeout_ms),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_connect_timeout_ms(config, NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_tcp_no_delay(NULL, &enabled), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_tcp_no_delay(config, NULL), GALAY_INVALID_ARGUMENT);

    REQUIRE_STATUS(galay_postgres_config_set_host(NULL, "host"), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_host(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_username(NULL, "user"), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_username(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_password(NULL, ""), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_password(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_database(NULL, ""), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_database(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_application_name(NULL, ""), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_application_name(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_port(NULL, 5432), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_connect_timeout_ms(NULL, 1), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_set_tcp_no_delay(NULL, GALAY_TRUE),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_validate(NULL), GALAY_INVALID_ARGUMENT);

    galay_postgres_config_destroy(config);
    return 0;
}

static int test_wire_argument_matrix(void)
{
    static const unsigned char message[] = {
        'Q', 0x00, 0x00, 0x00, 0x05, 0x00, 'x',
    };
    static const unsigned char short_length[] = {'Q', 0x00, 0x00, 0x00, 0x03};
    static const unsigned char signed_length[] = {'Q', 0x80, 0x00, 0x00, 0x00};
    galay_postgres_message_header_t header = {0};
    galay_postgres_message_view_t view = {0};
    galay_postgres_buffer_t* buffer = NULL;
    const unsigned char* data = NULL;
    size_t data_len = 0;

    REQUIRE_STATUS(galay_postgres_parse_message_header(NULL, sizeof(message), &header),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_parse_message_header(message, sizeof(message), NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_parse_message_header(short_length, sizeof(short_length), &header),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_STATUS(galay_postgres_parse_message_header(signed_length, sizeof(signed_length), &header),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_STATUS(galay_postgres_extract_message(NULL, sizeof(message), &view),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_extract_message(message, sizeof(message), NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_extract_message(message, sizeof(message), &view), GALAY_OK);
    REQUIRE_TRUE(view.consumed == 6 && view.payload_len == 1 && view.payload[0] == 0);

    REQUIRE_STATUS(galay_postgres_encode_query(NULL, &buffer), GALAY_INVALID_ARGUMENT);
    REQUIRE_TRUE(buffer == NULL);
    REQUIRE_STATUS(galay_postgres_encode_query("", &buffer), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_buffer_data(NULL, &data, &data_len), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_buffer_data(buffer, NULL, &data_len), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_buffer_data(buffer, &data, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_buffer_data(buffer, &data, &data_len), GALAY_OK);
    REQUIRE_TRUE(data_len == 6 && data[0] == 'Q' && data[5] == 0);
    galay_postgres_buffer_destroy(buffer);
    buffer = NULL;

    REQUIRE_STATUS(galay_postgres_encode_terminate(NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_sync(NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_sasl_initial_response(NULL, "first", &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_sasl_initial_response("", "first", &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_sasl_initial_response("SCRAM-SHA-256", NULL, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_sasl_response(NULL, &buffer), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_password_message(NULL, &buffer), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_describe_statement(NULL, &buffer), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_describe_portal(NULL, &buffer), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_close_statement(NULL, &buffer), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_close_portal(NULL, &buffer), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_execute(NULL, 0, &buffer), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_execute("", (uint32_t)INT32_MAX, &buffer), GALAY_OK);
    galay_postgres_buffer_destroy(buffer);
    buffer = NULL;
    REQUIRE_STATUS(galay_postgres_encode_execute("", (uint32_t)INT32_MAX + 1U, &buffer),
                   GALAY_INVALID_ARGUMENT);
    return 0;
}

static int test_extended_encoder_boundaries(void)
{
    const size_t maximum_count = (size_t)INT16_MAX;
    uint32_t* oids = (uint32_t*)calloc(maximum_count, sizeof(*oids));
    galay_postgres_stmt_bind_t* binds =
        (galay_postgres_stmt_bind_t*)calloc(maximum_count, sizeof(*binds));
    galay_postgres_buffer_t* buffer = NULL;
    galay_postgres_stmt_bind_t invalid_bind = {NULL, 1, GALAY_FALSE};
    galay_postgres_stmt_bind_t invalid_flag = {NULL, 0, (galay_bool_t)9};
    galay_postgres_stmt_bind_t oversized = {
        NULL, (size_t)INT32_MAX + 1U, GALAY_FALSE,
    };
    size_t index = 0;

    REQUIRE_TRUE(oids != NULL && binds != NULL);
    for (index = 0; index < maximum_count; ++index) {
        oids[index] = 25;
        binds[index].is_null = GALAY_TRUE;
    }

    REQUIRE_STATUS(galay_postgres_encode_parse(NULL, "SELECT 1", NULL, 0, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_parse("stmt", NULL, NULL, 0, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_parse("stmt", "SELECT 1", NULL, 1, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_parse("stmt", "SELECT 1", oids, maximum_count, &buffer),
                   GALAY_OK);
    galay_postgres_buffer_destroy(buffer);
    buffer = NULL;
    REQUIRE_STATUS(galay_postgres_encode_parse(
                       "stmt", "SELECT 1", oids, maximum_count + 1U, &buffer),
                   GALAY_INVALID_ARGUMENT);

    REQUIRE_STATUS(galay_postgres_encode_bind(NULL, "stmt", NULL, 0, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_bind("", NULL, NULL, 0, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_bind("", "stmt", NULL, 1, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_bind("", "stmt", &invalid_bind, 1, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_bind("", "stmt", &invalid_flag, 1, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_bind("", "stmt", &oversized, 1, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_encode_bind("", "stmt", NULL, 0, &buffer), GALAY_OK);
    galay_postgres_buffer_destroy(buffer);
    buffer = NULL;
    REQUIRE_STATUS(galay_postgres_encode_bind("", "stmt", binds, maximum_count, &buffer),
                   GALAY_OK);
    galay_postgres_buffer_destroy(buffer);
    buffer = NULL;
    REQUIRE_STATUS(galay_postgres_encode_bind(
                       "", "stmt", binds, maximum_count + 1U, &buffer),
                   GALAY_INVALID_ARGUMENT);

    free(binds);
    free(oids);
    return 0;
}

static int test_result_and_lifecycle_boundaries(void)
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
    galay_postgres_stmt_t* stmt = NULL;
    galay_postgres_pipeline_t* pipeline = NULL;
    galay_postgres_buffer_t* buffer = NULL;
    galay_postgres_client_t* client = NULL;
    galay_postgres_config_t* config = NULL;
    galay_postgres_pool_t* pool = NULL;
    galay_postgres_field_view_t field = {0};
    galay_postgres_value_view_t value = {0};
    const char* text = NULL;
    size_t count = 0;
    size_t expected_ready = 0;
    uint64_t affected_rows = 0;
    char transaction_status = 0;
    galay_bool_t connected = GALAY_FALSE;

    REQUIRE_STATUS(galay_postgres_result_set_decode(NULL, sizeof(response), &result),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_decode(response, 0, &result), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_decode(response, sizeof(response), NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_decode(response, sizeof(response), &result), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_result_set_field(NULL, 0, &field), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_field(result, 0, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_find_field(result, "missing", &count), GALAY_NOT_FOUND);
    REQUIRE_STATUS(galay_postgres_result_set_find_field(result, NULL, &count),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_value(result, 1, 0, &value), GALAY_NOT_FOUND);
    REQUIRE_STATUS(galay_postgres_result_set_value(result, 0, 1, &value), GALAY_NOT_FOUND);
    REQUIRE_STATUS(galay_postgres_result_set_value(result, 0, 0, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_field_count(NULL, &count), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_row_count(result, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_command_tag(result, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_affected_rows(result, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_transaction_status(result, NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_result_set_command_tag(result, &text), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_result_set_affected_rows(result, &affected_rows), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_result_set_transaction_status(result, &transaction_status),
                   GALAY_OK);
    REQUIRE_TRUE(strcmp(text, "SELECT 1") == 0 && affected_rows == 1 &&
                 transaction_status == 'I');
    galay_postgres_result_set_destroy(result);

    REQUIRE_STATUS(galay_postgres_stmt_create(NULL, &stmt), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_stmt_create("", &stmt), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_stmt_create("stmt", NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_stmt_create("stmt", &stmt), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_stmt_name(NULL, &text), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_stmt_name(stmt, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_stmt_param_count(stmt, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_stmt_column_count(NULL, &count), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_stmt_field(stmt, 0, &field), GALAY_NOT_FOUND);
    REQUIRE_STATUS(galay_postgres_stmt_field(stmt, 0, NULL), GALAY_INVALID_ARGUMENT);
    galay_postgres_stmt_destroy(stmt);

    REQUIRE_STATUS(galay_postgres_pipeline_create(NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pipeline_create(&pipeline), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_pipeline_build(pipeline, &buffer, &expected_ready),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pipeline_append_query(NULL, "SELECT 1"),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pipeline_append_query(pipeline, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pipeline_append_parse(NULL, "stmt", "SELECT 1"),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pipeline_append_parse(pipeline, NULL, "SELECT 1"),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pipeline_append_sync(NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pipeline_append_parse(pipeline, "stmt", "SELECT 1"), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_pipeline_append_sync(pipeline), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_pipeline_build(pipeline, NULL, &expected_ready),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pipeline_build(pipeline, &buffer, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pipeline_build(pipeline, &buffer, &expected_ready), GALAY_OK);
    REQUIRE_TRUE(expected_ready == 1);
    galay_postgres_buffer_destroy(buffer);
    galay_postgres_pipeline_destroy(pipeline);

    REQUIRE_STATUS(galay_postgres_client_create(NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_client_create(&client), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_client_is_connected(NULL, &connected), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_client_is_connected(client, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_config_create(&config), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_config_set_username(config, "user"), GALAY_OK);
    REQUIRE_STATUS(galay_postgres_client_connect(NULL, config), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_client_connect(client, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_client_connect(client, config), GALAY_UNSUPPORTED);
    REQUIRE_TRUE(galay_postgres_client_query_async(NULL, "SELECT 1", 0, &result).code ==
                 C_IOResultInvalid);
    REQUIRE_TRUE(galay_postgres_client_query_async(client, NULL, 0, &result).code ==
                 C_IOResultInvalid);
    REQUIRE_TRUE(galay_postgres_client_query_async(client, "SELECT 1", 0, NULL).code ==
                 C_IOResultInvalid);
    galay_postgres_client_destroy(client);

    REQUIRE_STATUS(galay_postgres_pool_create(NULL, 1, &pool), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pool_create(config, 0, &pool), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pool_create(config, 1, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pool_create(config, 1, &pool), GALAY_OK);
    REQUIRE_TRUE(galay_postgres_pool_acquire_async(NULL, 0, NULL).code == C_IOResultInvalid);
    REQUIRE_STATUS(galay_postgres_pool_lease_client(NULL, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_postgres_pool_lease_release(NULL), GALAY_INVALID_ARGUMENT);
    galay_postgres_pool_destroy(pool);
    galay_postgres_config_destroy(config);
    return 0;
}

int main(void)
{
    if (test_config_null_matrix() != 0) return 1;
    if (test_wire_argument_matrix() != 0) return 1;
    if (test_extended_encoder_boundaries() != 0) return 1;
    if (test_result_and_lifecycle_boundaries() != 0) return 1;
    return 0;
}
