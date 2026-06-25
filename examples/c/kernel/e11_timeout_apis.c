#include <galay/c/galay-kernel-c/async-c/async_file_c.h>
#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct CodeState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
} CodeState;

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static void reset_state(CodeState* state, int code)
{
    atomic_store(&state->done, 0);
    atomic_store(&state->code, code);
    atomic_store(&state->bytes, -1);
}

static void on_tcp_accept(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    CodeState* state = (CodeState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_TcpSocketIOFailed : (int)result->code);
    if (result != 0 && result->code == C_TcpSocketSuccess && result->socket.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&result->socket);
    }
    atomic_store(&state->done, 1);
}

static void on_udp_recv(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    CodeState* state = (CodeState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_watch(galay_kernel_file_watcher_watch_result_t* result, void* ctx)
{
    CodeState* state = (CodeState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_FileWatcherIOFailed : (int)result->code);
    atomic_store(&state->done, 1);
}

static void on_async_write(galay_kernel_async_file_write_result_t* result, void* ctx)
{
    CodeState* state = (CodeState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_async_read(galay_kernel_async_file_read_result_t* result, void* ctx)
{
    CodeState* state = (CodeState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_async_close(C_AsyncFileResultCode code, void* ctx)
{
    CodeState* state = (CodeState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

static int make_temp_path(char* path, size_t length, const char* prefix)
{
    if (snprintf(path, length, "/tmp/%s_XXXXXX", prefix) <= 0) {
        return 1;
    }
    int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    close(fd);
    unlink(path);
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
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host tcp_local = {0};
    C_Host udp_local = {0};
    char watch_path[] = "/tmp/galay-timeout-example-watch-XXXXXX";
    char async_path[64] = {0};
    int watch_fd = -1;
    int watch_descriptor = -1;
    int exit_code = 0;
    CodeState state;

    atomic_init(&state.done, 0);
    atomic_init(&state.code, 0);
    atomic_init(&state.bytes, -1);

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

    reset_state(&state, (int)C_TcpSocketSuccess);
    if (galay_kernel_tcp_socket_accept_timeout(&runtime, &listener, 5, on_tcp_accept, &state) != C_TcpSocketSuccess ||
        wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_TcpSocketTimeout) {
        exit_code = 3;
        goto cleanup;
    }
    printf("tcp accept timeout: %s\n",
           galay_kernel_tcp_socket_get_error((C_TcpSocketResultCode)atomic_load(&state.code)));

    if (galay_kernel_udp_socket_create(&udp, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&udp, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&udp, &udp_local) != C_UdpSocketSuccess) {
        exit_code = 4;
        goto cleanup;
    }

    char udp_buffer[8] = {0};
    reset_state(&state, (int)C_UdpSocketSuccess);
    if (galay_kernel_udp_socket_recvfrom_timeout(&runtime, &udp, udp_buffer, sizeof(udp_buffer), 5, on_udp_recv,
            &state) != C_UdpSocketSuccess ||
        wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_UdpSocketTimeout) {
        exit_code = 5;
        goto cleanup;
    }
    printf("udp recvfrom timeout: %s\n",
           galay_kernel_udp_socket_get_error((C_UdpSocketResultCode)atomic_load(&state.code)));

    watch_fd = mkstemp(watch_path);
    if (watch_fd >= 0) {
        close(watch_fd);
        watch_fd = -1;
        const C_FileWatcherResultCode watcher_created = galay_kernel_file_watcher_create(&watcher);
        if (watcher_created == C_FileWatcherSuccess &&
            galay_kernel_file_watcher_add_watch(&watcher, watch_path, C_FileWatchEventModify, &watch_descriptor) ==
                C_FileWatcherSuccess) {
            reset_state(&state, (int)C_FileWatcherSuccess);
            if (galay_kernel_file_watcher_watch_timeout(&runtime, &watcher, 5, on_watch, &state) !=
                    C_FileWatcherSuccess ||
                wait_done(&state.done) != 0 ||
                atomic_load(&state.code) != (int)C_FileWatcherTimeout) {
                exit_code = 6;
                goto cleanup;
            }
            printf("file watcher timeout: %s\n",
                   galay_kernel_file_watcher_get_error((C_FileWatcherResultCode)atomic_load(&state.code)));
        } else if (watcher_created == C_FileWatcherOperationUnsupported) {
            printf("file watcher timeout: unsupported backend\n");
        } else {
            exit_code = 7;
            goto cleanup;
        }
    }

    if (make_temp_path(async_path, sizeof(async_path), "galay_timeout_example_async") != 0) {
        exit_code = 8;
        goto cleanup;
    }

    const C_AsyncFileResultCode async_created = galay_kernel_async_file_create(&async_file);
    if (async_created == C_AsyncFileSuccess) {
        const char payload[] = "timeout";
        char read_buffer[16] = {0};
        reset_state(&state, (int)C_AsyncFileIOFailed);
        if (galay_kernel_async_file_open(&async_file, async_path, C_AsyncFileOpenModeReadWrite, 0600) !=
                C_AsyncFileSuccess ||
            galay_kernel_async_file_write_timeout(&runtime, &async_file, payload, sizeof(payload) - 1, 0, 1000,
                on_async_write, &state) != C_AsyncFileSuccess ||
            wait_done(&state.done) != 0 ||
            atomic_load(&state.code) != (int)C_AsyncFileSuccess ||
            atomic_load(&state.bytes) != (int)(sizeof(payload) - 1) ||
            galay_kernel_async_file_sync(&async_file) != C_AsyncFileSuccess) {
            exit_code = 9;
            goto cleanup;
        }

        reset_state(&state, (int)C_AsyncFileIOFailed);
        if (galay_kernel_async_file_read_timeout(&runtime, &async_file, read_buffer, sizeof(read_buffer), 0, 1000,
                on_async_read, &state) != C_AsyncFileSuccess ||
            wait_done(&state.done) != 0 ||
            atomic_load(&state.code) != (int)C_AsyncFileSuccess ||
            memcmp(read_buffer, payload, sizeof(payload) - 1) != 0) {
            exit_code = 10;
            goto cleanup;
        }
        printf("async file timeout API read bytes=%d\n", atomic_load(&state.bytes));
    } else if (async_created == C_AsyncFileOperationUnsupported) {
        printf("async file timeout API: unsupported backend\n");
    } else {
        exit_code = 11;
        goto cleanup;
    }

cleanup:
    if (async_file.file != 0) {
        reset_state(&state, (int)C_AsyncFileIOFailed);
        if (galay_kernel_async_file_close_timeout(&runtime, &async_file, 1000, on_async_close, &state) ==
            C_AsyncFileSuccess) {
            (void)wait_done(&state.done);
        }
        (void)galay_kernel_async_file_destroy(&async_file);
    }
    if (watcher.watcher != 0) {
        if (watch_descriptor >= 0) {
            (void)galay_kernel_file_watcher_remove_watch(&watcher, watch_descriptor);
        }
        (void)galay_kernel_file_watcher_destroy(&watcher);
    }
    if (udp.socket != 0) {
        (void)galay_kernel_udp_socket_destroy(&udp);
    }
    if (listener.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    if (watch_fd >= 0) {
        close(watch_fd);
    }
    unlink(watch_path);
    unlink(async_path);
    return exit_code;
}
