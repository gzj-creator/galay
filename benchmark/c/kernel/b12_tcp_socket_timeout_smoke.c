#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct TimeoutState {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult result;
} TimeoutState;

static void accept_timeout_entry(void* arg)
{
    TimeoutState* state = (TimeoutState*)arg;
    state->result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 0);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int parse_iterations(int argc, char** argv)
{
    int iterations = 100;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
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
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host local = {0};
    int completed = 0;
    int failures = 0;
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_tcp_socket_create(&listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(&listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&listener, 16) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_local_endpoint(&listener, &local) != C_TcpSocketSuccess ||
        local.port == 0) {
        failures = 1;
        exit_code = 1;
        goto cleanup;
    }

    const uint64_t start_ns = now_ns();
    for (int i = 0; i < iterations; ++i) {
        TimeoutState state = {
            .listener = &listener,
            .accepted = {0},
            .result = {C_IOResultInvalid, 0, 0, 0, NULL},
        };
        galay_coro_task_t task = {0};
        C_IOResult spawn_result = galay_coro_spawn(&runtime, accept_timeout_entry, &state, NULL, &task);
        if (spawn_result.code != C_IOResultOk) {
            ++failures;
            exit_code = 2;
            break;
        }
        C_IOResult join_result = galay_coro_join(&task, 2000);
        C_IOResult destroy_result = galay_coro_destroy(&task);
        if (join_result.code != C_IOResultOk || destroy_result.code != C_IOResultOk ||
            state.result.code != C_IOResultTimeout || state.accepted.socket != NULL) {
            ++failures;
            exit_code = 3;
            if (state.accepted.socket != NULL &&
                galay_kernel_tcp_socket_destroy(&state.accepted) != C_TcpSocketSuccess) {
                exit_code = 4;
            }
            break;
        }
        ++completed;
    }
    const uint64_t elapsed_ns = now_ns() - start_ns;

    if (printf("tcp_timeout_smoke iterations=%d completed=%d failures=%d elapsed_ns=%llu avg_ns=%.2f\n",
               iterations,
               completed,
               failures,
               (unsigned long long)elapsed_ns,
               completed == 0 ? 0.0 : (double)elapsed_ns / (double)completed) < 0) {
        exit_code = 5;
    }

cleanup:
    if (listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 6;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 7;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 8;
    }
    if (exit_code != 0) {
        return exit_code;
    }
    return failures == 0 && completed == iterations ? 0 : 9;
}
