#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int expect_code(C_IOResult result, C_IOResultCode expected)
{
    return result.code == expected ? 0 : 1;
}

static atomic_int g_cleanup_failed;

static void record_cleanup_failed(void)
{
    atomic_store(&g_cleanup_failed, 1);
}

static int take_cleanup_failed(void)
{
    return atomic_exchange(&g_cleanup_failed, 0);
}

static int merge_cleanup_status(int result, int cleanup_code)
{
    const int cleanup_failed = take_cleanup_failed();
    return result == 0 && cleanup_failed ? cleanup_code : result;
}

static void record_io_cleanup(C_IOResult result)
{
    if (result.code != C_IOResultOk && result.code != C_IOResultInvalid) {
        record_cleanup_failed();
    }
}

static void record_socket_cleanup(C_TcpSocketResultCode result)
{
    if (result != C_TcpSocketSuccess && result != C_TcpSocketParameterInvalid) {
        record_cleanup_failed();
    }
}

static void record_runtime_cleanup(C_RuntimeResultCode result)
{
    if (result != C_RuntimeSuccess && result != C_RuntimeParameterInvalid) {
        record_cleanup_failed();
    }
}

static void record_posix_cleanup(int result)
{
    if (result != 0) {
        record_cleanup_failed();
    }
}

static int checked_memset(void* buffer, int value, size_t size)
{
    return memset(buffer, value, size) == buffer ? 0 : 1;
}

static void record_diagnostic_write(int result)
{
    if (result < 0) {
        record_cleanup_failed();
    }
}

static int create_listener(galay_kernel_tcp_socket_t* listener, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    if (galay_kernel_tcp_socket_create(listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(listener, 16) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_local_endpoint(listener, local) != C_TcpSocketSuccess ||
        local->port == 0) {
        return 1;
    }
    return 0;
}

static int connect_posix_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        record_posix_cleanup(close(fd));
        return -1;
    }
    if (connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
        record_posix_cleanup(close(fd));
        return -1;
    }
    return fd;
}

static int recv_until_quiet(int fd, char marker, int* saw_marker)
{
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    record_posix_cleanup(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout)));

    char buffer[4096];
    for (int reads = 0; reads < 4096; ++reads) {
        ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received == 0) {
            return 0;
        }
        if (received < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : errno;
        }
        for (ssize_t i = 0; i < received; ++i) {
            if (buffer[i] == marker) {
                *saw_marker = 1;
            }
        }
    }
    return EOVERFLOW;
}

typedef struct EchoState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    galay_kernel_tcp_socket_t client;
    C_IOResult accept_result;
    C_IOResult connect_result;
    C_IOResult server_recv_result;
    C_IOResult server_send_result;
    C_IOResult client_send_result;
    C_IOResult client_recv_result;
    C_IOResult server_close_result;
    C_IOResult client_close_result;
    char server_buffer[64];
    char client_buffer[64];
} EchoState;

typedef struct IovSendfileState {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    C_Host accepted_peer;
    int file_fd;
    C_IOResult accept_result;
    C_IOResult readv_result;
    C_IOResult writev_result;
    C_IOResult sendfile_result;
    C_IOResult close_result;
    char read_a[8];
    char read_b[8];
} IovSendfileState;

static void echo_server_entry(void* arg)
{
    static const char response[] = "coro-pong-response";
    EchoState* state = (EchoState*)arg;
    state->accept_result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }

    const size_t expected = strlen("coro-ping-request");
    size_t received = 0;
    while (received < expected) {
        const size_t remaining = expected - received;
        const size_t chunk = remaining < 5 ? remaining : 5;
        C_IOResult result = galay_kernel_tcp_socket_recv(&state->accepted,
                                                state->server_buffer + received,
                                                chunk,
                                                1000);
        state->server_recv_result = result;
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return;
        }
        received += result.bytes;
    }

    size_t sent = 0;
    const size_t length = sizeof(response) - 1;
    while (sent < length) {
        const size_t remaining = length - sent;
        const size_t chunk = remaining < 4 ? remaining : 4;
        C_IOResult result = galay_kernel_tcp_socket_send(&state->accepted,
                                                response + sent,
                                                chunk,
                                                1000);
        state->server_send_result = result;
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return;
        }
        sent += result.bytes;
    }

    state->server_close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void echo_client_entry(void* arg)
{
    static const char request[] = "coro-ping-request";
    EchoState* state = (EchoState*)arg;
    if (galay_kernel_tcp_socket_create(&state->client, C_IPTypeIPV4) != C_TcpSocketSuccess) {
        state->connect_result.code = C_IOResultError;
        return;
    }
    state->connect_result = galay_kernel_tcp_socket_connect(&state->client, &state->peer, 1000);
    if (state->connect_result.code != C_IOResultOk) {
        return;
    }

    size_t sent = 0;
    const size_t request_len = sizeof(request) - 1;
    while (sent < request_len) {
        const size_t remaining = request_len - sent;
        const size_t chunk = remaining < 4 ? remaining : 4;
        C_IOResult result = galay_kernel_tcp_socket_send(&state->client,
                                                request + sent,
                                                chunk,
                                                1000);
        state->client_send_result = result;
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return;
        }
        sent += result.bytes;
    }

    const size_t expected = strlen("coro-pong-response");
    size_t received = 0;
    while (received < expected) {
        const size_t remaining = expected - received;
        const size_t chunk = remaining < 3 ? remaining : 3;
        C_IOResult result = galay_kernel_tcp_socket_recv(&state->client,
                                                state->client_buffer + received,
                                                chunk,
                                                1000);
        state->client_recv_result = result;
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return;
        }
        received += result.bytes;
    }

    state->client_close_result = galay_kernel_tcp_socket_close(&state->client, 1000);
}

