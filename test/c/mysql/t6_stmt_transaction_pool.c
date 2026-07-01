#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-mysql-c/mysql_c.h>

#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

enum {
    MYSQL_CAP_LONG_PASSWORD = 0x00000001,
    MYSQL_CAP_PROTOCOL_41 = 0x00000200,
    MYSQL_CAP_TRANSACTIONS = 0x00002000,
    MYSQL_CAP_SECURE_CONNECTION = 0x00008000,
    MYSQL_CAP_PLUGIN_AUTH = 0x00080000
};

typedef struct MysqlWorkflowState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult server_result;
    C_IOResult client_result;
    int checked_commands;
    int pipeline_count;
    int reused_lease;
} MysqlWorkflowState;

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

static void put_u16(unsigned char* out, size_t* pos, uint16_t value)
{
    out[(*pos)++] = (unsigned char)(value & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 8u) & 0xffu);
}

static void put_u24(unsigned char* out, size_t* pos, uint32_t value)
{
    out[(*pos)++] = (unsigned char)(value & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 8u) & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 16u) & 0xffu);
}

static void put_u32(unsigned char* out, size_t* pos, uint32_t value)
{
    out[(*pos)++] = (unsigned char)(value & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 8u) & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 16u) & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 24u) & 0xffu);
}

static size_t build_handshake(unsigned char* out)
{
    static const char salt_1[] = "12345678";
    static const char salt_2[] = "abcdefghijkl";
    static const char plugin[] = "mysql_native_password";
    const uint32_t caps = MYSQL_CAP_LONG_PASSWORD | MYSQL_CAP_PROTOCOL_41 |
        MYSQL_CAP_TRANSACTIONS | MYSQL_CAP_SECURE_CONNECTION | MYSQL_CAP_PLUGIN_AUTH;
    size_t payload_len_pos = 0;
    size_t pos = 0;
    put_u24(out, &pos, 0);
    out[pos++] = 0;
    payload_len_pos = pos;
    out[pos++] = 10;
    memcpy(out + pos, "8.0.0-loopback", 14);
    pos += 14;
    out[pos++] = '\0';
    put_u32(out, &pos, 7);
    memcpy(out + pos, salt_1, 8);
    pos += 8;
    out[pos++] = '\0';
    put_u16(out, &pos, (uint16_t)(caps & 0xffffu));
    out[pos++] = 45;
    put_u16(out, &pos, 2);
    put_u16(out, &pos, (uint16_t)((caps >> 16u) & 0xffffu));
    out[pos++] = 21;
    memset(out + pos, 0, 10);
    pos += 10;
    memcpy(out + pos, salt_2, 12);
    pos += 12;
    out[pos++] = '\0';
    memcpy(out + pos, plugin, sizeof(plugin));
    pos += sizeof(plugin);
    out[0] = (unsigned char)((pos - payload_len_pos) & 0xffu);
    out[1] = (unsigned char)(((pos - payload_len_pos) >> 8u) & 0xffu);
    out[2] = (unsigned char)(((pos - payload_len_pos) >> 16u) & 0xffu);
    return pos;
}

static int recv_exact(galay_kernel_tcp_socket_t* socket, unsigned char* buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        C_IOResult result = galay_kernel_tcp_socket_recv(socket,
                                                         (char*)buffer + received,
                                                         length - received,
                                                         1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        received += result.bytes;
    }
    return 0;
}

static int recv_packet(galay_kernel_tcp_socket_t* socket, unsigned char* buffer, size_t* packet_len)
{
    uint32_t payload_len = 0;
    if (recv_exact(socket, buffer, 4) != 0) {
        return 1;
    }
    payload_len = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8u) |
        ((uint32_t)buffer[2] << 16u);
    if (payload_len + 4 > *packet_len) {
        return 2;
    }
    if (recv_exact(socket, buffer + 4, payload_len) != 0) {
        return 3;
    }
    *packet_len = payload_len + 4;
    return 0;
}

static int send_ok(galay_kernel_tcp_socket_t* socket)
{
    static const unsigned char ok_packet[] = {
        0x07, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
    };
    C_IOResult result = galay_kernel_tcp_socket_send(socket,
                                                     (const char*)ok_packet,
                                                     sizeof(ok_packet),
                                                     1000);
    return result.code == C_IOResultOk ? 0 : 1;
}

static int send_prepare_ok(galay_kernel_tcp_socket_t* socket)
{
    unsigned char packet[16];
    size_t pos = 0;
    put_u24(packet, &pos, 12);
    packet[pos++] = 1;
    packet[pos++] = 0x00;
    put_u32(packet, &pos, 42);
    put_u16(packet, &pos, 0);
    put_u16(packet, &pos, 0);
    packet[pos++] = 0;
    put_u16(packet, &pos, 0);
    C_IOResult result = galay_kernel_tcp_socket_send(socket, (const char*)packet, pos, 1000);
    return result.code == C_IOResultOk ? 0 : 1;
}

