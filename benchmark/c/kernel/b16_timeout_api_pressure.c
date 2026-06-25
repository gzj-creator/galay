#include <galay/c/galay-kernel-c/async-c/async_file_c.h>
#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct TcpAcceptState {
    atomic_int done;
    atomic_int code;
} TcpAcceptState;

typedef struct UdpRecvState {
    atomic_int done;
    atomic_int code;
} UdpRecvState;

typedef struct WatchState {
    atomic_int done;
    atomic_int code;
} WatchState;

typedef struct AsyncFileIoState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
} AsyncFileIoState;

typedef struct AsyncFileCloseState {
    atomic_int done;
    atomic_int code;
} AsyncFileCloseState;

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

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void on_tcp_accept(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    TcpAcceptState* state = (TcpAcceptState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_TcpSocketIOFailed : (int)result->code);
    if (result != 0 && result->code == C_TcpSocketSuccess && result->socket.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&result->socket);
    }
    atomic_store(&state->done, 1);
}

static void on_udp_recv(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    UdpRecvState* state = (UdpRecvState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->done, 1);
}

static void on_watch(galay_kernel_file_watcher_watch_result_t* result, void* ctx)
{
    WatchState* state = (WatchState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_FileWatcherIOFailed : (int)result->code);
    atomic_store(&state->done, 1);
}