static void iov_sendfile_server_entry(void* arg)
{
    IovSendfileState* state = (IovSendfileState*)arg;
    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener,
                                       &state->accepted,
                                       &state->accepted_peer,
                                       1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }

    struct iovec read_iov[2];
    read_iov[0].iov_base = state->read_a;
    read_iov[0].iov_len = 3;
    read_iov[1].iov_base = state->read_b;
    read_iov[1].iov_len = 4;
    state->readv_result =
        galay_kernel_tcp_socket_readv(&state->accepted, read_iov, 2, 1000);
    if (state->readv_result.code != C_IOResultOk) {
        return;
    }

    const char write_a[] = "iov-";
    const char write_b[] = "reply";
    struct iovec write_iov[2];
    write_iov[0].iov_base = (void*)write_a;
    write_iov[0].iov_len = sizeof(write_a) - 1;
    write_iov[1].iov_base = (void*)write_b;
    write_iov[1].iov_len = sizeof(write_b) - 1;
    state->writev_result =
        galay_kernel_tcp_socket_writev(&state->accepted, write_iov, 2, 1000);
    if (state->writev_result.code != C_IOResultOk) {
        return;
    }

    state->sendfile_result =
        galay_kernel_tcp_socket_sendfile(&state->accepted, state->file_fd, 6, 9, 1000);
    if (state->sendfile_result.code != C_IOResultOk) {
        return;
    }

    state->close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

typedef struct AcceptTimeoutState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    galay_kernel_tcp_socket_t accepted_late;
    galay_kernel_tcp_socket_t late_client;
    C_IOResult result;
    C_IOResult late_accept_result;
    C_IOResult late_connect_result;
    C_IOResult late_client_close_result;
    C_IOResult late_server_close_result;
    atomic_int phase;
} AcceptTimeoutState;

static void accept_timeout_entry(void* arg)
{
    AcceptTimeoutState* state = (AcceptTimeoutState*)arg;
    state->result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 5);
}

static void accept_timeout_late_accept_entry(void* arg)
{
    AcceptTimeoutState* state = (AcceptTimeoutState*)arg;
    state->late_accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted_late, NULL, 1000);
}

static void accept_timeout_late_client_entry(void* arg)
{
    AcceptTimeoutState* state = (AcceptTimeoutState*)arg;
    while (atomic_load(&state->phase) != 1) {
        record_io_cleanup(galay_coro_yield());
    }
    if (galay_kernel_tcp_socket_create(&state->late_client, C_IPTypeIPV4) !=
        C_TcpSocketSuccess) {
        state->late_connect_result.code = C_IOResultError;
        return;
    }
    state->late_connect_result =
        galay_kernel_tcp_socket_connect(&state->late_client, &state->peer, 1000);
    if (state->late_connect_result.code == C_IOResultOk) {
        state->late_client_close_result = galay_kernel_tcp_socket_close(&state->late_client, 1000);
    }
}

typedef struct RecvTimeoutState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    galay_kernel_tcp_socket_t client;
    C_IOResult accept_result;
    C_IOResult connect_result;
    C_IOResult recv_result;
    C_IOResult late_send_result;
    C_IOResult late_recv_result;
    C_IOResult server_close_result;
    C_IOResult client_close_result;
    atomic_int phase;
    char buffer[8];
    char late_buffer[8];
} RecvTimeoutState;

static void recv_timeout_server_entry(void* arg)
{
    RecvTimeoutState* state = (RecvTimeoutState*)arg;
    state->accept_result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    state->recv_result = galay_kernel_tcp_socket_recv(&state->accepted,
                                             state->buffer,
                                             sizeof(state->buffer),
                                             5);
    atomic_store(&state->phase, 1);
    state->late_recv_result = galay_kernel_tcp_socket_recv(&state->accepted,
                                                  state->late_buffer,
                                                  sizeof(state->late_buffer),
                                                  1000);
    state->server_close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void recv_timeout_client_entry(void* arg)
{
    RecvTimeoutState* state = (RecvTimeoutState*)arg;
    if (galay_kernel_tcp_socket_create(&state->client, C_IPTypeIPV4) != C_TcpSocketSuccess) {
        state->connect_result.code = C_IOResultError;
        return;
    }
    state->connect_result = galay_kernel_tcp_socket_connect(&state->client, &state->peer, 1000);
    if (state->connect_result.code != C_IOResultOk) {
        return;
    }
    while (atomic_load(&state->phase) != 1) {
        record_io_cleanup(galay_coro_yield());
    }
    state->late_send_result = galay_kernel_tcp_socket_send(&state->client, "late", 4, 1000);
    state->client_close_result = galay_kernel_tcp_socket_close(&state->client, 1000);
}

typedef struct CloseWaitingState {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult recv_result;
    C_IOResult close_result;
    atomic_int phase;
    char buffer[8];
} CloseWaitingState;

static void close_waiting_server_entry(void* arg)
{
    CloseWaitingState* state = (CloseWaitingState*)arg;
    state->accept_result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        atomic_store(&state->phase, 3);
        return;
    }
    atomic_store(&state->phase, 1);
    state->recv_result = galay_kernel_tcp_socket_recv(&state->accepted,
                                             state->buffer,
                                             sizeof(state->buffer),
                                             1000);
    atomic_store(&state->phase, 2);
}

