#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-ws-c/ws_c.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct EchoServerState {
    galay_kernel_tcp_socket_t* listener;
    int exit_code;
} EchoServerState;

static int create_listener(galay_kernel_tcp_socket_t* listener, uint16_t port)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", port};
    return galay_kernel_tcp_socket_create(listener, C_IPTypeIPV4) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_bind(listener, &bind_host) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_listen(listener, 16) == C_TcpSocketSuccess
        ? 0
        : 1;
}

static void echo_server_entry(void* arg)
{
    EchoServerState* state = (EchoServerState*)arg;
    galay_kernel_tcp_socket_t accepted = {0};
    galay_ws_session_t* session = NULL;
    galay_ws_connection_t* connection = NULL;
    galay_ws_received_frame_t* frame = NULL;

    C_IOResult accepted_result =
        galay_kernel_tcp_socket_accept(state->listener, &accepted, NULL, -1);
    if (accepted_result.code != C_IOResultOk) {
        state->exit_code = 2;
        return;
    }
    if (galay_ws_session_adopt_tcp(&accepted, GALAY_TRUE, &session) != GALAY_OK ||
        galay_ws_session_accept_upgrade(session, 3000).code != C_IOResultOk ||
        galay_ws_session_connection(session, &connection) != GALAY_OK ||
        connection == NULL) {
        state->exit_code = 3;
        goto cleanup;
    }
    C_IOResult received = galay_ws_connection_recv_frame(connection, -1, &frame);
    if (received.code != C_IOResultOk || frame == NULL) {
        state->exit_code = 4;
        goto cleanup;
    }

    const uint8_t* payload = NULL;
    size_t payload_len = 0;
    if (galay_ws_received_frame_payload(frame, &payload, &payload_len) != GALAY_OK) {
        state->exit_code = 5;
        goto cleanup;
    }
    const galay_ws_opcode_t opcode = galay_ws_received_frame_opcode(frame);
    C_IOResult sent = {C_IOResultInvalid, 0, 0, 0, NULL};
    if (opcode == GALAY_WS_OPCODE_TEXT) {
        sent = galay_ws_connection_send_text(connection, payload, payload_len, 3000);
    } else if (opcode == GALAY_WS_OPCODE_BINARY) {
        sent = galay_ws_connection_send_binary(connection, payload, payload_len, 3000);
    } else if (opcode == GALAY_WS_OPCODE_PING) {
        sent = galay_ws_connection_send_pong(connection, payload, payload_len, 3000);
    } else if (opcode == GALAY_WS_OPCODE_CLOSE) {
        sent = galay_ws_connection_send_close(connection, GALAY_WS_CLOSE_NORMAL, NULL, 0, 3000);
    }
    if (sent.code != C_IOResultOk) {
        state->exit_code = 6;
        goto cleanup;
    }
    C_IOResult closed = galay_ws_connection_close(connection, 3000);
    state->exit_code = closed.code == C_IOResultOk ? 0 : 7;

cleanup:
    galay_ws_received_frame_destroy(frame);
    galay_ws_session_destroy(session);
    if (accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&accepted) != C_TcpSocketSuccess &&
        state->exit_code == 0) {
        state->exit_code = 8;
    }
}

int main(int argc, char** argv)
{
    const uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 9001;
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    galay_coro_task_t task = {0};
    EchoServerState state = {.listener = &listener, .exit_code = 1};

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, port) != 0 ||
        printf("listening on ws://127.0.0.1:%u/\n", (unsigned)port) < 0 ||
        galay_coro_spawn(&runtime, echo_server_entry, &state, NULL, &task).code != C_IOResultOk ||
        galay_coro_join(&task, -1).code != C_IOResultOk) {
        state.exit_code = 9;
    }
    if (task.task != NULL && galay_coro_destroy(&task).code != C_IOResultOk && state.exit_code == 0) {
        state.exit_code = 10;
    }
    if (listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
        state.exit_code == 0) {
        state.exit_code = 11;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess &&
        state.exit_code == 0) {
        state.exit_code = 12;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess &&
        state.exit_code == 0) {
        state.exit_code = 13;
    }
    return state.exit_code;
}
