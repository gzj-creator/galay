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

static int test_find_one_builder(void)
{
    galay_mongo_document_t* filter = NULL;
    galay_mongo_document_t* projection = NULL;
    galay_mongo_document_t* command = NULL;
    galay_mongo_document_t* decoded_filter = NULL;
    const char* collection = NULL;
    size_t collection_len = 0;
    int32_t answer = 0;
    int32_t limit = 0;

    REQUIRE_STATUS(galay_mongo_document_create(&filter), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_create(&projection), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_int32(filter, "answer", 42), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_int32(projection, "_id", 0), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_command_find_one("app", "users", filter, projection, &command),
                   GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_string(command, "find", &collection,
                                                   &collection_len), GALAY_OK);
    REQUIRE_TRUE(collection_len == 5);
    REQUIRE_TRUE(memcmp(collection, "users", collection_len) == 0);
    REQUIRE_STATUS(galay_mongo_document_get_int32(command, "limit", &limit), GALAY_OK);
    REQUIRE_TRUE(limit == 1);
    REQUIRE_STATUS(galay_mongo_document_get_document(command, "filter", &decoded_filter), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_int32(decoded_filter, "answer", &answer), GALAY_OK);
    REQUIRE_TRUE(answer == 42);

    galay_mongo_document_destroy(decoded_filter);
    galay_mongo_document_destroy(command);
    galay_mongo_document_destroy(projection);
    galay_mongo_document_destroy(filter);
    return 0;
}

static int test_insert_update_delete_builders(void)
{
    galay_mongo_document_t* document = NULL;
    galay_mongo_document_t* filter = NULL;
    galay_mongo_document_t* set_values = NULL;
    galay_mongo_document_t* insert = NULL;
    galay_mongo_document_t* update = NULL;
    galay_mongo_document_t* delete_cmd = NULL;
    galay_mongo_array_t* documents = NULL;
    galay_mongo_array_t* updates = NULL;
    galay_mongo_array_t* deletes = NULL;
    galay_mongo_document_t* first_update = NULL;
    galay_mongo_document_t* first_delete = NULL;
    const char* collection = NULL;
    size_t collection_len = 0;
    int32_t limit = 0;
    galay_bool_t upsert = GALAY_FALSE;

    REQUIRE_STATUS(galay_mongo_document_create(&document), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_create(&filter), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_create(&set_values), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_string(document, "name", "ada", 3), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_string(filter, "name", "ada", 3), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_append_int32(set_values, "age", 37), GALAY_OK);

    REQUIRE_STATUS(galay_mongo_command_insert_one("app", "users", document, &insert), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_string(insert, "insert", &collection,
                                                   &collection_len), GALAY_OK);
    REQUIRE_TRUE(collection_len == 5);
    REQUIRE_TRUE(memcmp(collection, "users", collection_len) == 0);
    REQUIRE_STATUS(galay_mongo_document_get_array(insert, "documents", &documents), GALAY_OK);
    REQUIRE_TRUE(galay_mongo_array_size(documents) == 1);

    REQUIRE_STATUS(galay_mongo_command_update_one("app", "users", filter, set_values,
                                                  GALAY_TRUE, &update), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_array(update, "updates", &updates), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_array_get_document(updates, 0, &first_update), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_bool(first_update, "upsert", &upsert), GALAY_OK);
    REQUIRE_TRUE(upsert == GALAY_TRUE);

    REQUIRE_STATUS(galay_mongo_command_delete_one("app", "users", filter, &delete_cmd), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_array(delete_cmd, "deletes", &deletes), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_array_get_document(deletes, 0, &first_delete), GALAY_OK);
    REQUIRE_STATUS(galay_mongo_document_get_int32(first_delete, "limit", &limit), GALAY_OK);
    REQUIRE_TRUE(limit == 1);

    galay_mongo_document_destroy(first_delete);
    galay_mongo_document_destroy(first_update);
    galay_mongo_array_destroy(deletes);
    galay_mongo_array_destroy(updates);
    galay_mongo_array_destroy(documents);
    galay_mongo_document_destroy(delete_cmd);
    galay_mongo_document_destroy(update);
    galay_mongo_document_destroy(insert);
    galay_mongo_document_destroy(set_values);
    galay_mongo_document_destroy(filter);
    galay_mongo_document_destroy(document);
    return 0;
}

int main(void)
{
    if (test_find_one_builder() != 0) {
        return 1;
    }
    if (test_insert_update_delete_builders() != 0) {
        return 1;
    }
    return 0;
}