static void close_waiting_closer_entry(void* arg)
{
    CloseWaitingState* state = (CloseWaitingState*)arg;
    while (atomic_load(&state->phase) != 1) {
        record_io_cleanup(galay_coro_yield());
    }
    state->close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

typedef struct ZeroTimeoutConnectState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t client;
    galay_kernel_tcp_socket_t accepted_from_zero;
    galay_kernel_tcp_socket_t accepted_after_connect;
    C_IOResult zero_connect_result;
    C_IOResult zero_accept_probe_result;
    C_IOResult connect_result;
    C_IOResult accept_result;
    C_IOResult close_client_result;
    C_IOResult close_server_result;
} ZeroTimeoutConnectState;

typedef struct ImmediateAcceptState {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
} ImmediateAcceptState;

typedef struct ConnectTimeoutCloseState {
    C_Host peer;
    galay_kernel_tcp_socket_t client;
    C_IOResult connect_result;
    C_TcpSocketResultCode endpoint_after_timeout;
} ConnectTimeoutCloseState;

typedef struct SendTimeoutState {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult send_result;
    C_IOResult close_result;
    int fill_errno;
    size_t filled_bytes;
} SendTimeoutState;

static void immediate_accept_entry(void* arg)
{
    ImmediateAcceptState* state = (ImmediateAcceptState*)arg;
    state->accept_result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
}

static void connect_timeout_close_entry(void* arg)
{
    ConnectTimeoutCloseState* state = (ConnectTimeoutCloseState*)arg;
    if (galay_kernel_tcp_socket_create(&state->client, C_IPTypeIPV4) != C_TcpSocketSuccess) {
        state->connect_result.code = C_IOResultError;
        return;
    }

    state->connect_result = galay_kernel_tcp_socket_connect(&state->client, &state->peer, 1);
    if (state->connect_result.code == C_IOResultTimeout) {
        C_Host endpoint = {0};
        state->endpoint_after_timeout =
            galay_kernel_tcp_socket_local_endpoint(&state->client, &endpoint);
    }
}

static void send_timeout_server_entry(void* arg)
{
    SendTimeoutState* state = (SendTimeoutState*)arg;
    state->accept_result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }

    int send_buffer_size = 4096;
    record_posix_cleanup(setsockopt((int)state->accept_result.value,
                     SOL_SOCKET,
                     SO_SNDBUF,
                     &send_buffer_size,
                     (socklen_t)sizeof(send_buffer_size)));

    char fill[4096];
    if (checked_memset(fill, 'a', sizeof(fill)) != 0) {
        state->fill_errno = EFAULT;
        return;
    }
    for (;;) {
        ssize_t sent = send((int)state->accept_result.value, fill, sizeof(fill), 0);
        if (sent > 0) {
            state->filled_bytes += (size_t)sent;
            if (state->filled_bytes > 4 * 1024 * 1024) {
                state->fill_errno = EOVERFLOW;
                return;
            }
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        state->fill_errno = errno != 0 ? errno : EIO;
        return;
    }

    state->send_result = galay_kernel_tcp_socket_send(&state->accepted, "z", 1, 1);
    state->close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void zero_timeout_connect_entry(void* arg)
{
    ZeroTimeoutConnectState* state = (ZeroTimeoutConnectState*)arg;
    if (galay_kernel_tcp_socket_create(&state->client, C_IPTypeIPV4) != C_TcpSocketSuccess) {
        state->zero_connect_result.code = C_IOResultError;
        return;
    }

    state->zero_connect_result = galay_kernel_tcp_socket_connect(&state->client, &state->peer, 0);
    state->zero_accept_probe_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted_from_zero, NULL, 20);
    if (state->zero_accept_probe_result.code == C_IOResultOk) {
        record_io_cleanup(galay_kernel_tcp_socket_close(&state->accepted_from_zero, 1000));
    }

    state->connect_result = galay_kernel_tcp_socket_connect(&state->client, &state->peer, 1000);
    if (state->connect_result.code == C_IOResultOk) {
        state->accept_result =
            galay_kernel_tcp_socket_accept(state->listener, &state->accepted_after_connect, NULL, 1000);
    }
    if (state->client.socket != 0) {
        state->close_client_result = galay_kernel_tcp_socket_close(&state->client, 12345);
    }
}

