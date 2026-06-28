#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef GALAY_SOURCE_DIR
#error "GALAY_SOURCE_DIR must point to the repository root"
#endif

enum { kMaxPath = 4096 };

static int join_path(char* out, size_t out_size, const char* left, const char* right)
{
    const int written = snprintf(out, out_size, "%s/%s", left, right);
    if (written <= 0 || (size_t)written >= out_size) {
        fprintf(stderr, "[T22] path too long: %s/%s\n", left, right);
        return 0;
    }
    return 1;
}

static int read_file(const char* path, char** out_data, size_t* out_len)
{
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "[T22] failed to open %s: %s\n", path, strerror(errno));
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "[T22] failed to seek %s\n", path);
        fclose(file);
        return 0;
    }

    const long size = ftell(file);
    if (size < 0) {
        fprintf(stderr, "[T22] failed to size %s\n", path);
        fclose(file);
        return 0;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[T22] failed to rewind %s\n", path);
        fclose(file);
        return 0;
    }

    char* data = (char*)malloc((size_t)size + 1);
    if (data == NULL) {
        fprintf(stderr, "[T22] failed to allocate %ld bytes for %s\n", size, path);
        fclose(file);
        return 0;
    }

    const size_t wanted = (size_t)size;
    const size_t actual = fread(data, 1, wanted, file);
    fclose(file);
    if (actual != wanted) {
        fprintf(stderr, "[T22] failed to read %s\n", path);
        free(data);
        return 0;
    }

    data[wanted] = '\0';
    *out_data = data;
    *out_len = wanted;
    return 1;
}

