#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-rpc-c/rpc.h>

#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

typedef struct StreamingLoopbackState {
    galay_rpc_server_t* server;
    C_Host peer;
    C_IOResult server_result;
    C_IOResult connect_result;
    C_IOResult open_result;
    C_IOResult write_first_result;
    C_IOResult read_first_result;
    C_IOResult write_second_result;
    C_IOResult read_second_result;
    C_IOResult close_stream_result;
    C_IOResult close_client_result;
    galay_rpc_response_buffer_t first;
    galay_rpc_response_buffer_t second;
    int frames_seen;
} StreamingLoopbackState;

static galay_rpc_error_code_t bidi_echo_handler(const galay_rpc_request_t* request,
                                                galay_rpc_response_t* response,
                                                void* user_data)
{
    StreamingLoopbackState* state = (StreamingLoopbackState*)user_data;
    state->frames_seen += 1;
    response->payload = request->payload;
    response->payload_len = request->payload_len;
    response->end_of_stream = request->end_of_stream;
    return GALAY_RPC_ERROR_OK;
}

static void streaming_server_entry(void* arg)
{
    StreamingLoopbackState* state = (StreamingLoopbackState*)arg;
    state->server_result = galay_rpc_server_serve_one(state->server, 3000);
}

static void streaming_client_entry(void* arg)
{
    StreamingLoopbackState* state = (StreamingLoopbackState*)arg;
    galay_rpc_client_config_t config = galay_rpc_client_config_default();
    galay_rpc_client_t* client = NULL;
    galay_rpc_stream_t* stream = NULL;
    config.host = state->peer.address;
    config.port = state->peer.port;
    config.connect_timeout_ms = 1000;

    if (galay_rpc_client_create(&config, &client) != GALAY_OK || client == NULL) {
        state->connect_result.code = C_IOResultError;
        return;
    }

    state->connect_result = galay_rpc_client_connect(client, 1000);
    if (state->connect_result.code == C_IOResultOk) {
        state->open_result = galay_rpc_client_stream_open(client,
                                                          "StreamService",
                                                          strlen("StreamService"),
                                                          "BidiEcho",
                                                          strlen("BidiEcho"),
                                                          GALAY_RPC_CALL_BIDI_STREAMING,
                                                          &stream);
        if (state->open_result.code == C_IOResultOk) {
            state->write_first_result =
                galay_rpc_stream_write(stream, "one", strlen("one"), GALAY_FALSE, 1000);
            state->read_first_result = galay_rpc_stream_read(stream, 1000, &state->first);
            state->write_second_result =
                galay_rpc_stream_write(stream, "two", strlen("two"), GALAY_TRUE, 1000);
            state->read_second_result = galay_rpc_stream_read(stream, 1000, &state->second);
            state->close_stream_result = galay_rpc_stream_close(stream, 1000);
            galay_rpc_stream_destroy(stream);
        }
        state->close_client_result = galay_rpc_client_close(client, 1000);
    }
    galay_rpc_client_destroy(client);
}

static int run_streaming_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_rpc_server_t* server = NULL;
    galay_rpc_service_t* service = NULL;
    C_Host local = {0};
    StreamingLoopbackState state;
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
    REQUIRE_TRUE(galay_rpc_service_create("StreamService", strlen("StreamService"), &service) ==
                     GALAY_OK && service != NULL,
                 4);
    REQUIRE_TRUE(galay_rpc_service_register_streaming(service,
                                                      "BidiEcho",
                                                      strlen("BidiEcho"),
                                                      GALAY_RPC_CALL_BIDI_STREAMING,
                                                      bidi_echo_handler,
                                                      &state) == GALAY_OK,
                 5);
    REQUIRE_TRUE(galay_rpc_server_register_service(server, service) == GALAY_OK, 6);
    REQUIRE_TRUE(galay_rpc_server_listen(server) == GALAY_OK, 7);
    REQUIRE_TRUE(galay_rpc_server_local_endpoint(server, &local) == GALAY_OK && local.port != 0, 8);
    state.server = server;
    state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, streaming_server_entry, &state, NULL, &server_task).code ==
                     C_IOResultOk,
                 9);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, streaming_client_entry, &state, NULL, &client_task).code ==
                     C_IOResultOk,
                 10);
    REQUIRE_TRUE(galay_coro_join(&server_task, 3000).code == C_IOResultOk, 11);
    REQUIRE_TRUE(galay_coro_join(&client_task, 3000).code == C_IOResultOk, 12);

    if (state.server_result.code != C_IOResultOk ||
        state.connect_result.code != C_IOResultOk ||
        state.open_result.code != C_IOResultOk ||
        state.write_first_result.code != C_IOResultOk ||
        state.read_first_result.code != C_IOResultOk ||
        state.first.error_code != GALAY_RPC_ERROR_OK ||
        state.first.end_of_stream != GALAY_FALSE ||
        state.first.payload_len != strlen("one") ||
        memcmp(state.first.payload, "one", state.first.payload_len) != 0 ||
        state.write_second_result.code != C_IOResultOk ||
        state.read_second_result.code != C_IOResultOk ||
        state.second.error_code != GALAY_RPC_ERROR_OK ||
        state.second.end_of_stream != GALAY_TRUE ||
        state.second.payload_len != strlen("two") ||
        memcmp(state.second.payload, "two", state.second.payload_len) != 0 ||
        state.close_stream_result.code != C_IOResultOk ||
        state.close_client_result.code != C_IOResultOk ||
        state.frames_seen != 2) {
        result = 13;
    }

    galay_rpc_response_buffer_destroy(&state.first);
    galay_rpc_response_buffer_destroy(&state.second);
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
    return run_streaming_loopback();
}
