#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-mysql-c/mysql.h>

#include <stdio.h>
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
    MYSQL_CAP_CONNECT_WITH_DB = 0x00000008,
    MYSQL_CAP_PLUGIN_AUTH = 0x00080000
};

typedef struct MysqlAuthLoopbackState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    const char* plugin_name;
    size_t expected_auth_len;
    int use_caching_fast_auth;
    int use_caching_full_auth;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult server_send_result;
    C_IOResult server_recv_result;
    C_IOResult server_close_result;
    C_IOResult client_connect_result;
    C_IOResult client_close_result;
    galay_bool_t connected_after_auth;
    unsigned char auth_packet[512];
    size_t auth_packet_len;
    int auth_payload_ok;
} MysqlAuthLoopbackState;

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

static size_t build_handshake(unsigned char* out, const char* plugin)
{
    static const char salt_1[] = "12345678";
    static const char salt_2[] = "abcdefghijkl";
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
    put_u32(out, &pos, 99);
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
    const size_t plugin_len = strlen(plugin);
    memcpy(out + pos, plugin, plugin_len);
    pos += plugin_len;
    out[pos++] = '\0';
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

static int send_exact(galay_kernel_tcp_socket_t* socket, const unsigned char* buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        C_IOResult result = galay_kernel_tcp_socket_send(socket,
                                                         (const char*)buffer + sent,
                                                         length - sent,
                                                         1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        sent += result.bytes;
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

static int check_auth_payload(const unsigned char* packet, size_t packet_len,
                              const char* expected_plugin, size_t expected_auth_len)
{
    const unsigned char* payload = packet + 4;
    const size_t payload_len = packet_len - 4;
    const char* user = NULL;
    const char* database = NULL;
    const char* plugin = NULL;
    uint32_t caps = 0;
    size_t pos = 0;
    size_t user_len = 0;
    size_t database_len = 0;
    size_t plugin_len = 0;
    uint8_t auth_len = 0;

    if (payload_len < 4 + 4 + 1 + 23 + 1) {
        return 0;
    }
    caps = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8u) |
        ((uint32_t)payload[2] << 16u) | ((uint32_t)payload[3] << 24u);
    if ((caps & MYSQL_CAP_PROTOCOL_41) == 0 ||
        (caps & MYSQL_CAP_SECURE_CONNECTION) == 0 ||
        (caps & MYSQL_CAP_PLUGIN_AUTH) == 0 ||
        (caps & MYSQL_CAP_CONNECT_WITH_DB) == 0) {
        return 0;
    }
    pos = 4 + 4 + 1 + 23;
    user = (const char*)payload + pos;
    user_len = strlen(user);
    if (pos + user_len + 1 >= payload_len || strcmp(user, "tester") != 0) {
        return 0;
    }
    pos += user_len + 1;
    auth_len = payload[pos++];
    if (auth_len != expected_auth_len || pos + auth_len >= payload_len) {
        return 0;
    }
    pos += auth_len;
    database = (const char*)payload + pos;
    database_len = strlen(database);
    if (pos + database_len + 1 >= payload_len || strcmp(database, "appdb") != 0) {
        return 0;
    }
    pos += database_len + 1;
    plugin = (const char*)payload + pos;
    plugin_len = strlen(plugin);
    if (pos + plugin_len + 1 > payload_len || strcmp(plugin, expected_plugin) != 0) {
        return 0;
    }
    return 1;
}

static void mysql_auth_server_entry(void* arg)
{
    unsigned char handshake[128];
    static const unsigned char ok_packet[] = {
        0x07, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
    };
    static const unsigned char fast_auth_packet[] = {
        0x02, 0x00, 0x00, 0x02, 0x01, 0x03
    };
    static const unsigned char full_auth_packet[] = {
        0x02, 0x00, 0x00, 0x02, 0x01, 0x04
    };
    static const char public_key_pem[] =
        "-----BEGIN PUBLIC KEY-----\n"
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAwPEGdRCRpZDptJqhD1Zk\n"
        "U6Zz5XhBqL3DGi18XgpjmgJBKsY5Zli75fD9snqkb738i/DLBN1+6UGBeYxdOpT8\n"
        "RE4/8kbTAAilMjp20/B1LI8rMf2g/R4wfVncBw+DaU7LNBtOv39I133DnyyjWX2l\n"
        "BykLLTzHCNrQwPHgx7x73Qo65x9GVkk1QVfAdMH2LTohc4KRHyprsJ5UFzPi2R0j\n"
        "+U8mDTovuptlslCTQg721EOE2iDjfvPRe9qo6t+hb/OtAKa1Hzks7CmXo+8GbNji\n"
        "kVWUbSH3+ubRm0Y4ABMRYrTcMip2KlXWW0Qlgiuiiug5r7M94W0fSFoUh1qDv+x9\n"
        "0QIDAQAB\n"
        "-----END PUBLIC KEY-----\n";
    MysqlAuthLoopbackState* state = (MysqlAuthLoopbackState*)arg;
    const size_t handshake_len = build_handshake(handshake, state->plugin_name);

    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    if (send_exact(&state->accepted, handshake, handshake_len) != 0) {
        state->server_send_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    state->server_send_result = (C_IOResult){C_IOResultOk, 0, handshake_len, 0, NULL};
    if (state->server_send_result.bytes != handshake_len) {
        return;
    }
    state->auth_packet_len = sizeof(state->auth_packet);
    if (recv_packet(&state->accepted, state->auth_packet, &state->auth_packet_len) != 0) {
        state->server_recv_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    state->auth_payload_ok = check_auth_payload(state->auth_packet,
                                                state->auth_packet_len,
                                                state->plugin_name,
                                                state->expected_auth_len);
    state->server_recv_result = (C_IOResult){
        state->auth_payload_ok ? C_IOResultOk : C_IOResultError,
        0,
        0,
        (int64_t)state->auth_packet_len,
        NULL
    };
    if (!state->auth_payload_ok) {
        return;
    }
    if (state->use_caching_fast_auth) {
        if (send_exact(&state->accepted, fast_auth_packet, sizeof(fast_auth_packet)) != 0) {
            state->server_send_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
            return;
        }
        state->server_send_result =
            (C_IOResult){C_IOResultOk, 0, sizeof(fast_auth_packet), 0, NULL};
    }
    if (state->use_caching_full_auth) {
        unsigned char public_key_packet[1024];
        size_t public_key_packet_len = 0;
        if (send_exact(&state->accepted, full_auth_packet, sizeof(full_auth_packet)) != 0) {
            state->server_send_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
            return;
        }
        state->server_send_result =
            (C_IOResult){C_IOResultOk, 0, sizeof(full_auth_packet), 0, NULL};
        state->auth_packet_len = sizeof(state->auth_packet);
        if (recv_packet(&state->accepted, state->auth_packet, &state->auth_packet_len) != 0 ||
            state->auth_packet_len != 5 ||
            state->auth_packet[4] != 0x02) {
            state->server_recv_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
            state->auth_payload_ok = 0;
            return;
        }
        public_key_packet_len = 4;
        public_key_packet[4] = 0x01;
        memcpy(public_key_packet + 5, public_key_pem, sizeof(public_key_pem) - 1);
        public_key_packet_len += 1 + sizeof(public_key_pem) - 1;
        public_key_packet[0] = (unsigned char)((public_key_packet_len - 4) & 0xffu);
        public_key_packet[1] = (unsigned char)(((public_key_packet_len - 4) >> 8u) & 0xffu);
        public_key_packet[2] = (unsigned char)(((public_key_packet_len - 4) >> 16u) & 0xffu);
        public_key_packet[3] = 4;
        if (send_exact(&state->accepted, public_key_packet, public_key_packet_len) != 0) {
            state->server_send_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
            return;
        }
        state->server_send_result =
            (C_IOResult){C_IOResultOk, 0, public_key_packet_len, 0, NULL};
        state->auth_packet_len = sizeof(state->auth_packet);
        if (recv_packet(&state->accepted, state->auth_packet, &state->auth_packet_len) != 0 ||
            state->auth_packet_len <= 4) {
            state->server_recv_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
            state->auth_payload_ok = 0;
            return;
        }
    }
    if (send_exact(&state->accepted, ok_packet, sizeof(ok_packet)) != 0) {
        state->server_send_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    state->server_send_result = (C_IOResult){C_IOResultOk, 0, sizeof(ok_packet), 0, NULL};
    state->server_close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void mysql_auth_client_entry(void* arg)
{
    MysqlAuthLoopbackState* state = (MysqlAuthLoopbackState*)arg;
    galay_mysql_config_t* config = NULL;
    galay_mysql_client_t* client = NULL;

    if (galay_mysql_config_create(&config) != GALAY_OK ||
        galay_mysql_config_set_host(config, state->peer.address) != GALAY_OK ||
        galay_mysql_config_set_port(config, state->peer.port) != GALAY_OK ||
        galay_mysql_config_set_username(config, "tester") != GALAY_OK ||
        galay_mysql_config_set_password(config, "secret") != GALAY_OK ||
        galay_mysql_config_set_database(config, "appdb") != GALAY_OK ||
        galay_mysql_client_create(&client) != GALAY_OK ||
        client == NULL) {
        state->client_connect_result.code = C_IOResultError;
        goto cleanup;
    }

    state->client_connect_result = galay_mysql_client_connect_auth_async(client, config, 1000);
    if (state->client_connect_result.code == C_IOResultOk &&
        galay_mysql_client_is_connected(client, &state->connected_after_auth) != GALAY_OK) {
        state->client_connect_result.code = C_IOResultError;
    }
    if (state->client_connect_result.code == C_IOResultOk) {
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

static int run_loopback(const char* plugin_name,
                        size_t expected_auth_len,
                        int use_caching_fast_auth,
                        int use_caching_full_auth)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    MysqlAuthLoopbackState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    state.listener = &listener;
    state.peer = local;
    state.plugin_name = plugin_name;
    state.expected_auth_len = expected_auth_len;
    state.use_caching_fast_auth = use_caching_fast_auth;
    state.use_caching_full_auth = use_caching_full_auth;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, mysql_auth_server_entry, &state, NULL, &server).code ==
                     C_IOResultOk,
                 4);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, mysql_auth_client_entry, &state, NULL, &client).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_join(&server, 2000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_coro_join(&client, 2000).code == C_IOResultOk, 7);

    if (state.accept_result.code != C_IOResultOk ||
        state.server_send_result.code != C_IOResultOk ||
        state.server_recv_result.code != C_IOResultOk ||
        state.client_connect_result.code != C_IOResultOk ||
        state.connected_after_auth != GALAY_TRUE ||
        state.auth_payload_ok != 1 ||
        state.client_close_result.code != C_IOResultOk) {
        fprintf(stderr,
                "diag plugin=%s accept=%d send=%d send_bytes=%zu recv=%d recv_bytes=%zu "
                "client=%d client_value=%lld connected=%d auth_ok=%d close=%d\n",
                state.plugin_name,
                state.accept_result.code,
                state.server_send_result.code,
                state.server_send_result.bytes,
                state.server_recv_result.code,
                state.server_recv_result.bytes,
                state.client_connect_result.code,
                (long long)state.client_connect_result.value,
                state.connected_after_auth,
                state.auth_payload_ok,
                state.client_close_result.code);
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
    int result = run_loopback("mysql_native_password", 20, 0, 0);
    if (result != 0) {
        return result;
    }
    result = run_loopback("caching_sha2_password", 32, 1, 0);
    if (result != 0) {
        return 100 + result;
    }
    result = run_loopback("caching_sha2_password", 32, 0, 1);
    if (result != 0) {
        return 200 + result;
    }
    return 0;
}
