#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef struct CallbackState {
    atomic_int done;
} CallbackState;

typedef struct RecvState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
    char* buffer;
    size_t length;
    C_Host from;
} RecvState;

typedef struct RecvLoopState {
    atomic_int count;
    atomic_int terminal;
    atomic_int errors;
    atomic_int zero_count;
    char* buffer;
    size_t length;
    int bytes[4];
    char messages[4][16];
    C_Host from[4];
} RecvLoopState;

typedef struct SendState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
    const char* buffer;
    size_t length;
    C_Host to;
} SendState;

typedef struct SendLoopState {
    atomic_int count;
    atomic_int terminal;
    atomic_int errors;
    const char* buffer;
    size_t length;
    C_Host to;
} SendLoopState;

typedef struct CloseState {
    atomic_int done;
    atomic_int code;
} CloseState;

static int expect_status(C_UdpSocketResultCode actual, C_UdpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int wait_at_least(atomic_int* value, int expected)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(value) >= expected) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static int wait_done(atomic_int* done)
{
    return wait_at_least(done, 1);
}

static int same_host(const C_Host* lhs, const C_Host* rhs)
{
    return lhs->type == rhs->type &&
        lhs->port == rhs->port &&
        strncmp(lhs->address, rhs->address, sizeof(lhs->address)) == 0;
}

static void mark_done(void* ctx)
{
    if (ctx != 0) {
        CallbackState* state = (CallbackState*)ctx;
        atomic_store(&state->done, 1);
    }
}

static void on_recv(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? 0 : (int)result->bytes);
    state->buffer = result == 0 ? 0 : result->buffer;
    state->length = result == 0 ? 0 : result->length;
    if (result != 0) {
        state->from = result->from;
    }
    atomic_store(&state->done, 1);
}

static int on_recv_loop(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    RecvLoopState* state = (RecvLoopState*)ctx;
    if (result == 0 || result->code != C_UdpSocketSuccess) {
        atomic_fetch_add(&state->errors, 1);
        atomic_store(&state->terminal, 1);
        return 1;
    }
    if (result->buffer != state->buffer || result->length != state->length) {
        atomic_fetch_add(&state->errors, 1);
        atomic_store(&state->terminal, 1);
        return 1;
    }

    int index = atomic_fetch_add(&state->count, 1);
    if (index < 4) {
        state->bytes[index] = (int)result->bytes;
        state->from[index] = result->from;
        size_t copy_length = result->bytes;
        if (copy_length >= sizeof(state->messages[index])) {
            copy_length = sizeof(state->messages[index]) - 1;
        }
        if (copy_length != 0) {
            memcpy(state->messages[index], result->buffer, copy_length);
        }
        state->messages[index][copy_length] = '\0';
    }
    if (result->bytes == 0) {
        atomic_fetch_add(&state->zero_count, 1);
    }
    if (index + 1 >= 3) {
        atomic_store(&state->terminal, 1);
        return 1;
    }
    return 0;
}

