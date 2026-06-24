#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <stdatomic.h>
#include <time.h>

typedef struct CallbackState {
    atomic_int done;
} CallbackState;

static int expect_status(C_TcpSocketResultCode actual, C_TcpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void mark_done(void* ctx)
{
    if (ctx != 0) {
        CallbackState* state = (CallbackState*)ctx;
        atomic_store(&state->done, 1);
    }
}

static void on_connect(C_TcpSocketResultCode code, void* ctx)
{
    (void)code;
    mark_done(ctx);
}

static void on_accept(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
}

static int on_accept_loop(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
    return 1;
}

static void on_recv(galay_kernel_tcp_recv_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
}

static int on_recv_loop(galay_kernel_tcp_recv_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
    return 1;
}

static void on_send(galay_kernel_tcp_send_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
}

static int on_send_loop(galay_kernel_tcp_send_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
    return 1;
}

static void on_close(C_TcpSocketResultCode code, void* ctx)
{
    (void)code;
    mark_done(ctx);
}

static void wait_if_spawned(C_TcpSocketResultCode actual, CallbackState* state)
{
    if (actual != C_TcpSocketSuccess) {
        return;
    }

    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(&state->done)) {
            return;
        }
        nanosleep(&pause, 0);
    }
}

static int expect_not_running(C_TcpSocketResultCode actual, CallbackState* state)
{
    wait_if_spawned(actual, state);
    return expect_status(actual, C_TcpSocketRuntimeNotRunning);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t tcp = {0};
    C_Host host = {C_IPTypeIPV4, "127.0.0.1", 9};
    char recv_buffer[8] = {0};
    const char send_buffer[] = "x";
    int exit_code = 0;

    CallbackState callback_state;
    atomic_init(&callback_state.done, 0);

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 2;
        goto cleanup;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess) {
        exit_code = 3;
        goto cleanup;
    }
    if (galay_kernel_runtime_is_running(&runtime)) {
        exit_code = 4;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_create(&tcp, C_IPTypeIPV4), C_TcpSocketSuccess)) {
        exit_code = 5;
        goto cleanup;
    }

    if (expect_not_running(galay_kernel_tcp_socket_close(&runtime, &tcp, on_close, &callback_state), &callback_state)) {
        exit_code = 6;
        goto cleanup;
    }
    if (expect_not_running(galay_kernel_tcp_socket_connect(&runtime, &tcp, &host, on_connect, &callback_state), &callback_state)) {
        exit_code = 7;
        goto cleanup;
    }
    if (expect_not_running(galay_kernel_tcp_socket_accept(&runtime, &tcp, on_accept, &callback_state), &callback_state)) {
        exit_code = 8;
        goto cleanup;
    }
    if (expect_not_running(galay_kernel_tcp_socket_accept_loop(&runtime, &tcp, on_accept_loop, &callback_state), &callback_state)) {
        exit_code = 9;
        goto cleanup;
    }
    if (expect_not_running(galay_kernel_tcp_socket_recv(&runtime, &tcp, recv_buffer, sizeof(recv_buffer), on_recv, &callback_state), &callback_state)) {
        exit_code = 10;
        goto cleanup;
    }
    if (expect_not_running(galay_kernel_tcp_socket_recv_loop(&runtime, &tcp, recv_buffer, sizeof(recv_buffer), on_recv_loop, &callback_state), &callback_state)) {
        exit_code = 11;
        goto cleanup;
    }
    if (expect_not_running(galay_kernel_tcp_socket_send(&runtime, &tcp, send_buffer, sizeof(send_buffer) - 1, on_send, &callback_state), &callback_state)) {
        exit_code = 12;
        goto cleanup;
    }
    if (expect_not_running(galay_kernel_tcp_socket_send_loop(&runtime, &tcp, send_buffer, sizeof(send_buffer) - 1, on_send_loop, &callback_state), &callback_state)) {
        exit_code = 13;
        goto cleanup;
    }

cleanup:
    if (tcp.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&tcp);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}
