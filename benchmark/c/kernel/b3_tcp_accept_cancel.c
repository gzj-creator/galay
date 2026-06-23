#include <galay/c/galay-kernel/galay_kernel.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

enum {
    CANCEL_ITERATIONS = 64
};

static void on_alarm(int signo)
{
    (void)signo;
    _Exit(124);
}

static int64_t now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
}

static int start_listener(galay_kernel_tcp_socket_t** out_listener)
{
    galay_kernel_tcp_host_config_t bind_host = {GALAY_KERNEL_IP_V4, "127.0.0.1", 0};

    if (galay_kernel_tcp_socket_create(GALAY_KERNEL_IP_V4, out_listener) != GALAY_OK) {
        return 1;
    }
    if (galay_kernel_tcp_socket_bind(*out_listener, &bind_host) != GALAY_OK) {
        return 2;
    }
    if (galay_kernel_tcp_socket_listen(*out_listener, 16) != GALAY_OK) {
        return 3;
    }
    return 0;
}

static int run_cancel_once(galay_kernel_runtime_t* runtime)
{
    int result = 0;
    galay_kernel_tcp_socket_t* listener = 0;
    galay_kernel_tcp_accept_t* accept = 0;

    result = start_listener(&listener);
    if (result != 0) {
        result += 10;
        goto cleanup;
    }
    if (galay_kernel_tcp_accept_start(runtime, listener, &accept) != GALAY_OK || accept == 0) {
        result = 20;
        goto cleanup;
    }
    if (galay_kernel_tcp_accept_cancel(accept) != GALAY_OK) {
        result = 21;
        goto cleanup;
    }
    if (galay_kernel_tcp_accept_wait(accept) != GALAY_OK) {
        result = 22;
        goto cleanup;
    }

cleanup:
    if (accept != 0) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
    }
    if (listener != 0) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
    }
    return result;
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(10);

    galay_kernel_runtime_t* runtime = 0;
    galay_kernel_runtime_config_t config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 1;

    if (galay_kernel_runtime_create(&config, &runtime) != GALAY_OK) {
        return 1;
    }
    if (galay_kernel_runtime_start(runtime) != GALAY_OK) {
        (void)galay_kernel_runtime_destroy(&runtime);
        return 2;
    }

    const int64_t start = now_us();
    for (int i = 0; i < CANCEL_ITERATIONS; ++i) {
        const int run_result = run_cancel_once(runtime);
        if (run_result != 0) {
            (void)galay_kernel_runtime_stop(runtime);
            (void)galay_kernel_runtime_destroy(&runtime);
            return 10 + run_result;
        }
    }
    const int64_t elapsed = now_us() - start;
    const double seconds = elapsed > 0 ? elapsed / 1000000.0 : 0.0;
    const double ops_per_second = seconds > 0.0 ? CANCEL_ITERATIONS / seconds : 0.0;

    printf("tcp_accept_cancel iterations=%d elapsed_ms=%.3f ops_per_sec=%.2f\n",
           CANCEL_ITERATIONS,
           elapsed / 1000.0,
           ops_per_second);

    (void)galay_kernel_runtime_stop(runtime);
    const int destroy_ok = galay_kernel_runtime_destroy(&runtime) == GALAY_OK;
    alarm(0);
    return destroy_ok ? 0 : 3;
}