typedef struct MultiSchedulerMisuseState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t client;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult connect_result;
    C_IOResult accept_result;
    C_IOResult misuse_send_result;
    C_IOResult owner_close_client_result;
    C_IOResult owner_close_server_result;
    atomic_int phase;
} MultiSchedulerMisuseState;

typedef struct OwnerBindingPollutionState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t client;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult poison_send_result;
    C_IOResult connect_result;
    C_IOResult accept_result;
    C_IOResult close_client_result;
    C_IOResult close_server_result;
} OwnerBindingPollutionState;

static void multi_scheduler_owner_entry(void* arg)
{
    MultiSchedulerMisuseState* state = (MultiSchedulerMisuseState*)arg;
    if (galay_kernel_tcp_socket_create(&state->client, C_IPTypeIPV4) != C_TcpSocketSuccess) {
        state->connect_result.code = C_IOResultError;
        atomic_store(&state->phase, 3);
        return;
    }
    state->connect_result = galay_kernel_tcp_socket_connect(&state->client, &state->peer, 1000);
    if (state->connect_result.code == C_IOResultOk) {
        state->accept_result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    }
    atomic_store(&state->phase, 1);
    while (atomic_load(&state->phase) == 1) {
        record_io_cleanup(galay_coro_yield());
    }
    if (state->client.socket != 0) {
        state->owner_close_client_result = galay_kernel_tcp_socket_close(&state->client, 1000);
    }
    if (state->accepted.socket != 0) {
        state->owner_close_server_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
    }
}

static void multi_scheduler_misuse_entry(void* arg)
{
    MultiSchedulerMisuseState* state = (MultiSchedulerMisuseState*)arg;
    while (atomic_load(&state->phase) == 0) {
        record_io_cleanup(galay_coro_yield());
    }
    if (atomic_load(&state->phase) == 1) {
        state->misuse_send_result = galay_kernel_tcp_socket_send(&state->client, "x", 1, 1000);
    }
    atomic_store(&state->phase, 2);
}

static void owner_binding_poison_entry(void* arg)
{
    OwnerBindingPollutionState* state = (OwnerBindingPollutionState*)arg;
    if (galay_kernel_tcp_socket_create(&state->client, C_IPTypeIPV4) != C_TcpSocketSuccess) {
        state->poison_send_result.code = C_IOResultError;
        return;
    }
    state->poison_send_result = galay_kernel_tcp_socket_send(&state->client, "x", 1, 20);
}

static void owner_binding_reuse_entry(void* arg)
{
    OwnerBindingPollutionState* state = (OwnerBindingPollutionState*)arg;
    state->connect_result = galay_kernel_tcp_socket_connect(&state->client, &state->peer, 1000);
    if (state->connect_result.code == C_IOResultOk) {
        state->accept_result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    }
    if (state->client.socket != 0) {
        state->close_client_result = galay_kernel_tcp_socket_close(&state->client, 1000);
    }
    if (state->accepted.socket != 0) {
        state->close_server_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
    }
}

static int run_echo(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    EchoState state = {0};
    state.listener = &listener;

    if (create_listener(&listener, &local) != 0) {
        return 101;
    }
    state.peer = local;

    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    if (expect_code(galay_coro_spawn(runtime, echo_server_entry, &state, 0, &server),
                    C_IOResultOk) ||
        expect_code(galay_coro_spawn(runtime, echo_client_entry, &state, 0, &client),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&server, 2000), C_IOResultOk) ||
        expect_code(galay_coro_join(&client, 2000), C_IOResultOk)) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 102;
    }

    const int failed =
        state.accept_result.code != C_IOResultOk ||
        state.connect_result.code != C_IOResultOk ||
        state.server_recv_result.code != C_IOResultOk ||
        state.server_send_result.code != C_IOResultOk ||
        state.client_send_result.code != C_IOResultOk ||
        state.client_recv_result.code != C_IOResultOk ||
        state.server_close_result.code != C_IOResultOk ||
        state.client_close_result.code != C_IOResultOk ||
        state.server_recv_result.bytes == 0 ||
        state.server_send_result.bytes == 0 ||
        state.client_send_result.bytes == 0 ||
        state.client_recv_result.bytes == 0 ||
        memcmp(state.server_buffer, "coro-ping-request", strlen("coro-ping-request")) != 0 ||
        memcmp(state.client_buffer, "coro-pong-response", strlen("coro-pong-response")) != 0;

    record_io_cleanup(galay_coro_destroy(&server));
    record_io_cleanup(galay_coro_destroy(&client));
    if (state.accepted.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted));
    }
    if (state.client.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.client));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    return failed ? 103 : 0;
}

