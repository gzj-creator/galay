#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct TimeoutState {
    atomic_int done;
    atomic_int code;
} TimeoutState;

static void on_recv(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    TimeoutState* state = (TimeoutState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->done, 1);
}

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv)
{
    int iterations = 100;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0) {
            iterations = parsed;
        }
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_udp_socket_t socket = {0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host local = {0};
    char recv_buffer[1] = {0};
    int completed = 0;
    int failures = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_udp_socket_create(&socket, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&socket, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&socket, &local) != C_UdpSocketSuccess) {
        failures = 1;
        goto cleanup;
    }
    (void)local;

    const uint64_t start_ns = now_ns();
    for (int i = 0; i < iterations; ++i) {
        TimeoutState state;
        atomic_init(&state.done, 0);
        atomic_init(&state.code, (int)C_UdpSocketSuccess);

        if (galay_kernel_udp_socket_recvfrom_timeout(
                &runtime,
                &socket,
                recv_buffer,
                sizeof(recv_buffer),
                0,
                on_recv,
                &state) != C_UdpSocketSuccess) {
            ++failures;
            break;
        }
        if (wait_done(&state.done) != 0 || atomic_load(&state.code) != (int)C_UdpSocketTimeout) {
            ++failures;
            break;
        }
        ++completed;
    }
    const uint64_t elapsed_ns = now_ns() - start_ns;

    printf("udp_timeout_smoke iterations=%d completed=%d failures=%d elapsed_ns=%llu avg_ns=%.2f\n",
           iterations,
           completed,
           failures,
           (unsigned long long)elapsed_ns,
           completed == 0 ? 0.0 : (double)elapsed_ns / (double)completed);

cleanup:
    galay_kernel_udp_socket_destroy(&socket);
    galay_kernel_runtime_stop(&runtime);
    galay_kernel_runtime_destroy(&runtime);
    return failures == 0 && completed == iterations ? 0 : 1;
}
