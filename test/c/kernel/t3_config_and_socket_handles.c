#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct ConnectState {
    galay_kernel_tcp_socket_t* client;
    C_Host server;
    C_IOResult result;
} ConnectState;

static int expect_socket_status(C_TcpSocketResultCode actual, C_TcpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_io_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int close_fd(int fd)
{
    return fd < 0 || close(fd) == 0 ? 0 : 1;
}

static int sleep_briefly(void)
{
    const struct timespec pause = {0, 1000000};
    return nanosleep(&pause, NULL) == 0 ? 0 : 1;
}

static int make_loopback_listener(C_Host* endpoint)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        int ignored = close_fd(fd);
        return -1 - ignored;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        int ignored = close_fd(fd);
        return -1 - ignored;
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        int ignored = close_fd(fd);
        return -1 - ignored;
    }
    if (listen(fd, 16) != 0) {
        int ignored = close_fd(fd);
        return -1 - ignored;
    }

    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &addr_len) != 0) {
        int ignored = close_fd(fd);
        return -1 - ignored;
    }

    endpoint->type = C_IPTypeIPV4;
    if (strncpy(endpoint->address, "127.0.0.1", sizeof(endpoint->address)) !=
        endpoint->address) {
        int ignored = close_fd(fd);
        return -1 - ignored;
    }
    endpoint->address[sizeof(endpoint->address) - 1] = '\0';
    endpoint->port = ntohs(addr.sin_port);
    return fd;
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
    return accept(server_fd, NULL, NULL);
}

static void connect_entry(void* arg)
{
    ConnectState* state = (ConnectState*)arg;
    state->result = galay_kernel_tcp_socket_connect(state->client, &state->server, 1000);
}

static int cleanup_connect(galay_coro_task_t* task,
                           galay_kernel_tcp_socket_t* client,
                           int accepted_fd,
                           int server_fd,
                           int exit_code)
{
    if (task->task != NULL) {
        C_IOResult joined = galay_coro_join(task, 0);
        if ((joined.code == C_IOResultOk || joined.code == C_IOResultCancelled) &&
            galay_coro_destroy(task).code != C_IOResultOk &&
            exit_code == 0) {
            exit_code = 40;
        }
    }
    if (accepted_fd >= 0 && close_fd(accepted_fd) != 0 && exit_code == 0) {
        exit_code = 41;
    }
    if (client->socket != NULL &&
        expect_socket_status(galay_kernel_tcp_socket_destroy(client), C_TcpSocketSuccess) &&
        exit_code == 0) {
        exit_code = 42;
    }
    if (server_fd >= 0 && close_fd(server_fd) != 0 && exit_code == 0) {
        exit_code = 43;
    }
    return exit_code;
}

static int test_connect_direct(galay_kernel_runtime_t* runtime)
{
    C_Host server = {0};
    int server_fd = make_loopback_listener(&server);
    int accepted_fd = -1;
    if (server_fd < 0) {
        return 30;
    }

    galay_kernel_tcp_socket_t client = {0};
    if (expect_socket_status(galay_kernel_tcp_socket_create(&client, C_IPTypeIPV4),
                             C_TcpSocketSuccess)) {
        return cleanup_connect(&(galay_coro_task_t){0}, &client, accepted_fd, server_fd, 31);
    }

    ConnectState state;
    state.client = &client;
    state.server = server;
    state.result = (C_IOResult){C_IOResultInvalid, 0, 0, 0, NULL};
    galay_coro_task_t task = {0};
    if (expect_io_code(galay_coro_spawn(runtime, connect_entry, &state, NULL, &task),
                       C_IOResultOk)) {
        return cleanup_connect(&task, &client, accepted_fd, server_fd, 32);
    }

    accepted_fd = accept_posix_client(server_fd);
    if (accepted_fd < 0) {
        return cleanup_connect(&task, &client, accepted_fd, server_fd, 33);
    }
    if (expect_io_code(galay_coro_join(&task, 2000), C_IOResultOk)) {
        return cleanup_connect(&task, &client, accepted_fd, server_fd, 34);
    }
    if (expect_io_code(state.result, C_IOResultOk)) {
        return cleanup_connect(&task, &client, accepted_fd, server_fd, 35);
    }
    if (expect_io_code(galay_coro_destroy(&task), C_IOResultOk)) {
        task.task = NULL;
        return cleanup_connect(&task, &client, accepted_fd, server_fd, 36);
    }
    task.task = NULL;

    return cleanup_connect(&task, &client, accepted_fd, server_fd, 0);
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
    if (expect_socket_status(galay_kernel_tcp_socket_create(&tcp, C_IPTypeIPV4),
                             C_TcpSocketSuccess)) {
        return 2;
    }
    if (tcp.socket == NULL) {
        return 3;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_create(&tcp, (C_IPType)99),
                             C_TcpSocketParameterInvalid)) {
        return 4;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_create(NULL, C_IPTypeIPV4),
                             C_TcpSocketParameterInvalid)) {
        return 5;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return 6;
    }
    if (expect_io_code(galay_kernel_tcp_socket_connect(NULL, &bind_host, 0),
                       C_IOResultInvalid)) {
        return 7;
    }
    if (expect_io_code(galay_kernel_tcp_socket_connect(&tcp, NULL, 0),
                       C_IOResultInvalid)) {
        return 8;
    }
    if (expect_io_code(galay_kernel_tcp_socket_connect(&tcp, &invalid_host, 0),
                       C_IOResultInvalid)) {
        return 9;
    }
    if (expect_io_code(galay_kernel_tcp_socket_connect(&tcp, &bind_host, 0),
                       C_IOResultInvalid)) {
        return 10;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_bind(NULL, &bind_host),
                             C_TcpSocketParameterInvalid)) {
        return 12;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_bind(&tcp, NULL),
                             C_TcpSocketParameterInvalid)) {
        return 13;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_bind(&tcp, &invalid_host),
                             C_TcpSocketParameterInvalid)) {
        return 14;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_listen(NULL, 16),
                             C_TcpSocketParameterInvalid)) {
        return 15;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_bind(&tcp, &bind_host),
                             C_TcpSocketSuccess)) {
        return 16;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_listen(&tcp, 16),
                             C_TcpSocketSuccess)) {
        return 17;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_local_endpoint(NULL, &local),
                             C_TcpSocketParameterInvalid)) {
        return 18;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_local_endpoint(&tcp, NULL),
                             C_TcpSocketParameterInvalid)) {
        return 19;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_local_endpoint(&tcp, &local),
                             C_TcpSocketSuccess)) {
        return 20;
    }
    if (local.type != C_IPTypeIPV4 || local.address[0] == '\0' || local.port == 0) {
        return 21;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_destroy(&tcp), C_TcpSocketSuccess)) {
        return 22;
    }
    tcp.socket = NULL;

    int connect_result = test_connect_direct(&runtime);
    if (connect_result != 0) {
        return connect_result;
    }
    if (sleep_briefly() != 0) {
        return 25;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess) {
        return 23;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
        return 24;
    }

    return 0;
}
