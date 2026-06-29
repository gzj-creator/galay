#include <galay/c/galay-mongo-c/mongo.h>

#include <stdio.h>

static int print_encoded_size(const char* label, galay_mongo_document_t* command)
{
    const uint8_t* bson = NULL;
    size_t bson_len = 0;

    if (galay_mongo_document_encode(command, &bson, &bson_len) != GALAY_OK) {
        return 1;
    }
    return printf("%s BSON bytes=%zu\n", label, bson_len) < 0 ? 1 : 0;
}

int main(void)
{
    galay_mongo_document_t* filter = NULL;
    galay_mongo_document_t* projection = NULL;
    galay_mongo_document_t* document = NULL;
    galay_mongo_document_t* update = NULL;
    galay_mongo_document_t* find_cmd = NULL;
    galay_mongo_document_t* insert_cmd = NULL;
    galay_mongo_document_t* update_cmd = NULL;
    galay_mongo_document_t* delete_cmd = NULL;
    int exit_code = 0;

    if (galay_mongo_document_create(&filter) != GALAY_OK ||
        galay_mongo_document_create(&projection) != GALAY_OK ||
        galay_mongo_document_create(&document) != GALAY_OK ||
        galay_mongo_document_create(&update) != GALAY_OK ||
        galay_mongo_document_append_string(filter, "name", "ada", 3) != GALAY_OK ||
        galay_mongo_document_append_int32(projection, "_id", 0) != GALAY_OK ||
        galay_mongo_document_append_string(document, "name", "ada", 3) != GALAY_OK ||
        galay_mongo_document_append_int32(update, "age", 37) != GALAY_OK ||
        galay_mongo_command_find_one("app", "users", filter, projection, &find_cmd) != GALAY_OK ||
        galay_mongo_command_insert_one("app", "users", document, &insert_cmd) != GALAY_OK ||
        galay_mongo_command_update_one("app", "users", filter, update, GALAY_TRUE,
                                       &update_cmd) != GALAY_OK ||
        galay_mongo_command_delete_one("app", "users", filter, &delete_cmd) != GALAY_OK) {
        exit_code = 1;
        goto cleanup;
    }

    if (print_encoded_size("findOne", find_cmd) != 0 ||
        print_encoded_size("insertOne", insert_cmd) != 0 ||
        print_encoded_size("updateOne", update_cmd) != 0 ||
        print_encoded_size("deleteOne", delete_cmd) != 0) {
        exit_code = 2;
    }

cleanup:
    galay_mongo_document_destroy(delete_cmd);
    galay_mongo_document_destroy(update_cmd);
    galay_mongo_document_destroy(insert_cmd);
    galay_mongo_document_destroy(find_cmd);
    galay_mongo_document_destroy(update);
    galay_mongo_document_destroy(document);
    galay_mongo_document_destroy(projection);
    galay_mongo_document_destroy(filter);
    return exit_code;
}
