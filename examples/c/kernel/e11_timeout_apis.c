#include <galay/c/galay-kernel-c/async-c/async_file_c.h>
#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct TcpAcceptState {
    galay_kernel_tcp_socket_t* listener;
    C_IOResult result;
    galay_kernel_tcp_socket_t accepted;
} TcpAcceptState;

typedef struct UdpRecvState {
    galay_kernel_udp_socket_t* socket;
    C_IOResult result;
    char buffer[8];
    C_Host from;
} UdpRecvState;

typedef struct WatchState {
    galay_kernel_file_watcher_t* watcher;
    C_IOResult result;
    galay_kernel_file_watcher_watch_result_t watch_result;
} WatchState;

typedef struct AsyncFileState {
    galay_kernel_async_file_t* file;
    C_IOResult write_result;
    C_IOResult read_result;
    C_IOResult close_result;
    C_AsyncFileResultCode sync_result;
    char read_buffer[16];
} AsyncFileState;

static void set_cleanup_error(int* exit_code, int cleanup_code)
{
    if (*exit_code == 0) {
        *exit_code = cleanup_code;
    }
}

static int unlink_if_exists(const char* path)
{
    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    if (unlink(path) == 0) {
        return 0;
    }
    return errno == ENOENT ? 0 : 1;
}

static void tcp_accept_entry(void* ctx)
{
    TcpAcceptState* state = (TcpAcceptState*)ctx;
    state->result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, 0, 5);
}

static void udp_recv_entry(void* ctx)
{
    UdpRecvState* state = (UdpRecvState*)ctx;
    state->result =
        galay_kernel_udp_socket_recvfrom(state->socket,
                                         state->buffer,
                                         sizeof(state->buffer),
                                         &state->from,
                                         5);
}

static void watch_entry(void* ctx)
{
    WatchState* state = (WatchState*)ctx;
    state->result =
        galay_kernel_file_watcher_watch(state->watcher, &state->watch_result, 5);
}

static void async_file_entry(void* ctx)
{
    static const char payload[] = "timeout";
    AsyncFileState* state = (AsyncFileState*)ctx;
    state->write_result =
        galay_kernel_async_file_write(state->file, payload, sizeof(payload) - 1, 0, 1000);
    if (state->write_result.code != C_IOResultOk) {
        return;
    }
    state->sync_result = galay_kernel_async_file_sync(state->file);
    if (state->sync_result != C_AsyncFileSuccess) {
        return;
    }
    state->read_result =
        galay_kernel_async_file_read(state->file, state->read_buffer, sizeof(state->read_buffer), 0, 1000);
    state->close_result = galay_kernel_async_file_close(state->file, 1000);
}

