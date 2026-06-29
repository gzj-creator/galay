#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-ws-c/ws.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct EchoClientState {
    const char* host;
    uint16_t port;
    const char* path;
    int exit_code;
} EchoClientState;

static void echo_client_entry(void* arg)
{
    static const char message[] = "hello from galay C ws";
    EchoClientState* state = (EchoClientState*)arg;
    galay_ws_client_t* client = NULL;
    galay_ws_connection_t* connection = NULL;
    galay_ws_received_frame_t* frame = NULL;
    galay_ws_client_config_t config = {
        .host = state->host,
        .port = state->port,
        .path = state->path,
        .connect_timeout_ms = 3000,
    };

    if (galay_ws_client_create(&config, &client) != GALAY_OK || client == NULL) {
        state->exit_code = 2;
        return;
    }
    C_IOResult connected = galay_ws_client_connect(client, 3000, &connection);
    if (connected.code != C_IOResultOk) {
        state->exit_code = 3;
        goto cleanup;
    }
    C_IOResult sent = galay_ws_connection_send_text(connection,
                                                    (const uint8_t*)message,
                                                    sizeof(message) - 1,
                                                    3000);
    if (sent.code != C_IOResultOk) {
        state->exit_code = 4;
        goto cleanup;
    }
    C_IOResult received = galay_ws_connection_recv_frame(connection, 3000, &frame);
    if (received.code != C_IOResultOk || frame == NULL ||
        galay_ws_received_frame_opcode(frame) != GALAY_WS_OPCODE_TEXT) {
        state->exit_code = 5;
        goto cleanup;
    }

    const uint8_t* payload = NULL;
    size_t payload_len = 0;
    if (galay_ws_received_frame_payload(frame, &payload, &payload_len) != GALAY_OK) {
        state->exit_code = 6;
        goto cleanup;
    }
    if (printf("echo: %.*s\n", (int)payload_len, (const char*)payload) < 0) {
        state->exit_code = 7;
        goto cleanup;
    }
    C_IOResult close_sent =
        galay_ws_connection_send_close(connection, GALAY_WS_CLOSE_NORMAL, NULL, 0, 3000);
    if (close_sent.code != C_IOResultOk) {
        state->exit_code = 8;
        goto cleanup;
    }
    C_IOResult closed = galay_ws_connection_close(connection, 3000);
    state->exit_code = closed.code == C_IOResultOk ? 0 : 9;

cleanup:
    galay_ws_received_frame_destroy(frame);
    galay_ws_client_destroy(client);
}

int main(int argc, char** argv)
{
    EchoClientState state = {
        .host = argc > 1 ? argv[1] : "127.0.0.1",
        .port = argc > 2 ? (uint16_t)atoi(argv[2]) : 9001,
        .path = argc > 3 ? argv[3] : "/",
        .exit_code = 1,
    };
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t task = {0};

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_coro_spawn(&runtime, echo_client_entry, &state, NULL, &task).code != C_IOResultOk ||
        galay_coro_join(&task, 10000).code != C_IOResultOk) {
        state.exit_code = 10;
    }
    if (task.task != NULL && galay_coro_destroy(&task).code != C_IOResultOk && state.exit_code == 0) {
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
