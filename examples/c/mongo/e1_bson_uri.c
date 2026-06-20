#include <galay/c/galay-mongo/mongo.h>

#include <stdio.h>

int main(void)
{
    galay_mongo_document_t* document = NULL;
    galay_mongo_uri_t* uri = NULL;
    const uint8_t* bson = NULL;
    size_t bson_len = 0;
    const char* database = NULL;
    size_t database_len = 0;

    if (galay_mongo_document_create(&document) != GALAY_OK) {
        return 1;
    }
    if (galay_mongo_document_append_int32(document, "ping", 1) != GALAY_OK ||
        galay_mongo_document_encode(document, &bson, &bson_len) != GALAY_OK) {
        galay_mongo_document_destroy(document);
        return 1;
    }

    if (galay_mongo_uri_parse("mongodb://localhost:27017/admin", &uri) != GALAY_OK ||
        galay_mongo_uri_database(uri, &database, &database_len) != GALAY_OK) {
        galay_mongo_uri_destroy(uri);
        galay_mongo_document_destroy(document);
        return 1;
    }

    printf("mongo bson bytes=%zu database=%.*s\n", bson_len, (int)database_len, database);
    galay_mongo_uri_destroy(uri);
    galay_mongo_document_destroy(document);
    return 0;
}
