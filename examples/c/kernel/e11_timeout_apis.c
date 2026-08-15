#include <galay/c/galay-kernel-c/async-c/file_watcher.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket.h>
#include <galay/c/galay-kernel-c/async-c/udp_socket.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct TimeoutState {
    galay_c_tcp_socket_t* listener;
    galay_c_tcp_socket_t accepted;
    galay_c_udp_socket_t* udp;
    galay_c_file_watcher_t* watcher;
    C_IOResult tcp_result;
    C_IOResult udp_result;
    C_IOResult watcher_result;
    galay_c_file_event_t event;
    char buffer[8];
} TimeoutState;

static void tcp_timeout_entry(void* arg)
{
    TimeoutState* const state = arg;
    state->tcp_result =
        galay_c_tcp_socket_accept(state->listener, &state->accepted, NULL, 0);
}

static void udp_timeout_entry(void* arg)
{
    TimeoutState* const state = arg;
    state->udp_result =
        galay_c_udp_socket_recvfrom(state->udp, state->buffer, sizeof(state->buffer), NULL, 0);
}

static void watcher_timeout_entry(void* arg)
{
    TimeoutState* const state = arg;
    state->watcher_result = galay_c_file_watcher_wait(state->watcher, &state->event, 0);
}

int main(void)
{
    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_tcp_socket_t listener = {.fd = -1};
    galay_c_udp_socket_t udp = {.fd = -1};
    galay_c_file_watcher_t watcher = {.fd = -1};
    galay_c_coro_task_t tcp_task = {0};
    galay_c_coro_task_t udp_task = {0};
    galay_c_coro_task_t watcher_task = {0};
    TimeoutState state = {
        .listener = &listener,
        .accepted = {.fd = -1},
        .udp = &udp,
        .watcher = &watcher,
    };
    const C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    char path[] = "/tmp/galay-timeout-example-XXXXXX";
    const int temp_fd = mkstemp(path);
    int watch_descriptor = -1;
    int result = 0;
    if (temp_fd < 0 || close(temp_fd) != 0) {
        return 1;
    }

    if (galay_c_tcp_socket_create(&listener, C_IPTypeIPV4).code != C_IOResultOk ||
        galay_c_tcp_socket_bind(&listener, &bind_host).code != C_IOResultOk ||
        galay_c_tcp_socket_listen(&listener, 16).code != C_IOResultOk ||
        galay_c_udp_socket_create(&udp, C_IPTypeIPV4).code != C_IOResultOk ||
        galay_c_udp_socket_bind(&udp, &bind_host).code != C_IOResultOk ||
        galay_c_file_watcher_create(&watcher).code != C_IOResultOk) {
        result = 2;
        goto cleanup;
    }
    const C_IOResult added =
        galay_c_file_watcher_add_watch(&watcher, path, GALAY_C_WATCH_MODIFY);
    if (added.code != C_IOResultOk) {
        result = 3;
        goto cleanup;
    }
    watch_descriptor = (int)added.value;
    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_c_coro_spawn(&runtime, tcp_timeout_entry, &state, NULL, &tcp_task).code !=
            C_IOResultOk ||
        galay_c_coro_spawn(&runtime, udp_timeout_entry, &state, NULL, &udp_task).code !=
            C_IOResultOk ||
        galay_c_coro_spawn(&runtime, watcher_timeout_entry, &state, NULL,
                         &watcher_task).code != C_IOResultOk ||
        galay_c_coro_join(&tcp_task, 2000).code != C_IOResultOk ||
        galay_c_coro_join(&udp_task, 2000).code != C_IOResultOk ||
        galay_c_coro_join(&watcher_task, 2000).code != C_IOResultOk) {
        result = 4;
    } else if (state.tcp_result.code != C_IOResultTimeout ||
               state.udp_result.code != C_IOResultTimeout ||
               state.watcher_result.code != C_IOResultTimeout) {
        result = 5;
    } else if (printf("TCP, UDP and file watcher poll timeouts completed\n") < 0) {
        result = 6;
    }

cleanup:
    if (tcp_task.task != NULL && galay_c_coro_destroy(&tcp_task).code != C_IOResultOk &&
        result == 0) {
        result = 7;
    }
    if (udp_task.task != NULL && galay_c_coro_destroy(&udp_task).code != C_IOResultOk &&
        result == 0) {
        result = 8;
    }
    if (watcher_task.task != NULL &&
        galay_c_coro_destroy(&watcher_task).code != C_IOResultOk && result == 0) {
        result = 9;
    }
    if (state.accepted.fd >= 0 && galay_c_tcp_socket_close(&state.accepted).code !=
            C_IOResultOk &&
        result == 0) {
        result = 10;
    }
    if (listener.fd >= 0 && galay_c_tcp_socket_close(&listener).code != C_IOResultOk &&
        result == 0) {
        result = 11;
    }
    if (udp.fd >= 0 && galay_c_udp_socket_close(&udp).code != C_IOResultOk && result == 0) {
        result = 12;
    }
    if (watch_descriptor >= 0 && watcher.fd >= 0 &&
        galay_c_file_watcher_remove_watch(&watcher, watch_descriptor).code != C_IOResultOk &&
        result == 0) {
        result = 13;
    }
    if (watcher.fd >= 0 && galay_c_file_watcher_close(&watcher).code != C_IOResultOk &&
        result == 0) {
        result = 14;
    }
    if (runtime.runtime != NULL) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 15;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 16;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT && result == 0) {
        result = 17;
    }
    return result;
}
