#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-ssl-c/ssl_c.h>

#include <stdio.h>
#include <string.h>

#ifndef GALAY_SSL_EXAMPLE_CERT_DIR
#define GALAY_SSL_EXAMPLE_CERT_DIR ""
#endif

typedef struct EchoExample {
    galay_ssl_context_t* server_context;
    galay_ssl_context_t* client_context;
    galay_ssl_socket_t* listener;
    galay_ssl_socket_t* accepted;
    galay_ssl_socket_t* client;
    C_Host peer;
    int failed;
    char server_buffer[32];
    char client_buffer[32];
} EchoExample;

static int make_path(char* out, size_t out_size, const char* name)
{
    const int written = snprintf(out, out_size, "%s/%s", GALAY_SSL_EXAMPLE_CERT_DIR, name);
    return written > 0 && (size_t)written < out_size ? 0 : 1;
}

static void server_entry(void* arg)
{
    EchoExample* state = (EchoExample*)arg;
    state->accepted = NULL;
    C_IOResult accepted = galay_ssl_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (accepted.code != C_IOResultOk ||
        galay_ssl_socket_handshake(state->accepted, 1000).code != C_IOResultOk ||
        galay_ssl_socket_recv(state->accepted, state->server_buffer, strlen("ping"), 1000).code != C_IOResultOk ||
        memcmp(state->server_buffer, "ping", strlen("ping")) != 0 ||
        galay_ssl_socket_send(state->accepted, "pong", strlen("pong"), 1000).code != C_IOResultOk ||
        galay_ssl_socket_shutdown(state->accepted, 1000).code != C_IOResultOk ||
        galay_ssl_socket_close(state->accepted, 1000).code != C_IOResultOk) {
        state->failed = 1;
    }
}

static void client_entry(void* arg)
{
    EchoExample* state = (EchoExample*)arg;
    if (galay_ssl_socket_create(state->client_context, C_IPTypeIPV4, &state->client) != GALAY_OK ||
        galay_ssl_socket_set_hostname(state->client, "localhost") != GALAY_OK ||
        galay_ssl_socket_connect(state->client, &state->peer, 1000).code != C_IOResultOk ||
        galay_ssl_socket_handshake(state->client, 1000).code != C_IOResultOk ||
        galay_ssl_socket_send(state->client, "ping", strlen("ping"), 1000).code != C_IOResultOk ||
        galay_ssl_socket_recv(state->client, state->client_buffer, strlen("pong"), 1000).code != C_IOResultOk ||
        memcmp(state->client_buffer, "pong", strlen("pong")) != 0 ||
        galay_ssl_socket_shutdown(state->client, 1000).code != C_IOResultOk ||
        galay_ssl_socket_close(state->client, 1000).code != C_IOResultOk) {
        state->failed = 1;
    }
}

int main(void)
{
    char cert_path[512];
    char key_path[512];
    char ca_path[512];
    EchoExample state = {0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};

    if (make_path(cert_path, sizeof(cert_path), "server.crt") ||
        make_path(key_path, sizeof(key_path), "server.key") ||
        make_path(ca_path, sizeof(ca_path), "ca.crt") ||
        galay_ssl_context_create(GALAY_SSL_METHOD_TLS_SERVER, &state.server_context) != GALAY_OK ||
        galay_ssl_context_load_certificate(state.server_context, cert_path) != GALAY_OK ||
        galay_ssl_context_load_private_key(state.server_context, key_path) != GALAY_OK ||
        galay_ssl_context_create(GALAY_SSL_METHOD_TLS_CLIENT, &state.client_context) != GALAY_OK ||
        galay_ssl_context_load_ca(state.client_context, ca_path) != GALAY_OK ||
        galay_ssl_context_set_verify_mode(state.client_context, GALAY_SSL_VERIFY_PEER) != GALAY_OK ||
        galay_ssl_socket_create(state.server_context, C_IPTypeIPV4, &state.listener) != GALAY_OK ||
        galay_ssl_socket_bind(state.listener, &bind_host) != GALAY_OK ||
        galay_ssl_socket_listen(state.listener, 16) != GALAY_OK ||
        galay_ssl_socket_local_endpoint(state.listener, &state.peer) != GALAY_OK) {
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    galay_kernel_runtime_t runtime = {0};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return 2;
    }

    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    if (galay_coro_spawn(&runtime, server_entry, &state, NULL, &server).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &state, NULL, &client).code != C_IOResultOk ||
        galay_coro_join(&server, 3000).code != C_IOResultOk ||
        galay_coro_join(&client, 3000).code != C_IOResultOk) {
        return 3;
    }

    int result = state.failed ? 4 : 0;
    if (galay_coro_destroy(&server).code != C_IOResultOk ||
        galay_coro_destroy(&client).code != C_IOResultOk ||
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
        result = result == 0 ? 5 : result;
    }
    galay_ssl_socket_destroy(state.accepted);
    galay_ssl_socket_destroy(state.client);
    galay_ssl_socket_destroy(state.listener);
    galay_ssl_context_destroy(state.server_context);
    galay_ssl_context_destroy(state.client_context);
    return result;
}
