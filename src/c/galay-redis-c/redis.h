#ifndef GALAY_C_REDIS_REDIS_H
#define GALAY_C_REDIS_REDIS_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_redis_resp_type_t {
    GALAY_REDIS_RESP_SIMPLE_STRING = 0,
    GALAY_REDIS_RESP_ERROR = 1,
    GALAY_REDIS_RESP_INTEGER = 2,
    GALAY_REDIS_RESP_BULK_STRING = 3,
    GALAY_REDIS_RESP_ARRAY = 4
} galay_redis_resp_type_t;

typedef struct galay_redis_command_builder_t galay_redis_command_builder_t;
typedef struct galay_redis_reply_t galay_redis_reply_t;
typedef struct galay_redis_client_t galay_redis_client_t;

typedef struct galay_redis_client_config_t {
    const char* host;
    uint16_t port;
    const char* username;
    const char* password;
    int db_index;
    int resp_version;
    int connect_timeout_ms;
} galay_redis_client_config_t;

const char* galay_redis_get_error(galay_status_t status);
galay_status_t galay_redis_command_builder_create(galay_redis_command_builder_t** out);
void galay_redis_command_builder_destroy(galay_redis_command_builder_t* builder);
galay_status_t galay_redis_command_builder_build(galay_redis_command_builder_t* builder,
                                                 const char* command,
                                                 const char* const* args,
                                                 const size_t* arg_lens,
                                                 size_t arg_count,
                                                 const char** encoded,
                                                 size_t* encoded_len);
galay_status_t galay_redis_parse_reply(const char* data, size_t data_len,
                                       galay_redis_reply_t** out, size_t* consumed);
void galay_redis_reply_destroy(galay_redis_reply_t* reply);
galay_redis_resp_type_t galay_redis_reply_type(const galay_redis_reply_t* reply);
galay_status_t galay_redis_reply_string(const galay_redis_reply_t* reply, const char** value,
                                        size_t* value_len);
galay_status_t galay_redis_reply_array_size(const galay_redis_reply_t* reply, size_t* size);
galay_status_t galay_redis_client_create(const galay_redis_client_config_t* config,
                                         galay_redis_client_t** out);
void galay_redis_client_destroy(galay_redis_client_t* client);
galay_status_t galay_redis_client_disconnect(galay_redis_client_t* client);
galay_status_t galay_redis_client_command(galay_redis_client_t* client, const char* command,
                                          const char* const* args, const size_t* arg_lens,
                                          size_t arg_count, galay_redis_reply_t** reply);

#ifdef __cplusplus
}
#endif

#endif
