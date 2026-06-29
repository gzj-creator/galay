#ifndef GALAY_C_MYSQL_MYSQL_H
#define GALAY_C_MYSQL_MYSQL_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct galay_mysql_config_t galay_mysql_config_t;
typedef struct galay_mysql_buffer_t galay_mysql_buffer_t;
typedef struct galay_mysql_client_t galay_mysql_client_t;

typedef struct galay_mysql_packet_header_t {
    uint32_t payload_length;
    uint8_t sequence_id;
} galay_mysql_packet_header_t;

typedef struct galay_mysql_packet_view_t {
    const unsigned char* payload;
    size_t payload_len;
    uint8_t sequence_id;
    size_t consumed;
} galay_mysql_packet_view_t;

const char* galay_mysql_get_error(galay_status_t status);
galay_status_t galay_mysql_config_create(galay_mysql_config_t** out);
void galay_mysql_config_destroy(galay_mysql_config_t* config);
galay_status_t galay_mysql_config_host(const galay_mysql_config_t* config, const char** host);
galay_status_t galay_mysql_config_port(const galay_mysql_config_t* config, uint16_t* port);
galay_status_t galay_mysql_config_set_host(galay_mysql_config_t* config, const char* host);
galay_status_t galay_mysql_config_set_port(galay_mysql_config_t* config, uint16_t port);
galay_status_t galay_mysql_config_set_username(galay_mysql_config_t* config, const char* username);
galay_status_t galay_mysql_config_set_password(galay_mysql_config_t* config, const char* password);
galay_status_t galay_mysql_config_set_database(galay_mysql_config_t* config, const char* database);
galay_status_t galay_mysql_config_set_charset(galay_mysql_config_t* config, const char* charset);
galay_status_t galay_mysql_config_set_connect_timeout_ms(galay_mysql_config_t* config, uint32_t timeout_ms);
galay_status_t galay_mysql_config_validate(const galay_mysql_config_t* config);
galay_status_t galay_mysql_auth_response_for_plugin(const char* plugin, const char* password,
                                                    const unsigned char* salt, size_t salt_len,
                                                    galay_mysql_buffer_t** out);
void galay_mysql_buffer_destroy(galay_mysql_buffer_t* buffer);
galay_status_t galay_mysql_buffer_data(const galay_mysql_buffer_t* buffer,
                                       const unsigned char** data, size_t* data_len);
galay_status_t galay_mysql_parse_packet_header(const unsigned char* data, size_t data_len,
                                               galay_mysql_packet_header_t* header);
galay_status_t galay_mysql_extract_packet(const unsigned char* data, size_t data_len,
                                          galay_mysql_packet_view_t* view);
galay_status_t galay_mysql_encode_query_packet(const char* query, uint8_t sequence_id,
                                               galay_mysql_buffer_t** out);
galay_status_t galay_mysql_client_create(galay_mysql_client_t** out);
void galay_mysql_client_destroy(galay_mysql_client_t* client);
void galay_mysql_client_close(galay_mysql_client_t* client);
galay_status_t galay_mysql_client_is_connected(const galay_mysql_client_t* client,
                                               galay_bool_t* connected);
galay_status_t galay_mysql_client_connect(galay_mysql_client_t* client,
                                          const galay_mysql_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
