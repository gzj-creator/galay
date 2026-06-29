#ifndef GALAY_C_MONGO_MONGO_H
#define GALAY_C_MONGO_MONGO_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALAY_MONGO_MAX_KEY_LENGTH 255u
#define GALAY_MONGO_MAX_STRING_LENGTH 4096u

typedef struct galay_mongo_document_t galay_mongo_document_t;
typedef struct galay_mongo_uri_t galay_mongo_uri_t;
typedef struct galay_mongo_client_t galay_mongo_client_t;

const char* galay_mongo_get_error(galay_status_t status);
galay_status_t galay_mongo_document_create(galay_mongo_document_t** out);
void galay_mongo_document_destroy(galay_mongo_document_t* document);
size_t galay_mongo_document_size(const galay_mongo_document_t* document);
galay_status_t galay_mongo_document_append_int32(galay_mongo_document_t* document, const char* key, int32_t value);
galay_status_t galay_mongo_document_append_int64(galay_mongo_document_t* document, const char* key, int64_t value);
galay_status_t galay_mongo_document_append_double(galay_mongo_document_t* document, const char* key, double value);
galay_status_t galay_mongo_document_append_bool(galay_mongo_document_t* document, const char* key, galay_bool_t value);
galay_status_t galay_mongo_document_append_string(galay_mongo_document_t* document, const char* key, const char* value, size_t value_len);
galay_status_t galay_mongo_document_append_null(galay_mongo_document_t* document, const char* key);
galay_status_t galay_mongo_document_encode(galay_mongo_document_t* document, const uint8_t** bson, size_t* bson_len);
galay_status_t galay_mongo_document_decode(const uint8_t* bson, size_t bson_len, galay_mongo_document_t** out);
galay_status_t galay_mongo_document_get_int32(const galay_mongo_document_t* document, const char* key, int32_t* value);
galay_status_t galay_mongo_document_get_int64(const galay_mongo_document_t* document, const char* key, int64_t* value);
galay_status_t galay_mongo_document_get_double(const galay_mongo_document_t* document, const char* key, double* value);
galay_status_t galay_mongo_document_get_bool(const galay_mongo_document_t* document, const char* key, galay_bool_t* value);
galay_status_t galay_mongo_document_get_string(const galay_mongo_document_t* document, const char* key, const char** value, size_t* value_len);

galay_status_t galay_mongo_uri_parse(const char* uri_text, galay_mongo_uri_t** out);
void galay_mongo_uri_destroy(galay_mongo_uri_t* uri);
galay_status_t galay_mongo_uri_host(const galay_mongo_uri_t* uri, const char** host, size_t* host_len);
galay_status_t galay_mongo_uri_database(const galay_mongo_uri_t* uri, const char** database, size_t* database_len);
galay_status_t galay_mongo_uri_port(const galay_mongo_uri_t* uri, uint16_t* port);

galay_status_t galay_mongo_client_create(galay_mongo_client_t** out);
void galay_mongo_client_destroy(galay_mongo_client_t* client);
void galay_mongo_client_close(galay_mongo_client_t* client);
galay_bool_t galay_mongo_client_is_connected(const galay_mongo_client_t* client);
galay_status_t galay_mongo_client_ping(galay_mongo_client_t* client, const char* database);

#ifdef __cplusplus
}
#endif

#endif
