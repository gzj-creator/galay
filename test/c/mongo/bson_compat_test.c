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

static int test_real_bson_scalar_encoding(void)
{
    static const uint8_t expected[] = {
        0x2e, 0x00, 0x00, 0x00,
        0x10, 'a', 'n', 's', 'w', 'e', 'r', 0x00, 0x2a, 0x00, 0x00, 0x00,
        0x02, 'n', 'a', 'm', 'e', 0x00, 0x06, 0x00, 0x00, 0x00,
              'g', 'a', 'l', 'a', 'y', 0x00,
        0x08, 'f', 'l', 'a', 'g', 0x00, 0x01,
        0x0a, 'n', 'o', 'n', 'e', 0x00,
        0x00
    };

    galay_mongo_document_t* document = NULL;
    galay_mongo_document_t* decoded = NULL;
    const uint8_t* bson = NULL;
    size_t bson_len = 0;
    int32_t answer = 0;
    const char* name = NULL;
    size_t name_len = 0;
    galay_bool_t flag = GALAY_FALSE;

    REQUIRE_STATUS(galay_mongo_document_create(&document), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_int32(document, "answer", 42), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_string(document, "name", "galay", 5), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_bool(document, "flag", GALAY_TRUE), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_null(document, "none"), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_encode(document, &bson, &bson_len), GALAY_OK);
    REQUIRE_TRUE(bson != NULL);
    REQUIRE_TRUE(bson_len == sizeof(expected));
    REQUIRE_TRUE(memcmp(bson, expected, sizeof(expected)) == 0);

    REQUIRE_STATUS(galay_mongo_document_decode(expected, sizeof(expected), &decoded), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_int32(decoded, "answer", &answer), GALAY_OK);
    REQUIRE_TRUE(answer == 42);
    REQUIRE_STATUS(galay_mongo_document_get_string(decoded, "name", &name, &name_len), GALAY_OK);
    REQUIRE_TRUE(name_len == 5);
    REQUIRE_TRUE(memcmp(name, "galay", name_len) == 0);
    REQUIRE_STATUS(galay_mongo_document_get_bool(decoded, "flag", &flag), GALAY_OK);
    REQUIRE_TRUE(flag == GALAY_TRUE);
    REQUIRE_STATUS(galay_mongo_document_is_null(decoded, "none"), GALAY_OK);

    galay_mongo_document_destroy(decoded);
    galay_mongo_document_destroy(document);
    return 0;
}

static int test_nested_array_and_special_bson_types(void)
{
    static const uint8_t blob[] = {0xde, 0xad, 0xbe, 0xef};
    galay_mongo_document_t* document = NULL;
    galay_mongo_document_t* nested = NULL;
    galay_mongo_document_t* decoded = NULL;
    galay_mongo_document_t* decoded_nested = NULL;
    galay_mongo_array_t* array = NULL;
    galay_mongo_array_t* decoded_array = NULL;
    const uint8_t* bson = NULL;
    const uint8_t* decoded_blob = NULL;
    size_t bson_len = 0;
    size_t decoded_blob_len = 0;
    int32_t level = 0;
    int32_t first = 0;
    const char* second = NULL;
    size_t second_len = 0;
    const char* oid = NULL;
    size_t oid_len = 0;
    int64_t when = 0;
    uint64_t ts = 0;

    REQUIRE_STATUS(galay_mongo_document_create(&document), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_create(&nested), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_array_create(&array), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_int32(nested, "level", 7), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_array_append_int32(array, 1), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_array_append_string(array, "two", 3), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_document(document, "meta", nested), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_array(document, "list", array), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_binary(document, "bin", blob, sizeof(blob)), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_object_id(document, "oid",
                                                        "00112233445566778899aabb"), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_date_time(document, "when", 1234567890LL), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_timestamp(document, "ts",
                                                        0x0102030405060708ULL), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_encode(document, &bson, &bson_len), GALAY_OK);
    REQUIRE_TRUE(bson != NULL);
    REQUIRE_TRUE(bson_len > 5);
    REQUIRE_TRUE(bson[0] != 'G');

    REQUIRE_STATUS(galay_mongo_document_decode(bson, bson_len, &decoded), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_document(decoded, "meta", &decoded_nested), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_int32(decoded_nested, "level", &level), GALAY_OK);
    REQUIRE_TRUE(level == 7);
    REQUIRE_STATUS(galay_mongo_document_get_array(decoded, "list", &decoded_array), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_array_get_int32(decoded_array, 0, &first), GALAY_OK);
    REQUIRE_TRUE(first == 1);
    REQUIRE_STATUS(galay_mongo_array_get_string(decoded_array, 1, &second, &second_len), GALAY_OK);
    REQUIRE_TRUE(second_len == 3);
    REQUIRE_TRUE(memcmp(second, "two", second_len) == 0);
    REQUIRE_STATUS(galay_mongo_document_get_binary(decoded, "bin", &decoded_blob,
                                                   &decoded_blob_len), GALAY_OK);
    REQUIRE_TRUE(decoded_blob_len == sizeof(blob));
    REQUIRE_TRUE(memcmp(decoded_blob, blob, sizeof(blob)) == 0);
    REQUIRE_STATUS(galay_mongo_document_get_object_id(decoded, "oid", &oid, &oid_len), GALAY_OK);
    REQUIRE_TRUE(oid_len == 24);
    REQUIRE_TRUE(memcmp(oid, "00112233445566778899aabb", oid_len) == 0);
    REQUIRE_STATUS(galay_mongo_document_get_date_time(decoded, "when", &when), GALAY_OK);
    REQUIRE_TRUE(when == 1234567890LL);
    REQUIRE_STATUS(galay_mongo_document_get_timestamp(decoded, "ts", &ts), GALAY_OK);
    REQUIRE_TRUE(ts == 0x0102030405060708ULL);

    galay_mongo_array_destroy(decoded_array);
    galay_mongo_document_destroy(decoded_nested);
    galay_mongo_document_destroy(decoded);
    galay_mongo_array_destroy(array);
    galay_mongo_document_destroy(nested);
    galay_mongo_document_destroy(document);
    return 0;
}

int main(void)
{
    if (test_real_bson_scalar_encoding() != 0) {
        return 1;
    }
    if (test_nested_array_and_special_bson_types() != 0) {
        return 1;
    }
    return 0;
}