static int expect_query(galay_kernel_tcp_socket_t* socket, const char* sql)
{
    unsigned char packet[128];
    size_t packet_len = sizeof(packet);
    size_t sql_len = strlen(sql);
    if (recv_packet(socket, packet, &packet_len) != 0) {
        return 1;
    }
    if (packet_len != 5 + sql_len || packet[4] != 0x03 ||
        memcmp(packet + 5, sql, sql_len) != 0) {
        return 2;
    }
    return 0;
}

static int expect_prepare(galay_kernel_tcp_socket_t* socket, const char* sql)
{
    unsigned char packet[128];
    size_t packet_len = sizeof(packet);
    size_t sql_len = strlen(sql);
    if (recv_packet(socket, packet, &packet_len) != 0) {
        return 1;
    }
    if (packet_len != 5 + sql_len || packet[4] != 0x16 ||
        memcmp(packet + 5, sql, sql_len) != 0) {
        return 2;
    }
    return 0;
}

static int expect_execute(galay_kernel_tcp_socket_t* socket, uint32_t stmt_id)
{
    unsigned char packet[64];
    size_t packet_len = sizeof(packet);
    uint32_t got_id = 0;
    if (recv_packet(socket, packet, &packet_len) != 0) {
        return 1;
    }
    if (packet_len < 14 || packet[4] != 0x17) {
        return 2;
    }
    got_id = (uint32_t)packet[5] | ((uint32_t)packet[6] << 8u) |
        ((uint32_t)packet[7] << 16u) | ((uint32_t)packet[8] << 24u);
    return got_id == stmt_id ? 0 : 3;
}

