#include <galay/c/galay-mongo/mongo.h>

#include <stdio.h>

int main(void)
{
    for (int i = 0; i < 1000; ++i) {
        galay_mongo_document_t* document = NULL;
        galay_mongo_document_t* decoded = NULL;
        const uint8_t* bson = NULL;
        size_t bson_len = 0;

        if (galay_mongo_document_create(&document) != GALAY_OK) {
            return 1;
        }
        if (galay_mongo_document_append_int32(document, "iteration", i) != GALAY_OK ||
            galay_mongo_document_append_string(document, "module", "mongo", 5) != GALAY_OK ||
            galay_mongo_document_encode(document, &bson, &bson_len) != GALAY_OK ||
            galay_mongo_document_decode(bson, bson_len, &decoded) != GALAY_OK) {
            galay_mongo_document_destroy(decoded);
            galay_mongo_document_destroy(document);
            return 1;
        }

        galay_mongo_document_destroy(decoded);
        galay_mongo_document_destroy(document);
    }

    puts("c mongo bson codec smoke: 1000 iterations");
    return 0;
}
