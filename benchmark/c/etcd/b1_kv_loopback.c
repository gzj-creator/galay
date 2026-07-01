#include <galay/c/galay-etcd-c/etcd_c.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>
#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "require failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return (code); \
        } \
    } while (0)

typedef struct EtcdKvLoopbackState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult recv_results[3];
    C_IOResult send_results[3];
    C_IOResult close_result;
    galay_status_t connect_status;
    galay_status_t put_status;
    galay_status_t get_status;
    galay_status_t delete_status;
    galay_etcd_error_code_t connect_code;
    galay_etcd_error_code_t put_code;
    galay_etcd_error_code_t get_code;
    galay_etcd_error_code_t delete_code;
    int64_t deleted_count;
    size_t kv_count;
    char request[1024];
    char observed_value[32];
} EtcdKvLoopbackState;

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

static int request_contains(const char* request, size_t request_len, const char* needle)
{
    const size_t needle_len = strlen(needle);
    size_t i = 0;
    if (needle_len == 0 || request_len < needle_len) {
        return 0;
    }
    for (i = 0; i + needle_len <= request_len; ++i) {
        if (memcmp(request + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static C_IOResult send_http(galay_kernel_tcp_socket_t* socket, const char* body)
{
    char response[512];
    int len = snprintf(response,
                       sizeof(response),
                       "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                       "Content-Type: application/json\r\nConnection: keep-alive\r\n\r\n%s",
                       strlen(body),
                       body);
    if (len <= 0 || (size_t)len >= sizeof(response)) {
        return (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
    }
    return galay_kernel_tcp_socket_send(socket, response, (size_t)len, 1000);
}

static void etcd_kv_server_entry(void* arg)
{
    static const char* bodies[3] = {
        "{}",
        "{\"kvs\":[{\"key\":\"Zm9v\",\"value\":\"YmFy\"}],\"count\":\"1\"}",
        "{\"deleted\":\"1\"}",
    };
    EtcdKvLoopbackState* state = (EtcdKvLoopbackState*)arg;
    size_t i = 0;

    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    for (i = 0; i < 3; ++i) {
        memset(state->request, 0, sizeof(state->request));
        state->recv_results[i] =
            galay_kernel_tcp_socket_recv(&state->accepted,
                                         state->request,
                                         sizeof(state->request) - 1,
                                         1000);
        if (state->recv_results[i].code != C_IOResultOk) {
            return;
        }
        if (i == 0 &&
            (!request_contains(state->request, state->recv_results[i].bytes, "POST /v3/kv/put") ||
             !request_contains(state->request, state->recv_results[i].bytes, "\"key\":\"Zm9v\"") ||
             !request_contains(state->request, state->recv_results[i].bytes, "\"value\":\"YmFy\""))) {
            state->recv_results[i].code = C_IOResultError;
            return;
        }
        if (i == 1 &&
            !request_contains(state->request, state->recv_results[i].bytes, "POST /v3/kv/range")) {
            state->recv_results[i].code = C_IOResultError;
            return;
        }
        if (i == 2 &&
            !request_contains(state->request, state->recv_results[i].bytes, "POST /v3/kv/deleterange")) {
            state->recv_results[i].code = C_IOResultError;
            return;
        }
        state->send_results[i] = send_http(&state->accepted, bodies[i]);
        if (state->send_results[i].code != C_IOResultOk) {
            return;
        }
    }
    state->close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void etcd_kv_client_entry(void* arg)
{
    EtcdKvLoopbackState* state = (EtcdKvLoopbackState*)arg;
    char endpoint[64];
    galay_etcd_config_builder_t* builder = NULL;
    galay_etcd_client_t* client = NULL;
    galay_etcd_get_result_t* result = NULL;
    const char* key = NULL;
    const char* value = NULL;
    size_t key_len = 0;
    size_t value_len = 0;
    int len = snprintf(endpoint, sizeof(endpoint), "http://%s:%u", state->peer.address, state->peer.port);

    if (len <= 0 || (size_t)len >= sizeof(endpoint)) {
        state->connect_status = GALAY_INTERNAL_ERROR;
        return;
    }
    if (galay_etcd_config_builder_create(&builder) != GALAY_OK ||
        galay_etcd_config_builder_set_endpoint(builder, endpoint) != GALAY_OK ||
        galay_etcd_client_create(builder, &client) != GALAY_OK) {
        state->connect_status = GALAY_INTERNAL_ERROR;
        goto cleanup;
    }

    state->connect_status = galay_etcd_client_connect(client, &state->connect_code);
    if (state->connect_status == GALAY_OK) {
        state->put_status =
            galay_etcd_client_put(client, "foo", "bar", strlen("bar"), &state->put_code);
        state->get_status =
            galay_etcd_client_get(client, "foo", GALAY_FALSE, 0, &result, &state->get_code);
        if (state->get_status == GALAY_OK &&
            galay_etcd_get_result_count(result, &state->kv_count) == GALAY_OK &&
            galay_etcd_get_result_item(result, 0, &key, &key_len, &value, &value_len) == GALAY_OK &&
            value_len < sizeof(state->observed_value)) {
            memcpy(state->observed_value, value, value_len);
            state->observed_value[value_len] = '\0';
        }
        state->delete_status =
            galay_etcd_client_delete(client, "foo", GALAY_FALSE, &state->deleted_count, &state->delete_code);
    }

cleanup:
    if (result != NULL) {
        galay_etcd_get_result_destroy(result);
    }
    galay_etcd_client_destroy(client);
    galay_etcd_config_builder_destroy(builder);
}

static int run_kv_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    EtcdKvLoopbackState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    state.listener = &listener;
    state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, etcd_kv_server_entry, &state, NULL, &server).code ==
                     C_IOResultOk,
                 4);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, etcd_kv_client_entry, &state, NULL, &client).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_join(&client, 2000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_coro_join(&server, 2000).code == C_IOResultOk, 7);

    if (state.accept_result.code != C_IOResultOk ||
        state.recv_results[0].code != C_IOResultOk ||
        state.recv_results[1].code != C_IOResultOk ||
        state.recv_results[2].code != C_IOResultOk ||
        state.send_results[0].code != C_IOResultOk ||
        state.send_results[1].code != C_IOResultOk ||
        state.send_results[2].code != C_IOResultOk ||
        state.connect_status != GALAY_OK ||
        state.put_status != GALAY_OK ||
        state.get_status != GALAY_OK ||
        state.delete_status != GALAY_OK ||
        state.connect_code != GALAY_ETCD_ERROR_SUCCESS ||
        state.put_code != GALAY_ETCD_ERROR_SUCCESS ||
        state.get_code != GALAY_ETCD_ERROR_SUCCESS ||
        state.delete_code != GALAY_ETCD_ERROR_SUCCESS ||
        state.kv_count != 1 ||
        strcmp(state.observed_value, "bar") != 0 ||
        state.deleted_count != 1) {
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
    return run_kv_loopback();
}
