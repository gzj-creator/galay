#include <galay/c/galay-kernel-c/async-c/tcp_socket.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>
#include <galay/c/galay-postgres-c/postgres.h>

#include <stdint.h>
#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) return (code); \
    } while (0)

typedef struct PostgresLoopbackState {
    galay_c_tcp_socket_t* listener;
    C_Host peer;
    galay_c_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult server_result;
    C_IOResult client_result;
    int startup_ok;
    int auth_ok;
    int commands_checked;
    int error_drained;
    int result_values_ok;
} PostgresLoopbackState;

static uint32_t read_u32(const unsigned char* data)
{
    return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
        ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static void write_u32(unsigned char* data, uint32_t value)
{
    data[0] = (unsigned char)((value >> 24u) & 0xffu);
    data[1] = (unsigned char)((value >> 16u) & 0xffu);
    data[2] = (unsigned char)((value >> 8u) & 0xffu);
    data[3] = (unsigned char)(value & 0xffu);
}

static int create_listener(galay_c_tcp_socket_t* listener, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_c_tcp_socket_create(listener, C_IPTypeIPV4).code == C_IOResultOk &&
        galay_c_tcp_socket_bind(listener, &bind_host).code == C_IOResultOk &&
        galay_c_tcp_socket_listen(listener, 16).code == C_IOResultOk &&
        galay_c_tcp_socket_local_endpoint(listener, local).code == C_IOResultOk &&
        local->port != 0 ? 0 : 1;
}

static int recv_exact(galay_c_tcp_socket_t* socket, unsigned char* data, size_t data_len)
{
    size_t received = 0;
    while (received < data_len) {
        C_IOResult result = galay_c_tcp_socket_recv(
            socket, (char*)data + received, data_len - received, 2000);
        if (result.code != C_IOResultOk || result.bytes == 0) return 1;
        received += result.bytes;
    }
    return 0;
}

static int send_exact(galay_c_tcp_socket_t* socket,
                      const unsigned char* data,
                      size_t data_len)
{
    size_t sent = 0;
    while (sent < data_len) {
        C_IOResult result = galay_c_tcp_socket_send(
            socket, (const char*)data + sent, data_len - sent, 2000);
        if (result.code != C_IOResultOk || result.bytes == 0) return 1;
        sent += result.bytes;
    }
    return 0;
}

static int recv_frontend(galay_c_tcp_socket_t* socket,
                         unsigned char* data,
                         size_t capacity,
                         size_t* data_len)
{
    if (capacity < 5 || recv_exact(socket, data, 5) != 0) return 1;
    const uint32_t length = read_u32(data + 1);
    if (length < 4 || 1u + length > capacity) return 2;
    if (recv_exact(socket, data + 5, length - 4u) != 0) return 3;
    *data_len = 1u + length;
    return 0;
}

static int send_backend(galay_c_tcp_socket_t* socket,
                        unsigned char type,
                        const unsigned char* payload,
                        size_t payload_len)
{
    unsigned char header[5];
    header[0] = type;
    write_u32(header + 1, (uint32_t)(4u + payload_len));
    if (send_exact(socket, header, sizeof(header)) != 0) return 1;
    return payload_len == 0 || send_exact(socket, payload, payload_len) == 0 ? 0 : 2;
}

static int recv_startup(galay_c_tcp_socket_t* socket)
{
    static const unsigned char expected_user[] = {'u', 's', 'e', 'r', 0x00,
                                                   't', 'e', 's', 't', 'e', 'r', 0x00};
    unsigned char packet[256];
    if (recv_exact(socket, packet, 4) != 0) return 0;
    const uint32_t length = read_u32(packet);
    if (length < 9 || length > sizeof(packet) ||
        recv_exact(socket, packet + 4, length - 4u) != 0 ||
        read_u32(packet + 4) != 196608u) return 0;
    for (size_t position = 8; position + sizeof(expected_user) <= length; ++position) {
        if (memcmp(packet + position, expected_user, sizeof(expected_user)) == 0) return 1;
    }
    return 0;
}

static int expect_frontend(galay_c_tcp_socket_t* socket,
                           unsigned char expected_type,
                           const char* expected_payload)
{
    unsigned char message[256];
    size_t message_len = 0;
    if (recv_frontend(socket, message, sizeof(message), &message_len) != 0 ||
        message[0] != expected_type) return 0;
    const size_t payload_len = message_len - 5u;
    const size_t expected_len = expected_payload == NULL ? 0 : strlen(expected_payload) + 1u;
    return payload_len == expected_len &&
        (expected_len == 0 || memcmp(message + 5, expected_payload, expected_len) == 0);
}

static int send_ready(galay_c_tcp_socket_t* socket)
{
    static const unsigned char ready[] = {'I'};
    return send_backend(socket, 'Z', ready, sizeof(ready));
}

static int send_query_result(galay_c_tcp_socket_t* socket, unsigned char value)
{
    static const unsigned char description[] = {
        0x00, 0x01, 'v', 'a', 'l', 'u', 'e', 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x17, 0x00, 0x04,
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
    };
    unsigned char row[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x01, value};
    static const unsigned char complete[] = {'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0x00};
    return send_backend(socket, 'T', description, sizeof(description)) == 0 &&
        send_backend(socket, 'D', row, sizeof(row)) == 0 &&
        send_backend(socket, 'C', complete, sizeof(complete)) == 0 &&
        send_ready(socket) == 0 ? 0 : 1;
}

static int send_query_error(galay_c_tcp_socket_t* socket)
{
    static const unsigned char error[] = {
        'S', 'E', 'R', 'R', 'O', 'R', 0x00,
        'C', '4', '2', '6', '0', '1', 0x00,
        'M', 'b', 'a', 'd', ' ', 's', 'y', 'n', 't', 'a', 'x', 0x00,
        0x00,
    };
    return send_backend(socket, 'E', error, sizeof(error)) == 0 && send_ready(socket) == 0
        ? 0 : 1;
}

static void postgres_server_entry(void* arg)
{
    static const unsigned char cleartext[] = {0x00, 0x00, 0x00, 0x03};
    static const unsigned char auth_ok[] = {0x00, 0x00, 0x00, 0x00};
    static const unsigned char parameter[] = {
        's', 'e', 'r', 'v', 'e', 'r', '_', 'v', 'e', 'r', 's', 'i', 'o', 'n', 0x00,
        '1', '6', '.', '0', 0x00,
    };
    PostgresLoopbackState* state = (PostgresLoopbackState*)arg;
    unsigned char password[128];
    size_t password_len = 0;

    state->accept_result =
        galay_c_tcp_socket_accept(state->listener, &state->accepted, NULL, 2000);
    if (state->accept_result.code != C_IOResultOk) return;
    state->startup_ok = recv_startup(&state->accepted);
    if (!state->startup_ok || send_backend(&state->accepted, 'R', cleartext, sizeof(cleartext)) != 0 ||
        recv_frontend(&state->accepted, password, sizeof(password), &password_len) != 0 ||
        password[0] != 'p' || password_len != 12 || memcmp(password + 5, "secret\0", 7) != 0) {
        state->server_result = (C_IOResult){C_IOResultError, 0, 0, 1, NULL};
        return;
    }
    state->auth_ok = 1;
    if (send_backend(&state->accepted, 'R', auth_ok, sizeof(auth_ok)) != 0 ||
        send_backend(&state->accepted, 'S', parameter, sizeof(parameter)) != 0 ||
        send_ready(&state->accepted) != 0) {
        state->server_result = (C_IOResult){C_IOResultError, 0, 0, 2, NULL};
        return;
    }
    if (!expect_frontend(&state->accepted, 'Q', "SELECT 1") ||
        send_query_result(&state->accepted, '1') != 0 ||
        !expect_frontend(&state->accepted, 'Q', "BAD SQL") ||
        send_query_error(&state->accepted) != 0 ||
        !expect_frontend(&state->accepted, 'Q', "SELECT 2") ||
        send_query_result(&state->accepted, '2') != 0 ||
        !expect_frontend(&state->accepted, 'X', NULL)) {
        state->server_result = (C_IOResult){C_IOResultError, 0, 0, 3, NULL};
        return;
    }
    state->commands_checked = 4;
    state->server_result = galay_c_tcp_socket_close(&state->accepted);
}

static int result_equals(galay_postgres_result_set_t* result, const char* expected)
{
    galay_postgres_value_view_t value = {0};
    return result != NULL && galay_postgres_result_set_value(result, 0, 0, &value) == GALAY_OK &&
        value.is_null == GALAY_FALSE && value.data_len == strlen(expected) &&
        memcmp(value.data, expected, value.data_len) == 0;
}

static void postgres_client_entry(void* arg)
{
    PostgresLoopbackState* state = (PostgresLoopbackState*)arg;
    galay_postgres_config_t* config = NULL;
    galay_postgres_pool_t* pool = NULL;
    galay_postgres_pool_lease_t* lease = NULL;
    galay_postgres_client_t* client = NULL;
    galay_postgres_result_set_t* result = NULL;
    galay_postgres_result_set_t* reusable_result = NULL;

    if (galay_postgres_config_create(&config) != GALAY_OK ||
        galay_postgres_config_set_host(config, state->peer.address) != GALAY_OK ||
        galay_postgres_config_set_port(config, state->peer.port) != GALAY_OK ||
        galay_postgres_config_set_username(config, "tester") != GALAY_OK ||
        galay_postgres_config_set_password(config, "secret") != GALAY_OK ||
        galay_postgres_config_set_database(config, "app") != GALAY_OK ||
        galay_postgres_result_set_create(&reusable_result) != GALAY_OK ||
        galay_postgres_pool_create(config, 1, &pool) != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 1, NULL};
        goto cleanup;
    }
    state->client_result = galay_postgres_pool_acquire_async(pool, 2000, &lease);
    if (state->client_result.code != C_IOResultOk ||
        galay_postgres_pool_lease_client(lease, &client) != GALAY_OK || client == NULL) goto cleanup;

    state->client_result = galay_postgres_client_query_into_async(
        client, "SELECT 1", 2000, reusable_result);
    if (state->client_result.code != C_IOResultOk ||
        !result_equals(reusable_result, "1")) goto cleanup;

    state->client_result = galay_postgres_client_query_async(client, "BAD SQL", 2000, &result);
    if (state->client_result.code != C_IOResultError ||
        state->client_result.value != GALAY_PROTOCOL_ERROR || result != NULL) goto cleanup;
    state->error_drained = 1;

    state->client_result = galay_postgres_client_query_into_async(
        client, "SELECT 2", 2000, reusable_result);
    if (state->client_result.code != C_IOResultOk ||
        !result_equals(reusable_result, "2")) goto cleanup;
    state->result_values_ok = 1;
    state->client_result = galay_postgres_client_close_async(client, 2000);

cleanup:
    galay_postgres_result_set_destroy(result);
    galay_postgres_result_set_destroy(reusable_result);
    if (lease != NULL) {
        const galay_status_t released = galay_postgres_pool_lease_release(lease);
        if (released != GALAY_OK && state->client_result.code == C_IOResultOk) {
            state->client_result = (C_IOResult){C_IOResultError, 0, 0, released, NULL};
        }
    }
    galay_postgres_pool_destroy(pool);
    galay_postgres_config_destroy(config);
}

int main(void)
{
    C_RuntimeConfig runtime_config = galay_c_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_tcp_socket_t listener = {0};
    C_Host local = {0};
    PostgresLoopbackState state = {0};
    galay_c_coro_task_t server = {0};
    galay_c_coro_task_t client = {0};
    int result = 0;

    REQUIRE_TRUE(galay_c_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_c_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    state.listener = &listener;
    state.peer = local;
    REQUIRE_TRUE(galay_c_coro_spawn(&runtime, postgres_server_entry, &state, NULL, &server).code ==
                     C_IOResultOk, 4);
    REQUIRE_TRUE(galay_c_coro_spawn(&runtime, postgres_client_entry, &state, NULL, &client).code ==
                     C_IOResultOk, 5);
    REQUIRE_TRUE(galay_c_coro_join(&server, 5000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_c_coro_join(&client, 5000).code == C_IOResultOk, 7);

    if (state.accept_result.code != C_IOResultOk || state.server_result.code != C_IOResultOk ||
        state.client_result.code != C_IOResultOk || !state.startup_ok || !state.auth_ok ||
        state.commands_checked != 4 || !state.error_drained || !state.result_values_ok) result = 8;
    if (server.task != NULL && galay_c_coro_destroy(&server).code != C_IOResultOk && result == 0) result = 9;
    if (client.task != NULL && galay_c_coro_destroy(&client).code != C_IOResultOk && result == 0) result = 10;
    if (state.accepted.fd >= 0 &&
        galay_c_tcp_socket_close(&state.accepted).code != C_IOResultOk && result == 0) result = 11;
    if (galay_c_tcp_socket_close(&listener).code != C_IOResultOk && result == 0) result = 12;
    if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) result = 13;
    if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) result = 14;
    return result;
}
