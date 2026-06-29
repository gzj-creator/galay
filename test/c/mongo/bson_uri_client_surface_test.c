#include <galay/c/galay-mongo-c/mongo.h>

#include <stdint.h>
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

static int test_bson_builder_reader_round_trip(void)
{
    galay_mongo_document_t* document = NULL;
    galay_mongo_document_t* decoded = NULL;
    const uint8_t* bson = NULL;
    size_t bson_len = 0;
    int32_t i32 = 0;
    int64_t i64 = 0;
    double d = 0.0;
    galay_bool_t b = GALAY_FALSE;
    const char* text = NULL;
    size_t text_len = 0;

    REQUIRE_STATUS(galay_mongo_document_create(&document), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_int32(document, "answer", 42), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_int64(document, "large", 9000000000LL), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_double(document, "ratio", 1.5), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_bool(document, "flag", GALAY_TRUE), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_string(document, "name", "galay", 5), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_null(document, "none"), GALAY_OK);
    REQUIRE_TRUE(galay_mongo_document_size(document) == 6);

    REQUIRE_STATUS(galay_mongo_document_encode(document, &bson, &bson_len), GALAY_OK);
    REQUIRE_TRUE(bson != NULL);
    REQUIRE_TRUE(bson_len > 5);

    REQUIRE_STATUS(galay_mongo_document_decode(bson, bson_len, &decoded), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_int32(decoded, "answer", &i32), GALAY_OK);
    REQUIRE_TRUE(i32 == 42);
    REQUIRE_STATUS(galay_mongo_document_get_int64(decoded, "large", &i64), GALAY_OK);
    REQUIRE_TRUE(i64 == 9000000000LL);
    REQUIRE_STATUS(galay_mongo_document_get_double(decoded, "ratio", &d), GALAY_OK);
    REQUIRE_TRUE(d == 1.5);
    REQUIRE_STATUS(galay_mongo_document_get_bool(decoded, "flag", &b), GALAY_OK);
    REQUIRE_TRUE(b == GALAY_TRUE);
    REQUIRE_STATUS(galay_mongo_document_get_string(decoded, "name", &text, &text_len), GALAY_OK);
    REQUIRE_TRUE(text_len == 5);
    REQUIRE_TRUE(strncmp(text, "galay", text_len) == 0);
    REQUIRE_STATUS(galay_mongo_document_get_string(decoded, "missing", &text, &text_len), GALAY_NOT_FOUND);

    galay_mongo_document_destroy(decoded);
    galay_mongo_document_destroy(document);
    return 0;
}

static int test_bson_boundaries(void)
{
    galay_mongo_document_t* document = NULL;
    galay_mongo_document_t* decoded = NULL;
    const uint8_t malformed[] = {4, 0, 0, 0};
    char oversized_key[GALAY_MONGO_MAX_KEY_LENGTH + 2];
    char oversized_value[GALAY_MONGO_MAX_STRING_LENGTH + 2];

    memset(oversized_key, 'k', sizeof(oversized_key));
    oversized_key[sizeof(oversized_key) - 1] = '\0';
    memset(oversized_value, 'v', sizeof(oversized_value));
    oversized_value[sizeof(oversized_value) - 1] = '\0';

    REQUIRE_STATUS(galay_mongo_document_create(&document), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_decode(malformed, sizeof(malformed), &decoded),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_STATUS(galay_mongo_document_append_int32(document, oversized_key, 1),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mongo_document_append_string(document, "too_big", oversized_value,
                                                     sizeof(oversized_value)),
                   GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mongo_document_append_string(document, "null_data", NULL, 1),
                   GALAY_INVALID_ARGUMENT);

    galay_mongo_document_destroy(document);
    return 0;
}

static int test_uri_parse_boundaries(void)
{
    galay_mongo_uri_t* uri = NULL;
    const char* text = NULL;
    size_t text_len = 0;
    uint16_t port = 0;

    REQUIRE_STATUS(galay_mongo_uri_parse("mongodb://localhost:27018/app?appName=capi", &uri),
                   GALAY_OK);
    REQUIRE_STATUS(galay_mongo_uri_host(uri, &text, &text_len), GALAY_OK);
    REQUIRE_TRUE(text_len == strlen("localhost"));
    REQUIRE_TRUE(strncmp(text, "localhost", text_len) == 0);
    REQUIRE_STATUS(galay_mongo_uri_database(uri, &text, &text_len), GALAY_OK);
    REQUIRE_TRUE(text_len == strlen("app"));
    REQUIRE_STATUS(galay_mongo_uri_port(uri, &port), GALAY_OK);
    REQUIRE_TRUE(port == 27018);
    galay_mongo_uri_destroy(uri);

    REQUIRE_STATUS(galay_mongo_uri_parse("http://localhost/app", &uri), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mongo_uri_parse("mongodb://localhost", &uri), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mongo_uri_parse("mongodb://localhost/", &uri), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mongo_uri_parse("mongodb://localhost:99999/app", &uri),
                   GALAY_INVALID_ARGUMENT);
    return 0;
}

static int test_client_lifecycle_without_server(void)
{
    galay_mongo_client_t* client = NULL;

    REQUIRE_STATUS(galay_mongo_client_create(&client), GALAY_OK);
    REQUIRE_TRUE(galay_mongo_client_is_connected(client) == GALAY_FALSE);
    REQUIRE_STATUS(galay_mongo_client_ping(client, "admin"), GALAY_INVALID_ARGUMENT);
    galay_mongo_client_close(client);
    galay_mongo_client_destroy(client);
    return 0;
}

int main(void)
{
    if (test_bson_builder_reader_round_trip() != 0) {
        return 1;
    }
    if (test_bson_boundaries() != 0) {
        return 1;
    }
    if (test_uri_parse_boundaries() != 0) {
        return 1;
    }
    if (test_client_lifecycle_without_server() != 0) {
        return 1;
    }
    return 0;
}