static void on_send(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    SendState* state = (SendState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    state->buffer = result == 0 ? 0 : result->buffer;
    state->length = result == 0 ? 0 : result->length;
    if (result != 0) {
        state->to = result->to;
    }
    atomic_store(&state->done, 1);
}

static int on_send_loop(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    SendLoopState* state = (SendLoopState*)ctx;
    if (result == 0 || result->code != C_UdpSocketSuccess || result->buffer != state->buffer ||
        result->length != state->length || result->bytes != state->length ||
        !same_host(&result->to, &state->to)) {
        atomic_fetch_add(&state->errors, 1);
        atomic_store(&state->terminal, 1);
        return 1;
    }

    int index = atomic_fetch_add(&state->count, 1);
    if (index + 1 >= 2) {
        atomic_store(&state->terminal, 1);
        return 1;
    }
    return 0;
}

static void on_close(C_UdpSocketResultCode code, void* ctx)
{
    CloseState* state = (CloseState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

static void on_close_mark(C_UdpSocketResultCode code, void* ctx)
{
    (void)code;
    mark_done(ctx);
}

static void on_recv_mark(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
}

static int on_recv_loop_mark(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
    return 1;
}

static void on_send_mark(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
}

static int on_send_loop_mark(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    (void)result;
    mark_done(ctx);
    return 1;
}

static void reset_callback_state(CallbackState* state)
{
    atomic_store(&state->done, 0);
}

static void wait_if_spawned(C_UdpSocketResultCode actual, CallbackState* state)
{
    if (actual == C_UdpSocketSuccess) {
        (void)wait_done(&state->done);
    }
}

static int expect_not_running(C_UdpSocketResultCode actual, CallbackState* state)
{
    wait_if_spawned(actual, state);
    return expect_status(actual, C_UdpSocketRuntimeNotRunning) ||
        atomic_load(&state->done) != 0;
}

static int close_socket(galay_kernel_runtime_t* runtime, galay_kernel_udp_socket_t* socket)
{
    CloseState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_UdpSocketIOFailed);
    if (galay_kernel_udp_socket_close(runtime, socket, on_close, &state) != C_UdpSocketSuccess) {
        return 1;
    }
    return wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_UdpSocketSuccess;
}

static int test_parameters(galay_kernel_runtime_t* runtime)
{
    galay_kernel_udp_socket_t socket = {0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host invalid_host = {C_IPTypeIPV4, "not-an-ip", 0};
    C_Host local = {0};
    char recv_buffer[8] = {0};
    const char send_buffer[] = "x";

    RecvState recv_state;
    memset(&recv_state, 0, sizeof(recv_state));
    atomic_init(&recv_state.done, 0);
    atomic_init(&recv_state.code, (int)C_UdpSocketIOFailed);
    atomic_init(&recv_state.bytes, 0);

    SendState send_state;
    memset(&send_state, 0, sizeof(send_state));
    atomic_init(&send_state.done, 0);
    atomic_init(&send_state.code, (int)C_UdpSocketIOFailed);
    atomic_init(&send_state.bytes, 0);

    if (galay_kernel_udp_socket_get_error(C_UdpSocketSuccess) == 0) {
        return 1;
    }
    if (expect_status(galay_kernel_udp_socket_create(0, C_IPTypeIPV4), C_UdpSocketParameterInvalid)) {
        return 2;
    }
    if (expect_status(galay_kernel_udp_socket_create(&socket, (C_IPType)99), C_UdpSocketParameterInvalid)) {
        return 3;
    }
    if (expect_status(galay_kernel_udp_socket_destroy(0), C_UdpSocketParameterInvalid)) {
        return 4;
    }
    if (expect_status(galay_kernel_udp_socket_create(&socket, C_IPTypeIPV4), C_UdpSocketSuccess)) {
        return 5;
    }
    if (socket.socket == 0) {
        return 6;
    }
    if (expect_status(galay_kernel_udp_socket_bind(0, &bind_host), C_UdpSocketParameterInvalid)) {
        return 7;
    }
    if (expect_status(galay_kernel_udp_socket_bind(&socket, 0), C_UdpSocketParameterInvalid)) {
        return 8;
    }
    if (expect_status(galay_kernel_udp_socket_bind(&socket, &invalid_host), C_UdpSocketParameterInvalid)) {
        return 9;
    }
    if (expect_status(galay_kernel_udp_socket_local_endpoint(0, &local), C_UdpSocketParameterInvalid)) {
        return 10;
    }
    if (expect_status(galay_kernel_udp_socket_local_endpoint(&socket, 0), C_UdpSocketParameterInvalid)) {
        return 11;
    }
    if (expect_status(galay_kernel_udp_socket_bind(&socket, &bind_host), C_UdpSocketSuccess)) {
        return 12;
    }
    if (expect_status(galay_kernel_udp_socket_local_endpoint(&socket, &local), C_UdpSocketSuccess)) {
        return 13;
    }
    if (local.type != C_IPTypeIPV4 || local.address[0] == '\0' || local.port == 0) {
        return 14;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom(0, &socket, recv_buffer, sizeof(recv_buffer), on_recv, &recv_state), C_UdpSocketParameterInvalid)) {
        return 15;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom(runtime, 0, recv_buffer, sizeof(recv_buffer), on_recv, &recv_state), C_UdpSocketParameterInvalid)) {
        return 16;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom(runtime, &socket, 0, sizeof(recv_buffer), on_recv, &recv_state), C_UdpSocketParameterInvalid)) {
        return 17;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom(runtime, &socket, recv_buffer, sizeof(recv_buffer), 0, &recv_state), C_UdpSocketParameterInvalid)) {
        return 18;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_loop(0, &socket, recv_buffer, sizeof(recv_buffer), on_recv_loop_mark, &recv_state), C_UdpSocketParameterInvalid)) {
        return 19;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_loop(runtime, 0, recv_buffer, sizeof(recv_buffer), on_recv_loop_mark, &recv_state), C_UdpSocketParameterInvalid)) {
        return 20;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_loop(runtime, &socket, 0, sizeof(recv_buffer), on_recv_loop_mark, &recv_state), C_UdpSocketParameterInvalid)) {
        return 21;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_loop(runtime, &socket, recv_buffer, sizeof(recv_buffer), 0, &recv_state), C_UdpSocketParameterInvalid)) {
        return 22;
    }
    if (expect_status(galay_kernel_udp_socket_sendto(0, &socket, send_buffer, sizeof(send_buffer) - 1, &local, on_send, &send_state), C_UdpSocketParameterInvalid)) {
        return 23;
    }
    if (expect_status(galay_kernel_udp_socket_sendto(runtime, 0, send_buffer, sizeof(send_buffer) - 1, &local, on_send, &send_state), C_UdpSocketParameterInvalid)) {
        return 24;
    }
    if (expect_status(galay_kernel_udp_socket_sendto(runtime, &socket, 0, sizeof(send_buffer) - 1, &local, on_send, &send_state), C_UdpSocketParameterInvalid)) {
        return 25;
    }
    if (expect_status(galay_kernel_udp_socket_sendto(runtime, &socket, send_buffer, sizeof(send_buffer) - 1, 0, on_send, &send_state), C_UdpSocketParameterInvalid)) {
        return 26;
    }
    if (expect_status(galay_kernel_udp_socket_sendto(runtime, &socket, send_buffer, sizeof(send_buffer) - 1, &local, 0, &send_state), C_UdpSocketParameterInvalid)) {
        return 27;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_loop(0, &socket, send_buffer, sizeof(send_buffer) - 1, &local, on_send_loop_mark, &send_state), C_UdpSocketParameterInvalid)) {
        return 28;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_loop(runtime, 0, send_buffer, sizeof(send_buffer) - 1, &local, on_send_loop_mark, &send_state), C_UdpSocketParameterInvalid)) {
        return 29;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_loop(runtime, &socket, 0, sizeof(send_buffer) - 1, &local, on_send_loop_mark, &send_state), C_UdpSocketParameterInvalid)) {
        return 30;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_loop(runtime, &socket, send_buffer, sizeof(send_buffer) - 1, 0, on_send_loop_mark, &send_state), C_UdpSocketParameterInvalid)) {
        return 31;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_loop(runtime, &socket, send_buffer, sizeof(send_buffer) - 1, &local, 0, &send_state), C_UdpSocketParameterInvalid)) {
        return 32;
    }
    if (expect_status(galay_kernel_udp_socket_close(0, &socket, on_close, &send_state), C_UdpSocketParameterInvalid)) {
        return 33;
    }
    if (expect_status(galay_kernel_udp_socket_close(runtime, 0, on_close, &send_state), C_UdpSocketParameterInvalid)) {
        return 34;
    }
    if (expect_status(galay_kernel_udp_socket_close(runtime, &socket, 0, &send_state), C_UdpSocketParameterInvalid)) {
        return 35;
    }
    if (expect_status(galay_kernel_udp_socket_destroy(&socket), C_UdpSocketSuccess)) {
        return 36;
    }
    return 0;
}