static void mysql_workflow_server_entry(void* arg)
{
    unsigned char handshake[128];
    unsigned char packet[256];
    MysqlWorkflowState* state = (MysqlWorkflowState*)arg;
    size_t packet_len = sizeof(packet);
    const size_t handshake_len = build_handshake(handshake);

    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        state->server_result = state->accept_result;
        return;
    }
    state->server_result = galay_kernel_tcp_socket_send(&state->accepted,
                                                        (const char*)handshake,
                                                        handshake_len,
                                                        1000);
    if (state->server_result.code != C_IOResultOk) {
        return;
    }
    if (recv_packet(&state->accepted, packet, &packet_len) != 0 || send_ok(&state->accepted) != 0) {
        state->server_result = (C_IOResult){C_IOResultError, 0, 0, 1, NULL};
        return;
    }
    if (expect_query(&state->accepted, "START TRANSACTION") != 0 ||
        send_ok(&state->accepted) != 0 ||
        expect_prepare(&state->accepted, "SELECT ?") != 0 ||
        send_prepare_ok(&state->accepted) != 0 ||
        expect_execute(&state->accepted, 42) != 0 ||
        send_ok(&state->accepted) != 0 ||
        expect_query(&state->accepted, "SELECT 1") != 0 ||
        expect_query(&state->accepted, "SELECT 2") != 0 ||
        send_ok(&state->accepted) != 0 ||
        send_ok(&state->accepted) != 0 ||
        expect_query(&state->accepted, "COMMIT") != 0 ||
        send_ok(&state->accepted) != 0 ||
        expect_query(&state->accepted, "ROLLBACK") != 0 ||
        send_ok(&state->accepted) != 0) {
        state->server_result = (C_IOResult){C_IOResultError, 0, 0, 2, NULL};
        return;
    }
    state->checked_commands = 7;
    state->server_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void mysql_workflow_client_entry(void* arg)
{
    MysqlWorkflowState* state = (MysqlWorkflowState*)arg;
    galay_mysql_config_t* config = NULL;
    galay_mysql_pool_t* pool = NULL;
    galay_mysql_pool_lease_t* lease = NULL;
    galay_mysql_pool_lease_t* second_lease = NULL;
    galay_mysql_client_t* client = NULL;
    galay_mysql_client_t* second_client = NULL;
    galay_mysql_stmt_t* stmt = NULL;
    galay_mysql_pipeline_t* pipeline = NULL;
    galay_mysql_pipeline_result_t* pipeline_result = NULL;
    galay_mysql_result_set_t* result = NULL;
    uint32_t statement_id = 0;
    size_t result_count = 0;

    if (galay_mysql_config_create(&config) != GALAY_OK ||
        galay_mysql_config_set_host(config, state->peer.address) != GALAY_OK ||
        galay_mysql_config_set_port(config, state->peer.port) != GALAY_OK ||
        galay_mysql_config_set_username(config, "tester") != GALAY_OK ||
        galay_mysql_config_set_password(config, "secret") != GALAY_OK ||
        galay_mysql_pool_create(config, 1, &pool) != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 1, NULL};
        goto cleanup;
    }
    state->client_result = galay_mysql_pool_acquire_async(pool, 1000, &lease);
    if (state->client_result.code != C_IOResultOk ||
        galay_mysql_pool_lease_client(lease, &client) != GALAY_OK ||
        client == NULL) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 2, NULL};
        goto cleanup;
    }
    state->client_result = galay_mysql_client_begin_transaction_async(client, 1000, &result);
    if (state->client_result.code != C_IOResultOk) {
        goto cleanup;
    }
    galay_mysql_result_set_destroy(result);
    result = NULL;
    state->client_result = galay_mysql_client_stmt_prepare_async(client, "SELECT ?", 1000, &stmt);
    if (state->client_result.code != C_IOResultOk ||
        galay_mysql_stmt_id(stmt, &statement_id) != GALAY_OK ||
        statement_id != 42) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 3, NULL};
        goto cleanup;
    }
    state->client_result =
        galay_mysql_client_stmt_execute_async(client, stmt, NULL, 0, 1000, &result);
    if (state->client_result.code != C_IOResultOk) {
        goto cleanup;
    }
    galay_mysql_result_set_destroy(result);
    result = NULL;
    if (galay_mysql_pipeline_create(&pipeline) != GALAY_OK ||
        galay_mysql_pipeline_append_query(pipeline, "SELECT 1") != GALAY_OK ||
        galay_mysql_pipeline_append_query(pipeline, "SELECT 2") != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 4, NULL};
        goto cleanup;
    }
    state->client_result =
        galay_mysql_client_pipeline_async(client, pipeline, 1000, &pipeline_result);
    if (state->client_result.code != C_IOResultOk ||
        galay_mysql_pipeline_result_count(pipeline_result, &result_count) != GALAY_OK ||
        result_count != 2) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 5, NULL};
        goto cleanup;
    }
    state->pipeline_count = (int)result_count;
    state->client_result = galay_mysql_client_commit_async(client, 1000, &result);
    if (state->client_result.code != C_IOResultOk) {
        goto cleanup;
    }
    galay_mysql_result_set_destroy(result);
    result = NULL;
    if (galay_mysql_pool_lease_release(lease) != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 6, NULL};
        lease = NULL;
        goto cleanup;
    }
    lease = NULL;
    state->client_result = galay_mysql_pool_acquire_async(pool, 1000, &second_lease);
    if (state->client_result.code != C_IOResultOk ||
        galay_mysql_pool_lease_client(second_lease, &second_client) != GALAY_OK ||
        second_client != client) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 7, NULL};
        goto cleanup;
    }
    state->reused_lease = 1;
    state->client_result = galay_mysql_client_rollback_async(second_client, 1000, &result);

cleanup:
    if (result != NULL) {
        galay_mysql_result_set_destroy(result);
    }
    if (pipeline_result != NULL) {
        galay_mysql_pipeline_result_destroy(pipeline_result);
    }
    if (pipeline != NULL) {
        galay_mysql_pipeline_destroy(pipeline);
    }
    if (stmt != NULL) {
        galay_mysql_stmt_destroy(stmt);
    }
    if (second_lease != NULL) {
        if (galay_mysql_pool_lease_release(second_lease) != GALAY_OK &&
            state->client_result.code == C_IOResultOk) {
            state->client_result = (C_IOResult){C_IOResultError, 0, 0, 8, NULL};
        }
    }
    if (lease != NULL) {
        if (galay_mysql_pool_lease_release(lease) != GALAY_OK &&
            state->client_result.code == C_IOResultOk) {
            state->client_result = (C_IOResult){C_IOResultError, 0, 0, 9, NULL};
        }
    }
    if (pool != NULL) {
        galay_mysql_pool_destroy(pool);
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
    MysqlWorkflowState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    state.listener = &listener;
    state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, mysql_workflow_server_entry, &state, NULL, &server).code ==
                     C_IOResultOk,
                 4);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, mysql_workflow_client_entry, &state, NULL, &client).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_join(&server, 3000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_coro_join(&client, 3000).code == C_IOResultOk, 7);

    if (state.accept_result.code != C_IOResultOk ||
        state.server_result.code != C_IOResultOk ||
        state.client_result.code != C_IOResultOk ||
        state.checked_commands != 7 ||
        state.pipeline_count != 2 ||
        state.reused_lease != 1) {
        result = 8;
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
