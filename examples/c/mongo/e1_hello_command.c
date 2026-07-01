#include <galay/c/galay-mongo-c/mongo_c.h>

#include <stdio.h>

int main(void)
{
    galay_mongo_document_t* hello = NULL;
    const uint8_t* bson = NULL;
    size_t bson_len = 0;
    int exit_code = 0;

    if (galay_mongo_document_create(&hello) != GALAY_OK ||
        galay_mongo_document_append_int32(hello, "hello", 1) != GALAY_OK ||
        galay_mongo_document_append_bool(hello, "helloOk", GALAY_TRUE) != GALAY_OK ||
        galay_mongo_document_append_string(hello, "$db", "admin", 5) != GALAY_OK ||
        galay_mongo_document_encode(hello, &bson, &bson_len) != GALAY_OK) {
        exit_code = 1;
        goto cleanup;
    }

    if (printf("mongo hello BSON bytes=%zu\n", bson_len) < 0) {
        exit_code = 2;
    }

cleanup:
    galay_mongo_document_destroy(hello);
    return exit_code;
}
