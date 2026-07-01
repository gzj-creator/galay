#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-mongo-c/mongo_c.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct MongoLoopbackCase {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult server_result;
    C_IOResult client_result;
} MongoLoopbackCase;

static void write_i32_le(uint8_t* out, int32_t value)
{
    uint32_t u = (uint32_t)value;
    out[0] = (uint8_t)(u & 0xffu);
    out[1] = (uint8_t)((u >> 8u) & 0xffu);
    out[2] = (uint8_t)((u >> 16u) & 0xffu);
    out[3] = (uint8_t)((u >> 24u) & 0xffu);
}

static int32_t read_i32_le(const uint8_t* in)
{
    return (int32_t)((uint32_t)in[0] |
                     ((uint32_t)in[1] << 8u) |
                     ((uint32_t)in[2] << 16u) |
                     ((uint32_t)in[3] << 24u));
}

static C_IOResult make_result(C_IOResultCode code)
{
    C_IOResult result = {code, 0, 0, 0, NULL};
    return result;
}

static C_IOResult read_exact(galay_kernel_tcp_socket_t* socket,
                             uint8_t* data,
                             size_t data_len)
{
    size_t received = 0;
    while (received < data_len) {
        C_IOResult result = galay_kernel_tcp_socket_recv(socket,
                                                         (char*)data + received,
                                                         data_len - received,
                                                         1000);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_result(C_IOResultEof);
        }
        received += result.bytes;
    }
    C_IOResult result = make_result(C_IOResultOk);
    result.bytes = received;
    return result;
}

static C_IOResult write_exact(galay_kernel_tcp_socket_t* socket,
                              const uint8_t* data,
                              size_t data_len)
{
    size_t sent = 0;
    while (sent < data_len) {
        C_IOResult result = galay_kernel_tcp_socket_send(socket,
                                                         (const char*)data + sent,
                                                         data_len - sent,
                                                         1000);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_result(C_IOResultEof);
        }
        sent += result.bytes;
    }
    C_IOResult result = make_result(C_IOResultOk);
    result.bytes = sent;
    return result;
}

static C_IOResult read_request(galay_kernel_tcp_socket_t* socket,
                               galay_mongo_document_t** command,
                               int32_t* request_id)
{
    uint8_t header[16];
    uint8_t payload[512];
    C_IOResult header_result = read_exact(socket, header, sizeof(header));
    if (header_result.code != C_IOResultOk) {
        return header_result;
    }
    int32_t message_len = read_i32_le(header);
    if (message_len < 21 || message_len > (int32_t)(sizeof(header) + sizeof(payload))) {
        return make_result(C_IOResultError);
    }
    if (read_i32_le(header + 12) != 2013) {
        return make_result(C_IOResultError);
    }
    *request_id = read_i32_le(header + 4);
    C_IOResult payload_result = read_exact(socket, payload, (size_t)message_len - sizeof(header));
    if (payload_result.code != C_IOResultOk) {
        return payload_result;
    }
    if (read_i32_le(payload) != 0 || payload[4] != 0) {
        return make_result(C_IOResultError);
    }
    galay_status_t decoded =
        galay_mongo_document_decode(payload + 5, (size_t)message_len - sizeof(header) - 5, command);
    return decoded == GALAY_OK ? make_result(C_IOResultOk) : make_result(C_IOResultError);
}

static C_IOResult send_ok_reply(galay_kernel_tcp_socket_t* socket, int32_t response_to)
{
    galay_mongo_document_t* reply = NULL;
    const uint8_t* bson = NULL;
    size_t bson_len = 0;
    uint8_t frame[128];
    size_t frame_len = 0;

    if (galay_mongo_document_create(&reply) != GALAY_OK ||
        galay_mongo_document_append_double(reply, "ok", 1.0) != GALAY_OK ||
        galay_mongo_document_encode(reply, &bson, &bson_len) != GALAY_OK ||
        bson_len + 21 > sizeof(frame)) {
        galay_mongo_document_destroy(reply);
        return make_result(C_IOResultError);
    }
    frame_len = 16 + 4 + 1 + bson_len;
    write_i32_le(frame, (int32_t)frame_len);
    write_i32_le(frame + 4, response_to + 1000);
    write_i32_le(frame + 8, response_to);
    write_i32_le(frame + 12, 2013);
    write_i32_le(frame + 16, 0);
    frame[20] = 0;
    memcpy(frame + 21, bson, bson_len);
    C_IOResult sent = write_exact(socket, frame, frame_len);
    galay_mongo_document_destroy(reply);
    return sent;
}

