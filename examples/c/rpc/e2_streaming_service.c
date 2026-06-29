#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-rpc-c/rpc.h>

#include <stdio.h>
#include <string.h>

typedef struct StreamExample {
    galay_rpc_server_t* server;
    C_Host peer;
    C_IOResult server_result;
    C_IOResult client_result;
    galay_rpc_response_buffer_t response;
} StreamExample;

static galay_rpc_error_code_t stream_echo(const galay_rpc_request_t* request,
                                          galay_rpc_response_t* response,
                                          void* user_data)
{
    StreamExample* example = (StreamExample*)user_data;
    example->client_result.value += 1;
    response->payload = request->payload;
    response->payload_len = request->payload_len;
    response->end_of_stream = request->end_of_stream;
    return GALAY_RPC_ERROR_OK;
}

static void server_entry(void* arg)
{
    StreamExample* example = (StreamExample*)arg;
    example->server_result = galay_rpc_server_serve_one(example->server, 3000);
}

static void client_entry(void* arg)
{
    StreamExample* example = (StreamExample*)arg;
    galay_rpc_client_config_t config = galay_rpc_client_config_default();
    galay_rpc_client_t* client = NULL;
    galay_rpc_stream_t* stream = NULL;
    config.host = example->peer.address;
    config.port = example->peer.port;
    config.connect_timeout_ms = 1000;

    if (galay_rpc_client_create(&config, &client) != GALAY_OK || client == NULL) {
        example->client_result.code = C_IOResultError;
        return;
    }
    C_IOResult connected = galay_rpc_client_connect(client, 1000);
    if (connected.code == C_IOResultOk) {
        C_IOResult opened = galay_rpc_client_stream_open(client,
                                                         "StreamService",
                                                         strlen("StreamService"),
                                                         "Echo",
                                                         strlen("Echo"),
                                                         GALAY_RPC_CALL_BIDI_STREAMING,
                                                         &stream);
        if (opened.code == C_IOResultOk) {
            C_IOResult written =
                galay_rpc_stream_write(stream, "frame", strlen("frame"), GALAY_TRUE, 1000);
            if (written.code == C_IOResultOk) {
                example->client_result = galay_rpc_stream_read(stream, 1000, &example->response);
            } else {
                example->client_result = written;
            }
            C_IOResult closed_stream = galay_rpc_stream_close(stream, 1000);
            if (example->client_result.code == C_IOResultOk && closed_stream.code != C_IOResultOk) {
                example->client_result = closed_stream;
            }
            galay_rpc_stream_destroy(stream);
        } else {
            example->client_result = opened;
        }
        C_IOResult closed_client = galay_rpc_client_close(client, 1000);
        if (example->client_result.code == C_IOResultOk && closed_client.code != C_IOResultOk) {
            example->client_result = closed_client;
        }
    } else {
        example->client_result = connected;
    }
    galay_rpc_client_destroy(client);
}

int main(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;
    galay_kernel_runtime_t runtime = {0};
    galay_rpc_server_t* server = NULL;
    galay_rpc_service_t* service = NULL;
    StreamExample example;
    memset(&example, 0, sizeof(example));
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    C_Host local = {0};
    int exit_code = 0;

    galay_rpc_server_config_t server_config = galay_rpc_server_config_default();
    server_config.host = "127.0.0.1";
    server_config.port = 0;

    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_rpc_server_create(&server_config, &server) != GALAY_OK ||
        galay_rpc_service_create("StreamService", strlen("StreamService"), &service) != GALAY_OK ||
        galay_rpc_service_register_streaming(service,
                                             "Echo",
                                             strlen("Echo"),
                                             GALAY_RPC_CALL_BIDI_STREAMING,
                                             stream_echo,
                                             &example) != GALAY_OK ||
        galay_rpc_server_register_service(server, service) != GALAY_OK ||
        galay_rpc_server_listen(server) != GALAY_OK ||
        galay_rpc_server_local_endpoint(server, &local) != GALAY_OK) {
        exit_code = 1;
    } else {
        example.server = server;
        example.peer = local;
        if (galay_coro_spawn(&runtime, server_entry, &example, NULL, &server_task).code !=
                C_IOResultOk ||
            galay_coro_spawn(&runtime, client_entry, &example, NULL, &client_task).code !=
                C_IOResultOk ||
            galay_coro_join(&server_task, 3000).code != C_IOResultOk ||
            galay_coro_join(&client_task, 3000).code != C_IOResultOk ||
            example.server_result.code != C_IOResultOk ||
            example.client_result.code != C_IOResultOk ||
            example.response.error_code != GALAY_RPC_ERROR_OK) {
            exit_code = 2;
        }
    }
    if (exit_code == 0) {
        printf("rpc stream response: %.*s\n",
               (int)example.response.payload_len,
               (const char*)example.response.payload);
    }
    galay_rpc_response_buffer_destroy(&example.response);
    if (server_task.task != NULL && galay_coro_destroy(&server_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 3;
    }
    if (client_task.task != NULL && galay_coro_destroy(&client_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 4;
    }
    galay_rpc_server_destroy(server);
    galay_rpc_service_destroy(service);
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
        exit_code = 5;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
        exit_code = 6;
    }
    return exit_code;
}