static int run_iov_sendfile(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    int client_fd = -1;
    char template_path[] = "/tmp/galay-coro-tcp-sendfile-XXXXXX";
    IovSendfileState state = {0};
    state.listener = &listener;
    if (checked_memset(state.read_a, '?', sizeof(state.read_a)) != 0 ||
        checked_memset(state.read_b, '?', sizeof(state.read_b)) != 0) {
        return 164;
    }

    if (create_listener(&listener, &local) != 0) {
        return 151;
    }

    state.file_fd = mkstemp(template_path);
    if (state.file_fd < 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 152;
    }
    record_posix_cleanup(unlink(template_path));
    if (write(state.file_fd, "file-send-slice-data", 20) != 20) {
        record_posix_cleanup(close(state.file_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 153;
    }

    galay_coro_task_t server = {0};
    if (expect_code(galay_coro_spawn(runtime, iov_sendfile_server_entry, &state, 0, &server),
                    C_IOResultOk)) {
        record_posix_cleanup(close(state.file_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 154;
    }

    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        record_posix_cleanup(close(state.file_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 155;
    }
    if (write(client_fd, "abc1234", 7) != 7) {
        record_posix_cleanup(close(client_fd));
        record_posix_cleanup(close(state.file_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 156;
    }
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    record_posix_cleanup(setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout)));

    char peer_buffer[64] = {0};
    size_t received = 0;
    const size_t expected = strlen("iov-replyend-slice");
    while (received < expected) {
        ssize_t n = recv(client_fd,
                         peer_buffer + received,
                         expected - received,
                         0);
        if (n <= 0) {
            break;
        }
        received += (size_t)n;
    }

    if (expect_code(galay_coro_join(&server, 2000), C_IOResultOk)) {
        record_posix_cleanup(close(client_fd));
        record_posix_cleanup(close(state.file_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 157;
    }

    int failed = 0;
    if (state.accept_result.code != C_IOResultOk ||
        state.accepted_peer.type != C_IPTypeIPV4 ||
        strcmp(state.accepted_peer.address, "127.0.0.1") != 0 ||
        state.accepted_peer.port == 0) {
        failed = 158;
    } else if (state.readv_result.code != C_IOResultOk ||
               state.readv_result.bytes != 7 ||
               memcmp(state.read_a, "abc", 3) != 0 ||
               memcmp(state.read_b, "1234", 4) != 0) {
        failed = 159;
    } else if (state.writev_result.code != C_IOResultOk ||
               state.writev_result.bytes != strlen("iov-reply")) {
        failed = 160;
    } else if (state.sendfile_result.code != C_IOResultOk ||
               state.sendfile_result.bytes != 9) {
        failed = 161;
    } else if (received != expected ||
               memcmp(peer_buffer, "iov-replyend-slice", expected) != 0) {
        failed = 162;
    } else if (state.close_result.code != C_IOResultOk) {
        failed = 163;
    }

    record_posix_cleanup(close(client_fd));
    record_posix_cleanup(close(state.file_fd));
    record_io_cleanup(galay_coro_destroy(&server));
    if (state.accepted.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    return failed;
}

static int run_accept_timeout(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    int client_fd = -1;
    AcceptTimeoutState state = {0};
    state.listener = &listener;
    atomic_init(&state.phase, 0);

    if (create_listener(&listener, &local) != 0) {
        return 201;
    }
    state.peer = local;

    galay_coro_task_t task = {0};
    if (expect_code(galay_coro_spawn(runtime, accept_timeout_entry, &state, 0, &task),
                    C_IOResultOk)) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 202;
    }
    if (expect_code(galay_coro_join(&task, 2000), C_IOResultOk)) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 203;
    }
    if (expect_code(state.result, C_IOResultTimeout)) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 205;
    }
    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 206;
    }

    galay_coro_task_t late_accept = {0};
    if (expect_code(galay_coro_spawn(runtime, accept_timeout_late_accept_entry, &state, 0, &late_accept),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&late_accept, 2000), C_IOResultOk)) {
        record_posix_cleanup(close(client_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 207;
    }
    if (expect_code(state.late_accept_result, C_IOResultOk)) {
        record_posix_cleanup(close(client_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 208;
    }
    if (state.result.bytes != 0 || state.accepted.socket != 0) {
        record_posix_cleanup(close(client_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 210;
    }

    record_posix_cleanup(close(client_fd));
    record_io_cleanup(galay_coro_destroy(&task));
    record_io_cleanup(galay_coro_destroy(&late_accept));
    if (state.accepted_late.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted_late));
    }
    if (state.late_client.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.late_client));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    return 0;
}

static int run_recv_timeout(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    int client_fd = -1;
    RecvTimeoutState state = {0};
    state.listener = &listener;
    if (checked_memset(state.buffer, 'x', sizeof(state.buffer)) != 0) {
        return 307;
    }
    atomic_init(&state.phase, 0);

    if (create_listener(&listener, &local) != 0) {
        return 301;
    }
    state.peer = local;

    galay_coro_task_t server = {0};
    if (expect_code(galay_coro_spawn(runtime, recv_timeout_server_entry, &state, 0, &server),
                    C_IOResultOk)) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 302;
    }
    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 304;
    }
    record_posix_cleanup(usleep(20000));
    if (write(client_fd, "late", 4) != 4) {
        record_posix_cleanup(close(client_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 305;
    }
    if (expect_code(galay_coro_join(&server, 2000), C_IOResultOk)) {
        record_posix_cleanup(close(client_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 306;
    }

    const int failed =
        state.accept_result.code != C_IOResultOk ||
        state.recv_result.code != C_IOResultTimeout ||
        state.recv_result.bytes != 0 ||
        memcmp(state.buffer, "xxxxxxxx", sizeof(state.buffer)) != 0 ||
        state.late_recv_result.code != C_IOResultOk ||
        state.late_recv_result.bytes != 4 ||
        memcmp(state.late_buffer, "late", 4) != 0 ||
        state.server_close_result.code != C_IOResultOk;
    if (failed) {
        record_diagnostic_write(fprintf(stderr,
                                        "recv timeout state: accept=%d recv=%d recv_bytes=%zu late_recv=%d late_bytes=%zu close=%d buffer=%.*s late=%.*s\n",
                                        (int)state.accept_result.code,
                                        (int)state.recv_result.code,
                                        state.recv_result.bytes,
                                        (int)state.late_recv_result.code,
                                        state.late_recv_result.bytes,
                                        (int)state.server_close_result.code,
                                        (int)sizeof(state.buffer),
                                        state.buffer,
                                        (int)sizeof(state.late_buffer),
                                        state.late_buffer));
    }

    record_posix_cleanup(close(client_fd));
    record_io_cleanup(galay_coro_destroy(&server));
    if (state.accepted.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    return failed ? 303 : 0;
}

static int run_close_while_waiting(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    int client_fd = -1;
    CloseWaitingState state = {0};
    state.listener = &listener;
    atomic_init(&state.phase, 0);

    if (create_listener(&listener, &local) != 0) {
        return 401;
    }

    galay_coro_task_t server = {0};
    galay_coro_task_t closer = {0};
    if (expect_code(galay_coro_spawn(runtime, close_waiting_server_entry, &state, 0, &server),
                    C_IOResultOk)) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 402;
    }
    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 403;
    }
    if (expect_code(galay_coro_spawn(runtime, close_waiting_closer_entry, &state, 0, &closer),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&closer, 2000), C_IOResultOk) ||
        expect_code(galay_coro_join(&server, 2000), C_IOResultOk)) {
        record_posix_cleanup(close(client_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 404;
    }

    const int failed =
        state.accept_result.code != C_IOResultOk ||
        state.close_result.code != C_IOResultOk ||
        state.recv_result.code != C_IOResultCancelled ||
        atomic_load(&state.phase) != 2;

    record_posix_cleanup(close(client_fd));
    record_io_cleanup(galay_coro_destroy(&server));
    record_io_cleanup(galay_coro_destroy(&closer));
    if (state.accepted.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    return failed ? 405 : 0;
}

static int run_immediate_accept(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    int client_fd = -1;
    ImmediateAcceptState state = {0};
    state.listener = &listener;

    if (create_listener(&listener, &local) != 0) {
        return 451;
    }
    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 452;
    }

    galay_coro_task_t task = {0};
    if (expect_code(galay_coro_spawn(runtime, immediate_accept_entry, &state, 0, &task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&task, 2000), C_IOResultOk)) {
        record_posix_cleanup(close(client_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 453;
    }

    int failed = 0;
    if (state.accept_result.code != C_IOResultOk) {
        failed = 454;
    } else if (state.accepted.socket == 0 || state.accept_result.ptr != &state.accepted) {
        failed = 455;
    }

    record_posix_cleanup(close(client_fd));
    record_io_cleanup(galay_coro_destroy(&task));
    if (state.accepted.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    return failed;
}

static int run_send_timeout_contract(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    int client_fd = -1;
    SendTimeoutState state = {0};
    state.listener = &listener;

    if (create_listener(&listener, &local) != 0) {
        return 471;
    }

    galay_coro_task_t server = {0};
    if (expect_code(galay_coro_spawn(runtime, send_timeout_server_entry, &state, 0, &server),
                    C_IOResultOk)) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 472;
    }
    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 473;
    }
    if (expect_code(galay_coro_join(&server, 3000), C_IOResultOk)) {
        record_posix_cleanup(close(client_fd));
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 474;
    }

    int saw_late_byte = 0;
    const int drain_errno = recv_until_quiet(client_fd, 'z', &saw_late_byte);

    int failed = 0;
    if (state.accept_result.code != C_IOResultOk) {
        failed = 475;
    } else if (state.fill_errno != 0) {
        record_diagnostic_write(fprintf(stderr,
                                        "send timeout fill failed errno=%d filled=%zu\n",
                                        state.fill_errno,
                                        state.filled_bytes));
        failed = 476;
    } else if (drain_errno != 0) {
        record_diagnostic_write(fprintf(stderr,
                                        "send timeout peer drain failed errno=%d\n",
                                        drain_errno));
        failed = 481;
    } else if (state.send_result.code == C_IOResultTimeout && state.close_result.code != C_IOResultOk) {
        record_diagnostic_write(fprintf(stderr,
                                        "send timeout close got code=%d sys_errno=%d\n",
                                        (int)state.close_result.code,
                                        state.close_result.sys_errno));
        failed = 478;
    } else if (state.send_result.code == C_IOResultOk &&
               (state.send_result.bytes == 0 || !saw_late_byte)) {
        record_diagnostic_write(fprintf(stderr,
                                        "send completed but did not report/observe the test byte: bytes=%zu saw=%d\n",
                                        state.send_result.bytes,
                                        saw_late_byte));
        failed = 480;
    } else if (state.send_result.code != C_IOResultTimeout &&
               state.send_result.code != C_IOResultOk) {
        record_diagnostic_write(fprintf(stderr,
                                        "send timeout arbitration got unexpected code=%d sys_errno=%d\n",
                                        (int)state.send_result.code,
                                        state.send_result.sys_errno));
        failed = 477;
    }

    record_posix_cleanup(close(client_fd));
    record_io_cleanup(galay_coro_destroy(&server));
    if (state.accepted.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    return failed;
}

static int run_zero_timeout_connect(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    ZeroTimeoutConnectState state = {0};
    state.listener = &listener;

    if (create_listener(&listener, &local) != 0) {
        return 501;
    }
    state.peer = local;

    galay_coro_task_t task = {0};
    if (expect_code(galay_coro_spawn(runtime, zero_timeout_connect_entry, &state, 0, &task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&task, 2000), C_IOResultOk)) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
        return 502;
    }

    int failed = 0;
    if (state.zero_connect_result.code != C_IOResultTimeout) {
        failed = 503;
    } else if (state.zero_accept_probe_result.code != C_IOResultTimeout) {
        failed = 504;
    } else if (state.accepted_from_zero.socket != 0) {
        failed = 505;
    } else if (state.connect_result.code != C_IOResultOk) {
        failed = 506;
    } else if (state.accept_result.code != C_IOResultOk) {
        failed = 507;
    } else if (state.close_client_result.code != C_IOResultOk) {
        failed = 508;
    }

    record_io_cleanup(galay_coro_destroy(&task));
    if (state.accepted_from_zero.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted_from_zero));
    }
    if (state.accepted_after_connect.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted_after_connect));
    }
    if (state.client.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.client));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    return failed;
}

static int run_connect_timeout_closes_socket(galay_kernel_runtime_t* runtime)
{
    ConnectTimeoutCloseState state = {0};
    state.peer.type = C_IPTypeIPV4;
    state.peer.port = 9;
    const unsigned pid = (unsigned)getpid();
    if (snprintf(state.peer.address,
                 sizeof(state.peer.address),
                 "169.254.%u.%u",
                 ((pid / 251U) % 254U) + 1U,
                 (pid % 254U) + 1U) <= 0) {
        return 524;
    }
    state.endpoint_after_timeout = C_TcpSocketSuccess;

    galay_coro_task_t task = {0};
    if (expect_code(galay_coro_spawn(runtime, connect_timeout_close_entry, &state, 0, &task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&task, 3000), C_IOResultOk)) {
        if (state.client.socket != 0) {
            record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.client));
        }
        return 521;
    }

    int failed = 0;
    if (state.connect_result.code != C_IOResultTimeout) {
        record_diagnostic_write(fprintf(stderr,
                                        "connect timeout close expected Timeout, got code=%d sys_errno=%d\n",
                                        (int)state.connect_result.code,
                                        state.connect_result.sys_errno));
        failed = 522;
    } else if (state.endpoint_after_timeout != C_TcpSocketIOFailed) {
        record_diagnostic_write(fprintf(stderr,
                                        "connect timeout close expected closed fd, local_endpoint code=%d\n",
                                        (int)state.endpoint_after_timeout));
        failed = 523;
    }

    record_io_cleanup(galay_coro_destroy(&task));
    if (state.client.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.client));
    }
    return failed;
}

