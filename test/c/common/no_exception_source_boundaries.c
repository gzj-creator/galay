#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_identifier_char(unsigned char ch)
{
    return isalnum(ch) || ch == '_';
}

static int contains_token(const char* text, size_t len, const char* token)
{
    const size_t token_len = strlen(token);
    if (token_len == 0 || len < token_len) {
        return 0;
    }

    for (size_t i = 0; i + token_len <= len; ++i) {
        if (memcmp(text + i, token, token_len) != 0) {
            continue;
        }

        const int left_ok = i == 0 || !is_identifier_char((unsigned char)text[i - 1]);
        const size_t right = i + token_len;
        const int right_ok = right == len || !is_identifier_char((unsigned char)text[right]);
        if (left_ok && right_ok) {
            return 1;
        }
    }

    return 0;
}

static int read_file(const char* path, char** out_data, size_t* out_len)
{
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "failed to open %s\n", path);
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "failed to seek %s\n", path);
        fclose(file);
        return 0;
    }

    const long size = ftell(file);
    if (size < 0) {
        fprintf(stderr, "failed to size %s\n", path);
        fclose(file);
        return 0;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "failed to rewind %s\n", path);
        fclose(file);
        return 0;
    }

    char* data = (char*)malloc((size_t)size + 1);
    if (data == NULL) {
        fprintf(stderr, "failed to allocate %ld bytes for %s\n", size, path);
        fclose(file);
        return 0;
    }

    const size_t wanted = (size_t)size;
    const size_t actual = fread(data, 1, wanted, file);
    fclose(file);
    if (actual != wanted) {
        fprintf(stderr, "failed to read %s\n", path);
        free(data);
        return 0;
    }

    data[wanted] = '\0';
    *out_data = data;
    *out_len = wanted;
    return 1;
}

static int scan_file(const char* relative_path)
{
    char path[4096];
    const int written = snprintf(path, sizeof(path), "%s/%s", GALAY_SOURCE_DIR, relative_path);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        fprintf(stderr, "source path too long: %s\n", relative_path);
        return 1;
    }

    char* data = NULL;
    size_t len = 0;
    if (!read_file(path, &data, &len)) {
        return 1;
    }

    int failed = 0;
    const char* tokens[] = {"throw", "try", "catch"};
    for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
        if (contains_token(data, len, tokens[i])) {
            fprintf(stderr, "%s contains forbidden exception token: %s\n", relative_path, tokens[i]);
            failed = 1;
        }
    }

    free(data);
    return failed;
}

int main(void)
{
    const char* files[] = {
        "src/c/galay-mysql/mysql.cc",
        "src/c/galay-mongo/mongo.cc",
        "src/c/galay-tracing/tracing.cc",
        "src/c/galay-http2/http2.cc",
        "src/c/galay-redis/redis.cc",
        "src/c/galay-etcd/etcd.cc",
        "src/cpp/galay-utils/encoding/base64.hpp",
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        failed |= scan_file(files[i]);
    }

    if (failed) {
        return 1;
    }

    puts("C common no-exception source boundaries PASS");
    return 0;
}
