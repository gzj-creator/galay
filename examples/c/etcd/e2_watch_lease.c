#include <galay/c/galay-etcd-c/etcd.h>
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

typedef struct EtcdWatchStatsState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult recv_result;
    C_IOResult send_result;
    galay_status_t watch_status;
    galay_status_t cancel_status;
    galay_status_t next_after_cancel_status;
    galay_status_t create_after_close_status;
    galay_etcd_error_code_t cancel_code;
    galay_etcd_error_code_t closed_code;
    galay_etcd_client_stats_t stats_after_watch;
    int64_t watch_id;
    galay_etcd_watch_event_type_t event_type;
    char event_key[32];
    char event_value[32];
    char request[1024];
} EtcdWatchStatsState;

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

static C_IOResult send_watch_json(galay_kernel_tcp_socket_t* socket)
{
    static const char body[] =
        "{\"result\":{\"created\":true,\"watch_id\":\"9\",\"events\":[{\"type\":\"PUT\","
        "\"kv\":{\"key\":\"d2F0Y2gta2V5\",\"value\":\"bmV4dA==\"}}]}}";
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

static void watch_stats_server_entry(void* arg)
{
    EtcdWatchStatsState* state = (EtcdWatchStatsState*)arg;
    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    memset(state->request, 0, sizeof(state->request));
    state->recv_result =
        galay_kernel_tcp_socket_recv(&state->accepted,
                                     state->request,
                                     sizeof(state->request) - 1,
                                     1000);
    if (state->recv_result.code != C_IOResultOk ||
        strstr(state->request, "POST /v3/watch") == NULL ||
        strstr(state->request, "\"key\":\"d2F0Y2gta2V5\"") == NULL) {
        state->recv_result.code = C_IOResultError;
        return;
    }
    state->send_result = send_watch_json(&state->accepted);
}

static void watch_stats_client_entry(void* arg)
{
    EtcdWatchStatsState* state = (EtcdWatchStatsState*)arg;
    char endpoint[64];
    galay_etcd_config_builder_t* builder = NULL;
    galay_etcd_client_t* client = NULL;
    galay_etcd_watch_t* watch = NULL;
    galay_etcd_watch_t* closed_watch = NULL;
    galay_etcd_watch_event_t* event = NULL;
    const char* key = NULL;
    const char* value = NULL;
    size_t key_len = 0;
    size_t value_len = 0;
    galay_etcd_error_code_t code = GALAY_ETCD_ERROR_SUCCESS;
    int len = snprintf(endpoint, sizeof(endpoint), "http://%s:%u", state->peer.address, state->peer.port);

    if (len <= 0 || (size_t)len >= sizeof(endpoint)) {
        state->watch_status = GALAY_INTERNAL_ERROR;
        return;
    }
    if (galay_etcd_config_builder_create(&builder) != GALAY_OK ||
        galay_etcd_config_builder_set_endpoint(builder, endpoint) != GALAY_OK ||
        galay_etcd_config_builder_set_endpoint_policy(builder, GALAY_ETCD_ENDPOINT_POLICY_FIRST_HEALTHY) !=
            GALAY_OK ||
        galay_etcd_client_create(builder, &client) != GALAY_OK ||
        galay_etcd_client_connect(client, &code) != GALAY_OK ||
        galay_etcd_watch_create(client, "watch-key", GALAY_FALSE, &watch, &code) != GALAY_OK) {
        state->watch_status = GALAY_INTERNAL_ERROR;
        goto cleanup;
    }

    state->watch_status = galay_etcd_watch_next(watch, &event, &code);
    if (state->watch_status == GALAY_OK &&
        galay_etcd_watch_event_watch_id(event, &state->watch_id) == GALAY_OK &&
        galay_etcd_watch_event_type(event, &state->event_type) == GALAY_OK &&
        galay_etcd_watch_event_key_value(event, &key, &key_len, &value, &value_len) == GALAY_OK &&
        key_len < sizeof(state->event_key) &&
        value_len < sizeof(state->event_value)) {
        memcpy(state->event_key, key, key_len);
        state->event_key[key_len] = '\0';
        memcpy(state->event_value, value, value_len);
        state->event_value[value_len] = '\0';
    }
    if (galay_etcd_client_stats(client, &state->stats_after_watch) != GALAY_OK) {
        state->watch_status = GALAY_INTERNAL_ERROR;
    }
    galay_etcd_watch_event_destroy(event);
    event = NULL;

    state->cancel_status = galay_etcd_watch_cancel(watch, &state->cancel_code);
    state->next_after_cancel_status = galay_etcd_watch_next(watch, &event, &state->cancel_code);
    if (galay_etcd_client_close(client, &code) != GALAY_OK) {
        state->create_after_close_status = GALAY_INTERNAL_ERROR;
    } else {
        state->create_after_close_status =
            galay_etcd_watch_create(client, "closed", GALAY_FALSE, &closed_watch, &state->closed_code);
    }

cleanup:
    galay_etcd_watch_event_destroy(event);
    galay_etcd_watch_destroy(closed_watch);
    galay_etcd_watch_destroy(watch);
    galay_etcd_client_destroy(client);
    galay_etcd_config_builder_destroy(builder);
}

static int run_watch_stats_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    EtcdWatchStatsState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    state.listener = &listener;
    state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, watch_stats_server_entry, &state, NULL, &server).code ==
                     C_IOResultOk,
                 4);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, watch_stats_client_entry, &state, NULL, &client).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_join(&client, 3000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_coro_join(&server, 3000).code == C_IOResultOk, 7);

    if (state.accept_result.code != C_IOResultOk ||
        state.recv_result.code != C_IOResultOk ||
        state.send_result.code != C_IOResultOk ||
        state.watch_status != GALAY_OK ||
        state.cancel_status != GALAY_OK ||
        state.next_after_cancel_status != GALAY_IO_ERROR ||
        state.create_after_close_status != GALAY_INVALID_ARGUMENT ||
        state.cancel_code != GALAY_ETCD_ERROR_CANCELLED ||
        state.closed_code != GALAY_ETCD_ERROR_NOT_CONNECTED ||
        state.watch_id != 9 ||
        state.event_type != GALAY_ETCD_WATCH_EVENT_PUT ||
        strcmp(state.event_key, "watch-key") != 0 ||
        strcmp(state.event_value, "next") != 0 ||
        state.stats_after_watch.requests != 1 ||
        state.stats_after_watch.request_failures != 0) {
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
    return run_watch_stats_loopback();
}