static int test_stopped_runtime(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_udp_socket_t socket = {0};
    C_Host host = {C_IPTypeIPV4, "127.0.0.1", 9};
    char recv_buffer[8] = {0};
    const char send_buffer[] = "x";
    int exit_code = 0;

    CallbackState callback_state;
    atomic_init(&callback_state.done, 0);

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 2;
        goto cleanup;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess) {
        exit_code = 3;
        goto cleanup;
    }
    if (expect_status(galay_kernel_udp_socket_create(&socket, C_IPTypeIPV4), C_UdpSocketSuccess)) {
        exit_code = 4;
        goto cleanup;
    }

    reset_callback_state(&callback_state);
    if (expect_not_running(galay_kernel_udp_socket_recvfrom(&runtime, &socket, recv_buffer, sizeof(recv_buffer), on_recv_mark, &callback_state), &callback_state)) {
        exit_code = 5;
        goto cleanup;
    }
    reset_callback_state(&callback_state);
    if (expect_not_running(galay_kernel_udp_socket_recvfrom_loop(&runtime, &socket, recv_buffer, sizeof(recv_buffer), on_recv_loop_mark, &callback_state), &callback_state)) {
        exit_code = 6;
        goto cleanup;
    }
    reset_callback_state(&callback_state);
    if (expect_not_running(galay_kernel_udp_socket_sendto(&runtime, &socket, send_buffer, sizeof(send_buffer) - 1, &host, on_send_mark, &callback_state), &callback_state)) {
        exit_code = 7;
        goto cleanup;
    }
    reset_callback_state(&callback_state);
    if (expect_not_running(galay_kernel_udp_socket_sendto(&runtime, &socket, 0, 0, &host, on_send_mark, &callback_state), &callback_state)) {
        exit_code = 8;
        goto cleanup;
    }
    reset_callback_state(&callback_state);
    if (expect_not_running(galay_kernel_udp_socket_sendto_loop(&runtime, &socket, send_buffer, sizeof(send_buffer) - 1, &host, on_send_loop_mark, &callback_state), &callback_state)) {
        exit_code = 9;
        goto cleanup;
    }
    reset_callback_state(&callback_state);
    if (expect_not_running(galay_kernel_udp_socket_close(&runtime, &socket, on_close_mark, &callback_state), &callback_state)) {
        exit_code = 10;
        goto cleanup;
    }

