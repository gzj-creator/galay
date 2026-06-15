#include "galay-ssl/async/ssl_socket.h"
#include "galay-ssl/ssl/ssl_context.h"

#include <chrono>

using namespace galay::ssl;
using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

Task<void> instantiateRecvTimeoutSurface(SslSocket& socket)
{
    char buffer[16]{};
    auto result = co_await socket.recv(buffer, sizeof(buffer)).timeout(1ms);
    (void)result;
    co_return;
}

Task<void> instantiateSendTimeoutSurface(SslSocket& socket)
{
    constexpr char payload[] = "ping";
    auto result = co_await socket.send(payload, sizeof(payload) - 1).timeout(1ms);
    (void)result;
    co_return;
}

Task<void> instantiateHandshakeTimeoutSurface(SslSocket& socket)
{
    auto result = co_await socket.handshake().timeout(1ms);
    (void)result;
    co_return;
}

} // namespace

int main()
{
    SslContext ctx(SslMethod::TLS_Client);
    SslSocket socket(&ctx);
    (void)instantiateRecvTimeoutSurface(socket);
    (void)instantiateSendTimeoutSurface(socket);
    (void)instantiateHandshakeTimeoutSurface(socket);
    return 0;
}