static int make_temp_path(char* path, size_t length, const char* prefix)
{
    int written = snprintf(path, length, "/tmp/%s_XXXXXX", prefix);
    if (written <= 0 || (size_t)written >= length) {
        return 1;
    }
    int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    if (close(fd) != 0) {
        return 2;
    }
    if (unlink_if_exists(path) != 0) {
        return 3;
    }
    return 0;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    galay_kernel_udp_socket_t udp = {0};
    galay_kernel_file_watcher_t watcher = {0};
    galay_kernel_async_file_t async_file = {0};
    galay_coro_task_t task = {0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host tcp_local = {0};
    C_Host udp_local = {0};
    char watch_path[] = "/tmp/galay-timeout-example-watch-XXXXXX";
    char async_path[64] = {0};
    int watch_fd = -1;
    int watch_descriptor = -1;
    int watch_path_created = 0;
    int async_path_created = 0;
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return 1;
    }

    if (galay_kernel_tcp_socket_create(&listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(&listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&listener, 16) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_local_endpoint(&listener, &tcp_local) != C_TcpSocketSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    TcpAcceptState accept_state = {&listener, {0}, {0}};
    if (galay_coro_spawn(&runtime, tcp_accept_entry, &accept_state, 0, &task).code != C_IOResultOk ||
        galay_coro_join(&task, 2000).code != C_IOResultOk ||
        accept_state.result.code != C_IOResultTimeout) {
        exit_code = 3;
        goto cleanup;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk) {
        exit_code = 11;
        goto cleanup;
    }
    task.task = 0;
    if (printf("tcp accept timeout: %d\n", (int)accept_state.result.code) < 0) {
        exit_code = 27;
        goto cleanup;
    }

    if (galay_kernel_udp_socket_create(&udp, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&udp, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&udp, &udp_local) != C_UdpSocketSuccess) {
        exit_code = 4;
        goto cleanup;
    }

    UdpRecvState udp_state = {&udp, {0}, {0}, {0}};
    if (galay_coro_spawn(&runtime, udp_recv_entry, &udp_state, 0, &task).code != C_IOResultOk ||
        galay_coro_join(&task, 2000).code != C_IOResultOk ||
        udp_state.result.code != C_IOResultTimeout) {
        exit_code = 5;
        goto cleanup;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk) {
        exit_code = 12;
        goto cleanup;
    }
    task.task = 0;
    if (printf("udp recvfrom timeout: %d\n", (int)udp_state.result.code) < 0) {
        exit_code = 28;
        goto cleanup;
    }

    watch_fd = mkstemp(watch_path);
    if (watch_fd >= 0) {
        watch_path_created = 1;
        if (close(watch_fd) != 0) {
            exit_code = 13;
            goto cleanup;
        }
        watch_fd = -1;
        const C_FileWatcherResultCode watcher_created = galay_kernel_file_watcher_create(&watcher);
        if (watcher_created == C_FileWatcherSuccess &&
            galay_kernel_file_watcher_add_watch(&watcher, watch_path, C_FileWatchEventModify, &watch_descriptor) ==
                C_FileWatcherSuccess) {
            WatchState watch_state = {&watcher, {0}, {0}};
            if (galay_coro_spawn(&runtime, watch_entry, &watch_state, 0, &task).code != C_IOResultOk ||
                galay_coro_join(&task, 2000).code != C_IOResultOk ||
                watch_state.result.code != C_IOResultTimeout ||
                watch_state.watch_result.code != C_FileWatcherTimeout) {
                exit_code = 6;
                goto cleanup;
            }
            if (galay_coro_destroy(&task).code != C_IOResultOk) {
                exit_code = 14;
                goto cleanup;
            }
            task.task = 0;
            if (printf("file watcher timeout: %s\n",
                       galay_kernel_file_watcher_get_error(watch_state.watch_result.code)) < 0) {
                exit_code = 29;
                goto cleanup;
            }
        } else if (watcher_created == C_FileWatcherOperationUnsupported) {
            if (printf("file watcher timeout: unsupported backend\n") < 0) {
                exit_code = 30;
                goto cleanup;
            }
        } else {
            exit_code = 7;
            goto cleanup;
        }
    }

    if (make_temp_path(async_path, sizeof(async_path), "galay_timeout_example_async") != 0) {
        exit_code = 8;
        goto cleanup;
    }
    async_path_created = 1;

    const C_AsyncFileResultCode async_created = galay_kernel_async_file_create(&async_file);
    if (async_created == C_AsyncFileSuccess) {
        static const char payload[] = "timeout";
        AsyncFileState file_state = {&async_file, {0}, {0}, {0}, C_AsyncFileIOFailed, {0}};
        if (galay_kernel_async_file_open(&async_file, async_path, C_AsyncFileOpenModeReadWrite, 0600) !=
                C_AsyncFileSuccess ||
            galay_coro_spawn(&runtime, async_file_entry, &file_state, 0, &task).code != C_IOResultOk ||
            galay_coro_join(&task, 3000).code != C_IOResultOk ||
            file_state.write_result.code != C_IOResultOk ||
            file_state.write_result.bytes != sizeof(payload) - 1 ||
            file_state.sync_result != C_AsyncFileSuccess ||
            file_state.read_result.code != C_IOResultOk ||
            memcmp(file_state.read_buffer, payload, sizeof(payload) - 1) != 0 ||
            file_state.close_result.code != C_IOResultOk) {
            exit_code = 9;
            goto cleanup;
        }
        if (galay_coro_destroy(&task).code != C_IOResultOk) {
            exit_code = 15;
            goto cleanup;
        }
        task.task = 0;
        if (printf("async file timeout API read bytes=%zu\n", file_state.read_result.bytes) < 0) {
            exit_code = 31;
            goto cleanup;
        }
    } else if (async_created == C_AsyncFileOperationUnsupported) {
        if (printf("async file timeout API: unsupported backend\n") < 0) {
            exit_code = 32;
            goto cleanup;
        }
    } else {
        exit_code = 10;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk) {
            set_cleanup_error(&exit_code, 16);
        }
    }
    if (async_file.file != 0) {
        if (galay_kernel_async_file_destroy(&async_file) != C_AsyncFileSuccess) {
            set_cleanup_error(&exit_code, 17);
        }
    }
    if (watcher.watcher != 0) {
        if (watch_descriptor >= 0) {
            if (galay_kernel_file_watcher_remove_watch(&watcher, watch_descriptor) != C_FileWatcherSuccess) {
                set_cleanup_error(&exit_code, 18);
            }
        }
        if (galay_kernel_file_watcher_destroy(&watcher) != C_FileWatcherSuccess) {
            set_cleanup_error(&exit_code, 19);
        }
    }
    if (udp.socket != 0) {
        if (galay_kernel_udp_socket_destroy(&udp) != C_UdpSocketSuccess) {
            set_cleanup_error(&exit_code, 20);
        }
    }
    if (listener.socket != 0) {
        if (galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess) {
            set_cleanup_error(&exit_code, 21);
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess) {
            set_cleanup_error(&exit_code, 22);
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
            set_cleanup_error(&exit_code, 23);
        }
    }
    if (watch_fd >= 0) {
        if (close(watch_fd) != 0) {
            set_cleanup_error(&exit_code, 24);
        }
    }
    if (watch_path_created && unlink_if_exists(watch_path) != 0) {
        set_cleanup_error(&exit_code, 25);
    }
    if (async_path_created && unlink_if_exists(async_path) != 0) {
        set_cleanup_error(&exit_code, 26);
    }
    return exit_code;
}
