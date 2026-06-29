#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-mysql-c/mysql.h>

#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

typedef struct MysqlLoopbackState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult server_send_result;
    C_IOResult server_recv_result;
    C_IOResult server_close_result;
    C_IOResult client_connect_result;
    C_IOResult client_query_result;
    C_IOResult client_close_result;
    galay_bool_t connected_after_connect;
    galay_mysql_buffer_t* result_packet;
    char request[64];
} MysqlLoopbackState;

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

static int recv_exact(galay_kernel_tcp_socket_t* socket, char* buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        C_IOResult result =
            galay_kernel_tcp_socket_recv(socket, buffer + received, length - received, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        received += result.bytes;
    }
    return 0;
}

static void mysql_server_entry(void* arg)
{
    static const unsigned char handshake_packet[] = {
        0x05, 0x00, 0x00, 0x00, 'H', 'E', 'L', 'L', 'O'
    };
    static const unsigned char expected_query[] = {
        0x09, 0x00, 0x00, 0x00, 0x03, 'S', 'E', 'L', 'E', 'C', 'T', ' ', '1'
    };
    static const unsigned char ok_packet[] = {0x01, 0x00, 0x00, 0x01, 0x00};
    MysqlLoopbackState* state = (MysqlLoopbackState*)arg;

    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    state->server_send_result = galay_kernel_tcp_socket_send(&state->accepted,
                                                             (const char*)handshake_packet,
                                                             sizeof(handshake_packet),
                                                             1000);
    if (state->server_send_result.code != C_IOResultOk) {
        return;
    }
    if (recv_exact(&state->accepted, state->request, sizeof(expected_query)) != 0 ||
        memcmp(state->request, expected_query, sizeof(expected_query)) != 0) {
        state->server_recv_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    state->server_recv_result = (C_IOResult){C_IOResultOk, 0, 0, sizeof(expected_query), NULL};
    state->server_send_result = galay_kernel_tcp_socket_send(&state->accepted,
                                                             (const char*)ok_packet,
                                                             sizeof(ok_packet),
                                                             1000);
    if (state->server_send_result.code != C_IOResultOk) {
        return;
    }
    state->server_close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void mysql_client_entry(void* arg)
{
    MysqlLoopbackState* state = (MysqlLoopbackState*)arg;
    galay_mysql_config_t* config = NULL;
    galay_mysql_client_t* client = NULL;

    if (galay_mysql_config_create(&config) != GALAY_OK ||
        galay_mysql_config_set_host(config, state->peer.address) != GALAY_OK ||
        galay_mysql_config_set_port(config, state->peer.port) != GALAY_OK ||
        galay_mysql_client_create(&client) != GALAY_OK ||
        client == NULL) {
        state->client_connect_result.code = C_IOResultError;
        goto cleanup;
    }

    state->client_connect_result = galay_mysql_client_connect_async(client, config, 1000);
    if (state->client_connect_result.code == C_IOResultOk &&
        galay_mysql_client_is_connected(client, &state->connected_after_connect) != GALAY_OK) {
        state->client_connect_result.code = C_IOResultError;
    }
    if (state->client_connect_result.code == C_IOResultOk) {
        state->client_query_result =
            galay_mysql_client_query_async(client, "SELECT 1", 1000, &state->result_packet);
        state->client_close_result = galay_mysql_client_close_async(client, 1000);
    }

cleanup:
    if (client != NULL) {
        galay_mysql_client_destroy(client);
    }
    if (config != NULL) {
        galay_mysql_config_destroy(config);
    }
}

static int run_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    MysqlLoopbackState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    state.listener = &listener;
    state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, mysql_server_entry, &state, NULL, &server).code ==
                     C_IOResultOk,
                 4);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, mysql_client_entry, &state, NULL, &client).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_join(&server, 2000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_coro_join(&client, 2000).code == C_IOResultOk, 7);

    const unsigned char* packet = NULL;
    size_t packet_len = 0;
    galay_mysql_packet_view_t view = {0};
    if (state.accept_result.code != C_IOResultOk ||
        state.server_send_result.code != C_IOResultOk ||
        state.server_recv_result.code != C_IOResultOk ||
        state.server_close_result.code != C_IOResultOk ||
        state.client_connect_result.code != C_IOResultOk ||
        state.client_query_result.code != C_IOResultOk ||
        state.client_close_result.code != C_IOResultOk ||
        state.connected_after_connect != GALAY_TRUE ||
        state.result_packet == NULL ||
        galay_mysql_buffer_data(state.result_packet, &packet, &packet_len) != GALAY_OK ||
        galay_mysql_extract_packet(packet, packet_len, &view) != GALAY_OK ||
        view.sequence_id != 1 ||
        view.payload_len != 1 ||
        view.payload[0] != 0x00) {
        result = 8;
    }

    if (state.result_packet != NULL) {
        galay_mysql_buffer_destroy(state.result_packet);
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
