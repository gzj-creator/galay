#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

typedef struct UdpEchoState {
    galay_kernel_udp_socket_t server;
    galay_kernel_udp_socket_t client;
    C_Host server_local;
    C_Host client_local;
    C_Host server_from;
    C_Host client_from;
    C_IOResult server_recv_result;
    C_IOResult server_send_result;
    C_IOResult client_send_result;
    C_IOResult client_recv_result;
    C_IOResult server_close_result;
    C_IOResult client_close_result;
    char server_buffer[32];
    char client_buffer[32];
} UdpEchoState;

static int expect_status(C_UdpSocketResultCode actual, C_UdpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int create_bound_socket(galay_kernel_udp_socket_t* socket, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_kernel_udp_socket_create(socket, C_IPTypeIPV4) == C_UdpSocketSuccess &&
        galay_kernel_udp_socket_bind(socket, &bind_host) == C_UdpSocketSuccess &&
        galay_kernel_udp_socket_local_endpoint(socket, local) == C_UdpSocketSuccess &&
        local->port != 0
        ? 0
        : 1;
}

static int same_host(const C_Host* lhs, const C_Host* rhs)
{
    return lhs->type == rhs->type &&
        lhs->port == rhs->port &&
        strncmp(lhs->address, rhs->address, sizeof(lhs->address)) == 0;
}

static void udp_server_entry(void* arg)
{
    UdpEchoState* state = (UdpEchoState*)arg;
    state->server_recv_result =
        galay_kernel_udp_socket_recvfrom(&state->server,
                                         state->server_buffer,
                                         sizeof(state->server_buffer),
                                         &state->server_from,
                                         1000);
    if (state->server_recv_result.code != C_IOResultOk) {
        return;
    }
    state->server_send_result =
        galay_kernel_udp_socket_sendto(&state->server,
                                       "udp-pong",
                                       strlen("udp-pong"),
                                       &state->server_from,
                                       1000);
    state->server_close_result = galay_kernel_udp_socket_close(&state->server, 1000);
}

static void udp_client_entry(void* arg)
{
    UdpEchoState* state = (UdpEchoState*)arg;
    state->client_send_result =
        galay_kernel_udp_socket_sendto(&state->client,
                                       "udp-ping",
                                       strlen("udp-ping"),
                                       &state->server_local,
                                       1000);
    if (state->client_send_result.code != C_IOResultOk) {
        return;
    }
    state->client_recv_result =
        galay_kernel_udp_socket_recvfrom(&state->client,
                                         state->client_buffer,
                                         sizeof(state->client_buffer),
                                         &state->client_from,
                                         1000);
    state->client_close_result = galay_kernel_udp_socket_close(&state->client, 1000);
}

static int run_udp_echo(galay_kernel_runtime_t* runtime)
{
    UdpEchoState state = {0};
    if (create_bound_socket(&state.server, &state.server_local) != 0 ||
        create_bound_socket(&state.client, &state.client_local) != 0) {
        if (state.server.socket != 0 &&
            galay_kernel_udp_socket_destroy(&state.server) != C_UdpSocketSuccess) {
            return 104;
        }
        if (state.client.socket != 0 &&
            galay_kernel_udp_socket_destroy(&state.client) != C_UdpSocketSuccess) {
            return 105;
        }
        return 101;
    }

    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int result = 0;
    if (expect_code(galay_coro_spawn(runtime, udp_server_entry, &state, 0, &server),
                    C_IOResultOk) ||
        expect_code(galay_coro_spawn(runtime, udp_client_entry, &state, 0, &client),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&server, 2000), C_IOResultOk) ||
        expect_code(galay_coro_join(&client, 2000), C_IOResultOk)) {
        result = 102;
        goto cleanup;
    }

    const int failed =
        state.server_recv_result.code != C_IOResultOk ||
        state.server_recv_result.bytes != strlen("udp-ping") ||
        memcmp(state.server_buffer, "udp-ping", strlen("udp-ping")) != 0 ||
        !same_host(&state.server_from, &state.client_local) ||
        state.server_send_result.code != C_IOResultOk ||
        state.server_send_result.bytes != strlen("udp-pong") ||
        state.client_send_result.code != C_IOResultOk ||
        state.client_send_result.bytes != strlen("udp-ping") ||
        state.client_recv_result.code != C_IOResultOk ||
        state.client_recv_result.bytes != strlen("udp-pong") ||
        memcmp(state.client_buffer, "udp-pong", strlen("udp-pong")) != 0 ||
        !same_host(&state.client_from, &state.server_local) ||
        state.server_close_result.code != C_IOResultOk ||
        state.client_close_result.code != C_IOResultOk;

    result = failed ? 103 : 0;

cleanup:
    if (server.task != 0 && galay_coro_destroy(&server).code != C_IOResultOk && result == 0) {
        result = 106;
    }
    if (client.task != 0 && galay_coro_destroy(&client).code != C_IOResultOk && result == 0) {
        result = 107;
    }
    if (state.server.socket != 0) {
        if (galay_kernel_udp_socket_destroy(&state.server) != C_UdpSocketSuccess &&
            result == 0) {
            result = 108;
        }
    }
    if (state.client.socket != 0) {
        if (galay_kernel_udp_socket_destroy(&state.client) != C_UdpSocketSuccess &&
            result == 0) {
            result = 109;
        }
    }
    return result;
}

int main(void)
{
    galay_kernel_udp_socket_t invalid_socket = {0};
    C_Host host = {C_IPTypeIPV4, "127.0.0.1", 1};
    char buffer[8] = {0};
    if (galay_kernel_udp_socket_get_error(C_UdpSocketSuccess) == 0 ||
        expect_status(galay_kernel_udp_socket_create(0, C_IPTypeIPV4),
                      C_UdpSocketParameterInvalid) ||
        expect_status(galay_kernel_udp_socket_create(&invalid_socket, (C_IPType)99),
                      C_UdpSocketParameterInvalid) ||
        expect_status(galay_kernel_udp_socket_destroy(0), C_UdpSocketParameterInvalid) ||
        expect_code(galay_kernel_udp_socket_recvfrom(0, buffer, sizeof(buffer), 0, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_kernel_udp_socket_recvfrom(&invalid_socket, 0, sizeof(buffer), 0, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_kernel_udp_socket_sendto(0, buffer, sizeof(buffer), &host, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_kernel_udp_socket_sendto(&invalid_socket, 0, sizeof(buffer), &host, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_kernel_udp_socket_sendto(&invalid_socket, buffer, sizeof(buffer), 0, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_kernel_udp_socket_close(0, 0), C_IOResultInvalid)) {
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 2;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
            return 5;
        }
        return 3;
    }

    int result = run_udp_echo(&runtime);
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 6;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 4;
    }
    return result;
}
