#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef struct RecvState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
    char* buffer;
    size_t length;
} RecvState;

typedef struct RecvLoopState {
    atomic_int done;
    atomic_int code;
    atomic_int calls;
    atomic_int bytes;
} RecvLoopState;

typedef struct SendState {
    atomic_int done;
    atomic_int code;
} SendState;

typedef struct CloseState {
    atomic_int done;
    atomic_int code;
} CloseState;

static int expect_status(C_UdpSocketResultCode actual, C_UdpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static void on_recv(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    state->buffer = result == 0 ? 0 : result->buffer;
    state->length = result == 0 ? 0 : result->length;
    atomic_store(&state->done, 1);
}

static int on_recv_loop(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    RecvLoopState* state = (RecvLoopState*)ctx;
    atomic_fetch_add(&state->calls, 1);
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    atomic_store(&state->done, 1);
    return 0;
}

static void on_send(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    SendState* state = (SendState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->done, 1);
}

static int on_send_loop(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    SendState* state = (SendState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->done, 1);
    return 1;
}

static void on_close(C_UdpSocketResultCode code, void* ctx)
{
    CloseState* state = (CloseState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

static int create_bound_socket(galay_kernel_udp_socket_t* socket, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    if (galay_kernel_udp_socket_create(socket, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(socket, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(socket, local) != C_UdpSocketSuccess ||
        local->port == 0) {
        return 1;
    }
    return 0;
}

static int test_recvfrom_timeout(galay_kernel_runtime_t* runtime)
{
    galay_kernel_udp_socket_t socket = {0};
    C_Host local = {0};
    char recv_buffer[16] = {0};

    RecvState state;
    memset(&state, 0, sizeof(state));
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_UdpSocketSuccess);
    atomic_init(&state.bytes, -1);

    if (create_bound_socket(&socket, &local) != 0) {
        return 101;
    }
    (void)local;

    if (expect_status(galay_kernel_udp_socket_recvfrom_timeout(0, &socket, recv_buffer,
                                                               sizeof(recv_buffer), 10,
                                                               on_recv, &state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 102;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_timeout(runtime, &socket, 0,
                                                               sizeof(recv_buffer), 10,
                                                               on_recv, &state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 103;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_timeout(runtime, &socket, recv_buffer,
                                                               sizeof(recv_buffer), 10,
                                                               0, &state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 104;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_timeout(runtime, &socket, recv_buffer,
                                                               sizeof(recv_buffer), 10,
                                                               on_recv, &state),
                      C_UdpSocketSuccess)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 105;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_UdpSocketTimeout ||
        atomic_load(&state.bytes) != 0 ||
        state.buffer != recv_buffer ||
        state.length != sizeof(recv_buffer)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 106;
    }

    galay_kernel_udp_socket_destroy(&socket);
    return 0;
}

static int test_recvfrom_loop_timeout(galay_kernel_runtime_t* runtime)
{
    galay_kernel_udp_socket_t socket = {0};
    C_Host local = {0};
    char recv_buffer[16] = {0};

    RecvLoopState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_UdpSocketSuccess);
    atomic_init(&state.calls, 0);
    atomic_init(&state.bytes, -1);

    if (create_bound_socket(&socket, &local) != 0) {
        return 201;
    }
    (void)local;

    if (expect_status(galay_kernel_udp_socket_recvfrom_loop_timeout(0, &socket, recv_buffer,
                                                                    sizeof(recv_buffer), 10,
                                                                    on_recv_loop, &state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 202;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_loop_timeout(runtime, &socket, 0,
                                                                    sizeof(recv_buffer), 10,
                                                                    on_recv_loop, &state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 203;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_loop_timeout(runtime, &socket, recv_buffer,
                                                                    sizeof(recv_buffer), 10,
                                                                    0, &state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 204;
    }
    if (expect_status(galay_kernel_udp_socket_recvfrom_loop_timeout(runtime, &socket, recv_buffer,
                                                                    sizeof(recv_buffer), 10,
                                                                    on_recv_loop, &state),
                      C_UdpSocketSuccess)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 205;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.calls) != 1 ||
        atomic_load(&state.code) != (int)C_UdpSocketTimeout ||
        atomic_load(&state.bytes) != 0) {
        galay_kernel_udp_socket_destroy(&socket);
        return 206;
    }

    galay_kernel_udp_socket_destroy(&socket);
    return 0;
}

static int test_timeout_api_parameters(galay_kernel_runtime_t* runtime)
{
    galay_kernel_udp_socket_t socket = {0};
    C_Host local = {0};
    const char send_buffer[] = "x";

    SendState send_state;
    atomic_init(&send_state.done, 0);
    atomic_init(&send_state.code, (int)C_UdpSocketSuccess);

    CloseState close_state;
    atomic_init(&close_state.done, 0);
    atomic_init(&close_state.code, (int)C_UdpSocketSuccess);

    if (strcmp(galay_kernel_udp_socket_get_error(C_UdpSocketTimeout), "timeout") != 0) {
        return 301;
    }
    if (create_bound_socket(&socket, &local) != 0) {
        return 302;
    }

    if (expect_status(galay_kernel_udp_socket_sendto_timeout(0, &socket, send_buffer,
                                                             sizeof(send_buffer) - 1, &local,
                                                             10, on_send, &send_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 303;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_timeout(runtime, &socket, 0,
                                                             sizeof(send_buffer) - 1, &local,
                                                             10, on_send, &send_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 304;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_timeout(runtime, &socket, send_buffer,
                                                             sizeof(send_buffer) - 1, 0,
                                                             10, on_send, &send_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 305;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_timeout(runtime, &socket, send_buffer,
                                                             sizeof(send_buffer) - 1, &local,
                                                             10, 0, &send_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 306;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_loop_timeout(0, &socket, send_buffer,
                                                                  sizeof(send_buffer) - 1, &local,
                                                                  10, on_send_loop, &send_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 307;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_loop_timeout(runtime, &socket, 0,
                                                                  sizeof(send_buffer) - 1, &local,
                                                                  10, on_send_loop, &send_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 308;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_loop_timeout(runtime, &socket, send_buffer,
                                                                  sizeof(send_buffer) - 1, 0,
                                                                  10, on_send_loop, &send_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 309;
    }
    if (expect_status(galay_kernel_udp_socket_sendto_loop_timeout(runtime, &socket, send_buffer,
                                                                  sizeof(send_buffer) - 1, &local,
                                                                  10, 0, &send_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 310;
    }
    if (expect_status(galay_kernel_udp_socket_close_timeout(0, &socket, 10, on_close, &close_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 311;
    }
    if (expect_status(galay_kernel_udp_socket_close_timeout(runtime, 0, 10, on_close, &close_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 312;
    }
    if (expect_status(galay_kernel_udp_socket_close_timeout(runtime, &socket, 10, 0, &close_state),
                      C_UdpSocketParameterInvalid)) {
        galay_kernel_udp_socket_destroy(&socket);
        return 313;
    }
    galay_kernel_udp_socket_destroy(&socket);
    return 0;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    int result = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        galay_kernel_runtime_destroy(&runtime);
        return 2;
    }

    result = test_recvfrom_timeout(&runtime);
    if (result == 0) {
        result = test_recvfrom_loop_timeout(&runtime);
    }
    if (result == 0) {
        result = test_timeout_api_parameters(&runtime);
    }

    galay_kernel_runtime_stop(&runtime);
    galay_kernel_runtime_destroy(&runtime);
    return result;
}
