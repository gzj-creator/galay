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

void checkMovedFrom(SslSocket& socket)
{
    require(!socket.isValid(), "moved-from socket is invalid");
    require(socket.handle() == GHandle::invalid(), "moved-from handle is invalid");
    require(socket.controller() == nullptr, "moved-from controller is empty");
    require(!socket.engine()->isValid(), "moved-from engine is empty");
    require(!socket.option().handleNonBlock(), "moved-from option reports an error");
    const auto bound = socket.bind(Host(IPType::IPV4, "127.0.0.1", 0));
    require(!bound && bound.error().code() == kClosed, "moved-from bind reports closed");
    const auto listening = socket.listen();
    require(!listening && listening.error().code() == kClosed, "moved-from listen reports closed");
    auto closed = socket.close();
    require(closed.m_controller == nullptr, "moved-from close has no controller");

    SslOperationDriver driver(&socket);
    char byte = 0;
    driver.startRecv(&byte, 1);
    require(!driver.takeRecvResult(), "moved-from recv reports an error");
    driver.startSend(&byte, 1);
    require(!driver.takeSendResult(), "moved-from send reports an error");
}

} // namespace

int main()
{
    SslContext context(SslMethod::TLS_Client);
    require(context.isValid(), "context creation");
    IOController* registration_owner = nullptr;
    SslSocket destination(&context, GHandle::invalid());
    int transferred_fd = -1;
    {
        SslSocket source(&context);
        require(source.isValid(), "socket creation");
        auto* const controller = source.controller();
        auto* const engine = source.engine()->native();
        transferred_fd = source.handle().fd;
        auto close_before_move = source.close();
#if defined(USE_EPOLL) || defined(USE_KQUEUE)
        controller->bindRegistrationOwnerSlot(&registration_owner);
#endif

        SslSocket moved(std::move(source));
        require(moved.controller() == controller, "move construction preserves controller address");
        require(moved.handle().fd == transferred_fd, "move construction transfers fd");
        require(moved.engine()->native() == engine, "move construction transfers TLS engine");
        checkMovedFrom(source);

        destination = std::move(moved);
        require(destination.controller() == controller, "move assignment preserves controller address");
        require(destination.handle().fd == transferred_fd, "move assignment transfers fd");
        require(destination.engine()->native() == engine, "move assignment transfers TLS engine");
        require(close_before_move.m_controller == destination.controller(),
                "pre-move awaitable still refers to the owned controller");
        checkMovedFrom(moved);

        SslSocket empty(std::move(source));
        checkMovedFrom(empty);
        moved = std::move(empty);
        checkMovedFrom(moved);
    }
    require(::fcntl(transferred_fd, F_GETFD) >= 0, "source destruction preserves transferred fd");
#if defined(USE_EPOLL) || defined(USE_KQUEUE)
    require(registration_owner == destination.controller(), "reactor registration survives source destruction");
#endif
    auto& self = destination;
    auto& assigned = (destination = std::move(self));
    require(&assigned == &destination && destination.isValid(), "self-move preserves socket");
    require(destination.option().handleNonBlock().has_value(), "moved socket remains usable");

    // SslSocket retains its explicit-close contract; no scheduler is needed here.
    require(::close(transferred_fd) == 0, "close transferred fd");
    destination.controller()->m_handle = GHandle::invalid();
    {
        SslSocket owner(std::move(destination));
        owner = std::move(destination);
        checkMovedFrom(owner);
    }
    require(registration_owner == nullptr, "replacing controller clears registration owner");
    std::puts("SSL socket move PASS");
    return 0;
}
