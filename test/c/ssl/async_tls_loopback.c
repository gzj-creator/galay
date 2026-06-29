#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-ssl-c/ssl.h>

#include <stdio.h>
#include <string.h>

#ifndef GALAY_SSL_TEST_CERT_DIR
#define GALAY_SSL_TEST_CERT_DIR ""
#endif

typedef struct TlsLoopbackState {
    galay_ssl_context_t* server_context;
    galay_ssl_context_t* client_context;
    galay_ssl_socket_t* listener;
    galay_ssl_socket_t* accepted;
    galay_ssl_socket_t* client;
    C_Host peer;
    C_IOResult accept_result;
    C_IOResult connect_result;
    C_IOResult server_handshake_result;
    C_IOResult client_handshake_result;
    C_IOResult server_recv_result;
    C_IOResult server_send_result;
    C_IOResult server_shutdown_result;
    C_IOResult server_close_result;
    C_IOResult client_send_result;
    C_IOResult client_recv_result;
    C_IOResult client_shutdown_result;
    C_IOResult client_close_result;
    char server_buffer[64];
    char client_buffer[64];
} TlsLoopbackState;

static int make_path(char* out, size_t out_size, const char* name)
{
    const int written = snprintf(out, out_size, "%s/%s", GALAY_SSL_TEST_CERT_DIR, name);
    return written > 0 && (size_t)written < out_size ? 0 : 1;
}

static int expect_io(C_IOResult result, C_IOResultCode expected)
{
    return result.code == expected ? 0 : 1;
}

static int load_contexts(TlsLoopbackState* state)
{
    char cert_path[512];
    char key_path[512];
    char ca_path[512];
    if (make_path(cert_path, sizeof(cert_path), "server.crt") ||
        make_path(key_path, sizeof(key_path), "server.key") ||
        make_path(ca_path, sizeof(ca_path), "ca.crt")) {
        return 1;
    }

    if (galay_ssl_context_create(GALAY_SSL_METHOD_TLS_SERVER, &state->server_context) != GALAY_OK ||
        galay_ssl_context_load_certificate(state->server_context, cert_path) != GALAY_OK ||
        galay_ssl_context_load_private_key(state->server_context, key_path) != GALAY_OK ||
        galay_ssl_context_create(GALAY_SSL_METHOD_TLS_CLIENT, &state->client_context) != GALAY_OK ||
        galay_ssl_context_load_ca(state->client_context, ca_path) != GALAY_OK ||
        galay_ssl_context_set_verify_mode(state->client_context, GALAY_SSL_VERIFY_PEER) != GALAY_OK) {
        return 2;
    }
    return 0;
}

static int create_listener(TlsLoopbackState* state)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    if (galay_ssl_socket_create(state->server_context, C_IPTypeIPV4, &state->listener) != GALAY_OK ||
        galay_ssl_socket_bind(state->listener, &bind_host) != GALAY_OK ||
        galay_ssl_socket_listen(state->listener, 16) != GALAY_OK ||
        galay_ssl_socket_local_endpoint(state->listener, &state->peer) != GALAY_OK ||
        state->peer.port == 0) {
        return 1;
    }
    return 0;
}

static void server_entry(void* arg)
{
    static const char response[] = "tls-pong-response";
    TlsLoopbackState* state = (TlsLoopbackState*)arg;
    state->accept_result = galay_ssl_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    state->server_handshake_result = galay_ssl_socket_handshake(state->accepted, 1000);
    if (state->server_handshake_result.code != C_IOResultOk) {
        return;
    }
    state->server_recv_result = galay_ssl_socket_recv(
        state->accepted, state->server_buffer, strlen("tls-ping-request"), 1000);
    if (state->server_recv_result.code != C_IOResultOk) {
        return;
    }
    state->server_send_result = galay_ssl_socket_send(
        state->accepted, response, sizeof(response) - 1, 1000);
    if (state->server_send_result.code != C_IOResultOk) {
        return;
    }
    state->server_shutdown_result = galay_ssl_socket_shutdown(state->accepted, 1000);
    state->server_close_result = galay_ssl_socket_close(state->accepted, 1000);
}

