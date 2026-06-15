#include <chrono>
#include <iostream>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include <sstream>

#define private public
#include "galay-http2/kernel/stream_mgr.h"
#undef private

using namespace galay::http2;

namespace {

class MockSslSocket {
public:
    MockSslSocket() = default;
};

} // namespace
#endif

int main() {
#ifndef GALAY_SSL_FEATURE_ENABLED
    std::cout << "T76-H2TlsActivePollInterval SKIP (SSL disabled)\n";
    return 0;
#else
    Http2ConnImpl<MockSslSocket> conn(MockSslSocket{});
    Http2StreamManagerImpl<MockSslSocket> manager(conn);

    const auto hot_wait = manager.sslIoOwnerPollInterval(true);
    const auto idle_wait = manager.sslIoOwnerPollInterval(false);

    if (!(hot_wait < idle_wait)) {
        std::cerr << "[T76] hot wait poll interval should be shorter than idle poll interval\n";
        return 1;
    }

    std::cout << "T76-H2TlsActivePollInterval PASS\n";
    return 0;
#endif
}
