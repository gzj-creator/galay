#include <galay/c/galay-kernel/galay_kernel.h>

#include <signal.h>
#include <unistd.h>

static void on_alarm(int signo)
{
    (void)signo;
    _Exit(124);
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

static int run_cancel_wait_join_case(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t* listener = 0;
    galay_kernel_tcp_accept_t* accept = 0;
    galay_kernel_tcp_socket_t* accepted = 0;
    galay_kernel_tcp_host_config_t peer = {GALAY_KERNEL_IP_V4, 0, 0};

    const int listener_status = start_listener(&listener);
    if (listener_status != 0) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
        return 10 + listener_status;
    }

    if (galay_kernel_tcp_accept_start(runtime, listener, &accept) != GALAY_OK || accept == 0) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
        return 20;
    }

    if (expect_status(galay_kernel_tcp_socket_destroy(&listener), GALAY_OK)) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 21;
    }
    if (listener != 0) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 22;
    }

    if (expect_status(galay_kernel_tcp_accept_cancel(accept), GALAY_OK)) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 30;
    }
    if (expect_status(galay_kernel_tcp_accept_cancel(accept), GALAY_OK)) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 31;
    }
    if (expect_status(galay_kernel_tcp_accept_wait(accept), GALAY_OK)) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 32;
    }
    if (expect_status(galay_kernel_tcp_accept_join(accept, &accepted, &peer), GALAY_IO_ERROR)) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
        if (accepted != 0) {
            (void)galay_kernel_tcp_socket_destroy(&accepted);
        }
        return 33;
    }
    if (accepted != 0) {
        (void)galay_kernel_tcp_socket_destroy(&accepted);
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 34;
    }
    if (expect_status(galay_kernel_tcp_accept_cancel(accept), GALAY_OK)) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 35;
    }
    if (expect_status(galay_kernel_tcp_accept_wait(accept), GALAY_OK)) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 36;
    }
    if (expect_status(galay_kernel_tcp_accept_join(accept, &accepted, &peer), GALAY_INVALID_ARGUMENT)) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 37;
    }
    if (accepted != 0) {
        (void)galay_kernel_tcp_socket_destroy(&accepted);
        (void)galay_kernel_tcp_accept_destroy(&accept);
        return 38;
    }
    if (expect_status(galay_kernel_tcp_accept_destroy(&accept), GALAY_OK)) {
        return 39;
    }
    return accept == 0 ? 0 : 40;
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

    int result = run_cancel_wait_join_case(runtime);

    (void)galay_kernel_runtime_stop(runtime);
    if (expect_status(galay_kernel_runtime_destroy(&runtime), GALAY_OK) && result == 0) {
        result = 3;
    }

    alarm(0);
    return result;
}