static void client_entry(void* arg)
{
    static const char request[] = "tls-ping-request";
    TlsLoopbackState* state = (TlsLoopbackState*)arg;
    if (galay_ssl_socket_create(state->client_context, C_IPTypeIPV4, &state->client) != GALAY_OK ||
        galay_ssl_socket_set_hostname(state->client, "localhost") != GALAY_OK) {
        state->connect_result.code = C_IOResultError;
        return;
    }
    state->connect_result = galay_ssl_socket_connect(state->client, &state->peer, 1000);
    if (state->connect_result.code != C_IOResultOk) {
        return;
    }
    state->client_handshake_result = galay_ssl_socket_handshake(state->client, 1000);
    if (state->client_handshake_result.code != C_IOResultOk) {
        return;
    }
    state->client_send_result = galay_ssl_socket_send(state->client, request, sizeof(request) - 1, 1000);
    if (state->client_send_result.code != C_IOResultOk) {
        return;
    }
    state->client_recv_result = galay_ssl_socket_recv(
        state->client, state->client_buffer, strlen("tls-pong-response"), 1000);
    if (state->client_recv_result.code != C_IOResultOk) {
        return;
    }
    state->client_shutdown_result = galay_ssl_socket_shutdown(state->client, 1000);
    state->client_close_result = galay_ssl_socket_close(state->client, 1000);
}

static int run_loopback(galay_kernel_runtime_t* runtime)
{
    TlsLoopbackState state = {0};
    if (load_contexts(&state) != 0 || create_listener(&state) != 0) {
        return 10;
    }

    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    if (galay_coro_spawn(runtime, server_entry, &state, NULL, &server).code != C_IOResultOk ||
        galay_coro_spawn(runtime, client_entry, &state, NULL, &client).code != C_IOResultOk ||
        galay_coro_join(&server, 3000).code != C_IOResultOk ||
        galay_coro_join(&client, 3000).code != C_IOResultOk) {
        return 11;
    }

    const int failed =
        expect_io(state.accept_result, C_IOResultOk) ||
        expect_io(state.connect_result, C_IOResultOk) ||
        expect_io(state.server_handshake_result, C_IOResultOk) ||
        expect_io(state.client_handshake_result, C_IOResultOk) ||
        expect_io(state.server_recv_result, C_IOResultOk) ||
        expect_io(state.server_send_result, C_IOResultOk) ||
        expect_io(state.client_send_result, C_IOResultOk) ||
        expect_io(state.client_recv_result, C_IOResultOk) ||
        expect_io(state.server_shutdown_result, C_IOResultOk) ||
        expect_io(state.client_shutdown_result, C_IOResultOk) ||
        expect_io(state.server_close_result, C_IOResultOk) ||
        expect_io(state.client_close_result, C_IOResultOk) ||
        memcmp(state.server_buffer, "tls-ping-request", strlen("tls-ping-request")) != 0 ||
        memcmp(state.client_buffer, "tls-pong-response", strlen("tls-pong-response")) != 0;

    if (galay_coro_destroy(&server).code != C_IOResultOk ||
        galay_coro_destroy(&client).code != C_IOResultOk) {
        return 12;
    }
    galay_ssl_socket_destroy(state.accepted);
    galay_ssl_socket_destroy(state.client);
    galay_ssl_socket_destroy(state.listener);
    galay_ssl_context_destroy(state.server_context);
    galay_ssl_context_destroy(state.client_context);
    return failed ? 13 : 0;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
            return 3;
        }
        return 2;
    }
    const int result = run_loopback(&runtime);
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
        return result == 0 ? 4 : result;
    }
    return result;
}
