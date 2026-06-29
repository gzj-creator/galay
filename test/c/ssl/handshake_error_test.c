#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-ssl-c/ssl.h>

#include <string.h>

typedef struct HandshakeErrorState {
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
} HandshakeErrorState;

static int create_listener(HandshakeErrorState* state)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_ssl_socket_create(state->server_context, C_IPTypeIPV4, &state->listener) == GALAY_OK &&
        galay_ssl_socket_bind(state->listener, &bind_host) == GALAY_OK &&
        galay_ssl_socket_listen(state->listener, 16) == GALAY_OK &&
        galay_ssl_socket_local_endpoint(state->listener, &state->peer) == GALAY_OK &&
        state->peer.port != 0
        ? 0
        : 1;
}

static void server_entry(void* arg)
{
    HandshakeErrorState* state = (HandshakeErrorState*)arg;
    state->accept_result = galay_ssl_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code == C_IOResultOk) {
        state->server_handshake_result = galay_ssl_socket_handshake(state->accepted, 1000);
    }
}

static void client_entry(void* arg)
{
    HandshakeErrorState* state = (HandshakeErrorState*)arg;
    if (galay_ssl_socket_create(state->client_context, C_IPTypeIPV4, &state->client) != GALAY_OK) {
        state->connect_result.code = C_IOResultError;
        return;
    }
    state->connect_result = galay_ssl_socket_connect(state->client, &state->peer, 1000);
    if (state->connect_result.code == C_IOResultOk) {
        state->client_handshake_result = galay_ssl_socket_handshake(state->client, 1000);
    }
}

int main(void)
{
    HandshakeErrorState state = {0};
    if (galay_ssl_context_create(GALAY_SSL_METHOD_TLS_SERVER, &state.server_context) != GALAY_OK ||
        galay_ssl_context_create(GALAY_SSL_METHOD_TLS_CLIENT, &state.client_context) != GALAY_OK ||
        create_listener(&state) != 0) {
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

    const int failed = state.accept_result.code != C_IOResultOk ||
        state.connect_result.code != C_IOResultOk ||
        state.server_handshake_result.code == C_IOResultOk ||
        state.client_handshake_result.code == C_IOResultOk;

    if (galay_coro_destroy(&server).code != C_IOResultOk ||
        galay_coro_destroy(&client).code != C_IOResultOk ||
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
        return 4;
    }
    galay_ssl_socket_destroy(state.accepted);
    galay_ssl_socket_destroy(state.client);
    galay_ssl_socket_destroy(state.listener);
    galay_ssl_context_destroy(state.server_context);
    galay_ssl_context_destroy(state.client_context);
    return failed ? 5 : 0;
}
