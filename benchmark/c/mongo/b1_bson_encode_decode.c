#include <galay/c/galay-mongo-c/mongo_c.h>

#include <stdio.h>
#include <time.h>

int main(void)
{
    const int iterations = 10000;
    clock_t start = clock();
    size_t total_bytes = 0;

    for (int i = 0; i < iterations; ++i) {
        galay_mongo_document_t* document = NULL;
        galay_mongo_document_t* decoded = NULL;
        const uint8_t* bson = NULL;
        size_t bson_len = 0;

        if (galay_mongo_document_create(&document) != GALAY_OK ||
            galay_mongo_document_append_int32(document, "i", i) != GALAY_OK ||
            galay_mongo_document_append_string(document, "name", "galay", 5) != GALAY_OK ||
            galay_mongo_document_append_bool(document, "ok", GALAY_TRUE) != GALAY_OK ||
            galay_mongo_document_encode(document, &bson, &bson_len) != GALAY_OK ||
            galay_mongo_document_decode(bson, bson_len, &decoded) != GALAY_OK) {
            galay_mongo_document_destroy(decoded);
            galay_mongo_document_destroy(document);
            return 1;
        }

        total_bytes += bson_len;
        galay_mongo_document_destroy(decoded);
        galay_mongo_document_destroy(document);
    }

    clock_t end = clock();
    double seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
    if (printf("mongo BSON encode/decode iterations=%d bytes=%zu seconds=%.6f\n",
               iterations,
               total_bytes,
               seconds) < 0) {
        return 2;
    }
    return 0;
}