static void on_async_read(galay_kernel_async_file_read_result_t* result, void* ctx)
{
    AsyncFileIoState* state = (AsyncFileIoState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_async_write(galay_kernel_async_file_write_result_t* result, void* ctx)
{
    AsyncFileIoState* state = (AsyncFileIoState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_async_close(C_AsyncFileResultCode code, void* ctx)
{
    AsyncFileCloseState* state = (AsyncFileCloseState*)ctx;
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

static int close_async_file(galay_kernel_runtime_t* runtime, galay_kernel_async_file_t* file)
{
    if (file->file == 0) {
        return 0;
    }

    AsyncFileCloseState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_AsyncFileIOFailed);
    if (galay_kernel_async_file_close_timeout(runtime, file, 1000, on_async_close, &state) != C_AsyncFileSuccess) {
        return 1;
    }
    return wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_AsyncFileSuccess;
}

int main(int argc, char** argv)
{
    int iterations = 32;
    if (argc > 1) {
        const int parsed = atoi(argv[1]);
        if (parsed > 0) {
            iterations = parsed;
        }
    }

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
    char watch_path[] = "/tmp/galay_timeout_pressure_watch_XXXXXX";
    char async_path[64] = {0};
    int watch_fd = -1;
    int watch_descriptor = -1;
    int watcher_supported = 0;
    int async_supported = 0;
    int failures = 0;
    int tcp_completed = 0;
    int udp_completed = 0;
    int watch_completed = 0;
    int async_completed = 0;
    const char payload[] = "timeout pressure";
    const size_t payload_size = sizeof(payload) - 1;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_tcp_socket_create(&listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(&listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&listener, 64) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_local_endpoint(&listener, &tcp_local) != C_TcpSocketSuccess ||
        tcp_local.port == 0 ||
        galay_kernel_udp_socket_create(&udp, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&udp, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&udp, &udp_local) != C_UdpSocketSuccess ||
        udp_local.port == 0) {
        failures = 1;
        goto cleanup;
    }

    watch_fd = mkstemp(watch_path);
    if (watch_fd >= 0) {
        close(watch_fd);
        watch_fd = -1;
        const C_FileWatcherResultCode watcher_created = galay_kernel_file_watcher_create(&watcher);
        if (watcher_created == C_FileWatcherSuccess &&
            galay_kernel_file_watcher_add_watch(&watcher, watch_path, C_FileWatchEventModify, &watch_descriptor) ==
                C_FileWatcherSuccess) {
            watcher_supported = 1;
        } else if (watcher_created != C_FileWatcherOperationUnsupported) {
            failures = 1;
            goto cleanup;
        }
    }

    if (make_temp_path(async_path, sizeof(async_path), "galay_timeout_pressure_async") != 0) {
        failures = 1;
        goto cleanup;
    }

    const C_AsyncFileResultCode async_created = galay_kernel_async_file_create(&async_file);
    if (async_created == C_AsyncFileSuccess) {
        AsyncFileIoState write_state;
        atomic_init(&write_state.done, 0);
        atomic_init(&write_state.code, (int)C_AsyncFileIOFailed);
        atomic_init(&write_state.bytes, -1);
        if (galay_kernel_async_file_open(&async_file, async_path, C_AsyncFileOpenModeReadWrite, 0600) !=
                C_AsyncFileSuccess ||
            galay_kernel_async_file_write_timeout(&runtime, &async_file, payload, payload_size, 0, 1000,
                on_async_write, &write_state) != C_AsyncFileSuccess ||
            wait_done(&write_state.done) != 0 ||
            atomic_load(&write_state.code) != (int)C_AsyncFileSuccess ||
            atomic_load(&write_state.bytes) != (int)payload_size ||
            galay_kernel_async_file_sync(&async_file) != C_AsyncFileSuccess) {
            failures = 1;
            goto cleanup;
        }
        async_supported = 1;
    } else if (async_created != C_AsyncFileOperationUnsupported) {
        failures = 1;
        goto cleanup;
    }

    const int64_t start_us = now_us();
    for (int i = 0; i < iterations; ++i) {
        TcpAcceptState tcp_state;
        atomic_init(&tcp_state.done, 0);
        atomic_init(&tcp_state.code, (int)C_TcpSocketSuccess);
        if (galay_kernel_tcp_socket_accept_timeout(&runtime, &listener, 0, on_tcp_accept, &tcp_state) !=
                C_TcpSocketSuccess ||
            wait_done(&tcp_state.done) != 0 ||
            atomic_load(&tcp_state.code) != (int)C_TcpSocketTimeout) {
            ++failures;
            break;
        }
        ++tcp_completed;

        char udp_buffer[8] = {0};
        UdpRecvState udp_state;
        atomic_init(&udp_state.done, 0);
        atomic_init(&udp_state.code, (int)C_UdpSocketSuccess);
        if (galay_kernel_udp_socket_recvfrom_timeout(&runtime, &udp, udp_buffer, sizeof(udp_buffer), 0,
                on_udp_recv, &udp_state) != C_UdpSocketSuccess ||
            wait_done(&udp_state.done) != 0 ||
            atomic_load(&udp_state.code) != (int)C_UdpSocketTimeout) {
            ++failures;
            break;
        }
        ++udp_completed;

        if (watcher_supported) {
            WatchState watch_state;
            atomic_init(&watch_state.done, 0);
            atomic_init(&watch_state.code, (int)C_FileWatcherSuccess);
            if (galay_kernel_file_watcher_watch_timeout(&runtime, &watcher, 1, on_watch, &watch_state) !=
                    C_FileWatcherSuccess ||
                wait_done(&watch_state.done) != 0 ||
                atomic_load(&watch_state.code) != (int)C_FileWatcherTimeout) {
                ++failures;
                break;
            }
            ++watch_completed;
        }

        if (async_supported) {
            char read_buffer[32] = {0};
            AsyncFileIoState read_state;
            atomic_init(&read_state.done, 0);
            atomic_init(&read_state.code, (int)C_AsyncFileIOFailed);
            atomic_init(&read_state.bytes, -1);
            if (galay_kernel_async_file_read_timeout(&runtime, &async_file, read_buffer, sizeof(read_buffer), 0, 1000,
                    on_async_read, &read_state) != C_AsyncFileSuccess ||
                wait_done(&read_state.done) != 0 ||
                atomic_load(&read_state.code) != (int)C_AsyncFileSuccess ||
                atomic_load(&read_state.bytes) != (int)payload_size ||
                memcmp(read_buffer, payload, payload_size) != 0) {
                ++failures;
                break;
            }
            ++async_completed;
        }
    }
    const int64_t elapsed_us = now_us() - start_us;

    printf("timeout_api_pressure iterations=%d failures=%d tcp=%d udp=%d file_watcher=%d%s async_file=%d%s elapsed_us=%lld\n",
           iterations,
           failures,
           tcp_completed,
           udp_completed,
           watch_completed,
           watcher_supported ? "" : "(skipped)",
           async_completed,
           async_supported ? "" : "(skipped)",
           (long long)elapsed_us);

cleanup:
    if (async_file.file != 0) {
        (void)close_async_file(&runtime, &async_file);
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
    return failures == 0 ? 0 : 1;
}
