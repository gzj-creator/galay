#include <galay/c/galay-kernel-c/async-c/async_file_c.h>
#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct TcpAcceptState {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult result;
} TcpAcceptState;

typedef struct UdpRecvState {
    galay_kernel_udp_socket_t* socket;
    C_IOResult result;
    C_Host from;
    char buffer[8];
} UdpRecvState;

typedef struct WatchState {
    galay_kernel_file_watcher_t* watcher;
    galay_kernel_file_watcher_watch_result_t watch_result;
    C_IOResult result;
} WatchState;

typedef struct AsyncFileState {
    galay_kernel_async_file_t* file;
    const char* payload;
    size_t payload_size;
    char* read_buffer;
    size_t read_buffer_size;
    C_IOResult result;
} AsyncFileState;

static void tcp_accept_entry(void* arg)
{
    TcpAcceptState* state = (TcpAcceptState*)arg;
    state->result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 0);
}

static void udp_recv_entry(void* arg)
{
    UdpRecvState* state = (UdpRecvState*)arg;
    state->result = galay_kernel_udp_socket_recvfrom(
        state->socket,
        state->buffer,
        sizeof(state->buffer),
        &state->from,
        0);
}

static void watch_entry(void* arg)
{
    WatchState* state = (WatchState*)arg;
    state->result = galay_kernel_file_watcher_watch(state->watcher, &state->watch_result, 1);
}

static void async_write_entry(void* arg)
{
    AsyncFileState* state = (AsyncFileState*)arg;
    state->result = galay_kernel_async_file_write(
        state->file,
        state->payload,
        state->payload_size,
        0,
        1000);
}

static void async_read_entry(void* arg)
{
    AsyncFileState* state = (AsyncFileState*)arg;
    state->result = galay_kernel_async_file_read(
        state->file,
        state->read_buffer,
        state->read_buffer_size,
        0,
        1000);
}

static void async_close_entry(void* arg)
{
    AsyncFileState* state = (AsyncFileState*)arg;
    state->result = galay_kernel_async_file_close(state->file, 1000);
}

static int run_task(galay_kernel_runtime_t* runtime,
                    galay_coro_entry_fn entry,
                    void* state,
                    int64_t join_timeout_ms)
{
    galay_coro_task_t task = {0};
    C_IOResult spawn_result = galay_coro_spawn(runtime, entry, state, NULL, &task);
    if (spawn_result.code != C_IOResultOk) {
        return 1;
    }
    C_IOResult join_result = galay_coro_join(&task, join_timeout_ms);
    C_IOResult destroy_result = galay_coro_destroy(&task);
    return join_result.code == C_IOResultOk && destroy_result.code == C_IOResultOk ? 0 : 1;
}

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
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
        return 1;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        return 1;
    }
    return 0;
}

static int unlink_if_exists(const char* path)
{
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (unlink(path) == 0) {
        return 0;
    }
    return errno == ENOENT ? 0 : 1;
}

static int parse_iterations(int argc, char** argv)
{
    int iterations = 32;
    if (argc > 1) {
        const int parsed = atoi(argv[1]);
        if (parsed > 0) {
            iterations = parsed;
        }
    }
    return iterations;
}

