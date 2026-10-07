/**
 * @file t16_socket_move.cc
 * @brief SSL socket 移动时保持不可移动 controller 的地址和注册关系。
 */

#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

#ifdef GALAY_TEST_SSL_IMPORT
import galay.kernel;
import galay.ssl;
#else
#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#endif

using namespace galay::kernel;
using namespace galay::ssl;

static_assert(!std::is_move_constructible_v<IOController>);
static_assert(!std::is_move_assignable_v<IOController>);
static_assert(std::is_nothrow_move_constructible_v<SslSocket>);
static_assert(std::is_nothrow_move_assignable_v<SslSocket>);

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "SSL socket move: %s\n", message);
        std::exit(1);
    }
}

void check_moved_from(SslSocket& socket)
{
    require(!socket.is_valid(), "moved-from socket is invalid");
    require(socket.handle() == GHandle::invalid(), "moved-from handle is invalid");
    require(socket.controller() == nullptr, "moved-from controller is empty");
    require(!socket.engine()->is_valid(), "moved-from engine is empty");
    require(!socket.option().handle_non_block(), "moved-from option reports an error");
    const auto bound = socket.bind(Host(IPType::IPV4, "127.0.0.1", 0));
    require(!bound && bound.error().code() == kClosed, "moved-from bind reports closed");
    const auto listening = socket.listen();
    require(!listening && listening.error().code() == kClosed, "moved-from listen reports closed");
    auto closed = socket.close();
    require(closed.m_controller == nullptr, "moved-from close has no controller");

    SslOperationDriver driver(&socket);
    char byte = 0;
    driver.start_recv(&byte, 1);
    require(!driver.take_recv_result(), "moved-from recv reports an error");
    driver.start_send(&byte, 1);
    require(!driver.take_send_result(), "moved-from send reports an error");
}

} // namespace

int main()
{
    SslContext context(SslMethod::TLS_Client);
    require(context.is_valid(), "context creation");
    IOController* registration_owner = nullptr;
    SslSocket destination(&context, GHandle::invalid());
    int transferred_fd = -1;
    {
        SslSocket source(&context);
        require(source.is_valid(), "socket creation");
        auto* const controller = source.controller();
        auto* const engine = source.engine()->native();
        transferred_fd = source.handle().fd;
        auto close_before_move = source.close();
#if defined(USE_EPOLL) || defined(USE_KQUEUE)
        controller->bind_registration_owner_slot(&registration_owner);
#endif

        SslSocket moved(std::move(source));
        require(moved.controller() == controller, "move construction preserves controller address");
        require(moved.handle().fd == transferred_fd, "move construction transfers fd");
        require(moved.engine()->native() == engine, "move construction transfers TLS engine");
        check_moved_from(source);

        destination = std::move(moved);
        require(destination.controller() == controller, "move assignment preserves controller address");
        require(destination.handle().fd == transferred_fd, "move assignment transfers fd");
        require(destination.engine()->native() == engine, "move assignment transfers TLS engine");
        require(close_before_move.m_controller == destination.controller(),
                "pre-move awaitable still refers to the owned controller");
        check_moved_from(moved);

        SslSocket empty(std::move(source));
        check_moved_from(empty);
        moved = std::move(empty);
        check_moved_from(moved);
    }
    require(::fcntl(transferred_fd, F_GETFD) >= 0, "source destruction preserves transferred fd");
#if defined(USE_EPOLL) || defined(USE_KQUEUE)
    require(registration_owner == destination.controller(), "reactor registration survives source destruction");
#endif
    auto& self = destination;
    auto& assigned = (destination = std::move(self));
    require(&assigned == &destination && destination.is_valid(), "self-move preserves socket");
    require(destination.option().handle_non_block().has_value(), "moved socket remains usable");

    // SslSocket retains its explicit-close contract; no scheduler is needed here.
    require(::close(transferred_fd) == 0, "close transferred fd");
    destination.controller()->m_handle = GHandle::invalid();
    {
        SslSocket owner(std::move(destination));
        owner = std::move(destination);
        check_moved_from(owner);
    }
    require(registration_owner == nullptr, "replacing controller clears registration owner");
    std::puts("SSL socket move PASS");
    return 0;
}
