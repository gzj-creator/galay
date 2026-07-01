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

typedef struct EtcdLeasePipelineState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult recv_results[6];
    C_IOResult send_results[6];
    galay_status_t grant_status;
    galay_status_t keepalive_status;
    galay_status_t revoke_status;
    galay_status_t pipeline_status;
    int64_t grant_id;
    int64_t keepalive_id;
    size_t pipeline_count;
    int64_t pipeline_deleted;
    char pipeline_value[32];
    char request[1024];
} EtcdLeasePipelineState;

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

static int contains_bytes(const char* text, size_t text_len, const char* needle)
{
    size_t needle_len = strlen(needle);
    size_t i = 0;
    if (text_len < needle_len) {
        return 0;
    }
    for (i = 0; i + needle_len <= text_len; ++i) {
        if (memcmp(text + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static C_IOResult send_json(galay_kernel_tcp_socket_t* socket, const char* body)
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

static void lease_pipeline_server_entry(void* arg)
{
    static const char* paths[6] = {
        "POST /v3/lease/grant",
        "POST /v3/lease/keepalive",
        "POST /v3/lease/revoke",
        "POST /v3/kv/put",
        "POST /v3/kv/range",
        "POST /v3/kv/deleterange",
    };
    static const char* bodies[6] = {
        "{\"ID\":\"77\",\"TTL\":\"5\"}",
        "{\"result\":{\"ID\":\"77\",\"TTL\":\"5\"}}",
        "{}",
        "{}",
        "{\"kvs\":[{\"key\":\"cGlwZQ==\",\"value\":\"dmFsdWU=\"}],\"count\":\"1\"}",
        "{\"deleted\":\"1\"}",
    };
    EtcdLeasePipelineState* state = (EtcdLeasePipelineState*)arg;
    size_t i = 0;

    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    for (i = 0; i < 6; ++i) {
        memset(state->request, 0, sizeof(state->request));
        state->recv_results[i] =
            galay_kernel_tcp_socket_recv(&state->accepted,
                                         state->request,
                                         sizeof(state->request) - 1,
                                         1000);
        if (state->recv_results[i].code != C_IOResultOk ||
            !contains_bytes(state->request, state->recv_results[i].bytes, paths[i])) {
            state->recv_results[i].code = C_IOResultError;
            return;
        }
        state->send_results[i] = send_json(&state->accepted, bodies[i]);
        if (state->send_results[i].code != C_IOResultOk) {
            return;
        }
    }
}

static void lease_pipeline_client_entry(void* arg)
{
    EtcdLeasePipelineState* state = (EtcdLeasePipelineState*)arg;
    char endpoint[64];
    galay_etcd_config_builder_t* builder = NULL;
    galay_etcd_client_t* client = NULL;
    galay_etcd_pipeline_t* pipeline = NULL;
    galay_etcd_pipeline_result_t* pipeline_result = NULL;
    const galay_etcd_get_result_t* get_result = NULL;
    const char* key = NULL;
    const char* value = NULL;
    size_t key_len = 0;
    size_t value_len = 0;
    galay_etcd_error_code_t code = GALAY_ETCD_ERROR_SUCCESS;
    int len = snprintf(endpoint, sizeof(endpoint), "http://%s:%u", state->peer.address, state->peer.port);

    if (len <= 0 || (size_t)len >= sizeof(endpoint)) {
        state->grant_status = GALAY_INTERNAL_ERROR;
        return;
    }
    if (galay_etcd_config_builder_create(&builder) != GALAY_OK ||
        galay_etcd_config_builder_set_endpoint(builder, endpoint) != GALAY_OK ||
        galay_etcd_client_create(builder, &client) != GALAY_OK ||
        galay_etcd_client_connect(client, &code) != GALAY_OK) {
        state->grant_status = GALAY_INTERNAL_ERROR;
        goto cleanup;
    }

    state->grant_status = galay_etcd_client_lease_grant(client, 5, &state->grant_id, &code);
    state->keepalive_status =
        galay_etcd_client_lease_keepalive(client, state->grant_id, &state->keepalive_id, &code);
    state->revoke_status = galay_etcd_client_lease_revoke(client, state->grant_id, &code);

    if (galay_etcd_pipeline_create(&pipeline) != GALAY_OK ||
        galay_etcd_pipeline_add_put(pipeline, "pipe", "value", strlen("value"), 0) != GALAY_OK ||
        galay_etcd_pipeline_add_get(pipeline, "pipe", GALAY_FALSE, 0) != GALAY_OK ||
        galay_etcd_pipeline_add_delete(pipeline, "pipe", GALAY_FALSE) != GALAY_OK) {
        state->pipeline_status = GALAY_INTERNAL_ERROR;
        goto cleanup;
    }
    state->pipeline_status =
        galay_etcd_client_pipeline_execute(client, pipeline, &pipeline_result, &code);
    if (state->pipeline_status == GALAY_OK &&
        galay_etcd_pipeline_result_count(pipeline_result, &state->pipeline_count) == GALAY_OK &&
        galay_etcd_pipeline_result_item_get_result(pipeline_result, 1, &get_result) == GALAY_OK &&
        galay_etcd_get_result_item(get_result, 0, &key, &key_len, &value, &value_len) == GALAY_OK &&
        value_len < sizeof(state->pipeline_value) &&
        galay_etcd_pipeline_result_item_deleted_count(pipeline_result, 2, &state->pipeline_deleted) == GALAY_OK) {
        memcpy(state->pipeline_value, value, value_len);
        state->pipeline_value[value_len] = '\0';
    }

cleanup:
    galay_etcd_pipeline_result_destroy(pipeline_result);
    galay_etcd_pipeline_destroy(pipeline);
    galay_etcd_client_destroy(client);
    galay_etcd_config_builder_destroy(builder);
}

static int run_lease_pipeline_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    EtcdLeasePipelineState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int result = 0;
    size_t i = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    state.listener = &listener;
    state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, lease_pipeline_server_entry, &state, NULL, &server).code ==
                     C_IOResultOk,
                 4);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, lease_pipeline_client_entry, &state, NULL, &client).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_join(&client, 3000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_coro_join(&server, 3000).code == C_IOResultOk, 7);

    for (i = 0; i < 6; ++i) {
        if (state.recv_results[i].code != C_IOResultOk ||
            state.send_results[i].code != C_IOResultOk) {
            result = 8;
        }
    }
    if (state.accept_result.code != C_IOResultOk ||
        state.grant_status != GALAY_OK ||
        state.keepalive_status != GALAY_OK ||
        state.revoke_status != GALAY_OK ||
        state.pipeline_status != GALAY_OK ||
        state.grant_id != 77 ||
        state.keepalive_id != 77 ||
        state.pipeline_count != 3 ||
        state.pipeline_deleted != 1 ||
        strcmp(state.pipeline_value, "value") != 0) {
        result = 9;
    }

    if (server.task != NULL && galay_coro_destroy(&server).code != C_IOResultOk && result == 0) {
        result = 10;
    }
    if (client.task != NULL && galay_coro_destroy(&client).code != C_IOResultOk && result == 0) {
        result = 11;
    }
    if (state.accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&state.accepted) != C_TcpSocketSuccess &&
        result == 0) {
        result = 12;
    }
    if (galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess && result == 0) {
        result = 13;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 14;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 15;
    }
    return result;
}

int main(void)
{
    return run_lease_pipeline_loopback();
}