static int run_multi_scheduler_misuse(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 2;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 701;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        record_runtime_cleanup(galay_kernel_runtime_destroy(&runtime));
        return 702;
    }

    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    MultiSchedulerMisuseState state = {0};
    state.listener = &listener;
    atomic_init(&state.phase, 0);

    int result = 0;
    if (create_listener(&listener, &local) != 0) {
        result = 703;
    } else {
        state.peer = local;
        galay_coro_task_t owner = {0};
        galay_coro_task_t misuse = {0};
        if (expect_code(galay_coro_spawn(&runtime, multi_scheduler_owner_entry, &state, 0, &owner),
                        C_IOResultOk) ||
            expect_code(galay_coro_spawn(&runtime, multi_scheduler_misuse_entry, &state, 0, &misuse),
                        C_IOResultOk) ||
            expect_code(galay_coro_join(&misuse, 2000), C_IOResultOk) ||
            expect_code(galay_coro_join(&owner, 2000), C_IOResultOk)) {
            result = 704;
        } else if (state.connect_result.code != C_IOResultOk ||
                   state.accept_result.code != C_IOResultOk ||
                   state.misuse_send_result.code != C_IOResultOk ||
                   state.owner_close_client_result.code != C_IOResultOk ||
                   state.owner_close_server_result.code != C_IOResultOk) {
            result = 705;
        }
        record_io_cleanup(galay_coro_destroy(&owner));
        record_io_cleanup(galay_coro_destroy(&misuse));
    }

    if (state.client.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.client));
    }
    if (state.accepted.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    record_runtime_cleanup(galay_kernel_runtime_stop(&runtime));
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 706;
    }
    return result;
}