static int contains_text(const char* text, size_t text_len, const char* needle)
{
    const size_t needle_len = strlen(needle);
    if (needle_len == 0 || text_len < needle_len) {
        return 0;
    }

    for (size_t i = 0; i + needle_len <= text_len; ++i) {
        if (memcmp(text + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int has_suffix(const char* path, const char* suffix)
{
    const size_t path_len = strlen(path);
    const size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

static int is_source_file(const char* path)
{
    return has_suffix(path, ".c") ||
           has_suffix(path, ".cc") ||
           has_suffix(path, ".cpp") ||
           has_suffix(path, ".cxx") ||
           has_suffix(path, ".h") ||
           has_suffix(path, ".hh") ||
           has_suffix(path, ".hpp") ||
           has_suffix(path, ".hxx") ||
           has_suffix(path, ".inl");
}

static const char* basename_ptr(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static int is_coro_boundary_path(const char* relative_path)
{
    return strstr(relative_path, "/coro-c/") != NULL ||
           strstr(basename_ptr(relative_path), "coro") != NULL;
}

static int is_directory(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

static int is_regular_file(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode);
}

static int scan_coro_file(const char* full_path, const char* relative_path)
{
    char* data = NULL;
    size_t len = 0;
    if (!read_file(full_path, &data, &len)) {
        return 1;
    }

    int failed = 0;
    if (contains_text(data, len, "runtime->spawn(")) {
        fprintf(stderr, "[T22] %s must not bridge C coroutines through runtime->spawn(\n", relative_path);
        failed = 1;
    }
    if (contains_text(data, len, "Task<void> c_api_")) {
        fprintf(stderr, "[T22] %s must not implement C coroutine APIs as Task<void> c_api_ wrappers\n", relative_path);
        failed = 1;
    }

    free(data);
    return failed;
}

static int scan_tree(const char* relative_dir, int* scanned_files)
{
    char full_dir[kMaxPath];
    if (!join_path(full_dir, sizeof(full_dir), GALAY_SOURCE_DIR, relative_dir)) {
        return 1;
    }

    DIR* dir = opendir(full_dir);
    if (dir == NULL) {
        fprintf(stderr, "[T22] failed to open directory %s: %s\n", relative_dir, strerror(errno));
        return 1;
    }

    int failed = 0;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                fprintf(stderr, "[T22] failed to read directory %s: %s\n", relative_dir, strerror(errno));
                failed = 1;
            }
            break;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_relative[kMaxPath];
        char child_full[kMaxPath];
        if (!join_path(child_relative, sizeof(child_relative), relative_dir, entry->d_name) ||
            !join_path(child_full, sizeof(child_full), GALAY_SOURCE_DIR, child_relative)) {
            failed = 1;
            continue;
        }

        if (is_directory(child_full)) {
            failed |= scan_tree(child_relative, scanned_files);
            continue;
        }

        if (!is_regular_file(child_full) || !is_source_file(child_relative)) {
            continue;
        }

        if (!is_coro_boundary_path(child_relative)) {
            continue;
        }

        ++(*scanned_files);
        failed |= scan_coro_file(child_full, child_relative);
    }

    closedir(dir);
    return failed;
}

static int require_legacy_callback_bridge(void)
{
    const char* relative_path = "src/c/galay-kernel-c/async-c/tcp_socket_c.cc";
    char full_path[kMaxPath];
    if (!join_path(full_path, sizeof(full_path), GALAY_SOURCE_DIR, relative_path)) {
        return 1;
    }

    char* data = NULL;
    size_t len = 0;
    if (!read_file(full_path, &data, &len)) {
        return 1;
    }

    int failed = 0;
    if (!contains_text(data, len, "runtime->spawn(")) {
        fprintf(stderr, "[T22] legacy callback TCP bridge no longer exposes runtime->spawn( detector input\n");
        failed = 1;
    }
    if (!contains_text(data, len, "Task<void> c_api_")) {
        fprintf(stderr, "[T22] legacy callback TCP bridge no longer exposes Task<void> c_api_ detector input\n");
        failed = 1;
    }

    free(data);
    return failed;
}

static int require_direct_tcp_c_api_uses_core_bridge(void)
{
    const char* relative_path = "src/c/galay-kernel-c/async-c/tcp_socket_coro_c.cc";
    char full_path[kMaxPath];
    if (!join_path(full_path, sizeof(full_path), GALAY_SOURCE_DIR, relative_path)) {
        return 1;
    }

    char* data = NULL;
    size_t len = 0;
    if (!read_file(full_path, &data, &len)) {
        return 1;
    }

    int failed = 0;
    const char* required[] = {
        "galay_core_coro_tcp_accept",
        "galay_core_coro_tcp_connect",
        "galay_core_coro_tcp_recv",
        "galay_core_coro_tcp_send",
        "galay_core_coro_tcp_close",
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!contains_text(data, len, required[i])) {
            fprintf(stderr, "[T22] direct TCP C API must call core bridge %s\n", required[i]);
            failed = 1;
        }
    }

    const char* forbidden[] = {
        "AcceptAwaitable",
        "ConnectAwaitable",
        "RecvAwaitable",
        "SendAwaitable",
        "IOController",
        "IOScheduler",
        "registerIOSchedulerEvent",
        "registerIOSchedulerClose",
        "m_awaitable",
        "m_sequence_owner",
        "m_owner_scheduler",
        "m_accept_result_assigned",
        "m_recv_result_assigned",
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        if (contains_text(data, len, forbidden[i])) {
            fprintf(stderr,
                    "[T22] direct TCP C API must not directly depend on C++ private symbol %s; use the core C bridge instead\n",
                    forbidden[i]);
            failed = 1;
        }
    }

    free(data);
    return failed;
}

static int require_direct_tcp_iouring_result_flag_reset(void)
{
    const char* relative_path = "src/cpp/galay-kernel/core/c_coro_tcp_bridge.cc";
    char full_path[kMaxPath];
    if (!join_path(full_path, sizeof(full_path), GALAY_SOURCE_DIR, relative_path)) {
        return 1;
    }

    char* data = NULL;
    size_t len = 0;
    if (!read_file(full_path, &data, &len)) {
        return 1;
    }

    int failed = 0;
    if (!contains_text(data, len, "m_controller->m_accept_result_assigned = false;")) {
        fprintf(stderr, "[T22] direct TCP accept must reset io_uring accept result assignment on C result consumption\n");
        failed = 1;
    }
    if (!contains_text(data, len, "m_controller->m_recv_result_assigned = false;")) {
        fprintf(stderr, "[T22] direct TCP recv must reset io_uring recv result assignment on C result consumption\n");
        failed = 1;
    }

    free(data);
    return failed;
}

static int require_direct_tcp_timeout_arbitration(void)
{
    const char* relative_path = "src/cpp/galay-kernel/core/c_coro_tcp_bridge.cc";
    char full_path[kMaxPath];
    if (!join_path(full_path, sizeof(full_path), GALAY_SOURCE_DIR, relative_path)) {
        return 1;
    }

    char* data = NULL;
    size_t len = 0;
    if (!read_file(full_path, &data, &len)) {
        return 1;
    }

    int failed = 0;
    if (!contains_text(data,
                       len,
                       "expected == CoroTcpCompletionPhase::TimedOut ||\n"
                       "                expected == CoroTcpCompletionPhase::Cancelled) {\n"
                       "                return false;\n"
                       "            }")) {
        fprintf(stderr,
                "[T22] direct TCP guarded completion must reject CQE delivery after timeout/cancel\n");
        failed = 1;
    }

    free(data);
    return failed;
}

static int require_iouring_accept_uses_direct_completion_arbitration(void)
{
    const char* relative_path = "src/cpp/galay-kernel/core/uring_reactor.cc";
    char full_path[kMaxPath];
    if (!join_path(full_path, sizeof(full_path), GALAY_SOURCE_DIR, relative_path)) {
        return 1;
    }

    char* data = NULL;
    size_t len = 0;
    if (!read_file(full_path, &data, &len)) {
        return 1;
    }

    int failed = 0;
    if (!contains_text(data, len, "awaitable->handleComplete(cqe, controller->m_handle)")) {
        fprintf(stderr,
                "[T22] io_uring accept completion must pass through awaitable arbitration before handing off an accepted fd\n");
        failed = 1;
    }
    if (!contains_text(data,
                       len,
                       "if (awaitable->handleComplete(cqe, controller->m_handle)) {\n"
                       "                controller->enqueueAcceptedHandle(*result);\n"
                       "                if (controller->tryConsumeAcceptedHandle(awaitable->m_host, awaitable->m_result))")) {
        fprintf(stderr,
                "[T22] io_uring accept completion must still use controller accept delivery so C++ awaitables keep peer host semantics\n");
        failed = 1;
    }
    if (!contains_text(data, len, "closeUndeliveredAcceptedHandle")) {
        fprintf(stderr,
                "[T22] io_uring accept completion must close an accepted fd when a timed-out/cancelled direct waiter rejects delivery\n");
        failed = 1;
    }

    free(data);
    return failed;
}

int main(void)
{
    int failed = require_legacy_callback_bridge();
    failed |= require_direct_tcp_c_api_uses_core_bridge();
    failed |= require_direct_tcp_iouring_result_flag_reset();
    failed |= require_direct_tcp_timeout_arbitration();
    failed |= require_iouring_accept_uses_direct_completion_arbitration();

    int scanned_files = 0;
    failed |= scan_tree("src/c/galay-kernel-c", &scanned_files);

    if (failed) {
        return 1;
    }

    if (scanned_files == 0) {
        printf("T22-CoroSourceBoundaries SKIP; no future C coroutine source files found under src/c/galay-kernel-c, so bridge boundary checks are not active yet\n");
        return 0;
    }

    printf("T22-CoroSourceBoundaries PASS; checked %d future C coroutine source file(s)\n", scanned_files);
    return 0;
}
