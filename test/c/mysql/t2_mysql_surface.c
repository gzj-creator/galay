#include <galay/c/galay-mysql-c/mysql.h>

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

static int test_config_builder_boundaries(void)
{
    galay_mysql_config_t* config = NULL;
    const char* host = NULL;
    uint16_t port = 0;

    REQUIRE_STATUS(galay_mysql_config_create(NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_config_create(&config), GALAY_OK);
    REQUIRE_TRUE(config != NULL);

    REQUIRE_STATUS(galay_mysql_config_host(config, &host), GALAY_OK);
    REQUIRE_TRUE(host != NULL);
    REQUIRE_TRUE(strcmp(host, "127.0.0.1") == 0);
    REQUIRE_STATUS(galay_mysql_config_port(config, &port), GALAY_OK);
    REQUIRE_TRUE(port == 3306);

    REQUIRE_STATUS(galay_mysql_config_set_host(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_config_set_host(config, ""), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_config_set_port(config, 0), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_config_set_username(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_config_set_password(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_config_set_database(config, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_config_set_charset(config, ""), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_config_set_connect_timeout_ms(config, 0), GALAY_INVALID_ARGUMENT);

    REQUIRE_STATUS(galay_mysql_config_set_host(config, "db.local"), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_config_set_port(config, 3307), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_config_set_username(config, "user"), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_config_set_password(config, "secret"), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_config_set_database(config, "app"), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_config_set_charset(config, "utf8mb4"), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_config_set_connect_timeout_ms(config, 1000), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_config_validate(config), GALAY_OK);

    galay_mysql_config_destroy(config);
    galay_mysql_config_destroy(NULL);
    return 0;
}

static int test_auth_plugin_boundaries(void)
{
    static const unsigned char salt[] = "12345678901234567890";
    galay_mysql_buffer_t* buffer = NULL;
    const unsigned char* data = NULL;
    size_t data_len = 0;

    REQUIRE_STATUS(galay_mysql_auth_response_for_plugin("mysql_native_password",
                                                        "secret",
                                                        salt,
                                                        sizeof(salt) - 1,
                                                        &buffer),
                   GALAY_OK);
    REQUIRE_STATUS(galay_mysql_buffer_data(buffer, &data, &data_len), GALAY_OK);
    REQUIRE_TRUE(data != NULL);
    REQUIRE_TRUE(data_len == 20);
    galay_mysql_buffer_destroy(buffer);
    buffer = NULL;

    REQUIRE_STATUS(galay_mysql_auth_response_for_plugin("not_supported",
                                                        "secret",
                                                        salt,
                                                        sizeof(salt) - 1,
                                                        &buffer),
                   GALAY_UNSUPPORTED);
    REQUIRE_TRUE(buffer == NULL);

    REQUIRE_STATUS(galay_mysql_auth_response_for_plugin(NULL, "secret", salt,
                                                        sizeof(salt) - 1, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_auth_response_for_plugin("mysql_native_password", NULL, salt,
                                                        sizeof(salt) - 1, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_auth_response_for_plugin("mysql_native_password", "secret",
                                                        NULL, sizeof(salt) - 1, &buffer),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_auth_response_for_plugin("mysql_native_password", "secret",
                                                        salt, sizeof(salt) - 1, NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_buffer_data(NULL, &data, &data_len), GALAY_INVALID_ARGUMENT);
    return 0;
}

static int test_packet_helpers_and_parse_errors(void)
{
    const unsigned char query_packet[] = {
        0x09, 0x00, 0x00, 0x00,
        0x03, 'S', 'E', 'L', 'E', 'C', 'T', ' ', '1'
    };
    const unsigned char short_header[] = {0x01, 0x00, 0x00};
    const unsigned char short_payload[] = {0x04, 0x00, 0x00, 0x00, 'a'};
    galay_mysql_packet_header_t header;
    galay_mysql_packet_view_t view;
    galay_mysql_buffer_t* encoded = NULL;
    const unsigned char* encoded_data = NULL;
    size_t encoded_len = 0;

    REQUIRE_STATUS(galay_mysql_parse_packet_header(query_packet, sizeof(query_packet), &header), GALAY_OK);
    REQUIRE_TRUE(header.payload_length == 9);
    REQUIRE_TRUE(header.sequence_id == 0);
    REQUIRE_STATUS(galay_mysql_extract_packet(query_packet, sizeof(query_packet), &view), GALAY_OK);
    REQUIRE_TRUE(view.payload != NULL);
    REQUIRE_TRUE(view.payload_len == 9);
    REQUIRE_TRUE(view.sequence_id == 0);
    REQUIRE_TRUE(view.consumed == sizeof(query_packet));

    REQUIRE_STATUS(galay_mysql_parse_packet_header(short_header, sizeof(short_header), &header),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_STATUS(galay_mysql_extract_packet(short_payload, sizeof(short_payload), &view),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_STATUS(galay_mysql_parse_packet_header(query_packet, sizeof(query_packet), NULL),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_extract_packet(query_packet, sizeof(query_packet), NULL),
                   GALAY_INVALID_ARGUMENT);

    REQUIRE_STATUS(galay_mysql_encode_query_packet("SELECT 1", 0, &encoded), GALAY_OK);
    REQUIRE_STATUS(galay_mysql_buffer_data(encoded, &encoded_data, &encoded_len), GALAY_OK);
    REQUIRE_TRUE(encoded_data != NULL);
    REQUIRE_TRUE(encoded_len == sizeof(query_packet));
    REQUIRE_TRUE(memcmp(encoded_data, query_packet, sizeof(query_packet)) == 0);
    galay_mysql_buffer_destroy(encoded);

    REQUIRE_STATUS(galay_mysql_encode_query_packet(NULL, 0, &encoded), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_encode_query_packet("SELECT 1", 0, NULL), GALAY_INVALID_ARGUMENT);
    return 0;
}

static int test_sync_client_lifecycle(void)
{
    galay_mysql_client_t* client = NULL;
    galay_bool_t connected = GALAY_TRUE;

    REQUIRE_STATUS(galay_mysql_client_create(NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_client_create(&client), GALAY_OK);
    REQUIRE_TRUE(client != NULL);
    REQUIRE_STATUS(galay_mysql_client_is_connected(client, &connected), GALAY_OK);
    REQUIRE_TRUE(connected == GALAY_FALSE);
    REQUIRE_STATUS(galay_mysql_client_is_connected(client, NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_client_is_connected(NULL, &connected), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mysql_client_connect(NULL, NULL), GALAY_INVALID_ARGUMENT);
    galay_mysql_client_close(client);
    galay_mysql_client_destroy(client);
    galay_mysql_client_destroy(NULL);
    return 0;
}

int main(void)
{
    if (test_config_builder_boundaries() != 0) {
        return 1;
    }
    if (test_auth_plugin_boundaries() != 0) {
        return 1;
    }
    if (test_packet_helpers_and_parse_errors() != 0) {
        return 1;
    }
    if (test_sync_client_lifecycle() != 0) {
        return 1;
    }
    return 0;
}
