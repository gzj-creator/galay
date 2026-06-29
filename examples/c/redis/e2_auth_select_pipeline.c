#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-redis-c/redis.h>

#include <stdio.h>
#include <string.h>

typedef struct RedisAuthPipelineExample {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult server_result;
    C_IOResult client_result;
    char request[128];
    char first_reply[16];
    char second_reply[16];
} RedisAuthPipelineExample;

static C_IOResult example_error(void)
{
    return (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
}

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

static int recv_exact(galay_kernel_tcp_socket_t* socket, char* buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        C_IOResult result =
            galay_kernel_tcp_socket_recv(socket, buffer + received, length - received, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        received += result.bytes;
    }
    return 0;
}

static int expect_request(galay_kernel_tcp_socket_t* socket,
                          char* buffer,
                          const char* expected,
                          size_t expected_len)
{
    return recv_exact(socket, buffer, expected_len) == 0 &&
        memcmp(buffer, expected, expected_len) == 0
        ? 0
        : 1;
}

static int copy_simple_string(galay_redis_reply_t* reply, char* out, size_t out_len)
{
    const char* value = NULL;
    size_t value_len = 0;
    if (reply == NULL ||
        galay_redis_reply_type(reply) != GALAY_REDIS_RESP_SIMPLE_STRING ||
        galay_redis_reply_string(reply, &value, &value_len) != GALAY_OK ||
        value_len >= out_len) {
        return 1;
    }
    memcpy(out, value, value_len);
    out[value_len] = '\0';
    return 0;
}

static void server_entry(void* arg)
{
    static const char auth_request[] =
        "*3\r\n$4\r\nAUTH\r\n$7\r\ndefault\r\n$6\r\nsecret\r\n";
    static const char select_request[] = "*2\r\n$6\r\nSELECT\r\n$1\r\n2\r\n";
    static const char ping_request[] = "*1\r\n$4\r\nPING\r\n";
    static const char echo_request[] = "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";
    static const char ok_response[] = "+OK\r\n";
    static const char pipeline_response[] = "+PONG\r\n+hello\r\n";
    RedisAuthPipelineExample* example = (RedisAuthPipelineExample*)arg;

    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(example->listener, &example->accepted, NULL, 1000);
    if (accepted.code != C_IOResultOk) {
        example->server_result = accepted;
        return;
    }

    if (expect_request(
            &example->accepted, example->request, auth_request, sizeof(auth_request) - 1) != 0) {
        example->server_result = example_error();
        return;
    }
    C_IOResult sent =
        galay_kernel_tcp_socket_send(&example->accepted, ok_response, sizeof(ok_response) - 1, 1000);
    if (sent.code != C_IOResultOk) {
        example->server_result = sent;
        return;
    }
    if (expect_request(&example->accepted,
                       example->request,
                       select_request,
                       sizeof(select_request) - 1) != 0) {
        example->server_result = example_error();
        return;
    }
    sent =
        galay_kernel_tcp_socket_send(&example->accepted, ok_response, sizeof(ok_response) - 1, 1000);
    if (sent.code != C_IOResultOk) {
        example->server_result = sent;
        return;
    }
    if (expect_request(
            &example->accepted, example->request, ping_request, sizeof(ping_request) - 1) != 0 ||
        expect_request(
            &example->accepted, example->request, echo_request, sizeof(echo_request) - 1) != 0) {
        example->server_result = example_error();
        return;
    }
    sent = galay_kernel_tcp_socket_send(
        &example->accepted, pipeline_response, sizeof(pipeline_response) - 1, 1000);
    if (sent.code != C_IOResultOk) {
        example->server_result = sent;
        return;
    }
    C_IOResult closed = galay_kernel_tcp_socket_close(&example->accepted, 1000);
    example->server_result = closed;
}

static void client_entry(void* arg)
{
    RedisAuthPipelineExample* example = (RedisAuthPipelineExample*)arg;
    galay_redis_client_config_t config = {
        .host = example->peer.address,
        .port = example->peer.port,
        .username = NULL,
        .password = NULL,
        .db_index = 0,
        .resp_version = 2,
        .connect_timeout_ms = 1000,
    };
    galay_redis_client_t* client = NULL;
    galay_redis_pipeline_t* pipeline = NULL;
    galay_redis_reply_t** replies = NULL;
    size_t reply_count = 0;
    const char* echo_args[] = {"hello"};

    if (galay_redis_client_create(&config, &client) != GALAY_OK || client == NULL) {
        example->client_result = example_error();
        return;
    }
    example->client_result = galay_redis_client_connect(client, 1000);
    if (example->client_result.code == C_IOResultOk) {
        example->client_result =
            galay_redis_client_auth(client, "default", "secret", 1000);
    }
    if (example->client_result.code == C_IOResultOk) {
        example->client_result = galay_redis_client_select(client, 2, 1000);
    }
    if (example->client_result.code == C_IOResultOk &&
        (galay_redis_pipeline_create(&pipeline) != GALAY_OK ||
         galay_redis_pipeline_add_command(pipeline, "PING", NULL, NULL, 0) != GALAY_OK ||
         galay_redis_pipeline_add_command(pipeline, "ECHO", echo_args, NULL, 1) != GALAY_OK)) {
        example->client_result = example_error();
    }
    if (example->client_result.code == C_IOResultOk) {
        example->client_result = galay_redis_client_pipeline_async(client,
                                                                   pipeline,
                                                                   1000,
                                                                   &replies,
                                                                   &reply_count);
    }
    if (example->client_result.code == C_IOResultOk &&
        (reply_count != 2 ||
         copy_simple_string(replies[0], example->first_reply, sizeof(example->first_reply)) != 0 ||
         copy_simple_string(replies[1], example->second_reply, sizeof(example->second_reply)) !=
             0)) {
        example->client_result = example_error();
    }
    if (replies != NULL) {
        galay_redis_pipeline_replies_destroy(replies, reply_count);
    }
    if (pipeline != NULL) {
        galay_redis_pipeline_destroy(pipeline);
    }
    if (client != NULL) {
        C_IOResult closed = galay_redis_client_close(client, 1000);
        if (example->client_result.code == C_IOResultOk && closed.code != C_IOResultOk) {
            example->client_result = closed;
        }
        galay_redis_client_destroy(client);
    }
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    RedisAuthPipelineExample example = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, &local) != 0) {
        exit_code = 1;
        goto cleanup;
    }
    example.listener = &listener;
    example.peer = local;

    if (galay_coro_spawn(&runtime, server_entry, &example, NULL, &server).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &example, NULL, &client).code != C_IOResultOk ||
        galay_coro_join(&server, 2000).code != C_IOResultOk ||
        galay_coro_join(&client, 2000).code != C_IOResultOk) {
        exit_code = 2;
        goto cleanup;
    }
    if (example.server_result.code != C_IOResultOk ||
        example.client_result.code != C_IOResultOk ||
        strcmp(example.first_reply, "PONG") != 0 ||
        strcmp(example.second_reply, "hello") != 0) {
        exit_code = 3;
        goto cleanup;
    }
    if (printf("c_redis_auth_select_pipeline replies=%s,%s port=%u\n",
               example.first_reply,
               example.second_reply,
               local.port) < 0) {
        exit_code = 4;
    }

cleanup:
    if (server.task != NULL && galay_coro_destroy(&server).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 5;
    }
    if (client.task != NULL && galay_coro_destroy(&client).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 6;
    }
    if (example.accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&example.accepted) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 7;
    }
    if (listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 8;
    }
    if (runtime.runtime != NULL) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 9;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 10;
        }
    }
    return exit_code;
}
