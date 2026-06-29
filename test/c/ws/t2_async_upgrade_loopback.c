#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-ws-c/ws.h>

#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

typedef struct WsLoopbackState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    galay_ws_session_t* server_session;
    galay_ws_client_t* client;
    galay_ws_connection_t* client_conn;
    C_IOResult accept_result;
    C_IOResult server_upgrade_result;
    C_IOResult server_recv_text_result;
    C_IOResult server_send_text_result;
    C_IOResult server_recv_close_result;
    C_IOResult server_send_close_result;
    C_IOResult server_close_result;
    C_IOResult client_connect_result;
    C_IOResult client_send_text_result;
    C_IOResult client_recv_text_result;
    C_IOResult client_send_close_result;
    C_IOResult client_recv_close_result;
    C_IOResult client_close_result;
    galay_ws_received_frame_t* server_text;
    galay_ws_received_frame_t* server_close;
    galay_ws_received_frame_t* client_text;
    galay_ws_received_frame_t* client_close;
} WsLoopbackState;

static int create_listener(galay_kernel_tcp_socket_t* listener, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_kernel_tcp_socket_create(listener, C_IPTypeIPV4) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_bind(listener, &bind_host) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_listen(listener, 16) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_local_endpoint(listener, local) == C_TcpSocketSuccess &&
        local->port != 0
        ? 0
        : 1;
}

static int expect_frame_payload(const galay_ws_received_frame_t* frame,
                                galay_ws_opcode_t opcode,
                                const char* expected,
                                size_t expected_len)
{
    const uint8_t* payload = NULL;
    size_t payload_len = 0;
    if (galay_ws_received_frame_opcode(frame) != opcode ||
        galay_ws_received_frame_payload(frame, &payload, &payload_len) != GALAY_OK ||
        payload_len != expected_len ||
        (expected_len != 0 && memcmp(payload, expected, expected_len) != 0)) {
        return 1;
    }
    return 0;
}

static void ws_server_entry(void* arg)
{
    static const char response[] = "echo:hello";
    WsLoopbackState* state = (WsLoopbackState*)arg;

    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    if (galay_ws_session_adopt_tcp(&state->accepted,
                                   GALAY_TRUE,
                                   &state->server_session) != GALAY_OK) {
        state->server_upgrade_result.code = C_IOResultError;
        return;
    }
    state->server_upgrade_result =
        galay_ws_session_accept_upgrade(state->server_session, 1000);
    if (state->server_upgrade_result.code != C_IOResultOk) {
        return;
    }

    galay_ws_connection_t* conn = NULL;
    if (galay_ws_session_connection(state->server_session, &conn) != GALAY_OK || conn == NULL) {
        state->server_recv_text_result.code = C_IOResultError;
        return;
    }

    state->server_recv_text_result =
        galay_ws_connection_recv_frame(conn, 1000, &state->server_text);
    if (state->server_recv_text_result.code != C_IOResultOk) {
        return;
    }
    state->server_send_text_result =
        galay_ws_connection_send_text(conn,
                                      (const uint8_t*)response,
                                      sizeof(response) - 1,
                                      1000);
    if (state->server_send_text_result.code != C_IOResultOk) {
        return;
    }

    state->server_recv_close_result =
        galay_ws_connection_recv_frame(conn, 1000, &state->server_close);
    if (state->server_recv_close_result.code != C_IOResultOk) {
        return;
    }
    state->server_send_close_result =
        galay_ws_connection_send_close(conn, GALAY_WS_CLOSE_NORMAL, NULL, 0, 1000);
    if (state->server_send_close_result.code != C_IOResultOk) {
        return;
    }
    state->server_close_result = galay_ws_connection_close(conn, 1000);
}

