#include <galay/c/galay-kernel-c/async-c/udp_socket.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include "socket_test_support.h"

#include <stdint.h>
#include <string.h>

typedef struct UdpTimeoutState {
    galay_c_udp_socket_t* socket;
    C_Host* peer;
    C_IOResult zero_recv_result;
    C_IOResult timed_recv_result;
    C_IOResult zero_send_result;
    C_IOResult close_result;
    char buffer[16];
} UdpTimeoutState;

static int expect_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int create_bound_socket(galay_c_udp_socket_t* socket, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_c_udp_socket_create(socket, C_IPTypeIPV4).code == C_IOResultOk &&
        galay_c_udp_socket_bind(socket, &bind_host).code == C_IOResultOk &&
        galay_c_udp_socket_local_endpoint(socket, local).code == C_IOResultOk &&
        local->port != 0
        ? 0
        : 1;
}

static void timeout_entry(void* arg)
{
    UdpTimeoutState* state = (UdpTimeoutState*)arg;
    state->zero_recv_result =
        galay_c_udp_socket_recvfrom(state->socket,
                                         state->buffer,
                                         sizeof(state->buffer),
                                         NULL,
                                         0);
    state->timed_recv_result =
        galay_c_udp_socket_recvfrom(state->socket,
                                         state->buffer,
                                         sizeof(state->buffer),
                                         NULL,
                                         5);
    state->zero_send_result =
        galay_c_udp_socket_sendto(state->socket, "x", 1, state->peer, 0);
    state->close_result = galay_c_udp_socket_close(state->socket);
}

int main(void)
{
    const galay_test_socket_capability_t socket_capability =
        galay_test_socket_capability(SOCK_DGRAM);
    if (socket_capability == GALAY_TEST_SOCKET_PERMISSION_DENIED) {
        return 125;
    }
    if (socket_capability == GALAY_TEST_SOCKET_PROBE_FAILED) {
        return 126;
    }

    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_c_runtime_t runtime = {0};
    galay_c_udp_socket_t socket = {.fd = -1};
    C_Host local = {0};
    UdpTimeoutState state = {0};
    state.socket = &socket;
    state.peer = &local;

    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_c_runtime_start(&runtime) != C_RuntimeSuccess) {
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess) {
            return 7;
        }
        return 2;
    }
    if (create_bound_socket(&socket, &local) != 0) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess) {
            return 8;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess) {
            return 9;
        }
        return 3;
    }

    galay_c_coro_task_t task = {0};
    int result = 0;
    if (expect_code(galay_c_coro_spawn(&runtime, timeout_entry, &state, 0, &task),
                    C_IOResultOk) ||
        expect_code(galay_c_coro_join(&task, 2000), C_IOResultOk)) {
        result = 4;
    } else if (state.zero_recv_result.code != C_IOResultTimeout ||
               state.timed_recv_result.code != C_IOResultTimeout ||
               state.zero_send_result.code != C_IOResultOk ||
               state.zero_send_result.bytes != 1 ||
               state.close_result.code != C_IOResultOk) {
        result = 5;
    }

    if (galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 10;
    }
    if (socket.fd >= 0) {
        if (galay_c_udp_socket_close(&socket).code != C_IOResultOk &&
            result == 0) {
            result = 11;
        }
    }
    if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 12;
    }
    if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 6;
    }
    return result;
}
