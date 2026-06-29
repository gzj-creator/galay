#include <galay/c/galay-http-c/http.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>
#include <string.h>

typedef struct ServerExample {
    galay_http_server_t* server;
    galay_http_client_t* client;
    C_Host endpoint;
    C_IOResult server_result;
    C_IOResult client_result;
    int handled;
} ServerExample;

static int assign_loopback(C_Host* host)
{
    host->type = C_IPTypeIPV4;
    int written = snprintf(host->address, sizeof(host->address), "%s", "127.0.0.1");
    if (written <= 0 || (size_t)written >= sizeof(host->address)) {
        return 1;
    }
    host->port = 0;
    return 0;
}

static galay_status_t status_route(const galay_http_request_t* request,
                                   galay_http_response_t* response,
                                   void* user_data)
{
    ServerExample* example = (ServerExample*)user_data;
    const char* path = NULL;
    size_t path_len = 0;
    if (example == NULL ||
        galay_http_request_path(request, &path, &path_len) != GALAY_OK ||
        path_len != strlen("/status") ||
        strncmp(path, "/status", path_len) != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    example->handled += 1;
    if (galay_http_response_set_status(response, GALAY_HTTP_STATUS_OK) != GALAY_OK) {
        return GALAY_INTERNAL_ERROR;
    }
    return galay_http_response_set_body(response, "ok", strlen("ok"));
}

static void server_entry(void* arg)
{
    ServerExample* example = (ServerExample*)arg;
    example->server_result = galay_http_server_serve_one(example->server, 2000);
}

static void client_probe_entry(void* arg)
{
    ServerExample* example = (ServerExample*)arg;
    galay_http_request_t* request = NULL;
    galay_http_response_t* response = NULL;

    if (galay_http_request_create(&request) != GALAY_OK ||
        galay_http_request_set_method_path(request, GALAY_HTTP_METHOD_GET, "/status") != GALAY_OK ||
        galay_http_request_set_body(request, NULL, 0) != GALAY_OK) {
        example->client_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        if (request != NULL) {
            galay_http_request_destroy(request);
        }
        return;
    }
    example->client_result = galay_http_client_connect(example->client, &example->endpoint, 2000);
    if (example->client_result.code == C_IOResultOk) {
        example->client_result = galay_http_client_send_request(example->client, request, 2000);
    }
    if (example->client_result.code == C_IOResultOk) {
        example->client_result =
            galay_http_client_recv_response(example->client, &response, 4096, 4096, 2000);
    }
    if (response != NULL) {
        galay_http_response_destroy(response);
    }
    galay_http_request_destroy(request);
}

static int cleanup(galay_kernel_runtime_t* runtime,
                   galay_coro_task_t* server_task,
                   galay_coro_task_t* client_task,
                   ServerExample* example,
                   int exit_code)
{
    if (server_task->task != NULL && galay_coro_destroy(server_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 10;
    }
    if (client_task->task != NULL && galay_coro_destroy(client_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 11;
    }
    if (example->client != NULL) {
        C_IOResult closed = galay_http_client_close(example->client, 2000);
        if (closed.code != C_IOResultOk && closed.code != C_IOResultInvalid && exit_code == 0) {
            exit_code = 12;
        }
        if (galay_http_client_destroy(example->client) != GALAY_OK && exit_code == 0) {
            exit_code = 13;
        }
    }
    if (example->server != NULL) {
        C_IOResult stopped = galay_http_server_stop(example->server, 2000);
        if (stopped.code != C_IOResultOk && stopped.code != C_IOResultInvalid && exit_code == 0) {
            exit_code = 14;
        }
        if (galay_http_server_destroy(example->server) != GALAY_OK && exit_code == 0) {
            exit_code = 15;
        }
    }
    if (runtime->runtime != NULL) {
        if (galay_kernel_runtime_stop(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 16;
        }
        if (galay_kernel_runtime_destroy(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 17;
        }
    }
    return exit_code;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    ServerExample example = {0};

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_http_server_create(&example.server) != GALAY_OK ||
        galay_http_client_create(&example.client) != GALAY_OK ||
        assign_loopback(&example.endpoint) != 0 ||
        galay_http_server_bind(example.server, &example.endpoint) != GALAY_OK ||
        galay_http_server_listen(example.server, 16) != GALAY_OK ||
        galay_http_server_local_endpoint(example.server, &example.endpoint) != GALAY_OK ||
        galay_http_server_add_route(example.server, GALAY_HTTP_METHOD_GET, "/status",
                                    status_route, &example) != GALAY_OK) {
        return cleanup(&runtime, &server_task, &client_task, &example, 1);
    }
    if (galay_coro_spawn(&runtime, server_entry, &example, NULL, &server_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_probe_entry, &example, NULL, &client_task).code != C_IOResultOk ||
        galay_coro_join(&server_task, 3000).code != C_IOResultOk ||
        galay_coro_join(&client_task, 3000).code != C_IOResultOk ||
        example.server_result.code != C_IOResultOk ||
        example.client_result.code != C_IOResultOk ||
        example.handled != 1) {
        return cleanup(&runtime, &server_task, &client_task, &example, 2);
    }
    if (printf("c_http_async_server handled=%d port=%u\n",
               example.handled, example.endpoint.port) < 0) {
        return cleanup(&runtime, &server_task, &client_task, &example, 3);
    }
    return cleanup(&runtime, &server_task, &client_task, &example, 0);
}
