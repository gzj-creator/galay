#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-rpc-c/rpc_c.h>

#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

typedef struct StallServerState {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult recv_result;
    C_IOResult hold_result;
    C_IOResult close_result;
    char request[256];
    char hold[1];
} StallServerState;

typedef struct DeadlineState {
    C_Host peer;
    C_IOResult connect_result;
    C_IOResult deadline_result;
    C_IOResult cancelled_result;
    C_IOResult close_result;
    C_IOResult after_close_result;
    galay_rpc_response_buffer_t deadline_response;
    galay_rpc_response_buffer_t cancelled_response;
    galay_rpc_response_buffer_t after_close_response;
} DeadlineState;

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

static void stall_server_entry(void* arg)
{
    StallServerState* state = (StallServerState*)arg;
    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    state->recv_result =
        galay_kernel_tcp_socket_recv(&state->accepted, state->request, sizeof(state->request), 1000);
    if (state->recv_result.code != C_IOResultOk) {
        return;
    }
    state->hold_result =
        galay_kernel_tcp_socket_recv(&state->accepted, state->hold, sizeof(state->hold), 1000);
    state->close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void deadline_client_entry(void* arg)
{
    DeadlineState* state = (DeadlineState*)arg;
    galay_rpc_client_config_t config = galay_rpc_client_config_default();
    galay_rpc_client_t* client = NULL;
    galay_rpc_cancellation_source_t* cancellation = NULL;
    galay_rpc_call_options_t options = galay_rpc_call_options_default();
    config.host = state->peer.address;
    config.port = state->peer.port;
    config.connect_timeout_ms = 1000;

    if (galay_rpc_client_create(&config, &client) != GALAY_OK || client == NULL) {
        state->connect_result.code = C_IOResultError;
        return;
    }
    if (galay_rpc_cancellation_source_create(&cancellation) != GALAY_OK || cancellation == NULL) {
        state->connect_result.code = C_IOResultError;
        galay_rpc_client_destroy(client);
        return;
    }

    state->connect_result = galay_rpc_client_connect(client, 1000);
    if (state->connect_result.code == C_IOResultOk) {
        options.timeout_ms = 10;
        state->deadline_result =
            galay_rpc_client_call_with_options(client,
                                               "SlowService",
                                               strlen("SlowService"),
                                               "Slow",
                                               strlen("Slow"),
                                               "wait",
                                               strlen("wait"),
                                               &options,
                                               &state->deadline_response);

        galay_rpc_cancellation_source_cancel(cancellation);
        options.timeout_ms = 1000;
        options.cancellation = cancellation;
        state->cancelled_result =
            galay_rpc_client_call_with_options(client,
                                               "SlowService",
                                               strlen("SlowService"),
                                               "Slow",
                                               strlen("Slow"),
                                               "cancel",
                                               strlen("cancel"),
                                               &options,
                                               &state->cancelled_response);

        state->close_result = galay_rpc_client_close(client, 1000);
        state->after_close_result =
            galay_rpc_client_call(client,
                                  "SlowService",
                                  strlen("SlowService"),
                                  "Slow",
                                  strlen("Slow"),
                                  NULL,
                                  0,
                                  1000,
                                  &state->after_close_response);
    }

    galay_rpc_cancellation_source_destroy(cancellation);
    galay_rpc_client_destroy(client);
}

static int run_deadline_cancellation_close(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    StallServerState server_state;
    DeadlineState client_state;
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    int result = 0;
    memset(&server_state, 0, sizeof(server_state));
    memset(&client_state, 0, sizeof(client_state));

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    server_state.listener = &listener;
    client_state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, stall_server_entry, &server_state, NULL, &server_task).code ==
                     C_IOResultOk,
                 4);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, deadline_client_entry, &client_state, NULL, &client_task).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_join(&client_task, 3000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_coro_join(&server_task, 3000).code == C_IOResultOk, 7);

    if (server_state.accept_result.code != C_IOResultOk ||
        server_state.recv_result.code != C_IOResultOk ||
        client_state.connect_result.code != C_IOResultOk ||
        client_state.deadline_result.code != C_IOResultTimeout ||
        client_state.deadline_response.error_code != GALAY_RPC_ERROR_DEADLINE_EXCEEDED ||
        client_state.cancelled_result.code != C_IOResultCancelled ||
        client_state.cancelled_response.error_code != GALAY_RPC_ERROR_CANCELLED ||
        client_state.close_result.code != C_IOResultOk ||
        client_state.after_close_result.code != C_IOResultInvalid ||
        client_state.after_close_response.error_code != GALAY_RPC_ERROR_CONNECTION_CLOSED) {
        result = 8;
    }

    galay_rpc_response_buffer_destroy(&client_state.deadline_response);
    galay_rpc_response_buffer_destroy(&client_state.cancelled_response);
    galay_rpc_response_buffer_destroy(&client_state.after_close_response);
    if (server_task.task != NULL &&
        galay_coro_destroy(&server_task).code != C_IOResultOk &&
        result == 0) {
        result = 9;
    }
    if (client_task.task != NULL &&
        galay_coro_destroy(&client_task).code != C_IOResultOk &&
        result == 0) {
        result = 10;
    }
    if (server_state.accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&server_state.accepted) != C_TcpSocketSuccess &&
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

