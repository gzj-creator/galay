#include <galay/c/galay-mongo-c/mongo.h>

#include <stdio.h>
#include <time.h>

int main(void)
{
    const int iterations = 5000;
    clock_t start = clock();
    size_t total_bytes = 0;

    for (int i = 0; i < iterations; ++i) {
        galay_mongo_document_t* filter = NULL;
        galay_mongo_document_t* command = NULL;
        galay_mongo_document_t* decoded = NULL;
        const uint8_t* bson = NULL;
        size_t bson_len = 0;

        if (galay_mongo_document_create(&filter) != GALAY_OK ||
            galay_mongo_document_append_int32(filter, "i", i) != GALAY_OK ||
            galay_mongo_command_find_one("bench", "items", filter, NULL, &command) != GALAY_OK ||
            galay_mongo_document_encode(command, &bson, &bson_len) != GALAY_OK ||
            galay_mongo_document_decode(bson, bson_len, &decoded) != GALAY_OK) {
            galay_mongo_document_destroy(decoded);
            galay_mongo_document_destroy(command);
            galay_mongo_document_destroy(filter);
            return 1;
        }

        total_bytes += bson_len;
        galay_mongo_document_destroy(decoded);
        galay_mongo_document_destroy(command);
        galay_mongo_document_destroy(filter);
    }

    clock_t end = clock();
    double seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
    if (printf("mongo command loopback iterations=%d bytes=%zu seconds=%.6f\n",
               iterations,
               total_bytes,
               seconds) < 0) {
        return 2;
    }
    return 0;
}
