#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct ConnectState {
    atomic_int done;
    atomic_int code;
} ConnectState;

static void on_connect(C_TcpSocketResultCode code, void* ctx)
{
    ConnectState* state = (ConnectState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

static int expect_status(C_TcpSocketResultCode actual, C_TcpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int make_loopback_listener(C_Host* endpoint)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }

    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &addr_len) != 0) {
        close(fd);
        return -1;
    }

    endpoint->type = C_IPTypeIPV4;
    strncpy(endpoint->address, "127.0.0.1", sizeof(endpoint->address));
    endpoint->address[sizeof(endpoint->address) - 1] = '\0';
    endpoint->port = ntohs(addr.sin_port);
    return fd;
}

static int wait_for_connect(ConnectState* state)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(&state->done)) {
            return atomic_load(&state->code) == (int)C_TcpSocketSuccess ? 0 : 1;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static int accept_posix_client(int server_fd)
{
    struct pollfd pfd;
    pfd.fd = server_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 2000) <= 0 || (pfd.revents & POLLIN) == 0) {
        return -1;
    }
    return accept(server_fd, 0, 0);
}

static int test_connect_callback(galay_kernel_runtime_t* runtime)
{
    C_Host server = {0};
    int server_fd = make_loopback_listener(&server);
    int accepted_fd = -1;
    if (server_fd < 0) {
        return 30;
    }

    galay_kernel_tcp_socket_t client = {0};
    if (expect_status(galay_kernel_tcp_socket_create(&client, C_IPTypeIPV4), C_TcpSocketSuccess)) {
        close(server_fd);
        return 31;
    }

    ConnectState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_TcpSocketIOFailed);
    if (expect_status(galay_kernel_tcp_socket_connect(runtime, &client, &server, on_connect, &state), C_TcpSocketSuccess)) {
        (void)galay_kernel_tcp_socket_destroy(&client);
        close(server_fd);
        return 32;
    }
    accepted_fd = accept_posix_client(server_fd);
    if (accepted_fd < 0) {
        (void)galay_kernel_tcp_socket_destroy(&client);
        close(server_fd);
        return 33;
    }
    if (wait_for_connect(&state)) {
        close(accepted_fd);
        (void)galay_kernel_tcp_socket_destroy(&client);
        close(server_fd);
        return 34;
    }

    close(accepted_fd);
    (void)galay_kernel_tcp_socket_destroy(&client);
    close(server_fd);
    return 0;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t tcp = {0};
    C_Host invalid_host = {C_IPTypeIPV4, "not-an-ip", 0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host local = {0};

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (expect_status(galay_kernel_tcp_socket_create(&tcp, C_IPTypeIPV4), C_TcpSocketSuccess)) {
        return 2;
    }
    if (tcp.socket == 0) {
        return 3;
    }
    if (expect_status(galay_kernel_tcp_socket_create(&tcp, (C_IPType)99), C_TcpSocketParameterInvalid)) {
        return 4;
    }
    if (expect_status(galay_kernel_tcp_socket_create(0, C_IPTypeIPV4), C_TcpSocketParameterInvalid)) {
        return 5;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return 6;
    }
    if (expect_status(galay_kernel_tcp_socket_connect(0, &tcp, &bind_host, on_connect, 0), C_TcpSocketParameterInvalid)) {
        return 7;
    }
    if (expect_status(galay_kernel_tcp_socket_connect(&runtime, 0, &bind_host, on_connect, 0), C_TcpSocketParameterInvalid)) {
        return 8;
    }
    if (expect_status(galay_kernel_tcp_socket_connect(&runtime, &tcp, 0, on_connect, 0), C_TcpSocketParameterInvalid)) {
        return 9;
    }
    if (expect_status(galay_kernel_tcp_socket_connect(&runtime, &tcp, &bind_host, 0, 0), C_TcpSocketParameterInvalid)) {
        return 10;
    }
    if (expect_status(galay_kernel_tcp_socket_connect(&runtime, &tcp, &invalid_host, on_connect, 0), C_TcpSocketParameterInvalid)) {
        return 11;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(0, &bind_host), C_TcpSocketParameterInvalid)) {
        return 12;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(&tcp, 0), C_TcpSocketParameterInvalid)) {
        return 13;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(&tcp, &invalid_host), C_TcpSocketParameterInvalid)) {
        return 14;
    }
    if (expect_status(galay_kernel_tcp_socket_listen(0, 16), C_TcpSocketParameterInvalid)) {
        return 15;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(&tcp, &bind_host), C_TcpSocketSuccess)) {
        return 16;
    }
    if (expect_status(galay_kernel_tcp_socket_listen(&tcp, 16), C_TcpSocketSuccess)) {
        return 17;
    }
    if (expect_status(galay_kernel_tcp_socket_local_endpoint(0, &local), C_TcpSocketParameterInvalid)) {
        return 18;
    }
    if (expect_status(galay_kernel_tcp_socket_local_endpoint(&tcp, 0), C_TcpSocketParameterInvalid)) {
        return 19;
    }
    if (expect_status(galay_kernel_tcp_socket_local_endpoint(&tcp, &local), C_TcpSocketSuccess)) {
        return 20;
    }
    if (local.type != C_IPTypeIPV4 || local.address[0] == '\0' || local.port == 0) {
        return 21;
    }
    if (expect_status(galay_kernel_tcp_socket_destroy(&tcp), C_TcpSocketSuccess)) {
        return 22;
    }
    int connect_result = test_connect_callback(&runtime);
    if (connect_result != 0) {
        return connect_result;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess) {
        return 23;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
        return 24;
    }

    return 0;
}