static int run_pool_lifecycle(void)
{
    galay_rpc_pool_t* pool = NULL;
    galay_rpc_pool_lease_t* first = NULL;
    galay_rpc_pool_lease_t* second = NULL;
    galay_rpc_pool_config_t config = galay_rpc_pool_config_default();
    size_t available = 99;
    size_t in_use = 99;
    uint64_t first_id = 0;
    uint64_t second_id = 0;

    config.min_connections_per_endpoint = 1;
    config.max_connections_per_endpoint = 1;

    REQUIRE_TRUE(galay_rpc_pool_create(&config, &pool) == GALAY_OK && pool != NULL, 21);
    REQUIRE_TRUE(galay_rpc_pool_ensure_endpoint(pool, "127.0.0.1", 9000) == GALAY_OK, 22);
    REQUIRE_TRUE(galay_rpc_pool_available_count(pool, "127.0.0.1", 9000, &available) ==
                     GALAY_OK && available == 1,
                 23);
    REQUIRE_TRUE(galay_rpc_pool_acquire(pool, "127.0.0.1", 9000, &first) == GALAY_OK &&
                     first != NULL,
                 24);
    REQUIRE_TRUE(galay_rpc_pool_lease_id(first, &first_id) == GALAY_OK && first_id != 0, 25);
    REQUIRE_TRUE(galay_rpc_pool_in_use_count(pool, "127.0.0.1", 9000, &in_use) == GALAY_OK &&
                     in_use == 1,
                 26);
    REQUIRE_TRUE(galay_rpc_pool_release(pool, first, GALAY_TRUE) == GALAY_OK, 27);
    first = NULL;
    REQUIRE_TRUE(galay_rpc_pool_acquire(pool, "127.0.0.1", 9000, &second) == GALAY_OK &&
                     second != NULL,
                 28);
    REQUIRE_TRUE(galay_rpc_pool_lease_id(second, &second_id) == GALAY_OK &&
                     second_id != 0 &&
                     second_id != first_id,
                 29);
    REQUIRE_TRUE(galay_rpc_pool_release(pool, second, GALAY_FALSE) == GALAY_OK, 30);
    second = NULL;
    REQUIRE_TRUE(galay_rpc_pool_shutdown(pool) == GALAY_OK, 31);
    REQUIRE_TRUE(galay_rpc_pool_acquire(pool, "127.0.0.1", 9000, &first) == GALAY_IO_ERROR &&
                     first == NULL,
                 32);
    galay_rpc_pool_destroy(pool);
    return 0;
}

int main(void)
{
    int result = run_deadline_cancellation_close();
    if (result != 0) {
        return result;
    }
    return run_pool_lifecycle();
}