static int run_owner_binding_failed_request_does_not_poison(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 2;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 801;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        record_runtime_cleanup(galay_kernel_runtime_destroy(&runtime));
        return 802;
    }

    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    OwnerBindingPollutionState state = {0};
    state.listener = &listener;

    int result = 0;
    if (create_listener(&listener, &local) != 0) {
        result = 803;
    } else {
        state.peer = local;
        galay_coro_task_t poison = {0};
        galay_coro_task_t reuse = {0};
        if (expect_code(galay_coro_spawn(&runtime, owner_binding_poison_entry, &state, 0, &poison),
                        C_IOResultOk) ||
            expect_code(galay_coro_join(&poison, 2000), C_IOResultOk)) {
            result = 804;
        } else if (state.poison_send_result.code == C_IOResultOk) {
            result = 805;
        } else if (expect_code(galay_coro_spawn(&runtime, owner_binding_reuse_entry, &state, 0, &reuse),
                               C_IOResultOk) ||
                   expect_code(galay_coro_join(&reuse, 2000), C_IOResultOk)) {
            result = 806;
        } else if (state.connect_result.code != C_IOResultOk ||
                   state.accept_result.code != C_IOResultOk ||
                   state.close_client_result.code != C_IOResultOk ||
                   state.close_server_result.code != C_IOResultOk) {
            record_diagnostic_write(fprintf(stderr,
                                            "owner binding reuse failed poison=%d connect=%d accept=%d close_client=%d close_server=%d\n",
                                            (int)state.poison_send_result.code,
                                            (int)state.connect_result.code,
                                            (int)state.accept_result.code,
                                            (int)state.close_client_result.code,
                                            (int)state.close_server_result.code));
            result = 807;
        }
        record_io_cleanup(galay_coro_destroy(&poison));
        record_io_cleanup(galay_coro_destroy(&reuse));
    }

    if (state.client.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.client));
    }
    if (state.accepted.socket != 0) {
        record_socket_cleanup(galay_kernel_tcp_socket_destroy(&state.accepted));
    }
    record_socket_cleanup(galay_kernel_tcp_socket_destroy(&listener));
    record_runtime_cleanup(galay_kernel_runtime_stop(&runtime));
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 808;
    }
    return result;
}