static int create_listener(galay_kernel_tcp_socket_t* listener, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_kernel_tcp_socket_create(listener, C_IPTypeIPV4) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_bind(listener, &bind_host) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_listen(listener, 16) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_local_endpoint(listener, local) == C_TcpSocketSuccess &&
        local->port != 0 ? 0 : 1;
}

static void server_entry(void* arg)
{
    MongoLoopbackCase* test = (MongoLoopbackCase*)arg;
    galay_mongo_document_t* command = NULL;
    int32_t request_id = 0;
    int32_t one = 0;

    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(test->listener, &test->accepted, NULL, 1000);
    if (accepted.code != C_IOResultOk) {
        test->server_result = accepted;
        return;
    }

    test->server_result = read_request(&test->accepted, &command, &request_id);
    if (test->server_result.code != C_IOResultOk ||
        galay_mongo_document_get_int32(command, "hello", &one) != GALAY_OK ||
        one != 1) {
        test->server_result = make_result(C_IOResultError);
        galay_mongo_document_destroy(command);
        return;
    }
    galay_mongo_document_destroy(command);
    command = NULL;
    test->server_result = send_ok_reply(&test->accepted, request_id);
    if (test->server_result.code != C_IOResultOk) {
        return;
    }

    test->server_result = read_request(&test->accepted, &command, &request_id);
    if (test->server_result.code != C_IOResultOk ||
        galay_mongo_document_get_int32(command, "ping", &one) != GALAY_OK ||
        one != 1) {
        test->server_result = make_result(C_IOResultError);
        galay_mongo_document_destroy(command);
        return;
    }
    galay_mongo_document_destroy(command);
    test->server_result = send_ok_reply(&test->accepted, request_id);
}

static void client_entry(void* arg)
{
    MongoLoopbackCase* test = (MongoLoopbackCase*)arg;
    galay_mongo_client_t* client = NULL;
    galay_mongo_document_t* hello_reply = NULL;
    galay_mongo_document_t* ping = NULL;
    galay_mongo_document_t* ping_reply = NULL;
    double ok = 0.0;

    if (galay_mongo_client_create(&client) != GALAY_OK ||
        galay_mongo_client_set_endpoint(client, test->peer.address, test->peer.port, "admin") !=
            GALAY_OK) {
        test->client_result = make_result(C_IOResultError);
        return;
    }
    test->client_result = galay_mongo_client_connect_async(client, 1000);
    if (test->client_result.code != C_IOResultOk) {
        galay_mongo_client_destroy(client);
        return;
    }
    test->client_result = galay_mongo_client_hello_async(client, 1000, &hello_reply);
    if (test->client_result.code != C_IOResultOk ||
        galay_mongo_document_get_double(hello_reply, "ok", &ok) != GALAY_OK ||
        ok != 1.0 ||
        galay_mongo_document_create(&ping) != GALAY_OK ||
        galay_mongo_document_append_int32(ping, "ping", 1) != GALAY_OK) {
        test->client_result = make_result(C_IOResultError);
        goto cleanup;
    }
    test->client_result =
        galay_mongo_client_command_async(client, "admin", ping, 1000, &ping_reply);
    if (test->client_result.code != C_IOResultOk ||
        galay_mongo_document_get_double(ping_reply, "ok", &ok) != GALAY_OK ||
        ok != 1.0) {
        test->client_result = make_result(C_IOResultError);
        goto cleanup;
    }

cleanup:
    galay_mongo_document_destroy(ping_reply);
    galay_mongo_document_destroy(ping);
    galay_mongo_document_destroy(hello_reply);
    if (client != NULL) {
        C_IOResult closed = galay_mongo_client_close_async(client, 1000);
        if (test->client_result.code == C_IOResultOk && closed.code != C_IOResultOk) {
            test->client_result = closed;
        }
        galay_mongo_client_destroy(client);
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
    MongoLoopbackCase test = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, &local) != 0) {
        exit_code = 1;
        goto cleanup;
    }
    test.listener = &listener;
    test.peer = local;

    if (galay_coro_spawn(&runtime, server_entry, &test, NULL, &server).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &test, NULL, &client).code != C_IOResultOk ||
        galay_coro_join(&server, 3000).code != C_IOResultOk ||
        galay_coro_join(&client, 3000).code != C_IOResultOk) {
        exit_code = 2;
        goto cleanup;
    }
    if (test.server_result.code != C_IOResultOk || test.client_result.code != C_IOResultOk) {
        exit_code = 3;
    }

cleanup:
    if (server.task != NULL && galay_coro_destroy(&server).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 4;
    }
    if (client.task != NULL && galay_coro_destroy(&client).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 5;
    }
    if (test.accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&test.accepted) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 6;
    }
    if (listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 7;
    }
    if (runtime.runtime != NULL) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 8;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 9;
        }
    }
    return exit_code;
}