int main(int argc, char** argv)
{
    const int iterations = parse_iterations(argc, argv);

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
    int exit_code = 0;
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
        exit_code = 1;
        goto cleanup;
    }

    watch_fd = mkstemp(watch_path);
    if (watch_fd >= 0) {
        if (close(watch_fd) != 0) {
            failures = 1;
            exit_code = 2;
            goto cleanup;
        }
        watch_fd = -1;
        const C_FileWatcherResultCode watcher_created = galay_kernel_file_watcher_create(&watcher);
        if (watcher_created == C_FileWatcherSuccess &&
            galay_kernel_file_watcher_add_watch(&watcher, watch_path, C_FileWatchEventModify, &watch_descriptor) ==
                C_FileWatcherSuccess) {
            watcher_supported = 1;
        } else if (watcher_created != C_FileWatcherOperationUnsupported) {
            failures = 1;
            exit_code = 3;
            goto cleanup;
        }
    }

    if (make_temp_path(async_path, sizeof(async_path), "galay_timeout_pressure_async") != 0) {
        failures = 1;
        exit_code = 4;
        goto cleanup;
    }

    const C_AsyncFileResultCode async_created = galay_kernel_async_file_create(&async_file);
    if (async_created == C_AsyncFileSuccess) {
        AsyncFileState write_state = {
            .file = &async_file,
            .payload = payload,
            .payload_size = payload_size,
            .read_buffer = NULL,
            .read_buffer_size = 0,
            .result = {C_IOResultInvalid, 0, 0, 0, NULL},
        };
        if (galay_kernel_async_file_open(&async_file, async_path, C_AsyncFileOpenModeReadWrite, 0600) !=
                C_AsyncFileSuccess ||
            run_task(&runtime, async_write_entry, &write_state, 3000) != 0 ||
            write_state.result.code != C_IOResultOk ||
            write_state.result.bytes != payload_size ||
            galay_kernel_async_file_sync(&async_file) != C_AsyncFileSuccess) {
            failures = 1;
            exit_code = 5;
            goto cleanup;
        }
        async_supported = 1;
    } else if (async_created != C_AsyncFileOperationUnsupported) {
        failures = 1;
        exit_code = 6;
        goto cleanup;
    }

    const int64_t start_us = now_us();
    for (int i = 0; i < iterations; ++i) {
        TcpAcceptState tcp_state = {
            .listener = &listener,
            .accepted = {0},
            .result = {C_IOResultInvalid, 0, 0, 0, NULL},
        };
        if (run_task(&runtime, tcp_accept_entry, &tcp_state, 2000) != 0 ||
            tcp_state.result.code != C_IOResultTimeout ||
            tcp_state.accepted.socket != NULL) {
            if (tcp_state.accepted.socket != NULL &&
                galay_kernel_tcp_socket_destroy(&tcp_state.accepted) != C_TcpSocketSuccess &&
                exit_code == 0) {
                exit_code = 7;
            }
            ++failures;
            break;
        }
        ++tcp_completed;

        UdpRecvState udp_state = {
            .socket = &udp,
            .result = {C_IOResultInvalid, 0, 0, 0, NULL},
            .from = {0},
            .buffer = {0},
        };
        if (run_task(&runtime, udp_recv_entry, &udp_state, 2000) != 0 ||
            udp_state.result.code != C_IOResultTimeout) {
            ++failures;
            if (exit_code == 0) {
                exit_code = 8;
            }
            break;
        }
        ++udp_completed;

        if (watcher_supported) {
            WatchState watch_state = {
                .watcher = &watcher,
                .watch_result = {0},
                .result = {C_IOResultInvalid, 0, 0, 0, NULL},
            };
            if (run_task(&runtime, watch_entry, &watch_state, 2000) != 0 ||
                watch_state.result.code != C_IOResultTimeout ||
                watch_state.watch_result.code != C_FileWatcherTimeout) {
                ++failures;
                if (exit_code == 0) {
                    exit_code = 9;
                }
                break;
            }
            ++watch_completed;
        }

        if (async_supported) {
            char read_buffer[32] = {0};
            AsyncFileState read_state = {
                .file = &async_file,
                .payload = NULL,
                .payload_size = 0,
                .read_buffer = read_buffer,
                .read_buffer_size = sizeof(read_buffer),
                .result = {C_IOResultInvalid, 0, 0, 0, NULL},
            };
            if (run_task(&runtime, async_read_entry, &read_state, 3000) != 0 ||
                read_state.result.code != C_IOResultOk ||
                read_state.result.bytes != payload_size ||
                memcmp(read_buffer, payload, payload_size) != 0) {
                ++failures;
                if (exit_code == 0) {
                    exit_code = 10;
                }
                break;
            }
            ++async_completed;
        }
    }
    const int64_t elapsed_us = now_us() - start_us;

    if (printf("timeout_api_pressure iterations=%d failures=%d tcp=%d udp=%d file_watcher=%d%s async_file=%d%s elapsed_us=%lld\n",
               iterations,
               failures,
               tcp_completed,
               udp_completed,
               watch_completed,
               watcher_supported ? "" : "(skipped)",
               async_completed,
               async_supported ? "" : "(skipped)",
               (long long)elapsed_us) < 0) {
        exit_code = 11;
    }

cleanup:
    if (async_file.file != NULL) {
        AsyncFileState close_state = {
            .file = &async_file,
            .payload = NULL,
            .payload_size = 0,
            .read_buffer = NULL,
            .read_buffer_size = 0,
            .result = {C_IOResultInvalid, 0, 0, 0, NULL},
        };
        if (runtime.runtime != NULL &&
            run_task(&runtime, async_close_entry, &close_state, 3000) != 0 &&
            exit_code == 0) {
            exit_code = 12;
        }
        if (runtime.runtime != NULL &&
            close_state.result.code != C_IOResultOk &&
            exit_code == 0) {
            exit_code = 13;
        }
        if (galay_kernel_async_file_destroy(&async_file) != C_AsyncFileSuccess && exit_code == 0) {
            exit_code = 14;
        }
    }
    if (watcher.watcher != NULL) {
        if (watch_descriptor >= 0 &&
            galay_kernel_file_watcher_remove_watch(&watcher, watch_descriptor) != C_FileWatcherSuccess &&
            exit_code == 0) {
            exit_code = 15;
        }
        if (galay_kernel_file_watcher_destroy(&watcher) != C_FileWatcherSuccess && exit_code == 0) {
            exit_code = 16;
        }
    }
    if (udp.socket != NULL &&
        galay_kernel_udp_socket_destroy(&udp) != C_UdpSocketSuccess &&
        exit_code == 0) {
        exit_code = 17;
    }
    if (listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 18;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 19;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 20;
    }
    if (watch_fd >= 0 && close(watch_fd) != 0 && exit_code == 0) {
        exit_code = 21;
    }
    if (unlink_if_exists(watch_path) != 0 && exit_code == 0) {
        exit_code = 22;
    }
    if (unlink_if_exists(async_path) != 0 && exit_code == 0) {
        exit_code = 23;
    }
    if (exit_code != 0) {
        return exit_code;
    }
    return failures == 0 ? 0 : 24;
}
