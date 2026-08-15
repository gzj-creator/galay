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
    return written > 0 && (size_t)written < out_size;
}

static int is_directory(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_regular_file(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int has_suffix(const char* path, const char* suffix)
{
    const size_t path_len = strlen(path);
    const size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

static int is_checked_file(const char* path)
{
    return has_suffix(path, ".c") || has_suffix(path, ".cc") ||
           has_suffix(path, ".cpp") || has_suffix(path, ".cxx") ||
           has_suffix(path, ".h") || has_suffix(path, ".hh") ||
           has_suffix(path, ".hpp") || has_suffix(path, "CMakeLists.txt");
}

static int read_file(const char* path, char** out_data, size_t* out_size)
{
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
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
    char* data = malloc((size_t)size + 1U);
    if (data == NULL) {
        fclose(file);
        return 0;
    }
    const size_t actual = fread(data, 1, (size_t)size, file);
    fclose(file);
    if (actual != (size_t)size) {
        free(data);
        return 0;
    }
    data[size] = '\0';
    *out_data = data;
    *out_size = (size_t)size;
    return 1;
}

static int contains_text(const char* data, size_t size, const char* needle)
{
    const size_t needle_size = strlen(needle);
    if (needle_size == 0 || size < needle_size) {
        return 0;
    }
    for (size_t index = 0; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_http2_cpp_bridge(const char* relative_path)
{
    return strncmp(relative_path, "src/c/galay-http2-c/", strlen("src/c/galay-http2-c/")) == 0;
}

static int check_source_file(const char* full_path, const char* relative_path)
{
    char* data = NULL;
    size_t size = 0;
    if (!read_file(full_path, &data, &size)) {
        fprintf(stderr, "[T22] cannot read %s: %s\n", relative_path, strerror(errno));
        return 1;
    }

    static const char* const forbidden_source_tokens[] = {
        "namespace ",
        "namespace\n",
        "galay::",
        "src/cpp/",
        "galay/cpp/",
        "extern \"C++\"",
    };
    static const char* const forbidden_cpp_targets[] = {
        "galay::utils",
        "galay::kernel",
        "galay::ssl",
        "galay::http",
        "galay::ws",
        "galay::http2",
        "galay::redis",
        "galay::etcd",
        "galay::mysql",
        "galay::postgres",
        "galay::mongo",
        "galay::rpc",
        "galay::mcp",
        "galay::tracing",
    };

    int failed = 0;
    if (!has_suffix(relative_path, "CMakeLists.txt")) {
        for (size_t index = 0;
             index < sizeof(forbidden_source_tokens) / sizeof(forbidden_source_tokens[0]);
             ++index) {
            if (is_http2_cpp_bridge(relative_path) &&
                (strcmp(forbidden_source_tokens[index], "galay::") == 0 ||
                 strcmp(forbidden_source_tokens[index], "galay/cpp/") == 0)) {
                continue;
            }
            if (contains_text(data, size, forbidden_source_tokens[index])) {
                fprintf(stderr, "[T22] C module contains C++ dependency token %s: %s\n",
                        forbidden_source_tokens[index], relative_path);
                failed = 1;
            }
        }
    }
    if (has_suffix(relative_path, "CMakeLists.txt")) {
        for (size_t index = 0;
             index < sizeof(forbidden_cpp_targets) / sizeof(forbidden_cpp_targets[0]);
             ++index) {
            if (is_http2_cpp_bridge(relative_path) &&
                (strcmp(forbidden_cpp_targets[index], "galay::http") == 0 ||
                 strcmp(forbidden_cpp_targets[index], "galay::http2") == 0)) {
                continue;
            }
            if (contains_text(data, size, forbidden_cpp_targets[index])) {
                fprintf(stderr, "[T22] C target depends on C++ target %s: %s\n",
                        forbidden_cpp_targets[index], relative_path);
                failed = 1;
            }
        }
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
    DIR* directory = opendir(full_dir);
    if (directory == NULL) {
        fprintf(stderr, "[T22] cannot open %s: %s\n", relative_dir, strerror(errno));
        return 1;
    }

    int failed = 0;
    for (;;) {
        errno = 0;
        const struct dirent* entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
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
        } else if (is_regular_file(child_full) && is_checked_file(child_relative)) {
            ++(*scanned_files);
            failed |= check_source_file(child_full, child_relative);
        }
    }
    closedir(directory);
    return failed;
}

int main(void)
{
    int scanned_files = 0;
    const int failed = scan_tree("src/c", &scanned_files);
    if (scanned_files == 0) {
        fprintf(stderr, "[T22] did not scan C module sources\n");
        return 1;
    }
    return failed ? 1 : 0;
}