int main(void)
{
    galay_kernel_tcp_socket_t invalid_socket = {0};
    galay_kernel_tcp_socket_t out_socket = {0};
    C_Host host = {C_IPTypeIPV4, "127.0.0.1", 1};
    char buffer[8] = {0};

    if (expect_code(galay_kernel_tcp_socket_accept(0, &out_socket, NULL, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_accept(&invalid_socket, 0, NULL, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_connect(0, &host, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_connect(&invalid_socket, 0, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_recv(0, buffer, sizeof(buffer), 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_recv(&invalid_socket, 0, sizeof(buffer), 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_recv(&invalid_socket, buffer, 0, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_send(0, buffer, sizeof(buffer), 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_send(&invalid_socket, 0, sizeof(buffer), 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_send(&invalid_socket, buffer, 0, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_readv(0, (const struct iovec*)buffer, 1, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_readv(&invalid_socket, 0, 1, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_writev(0, (const struct iovec*)buffer, 1, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_writev(&invalid_socket, 0, 1, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_sendfile(0, -1, 0, 1, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_sendfile(&invalid_socket, -1, 0, 1, 0), C_IOResultInvalid) ||
        expect_code(galay_kernel_tcp_socket_close(0, 0), C_IOResultInvalid)) {
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 2;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        record_runtime_cleanup(galay_kernel_runtime_destroy(&runtime));
        return merge_cleanup_status(3, 5);
    }

    int result = run_echo(&runtime);
    result = merge_cleanup_status(result, 901);
    if (result == 0) {
        result = run_iov_sendfile(&runtime);
        result = merge_cleanup_status(result, 902);
    }
    if (result == 0) {
        result = run_accept_timeout(&runtime);
        result = merge_cleanup_status(result, 903);
    }
    if (result == 0) {
        result = run_recv_timeout(&runtime);
        result = merge_cleanup_status(result, 904);
    }
    if (result == 0) {
        result = run_close_while_waiting(&runtime);
        result = merge_cleanup_status(result, 905);
    }
    if (result == 0) {
        result = run_immediate_accept(&runtime);
        result = merge_cleanup_status(result, 906);
    }
    if (result == 0) {
        result = run_send_timeout_contract(&runtime);
        result = merge_cleanup_status(result, 907);
    }
    if (result == 0) {
        result = run_zero_timeout_connect(&runtime);
        result = merge_cleanup_status(result, 908);
    }
    if (result == 0) {
        result = run_connect_timeout_closes_socket(&runtime);
        result = merge_cleanup_status(result, 909);
    }
    record_runtime_cleanup(galay_kernel_runtime_stop(&runtime));
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 4;
    }
    result = merge_cleanup_status(result, 910);
    if (result == 0) {
        result = run_multi_scheduler_misuse();
        result = merge_cleanup_status(result, 911);
    }
    if (result == 0) {
        result = run_owner_binding_failed_request_does_not_poison();
        result = merge_cleanup_status(result, 912);
    }
    return result;
}
