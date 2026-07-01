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

typedef struct UnaryLoopbackState {
    galay_rpc_server_t* server;
    C_Host peer;
    C_IOResult server_result;
    C_IOResult connect_result;
    C_IOResult missing_service_result;
    C_IOResult missing_method_result;
    C_IOResult echo_result;
    C_IOResult heartbeat_result;
    C_IOResult close_result;
    galay_rpc_response_buffer_t missing_service;
    galay_rpc_response_buffer_t missing_method;
    galay_rpc_response_buffer_t echo;
    int echo_calls;
} UnaryLoopbackState;

static galay_rpc_error_code_t echo_handler(const galay_rpc_request_t* request,
                                           galay_rpc_response_t* response,
                                           void* user_data)
{
    UnaryLoopbackState* state = (UnaryLoopbackState*)user_data;
    state->echo_calls += 1;
    response->payload = request->payload;
    response->payload_len = request->payload_len;
    return GALAY_RPC_ERROR_OK;
}

static void rpc_server_entry(void* arg)
{
    UnaryLoopbackState* state = (UnaryLoopbackState*)arg;
    state->server_result = galay_rpc_server_serve_one(state->server, 3000);
}

static void rpc_client_entry(void* arg)
{
    UnaryLoopbackState* state = (UnaryLoopbackState*)arg;
    galay_rpc_client_config_t config = galay_rpc_client_config_default();
    galay_rpc_client_t* client = NULL;
    config.host = state->peer.address;
    config.port = state->peer.port;
    config.connect_timeout_ms = 1000;

    if (galay_rpc_client_create(&config, &client) != GALAY_OK || client == NULL) {
        state->connect_result.code = C_IOResultError;
        return;
    }

    state->connect_result = galay_rpc_client_connect(client, 1000);
    if (state->connect_result.code == C_IOResultOk) {
        state->missing_service_result =
            galay_rpc_client_call(client,
                                  "MissingService",
                                  strlen("MissingService"),
                                  "Echo",
                                  strlen("Echo"),
                                  "ignored",
                                  strlen("ignored"),
                                  1000,
                                  &state->missing_service);
        state->missing_method_result =
            galay_rpc_client_call(client,
                                  "EchoService",
                                  strlen("EchoService"),
                                  "Missing",
                                  strlen("Missing"),
                                  "ignored",
                                  strlen("ignored"),
                                  1000,
                                  &state->missing_method);
        state->echo_result = galay_rpc_client_call(client,
                                                   "EchoService",
                                                   strlen("EchoService"),
                                                   "Echo",
                                                   strlen("Echo"),
                                                   "hello-rpc",
                                                   strlen("hello-rpc"),
                                                   1000,
                                                   &state->echo);
        state->heartbeat_result = galay_rpc_client_heartbeat(client, 1000);
        state->close_result = galay_rpc_client_close(client, 1000);
    }
    galay_rpc_client_destroy(client);
}

static int run_unary_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_rpc_server_t* server = NULL;
    galay_rpc_service_t* service = NULL;
    C_Host local = {0};
    UnaryLoopbackState state;
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    int result = 0;
    memset(&state, 0, sizeof(state));

    galay_rpc_server_config_t server_config = galay_rpc_server_config_default();
    server_config.host = "127.0.0.1";
    server_config.port = 0;
    server_config.backlog = 16;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(galay_rpc_server_create(&server_config, &server) == GALAY_OK && server != NULL, 3);
    REQUIRE_TRUE(galay_rpc_service_create("EchoService", strlen("EchoService"), &service) ==
                     GALAY_OK && service != NULL,
                 4);
    REQUIRE_TRUE(galay_rpc_service_register_unary(service,
                                                  "Echo",
                                                  strlen("Echo"),
                                                  echo_handler,
                                                  &state) == GALAY_OK,
                 5);
    REQUIRE_TRUE(galay_rpc_server_register_service(server, service) == GALAY_OK, 6);
    REQUIRE_TRUE(galay_rpc_server_listen(server) == GALAY_OK, 7);
    REQUIRE_TRUE(galay_rpc_server_local_endpoint(server, &local) == GALAY_OK && local.port != 0, 8);
    state.server = server;
    state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, rpc_server_entry, &state, NULL, &server_task).code ==
                     C_IOResultOk,
                 9);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, rpc_client_entry, &state, NULL, &client_task).code ==
                     C_IOResultOk,
                 10);
    REQUIRE_TRUE(galay_coro_join(&server_task, 3000).code == C_IOResultOk, 11);
    REQUIRE_TRUE(galay_coro_join(&client_task, 3000).code == C_IOResultOk, 12);

    if (state.server_result.code != C_IOResultOk ||
        state.connect_result.code != C_IOResultOk ||
        state.missing_service_result.code != C_IOResultOk ||
        state.missing_service.error_code != GALAY_RPC_ERROR_SERVICE_NOT_FOUND ||
        state.missing_method_result.code != C_IOResultOk ||
        state.missing_method.error_code != GALAY_RPC_ERROR_METHOD_NOT_FOUND ||
        state.echo_result.code != C_IOResultOk ||
        state.echo.error_code != GALAY_RPC_ERROR_OK ||
        state.echo.payload_len != strlen("hello-rpc") ||
        memcmp(state.echo.payload, "hello-rpc", state.echo.payload_len) != 0 ||
        state.heartbeat_result.code != C_IOResultOk ||
        state.echo_calls != 1 ||
        state.close_result.code != C_IOResultOk) {
        result = 13;
    }

    galay_rpc_response_buffer_destroy(&state.missing_service);
    galay_rpc_response_buffer_destroy(&state.missing_method);
    galay_rpc_response_buffer_destroy(&state.echo);
    if (server_task.task != NULL &&
        galay_coro_destroy(&server_task).code != C_IOResultOk &&
        result == 0) {
        result = 14;
    }
    if (client_task.task != NULL &&
        galay_coro_destroy(&client_task).code != C_IOResultOk &&
        result == 0) {
        result = 15;
    }
    galay_rpc_server_destroy(server);
    galay_rpc_service_destroy(service);
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 16;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 17;
    }
    return result;
}

int main(void)
{
    return run_unary_loopback();
}