static void ws_client_entry(void* arg)
{
    static const char request[] = "hello";
    WsLoopbackState* state = (WsLoopbackState*)arg;
    galay_ws_client_config_t config = {
        .host = state->peer.address,
        .port = state->peer.port,
        .path = "/chat",
        .connect_timeout_ms = 1000,
    };

    if (galay_ws_client_create(&config, &state->client) != GALAY_OK ||
        state->client == NULL) {
        state->client_connect_result.code = C_IOResultError;
        return;
    }
    state->client_connect_result =
        galay_ws_client_connect(state->client, 1000, &state->client_conn);
    if (state->client_connect_result.code != C_IOResultOk) {
        return;
    }

    state->client_send_text_result =
        galay_ws_connection_send_text(state->client_conn,
                                      (const uint8_t*)request,
                                      sizeof(request) - 1,
                                      1000);
    if (state->client_send_text_result.code != C_IOResultOk) {
        return;
    }
    state->client_recv_text_result =
        galay_ws_connection_recv_frame(state->client_conn, 1000, &state->client_text);
    if (state->client_recv_text_result.code != C_IOResultOk) {
        return;
    }
    state->client_send_close_result =
        galay_ws_connection_send_close(state->client_conn, GALAY_WS_CLOSE_NORMAL, NULL, 0, 1000);
    if (state->client_send_close_result.code != C_IOResultOk) {
        return;
    }
    state->client_recv_close_result =
        galay_ws_connection_recv_frame(state->client_conn, 1000, &state->client_close);
    if (state->client_recv_close_result.code != C_IOResultOk) {
        return;
    }
    state->client_close_result = galay_ws_connection_close(state->client_conn, 1000);
}

static int run_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    WsLoopbackState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    state.listener = &listener;
    state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, ws_server_entry, &state, NULL, &server).code ==
                     C_IOResultOk,
                 4);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, ws_client_entry, &state, NULL, &client).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_join(&server, 3000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_coro_join(&client, 3000).code == C_IOResultOk, 7);

    if (state.accept_result.code != C_IOResultOk ||
        state.server_upgrade_result.code != C_IOResultOk ||
        state.server_recv_text_result.code != C_IOResultOk ||
        state.server_send_text_result.code != C_IOResultOk ||
        state.server_recv_close_result.code != C_IOResultOk ||
        state.server_send_close_result.code != C_IOResultOk ||
        state.server_close_result.code != C_IOResultOk ||
        state.client_connect_result.code != C_IOResultOk ||
        state.client_send_text_result.code != C_IOResultOk ||
        state.client_recv_text_result.code != C_IOResultOk ||
        state.client_send_close_result.code != C_IOResultOk ||
        state.client_recv_close_result.code != C_IOResultOk ||
        state.client_close_result.code != C_IOResultOk ||
        expect_frame_payload(state.server_text, GALAY_WS_OPCODE_TEXT, "hello", 5) != 0 ||
        expect_frame_payload(state.client_text, GALAY_WS_OPCODE_TEXT, "echo:hello", 10) != 0 ||
        galay_ws_received_frame_opcode(state.server_close) != GALAY_WS_OPCODE_CLOSE ||
        galay_ws_received_frame_opcode(state.client_close) != GALAY_WS_OPCODE_CLOSE) {
        result = 8;
    }

    galay_ws_received_frame_destroy(state.server_text);
    galay_ws_received_frame_destroy(state.server_close);
    galay_ws_received_frame_destroy(state.client_text);
    galay_ws_received_frame_destroy(state.client_close);
    if (state.client != NULL) {
        galay_ws_client_destroy(state.client);
    }
    if (state.server_session != NULL) {
        galay_ws_session_destroy(state.server_session);
    }
    if (server.task != NULL && galay_coro_destroy(&server).code != C_IOResultOk && result == 0) {
        result = 9;
    }
    if (client.task != NULL && galay_coro_destroy(&client).code != C_IOResultOk && result == 0) {
        result = 10;
    }
    if (state.accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&state.accepted) != C_TcpSocketSuccess &&
        result == 0) {
        result = 11;
    }
    if (galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess && result == 0) {
        result = 12;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 13;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 14;
    }
    return result;
}

int main(void)
{
    return run_loopback();
}
