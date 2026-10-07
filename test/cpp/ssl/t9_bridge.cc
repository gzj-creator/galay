#include <galay/cpp/galay-ssl/async/awaitable.h>
#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <expected>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace galay::ssl;
using namespace galay::kernel;

using SurfaceResult = std::expected<size_t, SslError>;

struct SurfaceMachine {
    using result_type = SurfaceResult;

    SslMachineAction<result_type> advance()
    {
        return SslMachineAction<result_type>::complete(result_type{0});
    }

    void on_handshake(std::expected<void, SslError>) {}
    void on_recv(std::expected<Bytes, SslError>) {}
    void on_send(std::expected<size_t, SslError>) {}
    void on_shutdown(std::expected<void, SslError>) {}
};

static int run_child(IOErrorCode code)
{
    SslSocket socket(nullptr, IPType::IPV4);
    SslStateMachineAwaitable<SurfaceMachine> awaitable(socket.controller(), &socket, SurfaceMachine{});
    static_cast<SequenceAwaitableBase&>(awaitable).m_error = IOError(code, 0);

    auto result = awaitable.await_resume();
    if (result.has_value()) {
        return 250;
    }
    return static_cast<int>(result.error().code()) + 1;
}

static bool expect_child_exit(IOErrorCode input, SslErrorCode expected)
{
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::cerr << "fork failed\n";
        return false;
    }
    if (pid == 0) {
        ::_exit(run_child(input));
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        std::cerr << "waitpid failed\n";
        return false;
    }
    if (!WIFEXITED(status)) {
        std::cerr << "child terminated abnormally\n";
        return false;
    }

    const int expected_exit = static_cast<int>(expected) + 1;
    const int actual_exit = WEXITSTATUS(status);
    if (actual_exit != expected_exit) {
        std::cerr << "unexpected exit code: " << actual_exit
                  << ", expected: " << expected_exit << "\n";
        return false;
    }
    return true;
}

int main()
{
    if (!expect_child_exit(kNotReady, SslErrorCode::kUnknown)) {
        return 1;
    }
    if (!expect_child_exit(kTimeout, SslErrorCode::kTimeout)) {
        return 1;
    }
    return 0;
}