cleanup:
    if (socket.socket != 0) {
        (void)galay_kernel_udp_socket_destroy(&socket);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}

static int test_loopback_datagrams(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_udp_socket_t server = {0};
    galay_kernel_udp_socket_t client = {0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host server_local = {0};
    C_Host client_local = {0};
    int exit_code = 0;

    const char request[] = "ping";
    char recv_buffer[32] = {0};
    RecvState recv_state;
    memset(&recv_state, 0, sizeof(recv_state));
    atomic_init(&recv_state.done, 0);
    atomic_init(&recv_state.code, (int)C_UdpSocketIOFailed);
    atomic_init(&recv_state.bytes, 0);

    SendState send_state;
    memset(&send_state, 0, sizeof(send_state));
    atomic_init(&send_state.done, 0);
    atomic_init(&send_state.code, (int)C_UdpSocketIOFailed);
    atomic_init(&send_state.bytes, 0);

    char loop_buffer[32] = {0};
    RecvLoopState recv_loop_state;
    memset(&recv_loop_state, 0, sizeof(recv_loop_state));
    atomic_init(&recv_loop_state.count, 0);
    atomic_init(&recv_loop_state.terminal, 0);
    atomic_init(&recv_loop_state.errors, 0);
    atomic_init(&recv_loop_state.zero_count, 0);
    recv_loop_state.buffer = loop_buffer;
    recv_loop_state.length = sizeof(loop_buffer);

    SendState zero_send_state;
    memset(&zero_send_state, 0, sizeof(zero_send_state));
    atomic_init(&zero_send_state.done, 0);
    atomic_init(&zero_send_state.code, (int)C_UdpSocketIOFailed);
    atomic_init(&zero_send_state.bytes, -1);

    const char loop_payload[] = "loop";
    SendLoopState send_loop_state;
    memset(&send_loop_state, 0, sizeof(send_loop_state));
    atomic_init(&send_loop_state.count, 0);
    atomic_init(&send_loop_state.terminal, 0);
    atomic_init(&send_loop_state.errors, 0);
    send_loop_state.buffer = loop_payload;
    send_loop_state.length = sizeof(loop_payload) - 1;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_udp_socket_create(&server, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_create(&client, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&server, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&client, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&server, &server_local) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&client, &client_local) != C_UdpSocketSuccess) {
        exit_code = 1;
        goto cleanup;
    }
    send_loop_state.to = server_local;

    if (galay_kernel_udp_socket_recvfrom(&runtime, &server, recv_buffer, sizeof(recv_buffer), on_recv, &recv_state) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_sendto(&runtime, &client, request, sizeof(request) - 1, &server_local, on_send, &send_state) != C_UdpSocketSuccess) {
        exit_code = 2;
        goto cleanup;
    }
    if (wait_done(&send_state.done) != 0 ||
        wait_done(&recv_state.done) != 0 ||
        atomic_load(&send_state.code) != (int)C_UdpSocketSuccess ||
        atomic_load(&send_state.bytes) != (int)(sizeof(request) - 1) ||
        send_state.buffer != request ||
        send_state.length != sizeof(request) - 1 ||
        !same_host(&send_state.to, &server_local) ||
        atomic_load(&recv_state.code) != (int)C_UdpSocketSuccess ||
        atomic_load(&recv_state.bytes) != (int)(sizeof(request) - 1) ||
        recv_state.buffer != recv_buffer ||
        recv_state.length != sizeof(recv_buffer) ||
        memcmp(recv_buffer, request, sizeof(request) - 1) != 0 ||
        !same_host(&recv_state.from, &client_local)) {
        exit_code = 3;
        goto cleanup;
    }

    if (galay_kernel_udp_socket_recvfrom_loop(
            &runtime,
            &server,
            loop_buffer,
            sizeof(loop_buffer),
            on_recv_loop,
            &recv_loop_state) != C_UdpSocketSuccess) {
        exit_code = 4;
        goto cleanup;
    }
    if (galay_kernel_udp_socket_sendto(&runtime, &client, 0, 0, &server_local, on_send, &zero_send_state) != C_UdpSocketSuccess) {
        exit_code = 5;
        goto cleanup;
    }
    if (wait_done(&zero_send_state.done) != 0 ||
        wait_at_least(&recv_loop_state.count, 1) != 0 ||
        atomic_load(&zero_send_state.code) != (int)C_UdpSocketSuccess ||
        atomic_load(&zero_send_state.bytes) != 0 ||
        zero_send_state.buffer != 0 ||
        zero_send_state.length != 0 ||
        !same_host(&zero_send_state.to, &server_local) ||
        recv_loop_state.bytes[0] != 0 ||
        atomic_load(&recv_loop_state.zero_count) != 1 ||
        atomic_load(&recv_loop_state.terminal) != 0) {
        exit_code = 6;
        goto cleanup;
    }

    if (galay_kernel_udp_socket_sendto_loop(
            &runtime,
            &client,
            loop_payload,
            sizeof(loop_payload) - 1,
            &server_local,
            on_send_loop,
            &send_loop_state) != C_UdpSocketSuccess) {
        exit_code = 7;
        goto cleanup;
    }
    if (wait_done(&send_loop_state.terminal) != 0 ||
        wait_done(&recv_loop_state.terminal) != 0 ||
        atomic_load(&send_loop_state.count) != 2 ||
        atomic_load(&send_loop_state.errors) != 0 ||
        atomic_load(&recv_loop_state.count) != 3 ||
        atomic_load(&recv_loop_state.errors) != 0 ||
        recv_loop_state.bytes[1] != (int)(sizeof(loop_payload) - 1) ||
        recv_loop_state.bytes[2] != (int)(sizeof(loop_payload) - 1) ||
        strcmp(recv_loop_state.messages[1], loop_payload) != 0 ||
        strcmp(recv_loop_state.messages[2], loop_payload) != 0 ||
        !same_host(&recv_loop_state.from[0], &client_local) ||
        !same_host(&recv_loop_state.from[1], &client_local) ||
        !same_host(&recv_loop_state.from[2], &client_local)) {
        exit_code = 8;
        goto cleanup;
    }

    if (close_socket(&runtime, &client) != 0 ||
        close_socket(&runtime, &server) != 0) {
        exit_code = 9;
        goto cleanup;
    }

cleanup:
    if (client.socket != 0) {
        (void)galay_kernel_udp_socket_destroy(&client);
    }
    if (server.socket != 0) {
        (void)galay_kernel_udp_socket_destroy(&server);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    int parameter_result = test_parameters(&runtime);
    if (parameter_result != 0) {
        exit_code = 100 + parameter_result;
        goto cleanup;
    }

cleanup:
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    if (exit_code != 0) {
        return exit_code;
    }

    int stopped_result = test_stopped_runtime();
    if (stopped_result != 0) {
        return 200 + stopped_result;
    }

    int loopback_result = test_loopback_datagrams();
    if (loopback_result != 0) {
        return 300 + loopback_result;
    }

    return 0;
}
