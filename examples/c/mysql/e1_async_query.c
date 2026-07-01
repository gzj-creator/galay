#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-mysql-c/mysql_c.h>

#include <stdio.h>
#include <string.h>

typedef struct MysqlAsyncQueryExample {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult server_result;
    C_IOResult client_result;
    uint8_t result_sequence;
    uint8_t result_marker;
    char request[64];
} MysqlAsyncQueryExample;

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

static void server_entry(void* arg)
{
    static const unsigned char handshake_packet[] = {
        0x05, 0x00, 0x00, 0x00, 'H', 'E', 'L', 'L', 'O'
    };
    static const unsigned char expected_query[] = {
        0x09, 0x00, 0x00, 0x00, 0x03, 'S', 'E', 'L', 'E', 'C', 'T', ' ', '1'
    };
    static const unsigned char ok_packet[] = {0x01, 0x00, 0x00, 0x01, 0x00};
    MysqlAsyncQueryExample* example = (MysqlAsyncQueryExample*)arg;

    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(example->listener, &example->accepted, NULL, 1000);
    if (accepted.code != C_IOResultOk) {
        example->server_result = accepted;
        return;
    }
    C_IOResult sent = galay_kernel_tcp_socket_send(&example->accepted,
                                                   (const char*)handshake_packet,
                                                   sizeof(handshake_packet),
                                                   1000);
    if (sent.code != C_IOResultOk) {
        example->server_result = sent;
        return;
    }
    if (recv_exact(&example->accepted, example->request, sizeof(expected_query)) != 0 ||
        memcmp(example->request, expected_query, sizeof(expected_query)) != 0) {
        example->server_result = example_error();
        return;
    }
    sent = galay_kernel_tcp_socket_send(&example->accepted,
                                        (const char*)ok_packet,
                                        sizeof(ok_packet),
                                        1000);
    if (sent.code != C_IOResultOk) {
        example->server_result = sent;
        return;
    }
    example->server_result = galay_kernel_tcp_socket_close(&example->accepted, 1000);
}

static void client_entry(void* arg)
{
    MysqlAsyncQueryExample* example = (MysqlAsyncQueryExample*)arg;
    galay_mysql_config_t* config = NULL;
    galay_mysql_client_t* client = NULL;
    galay_mysql_buffer_t* result_packet = NULL;
    const unsigned char* packet = NULL;
    size_t packet_len = 0;
    galay_mysql_packet_view_t view = {0};

    if (galay_mysql_config_create(&config) != GALAY_OK ||
        galay_mysql_config_set_host(config, example->peer.address) != GALAY_OK ||
        galay_mysql_config_set_port(config, example->peer.port) != GALAY_OK ||
        galay_mysql_client_create(&client) != GALAY_OK ||
        client == NULL) {
        example->client_result = example_error();
        goto cleanup;
    }
    example->client_result = galay_mysql_client_connect_async(client, config, 1000);
    if (example->client_result.code == C_IOResultOk) {
        example->client_result =
            galay_mysql_client_query_async(client, "SELECT 1", 1000, &result_packet);
    }
    if (example->client_result.code == C_IOResultOk &&
        (galay_mysql_buffer_data(result_packet, &packet, &packet_len) != GALAY_OK ||
         galay_mysql_extract_packet(packet, packet_len, &view) != GALAY_OK ||
         view.payload_len != 1)) {
        example->client_result = example_error();
    }
    if (example->client_result.code == C_IOResultOk) {
        example->result_sequence = view.sequence_id;
        example->result_marker = view.payload[0];
    }
    if (client != NULL) {
        C_IOResult closed = galay_mysql_client_close_async(client, 1000);
        if (example->client_result.code == C_IOResultOk && closed.code != C_IOResultOk) {
            example->client_result = closed;
        }
    }

cleanup:
    if (result_packet != NULL) {
        galay_mysql_buffer_destroy(result_packet);
    }
    if (client != NULL) {
        galay_mysql_client_destroy(client);
    }
    if (config != NULL) {
        galay_mysql_config_destroy(config);
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
    MysqlAsyncQueryExample example = {0};
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
        example.result_sequence != 1 ||
        example.result_marker != 0x00) {
        exit_code = 3;
        goto cleanup;
    }
    if (printf("c_mysql_async_query sequence=%u marker=%u port=%u\n",
               example.result_sequence,
               example.result_marker,
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
