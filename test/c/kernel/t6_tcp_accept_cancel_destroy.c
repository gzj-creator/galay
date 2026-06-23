#include <galay/c/galay-kernel/galay_kernel.h>

#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

enum {
    DEADLINE_MS = 500
};

static void on_alarm(int signo)
{
    (void)signo;
    _Exit(124);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    return actual == expected ? 0 : 1;
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

static int run_pending_accept_case(galay_kernel_runtime_t* runtime, int call_cancel)
{
    galay_kernel_tcp_socket_t* listener = 0;
    galay_kernel_tcp_accept_t* accept = 0;

    const int listener_status = start_listener(&listener);
    if (listener_status != 0) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
        return 10 + listener_status;
    }

    if (galay_kernel_tcp_accept_start(runtime, listener, &accept) != GALAY_OK || accept == 0) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
        return 20;
    }

    const int64_t start = now_ms();
    if (call_cancel) {
        if (expect_status(galay_kernel_tcp_accept_cancel(accept), GALAY_OK)) {
            (void)galay_kernel_tcp_accept_destroy(&accept);
            (void)galay_kernel_tcp_socket_destroy(&listener);
            return 30;
        }
    }
    if (expect_status(galay_kernel_tcp_accept_destroy(&accept), GALAY_OK)) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
        return 31;
    }
    const int64_t elapsed = now_ms() - start;
    if (elapsed < 0 || elapsed > DEADLINE_MS) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
        return 32;
    }
    if (accept != 0) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
        return 33;
    }

    if (expect_status(galay_kernel_tcp_socket_destroy(&listener), GALAY_OK)) {
        return 40;
    }
    return 0;
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(3);

    galay_kernel_runtime_config_t config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 1;

    galay_kernel_runtime_t* runtime = 0;
    if (expect_status(galay_kernel_runtime_create(&config, &runtime), GALAY_OK)) {
        return 1;
    }
    if (expect_status(galay_kernel_runtime_start(runtime), GALAY_OK)) {
        (void)galay_kernel_runtime_destroy(&runtime);
        return 2;
    }

    int result = run_pending_accept_case(runtime, 0);
    if (result == 0) {
        result = run_pending_accept_case(runtime, 1);
    }

    (void)galay_kernel_runtime_stop(runtime);
    if (expect_status(galay_kernel_runtime_destroy(&runtime), GALAY_OK) && result == 0) {
        result = 3;
    }

    alarm(0);
    return result;
}
