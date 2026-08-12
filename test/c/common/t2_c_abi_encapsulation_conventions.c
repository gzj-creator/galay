#include <galay/c/galay-bridge-c/coro-c/c_coro_tcp_bridge.h>
#include <galay/c/galay-common-c/common/galay_c_error.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GALAY_SOURCE_DIR
#error "GALAY_SOURCE_DIR must point to the repository root"
#endif

enum { kMaxPath = 4096 };

static int read_text(const char* relative_path, char** out)
{
    char path[kMaxPath];
    const int written = snprintf(path, sizeof(path), "%s/%s", GALAY_SOURCE_DIR, relative_path);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        fprintf(stderr, "path too long: %s\n", relative_path);
        return 0;
    }

    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    const long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    char* data = (char*)malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(file);
        return 0;
    }
    const size_t wanted = (size_t)size;
    const size_t actual = fread(data, 1, wanted, file);
    fclose(file);
    if (actual != wanted) {
        free(data);
        return 0;
    }
    data[wanted] = '\0';
    *out = data;
    return 1;
}

static int require_contains(const char* relative_path, const char* needle)
{
    char* text = NULL;
    if (!read_text(relative_path, &text)) {
        return 0;
    }
    const int found = strstr(text, needle) != NULL;
    if (!found) {
        fprintf(stderr, "%s missing text: %s\n", relative_path, needle);
    }
    free(text);
    return found;
}

static int require_not_contains(const char* relative_path, const char* needle)
{
    char* text = NULL;
    if (!read_text(relative_path, &text)) {
        return 0;
    }
    const int absent = strstr(text, needle) == NULL;
    if (!absent) {
        fprintf(stderr, "%s still contains text: %s\n", relative_path, needle);
    }
    free(text);
    return absent;
}

int main(void)
{
    GalayCoreTcpSocket* tcp_socket = NULL;
    GalayCoreIOScheduler* scheduler = NULL;
    galay_iovec_t iovec = {0};
    if (tcp_socket != NULL || scheduler != NULL || iovec.base != NULL || iovec.len != 0) {
        return 1;
    }

    if (!require_contains("src/c/galay-bridge-c/coro-c/c_coro_tcp_bridge.h",
                          "GalayCoreTcpSocket* socket")) {
        return 5;
    }
    if (!require_contains("src/c/galay-bridge-c/coro-c/c_coro_tcp_bridge.h",
                          "允许调用线程")) {
        return 6;
    }
    if (!require_contains("src/c/galay-bridge-c/coro-c/c_coro_tcp_bridge.h",
                          "可重入")) {
        return 7;
    }
    if (!require_contains("src/c/galay-kernel-c/coro-c/coro_result_c.h",
                          "ABI 演进")) {
        return 8;
    }
    if (!require_contains("src/c/galay-common-c/common/galay_c_iovec.h",
                          "galay_iovec_t")) {
        return 9;
    }
    if (!require_not_contains("src/c/galay-bridge-c/coro-c/c_coro_tcp_bridge.h",
                              "#include <sys/uio.h>")) {
        return 10;
    }
    return 0;
}
